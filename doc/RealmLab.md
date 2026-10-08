# Samostatný realm pro vlastní testovací mapu

Stav přípravy: 8. října 2026. Větev `AI-World-lab` vychází z
`ai-world` na revizi `c08fec3f2a`. Připravené soubory oddělují provoz a
CI/CD; vlastní mapa a přenos AI mimo Elwynn jsou další etapa.

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
světa a je jiné číslo.** ID nové mapy zatím není zvolené; před jeho
přidělením je nutné zkontrolovat `Map.dbc` a možnosti extraktorů.

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
a posledního běhu `Realm Lab CI/CD`. Vlastní mapa a AI mimo Elwynn
nadále čekají na samostatnou implementaci.

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
požadavky jsou vypnuté. Vlastní mapa ještě není součástí této přípravy.**

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

Plán z 9. října 2026. Infrastruktura labu a automatické CI/CD jsou hotové;
níže uvedené mapové a AI etapy zatím nejsou dokončené. První cíl je jeden
mob na vlastní rovné mapě, který se opakovaně fyzicky vrátí domů.

Etapy postupují v tomto pořadí. Společný rozsah AI v kódu lze připravovat
souběžně s mapovým projektem, ale jeho aktivace počká na ověřenou navigaci.
Etapa se označí za dokončenou až po splnění její podmínky a uložení důkazu.

### 1. Projekt mapy a lab klient

- [x] Připravit samostatnou kopii klienta WoW 3.3.5a pro lab.
- [ ] Založit Noggit RED projekt a ověřit, že umí vytvořit a znovu otevřít novou mapu.
- [x] Podle skutečných klientských DBC vybrat volné MapID a AreaID.
- [x] Zvolit MapID nejvýše 999; místní mmap generátor čte ID z prvních tří znaků názvu souboru. RealmID 2 je nezávislé číslo.
- [x] Určit verzované vstupy projektu a umístění velkých mapových zdrojů v LFS nebo privátním artefaktovém úložišti. Celý klient, runtime data a secrets nepatří do Gitu.

Stav přípravy: klient `C:\WoWModding\Client-Lab` má build 12340 a locale
enUS. Noggit Studio 3.1446+ `[40bc7ed9]` vytvořil projekt
`runtime/lab/map-project` a mapu **725 / AIWorldLab / AI World Lab**.
Typ mapy v tomto editoru je **None** (`InstanceType=0`), expanze WotLK (2).
Podle skutečně načtených MPQ byla vybrána oblast **4988**, exploration bit
**3618**. Mapu editor již načetl; po dodatečném doplnění `AreaTable.dbc`
ještě zbývá nové otevření projektu a vizuální kontrola.

V Gitu je [manifest projektu](../data/realm_lab/aiworldlab/project-manifest.json)
a [postup práce se zdroji](../data/realm_lab/aiworldlab/README.md).
Binární zdroje jsou v soukromém lokálním artefaktu
`runtime/lab/map-source-artifacts/aiworldlab-725-area4988-flat-untextured-v1.zip`.
Obsah balíku je omezený na vlastní mapu, její projekt a potřebné DBC;
pracovní kopie Azerothu do něj nevstupuje. Před nasazením další verze
zajistit také kopii artefaktu na lab hostu.

**Hotovo, když:** editor otevře uložený projekt a zvolená ID nekolidují
s klientskými daty. Práce v editoru není zatím ověřená pro bezobslužnou automatizaci.

### 2. Minimální rovný terén

- [ ] Vytvořit jednu terénní dlaždici s rovnou testovací plochou přibližně 200 × 200 yardů a existující texturou.
- [ ] Označit domov a několik kontrolních bodů uvnitř dlaždice, mimo její hrany.
- [ ] Zachovat jeden povrch; první verze nemá vodu, svahy, budovy ani překážky.
- [x] Připravit WDT/ADT a potřebné změny `Map.dbc` a `AreaTable.dbc`; názvy adresáře a mapy musí souhlasit.

Vytvořená dlaždice **(30, 31)** má 256 částí terénu a konstantní výšku 0.
Neobsahuje vodu, díry ani objekty. Oblast 4988 je doplněná do DBC i všech
256 částí ADT. Zatím má **nulový počet texturových vrstev**; další ruční
krok je nanést jednu existující texturu a zaznamenat vnitřní kontrolní body.

**Hotovo, když:** mapa jde znovu otevřít, její rovina je vizuálně ověřená
a souřadnice kontrolních bodů jsou zaznamenané.

