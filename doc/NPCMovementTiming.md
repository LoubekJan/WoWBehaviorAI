# Plynulost a načasování NPC

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

Počáteční rozhodnutí se rozptylují v rozsahu 0–15 sekund. Běžná pauza po
přesunu trvá 4–8 sekund, po činnosti 8–15 sekund; liší se mezi NPC i dalšími
cykly stejného NPC. Návrat domů a hledání potravy zachovávají krátkou
sekundovou prodlevu. Rozběhnutý pohyb stále provádí pohybový systém enginu.

Nasazovací konfigurace používá `Logger.ai=3,Console Server` (INFO). Zůstávají
navigační sondy, chyby a souhrny výkonu, ale ne tisíce diagnostických zpráv
o potřebách a vjemech každou sekundu. Detailní DEBUG lze dočasně vrátit
hodnotou 2.

## Krátký test po nasazení

1. Ověř úspěšné CI, deploy a novou revizi v `.server info`.
2. V Elwynnu sleduj 5–10 okolních samostatných NPC alespoň 3–5 minut.
   Začátky chůze a pauzy mají být různě načasované. Jednotlivé NPC se stále
   smí zastavit, rozhlížet, pást nebo odpočívat. Koordinovaný přesun členů
   jedné skupiny je očekávaný.
3. Vyzkoušej útok na jedno NPC: obrana/útěk a podpora spojenců musí dál
   fungovat. Běžná klidová pauza nesmí zablokovat reakci na nebezpečí.
4. Po několika minutách zkopíruj souhrny z terminálu na serveru:

   ```bash
   docker compose -f compose.yml -f compose.dev.yml logs --since=5m worldserver | grep AIWORLD_UPDATE
   ```

   Pokud Docker starší zprávy už nemá, stejné řádky jsou v souborovém logu:

   ```bash
   grep AIWORLD_UPDATE runtime/logs/Server.log | tail -10
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

Opakovaná zpoždění přes sekundový interval znamenají, že fronta nestíhá.
Velké `worldDiffMaxMs` při malém `totalMaxMs` ukazuje na potřebu měřit také
části serveru mimo AIWorld. Souhrn neprokazuje konkrétní nákladnou funkci
uvnitř pole `otherMaxMs`; k tomu by následovalo podrobnější měření.

Automatické C++ testy v `tests/game/AgentUpdateScheduler.cpp` ověřují rozptyl
1 859 sousedních ID, zachování uplynulého času, omezené odbavování po pauze
serveru, změny registru a rozptyl klidových prodlev. Běží přes existující
CI/CTest. Nenahrazují pozorování pohybu na skutečném serveru.
