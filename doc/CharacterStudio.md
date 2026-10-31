# Ateliér postav

## Finální tvorba mapy — `authoring-empty`

Pro další tvorbu světa je připravený prázdný profil `authoring-empty`.
Po jeho aktivaci nemá mapa registrovaná NPC ani dostupné cíle pro herní
export. Editor vrací výslovné vysvětlení, že se mapa připravuje. Nový
charakterový návrh, vzhled, příběh a editovatelnou zálohu lze dál vytvořit
a uložit; profil však neumožní stáhnout runtime kartu, vzhled, herní šablonu
ani ZIP pro neexistující postavu. Změna čísla ve starém návrhu zákaz neobejde.

Přechod je dokončený: po ověřené záloze byly odstraněny obě původní
populace (100 aktivních a 100 maskovaných NPC), tři vlastní šablony,
aktivní charakterové vstupy i všechny tři Studio drafts. Dne 31. října
kontrola revize `5437178873e7` potvrdila prázdné návrhy, zdrojové karty a
cíle; osm služeb je zdravých. Mapa, účty a hráčské postavy zůstaly
zachované. Provozní receipt je soukromý pod `runtime/lab/receipts/`.

Pro budoucí osídlení se nejprve musí navrhnout a zaregistrovat nová populace
nad finální mapou. Ateliér spawny ani registraci nevytváří. Níže uvedený
postup importu a rozsah `full-v8` popisují dosavadní implementaci; během
`authoring-empty` je nespouštějte za účelem obnovení starých 100 NPC.

## Editor a dosavadní populační profily

Webový editor na `http://<adresa-serveru>:9092` vytváří vlastní humanoidní NPC
pro klienta 3.3.5a (build 12340). Observer na portu 9091 obsahuje odkaz do editoru.
Předchozí nasazení `664d84687d7a` používalo `full-v8`: všech 100 tehdejších NPC
bylo pod AIWorld a LLM kartu měly Šaša, Matěj a Běla. Tento stav je po
přechodu na prázdnou mapu historický. Editor připravuje
návrh pro existující ověřený spawn; netvoří nové spawn pozice ani nerozšiřuje
populační registraci. Nové instalace podle `.env.example` začínají profilem
`settlement-v8` s vypnutým modelem, a proto nejprve potřebují přípravu
podporovaného profilu podle [provozního návodu](../README_DEV.md).

## Co lze vytvořit

- Deset původních hratelných ras, obě pohlaví, kůži, obličej, účes, barvu vlasů
  a vousy nebo doplňky. Dostupné kombinace odpovídají skutečným lokálním DBC.
- Oblečení a zbraně ze 166 ověřených položek ve 14 pozicích. Výchozí katalog
  obsahuje také vybavení startovních postav, nikoli celou databázi předmětů.
- Jméno, povolání, frakci, domov, hodnoty, cíle, slabosti, vztahy a minulost.
  V profilu `full-v8` se jméno používá i nad postavou a povolání jako podtitul
  její samostatné herní šablony. Frakce, domov a ostatní autorské podklady
  nepřepisují herní faction template, pozici spawnu, combat AI, úkoly ani zásobování.
- Životopis a osobnost pro stávající LLM agenty. Qwen umí navrhnout oba texty
  ze zadaných podkladů; autor návrh upraví a výslovně převezme.

Návrhy se ukládají do SQLite v samostatném Docker volume
`aitc_lab_character-studio-data`. Uložení, import ani generování nic nemění
v běžícím světě. Záloha návrhu a její opětovný import zachovávají editovatelné
podklady; import vytváří nový návrh. Konfliktní zápis ze starého okna editor
odmítne místo přepsání novější verze.

## První použití

1. Otevřete ateliér a zvolte novou postavu. Karty z položky **Načíst postavu
   z projektu** závisejí na skutečně nainstalovaných souborech; po odstranění
   starých karet se původní postavy nenabízejí. Není-li nainstalovaný vlastní
   vzhled, editor začne s výchozím lidským vzhledem, nikoli kopií jejich
   původního creature modelu.
2. Zvolte podobu a výbavu. V části **Charakter** napište motivace a způsob řeči;
   v části **Příběh** vyplňte minulost a vztahy. Návrh AI se automaticky neukládá.
3. Uložte postavu. V části **Export** vyberte ověřené řízené NPC z aktivního
   populačního profilu. V dřívějším profilu `full-v8` byla řízená NPC
   `901000`–`901099`, včetně městského obyvatele `901014`. Pouhé platné číslo
   Spawn ID nestačí: neznámé ID, jiná mapa ani NPC s původní AI v některém
   ze starších profilů nejsou platným cílem.
   Agent ID se rovná Spawn ID; velká uint64 ID se v editoru nepřevádějí na
   nepřesná JavaScript čísla. Návrh i jeho editovatelnou zálohu lze uložit
   bez platného cíle, herní export je však zablokovaný až do ověření.
   V `authoring-empty` je seznam cílů prázdný; pokračujte tvorbou návrhu
   a stáhněte **Zálohu návrhu (.json)**. Následující herní exportní kroky
   platí až pro budoucí ověřenou populaci nebo původní podporovaný profil.
