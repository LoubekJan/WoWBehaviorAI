# CI a deploy runnery vlastního světa

Provozní model k **31. říjnu 2026**. Původní Elwynn realm i jeho
automatické nasazování jsou na současném hostu vyřazené; jeden canonical
stack provozuje realm 2, profil `full-v8` a osm služeb včetně ateliéru postav.

Aktivní workflow je [Realm Lab CI/CD](../../.github/workflows/realm-lab.yml)
na větvi `AI-World-lab`. Používá dva samostatné runnery:

| Compose soubor / služba | Jméno a účel | Labels |
| --- | --- | --- |
| `compose.yml` / `ci-runner` | `wow-ci-runner`: C++ build a testy v oddělených CI volumes | `self-hosted,linux,docker,wow,ci` |
| `compose.lab.yml` / `lab-runner` | `wow-lab-deploy-runner`: nasazení otestovaného commitu do `/home/voslik/WoWBehaviorAI-lab` | `self-hosted,linux,docker,wow,realm-lab-deploy` |

CI projekt zůstává `runner`, tedy kontejner `runner-ci-runner-1`.
Deploy projekt zůstává `aitc-lab-runners`. Původní Elwynn deploy služba
`runner` a label `deploy` se už nedefinují. Starý workflow `ci.yml`
je vyřazený a nic nenasazuje.

## Příprava hostu

Vytvořit `/home/voslik/ci-runner-work` a persistentní checkout větve
`AI-World-lab` v `/home/voslik/WoWBehaviorAI-lab`. Cesty bind mountů
musí být na hostu i uvnitř runneru shodné: Docker CLI používá daemon
hostu a podle těchto absolutních cest načítá zdroje i runtime soubory.

```bash
cd /home/voslik/WoWBehaviorAI-lab/deploy/runner
umask 077
cp .env.example .env
# Nastavit GH_RUNNER_REPO_URL a GH_RUNNER_PAT.
docker compose --env-file .env -f compose.yml up -d --build ci-runner
docker compose --env-file .env -f compose.lab.yml up -d --build lab-runner
```

PAT slouží k registraci runnerů; potřebuje repository Administration
read/write. Soubor `.env` má práva 0600 a necommitovat jej. Runnery
používají Docker socket, proto trusted push build/deploy běží jen na
povolené větvi. PR buildy zůstávají na GitHub-hosted runneru.

V GitHub Settings → Actions → Runners ověřit oba výše uvedené názvy,
labels a stav online. Automatické nasazování dále vyžaduje
`REALM_LAB_DEPLOY_ENABLED=true` a dokončený provisioning světa.

## Co spouští pipeline

- Pull request do `AI-World-lab`: Python/MySQL kontrakty, Compose a skripty,
  testy Observeru, AI, účtů, ateliéru a jeho assetů; potom C++ build a CTest
  na GitHub-hosted runneru. Bez nasazení a bez přístupu k Docker socketu hostu.
- Push do `AI-World-lab`: stejné kontrakty, nativní build a testy na
  `wow-ci-runner`; teprve jejich úspěch umožní samostatnému deploy runneru
  nasadit přesný otestovaný commit.
- Ruční `workflow_dispatch`: kontrakty a serverový build, bez automatického
  nasazení. Deploy je podmíněný událostí `push` a proměnnou výše.

Workflow nemá filtr cest: i běžná změna samotné dokumentace v `AI-World-lab`
spustí stejný řetězec. Při povoleném deploy tedy může zahrnout restart
herního a přihlašovacího serveru. Dokumentační větev či PR samy živý svět nemění.

Čistě dokumentační commit lze výslovně označit `[skip ci]`, aby změna textů
nevyvolala build a restart. Toto je
[mechanismus GitHub Actions](https://docs.github.com/en/actions/how-tos/manage-workflow-runs/skip-workflow-runs)
pro push/PR; není to filtr v našem workflow. Nepoužívat pro změny kódu,
dat, konfigurace ani provozních skriptů. Pokud repository vyžaduje úspěšné
checks před sloučením, přeskočené checks zůstanou pending a merge nepovolí.

Nasazení používá zámek `runtime/lab/deploy.lock`, uzavře případný recorder,
ověří autorské vstupy ještě před zastavením světa a u `full-v8` provede
offline restore → bootstrap → apply vlastních charakterových šablon.
Při výměně sdílených binárek zastaví worldserver i authserver. Po startu
ověří zdraví osmi služeb, přesnou revizi a čerstvou populaci 100 řízených NPC.
Fyzický průchod všech tras a dlouhodobá ekonomika mají samostatné testy.

## Aktualizace a vyřazení původního runneru

Po aktualizaci checkoutu zopakovat příslušný `up -d --build` příkaz.
Build runner používá volumes `aitc_lab_ci_build-data` a
`aitc_lab_ci_ccache-data`, provozní build má jiné volumes.

Image obsahuje restart wrapper, který před registrací bez reuse odstraní
pouze pět lokálních konfiguračních souborů v `/actions-runner`. Starší
upstream ponechával `.runner_migrated` po smazání `.runner`, takže po
restartu mohl hlásit současně „already configured“ a
`configuredSettings=null`. Pracovní adresáře a hostitelské mounty se
nemění; původní entrypoint dál zajišťuje registraci a obsluhu signálů.
Přerušený CI job je po restartu potřeba ověřit či spustit znovu; wrapper
neobnovuje rozpracovaný job.

Pouze při prvním převodu jiné staré instalace: existující `runner-runner-1`
nejprve ponechat dostupný migračnímu
nástroji: ověřuje z něj registraci a deaktivované původní nasazování.
Vyřazení se dokončí až po migraci účtů a ověření canonical stacku.
Na odstranění původního světa nepoužívat hromadné mazání Docker volumes;
účty i současný vlastní svět musí zůstat zachované.

Na již převedeném hostu se starý runner ani původní svět znovu nevytváří.
Návod na běžný provoz a export postav je v [README_DEV.md](../../README_DEV.md).
