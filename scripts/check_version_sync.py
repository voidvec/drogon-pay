#!/usr/bin/env python3
"""Assert the project's version is declared in exactly one place, four times.

There is no Version.cmake on purpose: the root `project(VERSION ...)` is the
single source, and the other three sites just have to agree with it.

  CMakeLists.txt                     project(drogon-pay VERSION X.Y.Z ...)
  conanfile.py                       version = "X.Y.Z"
  examples/pay-admin/package.json    "version": "X.Y.Z"
  CHANGELOG.md                       ## [X.Y.Z] (required only at release)

Default mode (CI FAST gate) checks the three declarations agree. A version bump
in a pull request passes that mode before its tag exists; the tag itself is
checked by `.github/workflows/release.yml`, which runs this script with
`--tag "$GITHUB_REF_NAME"` and additionally demands a CHANGELOG section, so a
release cannot ship notes-less.

Stdlib-only, like every scripts/check_*.py: the FAST gate must not assume any
pip package is on the runner.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SEMVER_RE = re.compile(r"^\d+\.\d+\.\d+$")

SOURCES = {
    "CMakeLists.txt": re.compile(
        r"^\s*project\s*\(\s*\w[\w-]*\s+VERSION\s+(\d+\.\d+\.\d+)\b",
        re.MULTILINE,
    ),
    "conanfile.py": re.compile(r'^\s*version\s*=\s*["\'](\d+\.\d+\.\d+)["\']',
                               re.MULTILINE),
}


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
        match = pattern.search(read(REPO_ROOT / rel))
        if not match:
            raise SyncError(
                f"{rel}: no `VERSION X.Y.Z` / `version = \"X.Y.Z\"` line found "
                "- the checker's pattern and the file have drifted apart"
            )
        found[rel] = match.group(1)

    package_json = json.loads(read(REPO_ROOT / "examples/pay-admin/package.json"))
    version = package_json.get("version")
    if not isinstance(version, str) or not SEMVER_RE.match(version):
        raise SyncError(
            "examples/pay-admin/package.json: \"version\" must be X.Y.Z at the "
            "top level (a missing or non-semver field is how this check gets a "
            "false pass)"
        )
    found["examples/pay-admin/package.json"] = version
    return found


def changelog_has_section(version: str) -> bool:
    header = re.compile(rf"^## \[{re.escape(version)}\]", re.MULTILINE)
    return bool(header.search(read(REPO_ROOT / "CHANGELOG.md")))


def newest_tag() -> str | None:
    """Newest local `v*` tag, or None when the clone has no tags at all."""
    try:
        proc = subprocess.run(
            ["git", "tag", "--list", "v*", "--sort=-v:refname"],
            cwd=REPO_ROOT, capture_output=True, text=True, encoding="utf-8",
            errors="replace",
        )
    except FileNotFoundError:
        return None
    if proc.returncode != 0:
        return None
    lines = [line.strip() for line in proc.stdout.splitlines() if line.strip()]
    return lines[0] if lines else None


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--tag",
        help="Release ref to validate (e.g. v1.2.3); also requires a "
             "CHANGELOG section for that version",
    )
    args = ap.parse_args(argv)

    try:
        versions = declared_versions()
    except SyncError as exc:
        print(f"Version sync FAILED: {exc}")
        return 1

    print("Declared versions:")
    for site, version in sorted(versions.items()):
        print(f"  {version:<10} {site}")

    distinct = set(versions.values())
    if len(distinct) > 1:
        print(
            "\nVersion sync FAILED: the declarations disagree. There is no "
            "Version.cmake; bump CMakeLists.txt, conanfile.py and "
            "pay-admin/package.json together."
        )
        return 1

    declared = distinct.pop()

    if args.tag:
        if not args.tag.startswith("v") or not SEMVER_RE.match(args.tag[1:]):
            print(f"Version sync FAILED: {args.tag!r} is not v<n>.<m>.<p>")
            return 1
        tagged = args.tag[1:]
        if tagged != declared:
            print(
                f"Version sync FAILED: tag says {tagged}, the tree says "
                f"{declared}. Either bump the three declarations and re-tag, "
                "or push the tag that matches this commit."
            )
            return 1
        if not changelog_has_section(tagged):
            print(
                f"Version sync FAILED: CHANGELOG.md has no '## [{tagged}]' "
                "section, so the GitHub Release would ship without notes."
            )
            return 1
        print(f"Release gate OK: {args.tag} matches the tree and CHANGELOG.md")
        return 0

    head = newest_tag()
    if head:
        print(f"  (newest tag {head} is not compared here; "
              "release.yml does that with --tag)")
    print(f"Version sync passed ({declared} in 3 places).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
