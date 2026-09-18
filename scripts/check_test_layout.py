#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Test-layout guard (CI gate + local check).

The suite is split into tests/unit/ (pure logic) and tests/integration/
(running Drogon app / DB / HTTP surface). This gate keeps the invariants
that made the split worth having:

  L1  Every .cc under tests/unit|tests/integration is named *Test.cc.
  L2  tests/ root holds exactly one entry point (main.cc, the single
      DROGON_TEST_MAIN) and no loose test files.
  L3  DROGON_TEST cases live only under tests/.
  L4  Every test source is explicitly registered in tests/CMakeLists.txt
      (no file(GLOB ...) — a new file that nobody wired into the build
      must fail the gate, not silently skip).

Exit code 0 = all rules pass; 1 = violations (printed one per line).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
TESTS_DIR = REPO_ROOT / "tests"
CMAKE = TESTS_DIR / "CMakeLists.txt"

SCAN_CODE_DIRS = ("libs", "examples", "test_package")


def _test_sources(root: Path) -> list[Path]:
    return sorted(p for sub in ("unit", "integration")
                  for p in (root / sub).glob("*.cc"))


def check_naming() -> list[str]:
    errors = []
    for src in _test_sources(TESTS_DIR):
        if not src.name.endswith("Test.cc"):
            errors.append(
                f"[L1 naming] {src.relative_to(REPO_ROOT).as_posix()}: "
                "test sources under tests/unit|integration must end in Test.cc"
            )
    return errors


def check_root_layout() -> list[str]:
    errors = []
    mains: list[Path] = []
    for p in sorted(TESTS_DIR.glob("*.cc")):
        if "DROGON_TEST_MAIN" in p.read_text(encoding="utf-8", errors="replace"):
            mains.append(p)
        elif p.name != "main.cc":
            errors.append(
                f"[L2 root-layout] tests/{p.name}: loose test file — move it "
                "into tests/unit/ or tests/integration/"
            )
    if len(mains) > 1:
        errors.append(
            "[L2 root-layout] DROGON_TEST_MAIN defined in multiple files: "
            + ", ".join(p.name for p in mains)
        )
    elif not mains:
        errors.append("[L2 root-layout] no DROGON_TEST_MAIN found under tests/")
    return errors


def check_drogon_test_only_in_tests() -> list[str]:
    errors = []
    for top in SCAN_CODE_DIRS:
        root = REPO_ROOT / top
        if not root.is_dir():
            continue
        for p in sorted(root.rglob("*")):
            if not p.is_file() or p.suffix not in (".cc", ".cpp", ".h", ".hpp"):
                continue
            if "models" in p.parts:
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            for lineno, line in enumerate(text.splitlines(), 1):
                if re.search(r"\bDROGON_TEST\s*\(", line) and not re.search(
                    r"\bDROGON_TEST_MAIN\b", line
                ):
                    errors.append(
                        f"[L3 stray-test] {p.relative_to(REPO_ROOT).as_posix()}"
                        f":{lineno}: DROGON_TEST outside tests/"
                    )
    return errors


def check_cmake_registration() -> list[str]:
    errors = []
    if not CMAKE.is_file():
        return ["[L4 registration] tests/CMakeLists.txt not found"]
    text = CMAKE.read_text(encoding="utf-8", errors="replace")
    if re.search(r"\bfile\s*\(\s*GLOB", text):
        errors.append(
            "[L4 registration] tests/CMakeLists.txt uses file(GLOB ...) — "
            "test sources must be listed explicitly"
        )
    for src in _test_sources(TESTS_DIR):
        rel = src.relative_to(TESTS_DIR).as_posix()  # e.g. unit/FooTest.cc
        if rel not in text:
            errors.append(
                f"[L4 registration] {src.relative_to(REPO_ROOT).as_posix()}: "
                f"not listed in tests/CMakeLists.txt (add '{rel}')"
            )
    return errors


def main() -> int:
    violations = (
        check_naming()
        + check_root_layout()
        + check_drogon_test_only_in_tests()
        + check_cmake_registration()
    )
    if violations:
        print(f"Test layout guard FAILED ({len(violations)} violation(s)):")
        for v in violations:
            print("  " + v)
        return 1
    print("Test layout guard passed (4 rules: naming, root-layout, "
          "stray-tests, cmake-registration).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
