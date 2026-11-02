# Preview-v12 — tři doplněné mosty

Revize z 2. listopadu 2026 přidává tři mosty **232, 233 a 234** ze skutečného klientského modelu `STRANGLECHASMBRIDGE.M2`. Všech 109 původních umístění modelů včetně budov a stanů se nemění. Výšky, díry v terénu a zdrojová data vody všech čtyř dlaždic jsou shodné s v11. [Přesná změna zdroje](source-chunk-delta-v11.json), [změny umístění modelů](model-transform-delta-v11.json).

Klientský patch a serverová data vznikly ze stejných devíti uložených souborů. Autorský projekt zůstává beze změny; přiřazení oblasti 4988 probíhá v odděleném stagingu. [Projektový manifest](project-manifest.json), [klientský balík](client-patch.json), [serverový manifest](server-manifest.json) a [nezávislé ověření](data-verification.json) vážou všech **75 serverových souborů** na konkrétní verzi mapy. Kolize obsahují **332 instancí** včetně tří nových mostů.

## Průchodnost mostů

[Zdrojový model](bridge-stock-asset-audit.json) má vlastní kolizní síť s 92 vrcholy a 150 trojúhelníky. [Ověření mostů](bridge-navigation-verification.json) propojuje skutečné umístění ze zdroje s nativními kolizemi a pozemní navigací. Kontroluje mostovku, napojení na břehy a úplné cesty vedené přes most. Záznam rozlišuje kontrolu geometrie a navigace od fyzické chůze hráče nebo živého cyklu NPC.

Prošlo 15 hlavních kotev a 30 cest, navíc 183 bodů po jednom yardu a 360 sousedních cest. Všechny cesty jsou úplné, pozemní a uvnitř mostového koridoru. Navazující skutečné podlahy mají na koncích největší krok 0,321 yardu. Výšková aproximace navigačního rastru se u překryvu břehu a mostovky liší od skutečné kolizní podlahy nejvýše o 0,986 yardu; důkaz používá výslovnou mez 1 yard a samostatně ověřuje fyzickou podporu, sucho a volný prostor.

## Zachovaný řízený svět

Profil **living-v12** používá novou geometrii a zachovává současných **45 NPC**, jejich identity, samostatné šablony, domovy, frakce a role. Dvanáct farmářů, čtyři vesničtí spotřebitelé, dva nosiči, sedmnáct městských spotřebitelů a deset Defias používají stejné souřadnice interakcí jako living-v11. Aktivní tranzit mezi sklady vybírá samostatný ověřený katalog v12; původní interakční katalog zůstává beze změny. LLM zůstává vypnuté.

[Ověření domovů](residents-position-verification.json) přepočítává 46 projekcí a 90 cest nad v12. [Ověření interakcí](living-position-verification.json) znovu kontroluje zmrazené body polí, skladů a všech jednotlivých úseků stávajících tras; nevybírá nové polohy ani kratší náhradní trasy. [Populační manifest](living-population.json) zachovává vazbu na původní kompilovaný katalog interakcí a samostatně váže nový katalog tranzitních tras. Historické důkazy v11 a výsledky tehdejších farmářských cyklů zůstávají ve svém původním adresáři.

Nasazení zálohuje databáze, konfiguraci, všech 45 Studio návrhů a existující karty i šablony. Nová mapa se vymění až po úspěšném CI pod společným nasazovacím zámkem. Finální kontrola porovnává všechny původní návrhy a 90 zdrojových JSON souborů; seeder doplňuje pouze chybějící návrhy.

Vstup, start nových postav i hřbitov zůstávají na `.go xyz 266.667 800 4.209 725`. Klient dostává nový `patch-4.MPQ`; obsah `patch-X.MPQ` i patche W/Y se nemění. Známý rozdíl druhých vodních vrstev je znovu popsaný v [rozboru](multilayer-water-review.json); zdrojová voda se proti v11 nezměnila.

## Aktivní tranzitní trasy a živé ověření

Binár `9857ecc094a8` prošel 984 testy Pythonu a 739 nativními testy; osm služeb je zdravých a
všech 45 NPC je řízených, načtených a živých. [Nový provozní doklad](transit-activation.json) navazuje na [30 směrovaných tras](transit-route-verification.json)
mezi šesti nezměněnými sklady. Všech 240 úseků má úplné pozemní nativní spojení i samostatný
fyzický důkaz skutečných podlah, prostoru pro tělo a všech vodních vrstev. Ověřené jsou také celé
spojené trasy po převodu souřadnic na skutečné hodnoty C++ float. Nejvýše 17 kotev a 139,912 yardu
na úsek zůstává pod původními limity.