4. V části **Úplný balíček postavy** zvolte **Stáhnout ZIP**. Obsahuje `npc_agents/<spawn>.json`,
   `npc_appearances/<spawn>.json`, v profilu `full-v8` také
   `npc_templates/<spawn>.json`, editovatelný návrh a návod. V exportní části
   je náhled jména a podtitulu ve hře; čísla šablon se odvozují automaticky
   z ověřeného cíle a jsou jen ke čtení.
5. Zkontrolované soubory vložte do odpovídajících `data/` adresářů nasazovaného projektu
   a použijte [řízený offline import](#nasazení-balíčku).
   Samostatná šablona se připravuje offline před spuštěním světa. Nový spawn musí být
   zvlášť registrován jako řízené NPC v použitém populačním profilu.

Existující šestipolový formát charakterové karty zůstává beze změn. Vzhled je
samostatný volitelný soubor; prázdný adresář nepřepisuje původní modely NPC.
Server kontroluje skutečné DBC, shodu identity a mapy i sloty vybavení. Načítá
jen vzhledy existujících NPC zapnutých pro LLM. Současný controller má nadále
svůj původní limit 16 současně registrovaných LLM profilů.

Editor ověřuje cíl proti čtenému, hashově kontrolovanému populačnímu profilu
`consumers-v8`, `raids-v8`, `fields-v8`, `defias-v8` nebo `full-v8`. Před stažením server kontrolu
opakuje, takže ji nelze obejít starým otevřeným formulářem. Nepřebírá sám
běžné NPC do AI řízení, nemění jejich registraci ani nevytváří nové spawny.
Změna karty již obsazeného cíle znamená nahrazení jeho autorské identity
při nasazení; editor na existující jméno upozorní.

Převzetí všech 100 NPC profilem `full-v8` samo nevytváří 100 LLM karet.
AIWorld nadále používá běžné role a LLM řízení se zapíná až nainstalovanou
platnou kartou konkrétního NPC, v rámci uvedeného limitu controlleru.

## Nasazení balíčku

Úplný ZIP v `full-v8` má přesně pět souborů:

| Soubor v ZIP | Úloha | Kam patří při nasazení |
| --- | --- | --- |
| `npc_agents/<spawn>.json` | Šestipolová LLM karta | `data/npc_agents/` |
| `npc_appearances/<spawn>.json` | Parametry těla a výbavy | `data/npc_appearances/` |
| `npc_templates/<spawn>.json` | Předpis vlastní herní šablony | `data/npc_templates/` |
| `character-studio-draft.json` | Editovatelná záloha návrhu | Archiv autora nebo import do editoru |
| `README.txt` | Pokyny k balíčku | Archiv autora |

Přeneste všechny tři runtime JSON stejné postavy do odpovídajících adresářů
konkrétního serverového checkoutu. Soukromé životopisy a vzhledy nemusí být
ve veřejném Gitu; export ani samotný editor je do něj neposílá. Uživatelské
návrhy žijí ve volume studia. Původní karty a návrhy patří při přípravě
prázdného světa do chráněné zálohy, nikoli zpět do aktivních adresářů.
Archivujte také draft: nativní karta neobsahuje všechna
editovatelná autorská pole.

Následující postup je pro původní populace; **nepoužívat při `authoring-empty`**.
Pro již připravený server se správně sestavenou verzí kódu a aktivním
`LAB_AI_PROFILE=full-v8` použijte z kořene checkoutu:

```sh
make backup
make config
make setup-world
make start
```

`make setup-world` pod provozním zámkem nejprve ověří autorské soubory,
zastaví worldserver, dočasně obnoví původní entry ověřených spawnů, připraví
základní populaci a aplikuje vlastní šablony. `make start` svět znovu spustí.
Samotné `make restart-world` nebo znovuspuštění kontejneru vlastní šablonu
nevytvoří. Pro změnu nativního kódu použijte standardní CI/CD postup v
[README_DEV.md](../README_DEV.md); ruční build za běhu sdílených binárních
souborů není součástí tohoto importu.

Restart vymaže současnou krátkou a epizodickou paměť rozhovorů i další
runtime stav potřeb a zásob; jeho perzistence je odložená. Nevymaže uložené
drafty, databázové postavy ani nainstalované šablony. Po nasazení obnovte
editor přes **Ctrl+F5** a **znovu se přihlaste do WoW**. Načtení vlastní
šablony a runtime diagnostiku popisuje [NpcAgents.md](NpcAgents.md#ověřený-stav-a-diagnostika).

Při startu světa se chybný volitelný import izoluje: neplatná nebo neověřená
karta se nepoužije, vadný vzhled ponechá původní creature model. Ostatní
platné postavy se načtou dál. Není-li použitelná žádná karta, vypne se jen
LLM controller; samotný herní svět může naběhnout. Důvod odmítnutí je v logu.
Původní konfigurační kontroly oprávnění a rozsahu AIWorld zůstávají přísné.
Soubor karty ani návrh se při tomto odmítnutí nemaže nebo automaticky nemění.

## Samostatná herní šablona

`full-v8` vytváří ke každé kartě předpis vlastní šablony s `schema_version`,
`spawn_id`, `map_id`, `entry`, `base_entry`, `name` a `title`. Nasazení z něj
vytvoří úplnou kopii původní `world.creature_template`, která přebírá její
herní parametry. Mění vlastní identitu, jméno a podtitul; sdílená původní
šablona se nepřejmenovává. Vzhled a vybavení dál dodává samostatný appearance
soubor. Spawn ID, AI registrace a původní domov zůstávají zachované.

Installer nekopíruje jen předem vybraný seznam parametrů: načte všechny
sloupce skutečného schématu a prostředky vázané přímo na entry, například
výbavu, prodejce, učitele, texty a lokální názvy. Sdílené odkazované zdroje,
například loot ID, gossip menu nebo cesta, zůstávají sdílenými odkazy.
Šablona tedy přebírá funkce výchozího NPC; editor z ní nedělá libovolného
nového obchodníka, učitele nebo zadavatele úkolů a tyto funkce nemá vlastní
formulář pro nastavování.

Vlastnictví a hash původní i výsledné šablony eviduje
`world.ai_character_templates`. Ověření odmítne cizí kolizi, změněný základ,
změněnou kopii, navázané prostředky či přesunutý spawn. Opakovaný import
stejného balíčku je idempotentní. Vypuštění JSON z adresáře není příkaz
k odinstalaci a instalované vlastnictví se tím automaticky nemaže.

V předchozí populaci například obyvatel `901014` se základní šablonou `11328`
dostal soukromou šablonu `9800014`. Jiný obyvatel se stejným základem dostal
jiné číslo, takže změna Šašina jména nepřejmenovala Matěje ani další obyvatele. Při změně
jména téže karty zůstává přidělená identita stejná. Backend odvozuje oba
typy entry z ověřených serverových metadat, nikoli z hodnot dodaných klientem.

Původní entry zůstává zdrojem behaviorální klasifikace přes ověřený alias.
Před vyprázdněním mapy tak přejmenovaná Šaša zachovala roli `CITY_CONSUMER`,
Matěj a Běla roli `WORKER`. Alias se načítá před volitelnými LLM kartami a kontrola instalace
čte databázi nezávisle na právě rozpracovaných souborech. Vadná volitelná
karta nesmaže již instalovanou herní identitu. Při přejmenování však musí
souhlasit jméno v kartě a v předpisu šablony; exportujte a nasaďte celý balíček.

Na API je vlastní předpis dostupný také jako
`GET /api/characters/{id}/template`; úplný ZIP je
`GET /api/characters/{id}/bundle.zip`. Čísla entry se do návrhu ručně
nezadávají. V přehledu světa `entry` označuje původní behaviorální typ,
zatímco `template_entry` označuje skutečnou nainstalovanou šablonu.

Jméno má stávající limit 32 znaků / 80 UTF-8 bajtů. Povolání se pro podtitul
ořízne o okolní mezery, musí být na jednom řádku do 80 UTF-8 bajtů a může
zůstat prázdné. Delší autorské podklady lze uložit do návrhu; export úplného
balíčku vyžaduje platný podtitul. V podporované registrované populaci zůstávají
samostatné exporty původní šestipolové karty a vzhledu dostupné i bez herní
šablony; `authoring-empty` blokuje všechny runtime exporty.

Starší profily `consumers-v8`, `raids-v8`, `fields-v8` a `defias-v8` zachovávají
původní export karty a vzhledu. Jejich ZIP výslovně uvádí, že vlastní herní
šablonu neobsahuje; samostatný export šablony je v nich zablokovaný. Uložení
návrhu žádnou šablonu nevytváří v běžící databázi a samotný editor nemá SQL
přístup ani oprávnění provádět živé změny světa.

## Vzhled ve hře

Server používá původní mechanismus `SMSG_MIRRORIMAGE_DATA` a základní model
rasy. Klient obdrží parametry těla, 11 display ID oděvů a GUID aktuální inkarnace NPC;
zbraně se nastavují přes nativní virtual-item pole. Není potřeba měnit DBC
klienta ani vytvářet mapový patch. Jméno nad NPC bere klient ze samostatné
herní šablony; LLM a jeho chat používají stejné jméno z charakterové karty.
Změny se projeví po řízeném nasazení a novém přihlášení do hry.

Vzhled se aplikuje jednou na danou inkarnaci při navázání agenta a znovu po
smrti či uvolnění NPC. Skutečná transformační, shapeshift nebo clone kouzla
mají přednost. Editor nepodporuje živé přepínání vzhledů na běžících NPC.
Nativní testy ověřují délku a pořadí packetu, GUID i předání vybavení; výsledný
vzhled v konkrétním herním klientu musí ještě potvrdit autor ve hře.

## Přesný místní 3D náhled

Náhled používá původní M2/SKIN geometrii a BLP textury z vlastního klienta.
Záměrně nestahuje současné HD modely, které neodpovídají 3.3.5a. Ukazuje
statickou výchozí pózu, nikoli animace. Umí pleť, obličej, vlasy, ozdoby a
textury základních oděvů. Prostorové helmy, ramena, pláště a zbraně se v této
verzi v prohlížeči nekreslí; jejich data se exportují a používá je herní klient.
Chybějící vrstvy původního klienta označuje jako částečný náhled.

Herní data náhledu nejsou v Gitu. Po přípravě vlastního klienta spusťte:

```powershell
python -m pip install Pillow==12.0.0 mpyq==0.2.5
python tools/character_studio/build_preview_assets.py --client C:/WoWModding/Client-Lab --output runtime/lab/character-models
```

Vygenerovaný adresář se připojuje ke studiu pouze pro čtení. Při jeho prvním
vytvoření restartujte službu, aby mohla připojit `/model-data`. Vzhledy a export
fungují i bez náhledových dat; rozhraní chybějící náhled výslovně označí.
Obnovení katalogu vlastního klienta:

```powershell
python tools/realm_lab/character_appearance.py --help
```

## Docker a přístup

Služba `character-studio` je součástí `compose.yml`, `make start` a nasazovací
pipeline. `CHARACTER_STUDIO_PORT` určuje port, výchozí je 9092. Zdrojové profily,
vzhledy, katalog i herní data jsou připojené pouze pro čtení. Editor nemá
Docker socket ani přístup k herní databázi. Modelový klíč zůstává v `ai-server`;
prohlížeč používá stejný origin a proxy `/api/generate`.

Výchozí přístup je pro důvěryhodnou LAN, stejně jako Observer. Volitelné
`CHARACTER_STUDIO_PASSWORD` zapne HTTP Basic účet `studio`. Pro přístup mimo
LAN použijte HTTPS a ochranu přístupu. Zápisy vždy vyžadují relaci a CSRF token.
Editor vlastní samostatný slot pro jedno generování, timeout 30 sekund a
nevymýšlí náhradní text, pokud model selže. Statický příznak dostupnosti modelu
neznamená provedený test inference.

Zálohujte draftový volume standardním zálohováním Docker dat, nebo stahujte
jednotlivé editovatelné JSON návrhy. Běžný rebuild/restart služby volume zachová.

## Ověření a známá omezení

Předchozí nasazení `664d84687d7a` ověřilo tři vlastní šablony Šaši, Matěje a Běly,
nezměněné původní karty a ostatních 16 obyvatel společného základu `11328`.
Tato historická kontrola potvrdila všechny sloupce kopií a návazné prostředky, zachované domovy,
správná jména, skutečné nové entry v živých GUID a tři aktivní LLM controllery.
Z uložených návrhů prošel download předpisu šablony i úplného pětisouborového
ZIP. Přesné rozsahy CI a živého ověření jsou v
[NpcAgents.md](NpcAgents.md#ověřený-stav-a-diagnostika).

Uvedený důkaz neznamená, že tyto tři postavy zůstávají nainstalované po
požadované čistce. Nový prázdný profil je záměrně nepřebírá a jejich
budoucí obnovení by vyžadovalo nové výslovné rozhodnutí a registraci.

Vzhled v konkrétním WoW klientu a renderovaná stránka nebyly při tomto běhu
vizuálně kontrolované. Funkční HTTP a nativní testy neprokazují zobrazení
každého kusu výbavy ani dlouhodobou kvalitu dialogů. Přímé živé přepínání
vzhledu, tvorba nových spawnů, editace libovolných herních parametrů,
3D animace v náhledu a ukládání nových rozhovorů přes restart zatím chybí.

Pokud export není dostupný, nejprve rozlište neplatný cíl, nepodporovaný
populační profil, nedostupná metadata a neplatný podtitul. Návrh lze dál
uložit a zálohovat. Pokud se po nasazení neukáže nové jméno, ověřte instalaci
celého balíčku, shodu názvu v kartě a šabloně a nové přihlášení klienta.
Nevytvářejte náhradní postavu ani nepřejmenovávejte společný základ `11328`.
