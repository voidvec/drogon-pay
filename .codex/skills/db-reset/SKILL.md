---
name: db-reset
description: Reset the pay_test development schema and replay the SQL migration chain with the project executor
---

# Database Reset

Reset the `pay_test` development schema so it is rebuilt from
`sql/NNN_*.sql`, with the applied versions recorded.

## Usage

- User invokes: `/db-reset`
- Requires: PostgreSQL running (local or Docker), `psql` and `python` on PATH

## Do not drop tables by hand

The old recipe — pipe `000_drop_pay_tables.sql` and then every
`NNN_*.sql` into psql — is gone for two reasons:

1. It hardcoded the file list, so the day a migration was added the recipe
   silently stopped applying it.
2. `sql/000_*` drops the pay tables but knows nothing about
   `schema_migrations`, so the bookkeeping kept claiming the chain was applied
   against an empty schema. `migrate_db.py` now refuses to proceed when
   recorded versions have no tables behind them.

## Local PostgreSQL

```powershell
examples\pay-server\scripts\setup_database.bat         REM Windows
examples/pay-server/scripts/setup_database.sh          REM POSIX
```

Both call `scripts/migrate_db.py --reset-schema
--confirm-drop pay_test`, which drops/recreates the `public` schema and then
replays the chain in one pass. Password comes from `PGPASSWORD` /
`PAY_DB_PASSWORD` or `examples/pay-server/.env`; the scripts carry none.

`--reset-schema` is a loopback-only dev primitive: the executor refuses any
host that is not `localhost`/`127.x`/`::1`, because a remote host is where
staging and production live and `--confirm-drop` is filled in for you by these
scripts. To move a real environment forward, ship a versioned `sql/NNN_*.sql`
instead.

To apply only what is missing, without resetting: add `--keep-data`.

## Docker PostgreSQL

```bash
docker compose -f examples/pay-server/docker-compose.yml down -v
docker compose -f examples/pay-server/docker-compose.yml up -d postgres
# initdb.d already ran the chain as superuser and wrote no bookkeeping:
python scripts/migrate_db.py --host 127.0.0.1 --user postgres --db pay_test --baseline
```

`--baseline` records the on-disk chain as applied *without* running it, and
refuses if the tables are not actually there — so a half-provisioned volume
cannot be adopted by mistake.

## Verify

```bash
python scripts/migrate_db.py --status          # every version reads "applied"
psql -h 127.0.0.1 -U test -d pay_test -c '\dt' # pay_* tables + schema_migrations
```

Then the suite — through the wrapper, or from the binary's own directory (it
loads `./config.json` and `./.env` from the current directory, and
`build.bat` copies both next to the executable; the repository root has
neither):
```powershell
examples\pay-server\scripts\test.bat
# or: cd build\windows-msvc\tests\Release && .\PayBackendTests.exe
```

## ORM Model Regeneration

After the structure changes, regenerate models so they match the schema:

```powershell
cd libs/drogon-pay/src
drogon_ctl create model models
```

For details, see the `/orm-gen` skill. Never hand-edit `src/models/`.

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `database "pay_test" does not exist` | Creating it is provisioning, not a migration: `psql -U postgres -d postgres -c "CREATE DATABASE pay_test OWNER test"` (the app role has no CREATEDB, which is why the executor will not try) |
| `psql: FATAL: role "postgres" does not exist` | Check `examples/pay-server/docker-compose.yml` `POSTGRES_USER` |
| `schema_migrations says ... but these tables are absent` | Something dropped objects out-of-band; run `setup_database` (reset + replay) or delete the stale rows if the empty schema is intended |
| `changed after it was applied` | An applied migration was edited. Revert it and add a new version instead |