Jižní spojení používají most 233. Cesta mezi jižním skladem 4 a městem 5 má přibližně 521 yardů.
Mosty 232 a 234 nejsou vynucovanými zastávkami; jejich živé použití se posuzuje jen podle záznamu.
Nativní konektivita a fyzická bezpečnost koridoru jsou oddělené kontroly, pohyb při spuštění znovu
ověřuje kolize a podlahu.

Patnáctiminutový záznam má 450 platných snímků bez chyby bilance; obsahuje 8 fyzicky ověřených
přechodů mostu 233 oběma směry a 4 navazující dodávky po 6 porcích obou nosičů. Opakovaný úplný
pracovní cyklus je doložený u 12 z 12 farmářů. Otevřené zůstávají navigační limity části městských
spotřebitelů; podrobnosti uvádí přehled mapy.

## Otevřené navigační případy

Po přísunu jídla zůstalo bez dalšího fyzického postupu osm městských spotřebitelů: 902023, 902025,
902027, 902028, 902030, 902031, 902032, 902034. U 902023 a 902028 dokládá krátký předchozí záznam
stejnou zastavenou polohu a limit 2048 hran. U dalších šesti už před změnou existovalo čekání a
hlášení překážek v této části města; přesná shodná poloha ani příčina pro ně doložené nejsou.

Na konci má šest z těchto osmi postav stav `NO_FOOD`. Dřívější navigační limit se tím neoznačuje
za pokračující požadavek na novou cestu. Tyto otevřené případy jsou oddělené od ověřených přechodů
mostu a opakovaných farmářských cyklů; záznam není přijetím celé navigace všech 45 NPC.

| Farmář | Úplné cykly v aktuálním životě | Opakování |
| --- | ---: | --- |
| 902000 | 5 | ano |
| 902001 | 8 | ano |
| 902002 | 7 | ano |
| 902003 | 6 | ano |
| 902004 | 6 | ano |
| 902005 | 6 | ano |
| 902006 | 6 | ano |
| 902007 | 5 | ano |
| 902008 | 4 | ano |
| 902009 | 4 | ano |
| 902010 | 5 | ano |
| 902011 | 10 | ano |

Na konci záznamu je kumulativní stav od startu serveru 167 získaných porcí = 45 ve skladech + 13 v
nákladu + 109 snědených + 0 ztracených (počáteční jídlo 0). První snímek už obsahoval 24 získaných
porcí, za měření přibylo 143. Výchozí zásoby nejsou nově pozorované události.

Tento doklad nepřijímá všechny možné budoucí cesty ani všechny činnosti všech 45 NPC. Opakování
farmářů platí pouze pro pozorované životy a konečný záznam; úplné návraty spotřebitelů mají
samostatné otevřené ověření. Účty a hráčské postavy se porovnávají s konkrétním přednasazovacím
stavem a vyjmenovanými sloupci. Potřeby a zásoby se přes restart zatím neukládají.

## Historie prvního nasazení mapy

[CI/CD revize c2d4eb389949](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/41526858975) úspěšně prošlo 973 testy Pythonu a 736 nativními testy a dokončilo nasazení. [Živá kontrola](activation.json) ověřuje všech 75 instalovaných geometrických souborů, osm zdravých služeb a všech 45 živých řízených NPC. Zachované jsou celé původní Studio návrhy včetně revizí, všech 90 karet/šablon, účty a hráčské postavy. Opakovaný seeder vytvořil nula návrhů a zachoval všech 45.

[Klientský patch](client-install.json) je nainstalovaný v `C:\WoWModding\Client-Lab`. Původní patch-4/X, serverová geometrie a databáze mají zálohu. Patche W/Y byly porovnané se skutečným stavem bezprostředně před tímto v12 nasazením; starší instalace v11 zůstává datovanou historií. Geometrické kontroly mostů a zachovaných interakcí jsou nové; fyzické farmářské cykly ani návraty ostatních rolí se tímto nasazením znovu nepřijímají.
