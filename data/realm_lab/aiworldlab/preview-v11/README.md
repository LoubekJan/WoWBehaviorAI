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

[Řízené nasazení](living-activation.json) potvrzuje osm zdravých služeb a všech 45 řízených, načtených a živých NPC.
Binár `92f61dc23040` prošel CI/CD s 960 Python a 718 nativními testy.

Záznam po opravě má 300 platných snímků za přibližně 10 minut bez chyby bilance. Zachytil sklizeň
a dodávku u 8 z 12 farmářů, krádež a dopravu do tábora u všech 10 Defias a převoz u obou nosičů.
Jídlo ze skladu je doložené u 3 z 21 spotřebitelů; konkrétní pozorovaná i nepozorovaná NPC uvádí
provozní doklad. Na konci záznamu je kumulativní stav od startu serveru: 0 počátečních + 74
získaných porcí = 36 ve skladech + 3 v nákladu + 35 snědených + 0 ztracených. První snímek už měl
20 získaných porcí; ty se nepočítají jako události pozorované během záznamu. Dodávka do městského
skladu 5 zatím v tomto záznamu doložená není. Úzké průchody a část návratů domů zůstávají
otevřené; konkrétní případy rozlišuje níže uvedené TODO.

Oprava kombinuje jeden jemnější průzkum po vyčerpání hrubé mřížky a krátký odchodový bod u
vlastního skladu. Oba průzkumy sdílejí limit 512 uzlů / 2048 hran; skutečné podlahy, kolize těla,
hranice mapy a nebezpečí se dál ověřují.

První záznam na bináru `3d439d28ea96` z 18:38–18:48 CET zůstává samostatnou historií: 300 platných
snímků, celé řetězce u 11 farmářů, 7 Defias a jednoho nosiče, 22 snědených porcí. Nezaměňuje se s
novým záznamem po restartu, protože stav potravy se přes restart neukládá. [Původní nativní receipt](activation.json)
popisuje ještě starší osazení.

## TODO — úzké průchody a dokončení návratů domů

Polohově závislé omezení schodišť zůstává otevřené u farmářů **902006/902007** a jižního
spotřebitele **902015**. Kontrola skutečné geometrie s tělem o poloměru přibližně 0,306 a výšce
2,031 yardu našla u všech tří průchod při rozteči 0,625 yardu; při 1,25 yardu jej nenašla. Mapa
tedy nabízí fyzicky průchodnou cestu. Tento diagnostický výsledek není důkazem, že ji běžící NPC
provedlo. Současné provádění vyžaduje krok delší než jeden yard a neumí zachovat všechny krátké
zatáčky na těchto schodech.

Pro tyto tři geometricky ověřené případy je další úkol provést **celou jemnou lomenou cestu** se
zachováním každého potřebného rohu. Nesmí se přeskakovat ohyby, zeslabit kolize těla či podlahové
kontroly, měnit domovy ani obnovovat výchozí rozpočet hledání. Ověření musí fyzicky zachytit
odchod, práci nebo jídlo a návrat domů u všech tří NPC. První sklizeň a dodávka samy návrat domů
neprokazují. [Doklad nasazení a mezí](living-activation.json) uchovává otisk geometrické diagnostiky; žádný záznam zde
neoznačuje všechny činnosti všech 45 NPC za dokončené fyzické přijetí. V závěrečném záznamu je u
902007 zachycen návrat a odpočinek doma. Známý problém schodiště tedy závisí na výchozí poloze;
tato pozorování nepotvrzují všechny varianty průchodu.

Další čekání při návratu domů je v závěrečném provozním posouzení zaznamenané u 902004, 902008,
902009, 902010, 902011, 902013, 902014. Tato místa vyžadují samostatnou diagnostiku; příčina není
geometrickým důkazem tří schodišť potvrzená.

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
