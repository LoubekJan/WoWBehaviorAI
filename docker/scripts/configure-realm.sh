#!/bin/bash
# Upserts the auth.realmlist row so its address/port match this deployment
# instead of TrinityCore's 127.0.0.1 schema default — a versioned
# alternative to a one-off manual `UPDATE realmlist` against the DB. Reads
# REALM_* from the environment (see .env.example). The realmlist table
# itself is created by TrinityCore's own DB auto-setup the first time
# authserver starts, so this waits for that rather than requiring a
# specific run order.
set -euo pipefail

: "${TC_DB_USER:?TC_DB_USER must be set}"
: "${TC_DB_PASSWORD:?TC_DB_PASSWORD must be set}"

DB_HOST="${DB_HOST:-mysql}"
DB_PORT="${DB_PORT:-3306}"

REALM_NAME="${REALM_NAME:-Trinity}"
REALM_ADDRESS="${REALM_ADDRESS:-127.0.0.1}"
REALM_LOCAL_ADDRESS="${REALM_LOCAL_ADDRESS:-127.0.0.1}"
REALM_LOCAL_SUBNET_MASK="${REALM_LOCAL_SUBNET_MASK:-255.255.255.0}"
REALM_PORT="${REALM_PORT:-8085}"

echo "Waiting for auth.realmlist (created by authserver's DB auto-setup on first start)..."
table_ready=0
for attempt in $(seq 1 60); do
    if mysql -h"$DB_HOST" -P"$DB_PORT" -u"$TC_DB_USER" -p"$TC_DB_PASSWORD" -e "SELECT 1 FROM auth.realmlist LIMIT 1" >/dev/null 2>&1; then
        table_ready=1
        break
    fi
    sleep 2
done
if [ "$table_ready" -ne 1 ]; then
    echo "auth.realmlist isn't available yet — start authserver at least once (make start) before running this." >&2
    exit 1
fi

echo "Configuring realm '${REALM_NAME}' (${REALM_ADDRESS}:${REALM_PORT})..."
mysql -h"$DB_HOST" -P"$DB_PORT" -u"$TC_DB_USER" -p"$TC_DB_PASSWORD" auth <<SQL
INSERT INTO realmlist (id, name, address, localAddress, localSubnetMask, port)
VALUES (1, '${REALM_NAME}', '${REALM_ADDRESS}', '${REALM_LOCAL_ADDRESS}', '${REALM_LOCAL_SUBNET_MASK}', ${REALM_PORT})
ON DUPLICATE KEY UPDATE
    name = VALUES(name),
    address = VALUES(address),
    localAddress = VALUES(localAddress),
    localSubnetMask = VALUES(localSubnetMask),
    port = VALUES(port);
SQL

echo "Realm '${REALM_NAME}' configured: ${REALM_ADDRESS}:${REALM_PORT} (local ${REALM_LOCAL_ADDRESS}/${REALM_LOCAL_SUBNET_MASK})."
