# AI World Lab â€” mapovĂ˝ projekt

PrvnĂ­ vlastnĂ­ mapa: MapID **725**, AreaID **4988**, exploration bit **3618**.
InternĂ­ adresĂˇĹ™ `AIWorldLab`, nĂˇzev `AI World Lab`, `InstanceType=0`
(Noggit zobrazuje `None`), expanze WotLK (2). AktivnĂ­ je jedna dlaĹľdice
`(30, 31)`. AktuĂˇlnĂ­ terĂ©n je rovnĂ˝ v Z 0. VĹˇech 256 ÄŤĂˇstĂ­ ADT mĂˇ oblast
4988 a jednu vrstvu `tileset/emeralddream/dreamrock02.blp`.

## UmĂ­stÄ›nĂ­ a verze

- Klient: `C:\WoWModding\Client-Lab` â€” 3.3.5a build 12340, enUS.
- Editor: `C:\WoWModding\RelWithDebInfo\noggit.exe` â€” Noggit Studio
  3.1446+ `[40bc7ed9]`; jeho SHA-256 je v manifestu.
- PracovnĂ­ projekt: `C:\WoWBehaviorAI\runtime\lab\map-project`.
- KlientskĂ˝ zĂˇklad DBC: `runtime/lab/client-baseline/dbc`.
- SoukromĂ© binĂˇrnĂ­ artefakty: `runtime/lab/map-source-artifacts`.

V Gitu zĹŻstĂˇvĂˇ manifest s hashi a nĂˇstroj pro doplnÄ›nĂ­ oblasti. SamotnĂ˝
klient, plnĂ© DBC a binĂˇrnĂ­ zdroje mapy se uklĂˇdajĂ­ soukromÄ›. PrvnĂ­ lokĂˇlnĂ­
ZIP obsahuje jen soubory uvedenĂ© v `project-manifest.json`; jeho CRC i
seznam poloĹľek byly ovÄ›Ĺ™enĂ©. AktuĂˇlnĂ­ zdrojovĂˇ verze je
`aiworldlab-725-area4988-flat-textured-v2.zip` (71 167 bajtĹŻ, SHA-256
`b8913b91306a1b363e116867eb997234dda42b36c4b878e24ccc3fd20fe4a099`).
HistorickĂˇ netexturovanĂˇ v1 nenĂ­ vstupem aktuĂˇlnĂ­ extrakce.

KlientskĂ˝ patch `patch-4.MPQ` je sestavenĂ˝ a instalovanĂ˝ do
`C:\WoWModding\Client-Lab\Data`. Obsahuje pouze Ĺˇest mapovĂ˝ch/DBC souborĹŻ;
projekt `.noggitproj` do klientskĂ©ho patche nevstupuje. Patch mĂˇ 823 658
bajtĹŻ a SHA-256
`d974f5919762708acc57e83a70baa6d68a09966e978a45832e4924546baa8b14`.
StejnĂˇ verze je v `runtime/lab/client-patches` i ve zdroji extrakce
`/home/voslik/WoWBehaviorAI-lab/runtime/lab/client-source/map725-v2`.

ZĂˇkladnĂ­ DBC pochĂˇzejĂ­ z `Data/enUS/patch-enUS-3.MPQ`; provÄ›Ĺ™enĂ© byly
vĹˇechny archivy naÄŤtenĂ© editorem vÄŤetnÄ› vlastnĂ­ho `patch-W.MPQ`.
PĹŻvodnĂ­ `Map.dbc` mĂˇ 135 zĂˇznamĹŻ, `AreaTable.dbc` 2307. UloĹľenĂ˝ projekt
pĹ™idĂˇvĂˇ mapu 725, oblast 4988 a svÄ›tlo 2539; pĹŻvodnĂ­ zĂˇznamy a jejich
Ĺ™etÄ›zce zĹŻstĂˇvajĂ­ zachovanĂ©.

## DoplnÄ›nĂ­ oblasti

PĹ™ed zĂˇpisem uloĹľit prĂˇci a zavĹ™Ă­t Noggit. Z koĹ™ene repozitĂˇĹ™e:

```powershell
python tools/realm_lab/prepare_map_area.py --prepare
```

NĂˇstroj ovÄ›Ĺ™Ă­ mapu, kolize s klientskĂ˝m zĂˇkladem a jednu aktivnĂ­ dlaĹľdici.
PĹ™idĂˇ venkovnĂ­ neutrĂˇlnĂ­ oblast 4988 a pĹ™iĹ™adĂ­ ji vĹˇem 256 ÄŤĂˇstem ADT.
PĹ™ed zmÄ›nou uchovĂˇ pĹŻvodnĂ­ soubory pod `runtime/lab/map-project-backups`.
VĂ˝sledek zapĂ­Ĺˇe do `runtime/lab/map-area-receipt.json`. DalĹˇĂ­ ovÄ›Ĺ™enĂ­
bez zĂˇpisu:

