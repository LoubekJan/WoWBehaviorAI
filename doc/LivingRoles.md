# Živější role v Elwynnu

Lokální rozšíření z 22. 9. 2026 navazuje na ve hře ověřený vlčí pilot.
Uživatel potvrdil základní chování ve hře. Po následné opravě 22. 9. 2026 výslovně potvrdil také funkční lov Forest Spider a pomoc blízkých Defias. Oba původně neúspěšné scénáře tak mají potvrzený opakovaný herní test.

## Oprava návratů po doplnění navigace — 25. 9. 2026

Po doplnění chybějících dlaždic mapy 0 uživatel doložil načtenou dlaždici
`0004934.mmtile` a cestu pozemního NPC s `Result: true`, `Length: 13`,
`Type: 1`. To potvrzuje skutečný navmesh v testovaném místě; samo o sobě to
ještě neověřuje všechny trasy ani níže popsanou opravu chování.

- Návratový cíl se kontroluje také **po** přepočtu výšky a konce cesty.
  Posun nejvýše jeden yard se zamítne jako `RETURN_ZERO_STEP`; hledání
  pokračuje dalšími kandidáty.
- Po zvolení vlastní stopy NPC nejprve pokračuje po ní. Dosažený bod se
  odstraní podle skutečně vyřešeného cíle, i když se jeho výška změnila.
  Paměť nejvýše 64 poloh během návratu odmítá opakovaný cíl do jednoho yardu
  (`RETURN_REPEATED_STEP`). Nová materializace, dosažení domova nebo nové
  pronásledování/útěk tuto paměť zahodí.
- I pohybující se návrat po minutě umožní základní potřeby: hladová kořist
  může dokončit pastvu, unavené role odpočívat. Kontrola se opakuje nejvýše
  jednou za minutu mezi kroky (`RETURN_NEEDS_BREAK`). Zapamatované nebezpečí
  dále omezuje činnost na sledování okolí. Predátor může při návratu v
  dosavadním dosahu 80 yardů hledat dostupnou kořist; nevytváří se mu jídlo.
- Připojení k blízkému druhovi nepřebije potřebný návrat domů.
- Záznamník navíc hlásí desetiminutový nedokončený návrat i při pohybu
  (`return_duration`) a desetiminutový hlad kořisti v klidu (`prey_hunger`).

Po sestavení a nasazení sleduj zejména spawny **80782, 80992, 81328, 80672,
80406, 80418, 80697 a 80623**. Návraty se mohou skládat z více kroků,
ale nemají donekonečna opakovat stejné cíle. Při dlouhém návratu kořisti
má dokončená pastva snížit hlad. Poté nech nový čtyřhodinový záznam běžet
bez zásahů; GitHub CI deploy jej zapne automaticky.

Lokálně prošlo 39 C++ testů / 3 718 assertions, 21 testů vyhodnocování
chování a 20 testů záznamníku/deploy gate (jeden další test vyžadující
POSIX signály byl ve Windows vynechán). Prošla syntaktická kontrola
`LivingRole.cpp` a obou jednotek zachycování/serializace telemetrie.
Úplný build a herní ověření těchto změn musí proběhnout při nasazení;
lokální prostředí nemá úplnou instalaci Boost pro sestavení serveru.

## Rozsah a zapnutí

`AIWorld.LivingRolesEnabled = 1` je připravené v `deploy/worldserver.conf`.
V distribučním `worldserver.conf.dist` je výchozí hodnota `0`.
Změna začne platit po sestavení a restartu přes běžný postup projektu:

```bash
make build
make restart-world
```

Nový cyklus platí pro již registrovaná, materializovaná a živá NPC s režimem
`AI_WORLD_CONTROLLED`, známou rolí, mapou **0** a zónou **12 (Elwynn Forest)**.
Jejich trvalý spawn musí mít v katalogu účast `FULL_AGENT` nebo
`LIGHTWEIGHT_BACKGROUND`. Nic se tím automaticky nepřidává do AIWorld;
`OBSERVE_ONLY`, `VANILLA_ONLY`, `EXCLUDED`, peti a ovládaná NPC nový cyklus
nedostávají. Ostatní oblasti zůstávají mimo jeho rozsah.

Vlci odpovídající zapnutému pilotu `LivingWolves` pokračují ve svém ověřeném
smečkovém cyklu. Nová logika jim nepřebírá řízení. NPC s připraveným domovem
a pracovištěm zachovávají svůj dosavadní denní režim; nové role u nich řeší
ohrožení a rozšíření zásob přidává produkci k dokončené práci. Úprava z 24. 9.
navíc mezi dokončenými úkony umožňuje jídlo z vlastních zásob a odpočinek.
Ručně nastavené
skupinové činnosti mají v klidu přednost.

## Co jednotlivé role dělají

Tato základní tabulka popisuje ověřený cyklus z 22. 9. Rozdíly při zapnutém
rozšíření z 23. 9. jsou uvedené níže, včetně individuálních prahů útěku.

| Role | Chování v klidu | Reakce na ohrožení |
| --- | --- | --- |
| Predátor | Krátké toulání do 12 yardů od domovského bodu, rozhlížení, odpočinek; hladový hledá skutečně napadnutelnou kořist v okolí. Po úlovku jí a odpočívá. | Brání se, pod 30 % zdraví včetně utíká. |
| Kořist | Pohyb do 6 yardů, pastva/hledání potravy, rozhlížení a odpočinek. | Utíká i při plném zdraví. |
| Stráž | Obchůzka do 8 yardů a rozhlížení. | Brání se a pomáhá blízkým spojencům; utíká při 15 % zdraví nebo méně. |
| Bojovník | Pohyb do 10 yardů, krátká gesta, jídlo a odpočinek. | Brání se a pomáhá blízkým spojencům; pod 30 % zdraví včetně utíká. |
| Civilista | Pohyb do 4 yardů, rozhlížení, gesta hovoru, jídlo a odpočinek. | Utíká. |
| Pracovník | Jako civilista, v pracovní části dne navíc pracovní animace. | Utíká. |
| Cestovatel | Delší místní pohyb do 12 yardů, gesta, jídlo a odpočinek. | Utíká. |
| Služba / obchodník | Rozhlížení, gesta a krátké jídlo u stanoviště. | Utíká a po uklidnění se vrací. |

Prodejci, trenéři, bankéři, hostinští a další služby se běžně netoulají.
Také zadavatelé úkolů zůstávají u domovského bodu. Výjimkou je výslovně
připravená starší rutina s domovem a pracovištěm, která si zachovává prioritu.
Pohyb kontroluje výšku terénu, výhled, úplnou cestu a cílovou zónu.

