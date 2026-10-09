# AI World Lab — uživatelův terén v3

Vstupem je uložený původní projekt `runtime/lab/map-project`, mapa 725,
dlaždice 30,31. V3 zachovává uživatelův terén: výšky −1,244 až 33,989 yardů,
nejvyšší lokální sklon 47,499°, bez vody a děr. Obsahuje dvě umístění
Blacksmith.M2 a dvě PrisonLonghouse_Redridge.WMO. Obě WMO mají skutečné
extrahované kolize; Blacksmith.M2 nemá v ověřeném klientu kolizní mesh.

Profil `hunt-terrain-100` zachovává všech 100 spawn ID, X/Y, orientace,
role a názvy z rovné v2: 20 medvědů a 80 jelenů. U 93 domovů aktualizuje
výšku podle uloženého ADT. Výšky všech 100 domovů byly porovnané i se
skutečným extrahovaným `.map`; nejvyšší rozdíl je 0,000452 yardu.

Navigační verifikátor zkontroloval 104 projekcí a 88 úplných směrových
tras: domov prvního medvěda ↔ čtyři body vzdálené 40 yardů a každý z
20 lovců ↔ dvě nejbližší kořisti do 25 yardů. Nejvyšší odchylka projekce
je 0,816 yardu vodorovně a 0,908 yardu svisle. Tato kontrola prokazuje
navmesh; fyzický lov a návraty na v3 se přijímají až ze samostatného záznamu.

## Vstupy a nasazení

- [Projekt a zdrojové hashe](project-manifest.json).
- [Serverová data, kolizní závislosti a navigační důkaz](server-manifest.json).
- [Populace s novými výškami](hunt-population-terrain-100.json).
- [Vstup hráče a návratové body](test-points.json).
- [Záznam nasazení a živé stovky](activation.json).
- Soukromý zdroj: `runtime/lab/map-source-artifacts/aiworldlab-725-user-terrain-v3.zip`.
- Soukromý serverový balík: `runtime/lab/map-source-artifacts/aiworldlab-725-user-terrain-v3-server.zip`.
- Klientský patch: `runtime/lab/client-patches/map725-v3/patch-4.MPQ`.

V3 byla nasazená 9. 10. 2026 po úspěšném CI na revizi `7db5a1feaa`.
V prvním čerstvém vzorku bylo všech 100 NPC živých a řízených, domovy
odpovídaly nové geometrii a všechny čtyři služby byly zdravé. Původní
realm i authserver zachovaly stejné kontejnery a časy spuštění. CI prošlo
115 realm testů včetně skutečného MySQL, 112 Observer, 74 AI a 478 C++ testů.
Klientský patch je instalovaný v `C:\WoWModding\Client-Lab\Data\patch-4.MPQ`.

Migrace vyžaduje `LAB_AI_PROFILE=disabled`, skutečně zastavený lab
worldserver a soukromou SQL/datovou zálohu. Před zápisem ověřuje celou
původní nebo celou novou populaci; smíšené výšky a cizí entity odmítá.
Mění pouze `world.creature.position_z`, `characters.ai_agents.home_z`
a přepne řízení do Observe. Auth ani původní realm do SQL nevstupují.

```sh
docker compose --env-file deploy/lab/.env -f compose.lab.yml stop worldserver
docker compose --env-file deploy/lab/.env -f compose.lab.yml run --rm --no-deps -T --interactive=false tc-dev \
  python3 tools/realm_lab/manage.py migrate-hunt-terrain-100 /workspace/runtime/lab/data
docker compose --env-file deploy/lab/.env -f compose.lab.yml run --rm --no-deps -T --interactive=false tc-dev \
  python3 tools/realm_lab/manage.py activate-hunt-terrain-100 /workspace/runtime/lab/data
```

Před spuštěním musí být instalovaný celý odpovídající klientský a
serverový balík včetně tří VMAP souborů. Potom se nastaví
`LAB_AI_PROFILE=hunt-terrain-100` a spustí lab worldserver i Observer.
Start znovu ověří hashe dat, přesnou stovku a její schválené domovy.
Pouhé přepnutí na v2 nestačí: návrat vyžaduje obnovení v2 dat a
původních databázových výšek ze zálohy při zastaveném labu.

První herní vstup je `.go xyz 266.667 800 10.86 725`. Po dosednutí
má terén u tohoto bodu výšku přibližně 8,86. Hráčský a nativní NPC test
z v2 se nevydává za potvrzení v3. Rovný referenční profil a jeho důkazy
zůstávají v nadřazeném adresáři; nová měření patří k této v3.
