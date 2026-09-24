#!/usr/bin/env python3
"""Assert the project's version has one source of truth and four agreeing copies.

There is no Version.cmake on purpose: the root `project(VERSION ...)` is the
single source, and the other three files just have to agree with it.

  CMakeLists.txt                     project(drogon-pay VERSION X.Y.Z ...)
  conanfile.py                       version = "X.Y.Z"
  examples/pay-admin/package.json    "version": "X.Y.Z"
  examples/pay-server/openapi.yaml   info: version: X.Y.Z
  CHANGELOG.md                       ## [X.Y.Z] (required only at release)

The contract counts as a declaration because it publishes a version to
consumers: while it sat outside this check it stated a release nobody had
shipped, and no gate could see that.

Default mode (CI FAST gate) checks the four declarations agree. A version bump
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
OPENAPI_YAML = "examples/pay-server/openapi.yaml"

# Two-space indent, so a `version:` scalar nested deeper in a path item cannot
# pass for the contract's own declaration. The value is captured whole and
# validated separately, so an unparseable scalar fails loudly instead of
# looking like a missing line.
OPENAPI_VERSION_RE = re.compile(r"""^  version:(.*)$""")

# Distinguishes "flag absent" from `--tag ""`, which must fail rather than fall
# back to the default mode and report success.
NO_TAG = object()


class SyncError(RuntimeError):
    pass


def read(path: Path) -> str:
    if not path.is_file():
        raise SyncError(f"{path.relative_to(REPO_ROOT)} is missing")
    return path.read_text(encoding="utf-8", errors="replace")


def semver_scalar(scalar: str) -> str:
    """Read the contract's version scalar, or refuse a shape this cannot read.

    The gate reads one line of one file, so it accepts a single-line plain or
    quoted scalar with an optional trailing comment, and refuses anything a
    consumer's YAML parser resolves into different text: an anchor, an alias, a
    tag, a block scalar, an escape sequence, a tab or a non-breaking space used
    as padding. Refusing is not the failure mode here - guessing is, because a
    lenient read turns a file nobody can load into a passing version check.
    """
    if any(ch.isspace() and ch != " " for ch in scalar):
        raise SyncError(
            f"{OPENAPI_YAML}: `version: {scalar}` holds a tab or a non-ASCII "
            "space - YAML does not accept either as padding, so a consumer's "
            "parser stops on this file while a lenient read sees a version"
        )
    text = scalar.strip(" ")
    if not text:
        raise SyncError(f"{OPENAPI_YAML}: `version:` states no value")
    if text[0] in "&*!":
        kind = {"&": "anchor", "*": "alias", "!": "tag"}[text[0]]
        raise SyncError(
            f"{OPENAPI_YAML}: `version: {text}` resolves through a YAML {kind}, "
            "and this check reads a line rather than a document - it cannot say "
            "what the reference points at"
        )
    if text[0] in "|>":
        raise SyncError(
            f"{OPENAPI_YAML}: `version: {text}` is a block scalar, so the value "
            "lives on the following lines and not on the declaration site"
        )

    quote = text[0]
    if quote in "'\"":
        end = text.find(quote, 1)
        if end == -1:
            raise SyncError(
                f"{OPENAPI_YAML}: `version: {text}` opens a quote it never "
                "closes - no consumer's YAML parser reads a version from that"
            )
        value = text[1:end]
        rest = text[end + 1:].strip(" ")
        if rest and not rest.startswith("#"):
            raise SyncError(
                f"{OPENAPI_YAML}: `version: {text}` has text after its closing "
                "quote - a YAML parser rejects that, and a doubled '' escape is "
                "not something this check reads"
            )
        if quote == '"' and "\\" in value:
            raise SyncError(
                f"{OPENAPI_YAML}: `version: {text}` carries a backslash escape "
                "inside double quotes, which a YAML parser resolves to "
                "characters this line does not show"
            )
    else:
        value = text.split(" #", 1)[0].rstrip(" ")

    if not SEMVER_RE.match(value):
        raise SyncError(
            f"{OPENAPI_YAML}: `version: {scalar}` inside `info:` does not read "
            "as X.Y.Z"
        )
    return value


def openapi_info_version() -> str:
    """Read the contract's own `info: version:`, refusing an ambiguous read.

    `info:` has to appear exactly once as a top-level key. Taking the first
    occurrence and ignoring the rest is the bypass the `findall` rule exists to
    close for every other site: a second block would state one version to this
    checker and another to a YAML parser. The scan of the first block stops at
    the next top-level key, because the document carries `version`-shaped
    scalars deeper in `paths:` and `components:`. A deeper-indented line after
    the value is a continuation and is refused; a deeper-indented comment is
    nothing to a parser, so it must not be mistaken for one.
    """
    lines = read(REPO_ROOT / OPENAPI_YAML).splitlines()
    starts = [i for i, line in enumerate(lines) if line.rstrip() == "info:"]
    if not starts:
        raise SyncError(
            f"{OPENAPI_YAML}: no top-level `info:` block - the checker's idea of "
            "the contract's shape and the file have drifted apart"
        )
    if len(starts) > 1:
        raise SyncError(
            f"{OPENAPI_YAML}: {len(starts)} top-level `info:` blocks at lines "
            f"{', '.join(str(i + 1) for i in starts)}. This check would read the "
            "first and leave the rest unchecked, which is a version no gate "
            "compares."
        )

    found: list[str] = []
    for offset, line in enumerate(lines[starts[0] + 1:], starts[0] + 1):
        if line.strip() and not line.startswith("  "):
            break
        match = OPENAPI_VERSION_RE.match(line)
        if not match:
            continue
        raw = match.group(1)
        # A plain scalar continues while a following, more-indented line carries
        # content; a comment line never continues one, and a blank one does not
        # end the possibility either (a parser folds it in as a newline). So the
        # scan skips comments and blanks to the next line that means something.
        continued = ""
        continued_at = offset
        for peek, following in enumerate(lines[offset + 1:], offset + 1):
            stripped = following.strip()
            if not stripped or stripped.startswith("#"):
                continue
            continued = following
            continued_at = peek
            break
        if (raw.strip(" ") and continued
                and len(continued) - len(continued.lstrip(" ")) > 2):
            raise SyncError(
                f"{OPENAPI_YAML}: the `version:` value at line {offset + 1} "
                f"continues onto line {continued_at + 1}, so a YAML parser reads "
                f"`{raw.strip(' ')} {continued.strip(' ')}` and this check would "
                "read one line of it"
            )
        found.append(raw)

    if len(found) != 1:
        raise SyncError(
            f"{OPENAPI_YAML}: expected exactly one `info: version: X.Y.Z` line, "
            f"found {len(found)} "
            f"({', '.join(v.strip(' ') for v in found) or 'none'}). A second one "
            "is how this site gets a false pass: the check would read the first "
            "and the published contract would mean the other."
        )
    return semver_scalar(found[0])


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
    found[OPENAPI_YAML] = openapi_info_version()
    return found


def changelog_has_section(version: str) -> bool:
    header = re.compile(rf"^## \[{re.escape(version)}\]", re.MULTILINE)
    return bool(header.search(read(REPO_ROOT / "CHANGELOG.md")))


def agreed_version(versions: dict[str, str]) -> str:
    """Print every site and return the one version they state, or raise."""
    print("Declared versions:")
    for site, version in sorted(versions.items()):
        print(f"  {version:<10} {site}")

    distinct = set(versions.values())
    if len(distinct) > 1:
        listing = ", ".join(f"{v} in {s}" for s, v in sorted(versions.items()))
        raise SyncError(
            f"the declarations disagree ({listing}). There is no Version.cmake; "
            f"bump CMakeLists.txt, conanfile.py, {PACKAGE_JSON} and "
            f"{OPENAPI_YAML} together."
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
        versions = declared_versions()
        declared = agreed_version(versions)

        if args.tag is NO_TAG:
            print(f"Version sync passed ({declared} in {len(versions)} places).")
            return 0

        if not isinstance(args.tag, str) or not args.tag.startswith("v") \
                or not SEMVER_RE.match(args.tag[1:]):
            raise SyncError(f"{args.tag!r} is not a v<n>.<m>.<p> ref")
        tagged = args.tag[1:]
        if tagged != declared:
            raise SyncError(
                f"tag says {tagged}, the tree says {declared}. Either bump the "
                "four declarations and re-tag, or push the tag that matches "
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
