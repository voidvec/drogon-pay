#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Coverage reporter + ratchet gate.

Input: gcov intermediate JSON files (*.gcov.json.gz / *.gcov.json) produced
by `gcov -i` over a --coverage build (see .github/workflows/coverage.yml).
stdlib-only, like every other scripts/check_*.py gate.

Metric scope (drift-plan D4): overall + per-directory buckets for the
library (handlers/services/channels/utils/core) and the example host
(host-controllers/host-utils/host-core). Excluded on principle:
generated ORM models (never hand-edited, never the thing we want to
assert coverage about), tests, third-party code.

Anti-thrash rules, copied from the authforge playbook:
  1. tolerance 0.5pp — a bucket may not drop more than 0.5pp below the
     baseline, so floating-point noise and line-attribution jitter across
     gcov versions never flake the gate;
  2. small-bucket exemption — buckets under MIN_LINES total lines are
     reported but not gated (one added file would swing them 10pp);
  3. missing baseline = SEED — first run records the baseline and passes;
     the reviewed commit that adds a baseline is itself the audit trail;
  4. collapse = FAIL — if a gated bucket's measurable line count collapses
     (>50% shrink), the gate fails even when the percentage went up: that
     pattern means coverage data quietly stopped being produced, not that
     the code improved.

Usage:
    measure_coverage.py [--dir build/linux-coverage] (--report | --seed | --ratchet)

