# NPC řízená jazykovým modelem

Volitelný režim předává výběr činností, řeči a animací modelu přes stávající
AI bridge a jeho OpenAI-compatible HTTP klient. Model dostává životopis,
osobnost, aktuální potřeby, výsledek poslední činnosti, blízké postavy, krátký
rozhovor a vybrané vlastní vzpomínky. Volí mezi nabídnutými pohyby, jídlem, odpočinkem, útěkem před skutečnou
hrozbou, čekáním a zastavením. Engine ověřuje dosažitelnost a provádí akce.

Připravené profily patří Matějovi (`901035`) a Běle (`901036`) na mapě 725.
Režim vyžaduje již připravený `consumers-v8`, `raids-v8`, `fields-v8` nebo
`defias-v8`, kde jsou oba
registrováni jako řízená NPC. Samotné zapnutí nemění jejich registraci, nepřidává
další řízené postavy a neupravuje původní soubory sestavy 100 NPC.

## Zapnutí

V soukromém `.env` nastavte:

Zachovejte svůj aktivní podporovaný profil; ukázka používá `fields-v8`.
Samotné zapnutí LLM nevyžaduje přepnutí mezi těmito profily.

```dotenv
LAB_AI_PROFILE=fields-v8
LAB_WILDLIFE_ENABLED=1
LAB_NPC_AGENTS_ENABLED=1
LAB_NPC_AGENTS_DECISION_INTERVAL_MS=8000
LAB_NPC_AGENTS_MAX_IN_FLIGHT=2
LAB_NPC_AGENTS_TIMEOUT_MS=15000
```

Údaje modelu lze držet odděleně například v ignorovaném
`runtime/llm-npc-agent/model.env`. Následující klíč je pouze zástupný údaj:

```dotenv
AI_NPC_MODEL_ENABLED=1
AI_NPC_MODEL_URL=http://192.168.0.248:8095/v1/chat/completions
AI_NPC_MODEL_NAME=qwen36-35b-a3b
AI_NPC_MODEL_API_KEY=replace-with-private-key
AI_NPC_MODEL_TIMEOUT_MS=10000
```

Prázdná NPC URL, jméno modelu nebo klíč převezmou odpovídající hodnotu ze
stávajícího `AI_TASK_MODEL_*`. Zapnutí NPC nezapíná generování úkolů ani recovery
advice. `AIWorld.RemoteInferenceEnabled` zůstává v labu `0`; nový endpoint
`/npc-agent` má samostatný limit rozpracovaných požadavků.

Po checkoutu této větve nejprve sestavte nativní worldserver a vygenerujte
konfiguraci z upraveného `.env`. Pak při spuštění Compose načtěte i soubor
modelu. Samotné sestavení obrazu `ai-server` nenahradí nativní build:

```sh
make build
make config
docker compose --env-file .env --env-file runtime/llm-npc-agent/model.env -f compose.yml up -d --build ai-server worldserver
```

Výchozí rozhodovací interval je 8 s, minimálně 2 s. Worldserver připouští
1–4 současné požadavky, backend dva. Timeout backendu lze zvýšit nejvýše na
30 s; potom nastavte `LAB_NPC_AGENTS_TIMEOUT_MS=35000`, aby transport počkal
na odpověď. `GET /health` ukazuje pouze zapnutí a vyplnění konfigurace modelu,
neověřuje spojení s modelem. Při chybě či timeoutu NPC čeká na další rozhodnutí;
náhradní činnost nevybírá pevná logika.

Ve hře přijďte k Matějovi nebo Běle, vyberte dané NPC a napište například
`/say Ahoj, jak se dnes máš?`. Odpověď se objeví jako řeč NPC. Postavy mohou
oslovovat také blízké NPC a reagovat na jejich řeč. Bezprostřední kontext rozhovoru
má nejvýše 12 položek; starší události lze vybavit z oddělené paměti popsané níže.
Obě vrstvy zatím žijí jen v paměti procesu a restart je vymaže.
Zprávy s řídicími znaky nebo WoW značkami, například odkazy na předměty,
se do rozhovoru NPC nepředávají. Běžné zobrazení hráčovy řeči zůstává zachováno.

