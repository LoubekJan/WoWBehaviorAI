# NPC řízená jazykovým modelem

## Prázdný svět pro finální tvorbu mapy

Přechod na profil **`authoring-empty`** ponechává realm 2, vlastní mapu
725 / oblast 4988, účty a hráčské postavy, ale jeho cílovým stavem je
**0 mapových NPC a 0 LLM controllerů**. Dosavadní karty, vzhledy, předpisy
šablon a uložené charakterové návrhy se při přechodu odstraňují až po
chráněné záloze. Samotná přítomnost tohoto návodu není potvrzením dokončené
čistky; tu dokládá provozní receipt a prázdné serverové katalogy.

Ateliér v `authoring-empty` dál umožňuje vytvořit a zálohovat nový návrh,
ale neposkytuje žádný ověřený cíl pro runtime export. Životopis ani ručně
zadané číslo NPC neobnoví jeho spawn nebo registraci. Před novým osídlením
se musí navrhnout a schválit populace finální mapy. Nepřepínejte zpět na
`full-v8` jen kvůli odemknutí exportu: tím by se vracela dřívější sestava.

Následující oddíly popisují zachované implementované schopnosti a historicky
ověřený controller. Tři postavy z revize `664d84687d7a` jsou **předchozí
testovací populace**, nikoli aktivní postavy prázdné mapy.

## Implementované možnosti

Volitelný režim předává výběr činností, řeči a animací modelu přes stávající
AI bridge a jeho OpenAI-compatible HTTP klient. Model dostává životopis,
osobnost, aktuální potřeby, výsledek poslední činnosti, blízké postavy, krátký
rozhovor a vybrané vlastní vzpomínky. Volí mezi nabídnutými pohyby, jídlem, odpočinkem, útěkem před skutečnou
hrozbou, čekáním a zastavením. Engine ověřuje dosažitelnost a provádí akce.

Dosavadní implementace vyžaduje již připravený `consumers-v8`, `raids-v8`, `fields-v8`,
`defias-v8` nebo `full-v8` a konkrétní NPC registrované v daném profilu jako
řízený účastník. Vlastní herní šablony vyžadují `full-v8`. Samotné zapnutí
LLM nemění registraci, nepřidává
další řízené postavy a neupravuje původní soubory sestavy 100 NPC.

Původní `full-v8` registruje všech 100 testovacích NPC včetně `901014`. Každému lze
samostatně přiřadit vlastní kartu; samotná registrace žádný životopis ani
modelové požadavky nevytvoří. Současný katalog přijímá nejvýše 16 platných
instalovaných LLM karet; ostatní NPC používají běžné role AIWorld.

## Zapnutí v osídleném světě

Tato konfigurační ukázka patří dřívějšímu `full-v8`, nikoli prázdnému
režimu finální tvorby mapy. V `authoring-empty` ponechte LLM controller vypnutý.
Pro budoucí populaci bude nejprve nutná její vlastní ověřená registrace.

V soukromém kořenovém `.env` jsou pro původní podporované profily následující
hodnoty. Vlastní šablony jsou zatím vázané na `full-v8`. Pro ostatní
podporované profily zachovejte jejich ověřenou sestavu a dostupné cíle.

```dotenv
LAB_AI_PROFILE=full-v8
LAB_WILDLIFE_ENABLED=1
LAB_NPC_AGENTS_ENABLED=1
LAB_NPC_AGENTS_DECISION_INTERVAL_MS=8000
LAB_NPC_AGENTS_MAX_IN_FLIGHT=2
LAB_NPC_AGENTS_TIMEOUT_MS=15000
```

Do stejného soukromého `.env` patří také nastavení modelu. Běžná CI/CD
pipeline načítá právě tento soubor; druhý ručně přidaný `--env-file` při
příštím automatickém nasazení nepřevezme. Následující klíč je pouze
zástupný údaj:

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

Použijte řízené nasazení větve `AI-World-lab`, popsané v
[README_DEV.md](../README_DEV.md), které sestaví nativní worldserver,
vygeneruje konfiguraci a obnoví služby. Samotný build obrazu `ai-server`
nenahradí nativní build. Ruční překlad do provozního build volume vyžaduje
zastavení worldserveru i authserveru, které z něj používají binární soubory;
běžný postup zajišťuje nasazovací skript.

