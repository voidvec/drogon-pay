#!/usr/bin/env python3
"""clang-tidy hard gate (two-tier adoption, see .clang-tidy).

The repo-wide .clang-tidy ruleset stays ADVISORY: agents and developers see
all bugprone/modernize/performance findings without being blocked.

This script promotes CHECKS TO A HARD GATE in batches. A check is only
promotable once a `--report` run shows zero findings for it across all
first-party sources; then it is added to PROMOTED_CHECKS below, where it
runs with --warnings-as-errors and fails CI on any hit.

Usage:
  python3 scripts/clang_tidy_gate.py                 # hard gate on PROMOTED_CHECKS
  python3 scripts/clang_tidy_gate.py --report        # per-check hit counts (full advisory set)
  python3 scripts/clang_tidy_gate.py --compile-db build/linux-release

Exit codes: 0 = gate pass (or report), 1 = gate violation / setup error.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

# First-party trees the gate covers (mirrors .clang-tidy HeaderFilterRegex).
SCAN_DIRS = ("libs/drogon-pay", "examples/pay-server", "tests")

# Generated code is excluded from BOTH tiers: drogon_ctl output must not be
# hand-edited to satisfy a lint (see AGENTS.md hard constraint).
EXCLUDE_PARTS = {"models"}

# Checks promoted to the hard gate, in batches, each verified at zero
# findings by `--report` before being listed here. Keep the family prefix
# explicit (no wildcards) so a new clang-tidy release cannot silently
# widen the gate.
PROMOTED_CHECKS = [
    # Batch 1 (measured zero-finding via --report on clang-tidy 22.1.3;
    # re-verify whenever the list or the toolchain major changes):
    "bugprone-assert-side-effect",
    "bugprone-dangling-handle",
    "bugprone-infinite-loop",
    "bugprone-misplaced-widening-cast",
    "bugprone-sizeof-expression",
    "bugprone-suspicious-string-compare",
    "bugprone-undelegated-constructor",
    "performance-move-const-arg",
    "performance-no-int-to-ptr",
    # Next promotion candidates from the same report (fix findings first):
    # performance-unnecessary-value-param (21), modernize-use-scoped-lock (7),
    # bugprone-branch-clone (5).
]

# Checks --report should sweep even if they are not in .clang-tidy's set yet
# (report is advisory-only, used to pick the next promotion batch).
REPORT_CHECKS = "-*,bugprone-*,modernize-*,performance-*"

FINDING_RE = re.compile(r": (?:warning|error): .* \[([A-Za-z0-9_.+-]+)\]\s*$")
PARSE_ERROR_RE = re.compile(r"\[clang-diagnostic-error\]")


def load_entries(compile_db: Path) -> list[dict]:
    if not compile_db.is_file():
        die(
            f"compile database not found: {compile_db}\n"
            "Run `cmake --preset linux-release` (CMAKE_EXPORT_COMPILE_COMMANDS=ON)."
        )
    return json.loads(compile_db.read_text(encoding="utf-8"))


def gate_db_dir(compile_db: Path, entries: list[dict]) -> tuple[Path, str]:
    """Multi-config databases (Ninja Multi-Config / VS) store one entry per
    file per config, and only the config matching the Conan dependency build
    (Release) carries complete include flags. clang-tidy has no flag to
    select an entry by config (it always reads compile_commands.json from
    the -p directory and takes the first match), so prune to Release and
    write the subset next to the original db. Returns the directory to pass
    to -p and the config label for logging."""
    files = {e["file"] for e in entries}
    intdir = sum(1 for e in entries if "CMAKE_INTDIR" in e.get("command", ""))
    if intdir <= len(files):
        return compile_db.parent, ""
    release = []
    seen: set[str] = set()
    for e in entries:
        if not any(seg in e.get("output", "").replace("\\", "/") for seg in ("/Release/", "Release/")):
            continue
        if e["file"] in seen:
            continue
        seen.add(e["file"])
        release.append(e)
    if not release:
        die("multi-config compile db has no Release entries; configure with a Release build type")
    pruned_dir = compile_db.parent / ".gate-release-db"
    pruned_dir.mkdir(exist_ok=True)
    bs, q = chr(92), chr(34)
    fi_quoted = re.compile("/FI" + bs + bs + "?" + q + "(.*?)" + bs + bs + q)
    for e in release:
        # clang's cl-driver misparses /FI\"header\" (JSON-escaped quotes in
        # the command text) as an empty filename; these forced includes carry
        # no spaces, so strip the quotes.
        e["command"] = fi_quoted.sub(
            lambda m: "/FI" + m.group(1).rstrip(bs), e.get("command", "")
        )
    (pruned_dir / "compile_commands.json").write_text(json.dumps(release, indent=0), encoding="utf-8")
    return pruned_dir, "Release (pruned db)"


def candidate_files(entries: list[dict]) -> list[str]:
    """Translation units from the compile db that are first-party, non-model."""
    files: list[str] = []
    seen: set[str] = set()
    for entry in entries:
        src = Path(entry["file"])
        if not src.is_absolute():
            src = Path(entry.get("directory", ".")) / src
        try:
            rel = src.resolve().relative_to(REPO_ROOT.resolve()).as_posix()
        except ValueError:
            continue
        if not rel.startswith(SCAN_DIRS):
            continue
        if Path(rel).suffix not in (".cc", ".cpp"):
            continue
        if any(part in EXCLUDE_PARTS for part in Path(rel).parts):
            continue
        if rel not in seen:
            seen.add(rel)
            files.append(rel)
    if not files:
        die("compile db has no first-party translation units")
    return files


def run_tidy(
    binary: str,
    db_dir: Path,
    checks: str,
    files: list[str],
    as_errors: bool,
) -> tuple[int, tuple[Counter, list]]:
    cmd = [binary, "-p", str(db_dir), f"--checks={checks}", "--quiet"]
    if as_errors:
        cmd.append(f"--warnings-as-errors={checks}")
    cmd.extend(files)
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace")
    hits: Counter = Counter()
    output_lines = []
    parse_errors = 0
    parse_error_lines = []
    for line in (proc.stdout + proc.stderr).splitlines():
        if PARSE_ERROR_RE.search(line):
            parse_errors += 1
            parse_error_lines.append(line)
            continue
        m = FINDING_RE.search(line)
        if m:
            hits[m.group(1)] += 1
            output_lines.append(line)
    if parse_errors:
        for line in parse_error_lines[:10]:
            print(line, file=sys.stderr)
        die(
            f"clang-tidy hit {parse_errors} clang-diagnostic-error(s) (headers not found?): "
            "the compile database is not usable for this generator/config; findings would be meaningless"
        )
    return proc.returncode, (hits, output_lines)


def resolve_binary() -> str:
    env = os.environ.get("CLANG_TIDY_BIN")
    if env:
        return env
    for name in ("clang-tidy-22", "clang-tidy22", "clang-tidy"):
        found = shutil.which(name)
        if found:
            return found
    die("clang-tidy not found; install it or set CLANG_TIDY_BIN")
    return ""  # unreachable


def die(msg: str) -> None:
    print(f"clang-tidy-gate: ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def verify_promoted_names(binary: str) -> None:
    """clang-tidy silently drops unknown check names from --checks, so a
    renamed or mistyped entry would quietly disable its half of the gate.
    Resolve the list through --list-checks and fail on any drop."""
    probe = subprocess.run(
        [binary, "--list-checks", f"--checks=-*,{','.join(PROMOTED_CHECKS)}"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    enabled = {line.strip() for line in probe.stdout.splitlines() if line.startswith("    ")}
    missing = [c for c in PROMOTED_CHECKS if c not in enabled]
    if missing:
        die(
            "promoted check name(s) not registered by this clang-tidy "
            f"({', '.join(missing)}); rename or remove them in PROMOTED_CHECKS"
        )


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--compile-db",
        default=None,
        help="path to compile_commands.json (default: first preset build dir that has one)",
    )
    ap.add_argument("--report", action="store_true", help="sweep the full advisory set and print per-check hit counts")
    args = ap.parse_args()

    if args.compile_db:
        compile_db = Path(args.compile_db).resolve()
    else:
        for preset_dir in ("build/linux-release", "build/macos-arm64", "build/windows-msvc"):
            cand = (REPO_ROOT / preset_dir / "compile_commands.json").resolve()
            if cand.is_file():
                compile_db = cand
                break
        else:
            die("no compile_commands.json under any preset build dir; configure with CMAKE_EXPORT_COMPILE_COMMANDS=ON")

    binary = resolve_binary()
    entries = load_entries(compile_db)
    db_dir, config_label = gate_db_dir(compile_db, entries)
    files = candidate_files(entries)

    if args.report:
        rc, (hits, _) = run_tidy(binary, db_dir, REPORT_CHECKS, files, as_errors=False)
        label = f", {config_label}" if config_label else ""
        print(f"clang-tidy report ({len(files)} translation units{label}, exit {rc})")
        print("-" * 62)
        for check, count in sorted(hits.items(), key=lambda kv: (-kv[1], kv[0])):
            promoted = " [PROMOTED]" if check in PROMOTED_CHECKS else ""
            print(f"{count:6d}  {check}{promoted}")
        zero = [c for c in PROMOTED_CHECKS if hits.get(c, 0) == 0]
        print("-" * 62)
        print(f"promoted checks at zero findings: {len(zero)}/{len(PROMOTED_CHECKS)}")
        print("promotion candidates = rows with count 0 not yet marked [PROMOTED]")
        sys.exit(0)

    verify_promoted_names(binary)
    checks = "-*," + ",".join(PROMOTED_CHECKS)
    rc, (hits, lines) = run_tidy(binary, db_dir, checks, files, as_errors=True)
    if hits:
        for line in lines:
            print(line)
        total = sum(hits.values())
        print(
            f"clang-tidy-gate: FAIL — {total} finding(s) on {len(PROMOTED_CHECKS)} "
            f"promoted checks across {len(files)} translation units.\n"
            f"Promoted checks are a hard gate; fix them or (with review) "
            f"run --report to review the batch.",
            file=sys.stderr,
        )
        sys.exit(1)
    if rc != 0:
        die(f"clang-tidy exited {rc} without parseable findings (likely a tool/parse error)")
    print(f"clang-tidy-gate: OK — 0 findings on {len(PROMOTED_CHECKS)} promoted checks, {len(files)} translation units.")


if __name__ == "__main__":
    main()
