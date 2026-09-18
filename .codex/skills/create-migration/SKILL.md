---
name: create-migration
description: Create versioned SQL migration files for database schema changes, following project conventions
disable-model-invocation: true
---

# SQL Migration Skill

Create versioned SQL migration files for the Pay database schema
(`libs/drogon-pay` ORM models + PostgreSQL).

## When to Use

When adding, modifying, or constraining database tables/columns/indexes.
Triggered by `/create-migration`.

## Migration Naming

Pattern: `sql/{NNN}_{snake_case}.sql` (three digits, single underscore,
lowercase). Take the next number from the chain head:

```bash
ls sql/            # highest NNN wins; never reuse or renumber a version
```

`000_drop_pay_tables.sql` is a **dev reset helper**, not part of the version
chain: never add schema changes to it, never renumber the chain to 000.

## How Migrations Are Applied

One executor, one place that knows the file list: `scripts/migrate_db.py`.
It discovers `sql/NNN_*.sql`, applies what is missing in version order, each
migration inside its own transaction together with its `schema_migrations`
row, and refuses to run if a file already applied has changed bytes.

| Consumer | Command |
|----------|---------|
| CI (Linux/Windows/macOS) | `.github/workflows/_build-test.yml` calls `migrate_db.py` |
| Coverage job | `.github/workflows/coverage.yml` calls the same script |
| Deploy | `examples/pay-server/scripts/deploy.{bat,sh}` call it too (they used to run `000_drop_pay_tables.sql` as a migration) |
| Local dev | `examples/pay-server/scripts/setup_database.bat` or `.sh` (schema reset + replay) |
| Docker compose | mounts `sql/` as `docker-entrypoint-initdb.d`, which writes **no** `schema_migrations` rows — run `python scripts/migrate_db.py --baseline` once against such a database |

```bash
python scripts/migrate_db.py --status      # what the database has recorded
python scripts/migrate_db.py --dry-run     # what the next run would apply
```

Never hand-write `psql -f sql/...` in a workflow or script. Six copies of that
list used to exist and they had already drifted: the two in the CI workflows
applied only two of the four versions, and the deploy scripts globbed a `sql/`
path that the plugin refactor had moved, so their "Run migrations" step applied
nothing while reporting success.

## File Template

```sql
-- Migration: 00N_{description}
-- Created: {date}
-- Purpose: {why this change is needed}

{Idempotent SQL statements here}
```

There is no DOWN/rollback file convention in this repo (migrations are only
ever rolled forward onto fresh or staging databases).

## Validation Checklist

Rules 1-5 below are enforced by `scripts/check_migrations.py` (CI
`static-analysis` step) for every migration that is not yet baselined; run it
before you push.

1. **Naming**: `NNN_snake_case.sql`, next number after the latest migration
   (the guard also rejects a gap, because "everything after 003" has to mean
   something)
2. **Idempotent**: `CREATE TABLE/INDEX ... IF NOT EXISTS`,
   `ADD COLUMN IF NOT EXISTS`; `ADD CONSTRAINT x` must be paired with
   `DROP CONSTRAINT IF EXISTS x` in the same file, or checked inside a
   `DO $$ ... IF NOT EXISTS ... END $$` block; top-level `INSERT` needs
   `ON CONFLICT DO NOTHING`
3. **Non-destructive**: no `DROP TABLE/COLUMN`, `TRUNCATE`, or bare
   `DELETE FROM` (only `000_` may drop objects)
4. **ORM consistency**: schema must match `libs/drogon-pay/model.json`;
   regenerate models via `/orm-gen` (`generate_models.bat`) after schema
   changes — never hand-edit `src/models/`
5. **Existing files immutable**: never edit an applied `00N_*.sql`. Two
   gates enforce it — `migrate_db.py` compares the sha256 recorded in
   `schema_migrations`, and `check_migrations.py` compares against
   `scripts/migrations_baseline.json`. If a baselined file genuinely has to
   change, update that JSON by hand in the same PR (the friction is the
   point); `--write-missing` pins a *new* file without touching old entries, and
   refuses one that breaks the content rules (pinning exempts a file from them
   for good, so it cannot double as a waiver).

## Common Patterns

### Add Table
```sql
CREATE TABLE IF NOT EXISTS new_table (
    id SERIAL PRIMARY KEY,
    name VARCHAR(100) NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);
```

### Add Column
```sql
ALTER TABLE existing_table ADD COLUMN IF NOT EXISTS new_column VARCHAR(50);
```

### Add Index
```sql
CREATE INDEX IF NOT EXISTS idx_table_column ON existing_table(column);
```

### Add Unique/FK Constraint (idempotent pair)
```sql
ALTER TABLE child_table
    DROP CONSTRAINT IF EXISTS fk_child_parent;
ALTER TABLE child_table
    ADD CONSTRAINT fk_child_parent
    FOREIGN KEY (parent_id) REFERENCES parent_table(id) ON DELETE CASCADE;
```

## After Creating

1. `python scripts/check_migrations.py` — naming, chain, guards
2. Apply locally: `examples/pay-server/scripts/setup_database.sh`
   (or `.bat`), which resets the schema and replays the whole chain, so the
   new file is exercised from scratch rather than on top of drift
3. Run the suite: `ctest --test-dir build/<preset> --output-on-failure`
4. If ORM models need updating: `/orm-gen`
5. Update `libs/drogon-pay/model.json` if new tables were added
6. Add a line to `CHANGELOG.md` under `[Unreleased]`
7. Once the migration has shipped, pin it:
   `python scripts/check_migrations.py --write-missing`
