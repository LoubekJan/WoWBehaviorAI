# Ateliér postav

Webový editor na `http://<adresa-serveru>:9092` vytváří vlastní humanoidní NPC
pro klienta 3.3.5a (build 12340). Observer na portu 9091 obsahuje odkaz do editoru.

## Co lze vytvořit

- Deset původních hratelných ras, obě pohlaví, kůži, obličej, účes, barvu vlasů
  a vousy nebo doplňky. Dostupné kombinace odpovídají skutečným lokálním DBC.
- Oblečení a zbraně ze 166 ověřených položek ve 14 pozicích. Výchozí katalog
  obsahuje také vybavení startovních postav, nikoli celou databázi předmětů.
- Jméno, povolání, frakci, domov, hodnoty, cíle, slabosti, vztahy a minulost.
  Frakce a povolání zde slouží jako autorské podklady; nepřepisují herní faction
  template, combat AI, úkoly nebo zásobování.
- Životopis a osobnost pro stávající LLM agenty. Qwen umí navrhnout oba texty
  ze zadaných podkladů; autor návrh upraví a výslovně převezme.

Návrhy se ukládají do SQLite v samostatném Docker volume
`aitc_lab_character-studio-data`. Uložení, import ani generování nic nemění
v běžícím světě. Záloha návrhu a její opětovný import zachovávají editovatelné
podklady; import vytváří nový návrh. Konfliktní zápis ze starého okna editor
odmítne místo přepsání novější verze.

## První použití

1. Otevřete ateliér a zvolte novou postavu, případně načtěte Matěje nebo Bělu
   z položky **Načíst postavu z projektu**. Není-li pro ně nainstalovaný vlastní
   vzhled, editor začne s výchozím lidským vzhledem, nikoli kopií jejich
   původního creature modelu.
2. Zvolte podobu a výbavu. V části **Charakter** napište motivace a způsob řeči;
   v části **Příběh** vyplňte minulost a vztahy. Návrh AI se automaticky neukládá.
3. Uložte postavu. V části **Export** vyberte ověřené řízené NPC z aktivního
   populačního profilu. V profilu `full-v8` jsou řízená všechna současná NPC
   `901000`–`901099`, včetně městského obyvatele `901014`. Pouhé platné číslo
   Spawn ID nestačí: neznámé ID, jiná mapa ani NPC s původní AI v některém
   ze starších profilů nejsou platným cílem.
   Agent ID se rovná Spawn ID; velká uint64 ID se v editoru nepřevádějí na
   nepřesná JavaScript čísla. Návrh i jeho editovatelnou zálohu lze uložit
   bez platného cíle, herní export je však zablokovaný až do ověření.
4. Stáhněte ZIP. Obsahuje `npc_agents/<spawn>.json`,
   `npc_appearances/<spawn>.json`, editovatelný návrh a návod.
5. Zkontrolované soubory vložte do odpovídajících `data/` adresářů projektu,
   zahrňte je do běžného nasazení a restartu worldserveru. Nový spawn musí být
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

Při startu světa se chybný volitelný import izoluje: neplatná nebo neověřená
karta se nepoužije, vadný vzhled ponechá původní creature model. Ostatní
platné postavy se načtou dál. Není-li použitelná žádná karta, vypne se jen
LLM controller; samotný herní svět může naběhnout. Důvod odmítnutí je v logu.
Původní konfigurační kontroly oprávnění a rozsahu AIWorld zůstávají přísné.
Soubor karty ani návrh se při tomto odmítnutí nemaže nebo automaticky nemění.

## Vzhled ve hře

Server používá původní mechanismus `SMSG_MIRRORIMAGE_DATA` a základní model
rasy. Klient obdrží parametry těla, 11 display ID oděvů a původní GUID NPC;
zbraně se nastavují přes nativní virtual-item pole. Není potřeba měnit DBC
klienta ani vytvářet mapový patch. Jméno nad NPC dál patří jeho společné
creature šabloně; vlastní autorské jméno používá LLM chat.

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
