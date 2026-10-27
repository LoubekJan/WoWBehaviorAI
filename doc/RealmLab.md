# Vlastní svět jako jediný realm

Profil `fields-v8` přidává nad nezměněné přepady tři pole a vlastní
sklady horní, levé a pravé vesnice. Řídí 40 ze 100 původních NPC;
12 nových pracovníků má přesná přiřazení v
[manifestu polí](../data/realm_lab/aiworldlab/preview-v8/field-population.json).
Sklady 4/5/6 začínají prázdné, přijímají sklizeň pouze svých pracovníků
a poskytují jim skutečně odečítaná jídla. Jižní sklad 1 i původní
sklady 2/3 zůstávají zachované. Domovy a geometrie se nemění.
Podrobný cyklus a spuštění popisuje [README_DEV](../README_DEV.md#pole-a-vlastní-vesnické-sklady--fields-v8).
Nativní důkaz 51 bodů a 132 spojení je oddělený od fyzického testu
sklizně, doručení, jídla a návratu.

Nové rozšíření k 27. říjnu 2026: profil `raids-v8` navazuje na
`consumers-v8` a řídí 28 ze stávajících 100 NPC. Přidává pouze Defiase
901055 a strážného 901039; ostatních 72 NPC zůstává nativních. Nasazení
a fyzický výsledek se ověřují samostatně po úspěšném CI.

Hladový Defias při prázdném táboře a alespoň šesti porcích v jižním skladu
vyrazí po vymezených bodech ke skladu. Po pěti sekundách vlastní práce
na místě převede nejvýše dvě porce ze skladu do nákladu. Po skutečném
návratu a další pětisekundové práci převede náklad do zásobárny Defias;
tam jej může sníst. Strážný střídá čtyřminutovou službu doma a u skladu.
Při službě, cestě pro jídlo i návratu prochází bodem u původního domova
pracovníka 901035. Přímá úhlopříčka od domova strážného vedla podél budovy
a v živém testu jej blokovala; vlastní domov strážného se neposouvá.
Viditelného útočníka může napadnout; hráč může zasáhnout běžným bojem.
Přerušená krádež nepřidá potravu, smrt ztrácí nesený náklad a všechny
převody zůstávají v původní bilanci zásob. Stav přepadů, potřeby i zásoby
se podle současného rozhodnutí přes restart neukládají.

`AIWorld.LivingRaidsEnabled` je standardně vypnuté a zapíná jej pouze
profil `raids-v8` spolu se zásobováním a spotřebiteli. Domovy, typy NPC,
geometrie a původní lovecké vazby se nemění. Delší dosah 900 yd má pouze
konkrétní útočník. Každý pohyb nadále prochází skutečnou navigací a
kontrolami kolizí. Přijímací nástroj je
`python -m tools.realm_lab.raid_acceptance <recording-directory>`;
vyžaduje i původní lovecké a spotřebitelské řetězce ve stejném záznamu.

První [fyzický důkaz přepadů](../data/realm_lab/aiworldlab/preview-v8/raid-first-chain-acceptance.json)
prošel na revizi `44a83af87236`: uzavřený záznam 82 minut a 21 sekund
obsahuje dvě úplné krádeže s doručením a třetí pokus zastavený skutečným
útokem strážného před krádeží. Pokračovali i všichni tři lovci a původní
tři spotřebitelé. Bilance zůstala zachovaná a nezávislé přehrání se přesně
shoduje. Důkaz uvádí hashe, identitu běžící binárky i omezení měření;
neprokazuje dlouhodobou rovnováhu ani perzistenci. Při návratu strážného
zbývá zkrátit pozorované 50,75sekundové hledání bezpečné objížďky.

Verzovaný provozní model k **27. říjnu 2026**. Aktivní větev zůstává
`AI-World-lab`; hlavním vstupem jsou root `compose.yml`, `.env` a
`Makefile`. Původní Elwynn realm 1 je vyřazený. Přihlášení a
existující účty zajišťuje samostatná auth služba tohoto stacku.
Výsledek konkrétního nasazení potvrzují jeho privátní migrační receipts,
healthchecks a čerstvá telemetrie.

## Zachované identity a svět

| Část | Canonical hodnota |
| --- | --- |
| Realm | AI World Lab, ID 2 |
| Mapa / oblast | 725 / 4988 |
| Aktivní profil | `fields-v8` |
| Nasazovací checkout | `/home/voslik/WoWBehaviorAI-lab` |
| Compose projekt / síť | `aitc-lab` / `aitc-lab_default` |
| Auth / svět / viewer / registrace | 3724 / 9086 / 9091 / 9088 |
| Auth databáze | `auth-mysql`, volume `aitc_auth_mysql-data` |
| Svět a postavy | `lab-mysql`, volume `aitc_lab_mysql-data` |
| Runtime build / cache | `aitc_lab_build-data` / `aitc_lab_ccache-data` |
| CI build / cache | `aitc_lab_ci_build-data` / `aitc_lab_ci_ccache-data` |
| Mapová data / logy / záznamy | `runtime/lab/data`, `logs`, `recordings` |
| Privátní konfigurace | root `.env` |

Mapa preview-v8 má čtyři ADT dlaždice a rozměr přibližně 1067 × 1067
herních yardů. [Serverový manifest](../data/realm_lab/aiworldlab/preview-v8/server-manifest.json)
fixuje 61 nativních souborů a potřebnou kolizní geometrii. Datové svazky
postav a mapy se během sjednocení nemění.

[Verzovaná populace](../data/realm_lab/aiworldlab/preview-v8/settlement-population.json)
obsahuje GUID 901000–901099:

| Frakce / skupina | NPC |
| --- | ---: |
| Stormwind | 44 |
| Defias Brotherhood | 16 |
| Gnollové | 6 |
| Murloci | 6 |
| Koboldi | 8 |
| Divoká zvěř | 20 |

Polohy odpovídají autorským táborům, městu, vesnicím a dolu.
[Příslušné důkazy](../data/realm_lab/aiworldlab/preview-v8/settlement-position-verification.json)
jsou navázané na mapové hashe. Historická stovka regresních aktérů se
v existující instalaci ponechává maskovaná a bez řízení; čistá instalace
ji nemusí obsahovat. Částečný nebo změněný historický stav se odmítne.

## Přihlášení, start a smrt postavy

Účty zůstávají stejné. Migruje se celé `auth`, včetně autentizačních
údajů, banů a oprávnění; systémové MySQL účty se vytvoří odděleně.
Authserver vlastní schema owner, worldserver má pouze DML přístup do
`auth.*`. Světové i character migrace worldserveru používají masku 6,
tedy bez auth DDL.

Starý realm 1 se odstraní ze seznamu a jeho provozní reference se
vyřadí po ověření záloh. Globální oprávnění `RealmID=-1`, oprávnění pro
realm 2, herní účty a stávající postavy realmu 2 zůstávají zachované.
Čistý bootstrap umí odebrat pouze nepoužívaný výchozí placeholder
`Trinity`, nikoli libovolný cizí realm.

[Startovní bod](../data/realm_lab/aiworldlab/world-start.json) nastavuje
nové postavy na `725, 266.667, 800, 2.01539`; stávající uložené polohy
ani homebindy se plošně nepřepisují.
[Hřbitov](../data/realm_lab/aiworldlab/graveyard.json) používá safe-location
ID 9000 pro mapu 725 / oblast 4988. Jeho serverová DBC, odkaz v
`graveyard_zone` a klientský `patch-X.MPQ` se připravují společně.
Fyzický průchod vytvořením postavy, smrtí a návratem ke spirit healerovi
je samostatný herní acceptance test.

Account-web vytváří účty přes interní SOAP `http://worldserver:7878`.
Worldserver běží bez interaktivní konzole a bez virtuálního terminálu;
správa příkazů používá interní SOAP, logy zůstávají dostupné přes Docker.
`TC_SOAP_USER` musí mít administrátorské oprávnění na realm 2 nebo `-1`.
Registrace nastavuje rozšíření Wrath of the Lich King (2). SOAP se na
hostu nezveřejňuje; samotný registrační web je na portu 9088.

## Nasazování a migrace

`Realm Lab CI/CD` zůstává názvem jediného aktivního workflow, aby
navazoval na dosavadní běhy. Push do `AI-World-lab` spustí Python/MySQL
kontrakty, Observer/AI/account-web testy, C++ build/CTest a kontrolu
Compose modelů. PR běží bez přístupu do nasazovacího hostu. CI volumes
jsou oddělené od runtime.

Deploy používá stávající label `realm-lab-deploy`, proměnnou
`REALM_LAB_DEPLOY_ENABLED` a přesný commit, který prošel kontrolami:

```bash
python3 tools/realm_lab/promotion.py prepare
bash docker/scripts/deploy-lab.sh <otestovany-commit>
python3 tools/realm_lab/promotion.py finalize
```

Příprava zachytí chráněné zálohy původního auth/world/characters i
současného světa a převede účty do nového auth volume. Finalizace
probíhá až po ověření nového stacku a vyřadí původní aplikační kontejnery
a nasazování Elwynnu. Původní `.github/workflows/ci.yml` je pouze ruční
informační workflow; nic nenasazuje. Starý deploy runner se vyřadí,
sdílený CI runner a nový deploy runner zůstávají.

Běžné nasazování pod zámkem zastaví vlastní worldserver i authserver
před výměnou binárek. Ověří data, aktualizuje schémata, startovní politiku,
hřbitov a populaci, spustí celý stack a požaduje dvě čerstvé úplné dávky
telemetrie všech 100 NPC. Příprava čisté instalace je explicitní
`make setup-world` po importu TDB a dodání dat. Kompletní postup a
příkazy jsou v [README_DEV.md](../README_DEV.md).

`compose.lab.yml` zůstává kompatibilním vstupem přes `include`.
CI override se používá přímo s `compose.yml`, protože importované
Compose resources nelze přepisovat dalším souborem vedle wrapperu.
`compose.auth-share.yml` a externí auth síť už canonical stack nepoužívá.

## Zvěř, potrava a lov — wildlife-v8

Profil `wildlife-v8` přidává řízení ke stávajícím spawnům 901072–901091:
16 jelenů má roli kořisti, medvědi 901076, 901081, 901086 a 901091 jsou
predátoři. [Samostatný manifest](../data/realm_lab/aiworldlab/preview-v8/wildlife-population.json)
váže tato přiřazení na původní ověřené polohy a geometrii. Ostatních
80 obyvatel, frakční tábory i maskovaná regresní populace se nemění.

Zvířata využívají deterministické `LivingRole` chování: okolní pohyb,
růst hladu, pastvu kořisti, hledání kořisti predátorem, pronásledování,
krmení z mrtvoly a návrat. Vyhledávání potravy rozšiřuje dosah postupně
na 80–128 yardů a každý pohyb stále prochází kontrolou terénu a trasy.
Přímá změna domova, teleportace a klientské mapové změny nejsou součástí
této aktivace. Remote inference, recovery advice, automatické skupiny,
spawn reconciliation a automatická aktivace řízení zůstávají vypnuté.

Profil se vybírá v privátním `.env`:

```dotenv
LAB_AI_PROFILE=wildlife-v8
LAB_WILDLIFE_ENABLED=1
```

První příprava probíhá při zastaveném světě pod deploy zámkem. Ověří
všech 100 spawnů, šablony, geometrii a staré regresní identity. Vloží
jen chybějící AI vazby dvaceti zvířat a jejich účast v simulaci; aktivace
mění pouze jejich `control_mode`. Opakované nasazení nepřepisuje domovy,
postavy, ekonomický stav ani dlouhodobou paměť.

`LAB_WILDLIFE_ENABLED=0` pozastaví runtime AI tohoto profilu. Po změně
`.env` je potřeba kontejner znovu vytvořit, například:

```bash
docker compose --env-file .env -f compose.yml up -d --no-deps --force-recreate worldserver
```

Tato pauza ponechává identity a dlouhodobou paměť. Individuální hlad,
únava a průběh aktuální akce zatím nemají perzistenci přes restart;
jejich uložení a obnovení je odloženo, dokud nebude fungovat chování světa.
Návrat pouhým přepnutím na `settlement-v8` se po aktivaci odmítne,
protože původní profil správně nedovoluje nové AI vazby. Pro pauzu se
používá přepínač výše.

Viewer stále zobrazuje všech 100 NPC v jedné dávce. U dvaceti řízených
zvířat doplňuje potřeby a role, zbývajících 80 má nativní pozorování.
`runtime_guid` rozlišuje načtené NPC včetně mrtvoly; fallback jej nemá.
`living_role.lifetime_ms` rozlišuje aktuální kontext řízené role.
Starší záznamy bez těchto polí zůstávají čitelné, ale samy nepotvrzují
souvislý fyzický lovecký cyklus.

Po nasazení musí projít dvě čerstvé dávky s přesnou populací 20 + 80.
Fyzické chování má oddělené vyhodnocení:

```bash
python3 tools/realm_lab/wildlife_acceptance.py runtime/lab/recordings/aiworld-SESSION --output runtime/lab/validation/wildlife-acceptance.json
```

Pozitivní důkaz vyžaduje pohyb dvaceti zvířat, pozorovanou pastvu
s poklesem hladu a alespoň jeden souvislý cyklus predátora: lov konkrétní
živé kořisti → pozorované úmrtí téže kořisti → krmení → pokles hladu
→ návrat z více než 14 yardů do blízkosti domova. Respawn, změna identity,
stará telemetrie nebo mezera v záznamu cyklus přeruší. Samotný čítač
`FED`, zdravá služba ani přítomnost potřeby nestačí. Záznam dokládá
úmrtí sledované kořisti, nikoli autora poslední rány.

První provozní ověření **26. října 2026 prošlo (PASS)** na revizi
`3de22f87d6617313d1a1fc78b3bce61ad4dfd670`. Oficiální záznam dlouhý
438,819 sekundy má 439 čerstvých vzorků: všech 20 zvířat se pohnulo,
všech 16 jelenů se páslo s poklesem hladu a všichni čtyři medvědi
dokončili souvislý cyklus lov–krmení–návrat. Aktivní populace je 20 řízených
zvířat + 80 nativních NPC. [Versionovaný důkaz](../data/realm_lab/aiworldlab/preview-v8/wildlife-first-cycle-acceptance.json)
obsahuje jednotlivé cykly, kontrolní součty záznamu a 233 pozorování
ze spawn/last-known fallbacku, která evaluator vyloučil z živého fyzického
důkazu. Jde o první krátký lovecký gate; dlouhodobý běh a patnáctiminutový
gate nehybnosti zůstávají neověřené. Perzistence potřeb přes restart je
odložena až po zprovoznění chování světa.
Značka `FORAGE` u medvěda 901086 nemá potvrzený souvislý pohyb.
Obecný navigační analyzátor skončil pro zkrácený záznam `INCONCLUSIVE`;
výše uvedené `PASS` patří samostatnému wildlife evaluatoru.

## Zásobování lovem — supply-v8

**Profil je nasazený; první fyzický řetězec zásobování prošel (PASS)**
26. října 2026 na revizi `1937ad25ff05b588ba6c25d7f3fa5ea6f6f50e81`.
Ověření po úpravě plánovacího rozpočtu prošlo také na revizi
`ed629af322ef69f950f56c86a40d4a916865526f`.
Opt-in profil `supply-v8` přidává ke stejným 20 zvířatům tři lovce:
lidského 901034 z jižní vesnice a gnollí 901061/901062 z jejich tábora.
Celkem řídí 23 NPC a 77 zůstává nativních. Jde
o schopnost konkrétních spawnů, nikoliv přetypování všech NPC sdílejících
stejnou šablonu. [Manifest](../data/realm_lab/aiworldlab/preview-v8/supply-population.json)
připíná původní identitu, domov, geometrii a dvě sady sklad–loviště.

Pravidla první verze:

- Jedna mrtvola jelena poskytne nejvýše **6 porcí**. Porce jsou společně
  evidované pro dané zvíře a jeho aktuální život; opakovaný odběr stejné
  kořisti ani soupeřící rezervace je nemohou zdvojit.
- Medvěd při dokončeném krmení spotřebuje nejvýše **3 dostupné porce**
  na místě. Lovec odnese nejvýše **6 porcí** jako vlastní náklad.
- Zásobárny jižní vesnice a gnollího tábora mají každá kapacitu **30 porcí**,
  cíl doplňování **12** a hranici nízkých zásob **4**. Obě začínají na **nule**.
- Jídlo do skladu přibude až po fyzickém příchodu naloženého lovce a
  dokončení doručení. Spotřeba ze skladu sníží jeho zásobu; hlad lovce
  klesne až po dokončeném jídle. Zatím ze skladů jedí jen tři lovci.
  Ostatní obyvatelé nemají v této verzi zapnutou společnou spotřebu.

Lovec má oddělený původní domov, sklad a loviště. Lidská trasa vede
z jižní vesnice do `wildSouth`, gnollí z tábora do `wildLeft`.
[Navigační důkaz](../data/realm_lab/aiworldlab/preview-v8/supply-nav-probe.json)
ověřil všech sedm bodů a 12 směrovaných tras v nativním 64bitovém Detouru
s filtrem pouze `NAV_GROUND`; původních 61 geometrických souborů se shoduje.
Malá projekce bodu i úplná navmesh cesta jsou přípravný důkaz. Nepotvrzují
NPC skutečně procházející trasou, lov, průchod aktuální překážkou ani doručení.

Po sestavení verze podporující zásobování nejprve vytvoř zálohu:

```bash
make backup
```

V privátním `.env` zvol:

```dotenv
LAB_AI_PROFILE=supply-v8
LAB_WILDLIFE_ENABLED=1
```

Přípravu a spuštění proveď přes stávající pipeline:

```bash
make setup-world
make start
```

`setup-world` zastaví svět pod deploy zámkem a ověří původních 100 spawnů,
šablony, geometrii, 20 zvířecích vazeb a maskovanou regresní populaci.
Přidá jen chybějící vazby tří lovců a aktivuje přesně vybraných 23.
Původní domovy, postavy, účty, osobní ekonomický stav a dlouhodobá paměť
se nepřepisují. CI/CD pro vybraný profil provádí stejnou přípravu.

`LAB_WILDLIFE_ENABLED=0` je společná pauza pro všech 23 aktérů tohoto
profilu. Po změně `.env` znovu vytvoř worldserver:

```bash
docker compose --env-file .env -f compose.yml up -d --no-deps --force-recreate worldserver
```

Profil při pauze ponech `supply-v8`. Po založení loveckých vazeb už
`wildlife-v8` ani `settlement-v8` tuto novou populaci nepřijmou; identity
a paměť není potřeba kvůli pauze mazat. Nové množství potravy, rezervace,
náklad, zásoby a jejich čítače existují pouze v aktuálním běhu serveru.
Restart je resetuje a zásobárny znovu začnou prázdné. Nová perzistence
zůstává odložená, dokud nebude fungovat chování světa.

Viewer má ukazovat roli, fázi zásobování, množství neseného masa, přiřazený
sklad, zásoby a společnou bilanci. Provozní přijetí vyžaduje čerstvý záznam
celého řetězce: konkrétní živá kořist → její úmrtí → odběr konečného množství
masa → naložený lovec na cestě → dokončené doručení a růst zásoby → jídlo,
pokles zásoby a hladu. Zásoby, náklad, spotřeba a ztráty musí zachovat
celkovou bilanci; staré čítače, spawn fallback ani zdravý kontejner tento
fyzický důkaz nenahrazují.

První uzavřený [provozní záznam a jeho ověření](../data/realm_lab/aiworldlab/preview-v8/supply-first-chain-acceptance.json)
mají délku 496,196 sekundy. Všichni tři lovci dokončili celý řetězec;
evaluator doložil šest dokončených doručení a šest jídel s úbytkem zásoby
i hladu. Poslední platná celková bilance je 57 získaných porcí, 30 ve
skladech, nula v nákladu, 27 spotřebovaných a nula ztracených, při nulové
počáteční zásobě. Nezávislé přehrání staženého gzipu odpovídá uloženému
výsledku přesně; důkaz připíná tři zdrojové soubory a zvlášť hash offline
evaluatoru.

Aktivační read-back ověřil přesně 23 řízených a 77 nativních NPC,
61 původních geometrických souborů a zachování čtyř účtů. Kontrolní
součty uložených postav a jejich homebindů se shodují; důkaz obsahuje
filtrovaný výsledek a hash chráněného aktivačního protokolu.

Z 497 vzorků záznamu je 477 čerstvých a unikátních. Čtyři identická
opakování a 16 závěrečných zastaralých vzorků se do fyzického důkazu
nezapočítávají; 1840 fallback pozorování jednotlivých řízených aktérů
a dvě pozorování bez časové identity role také nedokazují živou akci.
`PASS` vychází z dřívějšího souvislého platného úseku. Nepotvrzuje trvalou
čerstvost telemetrie, dlouhodobou rovnováhu ani patnáctiminutové pozorování
nehybnosti. Obecný navigační analyzátor zůstal `INCONCLUSIVE`.

Tento první důkaz patří uvedené revizi `1937ad25ff05b588ba6c25d7f3fa5ea6f6f50e81`.
[Navazující záznam po úpravě plánovacího rozpočtu](../data/realm_lab/aiworldlab/preview-v8/supply-budget-chain-acceptance.json)
na revizi `ed629af322ef69f950f56c86a40d4a916865526f` prošel za 489,91 sekundy.
Všech 490 zaznamenaných vzorků je čerstvých a unikátních, bez zastaralé
dávky nebo opakovaného capture. Všichni tři lovci dokončili doručení
i jídlo; celkem je doloženo šest doručení a pět jídel. Poslední platná
bilance je 51 získaných porcí, 31 ve skladech, nula v nákladu,
20 spotřebovaných a nula ztracených. Také tento gzip byl nezávisle
přehrán stejnými bajty evaluatoru, které jsou součástí testované revize.
1431 fallback pozorování aktérů a tři pozorování bez časové identity
role se nepoužívají jako důkaz živých akcí.

Oba důkazy jsou připnuté k uvedeným revizím; další změny se ověřují
samostatně. Jde o krátké provozní řetězce, nikoliv dlouhodobou rovnováhu
nebo patnáctiminutové pozorování nehybnosti. Perzistence přes restart
zůstává odložená podle zadání.

## Spotřebitelé zásob — consumers-v8

**Nasazeno; fyzický gate PASS** na revizi
`28ce117538745eafaeb70378e3090cffddb326ae`.
[Versionovaný důkaz](../data/realm_lab/aiworldlab/preview-v8/consumer-first-chain-acceptance.json)
připíná záznam 794,033 sekundy, CI, zdrojové hashe a nezávislé přehrání.
Všech 3 177 přijatých snímků je čerstvých; 11 duplicitních časů zachycení
se do důkazu nepočítá. Ze 3 166 unikátních snímků plyne sedm doložených
jídel a šest úplných cyklů spotřebitelů: 901035 jednou, 901036 dvakrát
a 901063 třikrát. Všichni tři lovci dokončili doručení, celkem šest
úplných loveckých cyklů. Bilance je `0 + 69 = 20 + 0 + 49 + 0` porcí.

Záznam zachytil prázdné sklady bez snížení hladu; souběh o poslední porci
je doložený jednotkovými testy. Jeden začátek jídla nemá celé pozorované
časové okno a je z doložených jídel vyřazený. Jde o pozitivní krátký test,
nikoliv dlouhodobou rovnováhu, perzistenci nebo patnáctiminutový gate
nehybnosti. Nezávislé přehrání uzavřeného gzipu přesně odpovídá reportu.

Profil `consumers-v8` navazuje na stejné zásobování a mapu `preview-v8`.
Přidává pouze tři existující NPC:

| Spawn | Původní role | Vlastní zásobárna |
|---|---|---|
| 901035, 901036 | Pracovníci jižní vesnice | Jižní vesnice, sklad 1 |
| 901063 | Gnoll, původní bojová role | Gnollí tábor, sklad 2 |

Jde o schopnost spotřebitele; jejich klasifikace, frakce, domovy a běžné
chování se nemění. Nezískávají lov ani převoz. Při hladu dojdou ke svému
skladu, dokončí vlastní pětisekundové jídlo a atomicky odeberou jednu
porci. Hlad klesne pouze po úspěšném odběru. Při prázdném skladu čekají
a zkusí to později; dvě NPC nemohou spotřebovat tutéž poslední porci.
Přerušená animace, cizí akce ani jídlo mimo dosah a dohled skladu
nemají účinek. Po jídle se vrátí domů a pokračují v běžné činnosti.
U této skupiny běžná práce nevytváří osobní potraviny a běžná animace
jídla neobchází společnou bilanci.

Zásobovací pohyb používá vlastní průběžně uchovávané hledání koridoru.
Po odmítnutí fyzické kolize nepovažuje navmesh spojení za průchod:
zkusí další koncový bod nebo omezenou objížďku ověřenou proti terénu,
kolizím, hranicím mapy a nebezpečí. Každý spuštěný krok znovu ověřuje
server. Čekání na výpočetní rozpočet zachová kandidáta; vyčerpané hledání
se znovu prověří po 10 sekundách. Domov NPC tím nevzniká ani se nemění.
Telemetrie u zásobovacích aktérů exportuje také přesný důvod odmítnutí
a navigační diagnostiku.
Profil se zapnutými spotřebiteli odesílá telemetrii každých 250 ms,
aby záznam mohl zachytit i krátké dokončené pohybové kroky. Ostatní
profily zachovávají sekundový interval. Přijímač výslovně ověřuje také
fázi plánování `SUPPLY_CORRIDOR`.

Před první aktivací vytvoř `make backup`. Po úspěšném buildu nastav
v privátním `.env` a nasaď:

```dotenv
LAB_AI_PROFILE=consumers-v8
LAB_WILDLIFE_ENABLED=1
```

Offline bootstrap ověří původní populaci a rozšíří existujících 23 vazeb
o tyto tři: výsledkem je **26 řízených + 74 nativních = 100 NPC**.
Nový přepínač `AIWorld.LivingConsumersEnabled` se zapne jen pro tento
profil. Pro pauzu ponech profil `consumers-v8` a nastav
`LAB_WILDLIFE_ENABLED=0`; návrat na starší profil po založení vazeb
neprojde jeho přísnou kontrolou vlastnictví. Potřeby, zásobárny a nové
čítače zůstávají pouze v paměti a při restartu se resetují podle zadání.

Viewer rozlišuje schopnost `CONSUMER` od původní role NPC a ukazuje
sklad, fázi cesty a skutečnou spotřebu. Fyzické přijetí přes
`tools/realm_lab/consumer_acceptance.py` vyžaduje souvislý záznam
příchodu, dokončeného jídla s úbytkem zásob a hladu a návratu všech tří
spotřebitelů. Současně ověřuje původní lovecký řetězec a zachování bilance.
Výsledky jednotkových testů nepředstavují samy o sobě runtime důkaz.

## Telemetrie a navazující vývoj

Viewer na portu 9091 ukazuje vybraných 100 NPC: živou
pozici, zdraví a bojový stav. V nativním `settlement-v8` je AI vypnutá;
`wildlife-v8` přidává výše popsanou řízenou zvěř; `supply-v8`
navazuje třemi lovci a zásobárnami s prvním fyzickým důkazem. Aktivní
`consumers-v8` přidává tři spotřebitele se samostatným fyzickým důkazem
jídel a návratů všech tří.
Automatické skupiny,
přepady vesnic a dobývání teritoria zatím nejsou zapnuté.
Nové chování se má přidávat po ověřitelných krocích nad touto vlastní
mapou. Zdravý kontejner ani úplná telemetrie samy nepotvrzují fyzickou
průchodnost každé trasy.

[Historický popis odděleného labu, map a testů do 11. října 2026](archive/RealmLab-2026-10-11.md)
uchovává původní postup a důkazy. Sdílené faction CSV v `data/elwynn/factions`
a historie SQL migrací zůstávají potřebné i pro současné frakce vlastní mapy.
