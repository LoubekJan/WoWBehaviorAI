# Vývoj a provoz vlastního světa

Vývoj volitelných NPC řízených LLM, samostatné životopisy a přímý herní chat
popisuje [NpcAgents.md](doc/NpcAgents.md). Režim se zapíná zvlášť a výchozí
provozní profily ho nechávají vypnutý.

Vlastní humanoidní vzhled, charakterové karty a příběhy lze tvořit ve webovém
[Ateliéru postav](doc/CharacterStudio.md) na portu 9092. Uložené návrhy se do
herního světa nasazují až samostatným exportem.

Aktivní větev je `AI-World-lab`. Výchozí `compose.yml` provozuje jediný
herní realm **AI World Lab**, ID **2**, s vlastní mapou **725** a oblastí
**4988**. Profil `full-v8` přebírá všech 100 vytvořených NPC pod AIWorld.
Rozšiřuje `defias-v8`, který řídí 55 NPC: deset banditů vyráží ve skupinách,
šest dalších obyvatel tábora jí z vlastního skladu. Nasazení a
fyzický výsledek se ověřují samostatně. Výchozí
`.env.example` zůstává konzervativním profilem `settlement-v8`.

## Služby a data

| Služba | Úloha | Port hostu |
| --- | --- | --- |
| `auth-mysql` | Účty, přihlášení a seznam realmů | interní |
| `authserver` | Přihlášení existujících i nových účtů | 3724 |
| `lab-mysql` | Postavy, svět a AI paměť vlastního realmu | interní |
| `worldserver` | Herní svět a interní SOAP | 9086 |
| `world-viewer` | Pozice, zdraví a bojový stav 100 NPC | 9091 |
| `character-studio` | Editor vzhledu, osobnosti a životopisů NPC | 9092 |
| `account-web` | Formulář pro vytvoření účtů přes TrinityCore | 9088 |
| `ai-server` | AI protokoly; modelové požadavky jsou vypnuté | interní |
| `tc-dev` | Build, extraktory a správcovské nástroje | profil `tools` |
| `aiworld-recorder` | Volitelný záznam telemetrie | profil `recording` |

Compose zachovává identitu `aitc-lab`, databázi `aitc_lab_mysql-data`,
build `aitc_lab_build-data`, cache `aitc_lab_ccache-data` a soubory
`runtime/lab/`. Účty mají samostatný volume `aitc_auth_mysql-data`.
Všechny služby používají interní síť tohoto stacku; původní Elwynn
stack ani externí auth síť nejsou potřeba. SOAP port 7878 se na hostu
nezveřejňuje.

## Přechod existujícího serveru

Existující instalace musí nejprve převést celé `auth` ze staré MySQL do
nové `auth-mysql`, zachovat účty, přístupová práva a záznamy realmu 2,
vyřadit původní realm 1 a staré automatické nasazování. Staré databáze
se uchovají v chráněné záloze. Import TDB ani vytvoření náhradních účtů
se při tomto přechodu neopakují. Provozní stav a historii mapových
balíků popisuje [RealmLab.md](doc/RealmLab.md).

Root `.env` nahradil dřívější `deploy/lab/.env`. Zachovat existující
lab hesla, telemetry token, realm ID, název i porty; doplnit samostatné
přihlašovací údaje nové auth MySQL a jejího schema ownera. Soubory
`.env` ani databázové dumpy se necommitují.

## Čistá instalace na Linuxu

Potřebné jsou Docker Engine s Compose **2.24+**, Git, Make a Python 3.
Compose stáhne build závislosti; není potřeba hostitelská instalace C++.

```bash
git clone --branch AI-World-lab https://github.com/LoubekJan/WoWBehaviorAI.git
cd WoWBehaviorAI
umask 077
cp .env.example .env
```

V `.env` nahradit všechna `change-me-*` vlastními hesly a tokenem,
nastavit adresy dostupné klientovi. Auth schema owner a world DML účet
jsou oddělené účty MySQL; herní účty spravuje TrinityCore.

