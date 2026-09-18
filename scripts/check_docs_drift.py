#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Docs/AI-config drift guard (CI gate + local check).

The authforge benchmark showed that hand-maintained agent inventories and
spec docs rot within weeks. This gate makes the seven historically-broken
invariants fail loudly:

  R1  AGENTS.md "Claude Code Assets" lists exactly what exists on disk
      under .claude/{agents,rules,skills}/ (tombstones must be listed,
      annotated with [retired]).
  R2  Repo paths written in backticks in the governance docs must be
      resolvable — tracked in the index, or claimed by .gitignore when the
      prose describes something the build or the operator provisions (a
      generated certs/ dir, .env). Forward refs to files a later governance
      phase delivers need an explicit entry in PENDING_PATHS below. "On my
      disk" is not the test: the original exists() check passed on a laptop
      and failed on CI for exactly that reason.
  R3  No gtest vocabulary outside docs/history/ and CHANGELOG.md — the
      suite is Drogon DROGON_TEST; gtest examples in agent docs were
      actively misleading (see 2026-09 drift audit).
  R4  Migration versions cited in docs and skills must exist in sql/.
      Chains move forward; prose that quotes "001-004" after 005 lands is
      a runbook that will be copy-pasted into a deploy.
  R5  A file present under both .claude/ and .codex/ must be byte-identical.
      .codex is a mirror of .claude for a second agent, not a fork; three of
      its files had quietly drifted to pre-refactor paths, a retired layering
      line and the old four-tier log table, so that agent was enforcing rules
      the code no longer has.
  R6  No hand-maintained version / date stamps in live docs. Four runbooks
      carried "**版本：** 1.0.0 / **最后更新：** 2026-04-13" long after both
      were wrong; git owns those facts, prose must not copy them.
  R7  The five documented cross-platform entry scripts must exist as .sh AND
      .bat, the .sh side must be executable in the git index, and any other
      script in that directory must be declared single-platform (with a
      reason) — the doc's "every script has twins" claim was false.

Exit code 0 = all rules pass; 1 = violations (printed one per line).
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
AGENTS_MD = REPO_ROOT / "AGENTS.md"

# ---------------------------------------------------------------- R1

ASSET_SECTIONS = {
    # section header -> (dir under .claude, entry shape)
    "### Agents": (REPO_ROOT / ".claude" / "agents", "file"),
    "### Rules": (REPO_ROOT / ".claude" / "rules", "file"),
    "### Skills": (REPO_ROOT / ".claude" / "skills", "dir"),
}
ASSET_LINE_RE = re.compile(r"^- ([A-Za-z0-9._-]+) \(file: (.+?)\)", re.M)


def check_asset_inventory() -> list[str]:
    errors: list[str] = []
    text = AGENTS_MD.read_text(encoding="utf-8")
    idx = text.find("## Claude Code Assets")
    if idx < 0:
        return ["[rule1 inventory] AGENTS.md has no 'Claude Code Assets' section"]
    inventory = text[idx:]
    for header, (root, shape) in ASSET_SECTIONS.items():
        start = inventory.find(header)
        if start < 0:
            errors.append(f"[rule1 inventory] AGENTS.md missing section '{header}'")
            continue
        end = inventory.find("###", start + len(header))
        body = inventory[start:end if end > 0 else None]
        listed = {}
        for m in ASSET_LINE_RE.finditer(body):
            listed[m.group(1)] = m.group(2)
        if shape == "file":
            actual = {p.stem: f"{root.name}/{p.name}" for p in root.glob("*.md")}
        else:
            actual = {
                p.parent.name: f"{root.name}/{p.parent.name}/SKILL.md"
                for p in root.glob("*/SKILL.md")
            }
        for missing in sorted(set(actual) - set(listed)):
            errors.append(
                f"[rule1 inventory] {header}: on disk but not listed in "
                f"AGENTS.md: {missing} (file: .claude/{actual[missing]})"
            )
        for ghost in sorted(set(listed) - set(actual)):
            errors.append(
                f"[rule1 inventory] {header}: listed in AGENTS.md but not on "
                f"disk: {ghost} (file: .claude/{listed[ghost]})"
            )
    return errors


# ---------------------------------------------------------------- R2

R2_DOCS = [
    REPO_ROOT / "AGENTS.md",
    REPO_ROOT / "CLAUDE.md",
    REPO_ROOT / "TECH_SPECS.md",
    REPO_ROOT / "CONTRIBUTING.md",
] + sorted((REPO_ROOT / "docs").rglob("*.md"))

