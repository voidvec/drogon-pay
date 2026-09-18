#!/usr/bin/env bash
# Recreate the dev database and apply the migration chain (POSIX twin of
# setup_database.bat).
#
# The migration chain itself is applied by scripts/migrate_db.py, the same
# executor CI uses; this script only picks the python interpreter and the
# defaults. Credentials are read by migrate_db.py from the environment or
# examples/pay-server/.env, never from this file.
#
#   ./setup_database.sh              reset the schema, replay the chain
#   ./setup_database.sh --keep-data  apply pending migrations only
set -euo pipefail

cd "$(dirname "$0")/../../.."

: "${PGDATABASE:=pay_test}"
export PGDATABASE

if command -v python3 >/dev/null 2>&1; then
    PY=python3
else
    PY=python
fi

ARGS=(--env-file examples/pay-server/.env --db "$PGDATABASE")
if [ "${1:-}" = "--keep-data" ]; then
    echo "Applying pending migrations into existing $PGDATABASE..."
else
    ARGS+=(--reset-schema --confirm-drop "$PGDATABASE")
    echo "Resetting the public schema of $PGDATABASE, then applying the chain..."
fi

"$PY" scripts/migrate_db.py "${ARGS[@]}"
echo "Database setup complete."