```powershell
python tools/realm_lab/prepare_map_area.py
```

OvÄ›Ĺ™ovacĂ­ reĹľim odmĂ­tne neĂşplnĂ© pĹ™iĹ™azenĂ­. PĹ™ipravenĂ© soubory se pĹ™i
opakovanĂ©m bÄ›hu nemÄ›nĂ­. Na Windows nĂˇstroj odmĂ­tne zĂˇpis pĹ™i bÄ›ĹľĂ­cĂ­m
procesu `noggit`.

## DokonÄŤenĂ© mapovĂ© vstupy

Projekt a mapa se po doplnÄ›nĂ­ oblasti ĂşspÄ›ĹˇnÄ› znovu otevĹ™ely. UĹľivatel
natĹ™el celou dlaĹľdici a uloĹľil ji; editor byl pĹ™ed balenĂ­m zavĹ™enĂ˝.
Kontrola WDT/ADT potvrzuje jednu dlaĹľdici, 256 ÄŤĂˇstĂ­ s oblastĂ­ 4988 a
jednou texturou, 37 120 vĂ˝ĹˇkovĂ˝ch hodnot Z 0, ĹľĂˇdnou vodu, dĂ­ry ani
objekty. [KontrolnĂ­ body](test-points.json) uvĂˇdÄ›jĂ­ navrĹľenĂ˝ domov,
nĂˇvratovĂ© body Â±40 yardĹŻ a vnitĹ™nĂ­ testovacĂ­ region 200 Ă— 200 yardĹŻ.
Jde o souĹ™adnice odvozenĂ© z geometrie; GPS a pohyb jeĹˇtÄ› nejsou ovÄ›Ĺ™enĂ©.

## DokonÄŤenĂ˝ serverovĂ˝ balĂ­k a navmesh

Extrakce `dbc`/`maps`, `vmap4extractor` a `vmap4assembler` nad ovÄ›Ĺ™enĂ˝m
klientskĂ˝m podkladem v2 na lab hostu uspÄ›ly. PĹŻvodnĂ­ mmap generĂˇtor
vynechĂˇval konstantnĂ­ podlahu uloĹľenou s `MAP_HEIGHT_NO_HEIGHT`. NynĂ­ je
doplnÄ›nĂ˝ explicitnĂ­ opt-in `--includeFlatTerrain true`; vĂ˝chozĂ­ hodnota
zĹŻstĂˇvĂˇ false. OpravenĂ© meze BV stromu odpovĂ­dajĂ­ skuteÄŤnĂ© detailnĂ­
geometrii. Test `mmaps.flat_terrain` zahrnuje plochĂ˝ terĂ©n i regresi
prostorovĂ©ho hledĂˇnĂ­ polygonĹŻ a proĹˇel.

V ÄŤistĂ©m pracovnĂ­m adresĂˇĹ™i byla vygenerovanĂˇ pouze dlaĹľdice mapy 725:

```bash
mmaps_generator 725 --tile 30,31 --includeFlatTerrain true --threads 1 --debugOutput true --silent
```

`725.mmap` mĂˇ 28 bajtĹŻ, `7253130.mmtile` 100 732 bajtĹŻ. Vzniklo takĂ©
pÄ›t debug souborĹŻ. SamostatnĂˇ Detour kontrola promĂ­tla vĹˇech pÄ›t bodĹŻ a
ovÄ›Ĺ™ila **8/8 ĂşplnĂ˝ch tras** domov â†” ÄŤtyĹ™i nĂˇvratovĂ© body. KaĹľdĂˇ mÄ›la
tĹ™i polygony a `DT_SUCCESS`; opakovĂˇnĂ­ nad nasazenĂ˝mi daty takĂ© proĹˇlo.
Tento dĹŻkaz ovÄ›Ĺ™uje navmesh, nikoli hrĂˇÄŤskĂ˝ ÄŤi NPC pohyb.

[ServerovĂ˝ manifest](server-manifest.json) zaznamenĂˇvĂˇ hashe Ĺˇesti
serverovĂ˝ch souborĹŻ i debug vĂ˝stupĹŻ, nĂˇstrojovĂ˝ch binĂˇrek a aplikovanĂ˝ch
zdrojovĂ˝ch zmÄ›n. Extraktory vychĂˇzejĂ­ z
`93ff169538e5eb189b76651217b992876a617524`; mmap generĂˇtor mĂˇ pracovnĂ­
overlay doloĹľenĂ˝ hashi deseti souborĹŻ. Compiler je 11.4.0. ZĂˇkladnĂ­ Git
revize sama nepopisuje tyto zmÄ›ny. ZaznamenanĂ˝ hash vstupnĂ­ho manifestu
oznaÄŤuje jeho stav pĹ™i extrakci; nĂˇslednĂ© stavovĂ© aktualizace tohoto
projektovĂ©ho manifestu jej mohou zmÄ›nit.

