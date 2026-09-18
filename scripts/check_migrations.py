#!/usr/bin/env python3
"""Migration hygiene guard for sql/ (CI gate + local check).

Rules, in the order they fire:

  R1 naming     every file in sql/ is NNN_lower_snake_case.sql; exactly one
                000_ reset helper lives outside the version chain.
  R2 chain      versions are 001..N with no gaps and no duplicates, so
                "apply everything after 003" has an unambiguous meaning.
  R3 immutability  scripts/migrations_baseline.json pins the sha256 of the
                pre-tooling migrations. A baselined file may not change
                bytes; editing applied history is how environments silently
                diverge. Re-baseline by hand in the same PR to accept a
                deliberate change (deliberate friction, same idiom as
                PUBLIC_API_WHITELIST in check_architecture.py). Because a
                pinned file is exempt from R4/R5 for good, --write-missing
                runs those rules over its candidates and refuses the write:
                pinning a new migration must not be a self-granted waiver.
  R4 idempotent  new migrations (not in the baseline) must guard the objects
                they create: IF NOT EXISTS, CREATE OR REPLACE, a paired
                DROP ... IF EXISTS, or an IF NOT EXISTS check inside a DO
                block; a top-level INSERT needs ON CONFLICT DO NOTHING. A
                migration that dies on second run cannot be replayed onto a
                partially provisioned host.
  R5 non-destructive  new migrations may not DROP TABLE/DATABASE/SCHEMA,
                TRUNCATE, DELETE FROM, or DROP COLUMN. Retirement is a
                separate, reviewed change; the reset helper 000_ is the
                sanctioned destructive tool for dev databases.

R4/R5 deliberately do not apply to the baselined files: 001 uses
`DROP TRIGGER IF EXISTS` + `CREATE TRIGGER` and 004 uses a DO block, both
idempotent, but nothing in those shipped migrations is going to be re-run
onto a fresh environment through a rule they predate.

Exit code 0 = clean; 1 = violations (printed one per line).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SQL_DIR = REPO_ROOT / "sql"
BASELINE = REPO_ROOT / "scripts" / "migrations_baseline.json"
RESET_PREFIX = "000_"

NAME_RE = re.compile(r"^(\d{3})_([a-z0-9][a-z0-9_]*)\.sql$")

# ------------------------------------------------------------------ SQL parsing

LINE_COMMENT_RE = re.compile(r"--[^\n]*")
BLOCK_COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)
DOLLAR_TAG_RE = re.compile(r"\$(\w*)\$")


def strip_comments(text: str) -> str:
    """Blank out comments so rules never match prose in a migration header."""
    text = BLOCK_COMMENT_RE.sub(lambda m: "\n" * m.group(0).count("\n"), text)
    return LINE_COMMENT_RE.sub("", text)


def split_statements(text: str) -> list[tuple[int, str]]:
    """Split on top-level semicolons, keeping each statement's start offset.

    Semicolons inside single-quoted strings and inside dollar-quoted bodies
    (`$$ ... $$`, `$tag$ ... $tag$`) belong to the literal, so they do not
    end a statement — without this, 001's trigger function and 004's DO
    block would be shredded into non-statements.
    """
    statements: list[tuple[int, str]] = []
    start = 0
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if ch == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 1
        elif ch == "$":
            tag = DOLLAR_TAG_RE.match(text, i)
            if tag:
                close = text.find(tag.group(0), i + len(tag.group(0)))
                if close >= 0:
                    i = close + len(tag.group(0))
                    continue
        elif ch == ";":
            chunk = text[start:i].strip()
            if chunk:
                statements.append((start, chunk))
            start = i + 1
        i += 1
    tail = text[start:].strip()
    if tail:
        statements.append((start, tail))
    return statements


def is_inside_do(text: str, offset: int) -> bool:
    """True when offset falls within a DO <dollar-quoted> block."""
    for m in re.finditer(r"\bDO\b\s*(\$\w*\$)", text):
        opener = m.group(1)
        close = text.find(opener, m.end())
        if close < 0:
            continue
        if m.start() <= offset <= close + len(opener):
            return True
    return False


def object_name(stmt: str, kind: str) -> str | None:
    patterns = {
        "index": r"CREATE\s+(?:UNIQUE\s+)?INDEX\s+(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_]\w*)",
        "table": r"CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?([A-Za-z_][\w.]*)",
        "trigger": r"CREATE\s+(?:OR\s+REPLACE\s+)?TRIGGER\s+([A-Za-z_]\w*)",
        "constraint": r"ADD\s+CONSTRAINT\s+([A-Za-z_]\w*)",
    }
    m = re.search(patterns[kind], stmt, re.I)
    return m.group(1).split(".")[-1] if m else None


CREATING_KINDS = (
    (re.compile(r"\bCREATE\s+(?:UNIQUE\s+)?INDEX\b", re.I), "index", "CREATE INDEX"),
    (re.compile(r"\bCREATE\s+TABLE\b", re.I), "table", "CREATE TABLE"),
    (re.compile(r"\bCREATE\s+(?:OR\s+REPLACE\s+)?TRIGGER\b", re.I), "trigger",
     "CREATE TRIGGER"),
    (re.compile(r"\bALTER\s+TABLE\b[\s\S]*?\bADD\s+CONSTRAINT\b", re.I),
     "constraint", "ALTER TABLE ... ADD CONSTRAINT"),
)

INSERT_RE = re.compile(r"\bINSERT\b", re.I)
ON_CONFLICT_RE = re.compile(r"\bON\s+CONFLICT\b", re.I)

DROP_IF_EXISTS_RE = re.compile(
    r"\bDROP\s+(?:TABLE|INDEX|TRIGGER|CONSTRAINT)\s+(?:IF\s+EXISTS\s+)?"
    r"(?:ONLY\s+)?([A-Za-z_][\w.]*)", re.I
)

DESTRUCTIVE = (
    (re.compile(r"\bDROP\s+TABLE\b", re.I), "DROP TABLE"),
    (re.compile(r"\bDROP\s+DATABASE\b", re.I), "DROP DATABASE"),
    (re.compile(r"\bDROP\s+SCHEMA\b", re.I), "DROP SCHEMA"),
    (re.compile(r"\bTRUNCATE\b", re.I), "TRUNCATE"),
    (re.compile(r"\bDELETE\s+FROM\b", re.I), "DELETE FROM"),
    (re.compile(r"\bALTER\s+TABLE\b[\s\S]*?\bDROP\s+COLUMN\b", re.I),
     "ALTER TABLE ... DROP COLUMN"),
)


# ---------------------------------------------------------------------- rules

def sha256_of(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_baseline(path: Path) -> dict[str, str]:
    if not path.is_file():
        return {}
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path.name} must contain a JSON object")
    return {k: v for k, v in data.items() if not k.startswith("_")}


def check_naming(files: list[Path]) -> list[str]:
    errors: list[str] = []
    for path in files:
        if not NAME_RE.match(path.name):
            errors.append(
                f"[rule1 naming] sql/{path.name}: expected NNN_lower_snake.sql "
                f"(three-digit version, then a lowercase identifier)"
            )
    resets = [p.name for p in files if p.name.startswith(RESET_PREFIX)]
    if len(resets) > 1:
        errors.append(
            f"[rule1 naming] only one 000_ reset helper is allowed, found "
            f"{resets}"
        )
    return errors


def check_chain(files: list[Path]) -> list[str]:
    versions = sorted(
        m.group(1) for m in (NAME_RE.match(p.name) for p in files) if m
    )
    errors: list[str] = []
    seen: set[str] = set()
    for v in versions:
        if v in seen:
            errors.append(f"[rule2 chain] duplicate version {v}")
        seen.add(v)
    numbered = sorted(v for v in seen if not v.startswith("000"))
    if numbered:
        expected = [f"{i:03d}" for i in range(int(numbered[0]), int(numbered[-1]) + 1)]
        for gap in sorted(set(expected) - set(numbered)):
            errors.append(
                f"[rule2 chain] version {gap} is missing between "
                f"{numbered[0]} and {numbered[-1]} — never renumber or reuse a "
                f"version, add the next one"
            )
        if numbered[0] != "001":
            errors.append(
                f"[rule2 chain] the versioned chain should start at 001, found "
                f"{numbered[0]} (000_ is the reset helper and does not count)"
            )
    return errors


def check_baseline(files: list[Path], baseline: dict[str, str]) -> list[str]:
    errors: list[str] = []
    on_disk = {p.name: p for p in files}
    if not baseline:
        return [
            "[rule3 immutability] scripts/migrations_baseline.json is missing "
            "or empty — run `python scripts/check_migrations.py --write-missing` "
            "and commit the result"
        ]
    for name, digest in sorted(baseline.items()):
        path = on_disk.get(name)
        if path is None:
            errors.append(
                f"[rule3 immutability] baseline pins sql/{name} but the file is "
                f"gone — applied history was deleted, which is not a rollback"
            )
            continue
        actual = sha256_of(path)
        if actual != digest:
            errors.append(
                f"[rule3 immutability] sql/{name} changed after being baselined "
                f"(pinned {digest[:12]}, on disk {actual[:12]}). Applied "
                f"migrations are immutable: revert the edit, or fix forward "
                f"with a new NNN_ migration."
            )
    return errors


def check_content(path: Path) -> list[str]:
    """Idempotency + non-destruction, for migrations not in the baseline."""
    errors: list[str] = []
    raw = path.read_text(encoding="utf-8", errors="replace")
    text = strip_comments(raw)
    statements = split_statements(text)

    dropped: set[str] = set()
    for _, stmt in statements:
        for m in DROP_IF_EXISTS_RE.finditer(stmt):
            if re.search(r"\bIF\s+EXISTS\b", stmt, re.I):
                dropped.add(m.group(1).split(".")[-1].lower())

    for offset, stmt in statements:
        for pattern, kind, label in CREATING_KINDS:
            if not pattern.search(stmt):
                continue
            guarded = (
                re.search(r"\bIF\s+NOT\s+EXISTS\b", stmt, re.I)
                or re.search(r"\bCREATE\s+OR\s+REPLACE\b", stmt, re.I)
                or (kind == "trigger" and re.search(
                    r"\bCREATE\s+OR\s+REPLACE\s+TRIGGER\b", stmt, re.I))
            )
            name = object_name(stmt, kind)
            if not guarded and name and name.lower() in dropped:
                guarded = True
            if not guarded and is_inside_do(text, offset):
                do_body = _enclosing_do_body(text, offset)
                if do_body and re.search(r"\bIF\s+NOT\s+EXISTS\b", do_body, re.I):
                    guarded = True
            if not guarded:
                target = f" {name}" if name else ""
                errors.append(
                    f"[rule4 idempotent] sql/{path.name}:{target} {label} has "
                    f"no guard. Use IF NOT EXISTS, CREATE OR REPLACE, a paired "
                    f"DROP ... IF EXISTS, or an IF NOT EXISTS check inside a DO "
                    f"block — this file must survive a second run."
                )
            break

        if INSERT_RE.search(stmt) and stmt.lstrip().upper().startswith("INSERT") \
                and not ON_CONFLICT_RE.search(stmt):
            errors.append(
                f"[rule4 idempotent] sql/{path.name}: top-level INSERT has no "
                f"ON CONFLICT DO NOTHING, so the second run of this file "
                f"raises a unique-violation error."
            )

        for pattern, label in DESTRUCTIVE:
            if pattern.search(stmt):
                errors.append(
                    f"[rule5 destructive] sql/{path.name}: {label} is not "
                    f"allowed in a versioned migration. Retirement is its own "
                    f"reviewed change; sql/{RESET_PREFIX}* is the dev reset tool."
                )
                break
    return errors


def _enclosing_do_body(text: str, offset: int) -> str | None:
    for m in re.finditer(r"\bDO\b\s*(\$\w*\$)", text):
        opener = m.group(1)
        close = text.find(opener, m.end())
        if close < 0:
            continue
        if m.start() <= offset <= close + len(opener):
            return text[m.end():close]
    return None


# ----------------------------------------------------------------------- main

def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--sql-dir", type=Path, default=SQL_DIR)
    ap.add_argument("--baseline", type=Path, default=BASELINE)
    ap.add_argument("--write-missing", action="store_true",
                    help="add baseline entries for files not pinned yet; never "
                         "rewrites an existing entry, and refuses a candidate "
                         "that breaks the naming/idempotency/destruction rules")
    args = ap.parse_args(argv)

    try:
        baseline = load_baseline(args.baseline)
    except (OSError, ValueError) as exc:
        print(f"Migration hygiene guard FAILED: {exc}")
        return 1

    files = sorted(p for p in args.sql_dir.glob("*.sql") if p.is_file())

    if args.write_missing:
        added, refused = write_missing(files, baseline, args.baseline)
        if refused:
            print(f"--write-missing refused to pin {len(refused)} violation(s):")
            for problem in refused:
                print("  " + problem)
            print("A pinned file is exempt from the content rules for good, so "
                  "pinning is not a waiver: fix the SQL, or leave the file "
                  "unpinned and let the rules keep applying to it.")
            return 1
        if not added:
            print(f"{args.baseline.name}: every sql/*.sql file is already "
                  f"pinned; nothing to write.")
            return 0
        print(f"{args.baseline.name}: pinned {', '.join(added)}")
        return 0

    errors = check_naming(files) + check_chain(files) + check_baseline(files, baseline)
    for path in files:
        if path.name.startswith(RESET_PREFIX):
            continue
        if path.name in baseline:
            continue
        errors += check_content(path)

    if errors:
        print(f"Migration hygiene guard FAILED ({len(errors)} violation(s)):")
        for e in errors:
            print("  " + e)
        return 1
    new_files = [p.name for p in files
                 if not p.name.startswith(RESET_PREFIX) and p.name not in baseline]
    print(
        f"Migration hygiene guard passed: {len(files)} file(s) in sql/, "
        f"{len(baseline)} baselined, "
        f"{len(new_files)} subject to the content rules "
        f"({', '.join(new_files) if new_files else 'none'})."
    )
    return 0


def write_missing(files: list[Path], baseline: dict[str, str],
                  path: Path) -> tuple[list[str], list[str]]:
    """Pin the not-yet-pinned files, but only those that pass the rules.

    check_baseline exempts a pinned file from the content rules forever (that is
    how the pre-existing chain stays untouched), so pinning first and reviewing
    later would be a self-granted waiver: run the same rules over the candidates
    and refuse the write when any of them breaks them.
    """
    candidates = [f for f in files if f.name not in baseline]
    refused: list[str] = []
    for new_file in candidates:
        if new_file.name.startswith(RESET_PREFIX):
            continue
        refused += check_naming([new_file]) + check_content(new_file)
    if refused:
        return [], refused

    added: list[str] = []
    payload = {k: v for k, v in baseline.items()}
    if not payload:
        payload = {
            "_readme": (
                "sha256 of every sql/*.sql frozen when migrate_db.py landed. "
                "check_migrations.py fails if a pinned file changes; add an "
                "entry with --write-missing once a new migration has shipped "
                "(it refuses a file that breaks the content rules, because a "
                "pinned file is exempt from them for good)."
            )
        }
    for f in files:
        if f.name not in payload:
            payload[f.name] = sha256_of(f)
            added.append(f.name)
    if not added:
        return [], []
    path.write_text(
        json.dumps(dict(sorted(payload.items())), indent=2) + "\n",
        encoding="utf-8",
    )
    return added, []


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
