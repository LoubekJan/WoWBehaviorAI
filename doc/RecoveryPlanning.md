# Ověření návratů a rozpočtu plánování

Podklad: čtyřhodinový běh `20261002T151521Z-c174ec97`, build `853458f8a780`.
Záznam má 2880 čerstvých vzorků; 35 fyzických blokací bylo ověřeno skutečnými
polohami, 32 pokračovalo až do posledního vzorku. Samotný načtený navigační
tile ani dosažení místního kroku neprokazují návrat do původní domácí oblasti.

## Změny

- Hledání návratu pokračuje od dalšího neotestovaného bodu po vyčerpání
  rozpočtu. Nedostatek výpočetního času není `NO_PATH`, selhání návratu ani
  důvod pro dlouhou zotavovací pauzu. Stejný rozhodovací cyklus se zachová.
- Pokud úplná navmesh cesta neexistuje, lze ověřit celý přímý návrat po
  fyzickém terénu. Každého půl yardu se kontroluje podlaha, výškový skok,
  kolize u nohou i těla, Elwynn, původní domácí hranice a nebezpečí. Pohyb
  smí začít až po ověření celé trasy. Každý krátký provedený úsek se ověří
  znovu z aktuální polohy; neplatný start, stěna, sráz či chybějící tile
  zůstávají důvodem odmítnutí.
- Povrchová cesta má vlastní příznak a diagnostiku. Nepovažuje se za
  kompletní navmesh cestu a nepřepisuje spawn ani skutečný domov.
- Výběr kořisti a hledání potravy uchovávají postup přes odložené pokusy.
  Bezpečnostní kontrola přesunu před spuštěním pohybu zůstává povinná.
- Pravidelná péče o potřeby zachová rozpracovaný návrat. Čas odpočinku se
  nepovažuje za postup hledání a po pauze se znovu kontroluje jeho kontext.
  Potřebné pasení, jídlo či odpočinek mohou začít i při vyčerpaném rozpočtu
  plánování. Změna nebezpečí, domova, fáze světa či možností pohybu zruší
  starou úplnou cestu i neplatnou odpověď lokální AI.

## Automatické kontroly

CI sestavuje core s `BUILD_TESTING=1` a spouští celou sadu testů. Pro už
sestavený vývojový kontejner lze cílené navigační a plánovací regrese spustit:

```bash
make test-recovery-navigation
```

Testy mají ověřit návrat přes chybějící navigační spojení na souvislém terénu,
odmítnutí stěny/srázu/jiného patra, pokračování pozdějších kandidátů přes
vyčerpané rozpočty a zrušení rozpracovaného dotazu při změně jeho kontextu.
Tyto syntetické testy nenahrazují běh nad skutečnými mmaps a vmaps.

## Krátký test po deployi

1. Porovnej `git log -1 --oneline` s verzí běžící binárky:

   ```bash
   docker compose -f compose.yml -f compose.dev.yml exec -T worldserver /build/bin/worldserver --version
   ```

2. V čerstvém Observeru vyhledej spawn `146145`, potom obdobně `146193`,
   `81008` a `146194`. Použij aktuální polohu se zdrojem `live` a ve hře
   `.go xyz X Y Z 0`. Příkaz `.go creature 146145` tě přenese na databázový
   spawn bod; NPC se mezitím mohlo vzdálit. Historické souřadnice ani spawn
   bod proto nezaručují jeho aktuální přítomnost. Po výběru NPC příkazem
   `.aiworld group status` ověř spawn ID. Sleduj pohyb, lov, krmení a dosažení
   původní domácí oblasti.
3. Sleduj několik okolních NPC alespoň pět minut. Začátky chůze mají mít
   různé načasování. Ověř útokem, že obrana/útěk zůstává okamžitá. Tento
   ruční test odděl od dlouhého běhu bez zásahů.
4. Zkontroluj výkonové souhrny:

   ```bash
   docker compose -f compose.yml -f compose.dev.yml logs --since=5m worldserver | grep AIWORLD_UPDATE
   ```

   `planningDeferred` může být nenulové. Podstatný je vývoj
   `needsLateMaxMs`, `needsMaxMs` a `planningMaxMs`; samotný počet odložení
   ani menší počet dotazů neprokazují zlepšení. Původní běh měl medián
   půlminutových maxim zpoždění potřeb 2618 ms a maximum 8368 ms.

## Čtyřhodinový běh

Deploy automaticky zahajuje záznam. `make record-aiworld-status` ukáže jeho
průběh. Po skončení musí být všechny vzorky čerstvé a build v `summary.json`
musí odpovídat nasazené revizi. Výsledky jsou v `behavior-report.md/json`.

Vyhodnocuj fyzické blokace a nedokončené návraty společně s polohami a
životními instancemi NPC. Respawn v domácí poloze není dokončený návrat
předchozího života. Dále porovnej krmení a poklesy hladu; počet AI žádostí
ani `STEP_REACHED` není důkaz získané potravy. Průběžný patnáctiminutový
report vzniká jednou a nevylučuje pozdější chyby. Během tohoto záznamu
neprováděj ruční útoky ani přesuny testovaných NPC.
