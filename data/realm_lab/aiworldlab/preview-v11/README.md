# Preview-v11 — taverna, radnice a stanový tábor

## Aktivní AI World — living-v11

[Řízená populace](living-population.json) zachovává všech 45 současných obyvatel, jejich vlastní šablony, jména, příběhy
a pevné domovy. Změnou je výslovné převzetí běžného řízení pod AI World; LLM controller a provider
zůstávají vypnuté. Původní testovací stovka ani zvěř se nevytvářejí.

| Schopnost | Počet | Sklad |
| --- | ---: | --- |
| Farmáři na třech skutečných polích | 12 | 1 pravá sýpka, 2 horní sýpka, 3 levá sýpka |
| Jižní spotřebitelé | 4 | 4 venkovní sklad, bez pole a bez sýpky |
| Nosiči potravin | 2 | Převoz z vesnic do skladu 4 a města 5 |
| Městští spotřebitelé | 17 | 5 městský sklad |
| Defias s přepadovou schopností | 10 | 6 vlastní táborový sklad |

Skladové body patří k přístupným místům u staveb, nikoli dovnitř jejich neprůchodného modelu.
[Nové ověření míst](living-position-verification.json) pokrývá šest skladů, 36 bodů úrody, všech 45 domovů a stejných 74 souborů
geometrie. Nad 329 kolizními instancemi a všemi zdrojovými vrstvami vody prošlo **87 pozemních
projekcí a 346 úplných cest**. Šestnáct směrovaných tras má navíc **70 ověřených bodů a 144
úplných úseků**; nejdelší úsek měří přibližně 139 yardů. Manifest připíná také otisk z těchto dat
vygenerovaných bodů v serverovém kódu. Zásoby vznikají a mění majitele až dokončenými fyzickými
činnostmi; nosiči nepřidávají potraviny pouhým příchodem. Potřeby, úroda, zásoby a náklad se přes
restart nadále neukládají.

[Doklad farmářské navigace](farmer-navigation-acceptance.json) navazuje na stejnou mapu a stejné domovy. Binár `4235444e3902` prošel 960
Python a 736 nativními testy; osm služeb je zdravých a všech 45 NPC je řízených, načtených a
živých.

Patnáctiminutový záznam má 450 platných snímků bez chyby bilance. Úplný cyklus
sklizeň–dodávka–návrat–odpočinek–další práce je doložený u 12 z 12 farmářů, opakování u 12 z 12. V
tomto záznamu už žádný z farmářů na konci neměl dlouhé navigační čekání.

| Farmář | Dokončené cykly v aktuálním životě | Opakování | Stav na konci |
| --- | ---: | --- | --- |
| 902000 | 6 | ano | návrat a odpočinek doma |
| 902001 | 7 | ano | doprava zásob |
| 902002 | 7 | ano | doprava zásob |
| 902003 | 7 | ano | návrat a odpočinek doma |
| 902004 | 7 | ano | doprava zásob |
| 902005 | 6 | ano | odchod a další práce |
| 902006 | 7 | ano | další sklizeň |
| 902007 | 6 | ano | doprava zásob |
| 902008 | 7 | ano | doprava zásob |
| 902009 | 4 | ano | doprava zásob |
| 902010 | 7 | ano | doprava zásob |
| 902011 | 7 | ano | doprava zásob |

Doklad uvádí úplné počty dokončených cyklů v aktuálním životě, ale pro každého farmáře zveřejňuje
nejvýše první dva a poslední stručný důkaz. Před výběrem se ověřily všechny cykly z aktuálních
životů; úplný záznam zůstává soukromý.

Na konci záznamu činil kumulativní stav od startu serveru 159 získaných porcí = 49 ve skladech +
22 v nákladu + 88 snědených + 0 ztracených (počáteční jídlo 0). První snímek už měl 13 získaných
porcí; během měření přibylo 146. Výchozí stav není započtený jako nově pozorovaný cyklus.

