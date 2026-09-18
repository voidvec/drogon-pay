#!/usr/bin/env python3
"""Versioned migration executor for the pay schema (CI + local).

Before this script the migration chain had three separate consumers — the
Linux CI loop, the coverage workflow and `setup_database.bat` — each
carrying its own hardcoded file list, and one of them (coverage) silently
skipped `003`/`004`. This is the single implementation all three call.

How it works:

  * `sql/NNN_name.sql` is one migration; `NNN` is its version.
  * `sql/000_*.sql` is a dev *reset helper*, deliberately outside the
    version chain, and is never applied by this tool.
  * Applied versions live in the `schema_migrations` table
    (version, filename, sha256, applied_at).
  * Each pending migration runs inside one transaction together with its
    bookkeeping row, so a half-applied migration is never recorded.
  * A file whose version is already applied must keep its exact bytes:
    the recorded sha256 is compared on every run and a mismatch fails.
    Fix forward with a new migration, never by editing history.
  * `--baseline` records the whole on-disk chain as applied without
    running it, for a database already provisioned out-of-band (the
    docker-compose `initdb.d` path). It refuses unless every table the
    chain would create already exists, so it cannot paper over an empty
    schema.

Credentials are read from the environment (PGPASSWORD) or `--env-file` and
passed to psql through its own environment only: a password never reaches
a command line, CI log output, or the process list.

Usage:
    python scripts/migrate_db.py                  # apply pending
    python scripts/migrate_db.py --dry-run        # show the plan only
    python scripts/migrate_db.py --status         # what is applied
    python scripts/migrate_db.py --baseline       # adopt an existing db
    python scripts/migrate_db.py --reset-schema --confirm-drop pay_test
                                                  # dev reset, then replay the
                                                  # chain (setup_database.{bat,sh})
                                                  # loopback host only, by design

Exit code 0 = nothing to apply or applied cleanly; 1 = refused/failed.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SQL_DIR = REPO_ROOT / "sql"
TABLE = "schema_migrations"
RESET_PREFIX = "000_"  # dev reset helper, not a version

MIGRATION_NAME_RE = re.compile(r"^(\d{3})_([a-z0-9][a-z0-9_]*)\.sql$")
DBNAME_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_$]*$")
CREATE_TABLE_RE = re.compile(
    r"CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_]\w*)", re.I
)

ENSURE_TABLE_SQL = f"""
CREATE TABLE IF NOT EXISTS {TABLE} (
    version    TEXT PRIMARY KEY,
    filename   TEXT NOT NULL,
    sha256     TEXT NOT NULL,
    applied_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);
"""


class MigrationError(RuntimeError):
    pass


# ------------------------------------------------------------------ discovery

def discover(sql_dir: Path) -> list[tuple[str, Path]]:
    """Return (version, path) for the versioned chain, in apply order."""
    root = sql_dir if sql_dir.is_absolute() else REPO_ROOT / sql_dir
    if not root.is_dir():
        raise MigrationError(f"{sql_dir} is not a directory")
    found: list[tuple[str, Path]] = []
    for path in sorted(root.glob("*.sql")):
        if path.name.startswith(RESET_PREFIX):
            continue
        m = MIGRATION_NAME_RE.match(path.name)
        if not m:
            raise MigrationError(
                f"{rel(path)}: migration filenames must be "
                f"NNN_lower_snake_case.sql (see sql/001_init_pay_tables.sql)"
            )
        found.append((m.group(1), path))
    versions = [v for v, _ in found]
    dupes = sorted({v for v in versions if versions.count(v) > 1})
    if dupes:
        raise MigrationError(f"duplicate migration versions: {dupes}")
    return found


def rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def crlf_variant_of(path: Path) -> str:
    """Digest the file would have if read from a CRLF working copy."""
    raw = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha256(raw.replace(b"\n", b"\r\n")).hexdigest()


def tables_created_by(chain: list[tuple[str, Path]]) -> set[str]:
    tables: set[str] = set()
    for _, path in chain:
        text = path.read_text(encoding="utf-8", errors="replace")
        tables.update(m.lower() for m in CREATE_TABLE_RE.findall(text))
    return tables


# ------------------------------------------------------------------- psql glue

def parse_env_file(path: Path) -> dict[str, str]:
    """Read a KEY=VALUE file, ignoring comments and blank lines."""
    if not path.is_file():
        raise MigrationError(f"--env-file {rel(path)} does not exist")
    out: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, _, value = line.partition("=")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        out[key.strip()] = value
    return out


def build_env(args: argparse.Namespace) -> dict[str, str]:
    """psql's environment: process env, filled in by --env-file, then flags."""
    env = dict(os.environ)
    if args.env_file:
        for key, value in parse_env_file(Path(args.env_file)).items():
            env.setdefault(key, value)
    # The app's own config name for the same secret, so `--env-file
    # examples/pay-server/.env` is enough to reach the dev database.
    if not env.get("PGPASSWORD") and env.get("PAY_DB_PASSWORD"):
        env["PGPASSWORD"] = env["PAY_DB_PASSWORD"]
    for key, value in (
        ("PGHOST", args.host),
        ("PGPORT", args.port),
        ("PGUSER", args.user),
        ("PGDATABASE", args.db),
    ):
        if value:
            env[key] = str(value)
    return env


