# Vlastní svět jako jediný realm

Verzovaný provozní model k **26. říjnu 2026**. Aktivní větev zůstává
`AI-World-lab`; hlavním vstupem jsou root `compose.yml`, `.env` a
`Makefile`. Původní Elwynn realm 1 je určen k vyřazení. Přihlášení a
existující účty se převedou do samostatné auth služby tohoto stacku.
Výsledek konkrétního nasazení potvrzují jeho privátní migrační receipts,
healthchecks a čerstvá telemetrie.

## Zachované identity a svět

| Část | Canonical hodnota |
| --- | --- |
| Realm | AI World Lab, ID 2 |
| Mapa / oblast | 725 / 4988 |
| Profil | `settlement-v8` |
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
jejich uložení a obnovení je další samostatný krok. Návrat pouhým
přepnutím na `settlement-v8` se po aktivaci odmítne, protože původní
profil správně nedovoluje nové AI vazby. Pro pauzu se používá přepínač
výše.

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
důkazu. Jde o první krátký lovecký gate; dlouhodobý běh, patnáctiminutový
gate nehybnosti a perzistence potřeb přes restart zůstávají neověřené.
Značka `FORAGE` u medvěda 901086 nemá potvrzený souvislý pohyb.
Obecný navigační analyzátor skončil pro zkrácený záznam `INCONCLUSIVE`;
výše uvedené `PASS` patří samostatnému wildlife evaluatoru.

## Telemetrie a navazující vývoj

Viewer na portu 9091 ukazuje vybraných 100 NPC: živou
pozici, zdraví a bojový stav. V nativním `settlement-v8` je AI vypnutá;
`wildlife-v8` přidává výše popsanou řízenou zvěř. Automatické skupiny,
přepady vesnic a dobývání teritoria zatím nejsou zapnuté.
Nové chování se má přidávat po ověřitelných krocích nad touto vlastní
mapou. Zdravý kontejner ani úplná telemetrie samy nepotvrzují fyzickou
průchodnost každé trasy.

[Historický popis odděleného labu, map a testů do 11. října 2026](archive/RealmLab-2026-10-11.md)
uchovává původní postup a důkazy. Sdílené faction CSV v `data/elwynn/factions`
a historie SQL migrací zůstávají potřebné i pro současné frakce vlastní mapy.
