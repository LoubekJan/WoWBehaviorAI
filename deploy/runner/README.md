# CI a deploy runnery vlastního světa

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

Existující starý `runner-runner-1` nejprve ponechat dostupný migračnímu
nástroji: ověřuje z něj registraci a deaktivované původní nasazování.
Vyřazení se dokončí až po migraci účtů a ověření canonical stacku.
Na odstranění původního světa nepoužívat hromadné mazání Docker volumes;
účty i současný vlastní svět musí zůstat zachované.