VlastnĂ­ vmaps jsou oÄŤekĂˇvanÄ› nepĹ™Ă­tomnĂ©. NulovĂ˝ poÄŤet MDDF/MODF/WDT
modelovĂ˝ch spawnĹŻ znamenĂˇ, Ĺľe ĂşspÄ›ĹˇnĂ˝ assembler pro mapu 725 nevytvoĹ™Ă­
strom. NepĹ™idĂˇvajĂ­ se faleĹˇnĂ© prĂˇzdnĂ© vmaps. Povrch a AreaID jsou v
`maps`; modelovĂ© pĹ™ekĂˇĹľky pro LOS mapa nemĂˇ. Po pĹ™idĂˇnĂ­ modelĹŻ musĂ­
vzniknout skuteÄŤnĂ© vmaps i novĂˇ navigace.

Ĺ est souborĹŻ (tĹ™i DBC, `.map`, `.mmap` a `.mmtile`) je instalovanĂ˝ch do
`runtime/lab/data`; zĂˇloha je v `runtime/lab/data-backups/map725-v2-20261009`.
VĹˇechny ÄŤtyĹ™i lab sluĹľby jsou zdravĂ©. OstatnĂ­ mapovĂˇ data se pĹ™i nasazenĂ­
nemÄ›nila. SoukromĂ˝ balĂ­k
`runtime/lab/map-source-artifacts/aiworldlab-725-flat-textured-v2-server.zip`
mĂˇ 866 216 bajtĹŻ, SHA-256
`6e193c2e5fe9867b034c73dbd99f48bb7e5b16e0ec9b483f4188948201669dd6`;
lokĂˇlnĂ­ i hostitelskĂ© SHA/CRC ovÄ›Ĺ™enĂ­ proĹˇlo.

### OpakovĂˇnĂ­ samostatnĂ˝ch kontrol

V nakonfigurovanĂ©m CMake buildu s `TOOLS=1` a `BUILD_TESTING=ON`:

```bash
cmake --build "$mmap_build_dir" --target mmaps_flat_terrain_test
ctest --test-dir "$mmap_build_dir" -R '^mmaps[.]flat_terrain$' --output-on-failure --no-tests=error
```

`mmap_build_dir` je koĹ™en tohoto buildu. SamostatnĂ˝ verifikĂˇtor se v
build kontejneru zkompiluje proti **Detour z tohoto repozitĂˇĹ™e** a jeho
64bitovĂ˝m polygonovĂ˝m referencĂ­m, pĹ™i mountu zdrojĹŻ do `/workspace`
a odpovĂ­dajĂ­cĂ­m buildu v `/build`:

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

VĂ˝stupnĂ­ adresĂˇĹ™ `runtime/lab/validation` musĂ­ existovat. Exit 0 a
`complete_paths=8` jsou dĹŻkazem tÄ›chto navmesh tras; cizĂ­ Detour knihovna
ÄŤi samotnĂˇ existence `.mmtile` je nenahrazujĂ­.

## DalĹˇĂ­ krok â€” GPS a fyzickĂ˝ pohyb

NĂˇsleduje hrĂˇÄŤskĂˇ kontrola na labu: `.go xyz 266.667 800 2 725`, zruĹˇit
cĂ­l, poÄŤkat na dosednutĂ­ a pouĹľĂ­t `.gps`. OÄŤekĂˇvĂˇ se Map 725, Zone/Area
4988 a stabilnĂ­ podlaha Z 0. Zopakovat GPS a chĹŻzi ve vĹˇech ÄŤtyĹ™ech
nĂˇvratovĂ˝ch bodech. `.gps` ovÄ›Ĺ™uje navmesh jen na Ăşrovni mapy; samotnĂ˝
indikĂˇtor mmap neprokazuje konkrĂ©tnĂ­ dlaĹľdici ani Ăşplnou cestu. HlĂˇĹˇenĂ­
chybÄ›jĂ­cĂ­ch vlastnĂ­ch vmaps je zde oÄŤekĂˇvanĂ© pĹ™i nulovĂ©m poÄŤtu modelĹŻ.
GPS, chĹŻze hrĂˇÄŤe a fyzickĂ˝ pohyb NPC jeĹˇtÄ› nejsou ovÄ›Ĺ™enĂ©. Teprve potom
ovÄ›Ĺ™it fyzickĂ˝ pohyb jednoho NPC bez living rolĂ­. AI zĹŻstĂˇvĂˇ vypnutĂˇ.