```bash
make bootstrap
make config
make build
make test
make init-auth
make register-realm
make import-tdb TDB_VERSION=TDB335.25101 TDB_SHA256=b426641e6ce8da02e4109b40e570c2db75f89c228a412b3113f82eb3c8725299
```

Import TDB patří pouze do nové prázdné world databáze. Do
`runtime/lab/data/{dbc,maps,vmaps,mmaps}` dodat kompatibilní extrahovaná
data klienta 3.3.5a včetně schváleného serverového balíku vlastní mapy.
Šedesát jedna souborů balíku musí odpovídat manifestu
`data/realm_lab/aiworldlab/preview-v8/server-manifest.json`, včetně
potřebných sdílených VMAP modelů. Samotné zdrojové ADT z Noggit nestačí.
Do `runtime/lab/dbc-base/` patří neupravené `Faction.dbc` a
`FactionTemplate.dbc` pro generátor vlastních frakcí a `WorldSafeLocs.dbc`
pro hřbitov vlastní mapy.

```bash
make dbc-factions
make graveyard-data
make setup-world
make start
```

`setup-world` pod zámkem zastaví pouze vlastní worldserver, aktualizuje
world/characters schéma, nastaví start nových postav na vlastní mapu a
zapojí hřbitov a vloží či ověří přesných 100 NPC. Předchozí startovní záznamy uloží do
privátního receipt souboru. Existující postavy ani jejich homebindy
nepřesouvá. Mapové hashe a databázové kontrakty se kontrolují před
spuštěním; neúspěšná kontrola svět nespustí.

Klient potřebuje shodný mapový patch, patch vlastních faction DBC a
`runtime/lab/client-patch/patch-X.MPQ` s hřbitovem. Účet používá rozšíření
Wrath of the Lich King (2).
`realmlist.wtf` míří na auth adresu serveru; při výchozím portu 3724
například `set realmlist 192.168.0.248`. Herní port 9086 obdrží ze
seznamu realmů.

## Registrace účtů

`account-web` je na `http://<server>:9088/`. Nastavit `TC_SOAP_USER` a
`TC_SOAP_PASSWORD` na existující administrátorský herní účet pro realm
2 nebo globální realm `-1`; `LAB_SOAP_ENABLED=1` ponechá SOAP pouze
uvnitř Dockeru. První administrátorský účet lze vytvořit přes
TrinityCore konzoli:

```bash
docker attach aitc-lab-worldserver-1
# V konzoli serveru:
# account create JMENO HESLO
# account set gmlevel JMENO 3 -1
# Odpojení od konzole: Ctrl+P a poté Ctrl+Q.
docker compose --env-file .env -f compose.yml up -d --force-recreate account-web
```

Formulář začne vytvářet účty po nastavení servisního přihlášení.
Umožňuje také založení administrátora; port 9088 patří pouze do
spravované sítě.

## Běžný provoz

```bash
make start                 # Zapne celý stack a počká na healthchecks.
make restart-world         # Restartuje jen herní server.
make world-logs
make backup                # Privátní gzip dumpy auth a world/characters + SHA256.
make record                # Zahájí výchozí čtyřhodinový záznam.
make record-status
make record-stop
make stop                  # Zastaví kontejnery; volumes zachová.
```

Zálohy vznikají v `runtime/lab/backups/` s adresáři 0700 a soubory 0600,
včetně privátní konfigurace. Každá DB má konzistentní transakční dump;
pro společný migrační okamžik nejprve zastavit zápisy do světa i účtů.

Pro vývoj používat `make shell`, pro C++ kontroly `make test`.
Python kontroly: `python3 -m unittest discover -s tools/realm_lab/tests -v`
a testy v `docker/world-viewer`, `docker/ai`, `docker/account-web`.
Soubor `Makefile.lab` a `compose.lab.yml` zůstávají kompatibilními vstupy
do stejného stacku. Hlavní nasazování má jedinou větev `AI-World-lab`;
CI používá vlastní `aitc_lab_ci_*` build volumes.