[Ověření skutečné mapy](farmer-navigation-verification.json) pokrývá 169 cest: 51 základních, 40 dřívějších posunů výchozí polohy, 6 posunů
pozdější problémové polohy a 72 rotací pracovních bodů. Pevná mřížka je ukotvená ve světě a při
změně výchozí polohy NPC se neposouvá. Skutečný začátek zůstává nezměněný; i první spojení na
mřížku projde stejnou fyzickou kontrolou a společným rozpočtem. Každý původní roh se zachovává;
vykonání znovu kontroluje podlahu, celé tělo, dlaždice, prostor a nebezpečí. Výběr má limit 5
yardů, skutečně uzemněná cesta 6 yardů a maximálně 17 původních / 64 hustých bodů. Rozpočty 512
uzlů / 2048 hran zůstávají společné. Samotná geometrická kontrola není živým cyklem.

## Další ověření

Farmářské čekání z předchozího měření je v tomto záznamu pokryté opakovanými úplnými cykly.
Spotřebitelé **902013/902014/902015** mají samostatně otevřené ověření návratů; farmářský protokol
jejich úplný cyklus neposuzoval. Ani úspěšné farmářské opakování neznamená fyzické přijetí všech
činností všech 45 NPC.

## Historie předchozího živého měření — 2. listopadu 2026

Předchozí binár `e1965530c424` se sledoval 2. listopadu 19:53–20:06 UTC: 375 platných snímků za
748,065 sekundy. U všech dvanácti farmářů proběhlo opakování, později se však farmář 902011 znovu
zastavil při návratu domů. Záznam proto není úspěšným přijetím opravy. Sledování bylo přerušeno
při zápisu souhrnu; původní rámce zůstaly soukromě zachované a jejich nové vyhodnocení zachovává
tehdejší neúspěšný výsledek.

[Původní aktivace living-v11](living-activation.json) zůstává beze změny: binár `92f61dc23040`, 300 snímků a tehdejší čekání u deseti
NPC. Uvnitř zachovává také první záznam bináru `3d439d28ea96`. Nový záznam po restartu nepřičítá
starší potravu; její ukládání zůstává odložené. [Ještě starší nativní aktivace](activation.json) je rovněž historická.

## Historie nativního osazení residents-v11

Uložená mapa z 2. listopadu 2026 obsahuje přesunutou tavernu a upravenou výšku radnice, další drobné výškové změny a aktualizované díry v terénu. Nový profil **residents-v11** zachovává původních 22 obyvatel a přidává 23 nových NPC. Všechna mají vlastní herní šablony a editovatelné návrhy v Ateliéru.

| Umístění | Počet | Spawn ID | Vlastní entry | Příslušnost |
| --- | ---: | --- | --- | --- |
| 11 obytných domů | 22 | 902000–902021 | 9810000–9810021 | Stormwind, původní NPC |
| Taverna, model 209 | 3 | 902022–902024 | 9810022–9810024 | Stormwind |
| Radnice, model 199 | 10 | 902025–902034 | 9810025–9810034 | Stormwind |
| Tři stany vlevo, modely 223–225 | 10 | 902035–902044 | 9810035–9810044 | Vlastní Defias |

## Mapa a umístění

[Projektový manifest](project-manifest.json) připíná přesně devět autorských souborů. Zdrojový projekt zůstal beze změny a byl znovu zkontrolovaný po potvrzeném uložení a zavření editoru. Oddělený staging přiřazuje oblast 4988. [Změny modelů](model-transform-delta-v10.json) a [změny terénu](source-delta-v10.json) porovnávají verzi v10; jiné mapy se nebalí.

[Serverový manifest](server-manifest.json) a [nezávislé ověření](data-verification.json) pokrývají všech 74 geometrických souborů. Kolize i navigace byly nově vygenerované z totožného klientského patche. [Důkaz umístění NPC](residents-position-verification.json) ověřuje 329 kolizních instancí včetně vybavení budov, skutečné podlahy, volný prostor pro postavy, všechny zdrojové vrstvy vody a **46 pozemních projekcí / 90 úplných cest**. Původních 22 míst včetně natočení se nemění. Ostatní body jsou uvnitř taverny a radnice nebo rozptýlené kolem skutečných tří stanů.

