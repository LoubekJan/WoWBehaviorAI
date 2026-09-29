# Cílený test návratu a místního zotavení

První pokus je omezený na existující spawn **80883 (Gray Forest Wolf)**.
`deploy/worldserver.conf` nastavuje `AIWorld.LocalRecoverySpawnId = 80883`;
výchozí distribuční konfigurace má `0` (vypnuto). Nastavení se načítá při
startu serveru. Jde o spawn ID, nikoli creature entry. Hlad neubírá zdraví.

## Co se má změnit

Po nejméně pěti minutách neúspěšného návratu a alespoň minutě bez pokroku
může tento vlk přejít na dvě minuty do místního zotavení. Střídá péči podle
role a ověřené krátké přesuny. Střed se při prvním vstupu pevně zapamatuje;
přesuny mají cíl osm yardů od středu, celá cesta musí zůstat do 12 yardů
od něj a uvnitř původního návratového omezení. Platí navmesh, terén,
kolize a Elwynn. Nelze-li ověřit žádnou cestu, zůstane u péče a přibude
`refuge_blocked`; samotný režim bezpečný pohyb nezaručuje.

Potom dostane návrat domů nejméně jednu minutu před dalším místním oknem.
Domov/spawn ani původní mez se neposouvají. Místní zotavení skončí po
skutečném návratu do domácí oblasti nebo zániku materializace. Boj a útěk
mají přednost; při trvající paměti nebezpečí se nové místní okno nespouští.
V místním okně se nezahajuje nový lov; po jeho skončení se běžný lov opět
řídí dosavadními pravidly. Krátké pohyby nemají fungovat jako reset
neúspěšné návratové epizody.

## 1. Nejprve zopakovat navigační dotaz

Po úspěšném build/deployi ve hře použij GM účet:

```text
.gm on
.go creature 80883
```

Vyber vlka jako target. Ověř spawn v `.aiworld group status`. Potom:

```text
.aiworld group navigation
.aiworld group navigationfrom -9603.604 -1049.728 39.59414
```

Druhý příkaz opakuje navigační dotaz z pozice posledního nálezu tohoto
vlka (`20260929T111456Z-d9ec7ba0`). NPC se při něm nepřemisťuje a nezadává
se mu pohyb. Použije jeho nynější domov, fázi, pohybové vlastnosti a právě
načtenou geometrii. Je to reprodukce dotazu na trasu, nikoli přehrání celého
lovu či útěku z minulého běhu. Nejsou-li příslušné dlaždice načtené,
ukáže to výstup; takový výsledek nelze považovat za chybu plánovače.

`navigation` zkoumá skutečnou aktuální polohu, domácí kandidáty i krátké
povrchové přechody. `navigationfrom` zkoumá domácí kandidáty z dodané polohy;
povrchové přechody vyžadují skutečného tvora v daném místě a tento příkaz
je proto nepředstírá. Vstup omezuje na 256 yardů od domova a 100 yardů
výškového rozdílu. Oba příkazy vyžadují stejné debug oprávnění jako status.

Výstup obsahuje původní/cílové souřadnice, výškovou validaci, typ cesty,
ID a příznaky startovního/cílového polygonu, vzdálenosti od nich, načtení
dlaždic a skutečné krajní body vypočtené cesty. ID polygonů jsou jen pro
diagnostiku současně načtených dat. Úspěch samotného dotazu není povolením
vykonat trasu. U živého povrchového přechodu přibude důvod odmítnutí
a poloha/okolní výšky odmítnutého segmentu.

Pro orientaci lze doplnit `.mmap loc` a `.mmap loadedtiles`. Příkaz
`.mmap path` testuje cestu vybraného NPC **k hráči**, nikoli automaticky
domů; nezaměňovat jeho výsledek s domácím dotazem výše.

## 2. Pozorovat krátký pilot

Nechat NPC přirozeně jednat; pokud se nezasekne, nevynucovat problém útoky.
Návrat musí trvat pět minut, než má místní režim vůbec důvod začít.
Ověřit přibližně po 5, 7 a 8 minutách návratu:

```text
.aiworld group status
.aiworld group navigation
```

Ve statusu je `AIWorld local recovery: active=... episodes=... moves=... blocked=...`.
`moves` počítá zahájené přesuny, nikoli dokončené návraty. Smysluplný
výsledek je skutečný pohyb/činnost uvnitř pevné oblasti, následovaný
opětovným pokusem o návrat. `blocked` bez pohybu je stále nevyřešený
geometrický případ, který je potřeba zkoumat. Jestli vlk domů normálně
dojde, `episodes=0` není selhání ani důkaz otestování nouzového režimu.

Telemetrie pod `return_recovery` přidává `refuge_active`, `refuge_episodes`,
`refuge_moves`, `refuge_blocked`, `refuge_remaining_ms`, `refuge_anchor`.
Report stále kontroluje celkovou dobu nedokončeného návratu, i když NPC
provádí `LOCAL_RECOVERY_MOVE` nebo `LOCAL_RECOVERY_CARE`. Místní okno
se nezapočítává jako úspěšný návrat. Ostatní NPC tuto novou funkci nemají;
celkový čtyřhodinový report tedy zatím může zůstat FAIL.

## 3. Co předat k vyhodnocení

Diagnostika se kromě chatu zapisuje do `Server.log` s prefixem
`AIWORLD_NAV_PROBE`. Z kořene repozitáře na serveru:

```bash
grep 'AIWORLD_NAV_PROBE' runtime/logs/Server.log > runtime/navigation-probe-80883.log
make record-aiworld-status
```

Pošli tento log a adresář záznamu recorderu. Kopii logu udělej před
restartem, protože `Server.log` se při startu přepisuje. Připiš, kdy proběhla
GM návštěva; tento cílený běh označíme jako diagnostický. Na celkový test
bez zásahů přejdeme až po posouzení pilota. Nenastavovat další spawny,
dokud první případ nemá vysvětlený výsledek.

## Lokální ověření a hranice

Regrese ověřují časování, povinné okno návratu, neměnný střed a původní
hranici, telemetrii a to, že místní pohyb neskryje nedokončený návrat.
Skutečná průchodnost konkrétní mapy se musí ověřit serverovým dotazem.
Sestavení příkazového souboru a plné linkování serveru musí potvrdit CI;
lokální kontrolu celého příkazového souboru blokuje chybějící Boost.
