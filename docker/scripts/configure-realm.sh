#!/bin/bash
# Upserts the auth.realmlist row so its address/port match this deployment
# instead of TrinityCore's 127.0.0.1 schema default — a versioned
# alternative to a one-off manual `UPDATE realmlist` against the DB. Reads
# REALM_* from the environment (see .env.example) and requires all of them
# explicitly set — network config this critical must not silently fall back
# to 127.0.0.1. The realmlist table itself is created by TrinityCore's own
# DB auto-setup the first time authserver starts, so this waits for that
# rather than requiring a specific run order.
set -euo pipefail

: "${TC_DB_USER:?TC_DB_USER must be set}"
: "${TC_DB_PASSWORD:?TC_DB_PASSWORD must be set}"
: "${REALM_NAME:?REALM_NAME must be set}"
: "${REALM_ADDRESS:?REALM_ADDRESS must be set}"
: "${REALM_LOCAL_ADDRESS:?REALM_LOCAL_ADDRESS must be set}"
: "${REALM_LOCAL_SUBNET_MASK:?REALM_LOCAL_SUBNET_MASK must be set}"
: "${REALM_PORT:?REALM_PORT must be set}"

DB_HOST="${DB_HOST:-mysql}"
DB_PORT="${DB_PORT:-3306}"

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

readback="$(mysql -h"$DB_HOST" -P"$DB_PORT" -u"$TC_DB_USER" -p"$TC_DB_PASSWORD" -N auth -e "SELECT name, address, localAddress, localSubnetMask, port FROM realmlist WHERE id = 1")"
if [ -z "$readback" ]; then
    echo "Realm row missing after write — configuration NOT verified." >&2
    exit 1
fi
IFS=$'\t' read -r rb_name rb_address rb_local_address rb_local_subnet_mask rb_port <<< "$readback"

if [ "$rb_name" != "$REALM_NAME" ] || [ "$rb_address" != "$REALM_ADDRESS" ] || \
   [ "$rb_local_address" != "$REALM_LOCAL_ADDRESS" ] || [ "$rb_local_subnet_mask" != "$REALM_LOCAL_SUBNET_MASK" ] || \
   [ "$rb_port" != "$REALM_PORT" ]; then
    echo "Realm row does not match requested configuration after write:" >&2
    echo "  requested: name=${REALM_NAME} address=${REALM_ADDRESS} localAddress=${REALM_LOCAL_ADDRESS} localSubnetMask=${REALM_LOCAL_SUBNET_MASK} port=${REALM_PORT}" >&2
    echo "  actual:    name=${rb_name} address=${rb_address} localAddress=${rb_local_address} localSubnetMask=${rb_local_subnet_mask} port=${rb_port}" >&2
    exit 1
fi

echo "Realm configuration verified:"
echo "  name: ${rb_name}"
echo "  address: ${rb_address}"
echo "  localAddress: ${rb_local_address}"
echo "  localSubnetMask: ${rb_local_subnet_mask}"
echo "  port: ${rb_port}"
