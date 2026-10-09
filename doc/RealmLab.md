# Samostatný realm pro vlastní testovací mapu

Stav přípravy: 9. října 2026. Větev `AI-World-lab` vychází z
`ai-world` na revizi `c08fec3f2a`. Připravené soubory oddělují provoz a
CI/CD; vlastní rovná mapa, klientský patch a shodná serverová data jsou
nasazené. Samostatná kontrola navmesh prošla; GPS, fyzický pohyb a přenos
AI mimo Elwynn ještě čekají na ověření.

## Rozdělení společných a samostatných částí

| Část | Rozhodnutí | Důvod / důsledek |
|---|---|---|
| Přihlášení, `authserver`, databáze `auth` | Společné, spravované původním stackem | Stejné účty a společný seznam realmů. Lab nespouští druhý authserver. |
| Záznam `auth.realmlist` | Samostatný, navržené ID 2 | Původní ID 1 se nemění. Registrace odmítá cizí ID/název. |
| Postavy a AI stav | Nová `characters` ve vlastní MySQL instanci | Postavy, potřeby, paměti, skupiny a dynamické questy se nekopírují z Elwynnu. |
| Svět | Nová `world` ve vlastní MySQL instanci | Spawny, domovy, lokace a klasifikace se později definují pro vlastní mapu. |
| MySQL proces a datový volume | Samostatný pro lab | Reset či migrace labu nezasáhne původní `world`/`characters`. Názvy DB mohou zůstat stejné díky jiné instanci. |
| Worldserver, knihovny a build volume | Samostatné | Každý realm může běžet na jiné revizi. Binárky se nesdílejí mezi běžícími realmy. |
| TrinityCore/AIWorld zdrojový kód | Společný původ, samostatná větev | Opravy lze přenášet cíleným cherry-pickem. Upstream merge nesmí přepsat lab provozní soubory. |
| Schémata a generátory dat | Znovupoužitelné | Verze migrací se uplatňují odděleně pro každou DB. Auth migrace vlastní pouze původní stack. |
| Originální klientské modely a textury | Znovupoužitelné | Pro první mapu není potřeba vytvářet nové modely. |
| DBC, `maps`, `vmaps`, `mmaps` | Vlastní výstupní adresář, mount pouze pro čtení | Lze vycházet z kopie stejného klientského základu, ale upravené výstupy se nesmí zapisovat do dat Elwynnu. |
| Klientský patch / klient pro mapování | Vlastní | Doporučená samostatná kopie klienta pro lab; stejné přihlášení a účty. |
| AI HTTP služba | Samostatná instance ze stejného kódu | Změny protokolu a restart labu neovlivní Elwynn. |
| LLM server, modelové váhy, GPU | Mohou být společné | Sdílení výkonu může zkreslit časování; první pohybové testy mají modelové požadavky vypnuté. |
| Observer, token a recorder | Samostatné instance/výstupy | Snapshoty a záznamy obou světů se nemíchají. Současná mapa Observeru ještě vyžaduje zobecnění. |
| Account web | Společný, bez další lab instance | Vytváří společné účty. GM oprávnění na konkrétní realm se nastavují zvlášť. |
| Docker host a CI runner | Mohou být společné | CPU/RAM/I/O jsou sdílené; oba buildy mohou soupeřit o výkon. CI volumes jsou oddělené. |
| Deploy runner a pracovní checkout | Samostatné | Lab runner má vlastní label a vidí pouze lab checkout. Docker socket přesto poskytuje přístup k celému Docker hostu. |

Sdílené účty znamenají také společná globální nastavení účtů, například
bany a některé administrátorské operace. Oddělený realm není úplná
bezpečnostní izolace od společného auth. Lab uživatel má jen DML práva na
`auth.*`, žádné DDL ani přístup k původním `world`/`characters`. SQL práva
neomezují DML na jednotlivé řádky podle RealmID. GM účet s oprávněním pro
realm `-1` má globální oprávnění i na novém realmu.

## Provozní rozložení

| Parametr | Elwynn | Lab |
|---|---|---|
| Větev | `ai-world` | `AI-World-lab` |
| Deploy checkout | `/home/voslik/WoWBehaviorAI` | `/home/voslik/WoWBehaviorAI-lab` |
| Compose projekt | Stávající | `aitc-lab` |
| Přihlašovací port | 3724 | Společný 3724 |
| World port na hostu | 8085 | 9086, uvnitř kontejneru 8085 |
| Observer port | 8090 | 9091 |
| RealmID | 1 | 2, po ověření dostupnosti |
| MySQL volume | `aitc_mysql-data` | `aitc_lab_mysql-data` |
| Runtime build/cache | `aitc_build-data` / `aitc_ccache-data` | `aitc_lab_build-data` / `aitc_lab_ccache-data` |
| CI build/cache | `aitc_ci_build-data` / `aitc_ci_ccache-data` | `aitc_lab_ci_build-data` / `aitc_lab_ci_ccache-data` |
| Runtime soubory | `runtime/` | `runtime/lab/` v lab checkoutu |
| Secrets | `.env` | `deploy/lab/.env` |

