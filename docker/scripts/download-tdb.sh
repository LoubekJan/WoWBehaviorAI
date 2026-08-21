#!/bin/bash
# Downloads a pinned TrinityCore TDB (world content) release and imports it
# into the `world` database. Talks to mysql directly over the Compose
# network, so only `docker compose up -d mysql` needs to be running first
# (see README_DEV.md "World content (TDB)"). The release asset is resolved
# via the GitHub API for TDB_VERSION rather than a hardcoded URL, since the
# asset filename/extension varies by version.
set -euo pipefail

: "${TDB_VERSION:?TDB_VERSION must be set, e.g. TDB335.25101}"
: "${TC_DB_USER:?TC_DB_USER must be set}"
: "${TC_DB_PASSWORD:?TC_DB_PASSWORD must be set}"

DB_HOST="${TDB_DB_HOST:-mysql}"
DB_PORT="${TDB_DB_PORT:-3306}"
REPO="TrinityCore/TDB"

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

echo "Resolving release ${TDB_VERSION} from ${REPO}..."
release_json="$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/tags/${TDB_VERSION}")"

asset_url="$(echo "$release_json" | jq -r '.assets[].browser_download_url' | grep -Ei '\.(7z|zip|sql\.gz|sql)$' | head -n1)"
if [ -z "$asset_url" ]; then
    echo "No downloadable .7z/.zip/.sql(.gz) asset found on release ${TDB_VERSION}" >&2
    exit 1
fi

asset_file="$WORKDIR/$(basename "$asset_url")"
echo "Downloading $(basename "$asset_url")..."
curl -fsSL -o "$asset_file" "$asset_url"

if [ -n "${TDB_SHA256:-}" ]; then
    echo "Verifying checksum..."
    echo "${TDB_SHA256}  ${asset_file}" | sha256sum -c -
else
    echo "TDB_SHA256 not set — skipping checksum verification." >&2
fi

extract_dir="$WORKDIR/extracted"
mkdir -p "$extract_dir"

case "$asset_file" in
    *.7z)
        7z x -o"$extract_dir" "$asset_file" >/dev/null
        ;;
    *.zip)
        unzip -q -d "$extract_dir" "$asset_file"
        ;;
    *.sql.gz)
        gunzip -c "$asset_file" > "$extract_dir/$(basename "${asset_file%.gz}")"
        ;;
    *.sql)
        cp "$asset_file" "$extract_dir/"
        ;;
    *)
        echo "Unrecognized asset type: $asset_file" >&2
        exit 1
        ;;
esac

sql_file="$(find "$extract_dir" -name '*.sql' | head -n1)"
if [ -z "$sql_file" ]; then
    echo "No .sql file found after extracting $(basename "$asset_file")" >&2
    exit 1
fi

echo "Importing $(basename "$sql_file") into world database..."
mysql -h"$DB_HOST" -P"$DB_PORT" -u"$TC_DB_USER" -p"$TC_DB_PASSWORD" world < "$sql_file"

echo "TDB import complete (${TDB_VERSION})."
