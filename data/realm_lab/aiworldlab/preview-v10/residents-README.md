# Obyvatelé domů — residents-v10

Nové osídlení preview-v10 z 2. listopadu 2026 obsahuje **22 NPC v 11 obytných domech**, vždy dvě postavy v každém domě. Hostinec, kovárna, radnice, pila, sila a jeskyně jsou vynechané.

[Živé ověření nasazení](residents-activation.json) potvrzuje revizi `6a33965e5388`, osm zdravých služeb, všech 22 živých NPC, 22 samostatných šablon a 22 návrhů v Ateliéru. Ověřený je i jednotlivý export každé šablony a zachování účtů, hráčských postav a klientských patchů. [CI/CD](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/41461277536) prošlo 910 Python a 699 C++ testy. Před aktivací vznikla ověřená soukromá záloha databází, návrhů a konfigurace.

Každá postava má vlastní novou herní šablonu, jméno, podtitul a Studio návrh. Nativní parametry vycházejí z bezpečné neměnné šablony Eastvale Peasant `11328`; kopírují se všechny její sloupce a prostředky vázané na entry. Základní šablona zůstává zachovaná. Změna jednoho jména nepřejmenuje spolubydlící ani jinou postavu.

## Přiřazení domů

Čísla domů jsou pořadí pro editor; ID modelu určuje konkrétní umístění v uložené mapě. Souřadnice ukazují první ze dvou samostatných bodů na skutečné vnitřní podlaze.

| Dům | ID modelu | Obyvatelé | Spawn ID | Entry šablon | První bod X / Y / Z |
| --- | --- | --- | --- | --- | --- |
| 1 | 178 | Jan Polní, Petr Polní | 902000–902001 | 9810000–9810001 | 190.816 / 277.490 / 5.925 |
| 2 | 179 | Marek Kovář, Adam Kovář | 902002–902003 | 9810002–9810003 | 166.246 / 279.499 / 5.435 |
| 3 | 183 | Tomáš Březový, Daniel Březový | 902004–902005 | 9810004–9810005 | 359.126 / 535.418 / 4.188 |
| 4 | 184 | Matěj Zelený, Lukáš Zelený | 902006–902007 | 9810006–9810007 | 354.225 / 515.116 / 4.180 |
| 5 | 193 | Ondřej Sedlák, Jakub Sedlák | 902008–902009 | 9810008–9810009 | 43.695 / 843.835 / 2.960 |
| 6 | 194 | Martin Horský, Filip Horský | 902010–902011 | 9810010–9810011 | 73.254 / 836.969 / 3.050 |
| 7 | 196 | Pavel Dubový, Vít Dubový | 902012–902013 | 9810012–9810013 | -428.206 / 498.748 / 3.033 |
| 8 | 197 | Josef Potok, Šimon Potok | 902014–902015 | 9810014–9810015 | -388.161 / 504.536 / 2.308 |
| 9 | 213 | Karel Mlýnský, Jiří Mlýnský | 902016–902017 | 9810016–9810017 | 23.804 / 615.259 / 0.752 |
| 10 | 214 | Michal Strnad, Hynek Strnad | 902018–902019 | 9810018–9810019 | 43.525 / 616.126 / 0.612 |
| 11 | 215 | Antonín Veselý, Oldřich Veselý | 902020–902021 | 9810020–9810021 | 2.325 / 613.984 / 0.734 |

## Ověření a provoz

[Manifest populace](residents-population.json) připíná [důkaz umístění](residents-position-verification.json) a všech 74 stávajících mapových souborů. Každý bod má ověřenou vnitřní podlahu, volný prostor pro postavu, suchý povrch včetně všech zdrojových vrstev vody a úplnou navigační cestu od vstupu i zpět. Kontrola zahrnuje i vložené kolizní objekty v domech. Výška podlahy pochází z kolizních trojúhelníků, nikoliv z počátku modelu.

Profil používá stojící NPC s běžným herním AI. Denní režim, zaměstnání, potřeby, zásoby a LLM řízení jsou další práce. Navigační důkaz nedokazuje autonomně vykonanou cestu ani vizuální prohlídku v klientu.

Nasazení pod společným zámkem zastaví svět, ověří očekávaný stav, doplní jen nové vlastní spawny, obnoví a znovu přiřadí samostatné šablony a před startem zkontroluje všech 22 identit a domovů. Existující pozice se při opakovaném nasazení neposouvají. Staré populace v8 se neobnovují.

Ateliér postav na portu 9092 nabízí všech 22 uložených návrhů; doplňování chybějících návrhů zachovává již uložené autorské úpravy. Export jména/podtitulu patří k vlastní šabloně. Nativní model postavy zůstává zdrojový; vzhled v návrhu slouží pro budoucí rozšíření. Observer na portu 9091 ověřuje čerstvé snímky právě 22 nativních obyvatel.

Tato změna nevyžaduje nový klientský mapový patch. Předchozí stav databází, návrhů a soukromé konfigurace se před aktivací zálohuje.