R2_SKIP_DIRS = ("docs/history/", "docs/superpowers/")  # frozen archives + agent scratch

BACKTICK_RE = re.compile(r"`([^`\n]+)`")
PATHISH_RE = re.compile(
    r"^(?:\.claude|\.codex|\.github|libs|examples|tests|scripts|sql|cmake|docs|"
    r"build|CMakeLists\.txt|conanfile\.py|conan\.lock|"
    r"TECH_SPECS\.md|CHANGELOG\.md|README\.md)(?:[/\\]|$)"
)
PLACEHOLDER_RE = re.compile(
    r"<|>|\*|\{|N{2,}|xxx|XXX|YYYY|NNN|00N|\.\.\.|…|\$|:\d|\s"
)
# Runtime-provisioned secret locations (generated by operators, never tracked).
SECRET_EXTS = (".pem", ".key", ".crt", ".p12", ".pfx")

# Forward references: governance-phase deliverables that docs may cite
# before they land. Each entry needs a reason; prune as phases complete.
PENDING_PATHS: dict[str, str] = {
    # The coverage pipeline (measure_coverage.py + coverage.yml) landed, but
    # this file is written by the first green CI run, not by hand: seeding it
    # from a partial local run would pin fake floors.
    "scripts/coverage_baseline.json": "SEEDed by coverage.yml on its first green run",
}


