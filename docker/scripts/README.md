# docker/scripts

Helper scripts invoked from Dockerfiles or the Makefile.

- `render-conf-and-run.sh` — substitutes DB credential placeholders into a
  mounted `authserver.conf`/`worldserver.conf` template and execs the server
  binary against the rendered copy.
- `download-tdb.sh` — resolves a pinned TrinityCore TDB (world content)
  release via the GitHub API, verifies it against `TDB_SHA256` if given, and
  imports it into the `world` database. Invoked via `make db-import-tdb
  TDB_VERSION=...` (see README_DEV.md "World content (TDB)").
