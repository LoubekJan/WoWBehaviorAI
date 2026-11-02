# Preview-v12 — tři doplněné mosty

Revize z 2. listopadu 2026 přidává tři mosty **232, 233 a 234** ze skutečného klientského modelu `STRANGLECHASMBRIDGE.M2`. Všech 109 původních umístění modelů včetně budov a stanů se nemění. Výšky, díry v terénu a zdrojová data vody všech čtyř dlaždic jsou shodné s v11. [Přesná změna zdroje](source-chunk-delta-v11.json), [změny umístění modelů](model-transform-delta-v11.json).

Klientský patch a serverová data vznikly ze stejných devíti uložených souborů. Autorský projekt zůstává beze změny; přiřazení oblasti 4988 probíhá v odděleném stagingu. [Projektový manifest](project-manifest.json), [klientský balík](client-patch.json), [serverový manifest](server-manifest.json) a [nezávislé ověření](data-verification.json) vážou všech **75 serverových souborů** na konkrétní verzi mapy. Kolize obsahují **332 instancí** včetně tří nových mostů.

## Průchodnost mostů

[Zdrojový model](bridge-stock-asset-audit.json) má vlastní kolizní síť s 92 vrcholy a 150 trojúhelníky. [Ověření mostů](bridge-navigation-verification.json) propojuje skutečné umístění ze zdroje s nativními kolizemi a pozemní navigací. Kontroluje mostovku, napojení na břehy a úplné cesty vedené přes most. Záznam rozlišuje kontrolu geometrie a navigace od fyzické chůze hráče nebo živého cyklu NPC.

Prošlo 15 hlavních kotev a 30 cest, navíc 183 bodů po jednom yardu a 360 sousedních cest. Všechny cesty jsou úplné, pozemní a uvnitř mostového koridoru. Navazující skutečné podlahy mají na koncích největší krok 0,321 yardu. Výšková aproximace navigačního rastru se u překryvu břehu a mostovky liší od skutečné kolizní podlahy nejvýše o 0,986 yardu; důkaz používá výslovnou mez 1 yard a samostatně ověřuje fyzickou podporu, sucho a volný prostor.

## Zachovaný řízený svět

Profil **living-v12** používá novou geometrii a zachovává současných **45 NPC**, jejich identity, samostatné šablony, domovy, frakce a role. Dvanáct farmářů, čtyři vesničtí spotřebitelé, dva nosiči, sedmnáct městských spotřebitelů a deset Defias používají stejné souřadnice interakcí a stejný runtime katalog jako living-v11. LLM zůstává vypnuté.

[Ověření domovů](residents-position-verification.json) přepočítává 46 projekcí a 90 cest nad v12. [Ověření interakcí](living-position-verification.json) znovu kontroluje zmrazené body polí, skladů a všech jednotlivých úseků stávajících tras; nevybírá nové polohy ani kratší náhradní trasy. [Populační manifest](living-population.json) váže tyto důkazy na nezměněný kompilovaný katalog. Historické důkazy v11 a výsledky tehdejších farmářských cyklů zůstávají ve svém původním adresáři.

Nasazení zálohuje databáze, konfiguraci, všech 45 Studio návrhů a existující karty i šablony. Nová mapa se vymění až po úspěšném CI pod společným nasazovacím zámkem. Finální kontrola porovnává všechny původní návrhy a 90 zdrojových JSON souborů; seeder doplňuje pouze chybějící návrhy.

Vstup, start nových postav i hřbitov zůstávají na `.go xyz 266.667 800 4.209 725`. Klient dostává nový `patch-4.MPQ`; obsah `patch-X.MPQ` i patche W/Y se nemění. Známý rozdíl druhých vodních vrstev je znovu popsaný v [rozboru](multilayer-water-review.json); zdrojová voda se proti v11 nezměnila.