def _tracked_files() -> set[str]:
    """Every path in the index. This, not the working tree, is what a CI
    checkout can see."""
    proc = subprocess.run(
        ["git", "ls-files", "--cached"],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return set(proc.stdout.split())


def _git_ignored(probes: set[str]) -> set[str]:
    """The subset that .gitignore claims. A doc citing one of these is
    describing something the operator or the build provisions (`.env`, a
    generated `certs/`, `build/`), which no checkout will ever contain."""
    if not probes:
        return set()
    payload = ("\n".join(sorted(probes)) + "\n").encode("utf-8")
    proc = subprocess.run(
        ["git", "check-ignore", "--stdin"],
        cwd=str(REPO_ROOT),
        input=payload,
        capture_output=True,
    )
    # Bytes in, bytes out: passing `encoding` would flip this into text mode,
    # whose newline translation sends CRLF to git and silently stops every
    # path matching.
    return set(proc.stdout.decode("utf-8", errors="replace").split())


def check_doc_paths() -> list[str]:
    """R2, resolved against the index rather than the working tree.

    The first version called Path.exists(), which is a laptop-only truth: it
    passed here because the gitignored build/, .env and certs/ were on disk,
    and failed on the first CI run because a checkout has none of them. Both
    the index and .gitignore are versioned, so this now answers the same way
    everywhere.
    """
    tracked = _tracked_files()
    candidates: list[tuple[str, int, str, str]] = []
    probes: set[str] = set()
    for doc in R2_DOCS:
        if not doc.is_file():
            continue
        rel_doc = doc.relative_to(REPO_ROOT).as_posix()
        if any(rel_doc.startswith(p) for p in R2_SKIP_DIRS):
            continue
        for lineno, line in enumerate(
            doc.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            for token in BACKTICK_RE.findall(line):
                token = token.strip()
                if not PATHISH_RE.match(token) or PLACEHOLDER_RE.search(token):
                    continue
                token = token.rstrip(":").strip("/")
                probe = token.replace("\\", "/")
                if probe in PENDING_PATHS or probe.endswith(SECRET_EXTS):
                    continue
                candidates.append((rel_doc, lineno, token, probe))
                probes.add(probe)

    ignored = _git_ignored(probes)
    errors: list[str] = []
    for rel_doc, lineno, token, probe in candidates:
        if probe in ignored:
            continue
        in_index = probe in tracked or any(
            p.startswith(probe + "/") for p in tracked
        )
        if not in_index:
            errors.append(
                f"[rule2 dead-path] {rel_doc}:{lineno}: `{token}` is neither "
                f"tracked nor gitignored (add to PENDING_PATHS with a reason "
                f"if this is a deliberate forward reference)"
            )
    return errors


# ---------------------------------------------------------------- R3

R3_BANNED = (
    "Google Test",
    "google test",
    "TEST_F(",
    "EXPECT_EQ(",
    "EXPECT_TRUE",
    "ASSERT_EQ",
    "gtest_filter",
    "gtest_verbose",
    "--gtest",
    "#include <gtest",
)
R3_ALLOW_LINE_RE = re.compile(r"not gtest|非 gtest|没有 gtest|gtest 宏不存在")
R3_ROOTS = [REPO_ROOT / "AGENTS.md", REPO_ROOT / "CLAUDE.md",
            REPO_ROOT / "TECH_SPECS.md", REPO_ROOT / "CONTRIBUTING.md"]
R3_DIRS = [REPO_ROOT / "docs", REPO_ROOT / ".claude", REPO_ROOT / ".codex"]
R3_SKIP_DIRS = ("docs/history/",)  # archives + CHANGELOG.md keep historical truth


def check_gtest_vocabulary() -> list[str]:
    errors: list[str] = []
    files: list[Path] = list(R3_ROOTS)
    for root in R3_DIRS:
        if not root.is_dir():
            continue
        files += [p for p in root.rglob("*")
                  if p.is_file() and p.suffix in {".md", ".toml", ".py", ".sh"}]
    for f in files:
        rel = f.relative_to(REPO_ROOT).as_posix()
        if any(rel.startswith(p) for p in R3_SKIP_DIRS):
            continue
        for lineno, line in enumerate(
            f.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            if R3_ALLOW_LINE_RE.search(line):
                continue
            for token in R3_BANNED:
                if token in line:
                    errors.append(
                        f"[rule3 gtest-vocab] {rel}:{lineno}: '{token}' — "
                        f"this repo tests with Drogon DROGON_TEST"
                    )
                    break
    return errors


R4_SKIP_DIRS = ("docs/history/", "docs/superpowers/")
# READMEs that quote the chain by name, so they belong under the same rule as
# the governance docs even though they sit outside docs/.
R4_EXTRA_FILES = (
    REPO_ROOT / "README.md",
    REPO_ROOT / "examples" / "pay-server" / "README.md",
    REPO_ROOT / "libs" / "drogon-pay" / "src" / "models" / "README.md",
)

# `004_ledger_fk.sql` anywhere, with or without the sql/ prefix.
R4_FILE_RE = re.compile(r"(?<![\w/])(\d{3}_[a-z0-9][a-z0-9_]*\.sql)\b")
# `sql/004` without a filename.
R4_VERSION_RE = re.compile(r"sql/(\d{3})(?!\d)")
# "001 ~ 004" / "`000` → `001`" — backticks and separators between the numbers.
R4_RANGE_RE = re.compile(
    r"(?<!\d)(\d{3})[`\s]*(?:–|-|~|→|\.\.\.)[`\s]*(\d{3})(?!\d)"
)


def disk_migration_names() -> set[str]:
    return {p.name for p in (REPO_ROOT / "sql").glob("*.sql")}


def check_migration_versions() -> list[str]:
    """Rule 4 — docs may not cite a migration version the chain does not have.

    The migration chain is the one inventory that docs *and* skills quote by
    number, and a runbook that lists 001-004 while sql/ holds 001-006 is wrong
    in the way that gets copy-pasted into a deploy.
    """
    errors: list[str] = []
    names = disk_migration_names()
    versions = {name[:3] for name in names}
    files: list[Path] = list(R3_ROOTS) + [p for p in R4_EXTRA_FILES if p.is_file()]
    for root in R3_DIRS:
        if not root.is_dir():
            continue
        files += [p for p in root.rglob("*")
                  if p.is_file() and p.suffix in {".md", ".toml", ".py", ".sh"}]
    for f in files:
        rel = f.relative_to(REPO_ROOT).as_posix()
        if any(rel.startswith(p) for p in R4_SKIP_DIRS):
            continue
        for lineno, line in enumerate(
            f.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            for name in R4_FILE_RE.findall(line):
                if name not in names:
                    errors.append(
                        f"[rule4 migration-cite] {rel}:{lineno}: cites "
                        f"{name}, which is not in sql/ (on disk: "
                        f"{', '.join(sorted(names))})"
                    )
            for version in R4_VERSION_RE.findall(line):
                if version not in versions:
                    errors.append(
                        f"[rule4 migration-cite] {rel}:{lineno}: cites "
                        f"sql/{version}, but no {version}_*.sql exists in sql/"
                    )
            if ".sql" in line or "sql/" in line:
                for low, high in R4_RANGE_RE.findall(line):
                    for value in range(int(low), int(high) + 1):
                        if f"{value:03d}" not in versions:
                            errors.append(
                                f"[rule4 migration-cite] {rel}:{lineno}: cites "
                                f"the range {low}-{high}, but {value:03d} is "
                                f"not in sql/ (head is "
                                f"{max(versions) if versions else '-'})"
                            )
    return errors


# ---------------------------------------------------------------- R5

TWIN_ROOTS = (REPO_ROOT / ".claude", REPO_ROOT / ".codex")


def _twin_rels(root: Path) -> set[str]:
    if not root.is_dir():
        return set()
    return {p.relative_to(root).as_posix() for p in root.rglob("*")
            if p.is_file() and "__pycache__" not in p.parts}


def check_twin_parity() -> list[str]:
    """Rule 5 — a name held by both agent trees must hold the same bytes.

    Only the intersection is policed: `.claude/agents/*.md` legitimately has no
    codex twin (codex keeps one `.toml` of its own), and a file in one tree is
    that tool's business. What may not happen is a copy that drifts, because
    nothing re-reads it — the drift is invisible until an agent follows a rule
    the code stopped having months ago.
    """
    errors: list[str] = []
    claude, codex = TWIN_ROOTS
    for rel in sorted(_twin_rels(claude) & _twin_rels(codex)):
        a = (claude / rel).read_bytes()
        b = (codex / rel).read_bytes()
        if a != b:
            errors.append(
                f"[rule5 twin-parity] .codex/{rel} differs from .claude/{rel} "
                f"({len(b)} vs {len(a)} bytes). .codex is a mirror, not a fork: "
                f"edit the .claude copy and `cp` it over in the same commit"
            )
    return errors


# ---------------------------------------------------------------- R6

R6_ROOTS = [AGENTS_MD, REPO_ROOT / "CLAUDE.md", REPO_ROOT / "TECH_SPECS.md",
            REPO_ROOT / "CONTRIBUTING.md", REPO_ROOT / "README.md",
            REPO_ROOT / "README.zh-CN.md"]
R6_DIRS = [REPO_ROOT / "docs"]
R6_SKIP_DIRS = ("docs/history/", "docs/superpowers/")
# "**版本：** 1.0.0", "**更新时间：** 2026-04-13", "**文档版本**: v2.1",
# "**Last updated:** …" — a bolded label naming a version or a date, with the
# colon either inside the bold run (**版本：**) or after it (**文档版本**:), so
# ordinary prose ("the **release** version") stays quiet.
R6_STAMP_RE = re.compile(
    r"\*\*[^*\n]{0,12}(?:版本|更新时间|最后更新)[^*\n]{0,4}[:：]\s*\*\*"
    r"|\*\*[^*\n]{0,12}(?:版本|更新时间|最后更新)\s*\*\*\s*[:：]"
    r"|\*\*(?:[Dd]ocument(?:ation)?|[Pp]roject|[Ll]ibrary)?\s*[Vv]ersion\s*\*\*\s*[:：]"
    r"|\*\*(?:Last\s+)?[Uu]pdated\s*\*\*\s*[:：]"
)
# Release notes templates legitimately print a version heading.
R6_ALLOW_LINE_RE = re.compile(r"Release Notes|no-version-stamps|规则会拒掉戳记")


def check_version_stamps() -> list[str]:
    """Rule 6 — live prose may not restate a version or a last-updated date.

    Four runbooks carried "**版本：** 1.0.0 / **最后更新：** 2026-04-13" long
    after both were wrong, and the dates were the worse half: a reader trusts
    a stale stamp precisely because it looks maintained. git owns those facts.
    """
    errors: list[str] = []
    files: list[Path] = [p for p in R6_ROOTS if p.is_file()]
    for root in R6_DIRS:
        if root.is_dir():
            files += sorted(root.rglob("*.md"))
    for f in files:
        rel = f.relative_to(REPO_ROOT).as_posix()
        if any(rel.startswith(p) for p in R6_SKIP_DIRS):
            continue
        for lineno, line in enumerate(
            f.read_text(encoding="utf-8", errors="replace").splitlines(), 1
        ):
            if R6_ALLOW_LINE_RE.search(line) or not R6_STAMP_RE.search(line):
                continue
            errors.append(
                f"[rule6 no-version-stamps] {rel}:{lineno}: hand-maintained "
                f"version/date stamp — delete it and let `git log -- {rel}` "
                f"carry that fact"
            )
    return errors


# ---------------------------------------------------------------- R7

SCRIPT_DIR = REPO_ROOT / "examples" / "pay-server" / "scripts"
# The entry points a developer is told to run; each must exist on both sides.
REQUIRED_PAIRS = ("build", "test", "setup_database", "deploy", "check_config")
# Deliberately single-platform, with the reason TECH_SPECS documents. A new
# script that is not paired has to appear here, which forces the doc table to
# stay complete instead of silently growing orphans.
SINGLE_PLATFORM_SCRIPTS = {
    "full_test.bat": "Windows-only orchestrator over the other .bat files",
    "generate_models.bat": "drogon_ctl confirmation wrapper; not ported yet",
    "run_server.bat": "convenience launcher (POSIX: cd + ./PayServer)",
    "healthcheck.sh": "POSIX curl probe; unused by any doc (see TECH_SPECS)",
    "e2e_test.sh": "HTTP smoke; PowerShell twin is e2e_test.ps1, not a .bat",
    "e2e_test.ps1": "HTTP smoke; Bash twin is e2e_test.sh",
}


def _index_modes(dir_path: Path) -> dict[str, str]:
    """`git ls-files -s` for a directory: {path: '100755' | '100644'}.

    Index mode, not filesystem mode: the local checkout runs with
    core.fileMode=false, so only what git recorded is meaningful cross-platform.
    A directory outside the repo has no index entry, so it reads as empty.
    """
    try:
        rel = dir_path.relative_to(REPO_ROOT).as_posix()
    except ValueError:
        return {}
    try:
        out = subprocess.run(
            ["git", "ls-files", "-s", "--", rel],
            cwd=str(REPO_ROOT), capture_output=True, text=True, check=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError):
        return {}
    modes: dict[str, str] = {}
    for line in out.splitlines():
        meta, _, path = line.partition("\t")
        parts = meta.split()
        if len(parts) >= 1 and path:
            modes[path.strip()] = parts[0]
    return modes


def check_script_twins() -> list[str]:
    """Rule 7 — the documented twin set is the actual twin set."""
    errors: list[str] = []
    if not SCRIPT_DIR.is_dir():
        return [f"[rule7 twin-scripts] {SCRIPT_DIR} missing"]
    on_disk = {p.name for p in SCRIPT_DIR.iterdir()
               if p.is_file() and p.suffix in {".sh", ".bat", ".ps1"}}
    for base in REQUIRED_PAIRS:
        for suffix in (".sh", ".bat"):
            if f"{base}{suffix}" not in on_disk:
                errors.append(
                    f"[rule7 twin-scripts] {base}{suffix} is missing: entry "
                    f"scripts must exist on both platforms (TECH_SPECS "
                    f'"开发脚本跨平台对齐")'
                )
    paired = {f"{b}.{s}" for b in REQUIRED_PAIRS for s in ("sh", "bat")}
    for name in sorted(on_disk - paired - set(SINGLE_PLATFORM_SCRIPTS)):
        errors.append(
            f"[rule7 twin-scripts] {name} in {SCRIPT_DIR} "
            f"is neither a required pair nor declared single-platform: give it "
            f"a twin, or add it to SINGLE_PLATFORM_SCRIPTS with a reason and "
            f"document it in TECH_SPECS"
        )
    for name in sorted(set(SINGLE_PLATFORM_SCRIPTS) - on_disk):
        errors.append(
            f"[rule7 twin-scripts] SINGLE_PLATFORM_SCRIPTS lists {name}, which "
            f"is no longer on disk — drop the entry (and the TECH_SPECS row)"
        )
    modes = _index_modes(SCRIPT_DIR)
    for path, mode in sorted(modes.items()):
        if path.endswith(".sh") and mode != "100755":
            errors.append(
                f"[rule7 twin-scripts] {path} is {mode} in the git index; a "
                f"clone cannot `./` it — run `git update-index --chmod=+x {path}`"
            )
    return errors


def main() -> int:
    violations = (check_asset_inventory() + check_doc_paths()
                  + check_gtest_vocabulary() + check_migration_versions()
                  + check_twin_parity() + check_version_stamps()
                  + check_script_twins())
    if violations:
        print(f"Docs drift guard FAILED ({len(violations)} violation(s)):")
        for v in violations:
            print("  " + v)
        return 1
    print("Docs drift guard passed (7 rules: inventory, dead-paths, "
          "gtest-vocab, migration-versions, twin-parity, no-version-stamps, "
          "twin-scripts).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
