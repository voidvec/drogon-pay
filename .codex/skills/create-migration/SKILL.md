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

Pattern: `sql/{NNN}_{snake_case}.sql` (three digits, single underscore).

Current chain: `001_init_pay_tables` .. `004_ledger_fk` — next migration
number is **005**.

`000_drop_pay_tables.sql` is a **dev reset helper**, not part of the version
chain: never add schema changes to it, never renumber the chain to 000.

Always check before creating:
```bash
ls sql/
```

## How Migrations Are Applied

- **CI**: `.github/workflows/ci-linux.yml` applies `sql/*.sql` in numeric
  order with `psql -f` against a fresh database.
- **Local docker**: `examples/pay-server/docker-compose.yml` mounts `sql/` as
  `docker-entrypoint-initdb.d` (only runs on a brand-new volume).
- **Manual**: `examples/pay-server/scripts/setup_database.bat` (Windows) or
  `psql -f sql/00N_xxx.sql`.

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

1. **Naming**: `NNN_snake_case.sql`, next number after the latest migration
2. **Idempotent**: `CREATE TABLE/INDEX ... IF NOT EXISTS`,
   `ADD COLUMN IF NOT EXISTS`; `ADD CONSTRAINT x` must be paired with
   `DROP CONSTRAINT IF EXISTS x` in the same file; top-level `INSERT` needs
   `ON CONFLICT DO NOTHING`
3. **Non-destructive**: no `DROP TABLE/COLUMN`, `TRUNCATE`, or bare
   `DELETE FROM` (only `000_` may drop objects)
4. **ORM consistency**: schema must match `libs/drogon-pay/model.json`;
   regenerate models via `/orm-gen` (`generate_models.bat`) after schema
   changes — never hand-edit `src/models/`
5. **Existing files immutable**: never edit an applied `00N_*.sql`; add a
   new migration instead

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

1. Apply locally and run the suite: `ctest --test-dir build/<preset> ...`
2. If ORM models need updating: `/orm-gen`
3. Update `libs/drogon-pay/model.json` if new tables were added
4. Add a line to `CHANGELOG.md` under `[Unreleased]`