Vstup, start i hřbitov zůstávají `.go xyz 266.667 800 4.209 725`. Starší důkazy v10 zůstávají neměnné a jejich populační loader používá vlastní pevné otisky i po změně aktivního mapového indexu.

## Vlastní frakce Defias

Jen deset stanových NPC používá úplnou samostatnou kopii zdrojového Defias Thuga 38: **FactionTemplate 2300 → Faction.dbc 1201, WorldFaction 2**. Projekt tuto vlastní frakci již obsahuje. [Ověření frakce](residents-factions-verification.json) potvrzuje shodné skutečné DBC klienta (patch-W) a serveru; reakční příznaky jsou přesnou kopií nativní šablony 7. Nejde o přeznačení všech obyvatel ani o změnu sdílené vanilla šablony.

Zdroj 38 má jediný SmartAI skript: případnou bojovou hlášku při napadení. Vlastní klony přebírají i tento skript a textové prostředky. Zbylých 35 NPC používá samostatné klony zdroje 11328. LLM řízení, potřeby, zásoby a denní režimy se tímto osídlením nezapínají. Pozemní cesty jsou geometricky ověřené; důkaz neslibuje autonomně odchozené cykly.

## Nasazení a zachování dat

Během CI běží dosavadní svět. Pod společným nasazovacím zámkem se znovu ověří původní mapa a všech 22 NPC. Vznikne chráněná záloha databází world/characters, soukromé konfigurace, charakterových vstupů a konzistentních Studio návrhů. Teprve poté se zastaví svět, vymění přesně ověřená mapová data a přepne profil na residents-v11. CI/CD doplní chybějící spawny a šablony offline; původní domovy ani návrhy nepřepisuje.

Klient dostane nový patch-4.MPQ; patch-X.MPQ zůstává obsahově shodný. Patche W/Y se zachovávají. Ateliér na portu 9092 doplní 23 návrhů a ponechá původních 22 včetně autorských úprav. Observer na portu 9091 ověřuje všech 45 nativních NPC a deset vlastních příslušností Defias.

## Dokončené nasazení 2. listopadu 2026

[CI/CD otestované revize dd2accc67843](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/41473146338) úspěšně dokončilo nasazení; revize prošla 923 testy Pythonu a 699 testy C++. [Ověření běžícího světa](activation.json) potvrzuje osm zdravých služeb, všech 45 živých nativních NPC, 45 samostatných šablon a 45 návrhů i exportů v Ateliéru. Pouze deset postav u levých stanů má vlastní Defias; původních 22 domovů, celé autorské návrhy, účty a hráčské postavy zůstaly zachované. Opakované doplnění návrhů nevytvořilo žádné duplikáty.

[Instalace klienta](client-install.json) potvrzuje nový patch-4.MPQ, stejný patch-X.MPQ a zachované patche W/Y. Předchozí klientské soubory, geometrie i databáze mají zálohu. Závěrečné ověření pokrývá přítomnost NPC, šablony, frakce a návrhy; neoznačuje autonomní chování ani fyzicky odchozené cykly za přijaté.

Pro prohlídku lze použít `.go creature 902022` (taverna), `.go creature 902025` (radnice) a `.go creature 902035` (levé stany).

## Zachované limity mapy

Druhé vrstvy vody nadále zůstávají v klientských zdrojích: 33 buněk, 933 dalších viditelných čtverců, z toho 97 nad terénem. Nativní exportér převádí první vrstvu; [rozbor](multilayer-water-review.json) popisuje známý rozdíl. Všech 45 míst je ověřeně suchých vůči všem zdrojovým vrstvám. Neautorské krajní vzorky vody se normalizovaly podle [nového plánu](water-edge-normalization-plan.json) a navigace vznikla až poté.

Zdrojový šev má největší rozdíl přibližně 0,003098 yardu. Ověření zachovává oba vlastní autorské okraje, proti nimž kontroluje nativní data s původní tolerancí 0,001; zdroj se nevyhlazuje ani tiše neopravuje.
