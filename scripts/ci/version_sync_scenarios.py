#!/usr/bin/env python3
"""Pin the shapes the contract's version reader accepts and refuses.

`scripts/check_version_sync.py` reads `info: version:` out of
`examples/pay-server/openapi.yaml` line by line, because the FAST gate is
stdlib-only and cannot import a YAML parser. That is a scanner standing in for a
parser, and every way the two can disagree is a version gate that is green on a
number nobody publishes -- or red on a file that ships fine. Both directions have
already happened once, in this repository, on this function.

So each case below states what the scanner must do with a shape, and the shapes
are the ones a consumer's parser reads differently from a regex: an anchor, a
tag, an alias, a block scalar, a continuation line, a tab used as padding, an
unbalanced quote, a doubled quote, a comment. Nothing here asserts *why* the
refusal is worded a particular way; it asserts that the refusal happens.

    python3 scripts/ci/version_sync_scenarios.py

Run it before editing the reader. It changes nothing on disk: the module's file
read is replaced with a synthetic document, and the repository's own contract is
only ever read.
"""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SPEC_PATH = REPO_ROOT / "scripts" / "check_version_sync.py"

spec = importlib.util.spec_from_file_location("check_version_sync", SPEC_PATH)
cvs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cvs)

VERSION = "1.1.0"


def document(body: list[str]) -> str:
    """A contract whose `info:` block holds exactly these raw lines."""
    return "\n".join(
        ["openapi: 3.0.3", "info:", *body, "paths: {}", "components: {}"]
    )


def with_reader(text: str):
    """Patch the module's file read so the scanner sees `text`."""

    def fake_read(path: Path) -> str:
        if path.name == "openapi.yaml":
            return text
        raise AssertionError(f"reader touched an unexpected file: {path}")

    cvs.read = fake_read  # type: ignore[assignment]


# label, info-block lines, expected value or None for "must refuse"
CASES: list[tuple[str, list[str], str | None]] = [
    # The spellings a release may actually use.
    ("plain", ["  version: 1.1.0"], VERSION),
    ("double quoted", ['  version: "1.1.0"'], VERSION),
    ("single quoted", ["  version: '1.1.0'"], VERSION),
    ("trailing padding", ["  version: 1.1.0   "], VERSION),
    ("plain + comment", ["  version: 1.1.0 # bumped with the tag"], VERSION),
    ("quoted + comment", ['  version: "1.1.0" # bumped'], VERSION),
    ("quoted + hash no space", ['  version: "1.1.0"#bumped'], VERSION),
    ("sibling key after", ["  version: 1.1.0", "  title: pay"], VERSION),
    ("version is the last key", ["  title: pay", "  version: 1.1.0"], VERSION),
    # A comment is not content: a parser never folds it into the scalar, so the
    # scanner must not read the indented line below as a continuation either.
    ("deeper indented comment", ["  version: 1.1.0", "    # note", "  title: pay"], VERSION),
    # Shapes a YAML parser resolves into something the line does not show.
    ("anchor", ["  version: &v 1.1.0"], None),
    ("tag", ["  version: !!str 1.1.0"], None),
    ("alias", ["  version: *v"], None),
    ("folded block scalar", ["  version: >", "    1.1.0"], None),
    ("literal block scalar", ["  version: |", "    1.1.0"], None),
    ("block scalar header only", ["  version: >"], None),
    ("plain continuation", ["  version: 1.1.0", "    9.9.9"], None),
    ("blank line then deeper text", ["  version: 1.1.0", "", "    9.9.9"], None),
    ("empty value with nested block", ["  version:", "    a: 1"], None),
    ("tab after colon", ["  version:\t1.1.0"], None),
    ("tab in value", ["  version: 1.1\t0"], None),
    ("non-breaking space", ["  version: 1.1.0\u00a0"], None),
    ("escape in double quotes", ['  version: "1.1.0\\n"'], None),
    # The reads that would be ambiguous even if each line is well-formed.
    ("two version lines", ["  version: 1.1.0", "  version: 9.9.9"], None),
    ("unbalanced quote", ['  version: "1.1.0'], None),
    ("doubled quote escape", ["  version: '1.1''0'"], None),
    ("text after closing quote", ['  version: "1.1.0" junk'], None),
    ("no value", ["  version:"], None),
    ("flow mapping", ["  version: {a: 1}"], None),
    ("not semver", ["  version: 1.1"], None),
    ("prerelease suffix", ["  version: 1.1.0-rc1"], None),
    ("missing block", ["  title: pay"], None),
]


def run_cases() -> list[str]:
    failures: list[str] = []
    real_read = cvs.read
    for label, body, expected in CASES:
        text = document(body)
        try:
            with_reader(text)
            got = cvs.openapi_info_version()
            error = None
        except cvs.SyncError as exc:
            got = None
            error = str(exc)
        finally:
            cvs.read = real_read  # type: ignore[assignment]

        if expected is None:
            if got is None:
                print(f"PASS  refuses {label}")
            else:
                failures.append(f"{label}: accepted {got!r}, must refuse")
                print(f"FAIL  accepts  {label} -> {got!r}")
        else:
            if got == expected:
                print(f"PASS  reads    {label} -> {got}")
            else:
                failures.append(
                    f"{label}: expected {expected!r}, got "
                    f"{got!r} ({error or 'no error'})"
                )
                print(f"FAIL  {label}: expected {expected!r}, got {got!r}")
    return failures


def check_real_contract() -> list[str]:
    """The repository's own contract must read as one of the declared sites.

    Without this, every case above can pass while the function is dead code:
    the table proves the scanner's rules, not that the real file is one the
    scanner can read at all.
    """
    failures: list[str] = []
    try:
        sites = cvs.declared_versions()
    except cvs.SyncError as exc:
        return [f"the repository's own declarations do not agree: {exc}"]
    contract = sites.get(cvs.OPENAPI_YAML)
    others = {v for k, v in sites.items() if k != cvs.OPENAPI_YAML}
    if contract is None:
        failures.append(f"{cvs.OPENAPI_YAML} is not among the read sites")
    elif others and contract not in others:
        failures.append(
            f"contract says {contract}, the rest say {', '.join(sorted(others))}"
        )
    else:
        print(f"PASS  live contract reads {contract} beside {len(sites) - 1} others")
    return failures


def main() -> int:
    failures = run_cases() + check_real_contract()
    total = len(CASES) + 1
    print(f"\n{'FAILED' if failures else 'all passed'}: {total - len(failures)}/{total}")
    for line in failures:
        print(f"  - {line}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
