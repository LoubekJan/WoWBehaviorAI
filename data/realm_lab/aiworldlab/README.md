# AI World Lab — mapy, populace a důkazy

> Vyčištění proběhlo a bylo ověřeno 31. října na revizi `5437178873e7`:
> 0 NPC/AI vazeb/charakterů, osm zdravých služeb. Účty, hráčské postavy a
> geometrie jsou zachované; finální rozložení mapy zůstává další práce.


Aktualizováno **31. října 2026**. Aktivní svět používá mapu **725**, oblast
**4988**, realm **2** a geometrii **`preview-v8`**. Pro finální úpravy se
používá **`authoring-empty`**, bez NPC a aktivních charakterů. Manifesty
původních 100 aktérů zůstávají historickými/regresními daty; v tomto režimu
se jejich bootstrap nespouští. Původní Elwynn runtime a geometrie v2/v3 nejsou aktivní
provozní prostředí. Návod k provozu je v [README_DEV.md](../../../README_DEV.md)
a [RealmLab.md](../../../doc/RealmLab.md).

## Současná geometrie a zdroje

[Projektový manifest](preview-v8/project-manifest.json) vychází z uloženého
uživatelského projektu, při přípravě upravuje jen oddělený staging a
zachovává jeho evidované zdrojové soubory. Mapa má čtyři ADT dlaždice
`(30,31)`, `(31,31)`, `(30,32)`, `(31,32)` a přibližné hranice
X −533,333 až 533,333 a Y 0 až 1066,667 yardu. Není to původní jediná rovná
dlaždice v Z 0. Stavby, voda, důl, most a tábory vycházejí z autorovy mapy.

[Serverový manifest](preview-v8/server-manifest.json) a
[kontrola dat](preview-v8/data-verification.json) popisují shodný výstup
DBC/maps/vmaps/mmaps včetně kolizních závislostí. Populační preflight
ověřuje 61 geometrických souborů. Binární zdroje, klientské MPQ a extrahovaná
serverová data jsou soukromé; nejsou distribuované v Gitu. Změna geometrie
vyžaduje znovu sestavit shodný klientský i serverový balík. Pouhé nasazení
charakterové karty, vzhledu nebo vlastní šablony geometrii nemění.

Umístění NPC a jejich domovy vycházejí z
[autorských kotev](preview-v8/npc-anchor-audit.json),
[rozložení](preview-v8/npc-layout-proposal.json) a
[přijaté populace](preview-v8/settlement-population.json).
`full-v8` zachovává jejich identitu a domovy. Herní entry jednotlivé
pojmenované postavy může nahradit ověřená vlastní šablona; základní
populační manifest zůstává nezměněným zdrojem behaviorální identity.

## Profily nad stejnou mapou

| Profil | AIWorld / pouze nativní | Rozsah |
| --- | --- | --- |
| `settlement-v8` | 0 / 100 | Výchozí konzervativní `.env.example`, nativní přehled |
| `wildlife-v8` | 20 / 80 | 16 jelenů a 4 medvědi |
| `supply-v8` | 23 / 77 | Navíc lidský a dva gnollí lovci |
| `consumers-v8` | 26 / 74 | Navíc tři spotřebitelé |
| `raids-v8` | 28 / 72 | Navíc první Defias a strážný |
| `fields-v8` | 40 / 60 | Navíc dvanáct pracovníků polí |
| `defias-v8` | 55 / 45 | Všech 16 Defias, skupinové výpravy a táborová spotřeba |
| `full-v8` | 100 / 0 | Původní plná testovací populace, běžné role, dvě karavany a městská spotřeba |
| **`authoring-empty`** | **0 / 0** | **Příprava finální mapy**, žádné NPC, AI a telemetrie vypnuté |

Tyto počty jsou pro zapnutý příslušný profil, nikoli počty LLM karet.
Manifests [polí](preview-v8/field-population.json),
[Defias](preview-v8/defias-population.json) a
[plné populace](preview-v8/full-population.json) obsahují explicitní identity
a kapacity. Předchozí `full-v8` přidává městský sklad 7, nosiče 901040/901041,
doprovody 901042/901043 a městské spotřebitele 901014/901015.

## Co dokazují uložené výsledky