Pomoc spojenci vyžaduje živý boj, stejnou nenulovou WorldFaction a žádný nepřátelský
vztah mezi NPC, výhled a blízkost. Spojenec je nejvýše 25 yardů od pomocníka,
útočník nejvýše 30 yardů. Vyhledávání běží při výchozí kadenci potřeb přibližně jednou za sekundu.
Noví útočníci volí volné směry kolem cíle ve světových souřadnicích: otočení
hráče na místě jim nepřesune místa za jeho záda. Nejde o fyzické kolize;
při příchodu se jejich cesty mohou křížit.

Obrana trvá nejvýše 30 sekund nebo 30 yardů od místa zahájení; potom následuje
krátký ústup. Zraněný bojovník se vrací k obraně až nad 50 % zdraví, stráž
nad 35 %. Cyklus se váže na konkrétní materializaci NPC a po smrti nebo
nahrazení živého objektu nepřebírá starou činnost.

## Ověření ve hře

1. Vyber **existující NPC v Elwynnu** a použij `.aiworld group status`.
   Nové řádky `AIWorld role` zobrazí roli, stav, řízení, fázi, činnost, hlad,
   cíl a akci. Samotné `.npc add` nevytvoří AIWorld agenta.
2. Pro nový cyklus hledej `control=AI_WORLD_CONTROLLED`, `enabled=true` a
   `status=READY` nebo `ACTIVE`. `CURATED_ROUTINE` znamená prioritu připravené
   denní rutiny, `WOLF_PACK_CYCLE` původní vlčí pilot. `OBSERVE_ONLY`,
   `UNCLASSIFIED`, `PARTICIPATION_EXCLUDED` nebo `OUTSIDE_ELWYNN` vysvětlují,
   proč NPC tento cyklus nemá. Hláška o chybějícím členství ve skupině sama
   o sobě novým samostatným rolím nevadí.
3. Zůstaň poblíž alespoň **1–2 minuty**. Zahájení a pauzy jsou rozložené,
   takže NPC nemají vykonávat tutéž věc každou sekundu současně. Krátká
   činnost trvá 5 sekund, odpočinek 20 sekund, pohyb má limit 20 sekund.
   Mezi akcemi jsou pauzy přibližně 6–15 sekund.
4. Vyzkoušej několik kategorií z tabulky níže. U každé nejdříve potvrď
   skutečné řízení přes diagnostiku; přítomnost šablony v katalogu sama
   nedokazuje, že konkrétní spawn je převzatý do AIWorld.

| Příklad z místního katalogu | Co sledovat |
| --- | --- |
| Forest Spider, entry 30, `PREDATOR` | Pohyb; při hladu alespoň 0.65 lov dostupné napadnutelné kořisti, potom `FEEDING` a odpočinek. |
| Stonetusk Boar, entry 113, `PREY` | Toulání a `GRAZE`; při napadení `FLEEING`, po skončení nebezpečí návrat do okolí domovského bodu. |
| Defias Thug, entry 38, `COMBATANT` | Napadni jednoho ze dvou blízkých Defias se stejnou WorldFaction; druhý se stejnou WorldFaction má přispět k obraně. |
| Stormwind City Guard, entry 68, `GUARD` | Krátká obchůzka a obrana spojence proti skutečnému nepřátelskému útočníkovi. Běžné frakční vztahy zůstávají platné. |
| Eastvale Lumberjack, entry 1975, `WORKER` | Pracovní gesto v pracovní části cyklu, v ostatní době další místní činnosti. |
| Corina Steele, entry 54, `SERVICE` | Zůstává u prodejního místa, rozhlíží se/gestikuluje a nabídka obchodu stále funguje. |
| Pa Maclure, entry 250, připravená rutina | Dál používá svůj existující domov a pracoviště; nový cyklus nesmí nahradit jeho docházku. |
| Diseased Timber Wolf, entry 69, původní smečka | Regrese: společný lov, pomoc napadenému členovi a správné rozestupy při otáčení hráče. |

Hlad predátorů roste o 0.003 za sekundu materializované simulace; z nuly na
hranici lovu je to přibližně **3 minuty 37 sekund**. Zůstaň poblíž, aby oblast
zůstala načtená. Kořist musí být živá, klasifikovaná jako `PREY`, v Elwynnu,
viditelná, dosažitelná a napadnutelná podle běžných pravidel hry. Hráč není
kořist. Řízený predátor nově považuje klasifikovanou neutrální divokou kořist v Elwynnu za
nepřátelskou pro účely lovu. To umožňuje Forest Spider lovit Cow, Deer, Fawn nebo Rabbit. Výslovné přátelství, imunity a ostatní zákazy útoku zůstávají platné. Pravidlo se netýká hráčů, vlastněných/ovládaných zvířat, služeb, neřízených predátorů, původního vlčího pilotu ani NPC mimo Elwynn.

Pracovníci používají stávající **syntetický dvacetiminutový den**, nikoliv
hodiny klienta ani skutečný den/noc. Pracovní úsek je v konfiguraci 400–800
sekund tohoto cyklu. Když dřevorubec právě nepracuje, sleduj i další část
cyklu. Ne každý model podporuje všechny použité animace.

Při boji vyzkoušej otočení na místě, zabití jednoho útočníka, ústup z dosahu
a opětovný příchod. Přeživší nemají obíhat za záda jen kvůli otočení hráče.
Napadení během jídla/odpočinku má činnost přerušit; přerušené jídlo nesnižuje
hlad. Po skončení ohrožení se má obnovit běžná činnost. Zvlášť ověř odchod
z načtené oblasti a návrat, respawn a přechod mimo Elwynn.

Pokud se chování nespouští, zaznamenej výstup `.aiworld group status`,
`entry`, `spawnId` a okolnosti. DEBUG log `ai.world` obsahuje
`AI living role` a případně `AI living role rejected ... reason=...`.

Po opravě prvního runtime testu diagnostika navíc vypisuje poslední místní
průzkum. Čísla jsou ze scanování, ne trvale aktualizovaný soupis:

| Údaj | Význam |
| --- | --- |
| `lastHunt=HUNT_STARTED` | Poslední vyhledání spustilo lov. |
| `NO_PREY` | V okolí nebyla vhodná živá klasifikovaná divoká kořist. |
| `PREY_NOT_ATTACKABLE` | Kořist existuje, ale skutečná pravidla útoku ji odmítají. |
| `NO_REACHABLE_PREY` | Napadnutelná kořist nemá potřebný výhled nebo úplnou cestu. |
| `ACTION_REJECTED` | Navržený lov odmítl validátor nebo engine. |
| `lastAssist=THREAT_FOUND` | Spojenec bojuje s platným cílem pomoci. |
| `NO_ALLIES` | V dosahu/výhledu nejsou způsobilí řízení spojenci stejné WorldFaction. |
| `ALLIES_NOT_IN_COMBAT` | Spojenci jsou poblíž, ale nejsou v boji. |
| `NO_VALID_THREAT` | Spojenec bojuje, ale jeho protivník není platný/napadnutelný/viditelný/v dosahu. |