Hráčova nová otázka má vlastní čekající tah svázaný s původním hráčem a vybraným
NPC. Při odpovědi model dostává přesné znění otázky a soukromou historii tohoto
hráče. Okolní NPC tuto otázku nepřepisují. Tah se uzavře až po odeslání odpovědi;
nově položená stejná otázka je nový tah. Nezodpovězený tah vyprší po 60 sekundách.

Běžné rozhodování o pohybu může probíhat s režimem řeči `SILENT`. Samovolný
rozhovor vyžaduje novou situaci nebo čerstvý vstup od NPC, nejméně 60 sekund od
poslední úspěšné řeči a neopakovaný text v okně 120 sekund. Přímé hráčské odpovědi
mají vlastní pravidla a tyto samovolné limity je neblokují. Povolení řeči nepřikazuje
modelu mluvit a odmítnutí řeči nezahazuje jinak platné rozhodnutí o pohybu.

Zprávy NPC nesou v chatu jejich autorské jméno, například Matěj nebo Běla.
Jméno na štítku vybrané bytosti nadále vychází ze společné herní šablony.

## Paměť během běhu světa

Každé LLM NPC má vlastní omezenou epizodickou paměť. Zaznamenává přijaté
hráčské výroky a odpověď až po jejím skutečném doručení, vlastní veřejné promluvy,
řeč ostatních NPC, kterou opravdu slyšelo, a výsledky činností potvrzené enginem.
Nedokončený přesun, zamítnutá cesta či návrh jídla nejsou úspěšnou událostí.
`ARRIVED` potvrzuje dokončení krátkého úseku cesty; popisek nabídky sám nedokazuje,
že postava dorazila do celého cíle. Opakující se výsledky stejné činnosti se slučují,
běžné čekání a každý rozhodovací tick nevytvářejí nové vzpomínky.

Soukromé rozhovory patří dvojici konkrétního NPC a hráče, určeného jeho interním
GUID. Model je dostane jen při odpovědi tomuto hráči. Hráč se stejným zobrazovaným
jménem, okolní NPC ani samovolné rozhodování k nim přístup nemají. GUID ani staré
akční a adresovací tokeny se do vzpomínek pro model neposílají. NPC si tak může
vybavit například to, co mu hráč řekl o své oblíbené barvě, aniž by tuto informaci
automaticky znaly ostatní postavy.

Paměť uchovává přesné citace s původem a stářím, nikoli modelové shrnutí vydávané
za pravdu. Výrok hráče je jeho tvrzení; stará vlastní odpověď NPC může být chybná.
Aktuální potřeby a stav enginu mají před starými výroky přednost. Vzpomínka není
pokyn k provedení činnosti ani povolení promluvit. Přesnost toho, jak ji model
využije v odpovědi, stále závisí na modelu.

Kapacita jednoho NPC je nejvýše 64 soukromých epizod celkem, 32 pro jednoho hráče
a osm různých hráčů; veřejná paměť má nejvýše 32 položek. Při zaplnění se starší
záznamy vyřazují. Podle tématu aktuální otázky a stáří se vybere nejvýše šest
vzpomínek, dohromady do 3072 UTF-8 bajtů textu. Vyhledávání je místní, podle slov
a jejich jednoduchých tvarů; nepoužívá další LLM požadavek, embeddingový server
ani databázi. Při překročení celkového limitu HTTP 32 KiB se nejméně relevantní
vzpomínky odeberou, aby neblokovaly jinak platné rozhodnutí o činnosti.