| Důkaz | Rozsah |
| --- | --- |
| [Wildlife](preview-v8/wildlife-first-cycle-acceptance.json) | První fyzická pastva, lov, krmení a návrat zvěře na jeho zaznamenané revizi |
| [Supply](preview-v8/supply-first-chain-acceptance.json), [plánovací rozpočet](preview-v8/supply-budget-chain-acceptance.json) | Fyzický řetězec lovců, maso, náklad, doručení a jídlo |
| [Consumers](preview-v8/consumer-first-chain-acceptance.json) | Fyzická spotřeba a návrat tří tehdejších spotřebitelů |
| [Raid](preview-v8/raid-first-chain-acceptance.json) | Dva dokončené přepady a doložené přerušení strážným |
| Navigační audity v `preview-v8` | Průchodnost konkrétních cest, nikoli automaticky jejich dokončení NPC |

Receipts jsou připnuté k vlastním revizím, datům a záznamům. Neoznačovat je
za nový fyzický PASS celé populace, karavan nebo změněného modelu.
Revize `664d84687d7a` prošla 31. října
[CI/CD](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/41158138495)
(846 Python / 699 nativních testů) a následnou kontrolou osmi služeb,
100 řízených NPC a tří vlastních šablon s aktivními LLM controllery.
Její serverová kontrola nenahrazuje dlouhodobé fyzické přijetí `full-v8`.

Aktuální recorder a acceptance postupy jsou v
[ObserverRecording.md](../../../doc/ObserverRecording.md). Stav zásob,
potřeb, úrody, přepadů a karavan se přes restart nyní neukládá; tato práce
je výslovně odložená. Jména, vzhled a příběhy se připravují v
[Ateliéru postav](../../../doc/CharacterStudio.md).

## Historie rovné mapy v2

Následující text zachovává extrakci a testování původní v2. Výrazy
„aktuální“, „aktivní“, tehdejší čtyři služby, spawny 900725–900824 a uvedené
hashe platí v rámci tohoto historického experimentu. Nejsou popisem
nasazeného `preview-v8`. Příkazy pro starý bootstrap nebo doplnění jediné
ADT nespouštět nad dnešním mapovým projektem či databází. Terén
[v3](terrain-v3/README.md) představuje další oddělený historický krok.

První vlastní mapa: MapID **725**, AreaID **4988**, exploration bit **3618**.
Interní adresář `AIWorldLab`, název `AI World Lab`, `InstanceType=0`
(Noggit zobrazuje `None`), expanze WotLK (2). Aktivní je jedna dlaždice
`(30, 31)`. Aktuální terén je rovný v Z 0. Všech 256 částí ADT má oblast
4988 a jednu vrstvu `tileset/emeralddream/dreamrock02.blp`.

## Umístění a verze

- Klient: `C:\WoWModding\Client-Lab` — 3.3.5a build 12340, enUS.
- Editor: `C:\WoWModding\RelWithDebInfo\noggit.exe` — Noggit Studio
  3.1446+ `[40bc7ed9]`; jeho SHA-256 je v manifestu.
- Pracovní projekt: `C:\WoWBehaviorAI\runtime\lab\map-project`.
- Klientský základ DBC: `runtime/lab/client-baseline/dbc`.
- Soukromé binární artefakty: `runtime/lab/map-source-artifacts`.

V Gitu zůstává manifest s hashi a nástroj pro doplnění oblasti. Samotný
klient, plné DBC a binární zdroje mapy se ukládají soukromě. První lokální
ZIP obsahuje jen soubory uvedené v `project-manifest.json`; jeho CRC i
seznam položek byly ověřené. Aktuální zdrojová verze je
`aiworldlab-725-area4988-flat-textured-v2.zip` (71 167 bajtů, SHA-256
`b8913b91306a1b363e116867eb997234dda42b36c4b878e24ccc3fd20fe4a099`).
Historická netexturovaná v1 není vstupem aktuální extrakce.

Klientský patch `patch-4.MPQ` je sestavený a instalovaný do
`C:\WoWModding\Client-Lab\Data`. Obsahuje pouze šest mapových/DBC souborů;
projekt `.noggitproj` do klientského patche nevstupuje. Patch má 823 658
bajtů a SHA-256
`d974f5919762708acc57e83a70baa6d68a09966e978a45832e4924546baa8b14`.
Stejná verze je v `runtime/lab/client-patches` i ve zdroji extrakce
`/home/voslik/WoWBehaviorAI-lab/runtime/lab/client-source/map725-v2`.

Základní DBC pocházejí z `Data/enUS/patch-enUS-3.MPQ`; prověřené byly
všechny archivy načtené editorem včetně vlastního `patch-W.MPQ`.
Původní `Map.dbc` má 135 záznamů, `AreaTable.dbc` 2307. Uložený projekt
přidává mapu 725, oblast 4988 a světlo 2539; původní záznamy a jejich
řetězce zůstávají zachované.

## Doplnění oblasti

Před zápisem uložit práci a zavřít Noggit. Z kořene repozitáře:

```powershell
python tools/realm_lab/prepare_map_area.py --prepare
```

Nástroj ověří mapu, kolize s klientským základem a jednu aktivní dlaždici.
Přidá venkovní neutrální oblast 4988 a přiřadí ji všem 256 částem ADT.
Před změnou uchová původní soubory pod `runtime/lab/map-project-backups`.
Výsledek zapíše do `runtime/lab/map-area-receipt.json`. Další ověření
bez zápisu:

```powershell
python tools/realm_lab/prepare_map_area.py
```

Ověřovací režim odmítne neúplné přiřazení. Připravené soubory se při
opakovaném běhu nemění. Na Windows nástroj odmítne zápis při běžícím
procesu `noggit`.

## Dokončené mapové vstupy

Projekt a mapa se po doplnění oblasti úspěšně znovu otevřely. Uživatel
natřel celou dlaždici a uložil ji; editor byl před balením zavřený.
Kontrola WDT/ADT potvrzuje jednu dlaždici, 256 částí s oblastí 4988 a
jednou texturou, 37 120 výškových hodnot Z 0, žádnou vodu, díry ani
objekty. [Kontrolní body](test-points.json) uvádějí navržený domov,
návratové body ±40 yardů a vnitřní testovací region 200 × 200 yardů.
Jde o souřadnice odvozené z geometrie; GPS a pohyb ještě nejsou ověřené.

## Dokončený serverový balík a navmesh

Extrakce `dbc`/`maps`, `vmap4extractor` a `vmap4assembler` nad ověřeným
klientským podkladem v2 na lab hostu uspěly. Původní mmap generátor
vynechával konstantní podlahu uloženou s `MAP_HEIGHT_NO_HEIGHT`. Nyní je
doplněný explicitní opt-in `--includeFlatTerrain true`; výchozí hodnota
zůstává false. Opravené meze BV stromu odpovídají skutečné detailní
geometrii. Test `mmaps.flat_terrain` zahrnuje plochý terén i regresi
prostorového hledání polygonů a prošel.

V čistém pracovním adresáři byla vygenerovaná pouze dlaždice mapy 725:

```bash
mmaps_generator 725 --tile 30,31 --includeFlatTerrain true --threads 1 --debugOutput true --silent
```

`725.mmap` má 28 bajtů, `7253130.mmtile` 100 732 bajtů. Vzniklo také
pět debug souborů. Samostatná Detour kontrola promítla všech pět bodů a
ověřila **8/8 úplných tras** domov ↔ čtyři návratové body. Každá měla
tři polygony a `DT_SUCCESS`; opakování nad nasazenými daty také prošlo.
Tento důkaz ověřuje navmesh, nikoli hráčský či NPC pohyb.

[Serverový manifest](server-manifest.json) zaznamenává hashe šesti
serverových souborů i debug výstupů, nástrojových binárek a aplikovaných
zdrojových změn. Extraktory vycházejí z
`93ff169538e5eb189b76651217b992876a617524`; mmap generátor má pracovní
overlay doložený hashi deseti souborů. Compiler je 11.4.0. Základní Git
revize sama nepopisuje tyto změny. Zaznamenaný hash vstupního manifestu
označuje jeho stav při extrakci; následné stavové aktualizace tohoto
projektového manifestu jej mohou změnit.

Vlastní vmaps jsou očekávaně nepřítomné. Nulový počet MDDF/MODF/WDT
modelových spawnů znamená, že úspěšný assembler pro mapu 725 nevytvoří
strom. Nepřidávají se falešné prázdné vmaps. Povrch a AreaID jsou v
`maps`; modelové překážky pro LOS mapa nemá. Po přidání modelů musí
vzniknout skutečné vmaps i nová navigace.

Šest souborů (tři DBC, `.map`, `.mmap` a `.mmtile`) je instalovaných do
`runtime/lab/data`; záloha je v `runtime/lab/data-backups/map725-v2-20261009`.
Všechny čtyři lab služby jsou zdravé. Ostatní mapová data se při nasazení
neměnila. Soukromý balík
`runtime/lab/map-source-artifacts/aiworldlab-725-flat-textured-v2-server.zip`
má 866 216 bajtů, SHA-256
`6e193c2e5fe9867b034c73dbd99f48bb7e5b16e0ec9b483f4188948201669dd6`;
lokální i hostitelské SHA/CRC ověření prošlo.

### Opakování samostatných kontrol

V nakonfigurovaném CMake buildu s `TOOLS=1` a `BUILD_TESTING=ON`:

```bash
cmake --build "$mmap_build_dir" --target mmaps_flat_terrain_test
ctest --test-dir "$mmap_build_dir" -R '^mmaps[.]flat_terrain$' --output-on-failure --no-tests=error
```