Názvy `data/elwynn/factions` a `tools/dbc/build_elwynn_factions.py`
označují sdílené definice frakcí současných NPC. Jejich zachování
neobnovuje původní svět. Historické návratové/hunt testy zůstávají
samostatnými regresními profily. Výchozí `.env.example` používá nativní
`settlement-v8`; zvířata se aktivují profilem `wildlife-v8`.

## Vývoj zvěře

V `.env` nastav `LAB_AI_PROFILE=wildlife-v8` a `LAB_WILDLIFE_ENABLED=1`.
`make setup-world` připraví při zastaveném světě stejné spawny a AI vazby
20 zvířat; `make start` spustí stack. CI/CD tuto přípravu provádí samo pod
deploy zámkem. Před první aktivací použij `make backup`.

Pozastavení používá `LAB_WILDLIFE_ENABLED=0` a nové vytvoření kontejneru
worldserveru, aby se načetlo prostředí. Identita a dlouhodobá paměť zůstává;
individuální potřeby zatím nejsou uložené přes restart. Podrobnosti a
kritéria fyzického testu jsou v [doc/RealmLab.md](doc/RealmLab.md).

Pro měření lovu nastav u recorderu interval 1 sekunda; delší interval může
minout krátký přechod živé kořisti na mrtvolu. Záznam vyhodnotíš příkazem:

```bash
python3 tools/realm_lab/wildlife_acceptance.py runtime/lab/recordings/aiworld-SESSION --output runtime/lab/validation/wildlife-acceptance.json
```

Výsledky jsou `PASS`, `FAIL` nebo `INCONCLUSIVE` (exit 0, 3, 2). Zdravá
služba a dvě platné telemetrické dávky samy lovecký cyklus nepotvrzují.

## Všechna vytvořená NPC — full-v8

Profil přidává zbývajících 45 identit nad nezměněný `defias-v8`:
21 strážných, 7 poskytovatelů služeb, 4 pracovníky a 13 bojovníků.
Celkem jde o **100 řízených / 0 nativně řízených NPC**. Mapu, šablony,
obchodní nabídky, frakce, domovy ani trvalou paměť tento převod nepřepisuje.
Aktivace je jedna podmíněná transakce po offline ověření všech identit.

V soukromém `.env` nastavit `LAB_AI_PROFILE=full-v8` a ponechat
`LAB_WILDLIFE_ENABLED=1`. Běžné CI/CD profil připraví i ověří automaticky.
Ruční offline příprava používá `make setup-world`; zastavení světa a zámek
zajišťuje společný skript. Přímé kontroly a pauza za zastaveného worldserveru:

```bash
docker compose --profile tools run --rm --no-deps -e LAB_SETTLEMENT_WORLD_ABSENT=1 tc-dev python3 tools/realm_lab/manage.py preflight-full-v8 /workspace/runtime/lab/data
docker compose --profile tools run --rm --no-deps -e LAB_SETTLEMENT_WORLD_ABSENT=1 tc-dev python3 tools/realm_lab/manage.py deactivate-full-v8 /workspace/runtime/lab/data
```

Nové postavy používají obecné životní role AIWorld. Skutečná těžba,
rybolov nebo nová ekonomika tím nevzniká. Původní automatické SmartAI
skripty převzatých postav už neběží; zahrnuje to jejich zvláštní bojová
kouzla a sváteční události. Běžné nabídky obchodníků, trenérů a hostinských
i obnova zdraví a respawn zůstávají součástí herního jádra.

Vlastní LLM kartu lze nyní přiřadit i `901014`. Registrace všech 100 není
zapnutí 100 modelových agentů: karty se instalují jednotlivě, současný
limit je 16 karet a obvyklý limit souběžných požadavků zůstává 2.
Nativní šestipolový profil a samostatný vzhled se nemění.