Exit 0 = pass/seed/report; 1 = violations.
"""

from __future__ import annotations

import argparse
import gzip
import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
BASELINE = REPO_ROOT / "scripts" / "coverage_baseline.json"

TOLERANCE_PP = 0.5
MIN_LINES = 150
COLLAPSE_RATIO = 0.5

# First match wins. ("prefix", "bucket") or ("prefix", None) to exclude.
BUCKET_RULES: list[tuple[str, str | None]] = [
    ("libs/drogon-pay/src/models/", None),      # generated ORM code
    ("libs/drogon-pay/src/handlers/", "handlers"),
    ("libs/drogon-pay/src/services/", "services"),
    ("libs/drogon-pay/src/channels/", "channels"),
    ("libs/drogon-pay/src/utils/", "utils"),
    ("libs/drogon-pay/", "core"),               # PayPlugin, ChannelRegistry, public headers
    ("examples/pay-server/controllers/", "host-controllers"),
    ("examples/pay-server/utils/", "host-utils"),
    ("examples/pay-server/main.cc", "host-core"),
]

ORDER = ["handlers", "services", "channels", "utils", "core",
         "host-controllers", "host-utils", "host-core"]


# gcov emits the absolute path the compiler saw, which differs per runner
# (/home/runner/work/<repo>/<repo>/... in CI, /mnt/<drive>/... when a Windows
# checkout is read from WSL). Cutting at the first source-tree marker makes the
# mapping independent of the checkout location; the relative_to() fallback
# covers a gcov that emitted paths already relative to the build dir.
_REPO_MARKERS = ("libs/drogon-pay/", "examples/pay-server/", "tests/")


def normalize(path_str: str) -> str | None:
    """Absolute or build-relative gcov path -> repo-relative posix path."""
    p = path_str.replace("\\", "/")
    for marker in _REPO_MARKERS:
        idx = p.find(marker)
        if idx != -1:
            return p[idx:]
    q = Path(path_str)
    if q.is_absolute():
        try:
            return q.resolve().relative_to(REPO_ROOT).as_posix()
        except ValueError:
            return None
    for base in (REPO_ROOT, REPO_ROOT / "build"):
        try:
            return (base / p).resolve().relative_to(REPO_ROOT).as_posix()
        except ValueError:
            continue
    return None


def bucket_of(rel: str) -> str | None:
    for prefix, bucket in BUCKET_RULES:
        if rel.startswith(prefix):
            return bucket
    return None


def collect(build_dir: Path) -> dict[str, dict[str, int]]:
    """Returns bucket -> {lines, covered} aggregated over all gcov jsons.

    A shared header appears in the intermediate report of every translation
    unit that includes it, so entries are merged per (file, line) taking the
    maximum hit count: a line executed in any object counts as covered, and
    header lines are never counted once per includer.
    """
    files = sorted(build_dir.rglob("*.gcov.json.gz")) + sorted(build_dir.rglob("*.gcov.json"))
    if not files:
        sys.exit("measure-coverage: no *.gcov.json(.gz) under "
                 f"{build_dir} — did the workflow run `gcov -i` first?")
    hits: dict[str, dict[int, int]] = {}
    for f in files:
        raw = gzip.decompress(f.read_bytes()) if f.suffix == ".gz" else f.read_bytes()
        data = json.loads(raw.decode("utf-8", errors="replace"))
        for entry in data.get("files", []):
            rel = normalize(entry.get("file", ""))
            if rel is None:
                continue
            per_line = hits.setdefault(rel, {})
            for line in entry.get("lines", []):
                no = line.get("line_number")
                count = line.get("count") or 0
                if no is None:
                    continue
                # Unexecuted lines must stay in the denominator, so the key is
                # inserted even when its count is 0.
                if no not in per_line or count > per_line[no]:
                    per_line[no] = count
    buckets: dict[str, dict[str, int]] = {}
    for rel, per_line in hits.items():
        bucket = bucket_of(rel)
        if bucket is None:
            continue
        b = buckets.setdefault(bucket, {"lines": 0, "covered": 0})
        b["lines"] += len(per_line)
        b["covered"] += sum(1 for c in per_line.values() if c > 0)
    return buckets


def pct(b: dict[str, int]) -> float:
    return 100.0 * b["covered"] / b["lines"] if b["lines"] else 0.0


def overall(buckets: dict[str, dict[str, int]]) -> dict[str, int]:
    agg = {"lines": 0, "covered": 0}
    for b in buckets.values():
        agg["lines"] += b["lines"]
        agg["covered"] += b["covered"]
    return agg


def render(buckets: dict[str, dict[str, int]]) -> str:
    lines = []
    names = [n for n in ORDER if n in buckets] + [n for n in sorted(buckets) if n not in ORDER]
    for name in names:
        b = buckets[name]
        flag = "" if b["lines"] >= MIN_LINES else "  (<150 lines, gate-exempt)"
        lines.append(f"  {name:<18} {pct(b):6.2f}%  {b['covered']:>6}/{b['lines']:<6}{flag}")
    o = overall(buckets)
    lines.append(f"  {'OVERALL':<18} {pct(o):6.2f}%  {o['covered']:>6}/{o['lines']:<6}")
    return "\n".join(lines)


def write_baseline(buckets: dict[str, dict[str, int]]) -> None:
    payload = {
        "_comment": "Coverage ratchet baseline; regenerate with "
                    "`measure_coverage.py --seed` and review the diff.",
        "buckets": {name: {**b, "pct": round(pct(b), 2)} for name, b in sorted(buckets.items())},
        "overall": {**overall(buckets), "pct": round(pct(overall(buckets)), 2)},
    }
    BASELINE.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"measure-coverage: baseline written to {BASELINE.relative_to(REPO_ROOT)}")


def check_ratchet(buckets: dict[str, dict[str, int]]) -> list[str]:
    errors: list[str] = []
    payload = json.loads(BASELINE.read_text(encoding="utf-8"))
    baseline = dict(payload["buckets"])
    baseline["OVERALL"] = payload["overall"]
    cur = dict(buckets)
    cur["OVERALL"] = overall(buckets)
    for name, base in baseline.items():
        b_lines, b_pct = base.get("lines", 0), base.get("pct", 0.0)
        if b_lines < MIN_LINES and name != "OVERALL":
            continue  # gate-exempt at seed time too
        if name not in cur:
            errors.append(f"[ratchet] bucket '{name}' vanished from the report "
                          "(coverage data collapse?)")
            continue
        c = cur[name]
        if c["lines"] < b_lines * COLLAPSE_RATIO:
            errors.append(
                f"[ratchet] bucket '{name}' line count collapsed: baseline "
                f"{b_lines} -> {c['lines']} (data loss, not progress)"
            )
        if c["lines"] >= MIN_LINES or name == "OVERALL":
            if pct(c) < b_pct - TOLERANCE_PP:
                errors.append(
                    f"[ratchet] bucket '{name}' coverage dropped: "
                    f"{b_pct:.2f}% -> {pct(c):.2f}% (tolerance {TOLERANCE_PP}pp)"
                )
    return errors


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="build/linux-coverage",
                    help="build dir containing the *.gcov.json(.gz) files")
    mode = ap.add_mutually_exclusive_group(required=True)
    mode.add_argument("--report", action="store_true", help="print bucket table")
    mode.add_argument("--seed", action="store_true", help="write coverage_baseline.json")
    mode.add_argument("--ratchet", action="store_true",
                      help="gate against baseline (SEEDs if no baseline yet)")
    args = ap.parse_args()

    build_dir = (REPO_ROOT / args.dir) if not Path(args.dir).is_absolute() else Path(args.dir)
    buckets = collect(build_dir)
    print(render(buckets))

    if args.report:
        return 0
    if args.seed:
        write_baseline(buckets)
        return 0
    # --ratchet
    if not BASELINE.is_file():
        print("measure-coverage: no baseline yet — SEEDING (first run passes)")
        write_baseline(buckets)
        return 0
    errors = check_ratchet(buckets)
    if errors:
        print(f"Coverage ratchet FAILED ({len(errors)} violation(s)):")
        for e in errors:
            print("  " + e)
        return 1
    print(f"Coverage ratchet passed (tolerance {TOLERANCE_PP}pp, "
          f"small-bucket exemption {MIN_LINES} lines).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
