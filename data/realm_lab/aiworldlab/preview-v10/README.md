# Mapa preview-v10 — uložená verze z 1. listopadu 2026, 22:14

Od 2. listopadu je nad stejnou geometrií nasazený profil
[`residents-v10`](residents-README.md): 22 samostatných obyvatel v 11 domech.
[Aktuální provozní ověření](residents-activation.json) zachycuje toto osídlení;
níže je zachovaný záznam původního nasazení samotné prázdné mapy.

Další autorská úprava pro režim `authoring-empty`, realm 2, mapa 725,
oblast 4988. Přibyly hostinec, kovárna, radnice, farmy, pila a další
objekty; osazení NPC zůstává prázdné.

[Závěrečné ověření](activation.json) potvrzuje nasazení revize `055db924b08c`,
74 správných geometrických souborů, osm zdravých služeb a nulové NPC/AI
osazení. [CI/CD](https://github.com/LoubekJan/WoWBehaviorAI/actions/runs/41352203913)
prošlo 891 Python a 699 C++ testy. [Klientské patche](client-install.json)
jsou instalované a předchozí verze klienta i serveru je zálohovaná.

Čtyři stejné dlaždice mají aktualizovaný terén, 93 M2 a 16 WMO umístění.
V dlaždici (31,31) přibylo 12 čtverců děr, v (31,32) zůstalo původních 32.
Zdrojových devět souborů bylo zkontrolováno před i po přípravě. Oddělený
staging přiřazuje všem 1024 částem oblast 4988; originál projektu zůstává
beze změny a balík neobsahuje Azeroth ani Kalimdor.

[Serverový manifest](server-manifest.json) připíná všech 74 souborů DBC,
terénu, navigace a kolizních modelů. [Nezávislá kontrola](data-verification.json)
ověřila shodu zdrojů, úplnost modelových závislostí, čtyři navigační dlaždice
a suchý vstupní bod. Nevydává se za fyzickou hráčskou prohlídku celé mapy.

Vstup zůstává `.go xyz 266.667 800 4.209 725`; [start a hřbitov](player-entry.json)
používají shodný bod. Existující hráčské postavy a homebindy se nepřesouvají.
Klient potřebuje odpovídající `patch-4.MPQ` a `patch-X.MPQ`.

Původní mapa běží i během CI. Před samotnou výměnou se pod nasazovacím
zámkem znovu ověřují zdrojová data a sdílené modely; teprve poté se svět
zastaví, uloží se záloha a nasadí nový balík.

## Přetrvávající omezení vody

[Nový rozbor](multilayer-water-review.json) znovu potvrdil 33 buněk
s druhou MH2O vrstvou: 933 samostatných viditelných čtverců, z nichž 97
leží nad vzorkovaným terénem. Nativní exportér převádí první vrstvu.
Klient další vodu zobrazí, ale její serverové chování zatím neodpovídá.
Vstupní bod je suchý. Nejprve se podle [plánu](water-edge-normalization-plan.json)
normalizovalo 202 neautorských krajních vzorků kapaliny a poté se navigace
znovu vygenerovala a ověřila; autorské vodní hodnoty zůstaly zachované.
