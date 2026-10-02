# Ověření oprav lovu a návratu z 1. 10. 2026

Podklad: běh `20260929T164924Z-d87b329a`, sestavení `918f206f0cbc`.
První půlhodina obsahovala ruční testování včetně vlka 80883. Ze 40
fyzických zaseknutí začalo 36 až po této půlhodině. Pilot 80883 se do
místního zotavení vůbec nedostal; jeho další pozorování samo o sobě
neověří opravu problémových NPC.

## Co se změnilo

- Výběr kořisti i každé obnovení pronásledování používají
  `BuildElwynnHuntPath`: stejné příznaky navigace, podporu strmých polygonů,
  kontrolu skutečného začátku, hranic Elwynnu a zkráceného koncového úseku.
  Kořist s již neplatnou cestou se nepovažuje za dosažitelnou. Pohybující se
  kořist může cestu zneplatnit později; takové přerušení lovu zůstává správné.
- Body v domácím okruhu mohou sledovat výšku souvislého svahu. Původně
  všechny nesly Z spawnu a rozdíl nad tři yardy znamenal odmítnutí. Nový
  náhradní dotaz postupuje nejvýše po půl yardu, kontroluje podporu terénu,
  výškové skoky a kolize. Teprve potom se hledá kompletní navigační cesta.
- Skutečný konec kompletní cesty smí být jinde než navržený bod, pokud je
  stále uvnitř původní domácí oblasti a na ověřené výšce. Platí kontroly
  začátku cesty, celé trasy, nebezpečí a opětovné ověření každého přesunu.
- `home_path_failure` uchovává pokus, který došel nejdál ve validaci.
  Pozdější neplatná výška ho nepřepíše. `home_path_rejected` počítá důvody
  odmítnutí jednotlivých domácích cílů. Poslední ověřené napojení má vlastní
  `continuation_path_type`, `continuation_path_failure` a
  `continuation_path_rejected`. Počítadla patří k poslednímu dotazu,
  nikoli k celému životu NPC.

Změny platí pro stávající role v AIWorld v Elwynnu. Původní nastavení
místního zotavení omezeného na 80883 se nemění.

## Krátké ověření po úspěšném buildu a deployi

Ověř, že běží nové sestavení, nikoli předchozí binárka:

```bash
git log -1 --oneline
docker compose -f compose.yml -f compose.dev.yml exec -T worldserver /build/bin/worldserver --version
```

Ve hře zapni GM a přenes se za skutečným spawnem:

```text
.gm on
.go creature 81008
```

Vyber vlka, ověř `spawnId=81008` ve statusu a spusť:

```text
.aiworld group status
.aiworld group navigation
.aiworld group navigationfrom -9455.033 -456.1523 56.55837
```

Pak totéž u medvěda:

```text
.go creature 146194
```

Vyber medvěda, ověř `spawnId=146194` a spusť:

```text
.aiworld group status
.aiworld group navigation
.aiworld group navigationfrom -9796.085 -566.303 31.77749
```

`navigationfrom` NPC nepřemisťuje. Opakuje dotaz z zaznamenané pozice s
aktuálním domovem a načtenou geometrií. Nový řádek `home corridorPoints=…`
používá přímo produkční návratový plánovač. `failure=NONE` a nenulový
počet bodů potvrzují nalezení koridoru, nikoli dokončený pohyb. U živého
napojení se nově vypisuje také `continuation points=… failure=…`.

Chybějící dlaždice nebo jiný domov znemožňují přímé srovnání. Nález
`NO_COMPLETE_PATH` se nesmí označit za opravený jen proto, že existuje
krátký boční krok. Potřebujeme i pokračování domů a skutečný návrat.

Nech NPC 10–15 minut přirozeně jednat. Sleduj lov, návrat a plynulost.
Při trvajícím zaseknutí zopakuj **živý** `navigation` a status vybraného
NPC. Z kořene repozitáře na serveru ulož výstup:

```bash
grep AIWORLD_NAV_PROBE runtime/logs/Server.log > runtime/navigation-probe-stalls.log
docker compose -f compose.yml -f compose.dev.yml logs --since=5m worldserver | grep AIWORLD_UPDATE
make record-aiworld-status
```

Pokud zůstane `early_navigation=FAIL`, předej průběžný report a tyto
sondy hned. Delší běh stejného selhání není potřeba pro jeho potvrzení.
Pokud krátký test projde, pokračuj čtyřhodinovým během bez zásahů; ruční
část uveď při předání záznamu. Hodnotí se skutečné návraty a fyzická
zaseknutí, nikoli jen přejmenování stavu nebo snížení hladu.

## Automatické ověření a jeho meze

Regresní případy jsou v `tests/game/ChaseAngle.cpp`,
`tests/game/RecoveryAdvice.cpp`, `tests/game/Telemetry.cpp` a
`docker/world-viewer/tests/test_world_viewer.py`. Testují kompletnost
cesty, zkrácení za překážkou, skutečný začátek, svahy oběma směry,
propast, zeď, chybějící podklad, různá patra, domácí mez a přenos diagnostiky.

Tyto testy ověřují rozhodovací kód na řízených geometriích. Nedokazují
průchodnost konkrétních serverových mmap dlaždic ani zánik všech 40
zaseknutí. To musí potvrdit živé sondy a záznam nového sestavení.

Místně prošlo 80 testů chování (46 396 kontrol), 7 testů C++ telemetrie
(104 kontrol) a 78 Python testů přijímače/analyzátoru (1 další přeskočený).
Kontrola C++ syntaxe změněné herní logiky prošla. Kompilace souboru
herních příkazů je místně blokovaná chybějícím
`boost/preprocessor/repetition/repeat.hpp`; úplné sestavení musí potvrdit
CI. Na běžícím serveru tyto změny zatím ověřené nejsou.
