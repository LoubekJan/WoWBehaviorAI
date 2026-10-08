#!/bin/bash
# Dedicated MySQL instance: world + characters (including AIWorld persistence).
# No auth database or shared-auth credentials exist in this instance.
set -euo pipefail
: "${TC_DB_USER:?}"
: "${TC_DB_PASSWORD:?}"
[[ "$TC_DB_USER" =~ ^[a-zA-Z][a-zA-Z0-9_]{0,31}$ ]] || exit 2
[[ "$TC_DB_PASSWORD" != change-me* && "$MYSQL_ROOT_PASSWORD" != change-me* ]] || {
  echo 'Replace example lab database passwords before initializing MySQL.' >&2
  exit 2
}
case "$TC_DB_PASSWORD" in
  *';'*|*'"'*|*'\'*|*$'\n'*|*$'\r'*)
    echo 'Lab DB password is not compatible with TrinityCore connection strings.' >&2
    exit 2 ;;
esac
# Use literal escaping with NO_BACKSLASH_ESCAPES, never interpolate raw SQL.
password="${TC_DB_PASSWORD//\'/\'\'}"
MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -uroot <<SQL
SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES';
CREATE DATABASE IF NOT EXISTS world CHARACTER SET utf8mb4;
CREATE DATABASE IF NOT EXISTS characters CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS '${TC_DB_USER}'@'%' IDENTIFIED BY '${password}';
GRANT ALL PRIVILEGES ON world.* TO '${TC_DB_USER}'@'%';
GRANT ALL PRIVILEGES ON characters.* TO '${TC_DB_USER}'@'%';
SQL