Kontrola nasazení vyžaduje čerstvou telemetrii všech 100 řízených identit,
správné běžné typy a role. Původní sklady, pole a přepady se kontrolují
stejně přísně; nové obecné role nesmějí získat nepřiřazený sklad či přepad.
Pro fyzický záznam výprav používat
`python -m tools.realm_lab.defias_group_acceptance <recording-directory> --profile full-v8`.
Zdraví služeb ani převzetí vlastnictví samo nepotvrzuje dokončenou fyzickou výpravu.

## Celý tábor Defias — defias-v8

[Manifest](data/realm_lab/aiworldlab/preview-v8/defias-population.json)
rozšiřuje nezměněný `fields-v8` o 15 existujících identit. Všech 16 Defias
901044–901059 je řízených; celkem jde o **55 řízených a 45 nativních NPC**.
Deset banditů 901050–901059 má schopnost přepadů. Velitel 901044, tři
stráže 901045–901047 a dodavatelé 901048/901049 zůstávají obyvateli tábora
a jedí z jeho skladu 3. Zachovávají běžné typy strážce/civilista/bojovník,
frakci Defias, původní domovy i všech 61 souborů mapové geometrie.

Výpravy používají existující návrh koalice a rozmístění členů do formace.
Skupina má 2–5 členů; deset banditů běžně vytvoří nejvýše dvě výpravy.
Sestaví se v táboře a sdílí sklad 1/4/5/6, trasu a číslo pokusu po celou
výpravu. Vedoucí výpravy, jeden z banditů, volí cíl ze své vlastní
krátkodobé paměti. Každý člen si dál pamatuje pouze vlastní pozorování
a výsledky; sdílený cíl mu nepředává
vzdálené zásoby ani cizí zkušenosti. Mluvící NPC používají svůj dosavadní
řadič. Skupinové přesuny používají běžné individuální akce s navigací,
kolizemi a viditelností.

Členové na sebe při shromáždění a na společných úsecích čekají. Boj,
ztracený velitel, chybějící člen nebo vypršení čekání může ukončit postup
a zahájit ústup. Každý živý člen se vrací vlastní cestou; smrt ztrácí jeho
skutečný náklad. Skupinové dokončení nikoho nepřemístí ani nepřipíše jídlo.
Každý bandita musí sám dokončit pět sekund práce u cílového skladu,
unese nejvýše dvě porce a po návratu je samostatně pět sekund odevzdává.
Neúspěšná nebo přerušená práce potravu nevytváří. Všech šest skladů a
všech 55 řízených aktérů používá stejnou účetní bilanci.

Nasazení používá v soukromém `.env`:

```dotenv
LAB_AI_PROFILE=defias-v8
LAB_WILDLIFE_ENABLED=1
```

Po úspěšném CI proveď běžný build a offline `make setup-world`, potom
`make start`. Automatické nasazení rozpozná profil a provede jeho
`bootstrap-defias-v8`; `preflight-defias-v8` ověří přesné identity a
vlastnictví. Aktivace kontroluje celý předchozí profil i všech 15 nových
rezervací. `AIWorld.LivingDefias.Enabled` zapíná pouze tento profil;
obecné Defias autoformation/regroup/roam/hunt přepínače zůstávají na 0,
protože pohyb řídí konkrétní výprava. Pauza používá
`LAB_WILDLIFE_ENABLED=0`. Offline `deactivate-defias-v8` pozastaví všech
55 identit bez smazání jejich dlouhodobé paměti.

Nové zásoby, náklad, potřeby, zkušenosti z přepadů a skupiny se neukládají
přes restart. Skupiny nevkládají členství do databáze. Původní mapové,
polní a přepadové receipts zůstávají beze změny a platí pro své původní
profily a revize.

### Záznam a ověření skupinové výpravy

V `.env` nastav `AIWORLD_RECORD_INTERVAL=0.25`, spusť `make record` a
poznamenej adresář session z `make record-status`. Sleduj alespoň pět
minut souvislých čerstvých dat a celou výpravu od shromáždění přes cestu
ke skladu nebo střet se stráží až po návrat či úmrtí jednotlivých členů.
Poté ukonči záznam přes `make record-stop`, aby byly gzip soubory uzavřené.