`nearbyPrey` / `attackablePrey` ukazují počty kořisti a napadnutelné kořisti;
`nearbyAllies` / `alliesInCombat` počty způsobilých spojenců a bojujících z nich.
`NOT_SCANNED` znamená, že toto vyhledání zatím neproběhlo. Pro retest pavouka
použij existující spawn 79883, pro Defias např. 80201 a poblíž stojící 80200.
Při napadení ověř u **dosud nenapadeného pomocníka** přechod do `DEFENDING`.
Na prvním dodaném snímku Defias měl napadený už jen 15.5 % zdraví, takže jeho
správná reakce během trvajícího ohrožení je útěk; zdravý pomocník může dál bojovat.

Příčina první chyby lovu: Forest Spider má FT 22, který je k blízké kořisti
neutrální. Samotná klasifikace `PREDATOR/PREY` nepovolovala útok v enginu.
Nyní `GetReactionTo` u dvojice NPC nabízí úzce omezený ekologický vztah;
raw DBC reakce z obou stran musí být neutrální. Všechna ostatní pravidla
`IsValidAttackTarget` včetně viditelnosti a imunit stále platí. Mimo tyto
podmínky zůstává původní reakce, nevyžaduje se změna DBC nebo SQL.

Příčina první chyby pomoci: Defias Thug používá neutrální FT 7, respektive
jeho klon 2300. Je neutrální také k dalším Thugům, takže stará dodatečná
podmínka `IsFriendlyTo(ally)` blokovala společnou WorldFaction. Pomoc nyní
akceptuje i neutralitu mezi členy stejné WorldFaction, stále odmítá jejich
vzájemné nativní nepřátelství a vyžaduje skutečný boj proti napadnutelnému
útočníkovi. Bere v úvahu i živé combat reference, pokud hlavní threat cíl
chybí. Členství ve skupině není a nebylo podmínkou této pomoci.

## Potvrzení ve hře — 22. 9. 2026

Uživatel po nasazení opravy potvrdil oba navržené opakované testy: Forest
Spider loví kořist a blízcí Defias se přidávají na pomoc napadenému spojenci.
Ostatní základní chování potvrdil už při předchozím testu. Toto potvrzení
se vztahuje k popsaným základním scénářům; samo o sobě nedokládá všechny
hraniční případy, unload/rebind ani měření výkonu celé populace.

## Rozšíření okolí, spolupráce a zásob — 23. 9. 2026

Implementováno; **část herního ověření potvrzena, včetně opraveného lovu a návratu**
(podrobnosti níže).
Vyžaduje oba přepínače:

```ini
AIWorld.LivingRolesEnabled = 1
AIWorld.LivingRoleExtensionsEnabled = 1
```

V `deploy/worldserver.conf` jsou oba zapnuté, v distribuční konfiguraci
vypnuté. Restart je nutný. Vypnutí pouze `LivingRoleExtensionsEnabled`
vrátí základní cyklus rolí; uložené zásoby zůstanou zachované.

| Oblast | Nové skutečné chování | Omezení |
| --- | --- | --- |
| Vnímání nebezpečí | Kořist vidí řízeného predátora ještě před zásahem. Civilisté, pracovníci a služby reagují na blízkého nepřátelského predátora/bojovníka a na ohrožení spojence. | Vlastní výhled, detekce, platné NPC a vzdálenost přibližně 9–14 yardů; okolní boj má dosah o 3 yardy větší. Samotná přítomnost hráče poplach nespouští. |
| Útěk | Hledá místo poblíž spojenecké stráže, bezpečnější bod u domova nebo cestu pryč od hrozby. U stráže volí volný směr, včetně míst již rezervovaných dalšími příchozími. | Cíl musí být nejméně o 6 yardů dál od hrozby. Kontroluje se celá navmesh cesta i Elwynn. Když nenajde bezpečnou cestu, použije dosavadní engine útěk. |
| Poplach a stráže | Nezvířecí NPC uchová místní poplach; stráž pomůže ve skutečném boji, případně dojde prověřit poslední známé místo. | Poplach žije 15 s, obnovuje se nejdříve po 20 s. Stejná nenulová WorldFaction, viditelný oznamovatel do 25 yardů. Prověřování nejvýše 35 yardů od stanoviště; samotný poplach nepovoluje útok. Nejde o text v chatu. |
| Stádo | Kořist se občas přiblíží ke kompatibilnímu sousedovi a sdílí reakci na jeho viditelnou hrozbu. Deer a Fawn se mohou držet spolu. | Ostatní kořist pouze se stejným entry, řízení permanentní členové; sousední domovské body do 18 yardů. Cílové rozestupy přibližně 4 yardy. |
| Dvojice stráží | Místní obchůzky doplňuje přiblížení ke stráži stejné frakce; jeden vedoucí má nejvýše jednoho následovníka. | Místní sousedé, nikoliv nové dálkové hlídky. Pevné služby a zadavatelé úkolů se kvůli družení nestěhují. |
| Krátká paměť | NPC se 60 s po posledním pozorování snaží nejít přes nebezpečné místo. Když cesta domů není bezpečná, vyčkává a rozhlíží se. | Místní paměť konkrétní materializace; po unloadu nebo restartu zaniká. Připravené starší docházkové rutiny tuto novou kontrolu trasy zatím nepoužívají. |
| Rozdíly mezi jedinci | Stabilní opatrnost mění dosah vnímání a prahy ústupu bojovníků. | Odvozena z AgentId, zachová se při restartu. Oproti základním prahům rozdíl až ±6 procentních bodů zdraví; nezvířecí nevojáci a kořist dál utíkají bez boje. Není to učení zkušeností. |
| Výběr kořisti | Predátor porovnává vzdálenost a zdraví kořisti. Forest Spider navíc upřednostňuje Rabbit/Fawn. | Cow/Deer zůstávají platnou kořistí; původní vlčí pilot se nemění. Nejvýše 8 pokusů o cestu při jednom rozhodnutí. |
| Práce a jídlo | Dřevorubec entry 1975 vyrobí 2 Resource, ostatní klasifikovaní pracovníci 6 Food. Dokončené místní jídlo odečte 1 Food, pokud jej NPC má. | Jednou za syntetické pracovní okno, vlastní zásoba do 20. Lokální práce musí trvat nepřerušených 15 s a skončit ve stejném pracovním okně. Připravená rutina přidá produkci ke svému existujícímu dokončení práce a mzdě. |
| Hovor | NPC při gestu otočí hlavu/tělo ke skutečnému vhodnému sousedovi do 6 yardů. Bez partnera se rozhlédne. | Stejná WorldFaction, bez boje, viditelný stojící partner. Text rozhovoru ani trvalé vztahy nevznikají. |

