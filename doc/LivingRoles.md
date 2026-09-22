# Živější role v Elwynnu

Lokální rozšíření z 22. 9. 2026 navazuje na ve hře ověřený vlčí pilot.
Uživatel potvrdil ostatní základní chování ve hře. Lov Forest Spider a pomoc Defias při prvním testu nefungovaly; níže popsaná následná oprava čeká na opakovaný test.

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
ohrožení. Ručně nastavené skupinové činnosti mají v klidu přednost.

## Co jednotlivé role dělají

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

## Hranice této změny

Jde o místní rozhodování, skutečný pohyb, boj a viditelné animace.
Gesto hovoru zatím nevytváří text rozhovoru ani vztahy. Pracovní gesto
nevyrábí předměty a nevyplácí mzdu. Místní jídlo civilistů a pastva
neodečítají zásoby z ekonomiky. Cestovatelé zatím nedostávají dálkové trasy.
Stávající ekonomické účinky výslovně připravených rutin zůstávají jejich
vlastní součástí. Role se při restartu odvozují z katalogu a živých příznaků
NPC; běžící lokální činnost se nepersistuje.

## Automatická kontrola

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