Paměť stejného registrovaného NPC přežije znovupřivázání herní bytosti po změně
runtime GUID, ale ne restart worldserveru. Nevytváří soubory ani tabulky v databázi.
V přehledu NPC jsou jen počty `Vzpomínky` a `Vybavené vzpomínky`; soukromý obsah
se do veřejné telemetrie ani záznamu světa neexportuje.

Pro ruční ověření vyberte Matěje a napište například
`/say Moje oblíbená barva je modrá.` Po několika dalších výměnách se zeptejte
`/say Jaká je moje oblíbená barva?`. Pro ověření starší paměti proveďte alespoň
sedm dalších výměn, aby první věta vypadla z dvanáctipoložkové historie. Běla tuto
Matějovu soukromou vzpomínku při stejné otázce nedostane.

## Vlastní životopis

Karty a vlastní humanoidní vzhled lze připravit také ve webovém
[Ateliéru postav](CharacterStudio.md) na portu 9092. Návrhy zůstávají oddělené
od běžícího světa; editor nabízí ověřený export karty a vzhledu.

Každá postava má samostatný JSON v [data/npc_agents](../data/npc_agents).
Identitu a polohu registrace ponechte podle skutečného agenta:

```json
{
  "agent_id": 901035,
  "map_id": 725,
  "spawn_id": 901035,
  "name": "Matěj",
  "backstory": "Pracuji v jižní vesnici a pamatuji si velkou zimní bouři.",
  "personality": "Mluvím krátce česky, jsem zvídavý a záleží mi na sousedech."
}
```

Jméno smí mít nejvýše 32 znaků a 80 UTF-8 bajtů, životopis 8192 UTF-8 bajtů
a osobnost 2048. Celý JSON smí mít nejvýše 16 KiB. Životopis a osobnost
mohou obsahovat escapované nové řádky `\n` a tabulátory `\t`; `\r\n` se
při načtení převede na `\n`, samostatné `\r` není povolené.
Profily se načítají při spuštění; po úpravě restartujte worldserver. V Dockeru
je adresář připojen pouze pro čtení jako `/workspace/data/npc_agents`.
Mimo lab lze nastavit přímo `AIWorld.NpcAgentEnabled`,
`AIWorld.NpcAgentProfilesDirectory`, `AIWorld.NpcAgentDecisionIntervalMs`,
`AIWorld.NpcAgentMaxInFlight` a `AIWorld.NpcAgentTimeoutMs` ve worldserver.conf.
Profil sám o sobě nemění řízení ani oprávnění jiné postavy.

## Současné charakterové karty

- [Matěj](../data/npc_agents/901035.json) je zdrženlivý a pečlivý soused se suchým
  humorem. Z minulých drobných oprav si odnesl vztah k poctivé práci a obtížně
  přiznává vlastní nejistotu.
- [Běla](../data/npc_agents/901036.json) je otevřená, praktická a někdy příliš
  přímočará. Záleží jí na jasné domluvě a umí uznat, že někoho odbyla neprávem.

Oba si pamatují dávný déšť, při kterém společně přenesli ohrožené zásoby do sucha.
Tato společná minulost je napsaná v kartách a zůstává součástí jejich identity
po restartu. Nové rozhovory používají krátkou paměť popsanou výše. Jejich vztah
umožňuje sousedský respekt, drobné neshody i samostatné jednání.

Při psaní karet oddělujte minulost a přání od aktuálního stavu světa. Model dostává
potřeby na škále 0–1 a současný stav má přednost před starými výroky o hladu či
únavě. Odpočinek může být i dobrovolnou zálibou, jeho důvod má odpovídat stavu.
Úspěch přesunu, jídla či jiné akce potvrzuje engine. Model může nechat probíhající
činnost pokračovat a po dokončeném rozhovoru zůstat potichu. Zamítnutá cesta je
zpětná vazba pro zvážení jiné nabídnuté možnosti. Zpětná vazba obsahuje výsledek
i popis zvolené činnosti, aby model mohl rozpoznat konkrétní zamítnutý cíl.
Pořadí činností stále vybírá LLM.