Stáda a dvojice jsou **místní koordinace**, nikoliv nové záznamy v
AgentGroup. Hláška `not currently a member of any group` proto může být
správná i při `HERD_COHESION` nebo `PATROL_COMPANION`.

Zásoby používají stávající databázová pole a verzované zápisy, bez nové
SQL/DBC migrace. Marker pracovního okna se ukládá spolu s výrobkem, takže
načtení NPC znovu ve stejném okně nezpůsobí další výrobu. Přerušení místní
práce útokem, odchodem nebo zánikem aktéra nic nevyrobí. Zásoba nad 20
nastavená jiným systémem se nesnižuje ani nepřeteče.

### Herní test nové vrstvy

1. Sestav a restartuj běžným postupem výše. Vyber existující řízené NPC a
   použij `.aiworld group status`; musí ukazovat `extensions=true`.
   Přidané řádky obsahují `awareness`, `movement`, `caution`, `food`,
   `resource` a případně `local companion spawnId`.
2. `.go creature 79883` přenese k existujícímu Forest Spider. Sleduj také
   okolní Deer/Fawn/Cow; bez tvého zásahu mají při dostatečném přiblížení
   predátora přejít na `PREDATOR_SEEN` a `SEEKING_SAFETY`. Větší vzdálenost
   bez reakce je správná. GM režim můžeš pro pouhé pozorování ponechat zapnutý.
3. `.go creature 80361`: při přímém napadení kořisti s `.gm off` hledej
   `SEEK_SAFETY` a `AWAY_FROM_DANGER` či `HOME_REFUGE`. Bez cesty je přípustné
   `FLEEING`. Viditelný kompatibilní soused může reagovat přes `HERD_ALARM`.
   Po ústupu se nemají okamžitě vracet přes nedávný boj; sleduj
   `REMEMBERED_DANGER` / `WAITING_FOR_SAFETY` a pozdější `RETURN_HOME`.
4. U stráže stejné WorldFaction do 25 yardů napadni živého řízeného civilistu
   **postavou, které to běžné frakční vztahy dovolují** (např. nepřátelská
   postava Hordy proti Stormwind). GM režim vypnutý. Civilista může hledat
   `GUARD_REFUGE`, stráž přejít do `DEFENDING`. Když už nemá platný živý cíl
   boje, může prověřit čerstvý poplach jako `INVESTIGATING` /
   `CHECK_ALLY_ALARM`. Přátelská alianční postava není vhodným útočníkem
   tohoto testu. Samotná zpráva o nebezpečí nesmí spustit útok na neutrálního hráče.
5. Pozoruj 2–3 minuty Deer/Fawn a dvě blízké řízené stráže bez boje.
   Příležitostně očekávej `HERD_COHESION` / `PATROL_COMPANION`, různá místa
   vedle souseda a žádné obíhání při otočení hráče. Nejde o trvalé následování
   každého kroku; běžné místní činnosti a stanoviště mají stále význam.
6. `.go creature 81257`: před prací dřevorubce si zapiš `resource`. Během
   pracovní části syntetického dne nech proběhnout `WORK` alespoň 15 s.
   Očekávej nárůst o 2, nejvýše na 20; další práce ve stejném okně už nic
   nepřidá. U pracovníka farmy ověř obdobně Food +6 a po dokončeném místním
   `EAT` pokles o 1. U Pa Maclure zůstává rozhodující jeho připravená rutina.
   Zvlášť přeruš práci před dokončením a ověř, že se zásoba nezmění.
7. Odejdi z načtené oblasti a vrať se, případně restartuj server. Zásoby
   zůstanou, rozběhnutý útěk a místní poplach se nepřevezmou ze starého
   objektu. Opakuj regresi vlků a Defias, dostupnost obchodu a test mimo
   Elwynn. Pro porovnání vypni pouze nový přepínač a restartuj.

Pracovní interval je nadále 400.–800. sekunda dvacetiminutového syntetického
dne. Jídlo, odpočinek i nouze mohou práci odložit. `.npc add` stále
nenahrazuje registraci trvalého spawnu do AIWorld.

### Zpětná vazba a oprava pronásledování — 23. 9. 2026

Uživatel potvrdil, že dřevorubec při `WORK` získal **2 Resource**; snímek
s `REST` a nulovou zásobou nebyl důkazem poruchy výroby. Po zásahu civilisty
se rozeběhly stráže. Stádo hodnotil jako dobré a dvojice stráží předběžně
také. U prasete snímek potvrzuje rozhodnutí `SEEKING_SAFETY` /
`AWAY_FROM_DANGER`, ale kvalitu skutečné únikové trasy zatím nepotvrdil.

Forest Spider podle uživatele **celou dobu stojí**. Na snímku měl hlad
0,91, dvě napadnutelné kořisti a `lastHunt=HUNT_STARTED`, přitom už byl
`IDLE` bez akce. Starý údaj uchovával poslední zahájení, nikoli skutečný
výsledek lovu. Nelze z něj určit příčinu zastavení.

Lokální oprava sjednocuje přístup ke kořisti: samostatný `PredatorHunt`
kontroloval cestu přímo k cíli, ale executor dosud pronásledoval pevné místo
vedle něj. Nyní používá přímý engine chase do dosahu pro úder. Rozestupy
při obraně a původním skupinovém lovu zůstávají. Neprůchodná cesta, ztracený
victim nebo chybějící chase už neponechávají lov zdánlivě aktivní po celý
45sekundový limit. Predátor uvolní vlastní útok, po 2 s smí rozhodnout
znovu a kořist s neprůchodnou cestou 30 s vynechá. Root/stun tuto výjimku
nezpůsobuje. Uživatel následně potvrdil skutečné rozběhnutí pavouka za
kořistí. Kořist však unikla; navazující vyvážení rychlosti je popsané níže.

`.aiworld group status` nyní ukazuje také `lastEnd`, poslední dostupný
`preySpawn` a jeho aktuální `preyDistance`, vzdálenost od domova,
`inCombat`, `moving`, `movementBlocked`, `cannotReach`, `evading` a
`decisionWaitMs`. `preySpawn=0` / `preyDistance=-1` znamenají, že poslední
kořist už nelze načíst nebo žádný lov ještě nezačal. `lastEnd` zůstává
výsledkem předchozího ukončení i při novém scanu/lovu; řádek `phase` a
`lastHunt` popisují aktuální pokus nebo poslední scan. Čekání na rozhodnutí
se může překrývat s činností, bojem či návratem, není to slib nového lovu.

Opakovaný test po sestavení a restartu:

1. `.go creature 79883`, vybrat původního Forest Spider. Ověřit
   `AI_WORLD_CONTROLLED`, `enabled=true`, `extensions=true`, zdraví nad
   30 % a hlad alespoň 0,65. Pro pozorování není nutné vypínat GM režim.