class Psql:
    """One psql invocation style, shared by every query in a run."""

    def __init__(self, env: dict[str, str]):
        self.env = env
        self.argv = [
            "psql", "-X", "-q",
            "-v", "ON_ERROR_STOP=1",
            "-h", env.get("PGHOST", "127.0.0.1"),
            "-p", env.get("PGPORT", "5432"),
            "-U", env.get("PGUSER", "test"),
            "-d", env.get("PGDATABASE", "pay_test"),
        ]

    def run(self, sql: str, *, fetch: bool = False) -> str:
        argv = self.argv + (["-At", "-F", "|"] if fetch else [])
        try:
            proc = subprocess.run(
                argv, input=sql, text=True, encoding="utf-8",
                errors="replace", capture_output=True, env=self.env,
            )
        except FileNotFoundError:
            raise MigrationError(
                "psql not found on PATH (install the postgresql client)"
            )
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout or "").strip()
            raise MigrationError(
                f"psql exited {proc.returncode}:\n"
                + "\n".join("    " + line for line in detail.splitlines())
            )
        return proc.stdout


# ------------------------------------------------------------------- state

def read_applied(psql: Psql) -> dict[str, str]:
    """version -> recorded sha256 (creates the bookkeeping table if absent)."""
    psql.run(ENSURE_TABLE_SQL)
    rows = psql.run(f"SELECT version, sha256 FROM {TABLE} ORDER BY version;",
                    fetch=True)
    applied: dict[str, str] = {}
    for row in rows.splitlines():
        version, _, digest = row.partition("|")
        if version.strip():
            applied[version.strip()] = digest.strip()
    return applied


def check_applied_files(chain: list[tuple[str, Path]],
                        applied: dict[str, str]) -> list[str]:
    """An applied version whose file changed or vanished is a hard error."""
    problems: list[str] = []
    by_version = {v: p for v, p in chain}
    for version, digest in sorted(applied.items()):
        path = by_version.get(version)
        if path is None:
            problems.append(
                f"{TABLE} records version {version} but no sql/{version}_*.sql "
                f"exists on disk — restore the file; deleting applied history "
                f"is not a rollback"
            )
            continue
        actual = sha256_of(path)
        if digest and actual != digest:
            hint = ""
            if crlf_variant_of(path) == digest:
                hint = (
                    " The two digests differ only in line endings: this ledger "
                    "row was written from a CRLF checkout, before "
                    ".gitattributes pinned eol=lf. Replay the chain on the dev "
                    "database (examples/pay-server/scripts/setup_database.bat "
                    "or .sh) to re-record it; do not edit the migration."
                )
            problems.append(
                f"{rel(path)} changed after it was applied (recorded "
                f"{digest[:12]}, on disk {actual[:12]}). Applied migrations are "
                f"immutable: revert the edit, or fix forward with a new NNN_ "
                f"migration.{hint}"
            )
    return problems


def pending_of(chain, applied):
    return [(v, p) for v, p in chain if v not in applied]


# ----------------------------------------------------------------- operations

def apply_one(psql: Psql, version: str, path: Path, digest: str) -> None:
    body = path.read_text(encoding="utf-8")
    sql = (
        "BEGIN;\n"
        f"{body}\n"
        f"INSERT INTO {TABLE} (version, filename, sha256) "
        f"VALUES ('{version}', '{path.name}', '{digest}');\n"
        "COMMIT;\n"
    )
    psql.run(sql)


