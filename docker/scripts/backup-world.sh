#!/bin/bash
# Private online logical backups of the canonical auth and world databases.
# Each database uses a consistent transaction. Stop world writes first when
# an exact cross-database cutover snapshot is required.
set -euo pipefail
umask 077
env_file="${WORLD_ENV_FILE:-.env}"
test -f "$env_file"
test -f compose.yml
mkdir -p runtime/lab/backups
chmod 700 runtime/lab/backups
exec 9>runtime/lab/deploy.lock
flock -x 9
compose=(docker compose --env-file "$env_file" -f compose.yml)
"${compose[@]}" config --quiet
for service in auth-mysql lab-mysql; do
  cid="$("${compose[@]}" ps -q "$service")"
  test -n "$cid"
  test "$(docker inspect --format '{{.State.Running}}' "$cid")" = true
done
backup_dir="$(mktemp -d "runtime/lab/backups/world-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXXXX")"
chmod 700 "$backup_dir"
install -m 600 "$env_file" "$backup_dir/stack.env"
install -m 600 compose.yml "$backup_dir/compose.yml"
git rev-parse HEAD > "$backup_dir/revision.txt"
for service in auth-mysql lab-mysql; do
  target=auth.sql.gz
  databases=auth
  if [ "$service" = lab-mysql ]; then
    target=world-characters.sql.gz
    databases='world characters'
  fi
  partial="$backup_dir/$target.partial"
  "${compose[@]}" exec -T --interactive=false -e "BACKUP_DATABASES=$databases" "$service" sh -ec '
    export MYSQL_PWD="$MYSQL_ROOT_PASSWORD"
    # Database names above are fixed by this script, never user input.
    exec mysqldump --user=root --single-transaction --routines --events \
      --triggers --hex-blob --set-gtid-purged=OFF --databases $BACKUP_DATABASES
  ' | gzip -n > "$partial"
  test -s "$partial"
  gzip -t "$partial"
  mv "$partial" "$backup_dir/$target"
  chmod 600 "$backup_dir/$target"
done
(
  cd "$backup_dir"
  sha256sum auth.sql.gz world-characters.sql.gz stack.env compose.yml revision.txt > SHA256SUMS
  chmod 600 SHA256SUMS
)
printf 'Private database backup saved to %s\n' "$backup_dir"
