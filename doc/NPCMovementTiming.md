# Plynulost a načasování NPC

Platí pro současný vlastní svět: realm 2, mapa 725 / oblast 4988,
profil `full-v8`, 100 NPC pod AIWorld. Deterministické role, zásobovací
trasy, skupiny a volitelné LLM postavy používají různé rozhodovací cykly;
samotná společná telemetrie neznamená totožné časování všech NPC.

Potřeby a vnímání se dříve zpracovávaly pro celý registr najednou v sekundových
dávkách. Role navíc používaly `agentId % 15000` pro první rozhodnutí a stejnou
šestisekundovou pauzu po přesunu. Sousední ID tak začínala v jednom updatu
a podobně dlouhé přesuny je mohly udržovat ve společném rytmu.

Nově má každý agent vlastní termín aktualizace, rozptýlený stabilním hashem
přes celý interval. Vnímání a potřeby mají odlišné fáze. Každý průchod zpracuje
nejvýše 128 agentů a před dalším agentem kontroluje rozpočet 4 ms. Jednotlivý
výpočet se nepřerušuje, takže jeden náročný agent může tento rozpočet překročit.
Odložená práce zůstane ve frontě podle stáří termínu. Vynechané intervaly se
nepřehrávají hromadně; potřeby dostávají skutečně uplynulý čas daného agenta.

Výpočty nových cest uvnitř potřeb mají navíc společný rozpočet 2 ms a nejvýše
16 plánovacích pokusů na aktualizaci světa. Kontrola probíhá před jednotlivými
kandidáty a krátkými dávkami kontroly terénu. Dokončení již zahájeného dotazu se
nepřerušuje. Rozpočet proto není tvrdý limit délky jednoho dotazu na navigaci.
Nedokončené hledání si pamatuje dalšího kandidáta; odložení nezvyšuje počítadlo
selhání návratu. Pohyb, dokončení krmení a reakce na nebezpečí pokračují před
kontrolou rozpočtu. Změna polohy, domova, nebezpečí či fáze světa zruší původní
rozpracovaný dotaz. Jednotlivé strategie mají vlastní pracovní a časové
limity; čekání na rozpočet samo o sobě není neúspěšná cesta. Podrobnosti
zachování a invalidace návratového grafu jsou v
[RecoveryPlanning.md](RecoveryPlanning.md).

Počáteční rozhodnutí se rozptylují v rozsahu 0–15 sekund. Běžná pauza po
přesunu trvá 4–8 sekund, po činnosti 8–15 sekund; liší se mezi NPC i dalšími
cykly stejného NPC. Při pokračování ověřené návratové nebo průzkumné trasy
se další rozhodnutí probudí při příchodu bez vložené sekundové pauzy;
jiný návrat nebo nové hledání si může krátkou prodlevu ponechat.
Rozběhnutý pohyb stále provádí pohybový systém enginu.

Zásobovací přesuny slučují přímé ověřené body, zachovávají rohy a navazují
po skutečném příchodu. Čekání při hledání objížďky, pětisekundová práce,
krádež, nakládka a vykládka zůstávají samostatné činnosti. Karavany čekají
také na vlastní doprovod; jejich pauzu proto nelze automaticky označit za
zaseknutí. U LLM postavy rozlišuj interval dalšího rozhodnutí, běžící
požadavek modelu a skutečně prováděnou akci v `llm_agent`.

Nasazovací konfigurace používá `Logger.ai=3,Console Server` (INFO). Zůstávají
navigační sondy, chyby a souhrny výkonu, ale ne tisíce diagnostických zpráv
o potřebách a vjemech každou sekundu. Detailní DEBUG lze dočasně vrátit
hodnotou 2.

## Krátký test po nasazení

1. Ověř úspěšné CI, deploy a novou revizi v `.server info`.
2. Na vlastní mapě 725 sleduj 5–10 okolních samostatných NPC alespoň 3–5 minut.
   Začátky chůze a pauzy mají být různě načasované. Jednotlivé NPC se stále
   smí zastavit, rozhlížet, pást nebo odpočívat. Koordinovaný přesun členů
   jedné skupiny je očekávaný.
3. Vyzkoušej útok na jedno NPC: obrana/útěk a podpora spojenců musí dál
   fungovat. Běžná klidová pauza nesmí zablokovat reakci na nebezpečí.
4. Po několika minutách zkopíruj souhrny z terminálu na serveru:

   ```bash
   docker compose --env-file .env -f compose.yml logs --since=5m worldserver | grep AIWORLD_UPDATE
   ```

   Pokud Docker starší zprávy už nemá, stejné řádky jsou v souborovém logu:

   ```bash
   grep AIWORLD_UPDATE runtime/lab/logs/Server.log | tail -10
   ```

5. Teprve když se pohyb jeví správně, pokračuj dlouhým záznamem chování.
   Ruční test útokem neoznačuj jako běh bez zásahů.

## Jak číst souhrn `AIWORLD_UPDATE`

Vzniká jednou za přibližně 30 sekund. Maxima různých částí mohou pocházet
z různých aktualizací; nesčítají se.

| Pole | Význam |
| --- | --- |
| `windowMs`, `ticks` | Délka okna a počet aktualizací AIWorld. |
| `worldDiffMaxMs` | Nejdelší interval mezi aktualizacemi světa, nikoli čas samotné AI. |
| `totalMaxMs` | Nejdelší zpracování AIWorld v jednom updatu. |
| `needsMaxMs` | Nejdelší dávka potřeb a role, včetně hledání cest pro tuto dávku. |
| `perceptionMaxMs` | Nejdelší dávka pravidelného vnímání. |
| `telemetryMaxMs` | Nejdelší sběr telemetrie na vlákně světa. |
| `otherMaxMs` | Ostatní práce AIWorld: události, skupiny, plánovače atd. |
| `needsAgents`, `perceptionAgents` | Počet odbavených položek; zahrnuje i kontrolu nenahraných NPC. |
| `needsLateMaxMs`, `perceptionLateMaxMs` | Největší zpoždění proti termínu agenta, včetně čekající práce. |
| `planningStarted`, `planningDeferred` | Počet zahájených plánovacích bloků a odložení kvůli rozpočtu. Odložení není chyba cesty. |
| `planningTotalMs`, `planningMaxMs` | Součet časů plánovacích bloků a nejdelší jednotlivý blok v okně. |

Opakovaná zpoždění přes sekundový interval znamenají, že fronta nestíhá.
Velké `worldDiffMaxMs` při malém `totalMaxMs` ukazuje na potřebu měřit také
části serveru mimo AIWorld. Souhrn neprokazuje konkrétní nákladnou funkci
uvnitř pole `otherMaxMs`; k tomu by následovalo podrobnější měření.

Automatické C++ testy v `tests/game/AgentUpdateScheduler.cpp` ověřují rozptyl
1 859 sousedních ID, zachování uplynulého času, omezené odbavování po pauze
serveru, změny registru a rozptyl klidových prodlev. Běží přes existující
CI/CTest. Nenahrazují pozorování pohybu na skutečném serveru.
