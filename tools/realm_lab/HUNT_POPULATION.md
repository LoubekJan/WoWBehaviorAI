# Lov, krmení a návrat v AI World Lab

Profil `LAB_AI_PROFILE=hunt-cycle` rozšiřuje původní test na dva medvědy
(predátory) a čtyři jeleny (kořist). Mapa zůstává `725`, oblast `4988` a vymezený
prostor zůstává stejný. Klientský patch ani terén se kvůli této populaci nemění.
Definice populace je v `data/realm_lab/aiworldlab/hunt-population.json`; společné
hranice mapy jsou v `test-points.json` ve stejné složce.

| Spawn / agent | Šablona | Role | Domov X | Domov Y | Z |
| --- | --- | --- | ---: | ---: | ---: |
| 900725 | 1186, Elder Black Bear | PREDATOR | 266.667 | 800 | 0 |
| 900726 | 1186, Elder Black Bear | PREDATOR | 326.667 | 800 | 0 |
| 900727 | 883, Deer | PREY | 291.667 | 800 | 0 |
| 900728 | 883, Deer | PREY | 256.667 | 826 | 0 |
| 900729 | 883, Deer | PREY | 351.667 | 800 | 0 |
| 900730 | 883, Deer | PREY | 336.667 | 774 | 0 |

Domov původního medvěda `900725` zůstává zachovaný. Každé NPC má vlastní pevný
domov a dobu obnovení 120 sekund. Úmrtí a následné obnovení kořisti jsou očekávaná
součást simulace. Obnovení predátora přerušuje ověřování jeho aktuálního cyklu.

AIWorld řídí pouze těchto šest explicitních spawnů. Profil povoluje přirozený
růst hladu a běžné individuální chování. Skupiny, smečky, rozšíření living roles,
modelové požadavky, AI rady a testovací háčky zůstávají vypnuté. Hlad roste
rychlostí `0.003/s`; z nulové hodnoty dosáhne prahu lovu `0.65` přibližně za
217 sekund. Rozhodování a dostupnost kořisti mohou první lov posunout. Únava a
tlak na zdroje v tomto profilu nerostou.

## Příprava a aktivace

Příkazy se spouštějí v samostatném lab checkoutu na serveru. Nastav
`LAB_AI_PROFILE=disabled` v `deploy/lab/.env` a zastav lab worldserver před
jakoukoli změnou databáze:

```sh
docker compose --env-file deploy/lab/.env -f compose.lab.yml stop worldserver

docker compose --env-file deploy/lab/.env -f compose.lab.yml run --rm --no-deps -T --interactive=false tc-dev \
  python3 /workspace/tools/realm_lab/manage.py bootstrap-hunt /workspace/runtime/lab/data

docker compose --env-file deploy/lab/.env -f compose.lab.yml run --rm --no-deps -T --interactive=false tc-dev \
  python3 /workspace/tools/realm_lab/manage.py preflight-hunt /workspace/runtime/lab/data

docker compose --env-file deploy/lab/.env -f compose.lab.yml run --rm --no-deps -T --interactive=false tc-dev \
  python3 /workspace/tools/realm_lab/manage.py activate-hunt /workspace/runtime/lab/data
```

Bootstrap připraví všech šest řádků v režimu Observe. Aktivace zapne řízení
přesně těchto šesti NPC. Před zápisem se kontrolují původní potvrzení mapy a
nativního pohybu, skutečné soubory mapy, šablony, existující vazby a domovy. Jiný
řízený agent, další spawn na mapě 725, kolize ID nebo členství ve skupině operaci
zablokují. Změny používají pouze soukromé world/characters databáze labu.

Po úspěšné aktivaci nastav `LAB_AI_PROFILE=hunt-cycle` a spusť:

```sh
docker compose --env-file deploy/lab/.env -f compose.lab.yml up -d worldserver world-viewer
```

Každý start worldserveru znovu ověřuje datový balík a přesnou populaci. Pro
vypnutí simulace zastav lab worldserver, vrať `LAB_AI_PROFILE=disabled` a znovu
jej spusť. Databázové spawny zůstanou připravené pro další běh.

## Co se ověřuje

