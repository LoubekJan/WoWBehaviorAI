# Několikahodinový záznam chování AIWorld

Záznamník ukládá všechny agenty, které vidí Observer: AIWorld v Elwynnu, mapa 0.
Zachytí jejich role, lov, pohyb, návraty domů, potřeby, skupiny, jídlo a suroviny.
Každých 5 sekund uloží celý stav do komprimovaného souboru. Výpadky nebo zastaralá
data označí zvlášť, aby se při rozboru nezaměnily se zaseknutým NPC.
Po dokončení běhu také automaticky vyhodnotí chování a vytvoří report.

## Automatický herní test

Po úspěšném nasazení přes GitHub CI (`push` do `ai-world`) se test spustí sám.
Deploy nejprve uzavře předchozí záznam, ještě před změnou checkoutu nebo restartem
serveru. Potom nasadí přesný commit daného CI běhu. Po kontrolách zdraví a AI
počká nejvýše 180 sekund na dva navazující aktuální snímky živých rolí z nově
spuštěného serveru. Spustí nový záznam a do 60 sekund ověří první čerstvá data.
Každý deploy, i opakovaný pro stejný commit, vytvoří novou složku záznamu.

Test poběží **4 hodiny na pozadí**; nastavení `AIWORLD_RECORD_HOURS` v serverovém
`.env` může délku změnit. Report automaticky dostane SHA nasazeného commitu
v `build_label` a označení `deploy:<SHA>`. Předchozí archivy zůstávají zachované.
Pokud další deploy přijde dřív, předchozí test skončí předčasně a také vytvoří
report; nedostatek dat označí jako `INCONCLUSIVE`, prokázanou chybu jako `FAIL`.

Zelený deploy potvrzuje spuštění sběru, nikoli výsledek čtyřhodinového testu.
Pozdější `FAIL` nebo `INCONCLUSIVE` nezmění výsledek CI a nevrací nasazení zpět.
Finální výsledek najdeš v reportu a přes `make record-aiworld-status`.
Při neúspěšných kontrolách deploye se nový test nespustí; pokud se nepodaří
ověřit jeho první data, CI jej zastaví a označí deploy jako neúspěšný.

Po ručním nasazení na serveru s běžícím Observerem spusť:

```sh
make test-aiworld
```

Jde o stejnou službu jako `make record-aiworld`: běží čtyři hodiny na pozadí,
ukládá původní data a současně průběžně vyhodnocuje sledovaná pravidla. Po
skončení uloží `behavior-report.md` pro čtení a `behavior-report.json` pro další
automatické zpracování. Souhrn `summary.json` dostane také pole `behavior`.
Výsledky najdeš v nové složce `runtime/recordings/aiworld-<čas>-<id>/`.
Test nemění NPC ani neposílá příkazy do hry. Není třeba překládat worldserver.

Pro srovnatelné výsledky nech svět běžet bez zásahů a nastav
`AIWorld.ElwynnAlwaysActive = 1`. Záznamník musí sledovat aktuální telemetrii
verze 2, 3 nebo 4. Verze 1 neobsahuje potřebné údaje a dostane `INCONCLUSIVE`.

| Kontrola | Výchozí pravidlo |
| --- | --- |
| Zablokovaný návrat | Po hlášeném selhání návratu je NPC alespoň 5 minut nejvýše 1 yard od sledovaného bodu a dál mimo svůj běžný domovský okruh. Změna na náhradní animaci nebo `movement=NONE` toto měření sama nezruší. |
| Nedokončený návrat | Po pozorovaném `RETURN_HOME` nebo selhání návratu zůstává místní role alespoň 10 minut mimo domovský okruh. Pohyb toto měření nenuluje, takže zachytí i chození tam a zpět. Návrat domů, jiná činnost, boj, root, evade nebo výpadek návaznost přeruší. |
| Pohyb bez postupu | Nejméně 60 sekund v místním `MOVING` bez posunu přes 1 yard; samotné `moving=true` nestačí jako důkaz pohybu. Boj, root a evade se nepočítají. |
| Odchod z Elwynnu | NPC dříve pozorované pod řízením role zůstává alespoň 30 sekund v `OUTSIDE_ELWYNN`. Po výpadku, smrti či ztrátě živé reprezentace se návaznost neodvozuje. |
| Hlad navzdory zásobám | Civilní/bojová role se zapnutým rozšířením má mimo boj alespoň 10 minut hlad ≥ 0,95 a Food > 0. |
| Prázdná zásoba pracovníka | Pracovník s připraveným domovem a pracovištěm má mimo boj alespoň 10 minut hlad ≥ 0,95 a Food = 0. Vyžaduje zapnuté rozšíření; dřevorubec entry 1975 vyrábějící suroviny je vynechán. |
| Hlad predátorů | Souvislý hlad ≥ 0,95 po 30 minut vyvolá upozornění. Sám nezpůsobí selhání — kořist nemusí být dostupná. |
| Hlad kořisti | Kořist v `IDLE`, `ACTING` či `MOVING` má mimo boj, root a evade alespoň 10 minut hlad ≥ 0,95. Zachytí i hladovění při pohybu; pastva se skutečným poklesem hladu měření ukončí. |

