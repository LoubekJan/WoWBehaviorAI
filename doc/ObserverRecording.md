# Několikahodinový záznam chování AIWorld

Záznamník ukládá všechny agenty, které vidí Observer: AIWorld v Elwynnu, mapa 0.
Zachytí jejich role, lov, pohyb, návraty domů, potřeby, skupiny, jídlo a suroviny.
Každých 5 sekund uloží celý stav do komprimovaného souboru. Výpadky nebo zastaralá
data označí zvlášť, aby se při rozboru nezaměnily se zaseknutým NPC.

## 1. Spuštění na serveru

Přenes aktualizované soubory do repozitáře na serveru. Pro samotný záznamník není
potřeba překládat ani restartovat worldserver. Musí běžet worldserver s telemetrií
a funkční Observer — v prohlížeči musí ukazovat aktuální agenty.

V kořeni repozitáře spusť:

```sh
make record-aiworld
```

Záznam poběží **4 hodiny** na pozadí. Můžeš zavřít SSH; sám se ukončí. Pro osm
hodin použij místo toho:

```sh
AIWORLD_RECORD_HOURS=8 make record-aiworld
```

Pokud nemáš `make`, základní příkaz je `docker compose up -d aiworld-recorder`.
Samostatná služba používá Python image; při prvním spuštění se případně stáhne.
Záznamník se běžným `make start` nezapíná. `make stop` jej zastaví spolu se serverem.
Po restartu hostitele nebo Dockeru je potřeba jej znovu spustit.

## 2. Ověření po první minutě

```sh
make record-aiworld-status
```

V posledním výpisu hledej `last_status: "fresh"` a rostoucí počítadlo `fresh`
uvnitř `counts`. Hodnota `max_agents` musí být větší než nula. Po jedné minutě
bývá přibližně 12 čerstvých vzorků; nejde o počet NPC ani událostí.

Pokud uvidíš jiný stav:

| Stav | Význam / co ověřit |
| --- | --- |
| `unconfigured` | Observer nemá nastavený `WORLD_VIEWER_TELEMETRY_TOKEN`; zkontroluj konfiguraci telemetrie. |
| `waiting` | Observer ještě nedostal snímek; ověř běžící worldserver, zapnutou telemetrii a shodný token obou služeb. |
| `stale` | Přenos snímků se zastavil; samotné NPC z těchto dat posoudit nelze. |
| `empty` | Přenos běží, ale snímek neobsahuje žádné agenty AIWorld. |
| `error` | Observer není dostupný nebo vrací neplatnou odpověď. Výpis obsahuje typ chyby, případně HTTP kód. |

Záznamník při výpadku pokračuje v pokusech a zaznamená i obnovení spojení.
Pokud za celou dobu nezíská ani jeden čerstvý neprázdný snímek, skončí s kódem 2.

## 3. Průběh testu

Nech nejprve svět alespoň hodinu běžet bez vlastních zásahů. Pro simulaci bez
přítomnosti hráče musí být na běžícím serveru aktivní `AIWorld.ElwynnAlwaysActive = 1`;
v aktuální verzované konfiguraci je zapnuté. Díky tomu nemusíš stát u pavouků.

Potom můžeš zkusit lov, útěk, obranu spojenců nebo práci dřevorubců. Poznamenej
si čas a pokud možno `spawnId` dotčeného NPC. Připiš, zda jsi měl zapnuté GM,
změněnou rychlost nebo jiné úpravy testovací postavy; ovlivňují interpretaci.

Záznam po 5 sekundách ukáže delší průběh a opakující se problémy. Nezaručuje
zachycení každého jednotlivého úderu nebo krátké změny. Poslední důvod ukončení
lovu může zůstat stejný i během další činnosti, a proto nejde jednoduše počítat
každý výskyt `HUNT_LEASH` jako nový neúspěšný lov. Dlouhodobá paměť otevřená
v detailu NPC se tímto záznamníkem nestahuje.

## 4. Ukončení a soubory k poslání

Po nastavené době se záznam sám uzavře. Pro dřívější ukončení:

```sh
make record-aiworld-stop
make record-aiworld-status
```

Výsledky najdeš na serveru v:

```text
runtime/recordings/aiworld-<čas UTC>-<id>/
  summary.json
  part-0001.jsonl.gz
  part-0002.jsonl.gz   (jen pokud záznam narostl)
```

Pošli **celou složku dokončeného běhu** — `summary.json` a všechny `part-*.jsonl.gz`.
Soubory už jsou komprimované; není nutné je rozbalovat. Každá část má přibližný
limit 64 MiB, po něm se otevře další. Celková velikost závisí na délce a počtu
agentů. Staré záznamy se nepřepisují ani automaticky nemažou.

V `summary.json` bude na konci `status: "finished"` a `stop_reason: "duration"`
nebo `"stopped"`. `usable: true` znamená, že existují čerstvá data, nikoli že
chování NPC prošlo testem. Násilné ukončení nebo výpadek napájení může poškodit
rozepsanou část; již uzavřené části zůstanou čitelné.

K záznamu připoj krátce verzi/commit nasazeného serveru, délku testu a své
pozorování. Volitelně přilož `runtime/logs/Server.log` z téže doby — zkopíruj ho
**před restartem worldserveru**, protože aktuální konfigurace jej při startu
přepisuje.