2. Ponechat živou Cow/Deer/Fawn/Rabbit poblíž, nezasahovat a sledovat
   přibližně 60–90 s. Při lovu čekat `HUNTING` / `HUNT_PURSUING`, pohyb
   ke kořisti a `HUNT_IN_MELEE_RANGE` s údery po doběhnutí. Kořist může
   uniknout; samotný útěk není porucha lovu.
3. Po úspěšném lovu čekat `FEEDING`, pak `lastEnd=FED`, nižší hlad a
   odpočinek. Při trvalém stání zachytit celý výpis příkazu ihned a znovu
   po 5–10 s. `HUNT_PATH_BLOCKED`, `HUNT_CHASE_MISSING`, `HUNT_VICTIM_LOST`
   a `HUNT_EVADE` rozliší engine příčiny; `PREY_OUT_OF_RANGE`,
   `PREY_LOST_LOS`, `HUNT_LEASH` a `HUNT_TIMEOUT` důvody ukončení v plánovači.
4. Krátce zopakovat obranu více Defias nebo vlků a otočení hráče; zachovat
   rozestupy bez obíhání za záda. Produkce pracovníků se touto opravou nemění.

### Sprint při lovu a vzdálenost pronásledování — 23. 9. 2026

Po předchozí opravě uživatel **potvrdil rozběhnutí Forest Spider za kořistí**.
Kořist před ním utíkala a podle pozorování běžela stejně rychle. Nový snímek
ukazuje `lastEnd=HUNT_LEASH`, kořist Cow spawn 79880 ve vzdálenosti 29,5 yardu
a predátora 35,1 yardu od domova. Jde o ukončení kvůli dosavadnímu limitu
30 yardů od místa zahájení lovu. Snímek neobsahuje měření rychlostí.

Lokální navazující úprava dává samostatně lovícím predátorům krátký běžecký
sprint **+35 % na prvních 10 s aktivního pronásledování** a dovoluje lov
do **60 yardů od jeho začátku**. Dosah ke kořisti zůstává 30 yardů, celkový
čas 45 s a účast je stále omezena na řízená permanentní NPC v Elwynnu.
Kořist nemusí být pokaždé ulovena: rychlejší či vzdálenější zvíře a únik
za hranici lovu zůstávají možné. Návrat k domovu používá původní omezené kroky.

Bonus žije pouze v pohybovém generátoru tohoto `PredatorHunt`. Nemění
rychlost šablony ani `Unit` a nepřechází do obrany, útěku, krmení či dalšího
pohybu. Vypršení přepočte i běžící cestu ke stojícímu cíli. Změna rychlosti
aurou se zohledňuje přes aktuální rychlost jednotky; root/stun pohyb nadále
blokuje. Bonus se neuplatňuje při chůzi, plavání ani letu označeném
příslušným pohybovým příznakem. Ostatní chase včetně vlčího pilotu mají
výchozí násobek 1. Tato oprava samotného lovu platí i při vypnuté volitelné
vrstvě `LivingRoleExtensionsEnabled`.

Nový řádek `.aiworld group status` obsahuje rychlosti v yardech za sekundu:
`runSpeed` / `preyRunSpeed` jsou běžné aktuální rychlosti běhu a `moveSpeed` /
`preyMoveSpeed` rychlosti právě běžících cest (u stojícího NPC nula).
`sprint` je násobek přidělený současnému lovu, `sprintRemainingMs` jeho
zbývající čas; mimo lov mají hodnoty 1 a 0. Samotný násobek nedokazuje
pohyb — sleduj zároveň fázi, `moving` a `moveSpeed`.

Po `make build` a `make restart-world` zopakuj test u `.go creature 79883`:

1. Vyber původního řízeného pavouka, ponech živou kořist poblíž a počkej na
   hlad alespoň 0,65. Při začátku lovu očekávej `sprint=1.35`, odpočítávání
   z 10 000 ms a při běhu vyšší `moveSpeed` než `runSpeed`.
2. Při podobné základní rychlosti kořisti se má odstup zmenšovat. Ověř údery,
   případný úlovek, následné krmení a snížení hladu. Uživatel následně potvrdil
   fungující lov u původního pavouka; problém návratu jiného spawnu je níže.
3. Při delším lovu ověř po 10 s násobek 1 a běžnou rychlost. Při útoku
   hráče během lovu ověř přechod na obranu/útěk bez přenosu sprintu.
4. Při dalším neúspěchu zachyť celý výpis během běhu i po jeho konci.
   `HUNT_LEASH` zůstává očekávané pro příliš dlouhý únik; rychlosti a vzdálenosti
   rozliší pomalejšího lovce od překážky či neplatné kořisti.

### Návrat po neúspěšném lovu — 23. 9. 2026

Uživatel potvrdil funkční lov u původně testovaného pavouka. Další Forest
Spider **spawn 80700** dvakrát zasáhl Sheep **spawn 80366**, pak ji ztratil
a podle upřesnění **stál déle než 30 s**. Výpis ukazuje `HUNT_LEASH`,
`homeDistance=65.1`, `inCombat=false`, `cannotReach=false` a obě běžné
rychlosti 6 yardů/s. Sprint už skončil. Samotný únik kořisti je možný;
po něm má predátor po pauze pokračovat návratem do svého okolí.

Lokálně se podařilo reprodukovat chybu návratu: původní projekce přesně
30yardového kroku se na souřadnicích Elwynnu zaokrouhlí například na
30,000099 yardu. Následná kontrola `> 30` ho odmítne a při dalším rozhodnutí
spočte tentýž bod. Regresní test používá souřadnice domova spawnu 80700
a kontroluje opravený návrat ve 360 směrech. Přesná pozice pavouka při selhání z dodaného snímku
není známá; reprodukce prokazuje chybu kódu, nikoli její konkrétní výskyt
na serveru.

Oprava vybírá návratové body z vypočtené cesty, nejvýše **28 yardů po trase**.
Když nejvzdálenější bod neprojde, zkusí bližší body (14, 7, 3,5 a 1,75 yardu).
Každý krok stále musí projít vlastní kontrolou výšky, zóny, viditelnosti,
úplné cesty a případně paměti nebezpečí. Když celý domovský bod nelze
vyřešit jedním výpočtem, například po dalekém útěku, zkusí obdobně kratší
krok směrem domů s kompletní kontrolou tohoto kroku. Nejde o teleport ani
vynucený průchod terénem. Mezi navazujícími návratovými kroky čeká 1 s
místo 6 s; úvodní pauza po ukončení lovu zůstává.

Diagnostika používá existující `movement`: při návratu `RETURN_HOME`,
při neúspěšném hledání `RETURN_NO_PATH` nebo `RETURN_STEP_BLOCKED`, při
zamítnutí akce `RETURN_MOVE_REJECTED`. Díky tomu čekání na neúspěšný další
pokus neukazuje jen `NONE`. Oprava se vztahuje na místní návraty všech
řízených rolí v Elwynnu; připravené docházkové rutiny a vlčí pilot mají
svůj původní cyklus.