def baseline_one(psql: Psql, version: str, path: Path, digest: str) -> None:
    psql.run(
        f"INSERT INTO {TABLE} (version, filename, sha256) "
        f"VALUES ('{version}', '{path.name}', '{digest}');"
    )


def present_tables(psql: Psql) -> set[str]:
    rows = psql.run(
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema NOT IN ('pg_catalog', 'information_schema');",
        fetch=True,
    )
    return {line.strip().lower() for line in rows.splitlines() if line.strip()}


def check_applied_objects(chain: list[tuple[str, Path]],
                          applied: dict[str, str],
                          tables: set[str]) -> list[str]:
    """History that claims objects the schema no longer has is a lie.

    The dev reset helper (sql/000_*) drops the pay_* tables but knows nothing
    about schema_migrations, so after it the bookkeeping still says 001-004 are
    applied. Reporting "nothing to apply" against an empty schema at that point
    would hand the test suite a database that fails in a confusing way.
    """
    if not applied:
        return []
    applied_chain = [(v, p) for v, p in chain if v in applied]
    expected = tables_created_by(applied_chain)
    missing = sorted(expected - tables)
    if not missing:
        return []
    return [
        f"schema_migrations says {', '.join(sorted(applied))} are applied, but "
        f"these tables are absent: {', '.join(missing)}. Something dropped "
        f"objects out-of-band (sql/{RESET_PREFIX}* is the usual suspect). "
        f"Replay the chain with --reset-schema, or delete the stale "
        f"schema_migrations rows if this database is intentionally empty."
    ]


def assert_schema_present(psql: Psql, chain: list[tuple[str, Path]]) -> None:
    """--baseline is only honest if the objects the chain creates exist."""
    tables = tables_created_by(chain)
    if not tables:
        raise MigrationError("--baseline: no CREATE TABLE found in the chain")
    missing = sorted(tables - present_tables(psql))
    if missing:
        raise MigrationError(
            "--baseline refuses to run: the database is not already at the "
            f"head of the chain (missing table(s): {', '.join(missing)}). "
            f"Drop the flag to apply the migrations for real."
        )


def _is_loopback(host: str) -> bool:
    """True for the addresses a developer's own database is reachable at."""
    if host == "" or host.lower() == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def reset_public_schema(psql: Psql, dbname: str, confirm: str) -> None:
    """Empty the target schema so the chain can be replayed from scratch.

    This is the dev-reset primitive, and it is deliberately *not*
    DROP DATABASE: the old setup_database.bat dropped the database as the app
    role, then could not recreate it (no CREATEDB), which left the developer
    with neither. Dropping the schema keeps the provisioned database — whose
    creation belongs to provisioning, not to migrations — and needs only the
    privileges the owner already has.

    Two independent guards, because `setup_database.{sh,bat}` fills in
    `--confirm-drop` for the operator, so the name alone is not a human
    decision: the repeated name catches a mistyped `--db`, and the loopback
    rule keeps this dev primitive away from any remote server, where staging
    and production actually live.
    """
    host = psql.env.get("PGHOST", "127.0.0.1")
    if not _is_loopback(host):
        raise MigrationError(
            f"--reset-schema refuses to run against host {host!r}: this is a "
            "loopback-only dev reset, and a remote host is where staging and "
            "production live. Point PGHOST at 127.0.0.1, or migrate a real "
            "environment forward with a versioned sql/NNN_*.sql instead."
        )
    if confirm != dbname:
        raise MigrationError(
            f"--reset-schema also requires --confirm-drop {dbname} (you passed "
            f"{confirm or 'nothing'}); this deletes every object in "
            f"{dbname}'s public schema."
        )
    psql.run("DROP SCHEMA IF EXISTS public CASCADE;\nCREATE SCHEMA public;")
    print(f"Reset the public schema of {dbname} on {host}.")


def probe_database(psql: Psql, dbname: str, user: str,
                   maintenance_db: str) -> None:
    """Turn 'connection failed' into the provisioning step the reader needs.

    A missing database is not something migrations can fix: creating one takes
    CREATEDB, which the application role deliberately does not have. The
    answer comes from the server catalogue rather than from psql's message
    text, because that text is localized (and on a Chinese Windows console it
    does not even survive decoding as UTF-8).
    """
    try:
        psql.run("SELECT 1;", fetch=True)
        return
    except MigrationError as exc:
        original = exc

    try:
        admin_env = dict(psql.env)
        admin_env["PGDATABASE"] = maintenance_db
        if not DBNAME_RE.match(dbname):
            raise MigrationError(f"not querying the catalogue about {dbname!r}")
        rows = Psql(admin_env).run(
            f"SELECT 1 FROM pg_database WHERE datname = '{dbname}';",
            fetch=True,
        )
    except MigrationError:
        raise original
    if rows.strip():
        raise original
    raise MigrationError(
        f"database {dbname!r} does not exist on the target server. Creating "
        f"it is a provisioning step, not a migration; run it once with a role "
        f"that has CREATEDB:\n"
        f'    psql -U postgres -d {maintenance_db} -c '
        f'"CREATE DATABASE {dbname} OWNER {user}"'
    )