RealmID označuje server v seznamu realmů. **MapID označuje mapu uvnitř
světa a je jiné číslo.** Nová mapa má MapID **725**, oblast AreaID **4988**
a exploration bit **3618**, vybrané podle skutečně načtených klientských DBC.

Používat `make -f Makefile.lab ...`. Běžný `make build/start/reset-db`
nadále míří na původní stack; v lab checkoutu se pro provoz nepoužívá.
CI overlay `compose.lab.ci.yml` se používá pouze při kompilaci/testech,
nikoli při startu realmu. Vyžaduje Docker Compose >= 2.24.4 kvůli
`!override` ([dokumentace Dockeru](https://docs.docker.com/reference/compose-file/merge/)).

## Připravený host — 9. října 2026

Na hostu `192.168.0.248` je připravený checkout
`/home/voslik/WoWBehaviorAI-lab`, samostatná MySQL instance, oddělená
kopie klientských dat a realm `AI World Lab` s ID 2. Přihlašování zůstává
na původním authserveru. Veřejné lab porty jsou 9086 a 9091.

Lab používá nově vygenerovaná hesla a telemetry token; soubory
`deploy/lab/.env` a `deploy/runner/.env` mají práva 0600. Originální MySQL
bylo připojeno do auth sítě bez restartu. Pro jeho budoucí nahrazení je
v původním checkoutu uložený auth overlay a nastavený `COMPOSE_FILE`.
Původní `.env` je zálohované v `runtime/lab-prep/` na tomto hostu.

Základ světa je nový import `TDB335.25101`, nikoli dump živého Elwynnu.
Archiv má SHA256
`b426641e6ce8da02e4109b40e570c2db75f89c228a412b3113f82eb3c8725299`.
Samostatný runner `wow-lab-deploy-runner` je zaregistrovaný a repository
variable `REALM_LAB_DEPLOY_ENABLED` je zapnutá po výslovném schválení.
Budoucí úspěšné pushe do `AI-World-lab` tedy nasazují pouze lab.

[První CI](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/37848941333)
a [ověření neinteraktivních příkazů](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/37850347905)
prošly včetně C++ buildu/testů, extraktorů a skutečných MySQL testů.
Jejich deploy byl přeskočen, protože začaly před zapnutím deploy proměnné.
[První automatické nasazení](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/37850754973)
potom prošlo pro revizi `39d9cf62d8e1`. Ověření z 9. října potvrdilo zdravé
lab služby, oba realmy online a dostupnost portů 9086/9091 z klientského
počítače. Provozní záznam je na hostu v `runtime/lab/receipts/deployed.json`.

Jednorázový import TDB a založení auth účtu z následujícího návodu se na
tomto připraveném hostu neopakují. Aktuální provozní stav se ověřuje
pomocí `docker compose --env-file deploy/lab/.env -f compose.lab.yml ps`
a posledního běhu `Realm Lab CI/CD`. Zdroj vlastní mapy a klientský patch
jsou hotové; aktuální serverový mapový balík je nasazený. Po potvrzení
nativního pohybu je aktivní profil `single-return` pro jednoho lab medvěda.
LLM a skupiny jsou vypnuté. Navazující profil `hunt-cycle` přidává druhého
medvěda a čtyři jeleny pro automatický lov, krmení a návraty.

## První zprovoznění na Linux hostu

Toto je jednorázový postup pro správce hostu; workflow jej samo neprovádí.
Původní běžící realm není potřeba přepínat na novou větev.

### 1. Nový checkout a oddělená konfigurace

```bash
git clone --branch AI-World-lab https://github.com/LoubekJan/WoWBehaviorAI.git /home/voslik/WoWBehaviorAI-lab
cd /home/voslik/WoWBehaviorAI-lab
cp deploy/lab/.env.example deploy/lab/.env
chmod 600 deploy/lab/.env
```

Nastavit vlastní DB hesla, telemetry token, dosažitelnou adresu serveru,
RealmID a volné porty. Nenechat hodnoty `change-me-*`. Použít nového
uživatele `lab_auth`, nikoli původního `trinity` s přístupem ke všem DB.

### 2. Síť ke společnému auth MySQL

```bash
docker network create aitc-shared-auth
```

Pokud síť již existuje, ověřit ji pomocí `docker network inspect`.
Do této sítě připojit pouze MySQL původního stacku a lab worldserver /
setup nástroje. Název sítě musí odpovídat `SHARED_AUTH_NETWORK`.

Pro připojení bez restartu původního MySQL:

```bash
cd /home/voslik/WoWBehaviorAI
primary_mysql=$(docker compose ps -q mysql)
test -n "$primary_mysql"
docker network connect --alias aiworld-auth-db aitc-shared-auth "$primary_mysql"
```

Ruční připojení přetrvá restart stejného kontejneru, ale nepřetrvá jeho
nahrazení. Pro trvalé připojení zkopírovat `compose.auth-share.yml` z lab
checkoutu do původního checkoutu a do **původní** `.env` přidat:

```dotenv
COMPOSE_FILE=compose.yml:compose.auth-share.yml
```

Původní CD používá neparametrizované `docker compose up/restart`, které
tuto hodnotu načte. Jednorázové `docker compose up -d mysql` s overlayem
může MySQL kontejner znovu vytvořit a způsobit krátkou odstávku obou realmů;
naplánovat tento přechod. Pokud byla síť nejprve připojena ručně, není
nutné obnovu provést hned. Původní workflow nepoužívá `git clean`, takže
zkopírovaný overlay zůstane zachován i při jeho `git reset --hard`.

### 3. Lab build a účet pro společný auth

```bash
cd /home/voslik/WoWBehaviorAI-lab
make -f Makefile.lab bootstrap
make -f Makefile.lab build
make -f Makefile.lab test

export LAB_AUTH_ADMIN_USER=root
read -rsp 'Shared auth MySQL admin password: ' LAB_AUTH_ADMIN_PASSWORD
echo
export LAB_AUTH_ADMIN_PASSWORD
make -f Makefile.lab provision-auth
unset LAB_AUTH_ADMIN_PASSWORD
make -f Makefile.lab register-realm
```

Sdílený MySQL administrátor musí smět přistoupit přes Docker síť. Pokud
root dovoluje jen lokální socket, použít správcem vytvořeného dočasného
administrátora pro provisioning; nerozšiřovat vzdálený root jen kvůli labu.
Provisioning odmítá převzít již existujícího uživatele. Při opakovaném
setupu s již vytvořeným uživatelem tento krok přeskočit a ověřit jeho
grants. Registrace realmu je opakovatelná pro stejný ID/název, nastaví
adresu/port a založí počty postav existujících účtů pro nový realm.
Řádek začne jako offline, dokud jej worldserver neaktivuje.

### 4. Světová DB a klientská data

```bash
make -f Makefile.lab import-tdb TDB_VERSION=TDB335.25101 TDB_SHA256=<overeny-sha256-archivu>
```

Použít kompatibilní základ TDB; nejde o dump živého Elwynnu. Import
spouštět pouze pro novou prázdnou lab world DB, protože TDB import
přepisuje obsah. Characters schéma a migrace vytvoří worldserver.

Provisionovat kompletní kompatibilní extrahovaná data do
`runtime/lab/data/{dbc,maps,vmaps,mmaps}`. Vlastní mapa se potom přidává
právě sem. Kopie stejného základu / filesystem reflink je možná;
symlink na zapisovatelná produkční data by zrušil oddělení.

Do `runtime/lab/dbc-base/` dodat neupravené `Faction.dbc` a
`FactionTemplate.dbc`, poté:

```bash
make -f Makefile.lab dbc-factions
make -f Makefile.lab preflight
make -f Makefile.lab start
```

Současné world migrace používají vlastní faction IDs, proto se znovu
používá existující generátor, ale s explicitním **lab výstupem**.
Nepoužívat původní `make dbc-factions`, který zapisuje do `runtime/data`.
Preflight ověřuje přítomnost některých základních DBC a typů dat plus
přesnou registraci realmu. Není to kompletní kontrola všech DBC, dlaždic
ani důkaz průchodnosti nové mapy; další kontrolu provede startup serveru
a později mapový acceptance test.

V první fázi poběží samostatný server s kompatibilním základním světem.
**AI, automatické načtení Elwynnu, export Elwynn telemetrie a modelové
požadavky zůstávají vypnuté. Nasazení vlastní mapy vyžaduje dokončit
samostatný mapový balík a jeho kontroly popsané níže.**

### 5. Samostatné CI/CD

- `.github/workflows/realm-lab.yml`: push/PR na `AI-World-lab`.
- GitHub-hosted kontroly: konfigurace a izolace, Observer/AI protokoly,
  kontrola shody auth schématu/prepared statements proti `ai-world`.
- PR C++ build běží na GitHub-hosted runneru; cizí PR nemá přístup k
  Docker hostu ani lab secrets.
- Push/manual C++ build: stávající `wow,ci` runner, nové lab CI volumes,
  bez připojení do auth sítě. Kompilují se i všechny mapové extraktory.
- Deploy: nový runner s labelem `realm-lab-deploy`, checkout
  `/home/voslik/WoWBehaviorAI-lab`, prostředí GitHub `realm-lab`.
- Deploy potřebuje repository variable `REALM_LAB_DEPLOY_ENABLED=true`.
  Dokud není host připravený, tuto proměnnou nenastavovat. Ani ruční
  spuštění workflow neprovádí deploy, pouze test/build.

Lab runner lze přidat ze samostatného checkoutu:

```bash
cd /home/voslik/WoWBehaviorAI-lab/deploy/runner
cp .env.example .env
# Nastavit stejné repo a platný registrační PAT pro další runner.
docker compose -f compose.lab.yml up -d --build
```

Není nutné kopírovat/přestavovat původní deploy runner. Lab CD nikdy
nerestartuje authserver a nevykonává provisioning ani registraci realmu.
Při kompilaci nových runtime binárek zastaví jen lab worldserver.
Při selhání buildu lab zůstane zastavený; původní realm pokračuje.
Není zde automatický rollback DB migrací. Zálohy lab MySQL jsou potřebné
před změnami persistentních schémat.

## TODO — vlastní mapa a ověřené návraty

Stav z 9. října 2026. Infrastruktura labu a automatické CI/CD jsou hotové;
mapový projekt je znovu otevřený a texturovaný, klientský patch i serverový
balík jsou instalované. Extrakce a generování mmap uspěly, samostatná
kontrola navmesh i její opakování nad nasazenými daty prošly **8/8 tras**.
Uživatel potvrdil funkční vstup na mapu, chůzi a první nativní follow/evade
cyklus medvěda. Profil pro jednoho AI predátora je aktivní a krátký běh
bez hráče je zaznamenaný. Číselný výpis GPS, všechny čtyři kontrolní body
a deset fyzických AI návratů ještě čekají na ověření.
První cíl je jeden mob na vlastní rovné mapě, který se opakovaně fyzicky vrátí domů.

Etapy postupují v tomto pořadí. Společný rozsah AI v kódu lze připravovat
souběžně s mapovým projektem, ale jeho aktivace počká na ověřenou navigaci.
Etapa se označí za dokončenou až po splnění její podmínky a uložení důkazu.

### 1. Projekt mapy a lab klient

- [x] Připravit samostatnou kopii klienta WoW 3.3.5a pro lab.
- [x] Založit Noggit RED projekt a ověřit, že umí vytvořit a znovu otevřít novou mapu.
- [x] Podle skutečných klientských DBC vybrat volné MapID a AreaID.
- [x] Zvolit MapID nejvýše 999; místní mmap generátor čte ID z prvních tří znaků názvu souboru. RealmID 2 je nezávislé číslo.
- [x] Určit verzované vstupy projektu a umístění velkých mapových zdrojů v LFS nebo privátním artefaktovém úložišti. Celý klient, runtime data a secrets nepatří do Gitu.

Stav přípravy: klient `C:\WoWModding\Client-Lab` má build 12340 a locale
enUS. Noggit Studio 3.1446+ `[40bc7ed9]` vytvořil projekt
`runtime/lab/map-project` a mapu **725 / AIWorldLab / AI World Lab**.
Typ mapy v tomto editoru je **None** (`InstanceType=0`), expanze WotLK (2).
Podle skutečně načtených MPQ byla vybrána oblast **4988**, exploration bit
**3618**. Po doplnění `AreaTable.dbc` se projekt i mapa úspěšně znovu
otevřely. Uživatel nanesl texturu na celou dlaždici, uložil ji a editor
před balením a extrakcí zavřel.

V Gitu je [manifest projektu](../data/realm_lab/aiworldlab/project-manifest.json)
a [postup práce se zdroji](../data/realm_lab/aiworldlab/README.md).
Binární zdroje jsou v soukromém lokálním artefaktu
`runtime/lab/map-source-artifacts/aiworldlab-725-area4988-flat-textured-v2.zip`
(SHA-256 `b8913b91306a1b363e116867eb997234dda42b36c4b878e24ccc3fd20fe4a099`).
Obsah balíku je omezený na vlastní mapu, její projekt a potřebné DBC;
pracovní kopie Azerothu do něj nevstupuje. Stejný artefakt a ověřený
klientský podklad jsou připravené také na lab hostu pro extrakci verze v2.

**Hotovo, když:** editor otevře uložený projekt a zvolená ID nekolidují
s klientskými daty. Práce v editoru není zatím ověřená pro bezobslužnou automatizaci.

### 2. Minimální rovný terén

- [x] Vytvořit jednu terénní dlaždici s rovnou testovací plochou přibližně 200 × 200 yardů a existující texturou.
- [x] Zapsat souřadnice navrženého domova a několika kontrolních bodů uvnitř dlaždice, mimo její hrany.
- [x] Zachovat jeden povrch; první verze nemá vodu, svahy, budovy ani překážky.
- [x] Připravit WDT/ADT a potřebné změny `Map.dbc` a `AreaTable.dbc`; názvy adresáře a mapy musí souhlasit.

Vytvořená dlaždice **(30, 31)** má 256 částí terénu a konstantní výšku 0.
Neobsahuje vodu, díry ani objekty. Oblast 4988 je doplněná do DBC i všech
256 částí ADT. Každá má právě jednu vrstvu
`tileset/emeralddream/dreamrock02.blp`. Kontrola všech 37 120 výškových
hodnot potvrzuje rovinu v Z 0. [Kontrolní body](../data/realm_lab/aiworldlab/test-points.json)
jsou odvozené z ADT a serverové konverze os; jejich skutečné GPS ověření
ještě neproběhlo. Navržený domov je `(266.667, 800, 0)`, návratové body
leží 40 yardů v obou směrech X/Y a testovací region má 200 × 200 yardů.
Tyto souřadnice zatím nejsou založeným domovem AI agenta.

**Hotovo, když:** mapa jde znovu otevřít, její rovina je vizuálně ověřená
a souřadnice kontrolních bodů jsou zaznamenané.

### 3. Shodný klientský a serverový balík

- [x] Z mapového projektu sestavit klientský patch pro lab klienta.
- [x] Ze stejné verze obsahu získat `dbc`/`maps` pomocí `mapextractor`, potom `vmaps` pomocí `vmap4extractor` a `vmap4assembler`.
- [x] V čistém pracovním adresáři vygenerovat `mmaps` nové mapy, včetně debug geometrie pro kontrolu navigace.
- [x] Zapsat hashe vstupů a výstupů, základní revizi extraktorů a hashe jejich aplikovaných změn, MapID/AreaID a verzi klientského patche do manifestu.
- [x] Nasadit balík pouze do `runtime/lab/data` a restartovat lab.

Klientský patch `patch-4.MPQ` má 823 658 bajtů a SHA-256
`d974f5919762708acc57e83a70baa6d68a09966e978a45832e4924546baa8b14`.
Stejný soubor je v `runtime/lab/client-patches`, instalovaný v
`C:\WoWModding\Client-Lab\Data` a ve zdroji extrakce na hostu
`runtime/lab/client-source/map725-v2`. Mapové a vmap nástroje nad tímto
ověřeným podkladem uspěly. [Serverový manifest](../data/realm_lab/aiworldlab/server-manifest.json)
zachycuje všechny výstupy, binárky nástrojů, compiler 11.4.0 a základní
revizi `93ff169538e5eb189b76651217b992876a617524` s aplikovanými změnami
mmap generátoru. Tyto změny jsou doložené hashi deseti zdrojových souborů;
nejde o tvrzení, že všechny nástroje vznikly z nezměněného commitu.

Rovina je správně extrahovaná jako konstantní podlaha Z 0 s příznakem
`MAP_HEIGHT_NO_HEIGHT`. Původní mmap generátor takový terén vynechával.
Explicitní opt-in `--includeFlatTerrain true` nyní umožňuje jeho zahrnutí;
výchozí hodnota zůstává false. Opravené meze BV stromu odpovídají skutečné
detailní geometrii a regrese prošla CTest testem `mmaps.flat_terrain`.
V čistém pracovním adresáři vznikla pouze dlaždice nové mapy:

```bash
mmaps_generator 725 --tile 30,31 --includeFlatTerrain true --threads 1 --debugOutput true --silent
```

Výstupy zahrnují `maps/7253130.map` (68 bajtů), `mmaps/725.mmap`
(28 bajtů), `mmaps/7253130.mmtile` (100 732 bajtů) a pět debug souborů.
Detour kontrola promítla všech pět bodů a našla osm úplných tras
domov ↔ čtyři návratové body. Každá vrátila `DT_SUCCESS` a tři polygony;
stejná kontrola prošla nad nasazenými daty. Důkaz má rozsah **navmesh**,
nikoli fyzický pohyb.

Vlastní soubory `vmaps/725*` jsou **očekávaně nepřítomné**: mapa nemá
M2/WMO ani jiné modelové spawny a úspěšný assembler pro ni nevytvoří strom.
Nevytvářejí se náhradní prázdné vmaps. Podlaha a oblast pocházejí z `maps`;
modelové překážky pro LOS zde nejsou. Po přidání objektů je potřeba
vytvořit skutečné vmaps a znovu vygenerovat navigaci.

Nasazených šest souborů tvoří tři DBC, jednu mapu a dva mmap soubory.
Původní DBC záznamy a jejich řetězce byly zachované; přibyly pouze MapID
725, AreaID 4988 a Light ID 2539. Záloha je v
`runtime/lab/data-backups/map725-v2-20261009`. Všechny čtyři lab služby
jsou zdravé, data ostatních map se při tomto nasazení neměnila. Soukromý
balík `runtime/lab/map-source-artifacts/aiworldlab-725-flat-textured-v2-server.zip`
má 866 216 bajtů a SHA-256
`6e193c2e5fe9867b034c73dbd99f48bb7e5b16e0ec9b483f4188948201669dd6`;
SHA a CRC byly ověřené lokálně i na hostu.

**Hotovo, když:** klient a server používají dohledatelně shodný obsah a
všechny mapové výstupy pocházejí z aktuálních vstupů. Pouhé opakované
spuštění generátoru nestačí: existující mmap dlaždice stejné verze formátu
může přeskočit i po změně terénu. Běžný CI nemá klientská data; extrakce
proto musí proběhnout na vybaveném hostu.

### 4. Mapa a navigace bez AIWorld

- [x] Přihlásit hráče na lab a ověřit vstup a chůzi; potvrzeno uživatelem.
- [ ] Uložit číselný výpis `.gps` se správným MapID/AreaID a výškou podlahy.
- [x] Samostatně ověřit navmesh: všech pět kontrolních bodů a osm úplných tras v obou směrech; zopakovat nad nasazenými daty.
- [ ] Ve hře pomocí `.gps` a `.mmap` potvrdit polohu, podlahu a navigaci kolem kontrolních bodů.
- [x] Ověřit první fyzický pohyb jednoho NPC a nativní návrat bez living rolí; uživatel potvrdil nejméně jeden cyklus.
- [ ] Zaznamenat ověření nativního pohybu ze všech čtyř směrů.

Po nasazení shodných dat se v herním chatu GM postavy na lab realmu
použije `.go xyz 266.667 800 2 725` (pořadí X, Y, Z, MapID). Před `.gps`
zrušit vybraný cíl a počkat na dosednutí. Očekává se Map 725, Zone/Area
4988 a stabilní podlaha přibližně Z 0. Stejnou kontrolu a chůzi kolem bodu
zopakovat na všech čtyřech návratových bodech. Indikátor mmap v `.gps`
potvrzuje pouze navmesh mapy; neprokazuje konkrétní dlaždici ani úplnou
cestu. Pro tuto mapu může hlásit nepřítomné vmaps: to odpovídá nulovému
počtu modelových spawnů, nikoli selhání jejich assembleru. GPS, přítomnost
souborů a navmesh kontrola nenahrazují záznam skutečného pohybu NPC.

**Hotovo, když:** funguje navigace i skutečný pohyb v obou směrech.
Selhání této etapy se nejprve řeší v mapových datech nebo základním pohybu.

#### První fyzický test jednoho NPC

[Záznam hráčského potvrzení](../data/realm_lab/aiworldlab/in-game-validation.json)
uvádí odpověď uživatele a hranici dosud ověřeného výsledku. Pro další test
byla v lab databázi ověřena šablona **1186 / Elder Black Bear**, bez
`AIName` a `ScriptName`. Při tomto původním nativním testu byly AIWorld
a living role vypnuté.

1. Na labu zadat `.gm on` a `.go xyz 266.667 800 2 725`; nechat postavu
   dosednout na podlahu. `.mmap loc` má ukázat `7253130.mmtile`.
2. Zadat `.npc add temp noloot 1186`. Vybrat medvěda a zaznamenat
   `.npc info` a `.gps`: jeho GUID a původní souřadnice.
3. Zadat `.npc follow` a hráčem ujít přibližně 40 yardů směrem k
   `(306.667, 800)`. Sledovat souvislý fyzický pohyb medvěda.
4. S medvědem stále vybraným zadat `.npc follow stop` a `.npc evade`.
   Sledovat návrat na původní místo; `.gps` a `.npc info` porovnat s
   počátečním záznamem. GUID má zůstat stejný.
5. Vrátit hráče do středu a stejný test zopakovat směrem k ostatním
   třem návratovým bodům. Po dokončení lze vybraný dočasný spawn
   odstranit přes `.npc delete`.

Dočasný spawn se nezapisuje do DB. `follow stop` pouze odstraní
následování; návrat spouští `evade` a používá původní runtime home.
Tento test ověřuje nativní pohyb a návrat serveru. HomeMovementGenerator
může použít přímou náhradní trasu; úspěch proto sám o sobě nepotvrdí
AIWorld recovery ani striktní použití navmeshe při návratu. Uživatel
potvrdil „ano vše fungovalo“; zaznamenán je nejméně jeden úspěšný cyklus.

### 5. Přenos potřebných částí AI do labu

Připravený profil `single-return` používá mapu 725, root oblast 4988 a
region 200 × 200 yardů z `test-points.json`. Přebírá jediný persistentní
spawn **900725 / Elder Black Bear 1186** s domovem `(266.667, 800, 0)`.
Bootstrap nejprve nastaví Observe mode, aktivace vyžaduje samostatný
ověřený krok. Staré záznamy Elwynnu zůstávají v lab DB; nejsou členy
aktivního runtime. Výchozí `LAB_AI_PROFILE=disabled` zůstává bezpečným
startovním stavem.

`SimulationScope` je společná definice mapy, root zón, hranic a členství.
Living, striktní návratové cesty, chase a telemetrie ji používají bez
přepisování individuálního home. Rozsah gridů se načítá bez paddingu,
aby jedna dlaždice nevyžadovala okolní ADT. Observer a recorder odvozují
stejné hranice z verzovaných souřadnic a uchovávají i pozorování NPC,
které později opustí region.

První profil zapíná living a telemetrii, ale vypíná všechny vzdálené
AI požadavky, skupiny, růst hladu/únavy a vývoj zdrojové potřeby,
modelové rady a automatické Elwynn piloty. Běh bez hráče a fyzické
návraty se musí ověřit po nasazení; samotná kompilace je nepotvrzuje.

- [x] Zavést jednu společnou definici mapy, povolených oblastí a hranic simulace.
- [x] Nahradit rozptýlené předpoklady map 0 / zone 12 v living, lovu, návratu, perception, validátorech a chase. Zahrnout `LivingRolePolicy`, `LivingRole.cpp`, `LivingWolf.cpp`, `LivingRecoveryPath.cpp`, `ActionSystem` a `ElwynnHuntPath`.
- [x] Napojit načítání a aktualizaci gridů na nový rozsah a ověřit krátký běh území bez přihlášeného hráče.
- [x] Provést explicitní lab bootstrap: spawn 900725, Predator, FullAgent, kontrolované řízení a pevný domov. Historický pilot 80683 v soukromém labu převeden na Observe; runtime obsahuje pouze lab agenta.
- [x] Upravit telemetrii, Observer, hranice zobrazení, recorder a vyhodnocení pro novou mapu. Snapshoty obsahují jediného lab agenta.
- [x] Přidat ověřovaný aktivační profil do `tools/realm_lab/manage.py`; `disabled` zůstává výchozí a `single-return` je v labu aktivní.
- [x] Při prvním zapnutí ponechat LLM/modelové požadavky vypnuté.

[Záznam aktivace](../data/realm_lab/aiworldlab/scope-activation.json) obsahuje
20 čerstvých snapshotů za 40 sekund bez hráče, se shodným domovem,
nulovými potřebami a žádnou skupinou. Living role přešla mezi činnostmi
LOOK a REST; krátký záznam neobsahuje odvedení ani přijatý návratový test.
476 C++ testů a samostatné smluvní CI prošly, všechny čtyři lab služby
jsou zdravé. Původní realm a authserver zachovaly container ID i čas startu.

**Hotovo, když:** jeden lab agent má správnou identitu, roli, domov a
telemetrii a aktualizuje se i bez hráče. Aktivace musí navazovat na
ověřený mapový balík a bootstrap.

### 6. Samotný návrat jednoho predátora

- [x] Zaznamenat první kvalitativní potvrzení autonomního návratu: „ok vypadá to funkční“. Telemetrie při následné kontrole ukazuje 12 yardů od původního domova a stav READY; čas a GUID před/po nejsou doložené.
- [x] Založit jednoho obyčejného predátora: medvěd 900725 / entry 1186, bez skupiny, hladu a hrozeb.
- [ ] Pro test měnit pouze aktuální polohu, například na vzdálenost 40 yardů; zachovat původní spawn i runtime home.
- [x] Stanovit první experiment: produkční living návrat po `follow stop`, limit 60 s a domácí oblast predátora do 14 yardů; viz [podrobný postup](../tools/realm_lab/SINGLE_RETURN.md).
- [ ] Zaznamenat deset fyzických návratů z různých směrů do původní domácí oblasti a následnou další činnost.
- [ ] Zopakovat ověření bez přihlášeného hráče.

Další série používá pořadí **−X, +X, −Y, +Y, +X, −X, +Y, −Y, −X, +X**
a [čtyři body](../data/realm_lab/aiworldlab/test-points.json). Recorder
ukládá telemetrii po 2 sekundách; čas od `follow stop` a GUID před/po
se zaznamenávají zvlášť. Série proběhne bez restartu a respawnu. Změny
záznamu se pushnou po dokončení série, protože lab CI/CD zastavuje
recorder a restartuje worldserver.

Po prvním uživatelském potvrzení návratu uživatel zvolil pokračování
automatickou populací lovců a kořisti. Ruční série zůstává dostupným
regresním postupem; není vydávána za deset dokončených návratů.

**Hotovo, když:** všechny požadované návraty jsou skutečně dokončené
ve stejné životní instanci a ve stanoveném limitu. Přijetí pohybové akce,
teleport, respawn nebo restart nejsou důkazem návratu.

### 7. Lov a postupné rozšiřování

Aktuální navazující scénář je `LAB_AI_PROFILE=hunt-cycle`: **2 medvědi
(1186 / Predator) a 4 jeleni (883 / Prey)**, spawny 900725–900730.
Definice [populace](../data/realm_lab/aiworldlab/hunt-population.json)
zachovává původní domov medvěda 900725 a přidává ostatní domovy do stejné
ověřené oblasti. Nový bootstrap a aktivační kontrola povolují pouze tuto
populaci. Hlad roste přirozeně `0.003/s`, první lov nastane přibližně po
čtyřech minutách; modely a skupiny zůstávají vypnuté.

Populace byla aktivovaná 9. 10. 2026 na revizi `d7319c5387`.
[Záznam aktivace](../data/realm_lab/aiworldlab/hunt-activation.json)
potvrzuje všech šest živých NPC s jejich rolemi a pevnými domovy,
čtyři zdravé lab služby a nezměněný běh původního worldserveru i authserveru.
CI prošlo 68 realm kontraktů, 112 Observer testů, 74 AI testů a 478 C++ testů.
Záznam aktivace sám neprokazuje dokončený lov ani fyzický návrat.

[Postup nasazení a pozorování](../tools/realm_lab/HUNT_POPULATION.md)
popisuje automatický cyklus a jeho důkazy. Útěk kořisti v labu nyní
kontroluje mapu 725, root oblast 4988 a celý koridor uvnitř hranic;
nepoužívá pevnou zónu 12. Kořist se po smrti obnovuje, každý spawn má
prodlevu respawnu 120 sekund. Obnovení predátora přeruší jeho pozorovaný
cyklus. Terén ani klientský patch se kvůli populaci nemění.

- [x] Nasadit dva predátory a čtyři kořisti s pevnými domovy a přirozeným hladem na mapu 725.
- [ ] Zaznamenat deset automatických cyklů lov → potrava → fyzický návrat → další činnost; odlišit lov dokončený už v domácí oblasti.
- [ ] Po průchodu tohoto scénáře rozšířit populaci na deset NPC a ověřit plánovací rozpočet i návraty.
- [ ] Teprve po průchodu roviny přidat mírný svah a zopakovat stejné scénáře.
- [ ] Samostatně přidat jednu překážku s průchodem a zopakovat stejné scénáře.
- [ ] Další typy terénu, skupiny a modelové rady přidávat až podle výsledků předchozích etap.

**Hotovo, když:** každé rozšíření má samostatný průkazný záznam a nezhorší
již ověřené návraty.

### Postup při prvním selhání

Při selhání etapu zastavit, uložit přesný scénář, revizi kódu, hashe dat a
záznam pohybu. Reprodukovat stejný případ a určit, zda selhalo hledání
cesty, její validace, provedení nebo rozpoznání dokončení. Další etapa
počká na vysvětlení a ověření opravy; původní domov zůstává neměnný.
Úspěch infrastruktury a CI/CD není důkazem funkční mapy ani návratů.

## Ověření a zdroje

Lokální kontrakty: `python -m unittest discover -s tools/realm_lab/tests -v`.
Validace Compose může běžet bez Docker Engine pomocí `docker compose config`.
Plný C++ build, MySQL integraci a souběh dvou živých realmů musí potvrdit
CI / Linux host. Úspěšné CI infrastruktury není úspěšný test návratů.

Při přípravě prošlo 14 lokálních testů konfigurace/izolace, validace
runtime i CI Compose skutečným Compose 2.24.4 a syntaxe Bash skriptů.
Sada Observeru: 105 testů, z toho 1 přeskočený; AI protokoly: 74 testů.
Čtyři skutečné MySQL integrační testy jsou zapojené do CI proti jednorázové
MySQL službě; místně jsou přeskočené, protože zde není Docker Engine.

- [TrinityCore realmlist: samostatný záznam a shoda ID s RealmID](https://trinitycore.atlassian.net/wiki/spaces/tc/pages/2130016/).
- Lokální `DatabaseLoader.h`: masky AUTH=1, CHARACTER=2, WORLD=4;
  proto lab používá `Updates.EnableDatabases = 6`.
- [Noggit RED, upstream editoru pro 3.3.5](https://gitlab.com/prophecy-rp/noggit-red).
- [Generování a prohlížení vlastní navigace v TrinityCore 3.3.5](https://github.com/stoneharry/mmaps-for-custom-maps).