```bash
mkdir -p runtime/lab/validation
python3 -m tools.realm_lab.defias_group_acceptance runtime/lab/recordings/aiworld-SESSION --minimum-seconds 300 --output runtime/lab/validation/defias-group-acceptance.json
```

Výsledky jsou `PASS`, `FAIL`, `INCONCLUSIVE` (exit 0, 3, 2). Gate kontroluje
55/45 aktérů, sdílenou bilanci a skutečné shromáždění, společnou cestu,
společný cíl a doložený konec každého člena ve stejném pokusu. Řádný ústup
nebo smrt mohou být platným výsledkem výpravy. `PASS` sám nepotvrzuje
úspěšnou krádež, doručení ani příčinné použití paměti. Tyto výsledky je
nutné doložit samostatně; staré testy jednoho bandity pro `fields-v8`
a `raids-v8` nenahrazují nový důkaz celé skupiny. Živý výsledek nového
profilu se zapíše až podle konkrétního záznamu a běžící revize.

## Pole a vlastní vesnické sklady — fields-v8

Profil zachovává všech 28 aktérů `raids-v8` a přidává 12 stávajících
dělníků horní, levé a pravé vesnice. Každá dostává svůj sklad: ID 4, 5
a 6. Jižní vesnice si ponechává sklad 1; sklady gnollů a Defias zůstávají
oddělené. Domovy, 100 spawnů a mapová data se nemění.

Pole a 36 pracovních bodů pocházejí ze skutečného pokrytí texturou
`elwynncrop92.blp`. [Manifest](data/realm_lab/aiworldlab/preview-v8/field-population.json)
a [navigační důkaz](data/realm_lab/aiworldlab/preview-v8/field-nav-probe.json)
obsahují přiřazení pracovníků, sklady a 132 směrovaných spojení. Tento
důkaz potvrzuje nativní navigační síť; skutečný průchod a činnosti se měří
samostatně. Každý pohyb nadále kontroluje aktuální kolize a nebezpečí. Příchod
ke vlastnímu skladu používá u zemědělců toleranci 1,9 yd, aby nezastavili
na hraně blízké budovy. Tento dosah odpovídá rozlišení navigační sítě;
běžné domovy a práce na poli zachovávají 3 yd. Domov shodný se skladem
používá stejný přesnější příchod jako sklad. Rozpoznání zachová i
normalizovanou výšku skutečného domova, aniž by se měnily uložené souřadnice.

Dělník pracuje na vlastním poli nejméně 15 sekund. Dokončená sklizeň
převede jednu dostupnou porci úrody do nákladu, nejvýše dvě na dělníka.
Pětisekundové odevzdání ji přesune do vlastního skladu. Jídlo trvá pět
sekund a hlad sníží až po skutečném odečtení porce z tohoto skladu.
Následuje návrat domů, odpočinek a další pracovní místo. Plný sklad
vede k údržbě pole; přerušená práce, údržba a samotný příchod jídlo netvoří.
Po jídle bez dokončené práce dělník pokračuje na rozpracovaném bodě.
Na další bod přejde po dokončené práci a návratu s odpočinkem doma.

Sklady začínají prázdné a mají kapacitu 30 porcí. Každé pole má nejvýše
24 zralých porcí; jedna dorůstá za 30 sekund. Úroda se započítá jako
potrava až dokončenou sklizní. Celková bilance zahrnuje původní maso i
novou úrodu. Zásoby, úroda, náklad a potřeby zůstávají pouze stavem běhu.

Pro nasazení nastav `LAB_AI_PROFILE=fields-v8` a `LAB_WILDLIFE_ENABLED=1`.
Použij běžné `make setup-world` a `make start`; připravenost a doplnění
identit vyžadují zastavený worldserver. Pro pauzu ponech profil a nastav
`LAB_WILDLIFE_ENABLED=0`. `deactivate-fields-v8` také umí při zastaveném
světě přepnout všech 40 rezervovaných identit do Observe bez mazání paměti.
Starší profil tyto rozšířené rezervace automaticky nepřebírá.