**Potvrzení ve hře — 24. 9. 2026:** Uživatel potvrdil úspěšný opakovaný
test opravy návratu po neúspěšném lovu. Hlášené trvalé stání pavouka je
pro tento scénář vyřešené. Potvrzení se vztahuje k poslednímu testu návratu;
neprokazuje všechny překážky, unload/rebind ani chování celé populace.

Opakovaný test po sestavení a restartu:

1. `.go creature 80700`, vybrat existujícího pavouka. Nechat proběhnout lov;
   při úniku kořisti nebo `HUNT_LEASH` zůstat poblíž alespoň minutu.
2. Po skončení `decisionWaitMs` očekávat `phase=MOVING`,
   `movement=RETURN_HOME` a postupně klesající `homeDistance`. Při obcházení
   překážky nemusí vzdálenost k domovu klesnout při každém jednotlivém kroku.
3. Po návratu očekávat další místní činnosti nebo nový lov při dostatečném
   hladu a dostupné kořisti. Starý `lastEnd=HUNT_LEASH` sám o sobě nevypovídá
   o aktuálním pohybu.
4. Pokud dál stojí, zachytit celý status po 15 a 30 s. Nový údaj `movement`
   rozliší čekání, nedostupnou návratovou cestu a zamítnutý pohyb.
5. Krátce zopakovat lov spawnu 79883 a návrat prasete po útěku.

## Úpravy podle čtyřhodinového záznamu — 24. 9. 2026

Historický popis první opravy; návratové odbočky a produkci jídla dále
upravuje změna z 25. 9. popsaná níže.

Záznam bez zásahů hráče zachytil 81 NPC s návratovým problémem nejméně
minutu, z toho 62 déle než hodinu. Dále ukázal hlad Pa Maclure navzdory
zásobám a útěk Expeditionary Priest mimo Elwynn. Následující změny jsou
ověřené lokálními testy; jejich účinek na skutečné navmeshi musí potvrdit
nový herní záznam.

| Oblast | Nové chování |
| --- | --- |
| Zablokovaný návrat | Vedle kratších bodů původní trasy zkusí nejvýše osm jejích blízkých bodů a při opakovaném neúspěchu čtyři krátké šikmé kroky směrem k domovu. Každý krok stále vyžaduje platnou výšku, výhled a úplnou cestu v Elwynnu. |
| Čekání na další pokus | Minimální odstup po selhání roste 5, 10, 20, 40 až na 60 sekund. Mezitím může kořist spásat, ostatní odpočívat nebo se rozhlížet; civilní role také jíst. Zapamatované nebezpečí dovolí jen rozhlížení. Animace může další pokus ještě odložit. |
| Pohyb bez postupu | Dokončený nebo vypršený návratový krok s posunem nejvýše jeden yard spustí stejnou obnovu jako odmítnutá cesta. |
| Hladový predátor | Při zapnutém rozšíření a bez nalezené kořisti hledá na postupných místech 24, 44 a 64 yardů od domova. Jeden krok má nejvýše 20 yardů a jeho cesta zůstává do 80 yardů od domova. Po 120 sekundách nezačíná další hledací kroky; následuje návrat a nejméně minutová přestávka. Rozpracovaný pohyb či lov může doběhnout. |
| Lov během hledání | Predátor zkoumá skutečné místní okolí 25 yardů. Pořád platí napadnutelnost kořisti, výhled, cesta, původní sprint a limit pronásledování od místa zahájení lovu. Okruh 80 yardů omezuje hledací přesuny, nikoli celý následný chase. Pilot smečkových vlků se nemění. |
| Útěk | Místní úkryt zkouší i kratší kroky. Nouzový flee generátor volaný rolí nově kontroluje celou plánovanou trasu po nejvýše jednom yardu a odmítá neúplnou cestu i výstup ze zóny 12. Tato hranice se nepřidává běžnému fear pohybu ostatních NPC/hráčů. |
| Připravené rutiny | Při hladu alespoň 0,65 a Food > 0 se NPC mezi úkony nají. Po dokončených pěti sekundách spotřebuje jednu Food a sníží hlad. Při přerušení nebo ztrátě zásoby efekt nenastane. Při únavě alespoň 0,8 se vhodné role na 20 sekund zastaví k odpočinku. Probíhající přesun se nepřerušuje. |

Pokusy i plánování cest jsou omezené; nové hledání nepoužívá globální seznam
kořisti ani teleport. Pokud cesta opravdu neexistuje, náhradní činnost sama
návrat nezaručuje. Oprava útěku není obecné omezení všech externích pohybů
(například odhození, fear nebo samostatného bojového chase).

Ve stávajícím `.aiworld group status` a v poli `movement_purpose` záznamu
nově uvidíš `FORAGE_SEARCH`, `BOUNDED_ESCAPE` nebo konkrétní návratový důvod:
`RETURN_HEIGHT_INVALID`, `RETURN_OUTSIDE_ZONE`, `RETURN_LOS_BLOCKED`,
`RETURN_NO_PATH`, `RETURN_PATH_BOUNDS`, `RETURN_DANGER_BLOCKED`,
`RETURN_INVALID_STEP`, `RETURN_MOVE_REJECTED`, `RETURN_NO_PROGRESS`.
Důvod popisuje poslední odmítnutý kandidát, ne automaticky celou navmesh.
Při náhradní činnosti může být současně `phase=ACTING` a důvod návratu
v `movement`; po dokončení animace se `movement` běžně vyčistí.
`lastHunt=FORAGE_NO_PATH` znamená, že neprošly tři aktuální hledací cíle.

### Opakovaný herní test

Po přenosu všech změněných souborů včetně dvou nových hlaviček do serverového
checkoutu spusť `make build` a až po jeho úspěchu `make restart-world`.
Použij `AIWorld.LivingRolesEnabled = 1` a `AIWorld.LivingRoleExtensionsEnabled = 1`
(obě jsou připravené v `deploy/worldserver.conf`). Observer ani databáze
nepotřebují novou verzi schématu.

1. **Návraty:** sleduj původní spawny přes `.go creature 80418`, dále
   `80697`, `80623`, `80872` a `80907`. Vyber NPC a opakuj
   `.aiworld group status` po útěku či neúspěšném lovu. Očekávej návrat po
   krocích, nebo přesný důvod a střídání náhradních činností s dalšími pokusy.
   U kořisti má pastva snižovat hlad i při neprůchodné návratové cestě.
   Samotný restart nemusí znovu vytvořit původní zablokovanou polohu.
2. **Hledání:** hladového pavouka bez blízké kořisti sleduj několik minut.
   `movement=FORAGE_SEARCH` musí odpovídat skutečnému přesunu. Po hledání
   bez úlovku očekávej návrat; při nalezení kořisti obvyklý lov a krmení.
   Úlovek není zaručený. Zopakuj také dosud funkční spawny `79883` a `80700`.