Pokud je potřebná verze kódu již sestavená a měníte pouze soubory postavy,
použijte offline import uvedený v [Ateliéru postav](CharacterStudio.md#nasazení-balíčku).
Pouhý restart nevytvoří novou databázovou šablonu.

Výchozí rozhodovací interval je 8 s, minimálně 2 s. Worldserver připouští
1–4 současné požadavky, backend dva. Timeout backendu lze zvýšit nejvýše na
30 s; potom nastavte `LAB_NPC_AGENTS_TIMEOUT_MS=35000`, aby transport počkal
na odpověď. `GET /health` ukazuje pouze zapnutí a vyplnění konfigurace modelu,
neověřuje spojení s modelem. Při chybě či timeoutu NPC čeká na další rozhodnutí;
náhradní činnost nevybírá pevná logika.

Ve hře přijďte k libovolnému nainstalovanému LLM NPC, vyberte jej a napište například
`/say Ahoj, jak se dnes máš?`. Odpověď se objeví jako řeč NPC. Postavy mohou
oslovovat také blízká LLM NPC a reagovat na jejich řeč. Účastníci musejí být
živí, viditelní, na stejné mapě a v dosahu 25 yardů s přímou viditelností.
Bezprostřední kontext rozhovoru
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

Zprávy NPC nesou v chatu jejich autorské jméno. V `full-v8` úplný balíček
vytváří také vlastní nativní šablonu, ze které klient dostává stejné jméno
nad postavou a podtitul z autorského povolání. Starší samostatný export
karty bez šablony nadále může měnit pouze jméno používané LLM/chatem.
Po nasazení změny jména se znovu přihlaste do hry; server odvozuje verzi
klientské cache z nainstalovaných jmen a podtitulů.

## Co model skutečně ovládá

Model vybírá jednu z právě nabídnutých činností `WAIT`, `MOVE_TO`, `EAT`,
`REST`, `FLEE` nebo `STOP`, případně řeč a kosmetické gesto. Pohybové cíle
a adresáty vytváří server a model vrací jejich dočasné tokeny. Nemůže zadat
libovolnou souřadnici, vytvořit předmět, změnit potřebu přímým zápisem ani
si přidělit chybějící herní schopnost. Dostupná gesta zahrnují mávnutí,
mluvení, přikývnutí, smích, ukázání, jásot a nesouhlas; `NONE` je bez gesta.
Povolená akce stále prochází kontrolami vlastnictví, stavu a dosažitelnosti.

Po dokončeném jídle či odpočinku potřebu mění engine. `WAIT` může ponechat
vlastní rozpracovanou činnost běžet. V režimu LLM se běžná role nestává
náhradním rozhodovačem při výpadku modelu. NPC však zůstává herní bytostí
pod pravidly TrinityCore, včetně smrti, boje a omezení pohybu. Ovládání všech
bojových, zásobovacích či ekonomických úkonů libovolnou instrukcí zatím
není součástí tohoto controlleru.

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

Pro ruční ověření v osídleném světě vyberte jedno LLM NPC a napište například
`/say Moje oblíbená barva je modrá.` Po několika dalších výměnách se zeptejte
`/say Jaká je moje oblíbená barva?`. Pro ověření starší paměti proveďte alespoň
sedm dalších výměn, aby první věta vypadla z dvanáctipoložkové historie.
Jiné NPC soukromou vzpomínku této postavy při stejné otázce nedostane.

## Vlastní životopis

Karty a vlastní humanoidní vzhled lze připravit také ve webovém
[Ateliéru postav](CharacterStudio.md) na portu 9092. Návrhy zůstávají oddělené
od běžícího světa; editor nabízí ověřený export karty, vzhledu a v `full-v8`
také vlastní herní šablony. Backstory není živý herní stav a samotné
autorské pole frakce nebo domova nepřepisuje registraci, faction template
ani skutečnou domovskou pozici.

Každá postava má samostatný JSON v [data/npc_agents](../data/npc_agents).
Identitu a polohu registrace ponechte podle skutečného agenta:

```json
{
  "agent_id": 901035,
  "map_id": 725,
  "spawn_id": 901035,
  "name": "Ukázka formátu",
  "backstory": "Sem patří autorem připravená minulost postavy.",
  "personality": "Sem patří způsob řeči a povaha postavy."
}
```

Číslo v ukázce vychází z předchozí populace; v `authoring-empty` není
registrovaným cílem. Příklad nevytváří kartu ani postavu na serveru.

Jméno smí mít nejvýše 32 znaků a 80 UTF-8 bajtů, životopis 8192 UTF-8 bajtů
a osobnost 2048. Celý JSON smí mít nejvýše 16 KiB. Životopis a osobnost
mohou obsahovat escapované nové řádky `\n` a tabulátory `\t`; `\r\n` se
při načtení převede na `\n`, samostatné `\r` není povolené.
Profily se načítají při spuštění. Změnu jména postavy s vlastní šablonou
nasazujte současně s odpovídajícím `npc_templates/<spawn>.json`; jejich
nesoulad způsobí odmítnutí volitelné karty. Pro změnu životopisu nebo osobnosti
stačí načtení opravené karty při řízeném restartu. V Dockeru
je adresář připojen pouze pro čtení jako `/workspace/data/npc_agents`.
Mimo lab lze nastavit přímo `AIWorld.NpcAgentEnabled`,
`AIWorld.NpcAgentProfilesDirectory`, `AIWorld.NpcAgentDecisionIntervalMs`,
`AIWorld.NpcAgentMaxInFlight` a `AIWorld.NpcAgentTimeoutMs` ve worldserver.conf.
Profil sám o sobě nemění řízení ani oprávnění jiné postavy.

Pro herní export v Ateliéru postav zvolte ověřený řízený cíl. Číslo běžného
NPC s původní AI nestačí k jeho registraci jako LLM agenta. Chybnou volitelnou
kartu server při startu odmítne s diagnostikou a pokračuje s platnými profily;
samotný import tak nezastaví celý svět. Vadný vzhled ponechá původní model.

## Psaní budoucích karet

Staré karty Matěje, Běly a Šaši se do prázdného světa automaticky nenačítají.
Jejich původní soubory a návrhy patří při tomto přechodu do chráněné zálohy;
historická verze kódu nebo dokumentace není pokynem k jejich obnovení.

Při psaní karet oddělujte minulost a přání od aktuálního stavu světa. Model dostává
potřeby na škále 0–1 a současný stav má přednost před starými výroky o hladu či
únavě. Odpočinek může být i dobrovolnou zálibou, jeho důvod má odpovídat stavu.
Úspěch přesunu, jídla či jiné akce potvrzuje engine. Model může nechat probíhající
činnost pokračovat a po dokončeném rozhovoru zůstat potichu. Zamítnutá cesta je
zpětná vazba pro zvážení jiné nabídnuté možnosti. Zpětná vazba obsahuje výsledek
i popis zvolené činnosti, aby model mohl rozpoznat konkrétní zamítnutý cíl.
Pořadí činností stále vybírá LLM.

## Ověřený stav a diagnostika

**Historické ověření před vyprázdněním mapy:** nasazení revize
`664d84687d7a` dne 31. října 2026 ověřilo následující
identity. Číslo `entry` v telemetrii zachovává původní behaviorální typ;
nové `template_entry` a entry uvnitř živého GUID označují skutečnou herní
šablonu. Původní záznam `11328` dál nese jméno `Eastvale Peasant`.

| Postava | Spawn ID / Agent ID | `entry` | `template_entry` | Zachovaná role |
| --- | --- | --- | --- | --- |
| Šaša | 901014 | 11328 | 9800014 | `CITY_CONSUMER` |
| Matěj | 901035 | 11328 | 9800035 | `WORKER` |
| Běla | 901036 | 11328 | 9800036 | `WORKER` |

Živá kontrola potvrdila všechny tři aktivní LLM controllery, šest různých
čerstvých snímků a osm zdravých služeb bez automatického restartu. Zbývajících
16 obyvatel stejné zdrojové šablony nebylo přejmenováno. To dokládá instalaci
a vazbu identit, nikoli dlouhodobou kvalitu dialogu nebo dokončení každé
navržené herní činnosti. Zobrazení nových jmen nad NPC nebylo při této kontrole
vizuálně ověřeno v klientu.

V `runtime/lab/logs/Server.log` hledejte `Character template aliases enabled`
a `LLM NPC controller enabled: profiles=... appearances=...`. Odmítnutý
volitelný import má zprávu `LLM NPC profile ignored` nebo `LLM NPC appearance ignored`.
Kontrola instalovaných úplných šablon je pouze pro čtení:

```sh
docker compose --env-file .env -f compose.yml exec -T --interactive=false -w /workspace worldserver \
  python3 -m tools.realm_lab.character_templates verify
```

Zdravý `/health` nenahrazuje skutečný úspěšný modelový požadavek ani tuto
databázovou kontrolu. Úplné CI pro uvedenou revizi prošlo se 846 Python
a 699 nativními testy; [výsledek běhu](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/41158138495)
je připnutý ke konkrétní revizi.

Po přechodu do `authoring-empty` se tyto tři identity ani počty nevyžadují
v aktuální telemetrii. Správný výsledek je prázdná populace a vypnutý
controller; budoucí osídlení potřebuje vlastní nové ověření. Ověření šablon
v prázdném světě potvrzuje nulovou instalaci, nikoli přítomnost starých tří
postav. Stav odstranění charakterových návrhů se ověřuje odděleně v knihovně
Ateliéru a jeho katalogu zdrojových karet.