Fyzický test vyžaduje u všech 12 dělníků doloženou práci, náklad,
odevzdání, skutečné jídlo a návrat domů ve stejném životním cyklu:
ověřuje pracovní návrat i pozdější návrat po jídle. Dělník nemusí
jíst bezprostředně po odevzdání; sklad slouží společně jeho vesnici.

```bash
python3 -m tools.realm_lab.field_acceptance runtime/lab/recordings/aiworld-SESSION --output runtime/lab/validation/field-acceptance.json
```

Výsledky `PASS`, `FAIL` a `INCONCLUSIVE` oddělují dokončené řetězce,
porušení pravidel a chybějící důkaz. Zdravé služby samy pracovní cyklus
nepotvrzují. Tento test neopakuje úplný předchozí test přepadů.

### Defias a všechny vesnické sklady

V profilu `fields-v8` může Defias 901055 krást ze skladu jižní (1), horní
(4), levé (5) i pravé (6) vesnice. Hlad a prázdný tábor nadále spouštějí
výpravu. V aktivním profilu vybírá podle vlastních vzpomínek, délky cesty
a nebezpečí. Neznámé zásoby jsou důvodem k průzkumu; čerstvě zjištěný
sklad s méně než šesti porcemi odkládá. Sklady gnollů a samotných Defias
nejsou cíle.

Vybraný sklad se během výpravy, krádeže a zpáteční cesty nemění.
Defias musí ke skladu skutečně dojít, pět sekund pracovat a nejvýše dvě
porce odnést zpět do svého tábora. Každý sklad má vlastní stav zásob
i počet ukradených porcí; přesun jídlo nevytváří. Strážný 901039 nadále
chrání jižní sklad. Ostatní vesnice si ponechávají původní nativní stráže,
jejichž útok může výpravu také přerušit.
Observer ukazuje jméno cílového skladu a odcizené porce tohoto skladu.

Nové cesty používají vlastní průchozí body a stávající kontroly terénu,
kolizí a viditelnosti. Domovy a klientská mapa zůstávají stejné. Starší
profil `raids-v8` ponechává původní přepady pouze jižního skladu.
[Nativní navigační důkaz](data/realm_lab/aiworldlab/preview-v8/warehouse-raid-nav-probe.json)
ověřuje 28 směrovaných úseků nových cest; skutečné provedení přepadů
se ověřuje samostatně.

```bash
python3 -m tools.realm_lab.warehouse_raid_acceptance runtime/lab/recordings/aiworld-SESSION --output runtime/lab/validation/warehouse-raid-acceptance.json
```

Výchozí fyzický test vyžaduje propojenou krádež, přepravu a předání ze
všech čtyř skladů v jednom souvislém záznamu. Přepínač `--required-source`
lze opakovat pro užší test; výsledek výslovně uvádí nepozorované sklady.
Test kontroluje všech šest zásobáren a bilanci všech 40 řízených aktérů;
nenahrazuje samostatný důkaz obrany strážným nebo celý test zemědělců.

### Paměť při plánování přepadů

Přepady používají existující `ShortTermMemory` a `MemoryRetrieval`.
Zkušenost má konkrétního vlastníka, sklad, trasu, čas a skutečné místo
pozorování. Zásoby se zapisují až při fyzickém příchodu se vzájemnou
viditelností; vzdálený účetní stav skladu není informací bandity. Krádež
a doručení jsou dvě různé zkušenosti a úspěšné zásobení potvrzuje teprve
skutečné předání nákladu v táboře.

Setkání s viděným útočníkem označí jeho skutečné místo. Levá hlídka,
která přeruší cestu k pravému skladu, tak vytvoří vzpomínku na nebezpečnou
cestu. Pravý sklad tím není považován za prozkoumaný. Výběr zohledňuje
celou cestu, umí použít východní objížďku pravé vesnice a při nebezpečí
všech vhodných cest výpravu odloží. Každý skutečný pohyb dál prochází
kontrolou terénu, kolizí, dosahu a viditelnosti.
[Nativní důkaz objížďky](data/realm_lab/aiworldlab/preview-v8/raid-memory-nav-probe.json)
ověřuje jejích osm bodů a čtrnáct směrovaných spojení; skutečné projetí
a použití vzpomínek ověřuje samostatný živý test.

