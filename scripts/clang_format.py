#!/usr/bin/env python3
"""clang-format runner with a single pinned version (CI + agent hooks + docs).

Background: CI pinned clang-format-22, pre-commit used v17 and the agent
PostToolUse hook used whatever `clang-format` happened to be on PATH. v17
and v22 disagree on formatting for real (see the pin comment in
.github/workflows/ci.yml's static-analysis job), so every consumer must run the
SAME major version. This file is the one place that constant lives.

Usage:
  python scripts/clang_format.py --check   # exit 1 on drift (CI gate)
  python scripts/clang_format.py --fix     # rewrite files in place

Binary resolution order:
  1. $CLANG_FORMAT_BIN
  2. clang-format-22 / clang-format22 on PATH
  3. On Windows: C:\\Program Files\\LLVM\\bin\\clang-format.exe
  4. clang-format on PATH
...and the resolved binary's --version must report major == PINNED_MAJOR,
otherwise the run fails with remediation hints (never silently check
formatting with the wrong version).
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PINNED_MAJOR = 22
WINDOWS_LLMV_PATHS = (
    r"C:\Program Files\LLVM\bin\clang-format.exe",
    r"C:\Program Files (x86)\LLVM\bin\clang-format.exe",
)

SCAN_DIRS = ("libs/drogon-pay", "examples/pay-server", "tests")
SOURCE_EXTS = {".h", ".hpp", ".cc", ".cpp"}
EXCLUDE_PARTS = {"models"}  # generated ORM code, exempt everywhere

REPO_ROOT = Path(__file__).resolve().parent.parent


def _version_major(binary: str) -> int | None:
    try:
        out = subprocess.run(
            [binary, "--version"], capture_output=True, text=True, check=True
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    m = re.search(r"version\s+(\d+)\.", out)
    return int(m.group(1)) if m else None


def resolve_binary() -> str:
    candidates: list[str] = []
    env_bin = os.environ.get("CLANG_FORMAT_BIN")
    if env_bin:
        candidates.append(env_bin)
    candidates += [f"clang-format-{PINNED_MAJOR}", f"clang-format{PINNED_MAJOR}"]
    if os.name == "nt":
        candidates += [p for p in WINDOWS_LLMV_PATHS if Path(p).is_file()]
    candidates.append("clang-format")

    for cand in candidates:
        binary = shutil.which(cand) or (cand if Path(cand).is_file() else None)
        if binary and _version_major(binary) == PINNED_MAJOR:
            return binary

    found = []
    for cand in candidates:
        binary = shutil.which(cand) or (cand if Path(cand).is_file() else None)
        if binary:
            found.append(f"{binary} (major={_version_major(binary)})")
    hint = ", ".join(found) if found else "no clang-format on PATH"
    sys.exit(
        f"clang_format.py: need clang-format {PINNED_MAJOR}.x "
        f"(scripts/clang_format.py:PINNED_MAJOR), found: {hint}\n"
        "Fix: install clang-format-22 (apt.llvm.org on Ubuntu, "
        "LLVM installer on Windows) or set CLANG_FORMAT_BIN."
    )


def source_files() -> list[Path]:
    files: list[Path] = []
    for rel in SCAN_DIRS:
        root = REPO_ROOT / rel
        if not root.is_dir():
            continue
        for f in root.rglob("*"):
            if (
                f.is_file()
                and f.suffix in SOURCE_EXTS
                and not EXCLUDE_PARTS.intersection(f.parts)
            ):
                files.append(f)
    return sorted(files)


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in ("--check", "--fix"):
        sys.exit(__doc__)
    mode = sys.argv[1]
    binary = resolve_binary()
    files = source_files()
    bad: list[Path] = []
    for f in files:
        if mode == "--fix":
            rc = subprocess.run([binary, "-i", str(f)]).returncode
        else:
            rc = subprocess.run(
                [binary, "--dry-run", "--Werror", str(f)],
                capture_output=True,
            ).returncode
        if rc != 0:
            bad.append(f)
        if mode == "--fix" and rc != 0:
            sys.exit(f"clang-format -i failed on {f}")
    label = binary + f" (pinned major {PINNED_MAJOR})"
    if mode == "--check":
        if bad:
            print(f"{label}: {len(bad)} file(s) NOT format-clean:")
            for f in bad:
                print("  " + f.relative_to(REPO_ROOT).as_posix())
            print("Run: python scripts/clang_format.py --fix")
            return 1
        print(f"All {len(files)} file(s) format-clean under {label}.")
    else:
        print(f"Formatted {len(bad)} drift file(s) under {label}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