`mmap_build_dir` je kořen tohoto buildu. Samostatný verifikátor se v
build kontejneru zkompiluje proti **Detour z tohoto repozitáře** a jeho
64bitovým polygonovým referencím, při mountu zdrojů do `/workspace`
a odpovídajícím buildu v `/build`:

```bash
g++ -std=c++20 -O2 -DDT_POLYREF64 \
  -I/workspace/src/common -I/workspace/src/common/Utilities \
  -I/workspace/src/common/Collision/Maps \
  -I/workspace/dep/recastnavigation/Detour/Include \
  /workspace/tools/realm_lab/verify_navmesh.cpp \
  /build/dep/recastnavigation/Detour/libDetour.a \
  -o /workspace/runtime/lab/validation/verify_navmesh
/workspace/runtime/lab/validation/verify_navmesh /workspace/runtime/lab/data
```

Výstupní adresář `runtime/lab/validation` musí existovat. Exit 0 a
`complete_paths=8` jsou důkazem těchto navmesh tras; cizí Detour knihovna
či samotná existence `.mmtile` je nenahrazují.

## Další krok — GPS a fyzický pohyb

Následuje hráčská kontrola na labu: `.go xyz 266.667 800 2 725`, zrušit
cíl, počkat na dosednutí a použít `.gps`. Očekává se Map 725, Zone/Area
4988 a stabilní podlaha Z 0. Zopakovat GPS a chůzi ve všech čtyřech
návratových bodech. `.gps` ověřuje navmesh jen na úrovni mapy; samotný
indikátor mmap neprokazuje konkrétní dlaždici ani úplnou cestu. Hlášení
chybějících vlastních vmaps je zde očekávané při nulovém počtu modelů.
Uživatel potvrdil funkční vstup na mapu a chůzi. [Hráčský záznam](in-game-validation.json)
zachovává jeho potvrzení; číselný výpis GPS a ověření všech čtyřech bodů
ještě chybí. Uživatel potvrdil fyzický test dočasného medvěda 1186 přes
`.npc follow`, `.npc follow stop` a `.npc evade`, podle
[postupu v TODO](../../../doc/archive/RealmLab-2026-10-11.md#první-fyzický-test-jednoho-npc).
Potvrzen je nejméně jeden cyklus následování a nativního návratu. Uživatel
také uvedl, že první autonomní AIWorld návrat vypadá funkčně; časy a
GUID před/po zatím nezaznamenal. Mapový generátorový manifest nadále
popisuje stav při původní extrakci. Aktivace AI se řídí lab profilem.

Profil `single-return` připravil persistentního medvěda **900725**.
[Záznam aktivace](scope-activation.json) potvrzuje krátký běh bez hráče,
jediného kontrolovaného predátora na mapě 725, pevný domov, nulové
potřeby a nepřítomnost skupiny. Modelové požadavky jsou vypnuté.
[Další herní test](../../../tools/realm_lab/SINGLE_RETURN.md) odvede
medvěda 40 yardů přes follow a pozoruje autonomní návrat po follow stop,
v limitu 60 sekund do domácí oblasti o poloměru 14 yardů. Deset fyzických
cyklů a jejich opakování bez hráče zatím nejsou ověřené. Nové potvrzení
prvního návratu je v [hráčském záznamu](in-game-validation.json).

Aktivní profil **`hunt-cycle`** používá dva medvědy a čtyři jeleny,
spawny 900725–900730. [Definice populace](hunt-population.json) určuje
jejich role a pevné domovy. Hlad roste přirozeně; lov, krmení a návraty
probíhají automaticky. Modelové požadavky a skupiny zůstávají vypnuté.
[Podrobný postup](../../../tools/realm_lab/HUNT_POPULATION.md) uvádí
nasazení, záznam a ověření celého cyklu. [Záznam aktivace](hunt-activation.json)
potvrzuje šest řízených živých NPC a zdravé lab služby na revizi
`d7319c5387`; dokončené cykly se vyhodnocují samostatně ze záznamu.

Aktivní rozšíření na 100 NPC používá samostatný profil `hunt-100`
a [jeho definice](hunt-population-100.json): 20 predátorů a 80 kořistí,
spawny 900725–900824. Původní šestice si zachovává domovy a identity.
Aktivace vyžaduje samostatný bootstrap při zastaveném lab worldserveru;
průběh a výkonnost se vyhodnocují zvlášť.
[Záznam aktivace stovky](hunt-100-activation.json) potvrzuje živou populaci
20/80 na revizi `9305787193`, zachované původní domovy a zdravé služby.