Report uvádí spawn, nejdelší pozorovanou epizodu daného pravidla, časy UTC,
souřadnice a poslední pohybový důvod. Překročení kteréhokoli prahu kromě upozornění na hlad predátorů znamená
`FAIL` a zaslouží kontrolu; report sám neurčuje příčinu chyby navmeshe.
Souhrny rolí ukazují vzorkované začátky lovu/krmení, poklesy hladu a změny zásob.
Stání služeb ani dosažení stropu surovin 20 nejsou automaticky závadou.

Telemetrie v4 navíc ukládá `living_role.return_recovery`: strategii návratu,
poslední chybu, počet neúspěchů a zapamatovaných bodů, čas bez posunu a do dalšího
pokusu, příznaky cesty, původní/vyřešenou výšku a počty odmítnutí podle důvodu.
Tyto údaje přetrvají i při náhradní animaci nebo `movement=NONE`; automatický
test z nich také rozpozná selhání návratu. Podrobnosti nálezu jsou v JSON reportu.
Starší archivy zůstávají čitelné, ale nové údaje nelze zpětně dopočítat.
Pro v4 je nutné aktualizovat nejprve Observer a potom worldserver podle
[návodu](LivingRoles.md#ověření-oprav-z-25-9); samotný záznamník rebuild enginu nepotřebuje.

| Výsledek | Význam | Kód ukončení služby |
| --- | --- | ---: |
| `PASS` | Dostatečná data neukázala porušení sledovaných pravidel. Může obsahovat upozornění na hlad predátorů. | 0 |
| `FAIL` | Některé sledované pravidlo bylo porušeno; report ukazuje konkrétní případy. | 3 |
| `INCONCLUSIVE` | Chybí dostatek kvalitních dat nebo potřebné údaje. | 2 |

Pro `PASS` je výchozí minimum hodina navazujících čerstvých dat, nejméně 95 %
délky běhu a žádná zjištěná chyba posloupnosti, času, struktury nebo integrity.
Kontroly bez vhodných pozorovaných NPC mají `NOT_OBSERVED`. Pokud je současně
prokázané porušení a neúplný záznam, celkový výsledek je `FAIL`, ale kvalita dat
zůstane samostatně `INCONCLUSIVE`. Zastaralé snímky, mrtvé NPC a spawnové
souřadnice se nesčítají do doby nehybnosti. Epizody se nepřenášejí přes výpadky.

`PASS` nepotvrzuje nepozorované scénáře: zejména napadení hráčem, pomoc spojenců,
jednotlivé údery, vzhled animací, výkon CPU ani pilot smečkových vlků.
Vzorkování není přesný registr úlovků a neumožňuje spočítat jejich úspěšnost.

### Délka, označení sestavení a kontrola výsledku

```sh
AIWORLD_RECORD_HOURS=4 AIWORLD_RECORD_BUILD_LABEL="nasazeny-commit" make test-aiworld
make record-aiworld-status
```

Při ručním spuštění nahraď `nasazeny-commit` označením skutečně nasazeného
sestavení. Bez něj report uvádí `unknown`; záznamník nemůže ověřit shodu checkoutu
s běžící binárkou. CI deploy toto označení nastavuje sám na nasazovaný SHA,
nezávisle na hodnotě `AIWORLD_RECORD_BUILD_LABEL` v `.env`.
Pro krátký technický pokus lze použít například
`AIWORLD_RECORD_HOURS=0.1 AIWORLD_TEST_MINUTES=5 make test-aiworld`.
Takto krátký běh neověří pravidla s delším časovým prahem.
Pozorovaná pravidla s delším prahem proto zůstanou `INCONCLUSIVE`, i když
zkrácené minimum pro kvalitu dat už bylo splněné.

Příkaz `make test-aiworld` se vrátí hned po spuštění kontejneru. Jeho úspěch
není výsledek herního testu. Po skončení zkontroluj report a poslední log
v `make record-aiworld-status`; Docker ukáže finální kód služby.
Opětovné spuštění dokončené služby vytvoří novou session. Spuštění už běžící
služby se stejným nastavením ji ponechá běžet; nevytvoří souběžný druhý test.

### Vyhodnocení starého záznamu

Všechny části a souhrn dej do jedné složky pod `runtime/recordings` a spusť:

```sh
make analyze-aiworld SESSION=aiworld-20260924T155848Z-d7abf197
```

Tento příkaz pouze čte archiv a zapisuje report; nespouští worldserver ani
Observer. Archiv čte postupně včetně kontroly CRC každé gzip části. Chybějící
část, jiná session, porušené pořadí nebo nesouhlas s finálním souhrnem nemohou
dostat `PASS`. Report vytvářený během živého sběru používá přímo právě ukládané
snímky; pro následnou kontrolu integrity uložených souborů použij tento příkaz.

Bez Dockeru funguje Python 3.11+ bez dalších balíčků:

```sh
python3 docker/world-viewer/app/behavior.py cesta/k/session --output runtime/analysis/vysledek
```

Volba `--output` umožňuje ponechat dodaný archiv úplně beze změn. Původní gzip
části ani `summary.json` offline vyhodnocení nikdy nepřepisuje.

### Automatické kontroly samotného vyhodnocovače

Při pushi/PR běží v CI také linuxové testy sběru a vyhodnocení. Ověřují
skutečný HTTP sběr, rotaci a čtení gzip, odhalení popsaných poruch, přerušení
posloupnosti, legitimní stání, návratové kódy a ukončení přes `SIGTERM`.
Ověřují také odmítnutí staré telemetrie po restartu a rozpoznání nového záznamu
správného sestavení. Nasazení čeká i na úspěch tohoto testovacího jobu.
Čtyřhodinový běh světa začíná po úspěšných kontrolách deploye, jak je popsáno výše.

## 1. Ruční spuštění na serveru

Přenes aktualizované soubory do repozitáře na serveru. Pro samotný záznamník není
potřeba překládat ani restartovat worldserver. Musí běžet worldserver s telemetrií
a funkční Observer — v prohlížeči musí ukazovat aktuální agenty.

V kořeni repozitáře spusť:

```sh
make record-aiworld
```

Záznam poběží **4 hodiny** na pozadí. Můžeš zavřít SSH; sám se ukončí. Pro osm
hodin použij místo toho:

```sh
AIWORLD_RECORD_HOURS=8 make record-aiworld
```

Pokud nemáš `make`, základní příkaz je `docker compose up -d aiworld-recorder`.
Samostatná služba používá Python image; při prvním spuštění se případně stáhne.
Samotné `make start`, `make build` ani `make restart-world` nový záznam nezapínají;
automatický start je součástí GitHub CI deploye. `make stop` jej zastaví spolu se serverem.
Po restartu hostitele nebo Dockeru je potřeba jej znovu spustit.

## 2. Ověření po první minutě

```sh
make record-aiworld-status
```

V posledním výpisu hledej `last_status: "fresh"` a rostoucí počítadlo `fresh`
uvnitř `counts`. Hodnota `max_agents` musí být větší než nula. Po jedné minutě
bývá přibližně 12 čerstvých vzorků; nejde o počet NPC ani událostí.

Pokud uvidíš jiný stav:

| Stav | Význam / co ověřit |
| --- | --- |
| `unconfigured` | Observer nemá nastavený `WORLD_VIEWER_TELEMETRY_TOKEN`; zkontroluj konfiguraci telemetrie. |
| `waiting` | Observer ještě nedostal snímek; ověř běžící worldserver, zapnutou telemetrii a shodný token obou služeb. |
| `stale` | Přenos snímků se zastavil; samotné NPC z těchto dat posoudit nelze. |
| `empty` | Přenos běží, ale snímek neobsahuje žádné agenty AIWorld. |
| `error` | Observer není dostupný nebo vrací neplatnou odpověď. Výpis obsahuje typ chyby, případně HTTP kód. |

Záznamník při výpadku pokračuje v pokusech a zaznamená i obnovení spojení.
Pokud za celou dobu nezíská ani jeden čerstvý neprázdný snímek, skončí s kódem 2.

## 3. Průběh testu

Pro porovnání opravy návratů z 26. 9. nech **celé čtyři hodiny bez zásahů**.
Nový svět i Observer nasazuj společně; recorder se po CI deployi spouští
dosavadním automatickým postupem. Vedle výsledku `return` kontroluj také
`return_duration`, který zachytí nekonečný návrat i při skutečném pohybu.
Předchozí běh měl 34 nehybných návratů a 33 nedokončených návratů nad
deset minut. Hlad kořisti a pohyb bez posunu byly bez nálezu a mají tak zůstat.

V detailu NPC a `return_recovery.navigation` jsou nově dostupné:

| Pole | Význam |
| --- | --- |
| `mesh`, `start_tile`, `end_tile` | Dostupnost navigační sítě a obou dlaždic při posledním výpočtu. |
| `filter`, `start_flags`, `end_flags` | Povolené typy terénu a příznaky nejbližších nalezených polygonů. |
| `start_distance`, `end_distance` | Vzdálenost ve 3D od polygonu v yardech; `null` znamená, že nebyl nalezen/změřen. |
| `swimming`, `rejoin` | Byl sestaven vodní spojovací krok nebo krátké připojení na pozemní síť. |
| `failure` | Např. `MISSING_NAVMESH_TILE`, `NO_COMPLETE_PATH`, `REJOIN_UNSAFE_SURFACE`, `PATH_BOUNDS`, `DANGER_BLOCKED`; `NONE` bez odmítnutí. |
| `return_recovery.backtracks` | Počet zahájených povolených ústupů do navštíveného bodu, nejvýše 16 za návrat. |

Jde o uchovanou poslední diagnostiku, nikoli počítadlo nových událostí.
`path_type` zůstává výsledkem původního výpočtu navmeshe: u povoleného
vodního spojení může mít dál hodnotu 68, ale `swimming=true` a `failure=NONE`.
Nenulová chyba může následně odmítnout i sestavený spojovací krok.
Staré záznamy nemají `navigation` a nelze z nich dodatečně zjistit vzdálenosti
od polygonů. Samotný hlad predátorů dál zůstává upozorněním: bez dostupné
kořisti není důkazem chyby pohybu.

Nech nejprve svět alespoň hodinu běžet bez vlastních zásahů. Pro simulaci bez
přítomnosti hráče musí být na běžícím serveru aktivní `AIWorld.ElwynnAlwaysActive = 1`;
v aktuální verzované konfiguraci je zapnuté. Díky tomu nemusíš stát u pavouků.

Potom můžeš zkusit lov, útěk, obranu spojenců nebo práci dřevorubců. Poznamenej
si čas a pokud možno `spawnId` dotčeného NPC. Připiš, zda jsi měl zapnuté GM,
změněnou rychlost nebo jiné úpravy testovací postavy; ovlivňují interpretaci.

Záznam po 5 sekundách ukáže delší průběh a opakující se problémy. Nezaručuje
zachycení každého jednotlivého úderu nebo krátké změny. Poslední důvod ukončení
lovu může zůstat stejný i během další činnosti, a proto nejde jednoduše počítat
každý výskyt `HUNT_LEASH` jako nový neúspěšný lov. Dlouhodobá paměť otevřená
v detailu NPC se tímto záznamníkem nestahuje.

## 4. Ukončení a soubory k poslání

Po nastavené době se záznam sám uzavře. Pro dřívější ukončení:

```sh
make record-aiworld-stop
make record-aiworld-status
```

Výsledky najdeš na serveru v:

```text
runtime/recordings/aiworld-<čas UTC>-<id>/
  summary.json
  part-0001.jsonl.gz
  part-0002.jsonl.gz   (jen pokud záznam narostl)
  behavior-report.md
  behavior-report.json
```

Pošli **celou složku dokončeného běhu** — `summary.json` a všechny `part-*.jsonl.gz`.
Soubory už jsou komprimované; není nutné je rozbalovat. Každá část má přibližný
limit 64 MiB, po něm se otevře další. Celková velikost závisí na délce a počtu
agentů. Staré záznamy se nepřepisují ani automaticky nemažou.

V `summary.json` bude na konci `status: "finished"` a `stop_reason: "duration"`
nebo `"stopped"`. `usable: true` znamená, že existují čerstvá data. Výsledek chování
je samostatně v `behavior.status` a v reportu. Násilné ukončení nebo výpadek napájení může poškodit
rozepsanou část; již uzavřené části zůstanou čitelné.

K záznamu připoj krátce verzi/commit nasazeného serveru, délku testu a své
pozorování. Volitelně přilož `runtime/logs/Server.log` z téže doby — zkopíruj ho
**před restartem worldserveru**, protože aktuální konfigurace jej při startu
přepisuje.
