# NPC řízená jazykovým modelem

Volitelný režim předává výběr činností, řeči a animací modelu přes stávající
AI bridge a jeho OpenAI-compatible HTTP klient. Model dostává životopis,
osobnost, aktuální potřeby, výsledek poslední činnosti, blízké postavy a krátký
rozhovor. Volí mezi nabídnutými pohyby, jídlem, odpočinkem, útěkem před skutečnou
hrozbou, čekáním a zastavením. Engine ověřuje dosažitelnost a provádí akce.

Připravené profily patří Matějovi (`901035`) a Běle (`901036`) na mapě 725.
Režim vyžaduje již připravený `consumers-v8`, `raids-v8` nebo `fields-v8`, kde jsou oba
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

Při ručním spuštění Compose načtěte oba soubory prostředí:

```sh
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
oslovovat také blízké NPC a reagovat na jejich řeč. Rozhovor má nejvýše 12
položek; historie rozhovoru se nyní neukládá a restart ji vymaže.
Zprávy s řídicími znaky nebo WoW značkami, například odkazy na předměty,
se do rozhovoru NPC nepředávají. Běžné zobrazení hráčovy řeči zůstává zachováno.

## Vlastní životopis

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
