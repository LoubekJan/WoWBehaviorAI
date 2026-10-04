# Ověření návratů a rozpočtu plánování

## Pořadí návratových pokusů a časový limit grafu (4. října)

Běh `20261004T070453Z-981d68ba`, build `2bd012ee6c97`, měl 2880
čerstvých vzorků bez zásahů hráče. Výsledek zůstal `FAIL`: 61 NPC mělo
fyzickou blokaci a 45 dlouhé odkládání plánování. Medvěd 146193 tentokrát
chodil, krmil se a vracel se domů, ale novou objížďku nepoužil. Medvěd
146194 zůstal po lovu nehybný 11700 sekund. Jeho vykázaná podpůrná výška
byla o 2,92021 yardu níže než živé Z; záznam sám nepotvrzuje, která
podlaha je správná. Výpočetní postup proto nelze zaměňovat za pohyb NPC.

- Návrat nejprve využije zachovanou úplnou cestu, zpětnou stopu a krátké
  ověřené kroky směrem domů. Potom zkouší úplnou navmesh/přímou terénní
  cestu a až následně rozsáhlejší povrchový graf. Po něm pokračují místní
  objížďky, rejoin a povolený návrat po již navštíveném úseku. Každý
  pohybový krok nadále musí projít kontrolou ze skutečné aktuální polohy.
- Nedokončené hledání povrchového grafu má nejvýše 30 sekund skutečného
  uplynulého času od první obsluhy své etapy. Čekání na rozpočet i péče
  do limitu patří; nové uzly,
  hrany ani `MarkProgress` jej neobnovují. Při dalším plánovacím pokusu
  po vypršení se zaznamená `SURFACE_DETOUR_TIME_LIMIT` a pokračuje další
  návratová strategie. Samotný limit nezaručuje dosažení domova do 30 sekund.
  Již dokončený důkaz zůstává zachovaný i při delším čekání na kontrolu
  provedení; toto čekání jeho historický výsledek nepřepíše na timeout.
- Vyprší pouze graf daného rozhodnutí. Výsledky levnějších pokusů,
  zbývající kurzory a diagnostika zůstávají zachované; nevzniká nové celé
  hledání při každé aktualizaci. Samostatné čekání na rozpočet stále
  nezneplatňuje `Planning.Resume`. Změna skutečného geometrického kontextu
  nadále zruší neplatnou cestu.
- Úplný důkaz povrchové cesty se předá k provedení jednou. Pokud živá
  kontrola prvního úseku cestu odmítne, tentýž již vydaný důkaz se nesmí
  opakovaně instalovat a blokovat další strategie v tomto rozhodnutí.
  Nové rozhodnutí může cestu ověřit znovu.

Regrese `LivingReturnPipeline` používají stejný etapový koordinátor jako
produkční návrat, skutečný plánovací rozpočet a inkrementální povrchový
graf. Ověřují čtyři operace na jednu sekundovou obsluhu potřeb, pokračování
zachované úplné cesty po odkladu a přechod na další strategii po vypršení
grafu i při pokračujících výpočtech a pětisekundové péči. Samostatně drží
platný dokončený důkaz přes 35sekundové čekání na rozpočet nebo péči;
odmítnutý úsek potom pokračuje další strategií bez opakované instalace
důkazu. Také drží
živou polohu nezměněnou, dokud se ověřený krok výslovně neprovede; vybraný
krok ani změna výpočetního kurzoru nejsou dokončený fyzický návrat.
Adaptéry dotazů mají syntetický terén. Tyto testy neprokazují průchodnost
konkrétního místa ve skutečných Elwynn mmaps/vmaps.

Pohyb při lovu na suché zemi nyní po zkrácení navmesh cesty ověří celý
skutečně prováděný úsek proti fyzické podlaze, po nejvýše půl yardu.
Zachová živý počátek; další výšku odvozuje od předchozí ověřené podlahy
a zahrne legitimní hover offset. Vedle podpory kontroluje tiles, hranici
Elwynnu a kolize těla. Přijatá cesta obsahuje husté výškové body, aby
přerušený pohyb nezůstával na interpolované navmesh výšce nad terénem.
Počátek vzdálený od podpory více než jeden yard, sráz nebo neúplná kontrola
cestu odmítne; NPC se kvůli tomu nepřemístí ani nenapíše nový domov.
Limit je 64 yardů celé 3D cesty a 128 vzorků včetně počátku.
Počet kontrol je omezený, ale skutečný čas dotazů VMAP musí ověřit živý
běh a `AIWORLD_UPDATE`; čas syntetických callbacků jej neprokazuje.
V krátké kontrole po nasazení sleduj zvlášť `needsLateMaxMs`
(opoždění obsluhy potřeb), `needsMaxMs` (náklady obsluhy potřeb)
a `planningMaxMs` (nejdražší plánovací operaci). Spolu se skutečným
pohybem NPC porovnej několik po sobě jdoucích logovacích oken.