def print_status(applied: dict[str, str], chain) -> None:
    if not applied:
        print(f"{TABLE} is empty — every migration is pending.")
    for version, path in chain:
        state = "applied" if version in applied else "pending"
        print(f"  {version}  {path.name:<40} {state}")


# ---------------------------------------------------------------------- main

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--sql-dir", default=str(SQL_DIR))
    ap.add_argument("--host", default=None, help="overrides PGHOST")
    ap.add_argument("--port", default=None, help="overrides PGPORT")
    ap.add_argument("--user", default=None, help="overrides PGUSER")
    ap.add_argument("--db", default=None, help="overrides PGDATABASE")
    ap.add_argument("--env-file", default=None,
                    help="KEY=VALUE file (e.g. examples/pay-server/.env); "
                         "values stay in psql's environment, never printed")
    ap.add_argument("--dry-run", action="store_true",
                    help="print what would apply, write nothing")
    ap.add_argument("--baseline", action="store_true",
                    help="record the on-disk chain as applied without running it")
    ap.add_argument("--reset-schema", action="store_true",
                    help="drop and recreate the target database's public schema "
                         "first (loopback dev reset; needs --confirm-drop)")
    ap.add_argument("--confirm-drop", default=None,
                    help="must repeat the database name for --reset-schema")
    ap.add_argument("--maintenance-db", default="postgres",
                    help="database used for the existence probe only")
    ap.add_argument("--status", action="store_true")
    args = ap.parse_args(argv)

    try:
        env = build_env(args)
        psql = Psql(env)
        dbname = env.get("PGDATABASE", "pay_test")
        chain = discover(Path(args.sql_dir))

        probe_database(psql, dbname, env.get("PGUSER", "test"),
                       args.maintenance_db)

        if args.reset_schema:
            if args.dry_run:
                print(f"--reset-schema --dry-run: would drop and recreate the "
                      f"public schema of {dbname}, then apply "
                      f"{len(chain)} migration(s).")
                return 0
            reset_public_schema(psql, dbname, args.confirm_drop or "")
        elif args.confirm_drop:
            raise MigrationError(
                "--confirm-drop only means something with --reset-schema")

        applied = read_applied(psql)

        problems = check_applied_files(chain, applied)
        if args.status:
            problems += check_applied_objects(
                chain, applied, present_tables(psql))
            print_status(applied, chain)
            for problem in problems:
                print(f"  WARNING: {problem}")
            return 1 if problems else 0
        if not args.dry_run:
            problems += check_applied_objects(
                chain, applied, present_tables(psql))
        if problems:
            print("Migration state FAILED:")
            for problem in problems:
                print(f"  {problem}")
            return 1

        pending = pending_of(chain, applied)
        if not pending:
            head = chain[-1][0] if chain else "-"
            print(f"Nothing to apply: {len(chain)} migration(s) recorded, "
                  f"schema at head {head}.")
            return 0

        if args.baseline:
            assert_schema_present(psql, chain)
            for version, path in pending:
                baseline_one(psql, version, path, sha256_of(path))
                print(f"  baselined {path.name}")
            print(f"--baseline recorded {len(pending)} migration(s) as applied "
                  f"without executing them.")
            return 0

        verb = "would apply" if args.dry_run else "applying"
        print(f"{len(pending)} migration(s) pending, {verb}:")
        for version, path in pending:
            print(f"  {version}  {path.name}")
            if args.dry_run:
                continue
            apply_one(psql, version, path, sha256_of(path))
            print(f"         applied {path.name}")
        if args.dry_run:
            print("--dry-run: no changes written.")
        else:
            print(f"Applied {len(pending)} migration(s); schema at head "
                  f"{pending[-1][0]}.")
        return 0
    except MigrationError as exc:
        print(f"Migration run FAILED: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
