# Ověření návratů a rozpočtu plánování

Podklad poslední opravy: čtyřhodinový běh `20261002T222533Z-7c41f470`,
build `0b7b9118ff7c`, bez zásahů hráče. Má 2880 čerstvých vzorků. Oproti
předchozímu běhu stoupl počet fyzických blokací z 35 na 98 a klesl počet
krmení z 6256 na 2978. Medvěd 146194 stál celý běh doma a téměř pořád
odkládal rozhodování. Výpočetní rozpočet spotřebovávala i běžná údržba
potřeb; opakované pořadí NPC pak některým nedalo prostor pro hledání.
Samotný načtený navigační tile ani dosažení místního kroku neprokazují
návrat do původní domácí oblasti.

## Změny

- Čekající NPC mají trvalou frontu. Nejstarší požadavek z právě připravených
  NPC dostane přednost; po skutečné práci se přesune na konec. Odpočinek
  dočasně odebere připravenost a zachová stáří rozpracovaného hledání.
  Zánik NPC nebo ztráta řízení požadavek zruší.
- Rozpočet 2 ms / 16 operací na aktualizaci účtuje pouze těla plánovacích
  dotazů. Jedno NPC má díl 500 µs / 4 operace. Péče o potřeby jej nespotřebuje;
  vnořené kontroly jedné operace se neúčtují dvakrát. Nedělitelná poslední
  operace může časový limit překročit. Neobsloužené potřeby zůstávají splatné
  a při skutečné obsluze dostanou celý uplynulý čas.
- Čekání na výpočet samo nezruší hledání po 30 sekundách. Uchovává se kurzor
  kandidátů i již přijatá odpověď AI; skutečný čas poslední práce se
  nepřepisuje. Změna polohy, domova, nebezpečí, fáze či životní instance
  nadále ruší neplatný kontext. Již odmítnutá kořist se v jednom hledání
  neopakuje jen proto, že popošla; nové rozhodnutí ji může ověřit znovu.
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
Zátěžové regrese navíc obsluhují 1859 NPC při trvalé poptávce, různých
nákladech údržby, odpočinku a změnách členství. Ověřují přístup každého NPC
k práci a zachování uplynulého času potřeb; neprokazují úspěšný lov v terénu.

Observer zapisuje `living_role.planning` i pro NPC doma bez návratové
diagnostiky. Pole `reason` rozlišuje čekání na pořadí (`ADMISSION`) od
vyčerpaného dílu (`WORK_BUDGET`); `stage` označuje hledání lovu, potravy,
návratu či rady AI. `wait_ms`, `query_age_ms`, `no_progress_ms` jsou skutečné
stáří včetně péče; `resets` počítá zahazování rozpracovaných kontextů.

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
   ani menší počet dotazů neprokazují zlepšení. Poslední běh měl medián
   půlminutových maxim zpoždění potřeb přibližně 3017 ms.

## Čtyřhodinový běh

Deploy automaticky zahajuje záznam. `make record-aiworld-status` ukáže jeho
průběh. Po skončení musí být všechny vzorky čerstvé a build v `summary.json`
musí odpovídat nasazené revizi. Výsledky jsou v `behavior-report.md/json`.

Nová kontrola `planning_deferred` označí nejméně pět minut skutečně
nehybného odkládání rozhodnutí i doma. Péče (`REST`/`LOOK`) čas kontroly
pozastaví; pohyb, krmení, jiná skutečná aktivita, boj nebo změna životní
instance ji ukončí. Kontrola `return_duration` už nepřestane měřit návrat
při přechodu do `PLANNING_DEFERRED`. Obě kontroly jsou i v jednorázovém
patnáctiminutovém reportu. Jeho FAIL je důvod k rozboru konkrétního NPC;
PASS ještě nepotvrzuje celý čtyřhodinový běh.

Vyhodnocuj fyzické blokace a nedokončené návraty společně s polohami a
životními instancemi NPC. Respawn v domácí poloze není dokončený návrat
předchozího života. Dále porovnej krmení a poklesy hladu; počet AI žádostí
ani `STEP_REACHED` není důkaz získané potravy. Průběžný patnáctiminutový
report vzniká jednou a nevylučuje pozdější chyby. Během tohoto záznamu
neprováděj ruční útoky ani přesuny testovaných NPC.