Tento scénář probíhá automaticky. Příkazy `.npc follow`, `.npc evade`, teleport
NPC, ruční zabití kořisti nebo obnovení NPC nejsou součástí pozorovaného cyklu.
Pro prohlídku se na labu přenes hráčským příkazem
`.go xyz 266.667 800 2 725` po zapnutí `.gm on` a NPC pouze pozoruj.

Sleduj [lab Observer](http://192.168.0.248:9091). Po nasazení ověř obě role a
všech šest očekávaných identit v čerstvých vzorcích. Později může být kořist
mrtvá; požadavek, aby všech šest NPC bylo nepřetržitě živých, by odporoval testu
lovu. U každého žijícího predátora musí mapa, původní domov a řízení zůstat
správné.

Jeden úplný cyklus predátora má tuto posloupnost:

1. Hlad dosáhne prahu a predátor vybere živou kořist. Objeví se fáze `HUNTING`
   a její spawn v `target.spawn_id` nebo `living_role.last_hunt_target_spawn_id`.
2. Stejná kořist zemře; `living_role.hunt_end` může ukázat `PREY_DIED`.
3. Predátor přejde do `FEEDING`, dokončí krmení (`hunt_end = FED`) a jeho
   `needs.hunger` skutečně klesne. Samotná animace jídla nestačí.
4. Pokud krmení skončilo mimo oblast domova, následuje fyzický návrat s
   `movement_purpose = RETURN_HOME`. `movement.home_distance` klesne nejvýše na
   14 yardů. Změna domova, respawn nebo pouze přeskočení fáze návrat neprokazují.
5. Stejný predátor začne další běžnou činnost a při pozdějším růstu hladu další
   lov. Nový lov se musí objevit jako nový pozorovaný přechod; zůstávající text
   `FED` je výsledek předchozího lovu.

Počáteční rozestupy umožňují lov mimo oblast domova. Kořist se pohybuje, proto
lov dokončený již do 14 yardů od domova ověřuje lov a krmení, ale nezkouší návrat
z větší vzdálenosti. Pro první ověření sleduj alespoň 15 minut, oba predátory a
alespoň jeden skutečný návrat po krmení. Opakované cykly se vyhodnocují z delšího
záznamu; nasazení samo nepotvrzuje deset úspěšných návratů.

## Záznam a vyhodnocení

Z lab checkoutu spusť hodinový záznam po dvou sekundách. Pokud už vhodný záznam
běží, pokračuj v něm místo vytváření nového:

```sh
AIWORLD_RECORD_HOURS=1 AIWORLD_RECORD_INTERVAL=2 AIWORLD_TEST_MINUTES=15 \
AIWORLD_RECORD_LABEL=map725-hunt-cycle AIWORLD_RECORD_BUILD_LABEL="$(git rev-parse HEAD)" \
docker compose --env-file deploy/lab/.env -f compose.lab.yml \
  --profile recording up -d --no-deps aiworld-recorder
```

Záznamy gzip JSONL a souhrny jsou v `runtime/lab/recordings`. Čerstvé vzorky musí
přibývat a souhrn musí nést testovanou revizi a scope mapy 725. Push do lab větve
spustí CI/CD a restart labu; zaznamenej jej jako konec aktuálního pozorování.

Obecný behavior report hledá zaseknutí, opuštění scope a problémy s hladem.
Počítá pozorované začátky lovu a krmení, ale nepropojuje sám všechny kroky výše
do úplných cyklů. Jeho `PASS` tedy nenahrazuje kontrolu cyklu ani počet návratů.
Vzorkování také může vynechat krátký boj nebo okamžik zabití.

U každého ověřeného cyklu zaznamenej spawn predátora a kořisti, časy lovu,
krmení a návratu, hlad před/po krmení, počáteční a konečnou vzdálenost od domova
a navazující činnost. Domov v telemetrii vychází z agentova záznamu, zatímco
`home_distance` používá runtime domov creature; pro přesné srovnání runtime
domova použij `.aiworld group navigation` a pro GUID `.npc info`.

Současný telemetrický protokol neobsahuje runtime GUID, identifikátor života ani
počet online hráčů. Výpadek, smrt, restart nebo nezjištěný respawn proto přeruší
důkaz souvislého cyklu. Test bez hráče potřebuje navíc záznam nulového počtu
hráčů v době probíhajícího cyklu. Samotné odhlášení po již dokončeném návratu
tento scénář neověřuje.