Létání tuto pozemní kontrolu obchází jen při skutečném letu. Plavání ji
obchází až po omezeném ověření, že ve vodě leží celý úsek včetně živého
počátku; samotná schopnost plavat nebo mokrý cíl nestačí. Smíšená suchá
a vodní cesta musí projít pozemní kontrolou, jinak je odmítnuta.
Regrese `GroundedHuntPath` ověřují svah, patro nad terénem, přerušení
pohybu, výškové a pracovní limity i mokrý úsek se suchým počátkem.
AIWorld návratový provider a Elwynn lov navíc při skutečném pozemním
pohybu výslovně používají lineární průchod ověřenými body. Schopnost
`CAN_FLY` sama tak neaktivuje CatmullRom interpolaci, která by mohla
přestřelit ověřené rohy nebo podlahu. Toto nastavení platí pouze pro
tyto řízené trasy; skutečný let zachovává své letové provedení.
Počátek lovu i spuštěného návratu se čte z právě běžící spline ve
světových souřadnicích a předává do `CalculatePathFrom`. Běžné XYZ
uložené u NPC mohou při přechodu mezi buňkami mapy dočasně zaostávat za
spline; nejsou proto náhradou za skutečný počátek prováděného úseku.
Výběr zdroje, ověření podpory i spuštění používají stejnou fyzickou
polohu. Úspěšná předběžná kontrola nezastavuje běžící pohyb při každém
přepočtu. Aktivní transport se tímto postupem odmítne; dokončená spline
již používá uloženou světovou polohu NPC.
Záznam již zaseknutého NPC tato prevence sama neopravuje; jeho návrat
musí stále projít kontrolou ze skutečné živé polohy.

Stejnou kontrolu fyzické podlahy používá i `LivingRecoveryPath::Build`
pro běžnou suchou navmesh trasu návratu. Vedle podpory každého hustého
bodu zde hlídá původní domácí poloměr, Elwynn, tiles a celé tělo.
Neplatná podpora vrátí `navigation.failure=UNSAFE_GROUND_PATH`
a konkrétní `navigation.detail=GROUND_PATH_*`. Skutečný konec musí
nadále odpovídat požadovanému bodu do jednoho yardu; uzemnění nesmí
změnit význam přijaté odpovědi AI ani povolit jiný cíl. Samostatné
terénní spojky a vodní přechody zachovávají vlastní kontrolu celé trasy.

## Obejití překážky při návratu (3. října)

Běh `20261003T141818Z-9acccf90`, build `2f8ace9ef4d6`, obsahuje čtyři
hodiny a 2880 čerstvých vzorků. Medvěd 146193 na konci neměnil XYZ 3460
sekund a 146194 po dobu 8940 sekund, ve stejné životní instanci a bez boje.
Hledání pokračovalo; domácí navmesh cesta končila `NO_COMPLETE_PATH` a
přímá terénní alternativa `SURFACE_OBSTACLE`. Místní bezpečná spojka
neprokazovala úplnou cestu domů. Konkrétní překážku musí ještě potvrdit
test skutečného terénu; tento záznam nepotvrzuje její geometrii.

- Po selhání úplné navmesh a přímé terénní cesty se zkouší omezené hledání
  terénních mezibodů kolem překážky. Může nejprve vést od domova. Obsahuje
  nejvýše 512 uzlů, 2048 pokusů o hranu a 480 yardů celkové trasy.
- Každá hrana se ověřuje po nejvýše půl yardu proti skutečné podlaze,
  kolizím těla, tiles, Elwynnu, domácí hranici a nebezpečí. Celá cesta musí
  skončit na fyzicky podporovaném bodě původního domácího pásma. Samotný
  dostupný místní krok se nepovažuje za hotový návrat.
- Každý plánovací díl zpracuje nejvýše jednu hranu či její část, s nejvýše
  osmi vnitřními výškovými vzorky. Ověření cíle v domácím pásmu je samostatný
  omezený dotaz. Rozpočet účtuje skutečnou práci; rozpracovaný graf zůstává
  zachovaný přes čekání i péči a při změně kontextu se zahodí.
- Původní návratové hledání dostává přednost před tvorbou nových možností
  pro lokální AI. Již běžící poradní hledání nebo odpověď se zachová.
  Celý graf se neopakuje pro každý spekulativní poradní či rejoin bod.
- Provedení zachová každý roh. Terénní waypoint se odebere až při skutečné
  vzdálenosti nejvýše 0,5 yardu; spojka zůstane před svou navazující cestou
  do fyzického dosažení. Každý spuštěný úsek se znovu ověří ze živé polohy.
- Diagnostika `return_recovery.home_detour_failure`, `home_detour_nodes`
  a `home_detour_edges` rozlišuje úspěch, vyčerpání hledání a dosažení
  limitu. `NOT_CHECKED` znamená, že graf nebyl potřeba nebo povolen.
  Stejné položky `continuation_detour_*` zachovávají oddělený důkaz
  navazující cesty. Starší záznamy jsou nadále čitelné.

Po nasazení ověř verzi binárky a nech nový čtyřhodinový běh bez ručních
zásahů. U 146193 a 146194 kontroluj změny skutečných XYZ, návrat do
původního domácího pásma ve stejné životní instanci a další krmení.
Porovnej i celou populaci: `physical_stall`, `return_duration`, krmení,
`planning_deferred` a výkonová maxima `AIWORLD_UPDATE`. Úspěšné syntetické
obejití stěny ani `home_detour_failure=NONE` samy nepotvrzují pohyb ve hře.

## Předchozí oprava rozpočtu plánování

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