Informace o zásobách vydrží nejvýše pět minut, setkání s útočníkem
patnáct minut. Zkušenosti postupně ztrácejí váhu. Tyto záznamy se
nepovyšují do dlouhodobé paměti ani do databáze a restart je odstraní.
Mluvící NPC si ponechávají svůj dosavadní řadič; rozhodování přepadů
využívá stávající místní vyhledávání vzpomínek.

Observer u Defias ukazuje důvod volby nebo odkladu, trasu, počet vybavených
vzpomínek, poslední známé zásoby a stáří informace. Skutečné zásoby skladu
zůstávají samostatným údajem přehledu.

```bash
python3 -m tools.realm_lab.raid_memory_acceptance runtime/lab/recordings/aiworld-SESSION --output runtime/lab/validation/raid-memory-acceptance.json
```

Tento test spojuje skutečně pozorovaný útok s pozdějším vybavením vzpomínky
po návratu do tábora a s provedenou změnou cíle, trasy nebo odkladem. Vyžaduje
nejméně pět minut souvislého záznamu a zachovanou bilanci všech zásob.
Samotné jméno důvodu nebo počet vzpomínek nestačí. Výsledek nepotvrzuje
úspěšnou loupež; tu nadále ověřuje samostatný test přepadů.

## Přepady zásob — základ raids-v8

Profil přidává ke zvěři, lovcům a třem spotřebitelům Defiase 901055 a
strážného 901039. Přepad používá skutečné zásoby, náklad, fyzickou cestu
tam i zpět a pětisekundovou práci při krádeži i předání. Strážný může
viditelného útočníka zastavit a hráč může zasáhnout běžným bojem.
[První fyzický důkaz](data/realm_lab/aiworldlab/preview-v8/raid-first-chain-acceptance.json)
na revizi `44a83af87236` obsahuje dva úspěšné přepady a třetí pokus
zastavený strážným; pokračovalo původní zásobování i spotřeba.

Aktivní konfigurace je `LAB_AI_PROFILE=raids-v8` a
`LAB_WILDLIFE_ENABLED=1`. Pro pauzu ponech tento profil a nastav
`LAB_WILDLIFE_ENABLED=0`; standardní znovuvytvoření worldserveru pozastaví
všech 28 řízených aktérů. Potřeby, potrava, náklad a přepady se přes restart
zatím neukládají. Přijímací test uzavřeného záznamu:

```bash
python3 -m tools.realm_lab.raid_acceptance runtime/lab/recordings/aiworld-SESSION --output runtime/lab/validation/raid-acceptance.json
```

Výsledek je vázaný na konkrétní běžící verzi a uzavřené soubory; zdravé
služby samy nepotvrzují přepad. Pravidla a známé omezení zpáteční objížďky
strážného popisují [RealmLab](doc/RealmLab.md) a [roadmapa](AIWorld_Current_Roadmap.md).

## Navazování cílených přesunů

Dokončení vlastního přesunu ke skladu, na loviště, při přepadu, návratu
domů nebo hledání potravy může probudit další vyhodnocení na následujícím
world update. Pravidelná fáze potřeb a skutečně uplynulý čas zůstávají
zachované. Pokračování používá původní plánovací rozpočet a znovu kontroluje
kolize; nevzniká pohyb přímo v callbacku `MovementInform`.

Lovci a spotřebitelé také slučují přesně přímé body nativní cesty do
ověřovaných úseků do 14 yardů. Rohy, výškové zlomy a krátké povrchové
objížďky zůstávají zachované. Mezi úseky rozpracované cesty není vložená
sekundová pauza. Běžné procházky, práce, jídlo, odpočinek a čekání při
neúspěšném hledání si ponechávají vlastní pravidla.

