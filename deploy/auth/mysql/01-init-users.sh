#!/bin/bash
# Runs only for a NEW dedicated auth volume. Never mounts the retired world DB.
set -euo pipefail
: "${MYSQL_ROOT_PASSWORD:?}"
: "${TC_AUTH_SERVER_USER:?}"
: "${TC_AUTH_SERVER_PASSWORD:?}"
: "${TC_AUTH_DB_USER:?}"
: "${TC_AUTH_DB_PASSWORD:?}"

for name in "$TC_AUTH_SERVER_USER" "$TC_AUTH_DB_USER"; do
  [[ "$name" =~ ^[a-zA-Z][a-zA-Z0-9_]{0,31}$ ]] || {
    echo 'Auth database users must be simple MySQL usernames.' >&2
    exit 2
  }
done
[[ "$TC_AUTH_SERVER_USER" != "$TC_AUTH_DB_USER" ]] || {
  echo 'Auth schema owner and worldserver DML user must be different.' >&2
  exit 2
}
for secret in "$MYSQL_ROOT_PASSWORD" "$TC_AUTH_SERVER_PASSWORD" "$TC_AUTH_DB_PASSWORD"; do
  [[ "$secret" != change-me* && ! "$secret" =~ [[:cntrl:]] ]] || {
    echo 'Replace example auth passwords and remove control characters.' >&2
    exit 2
  }
done
for secret in "$TC_AUTH_SERVER_PASSWORD" "$TC_AUTH_DB_PASSWORD"; do
  case "$secret" in
    *';'*|*'"'*|*'\'*)
      echo 'Auth application password is incompatible with TrinityCore connection strings.' >&2
      exit 2 ;;
  esac
done
# NO_BACKSLASH_ESCAPES plus doubled apostrophes keeps credentials as literals.
server_password="${TC_AUTH_SERVER_PASSWORD//\'/\'\'}"
world_password="${TC_AUTH_DB_PASSWORD//\'/\'\'}"
MYSQL_PWD="$MYSQL_ROOT_PASSWORD" mysql -uroot <<SQL
SET SESSION sql_mode = 'NO_BACKSLASH_ESCAPES';
CREATE DATABASE IF NOT EXISTS auth CHARACTER SET utf8mb4;
CREATE USER IF NOT EXISTS '${TC_AUTH_SERVER_USER}'@'%' IDENTIFIED BY '${server_password}';
CREATE USER IF NOT EXISTS '${TC_AUTH_DB_USER}'@'%' IDENTIFIED BY '${world_password}';
GRANT ALL PRIVILEGES ON auth.* TO '${TC_AUTH_SERVER_USER}'@'%';
GRANT SELECT, INSERT, UPDATE, DELETE ON auth.* TO '${TC_AUTH_DB_USER}'@'%';
SQL
