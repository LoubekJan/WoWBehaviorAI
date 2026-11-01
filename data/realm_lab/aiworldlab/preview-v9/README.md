# Mapa preview-v9 — 1. listopadu 2026

Nová uživatelská mapa pro prohlídku v režimu `authoring-empty`, realm 2,
mapa 725, oblast 4988. Město, vesnice, tábory a most z předchozí mapy jsou
odstraněné. Nový zdroj obsahuje vegetaci a původní jeskyni. Osazení NPC je
prázdné, AI a telemetrie zůstávají vypnuté.

Čtyři ADT dlaždice zachovávají hranice X −533,333 až 533,333 a Y 0 až
1066,667. Všech devět zdrojových souborů zůstalo beze změny. Samostatný
staging přiřadil všem 1024 částem oblast 4988; jiné mapy nejsou v balíku.

[Serverový manifest](server-manifest.json) obsahuje 38 souborů: 3 DBC,
4 terénní mapy, 5 navigačních souborů a 26 VMAP souborů včetně 21 modelů.
[Nezávislá kontrola](data-verification.json) ověřuje shodu se zdroji,
kolizní závislosti, všechny čtyři navigační dlaždice a bezpečný vstup.
Nevydává se za hráčský průchod celou mapou ani globální důkaz všech cest.

[Vstup](player-entry.json): `.go xyz 266.667 800 4.209 725`.
Start nových postav a hřbitov 9000 používají stejný bod; existující postavy
a jejich homebindy se automaticky nepřesouvají. Klient potřebuje nový
`patch-4.MPQ` a `patch-X.MPQ` s odpovídajícím `WorldSafeLocs.dbc`.

## Známé omezení vody

Nativní exportér převádí první MH2O vrstvu každé části. Nový zdroj má
33 částí s druhou vrstvou a 933 dalšími viditelnými čtverci. U 97 z nich
je voda nad vzorkovaným terénem; tato část vody bude viditelná v klientu,
ale nebude mít odpovídající serverové chování. Podrobnosti jsou v
[rozboru vrstev](multilayer-water-review.json). Klientské vrstvy jsou
zachované. V této prohlídkové verzi se formát serverových vodních dat nemění.

195 neautorských krajních vzorků kapaliny bylo normalizováno podle
[plánu](water-edge-normalization-plan.json); autorské hodnoty zůstaly stejné.
Po této operaci byla navigace znovu vytvořena a nezávisle ověřena.