### 3. Shodný klientský a serverový balík

- [ ] Z mapového projektu sestavit klientský patch pro lab klienta.
- [ ] Ze stejné verze obsahu získat `dbc`/`maps` pomocí `mapextractor`, potom `vmaps` pomocí `vmap4extractor` a `vmap4assembler`.
- [ ] V čistém pracovním adresáři vygenerovat `mmaps` nové mapy, včetně debug geometrie pro kontrolu navigace.
- [ ] Zapsat hashe vstupů a výstupů, revizi extraktorů, MapID/AreaID a verzi klientského patche do manifestu.
- [ ] Nasadit balík pouze do `runtime/lab/data` a restartovat lab.

**Hotovo, když:** klient a server používají dohledatelně shodný obsah a
všechny mapové výstupy pocházejí z aktuálních vstupů. Pouhé opakované
spuštění generátoru nestačí: existující mmap dlaždice stejné verze formátu
může přeskočit i po změně terénu. Běžný CI nemá klientská data; extrakce
proto musí proběhnout na vybaveném hostu.

### 4. Mapa a navigace bez AIWorld

- [ ] Přihlásit hráče na lab, vstoupit na kontrolní bod a ověřit správné MapID/AreaID.
- [ ] Ověřit stabilní výšku podlahy, bez propadání a chybějících mapových souborů.
- [ ] Pomocí `.gps`, `.mmap` a debug geometrie prověřit trasy mezi několika kontrolními body v obou směrech.
- [ ] Ověřit běžný fyzický pohyb jednoho NPC mezi těmito body bez zapnutých living rolí.

**Hotovo, když:** funguje navigace i skutečný pohyb v obou směrech.
Selhání této etapy se nejprve řeší v mapových datech nebo základním pohybu.

### 5. Přenos potřebných částí AI do labu

- [ ] Zavést jednu společnou definici mapy, povolených oblastí a hranic simulace.
- [ ] Nahradit rozptýlené předpoklady map 0 / zone 12 v living, lovu, návratu, perception, validátorech a chase. Zahrnout `LivingRolePolicy`, `LivingRole.cpp`, `LivingWolf.cpp`, `LivingRecoveryPath.cpp`, `ActionSystem` a `ElwynnHuntPath`.
- [ ] Napojit načítání a aktualizaci gridů na nový rozsah a ověřit běh území bez přihlášeného hráče.
- [ ] Připravit explicitní lab bootstrap: nové spawny, role, účast v simulaci, control mode a domovy. Auditovat a oddělit historické Elwynn piloty/skupiny vytvořené migracemi.
- [ ] Upravit telemetrii, Observer, hranice zobrazení, recorder a vyhodnocení pro novou mapu. Ověřit, že snapshoty obsahují pouze lab populaci.
- [ ] Přidat ověřovaný aktivační profil do `tools/realm_lab/manage.py`; současný generátor AI/living/telemetrii přepisuje na nulu. Neupravovat ručně vygenerovaný conf.
- [ ] Při prvním zapnutí ponechat LLM/modelové požadavky vypnuté.

**Hotovo, když:** jeden lab agent má správnou identitu, roli, domov a
telemetrii a aktualizuje se i bez hráče. Aktivace musí navazovat na
ověřený mapový balík a bootstrap.

### 6. Samotný návrat jednoho predátora

- [ ] Založit jednoho obyčejného predátora, například medvěda, bez skupiny, hladu a hrozeb.
- [ ] Pro test měnit pouze aktuální polohu, například na vzdálenost 40 yardů; zachovat původní spawn i runtime home.
- [ ] Použít současný produkční návratový systém a před testem stanovit časový limit dokončení.
- [ ] Zaznamenat deset fyzických návratů z různých směrů do původní domácí oblasti a následnou další činnost.
- [ ] Zopakovat ověření bez přihlášeného hráče.

**Hotovo, když:** všechny požadované návraty jsou skutečně dokončené
ve stejné životní instanci a ve stanoveném limitu. Přijetí pohybové akce,
teleport, respawn nebo restart nejsou důkazem návratu.

### 7. Lov a postupné rozšiřování

- [ ] Přidat jednu kořist a zaznamenat deset cyklů lov → potrava → fyzický návrat → další činnost.
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
- [Noggit RED, editor pro 3.3.5](https://gitlab.com/serayn/noggit-red).
- [Generování a prohlížení vlastní navigace v TrinityCore 3.3.5](https://github.com/stoneharry/mmaps-for-custom-maps).
