# Preview-v11 — taverna, radnice a stanový tábor

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