3. **Zásoby a únava:** u `.go creature 80683` (Pa Maclure) porovnej Food
   a hlad před a po dokončeném `activity=EAT`. Food má klesnout o jednu,
   hlad na nulu (následující tick jej opět mírně zvýší). Po 20 sekundách
   `activity=REST` ověř pokles únavy v Observeru. Při útoku během jídla
   nemá přerušené jídlo spotřebovat zásobu ani snížit hlad.
4. **Hranice útěku:** u `.go creature 54003` sleduj útěk před hrozbou
   poblíž hranice Elwynnu. U role řízeného úkrytu nebo `BOUNDED_ESCAPE`
   nesmí plánovaná útěková trasa vyjet ze zóny 12. Po odeznění hrozby
   očekávej další činnost/návrat. Při problému zachyť stav i souřadnice.
5. **Regrese:** s `.gm off` krátce ověř pomoc sousedního Defias a stráží
   při napadení, rozmístění kolem hráče a reakci na otočení. Ruční testy
   odděl od dlouhého běhu bez zásahů.
6. **Dlouhý běh:** následně spusť `make record-aiworld` a po minutě
   `make record-aiworld-status`. Nech svět opět čtyři hodiny bez zásahů.
   Pošli celou novou session podle [návodu záznamníku](ObserverRecording.md).
   Porovnáme dlouhé nehybné úseky i přes nové důvody, návraty, hlad predátorů,
   spotřebu Food u 80683, stav 54003 a dostupnost kořisti. Vyšší počet lovů
   může změnit její populaci; samotný počet zahájených lovů není úspěšnost.

Strop Resource/Food 20 a výroba dřevorubců zůstávají dosavadní; doprava
surovin a společné sklady nejsou součástí této opravy.

## Úpravy podle druhého záznamu — 25. 9. 2026

Druhý čtyřhodinový běh bez zásahů obsahoval 2 880 čerstvých vzorků. Automatický
test zjistil 330 NPC se zablokovaným návratem; 315 případů trvalo nejméně hodinu.
Samostatná kontrola jejich nejdelších epizod potvrdila nulový posun. Pa Maclure
už jedl, ale pozorovaná spotřeba 62 Food převýšila doplnění 48 Food.

| Oblast | Oprava |
| --- | --- |
| Cesta kolem překážky | Úplná navmesh cesta může vést za roh bez přímého výhledu na konec kroku. Přímý výhled dál vyžaduje náhradní cesta bez navmeshe. Neúplné cesty a průchody přes překážky se odmítají. |
| Správné podlaží | Body navmeshe a skutečně navštívené body si ponechávají svou výšku. Nové volné cíle hledají blízký povrch; konec výsledné cesty musí souhlasit s požadovaným místem. |
| Návrat po vlastní trase | NPC si pamatuje nejvýše 64 skutečně navštívených bodů. Když běžný návrat selže, postupuje zpět po těchto bodech; každou cestu znovu ověří. Historie se nepřenáší přes novou materializaci. |
| Obchůzky | Další pokusy zkoušejí osm různých směrů v měnící se vzdálenosti 3–12 yardů, včetně krátkého kroku od domova. Celá obchůzka musí zůstat v Elwynnu a v pevném okruhu odvozeném při začátku návratu. Kontrola nebezpečí platí dál. |
| Další hledání kořisti | Nová hledací výprava začne až po skutečném návratu do domovského okruhu. Během návratu lze dál reagovat na dostupnou kořist a hrozby. |
| Jídlo pracovníků | Běžná dokončená práce doplní 6 Food místo 4. Pracovník s připraveným domovem/pracovištěm, prázdnou zásobou a hladem ≥ 0,65 může dojít na své pracoviště a po nepřerušených 15 s práce získat 2 Food i mimo pracovní dobu. Nouzové doplnění nepřidává mzdu ani nemění denní odměnu. Dřevorubců se netýká. |
| Diagnostika | Observer v4, záznam i `.aiworld group status` uchovají poslední chybu návratu během náhradních činností. Observer/záznam navíc obsahují počty odmítnutí podle příčiny, příznaky cesty a výšku. Skutečný posun ukončí měření nehybnosti. |
| Automatický test | Přidána kontrola dlouhého hladovění pracovníka s prázdnou zásobou. Zůstávají kontroly návratů, pohybu, hranic a jídla při dostupných zásobách. Příjem protokolu a kompatibilita starších záznamů se testují v CI. |

Obnova neteleportuje NPC a nezaručuje cestu tam, kde navmesh žádnou nenabízí.
Výsledný pokles počtu zaseknutých NPC musí ověřit nový běh skutečného serveru.

### Ověření oprav z 25. 9.

Při ručním nasazení aktualizuj nejprve příjemce protokolu a potom engine:

```sh
docker compose up -d --build world-viewer
make build
make restart-world
```

Restart proveď až po úspěšném sestavení. Databázová migrace není potřeba.
Observer `/api/state` má po prvním exportu hlásit `version: 4`; obnov stránku.
Standardní CI deploy toto pořadí zachovává a následně sám spustí čtyřhodinový
test. Po ručním nasazení použij `make test-aiworld`.

1. Krátký ruční test odděl od dlouhého běhu. Sleduj pavouky `.go creature 80406`
   a `.go creature 80700` po hledání nebo neúspěšném lovu. Očekávej skutečné
   návratové přesuny; při problému čti `return strategy`, `lastFailure`,
   `stalledMs` a podrobnosti v Observeru. `stalledMs` nesmí narůstat přes
   skutečný posun delší než jeden yard.
2. U `.go creature 80683` sleduj Food během práce a jídla: běžná práce +6
   do stropu 20, dokončené jídlo −1 a pokles hladu. Při prázdné zásobě očekávej
   `FOOD_SUPPLY`, přesun na pracoviště a po 15 s práce +2 Food. Přerušená
   práce zásobu nedoplní. Pokud zásoba neklesne na nulu, nouzový scénář nebyl ověřen.
3. Čtyřhodinový běh nech bez zásahů s `AIWorld.ElwynnAlwaysActive = 1`.
   Po minutě ověř `make record-aiworld-status`. Po skončení porovnej report,
   zejména počty a délky zablokovaných návratů, hlad predátorů a zásoby 80683.
   Pošli celou session včetně všech gzip částí a souhrnu podle
   [návodu](ObserverRecording.md).

## Hranice této změny

