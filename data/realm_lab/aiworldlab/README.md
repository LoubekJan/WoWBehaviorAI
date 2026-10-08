# AI World Lab — mapový projekt

První vlastní mapa: MapID **725**, AreaID **4988**, exploration bit **3618**.
Interní adresář `AIWorldLab`, název `AI World Lab`, `InstanceType=0`
(Noggit zobrazuje `None`), expanze WotLK (2). Aktivní je jedna dlaždice
`(30, 31)`. Aktuální terén je rovný a zatím bez textury.

## Umístění a verze

- Klient: `C:\WoWModding\Client-Lab` — 3.3.5a build 12340, enUS.
- Editor: `C:\WoWModding\RelWithDebInfo\noggit.exe` — Noggit Studio
  3.1446+ `[40bc7ed9]`; jeho SHA-256 je v manifestu.
- Pracovní projekt: `C:\WoWBehaviorAI\runtime\lab\map-project`.
- Klientský základ DBC: `runtime/lab/client-baseline/dbc`.
- Soukromé binární artefakty: `runtime/lab/map-source-artifacts`.

V Gitu zůstává manifest s hashi a nástroj pro doplnění oblasti. Samotný
klient, plné DBC a binární zdroje mapy se ukládají soukromě. První lokální
ZIP obsahuje jen soubory uvedené v `project-manifest.json`; jeho CRC i
seznam položek byly ověřené. Nejde o nasazený klientský patch. Před
serverovou extrakcí uložit odpovídající verzi také na lab host.

Základní DBC pocházejí z `Data/enUS/patch-enUS-3.MPQ`; prověřené byly
všechny archivy načtené editorem včetně vlastního `patch-W.MPQ`.
Původní `Map.dbc` má 135 záznamů, `AreaTable.dbc` 2307. Uložený projekt
přidává pouze mapu 725 a oblast 4988; původní záznamy zůstávají zachované.

## Doplnění oblasti

Před zápisem uložit práci a zavřít Noggit. Z kořene repozitáře:

```powershell
python tools/realm_lab/prepare_map_area.py --prepare
```

Nástroj ověří mapu, kolize s klientským základem a jednu aktivní dlaždici.
Přidá venkovní neutrální oblast 4988 a přiřadí ji všem 256 částem ADT.
Před změnou uchová původní soubory pod `runtime/lab/map-project-backups`.
Výsledek zapíše do `runtime/lab/map-area-receipt.json`. Další ověření
bez zápisu:

```powershell
python tools/realm_lab/prepare_map_area.py
```

Ověřovací režim odmítne neúplné přiřazení. Připravené soubory se při
opakovaném běhu nemění. Na Windows nástroj odmítne zápis při běžícím
procesu `noggit`.

## Další krok

Znovu otevřít projekt a mapu AI World Lab. Nanést jednu existující
texturu, zachovat rovinu a uložit. Potom vytvořit novou verzi artefaktu
a manifestu; současný manifest výslovně zachycuje stav **bez textury**.
Domov a kontrolní body vybrat uvnitř dlaždice, nejméně 100 yardů od hran.
Teprve ze stejného balíku vznikne klientský patch a serverová extrakce.