Rozpracované hledání zásobovací cesty po vyčerpání plánovacího rozpočtu
pokračuje v požadovaném termínu, obvykle za 100 ms. Stejný termín se znovu
naplánuje i tehdy, když mu předchází pravidelná aktualizace potřeb.
Hledání tak nečeká na další celou sekundu. Pořadí čekajících, limity práce,
kontroly překážek a skutečně uplynulý čas potřeb zůstávají zachované;
hotové neúspěšné hledání si ponechává původní pomalejší opakování.

Přehled na portu 9091 obnovuje čerstvé souřadnice po 250 ms plus době
požadavku a vykreslení; chyby a zastaralá data mají sekundový interval.
Zobrazení nevymýšlí mezilehlé pozice a požadavky se nepřekrývají. Po
nasazení změny skriptu obnov otevřenou stránku, aby načetla novou verzi.

## Příprava zásobování lovem — předchozí profily

Navazující profil `consumers-v8` přidává ke třem lovcům dva pracovníky
jižní vesnice (901035/901036) a gnolla 901063 jako spotřebitele jejich
vlastních zásobáren. Po jeho aktivaci jde o 26 řízených a 74 nativních NPC;
všech 100 původních spawnů a jejich domovy zůstává zachováno. Postup a
ověřovací podmínky jsou v [doc/RealmLab.md](doc/RealmLab.md#spotřebitelé-zásob--consumers-v8).
[První fyzický důkaz](data/realm_lab/aiworldlab/preview-v8/consumer-first-chain-acceptance.json)
prošel na revizi `28ce11753874`: všichni tři spotřebitelé dokončili jídlo
a návrat, všichni tři lovci doručili potravu a bilance zůstala zachovaná.

Volitelný profil `supply-v8` přidává ke stávající zvěři lidského lovce
901034, dva gnollí lovce 901061/901062 a dvě zásobárny. Po aktivaci jde
o 23 řízených a 77 nativních NPC na stejné mapě. První fyzický řetězec
lov–náklad–doručení–spotřeba prošel na revizi `1937ad25ff05`.
[Ověření po úpravě plánovacího rozpočtu](data/realm_lab/aiworldlab/preview-v8/supply-budget-chain-acceptance.json)
prošlo také na revizi `ed629af322ef`: za 489,91 sekundy má 490 čerstvých
unikátních vzorků, šest doručení a pět jídel, s účastí všech tří lovců.
[První důkaz](data/realm_lab/aiworldlab/preview-v8/supply-first-chain-acceptance.json)
zůstává historickým podkladem. Každý důkaz platí pro uvedenou revizi;
další změny se ověřují samostatně.

Po sestavení verze se zásobováním a před první aktivací použij `make backup`.
V privátním `.env` nastav `LAB_AI_PROFILE=consumers-v8` a
`LAB_WILDLIFE_ENABLED=1`, potom spusť `make setup-world` a `make start`.
CI/CD používá stejnou přípravu pod deploy zámkem. Výchozí `.env.example`
zůstává u `settlement-v8`; současný realm už používá navazující `raids-v8`
popsaný výše. Tento postup je pro první aktivaci etapy `consumers-v8`.

Nové zásoby, náklad a rezervace kořisti jsou pouze stav aktuálního běhu;
při restartu se resetují a sklady začínají prázdné. Jejich ukládání je
odložené podle zadání. Z identity ani starých osobních zásob se nové jídlo
nevytváří. Ze skladů jedí tři lovci a tři vybraní spotřebitelé;
spotřeba celé vesnice zatím zapnutá není.

V samotném profilu `consumers-v8` při pauze ponech tento profil,
nastav `LAB_WILDLIFE_ENABLED=0` a
znovu vytvoř worldserver. Přepínač pozastaví všech 26 řízených aktérů.
Po založení nových vazeb se prosté přepnutí na starší profil
odmítne; jejich identity a paměť se při pauze nemažou. Pravidla, limity,
navigační důkaz a fyzická kritéria jsou v
[doc/RealmLab.md](doc/RealmLab.md#zásobování-lovem--supply-v8).