Jde o místní rozhodování, skutečný pohyb, boj a viditelné animace.
Gesto hovoru zatím nevytváří text rozhovoru ani vztahy. Rozšířená práce
vyrábí číselné osobní zásoby, nikoliv inventářové předměty, společné sklady
nebo tržní nabídky; nová místní práce nevyplácí mzdu. Místní jídlo spotřebuje
vlastní Food, ale při prázdné zásobě zůstává dostupné dosavadní ambientní
jídlo; nejde tedy ještě o uzavřenou potravinovou ekonomiku. Pastva nevyčerpává
zdroje krajiny. Cestovatelé zatím nedostávají dálkové trasy.
Stávající ekonomické účinky výslovně připravených rutin zůstávají jejich
vlastní součástí. Role se při restartu odvozují z katalogu a živých příznaků
NPC; běžící lokální činnost se nepersistuje.
Globální demografie, nové questy z nedostatku, sémantické cestovní trasy,
trvalé vztahy a dlouhodobé učení zůstávají pro navazující rozšíření.

## Automatická kontrola

Úpravy z 25. 9. prošly **42 C++ testy / 3 753 assertions** (role, pohyb,
telemetrie), sadou 60 Python testů Observeru (jeden POSIX test se ve Windows
přeskakuje) a 8 JavaScript testy. V Linuxu samostatně prošlo všech 38 testů
sběru/vyhodnocení včetně `SIGTERM`. API test přijímá plný snímek 3 540 NPC
s diagnostikou v4; jeho velikost vyžaduje limit 12 MiB místo původních 8 MiB.
Prošla syntaktická kontrola `LivingRole.cpp`, `TelemetryCapture.cpp` a
`TelemetryJsonCodec.cpp`. Kontrolu celého příkazového souboru blokuje lokálně
chybějící Boost preprocessor; úplný build serveru ani test skutečné navmeshe
lokálně proveden nebyl. Výše uvedené testy tedy nepotvrzují výsledek dalšího
čtyřhodinového běhu.

Úpravy podle záznamu z 24. 9. prošly **1 711 assertions ve 34 testech** pod
MSVC/Catch2: zahrnují průchod trasy přes zakázanou oblast i při legálních
koncích, omezení počtu kontrol, návratové odbočky na souřadnicích Elwynnu,
odstup opakovaných pokusů, povolené náhradní činnosti, práci se zásobami,
širší hledání přes krátké kroky autorizované skutečným `ActionSystem`
a vyčištění stavu při nové materializaci. Prošla syntax upravených
běhových zdrojů včetně `FleeingMovementGenerator.cpp`.
Tyto kontroly nenahrazují sestavení celého serveru ani běh se skutečnými
mapami; lokálně není Docker a úplné nativní sestavení blokuje neúplný Boost.

Oprava návratu prošla **1 113 assertions ve 29 testech** pod MSVC/Catch2.
Test reprodukuje odmítaný původní krok přes skutečný `Position::GetExactDist2d`,
ověřuje nové kroky ve 360 směrech i jejich průchod skutečným `ActionSystem`,
dále ohyby cesty, výškové rozdíly, navazující návrat z 65 yardů, duplicity
a neplatné body či mapy. Prošla syntax běhového kódu a kontrola diffu.
Testy geometrie neověřují skutečnou navmesh ani chování na živém serveru.

Sprint a upravený limit prošly **1 082 assertions ve 26 testech** pod
MSVC/Catch2. Časovač vyprší přesně jednou včetně velkého opožděného ticku,
neplatné bonusy i běžný chase zachovávají násobek 1. Zjednodušený model
přímého běhu ověřuje, že kombinace sprintu a nového limitu umožní dostihnout
stejně rychlou kořist s náskokem 14 yardů, zatímco původní limit nestačil;
rychlejší kořist může dál uniknout. Nejde o simulaci navmeshe ani úderů.
Prošla syntax skutečných `ChaseMovementGenerator.cpp`, `MotionMaster.cpp`,
`MovementDefines.cpp`, `LivingRole.cpp`, `ActionExecutor.cpp` a
`AgentRegistry.cpp` se skutečnými hlavičkami enginu a kontrola diffu.

Oprava pronásledování z 23. 9. 2026 prošla **1 046 assertions ve 23 testech**
pod MSVC/Catch2. Nové scénáře rozlišují chybu cesty, ztrátu oběti, chybějící
chase, dosah pro úder a dočasné znehybnění; validátor odmítá formační úhel
u samostatného lovu a dál dovoluje úhly při obraně a skupinovém lovu.
Reset materializace zahazuje také diagnostiku a dočasně vynechanou kořist.
Prošla syntax běhového kódu a kontrola diffu. Žádný z těchto lokálních
testů neověřuje skutečný pohyb pavouka na uživatelově navmeshi.

Aktuální rozšíření z 23. 9. 2026 prošlo **1 012 assertions v 21 testech**
pod MSVC/Catch2 včetně regresí vlků a světových úhlů. Nové případy ověřují
autorizaci bezpečného úkrytu, zamítnutí útoku z pouhého poplachu, nebezpečné
úseky cest, rozdíly rolí a druhů, idempotenci/limity produkce, čištění
pozorování při nové materializaci a identitu partnera hovoru. Prošla také
syntax aktuálních `LivingRole.cpp`, `ActionExecutor.cpp` a `AgentRegistry.cpp`
se skutečnými hlavičkami enginu a `git diff --check`. Počty níže zachycují
historický test před tímto rozšířením.

Lokálně 22. 9. 2026 prošlo **698 assertions v 14 testech** v Catch2 v2.13.9
pod MSVC: nové role spolu s regresními testy vlků, formací a světových úhlů.
Nový `tests/game/LivingRole.cpp` pokrývá rozsah zóny a řízení, klasifikaci
rolí, dovolené činnosti, prahy útěku, zamítnutí neplatného lovu a rozmístění
přicházejících útočníků i odpojení staré činnosti při změně materializace.
Regrese navíc používají původní DBC reakce pavouka (FT 22), kořisti (FT 31) a neutrálního Defias Thug (klon FT 7); ověřují pomoc společné WorldFaction bez nativního přátelství a omezení neutrální kořisti na role predátor/kořist. Normální CMake testovací cíl jej načítá automaticky.
Lokální komponentový build používá skutečný ActionSystem a produkční
resolver úhlů; pro izolované linkování přebírá přesnou definici
`Position::NormalizeOrientation` z `Position.cpp`, stejně jako vlčí testy.

MSVC provedl také syntaktickou kontrolu skutečných zdrojů `LivingRole.cpp`,
`ActionExecutor.cpp` a `AgentRegistry.cpp` s hlavičkami enginu. Přidaná větev `GetReactionTo` se syntakticky ověřuje samostatně jako přesně převzatý úsek produkčního zdroje proti skutečným hlavičkám; nejde o sestavení celého `Object.cpp`.
Kompletní sestavení/linkování
serveru a samostatnou kontrolu příkazového souboru i `Object.cpp` blokuje chybějící plná
instalace Boost. Tyto kontroly nenahrazují runtime ověření pohybu, navmeshe,
animací, dostupnosti kořisti ani výkonu při větším počtu načtených NPC.
