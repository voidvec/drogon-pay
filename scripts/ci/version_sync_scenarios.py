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


def document(body: list[str], header: str = "info:") -> str:
    """A contract whose `info:` block holds exactly these raw lines."""
    return "\n".join(
        ["openapi: 3.0.3", header, *body, "paths: {}", "components: {}"]
    )


# Where a document marker sits, which `document()` cannot express: the same
# three characters are a legal start of one document, the end of one, or the
# start of a second that `safe_load` will never let this file be.
DOCUMENT_CASES: list[tuple[str, str, str | None]] = [
    ("leading ---", "---\nopenapi: 3.0.3\ninfo:\n  version: 1.1.0\n", VERSION),
    ("trailing ...", "openapi: 3.0.3\ninfo:\n  version: 1.1.0\n...\n", VERSION),
    ("bare trailing ---", "openapi: 3.0.3\ninfo:\n  version: 1.1.0\n---\n", None),
    ("... then content", "openapi: 3.0.3\ninfo:\n  version: 1.1.0\n...\nfoo: 1\n", None),
    ("--- then content", "openapi: 3.0.3\ninfo:\n  version: 1.1.0\n---\nfoo: 1\n", None),
    ("two leading ---", "---\nopenapi: 3.0.3\ninfo:\n  version: 1.1.0\n---\n", None),
]


def with_reader(text: str):
    """Patch the module's file read so the scanner sees `text`."""

    def fake_read(path: Path) -> str:
        if path.name == "openapi.yaml":
            return text
        raise AssertionError(f"reader touched an unexpected file: {path}")

    cvs.read = fake_read  # type: ignore[assignment]


# label, block header, info-block lines, expected value or None for "must refuse"
Case = tuple[str, str, list[str], str | None]
HEADER = "info:"
CASES: list[Case] = [
    # The spellings a release may actually use.
    ("plain", HEADER, ["  version: 1.1.0"], VERSION),
    ("double quoted", HEADER, ['  version: "1.1.0"'], VERSION),
    ("single quoted", HEADER, ["  version: '1.1.0'"], VERSION),
    ("trailing padding", HEADER, ["  version: 1.1.0   "], VERSION),
    ("plain + comment", HEADER, ["  version: 1.1.0 # bumped with the tag"], VERSION),
    ("quoted + comment", HEADER, ['  version: "1.1.0" # bumped'], VERSION),
    ("quoted + hash no space", HEADER, ['  version: "1.1.0"#bumped'], VERSION),
    ("sibling key after", HEADER, ["  version: 1.1.0", "  title: pay"], VERSION),
    ("nested block after", HEADER,
     ["  version: 1.1.0", "  contact:", "    name: pay"], VERSION),
    ("blank then sibling key", HEADER,
     ["  version: 1.1.0", "", "  title: pay"], VERSION),
    ("version is the last key", HEADER, ["  title: pay", "  version: 1.1.0"], VERSION),
    # The header itself: padding a parser ignores is padding this reader ignores
    # too, and padding YAML does not accept is not a header at all.
    ("header + trailing comment", "info: # the contract", ["  version: 1.1.0"], VERSION),
    ("header + space before colon", "info :", ["  version: 1.1.0"], VERSION),
    ("header padded with a tab", "info:\t", ["  version: 1.1.0"], None),
    ("header padded with nbsp", "info:\u00a0", ["  version: 1.1.0"], None),
    # A comment is not content: a parser never folds it into the scalar, so the
    # scanner must not read the indented line below as a continuation either.
    ("deeper indented comment", HEADER,
     ["  version: 1.1.0", "    # note", "  title: pay"], VERSION),
    # Shapes a YAML parser resolves into something the line does not show.
    ("anchor", HEADER, ["  version: &v 1.1.0"], None),
    ("tag", HEADER, ["  version: !!str 1.1.0"], None),
    ("alias", HEADER, ["  version: *v"], None),
    ("folded block scalar", HEADER, ["  version: >", "    1.1.0"], None),
    ("literal block scalar", HEADER, ["  version: |", "    1.1.0"], None),
    ("block scalar header only", HEADER, ["  version: >"], None),
    ("plain continuation", HEADER, ["  version: 1.1.0", "    9.9.9"], None),
    ("blank line then deeper text", HEADER, ["  version: 1.1.0", "", "    9.9.9"], None),
    ("equal-indent junk", HEADER, ["  version: 1.1.0", "  9.9.9"], None),
    ("equal-indent list item", HEADER, ["  version: 1.1.0", "  - x"], None),
    ("tab-indented follower", HEADER, ["  version: 1.1.0", "\t9.9.9"], None),
    ("form feed in value", HEADER, ["  version: 1.1.0\x0c9.9.9"], None),
    ("line separator in value", HEADER, ["  version: 1.1.0\u20289.9.9"], None),
    ("second document", HEADER, ["  version: 1.1.0", "---", "foo: 1"], None),
    ("empty value with nested block", HEADER, ["  version:", "    a: 1"], None),
    ("tab after colon", HEADER, ["  version:\t1.1.0"], None),
    ("tab in value", HEADER, ["  version: 1.1\t0"], None),
    ("non-breaking space", HEADER, ["  version: 1.1.0\u00a0"], None),
    ("escape in double quotes", HEADER, ['  version: "1.1.0\\n"'], None),
    # The reads that would be ambiguous even if each line is well-formed.
    ("two version lines", HEADER, ["  version: 1.1.0", "  version: 9.9.9"], None),
    ("unbalanced quote", HEADER, ['  version: "1.1.0'], None),
    ("doubled quote escape", HEADER, ["  version: '1.1''0'"], None),
    ("text after closing quote", HEADER, ['  version: "1.1.0" junk'], None),
    ("no value", HEADER, ["  version:"], None),
    ("flow mapping", HEADER, ["  version: {a: 1}"], None),
    ("not semver", HEADER, ["  version: 1.1"], None),
    ("leading zero", HEADER, ["  version: 01.1.0"], None),
    ("prerelease suffix", HEADER, ["  version: 1.1.0-rc1"], None),
    ("missing block", HEADER, ["  title: pay"], None),
]


def assert_verdict(label: str, text: str, expected: str | None) -> str | None:
    """Run one document through the reader and check it against the table."""
    real_read = cvs.read
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
            return None
        print(f"FAIL  accepts  {label} -> {got!r}")
        return f"{label}: accepted {got!r}, must refuse"
    if got == expected:
        print(f"PASS  reads    {label} -> {got}")
        return None
    print(f"FAIL  {label}: expected {expected!r}, got {got!r}")
    return f"{label}: expected {expected!r}, got {got!r} ({error or 'no error'})"


def run_cases() -> list[str]:
    failures: list[str] = []
    for label, header, body, expected in CASES:
        failure = assert_verdict(label, document(body, header), expected)
        failures += [failure] if failure else []
    for label, text, expected in DOCUMENT_CASES:
        failure = assert_verdict(label, text, expected)
        failures += [failure] if failure else []
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
    total = len(CASES) + len(DOCUMENT_CASES) + 1
    print(f"\n{'FAILED' if failures else 'all passed'}: {total - len(failures)}/{total}")
    for line in failures:
        print(f"  - {line}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
