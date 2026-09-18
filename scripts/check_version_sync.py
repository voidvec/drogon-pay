#!/usr/bin/env python3
"""Assert the project's version has one source of truth and three agreeing copies.

There is no Version.cmake on purpose: the root `project(VERSION ...)` is the
single source, and the other two files just have to agree with it.

  CMakeLists.txt                     project(drogon-pay VERSION X.Y.Z ...)
  conanfile.py                       version = "X.Y.Z"
  examples/pay-admin/package.json    "version": "X.Y.Z"
  CHANGELOG.md                       ## [X.Y.Z] (required only at release)

Default mode (CI FAST gate) checks the three declarations agree. A version bump
in a pull request passes that mode before its tag exists; the tag itself is
checked by `.github/workflows/release.yml`, which runs this script with
`--tag "$GITHUB_REF_NAME"` and additionally demands a CHANGELOG section, so a
release cannot ship notes-less.

Every pattern is applied with findall, not search: a second `project(VERSION ...)`
or `version = ...` line in the same file is exactly how a checker that reads only
the first match gets bypassed.

Stdlib-only, like every scripts/check_*.py: the FAST gate must not assume any
pip package is on the runner.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")

# The trailing (?![\d.]) turns `VERSION 1.0.0.1` into a "pattern and file have
# drifted apart" failure instead of a match that reads 1.0.0 out of four parts.
SOURCES = {
    "CMakeLists.txt": re.compile(
        r"^\s*project\s*\(\s*\w[\w-]*\s+VERSION\s+(\d+\.\d+\.\d+)(?![\d.])",
        re.MULTILINE,
    ),
    "conanfile.py": re.compile(
        r'^\s*version\s*=\s*["\'](\d+\.\d+\.\d+)["\'](?![\d.])',
        re.MULTILINE,
    ),
}

PACKAGE_JSON = "examples/pay-admin/package.json"

# Distinguishes "flag absent" from `--tag ""`, which must fail rather than fall
# back to the default mode and report success.
NO_TAG = object()


class SyncError(RuntimeError):
    pass


def read(path: Path) -> str:
    if not path.is_file():
        raise SyncError(f"{path.relative_to(REPO_ROOT)} is missing")
    return path.read_text(encoding="utf-8", errors="replace")


def declared_versions() -> dict[str, str]:
    """Map each declaration site to the version it states."""
    found: dict[str, str] = {}
    for rel, pattern in SOURCES.items():
        matches = pattern.findall(read(REPO_ROOT / rel))
        if not matches:
            raise SyncError(
                f"{rel}: no `VERSION X.Y.Z` / `version = \"X.Y.Z\"` line found "
                "- the checker's pattern and the file have drifted apart"
            )
        distinct = set(matches)
        if len(distinct) > 1:
            raise SyncError(
                f"{rel} declares {len(distinct)} versions "
                f"({', '.join(sorted(distinct))}) - keep exactly one line per "
                "file, or this check reads one and the build reads another"
            )
        found[rel] = matches[0]

    try:
        package_json = json.loads(read(REPO_ROOT / PACKAGE_JSON))
    except json.JSONDecodeError as exc:
        raise SyncError(f"{PACKAGE_JSON} is not valid JSON: {exc}") from exc
    version = package_json.get("version") if isinstance(package_json, dict) else None
    if not isinstance(version, str) or not SEMVER_RE.match(version):
        raise SyncError(
            f"{PACKAGE_JSON}: \"version\" must be X.Y.Z at the top level (a "
            "missing or non-semver field is how this check gets a false pass)"
        )
    found[PACKAGE_JSON] = version
    return found


def changelog_has_section(version: str) -> bool:
    header = re.compile(rf"^## \[{re.escape(version)}\]", re.MULTILINE)
    return bool(header.search(read(REPO_ROOT / "CHANGELOG.md")))


def agreed_version() -> str:
    """Print every site and return the one version they state, or raise."""
    versions = declared_versions()
    print("Declared versions:")
    for site, version in sorted(versions.items()):
        print(f"  {version:<10} {site}")

    distinct = set(versions.values())
    if len(distinct) > 1:
        listing = ", ".join(f"{v} in {s}" for s, v in sorted(versions.items()))
        raise SyncError(
            f"the declarations disagree ({listing}). There is no Version.cmake; "
            f"bump CMakeLists.txt, conanfile.py and {PACKAGE_JSON} together."
        )
    return distinct.pop()


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--tag",
        default=NO_TAG,
        help="Release ref to validate (e.g. v1.2.3); also requires a "
             "CHANGELOG section for that version",
    )
    args = ap.parse_args(argv)

    try:
        declared = agreed_version()

        if args.tag is NO_TAG:
            print(f"Version sync passed ({declared} in {len(SOURCES) + 1} places).")
            return 0

        if not isinstance(args.tag, str) or not args.tag.startswith("v") \
                or not SEMVER_RE.match(args.tag[1:]):
            raise SyncError(f"{args.tag!r} is not a v<n>.<m>.<p> ref")
        tagged = args.tag[1:]
        if tagged != declared:
            raise SyncError(
                f"tag says {tagged}, the tree says {declared}. Either bump the "
                "three declarations and re-tag, or push the tag that matches "
                "this commit."
            )
        if not changelog_has_section(tagged):
            raise SyncError(
                f"CHANGELOG.md has no '## [{tagged}]' section, so the GitHub "
                "Release would ship without notes."
            )
        print(f"Release gate OK: {args.tag} matches the tree and CHANGELOG.md")
        return 0
    except SyncError as exc:
        print(f"Version sync FAILED: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
