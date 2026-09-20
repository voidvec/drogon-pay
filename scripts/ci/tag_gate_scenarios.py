#!/usr/bin/env python3
"""Replay the tag gate's decision table against a stand-in API.

The gate in `.github/workflows/_tag-gate.yml` is a bash loop that reads GitHub's
check-run API and decides whether a `v*` tag may ship. Its verdicts are the
release policy, and none of them are exercised by the C++ suite, so this script
extracts the *live* script bytes from the workflow file -- never a copy -- and
drives them through the cases that decide a release: which verdict wins when a
name was reported several times, what a never-started pipeline does, what a
damaged policy list does.

    python3 scripts/ci/tag_gate_scenarios.py          # stubbed, offline
    python3 scripts/ci/tag_gate_scenarios.py --live   # read-only calls to GitHub

`--live` needs an authenticated `gh` and asks it about this repository's real
tags; it performs GETs only. Both modes must exit 0 before a change to the gate
is worth reviewing.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
GATE_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "_tag-gate.yml"

# The five contexts ci.yml reports for a master push, plus ci.yml's own entry
# jobs -- the same strings the workflow's `env:` block carries. Kept here so a
# drift between the two files is what fails, not a typo in one of them.
REQUIRED_CHECKS = "\n".join(
    [
        "linux-build-and-test / build-test",
        "windows-build-and-test / build-test",
        "macos-build / build-test",
        "sdk-smoke-linux / sdk-smoke",
        "sdk-smoke-windows / sdk-smoke",
    ]
)
ENTRY_CHECKS = "static-analysis,clang-tidy"

# Hard wall-clock budget per case, independent of the deadline a case sets for
# the gate itself.
CASE_TIMEOUT_SECONDS = 60

STUB_GH = """#!/usr/bin/env bash
# Stand-in for the handful of `gh api` reads the gate performs. Paths select
# the response; STUB_* hold the canned payloads.
set -euo pipefail
path="${2:-}"
case "$path" in
  *"/compare/"*)
    printf '%s\\n' "${STUB_COMPARE:-behind}"
    ;;
  *check-runs*)
    printf '%b' "${STUB_RUNS-}"
    ;;
  *commits*)
    printf '%s\\n' "${STUB_SHA:-0000000000000000000000000000000000000000}"
    ;;
  *)
    echo "stub gh: unhandled path: $path" >&2
    exit 1
    ;;
esac
"""


def extract_gate_script(workflow: Path) -> str:
    """Return the gate's `run:` body, dedented, exactly as Actions will run it."""
    lines = workflow.read_text(encoding="utf-8").splitlines(keepends=True)
    heads = [i for i, line in enumerate(lines) if line.rstrip() == "        run: |"]
    if len(heads) != 1:
        raise SystemExit(
            f"{workflow.name}: expected exactly one 8-space `run: |` block, found {len(heads)}."
            " This script judges the gate; if the gate gained a step, teach this where to look."
        )
    body: list[str] = []
    for line in lines[heads[0] + 1 :]:
        stripped = line.rstrip("\n")
        if stripped.strip() == "":
            body.append("")
            continue
        if not stripped.startswith(" " * 10):
            break
        body.append(stripped[10:])
    if not body:
        raise SystemExit(f"{workflow.name}: the `run: |` block is empty")
    return "\n".join(body) + "\n"


def run(bash: str, workspace: Path, script: Path, env: dict[str, str]) -> tuple[int, str]:
    # Every case here must reach a verdict on its first or second pass, so a
    # minute of no answer *is* the failure: without this, a case whose early-bail
    # or deadline logic broke would poll its full DEADLINE_MINUTES at
    # POLL_SECONDS=1 and the suite would look hung rather than wrong.
    try:
        proc = subprocess.run(
            [bash, str(script)],
            env=env,
            capture_output=True,
            text=True,
            errors="replace",
            cwd=workspace,
            shell=False,
            timeout=CASE_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        return 124, f"case ran past {CASE_TIMEOUT_SECONDS}s without deciding\n{_as_text(exc.stdout)}{_as_text(exc.stderr)}"
    return proc.returncode, (proc.stdout or "") + (proc.stderr or "")


def _as_text(chunk) -> str:
    if chunk is None:
        return ""
    return chunk if isinstance(chunk, str) else chunk.decode("utf-8", "replace")


def check(results: list[str], label: str, expect: str, code: int, out: str, want_code: int) -> None:
    ok = expect in out and code == want_code
    detail = "" if ok else f"  (exit={code}, wanted {want_code}; no match for {expect!r})"
    print(f"{'PASS' if ok else 'FAIL'}  {label}{detail}")
    if not ok:
        print("".join(f"      | {line}\n" for line in out.splitlines()))
        results.append(label)


def stubbed_cases() -> list[tuple[str, str, dict[str, str], int]]:
    """(label, expected substring, extra env, expected exit code)."""
    green = (
        "100\\tlinux-build-and-test / build-test\\tcompleted\\tsuccess\\n"
        "101\\twindows-build-and-test / build-test\\tcompleted\\tsuccess\\n"
        "102\\tmacos-build / build-test\\tcompleted\\tsuccess\\n"
        "103\\tsdk-smoke-linux / sdk-smoke\\tcompleted\\tsuccess\\n"
        "104\\tsdk-smoke-windows / sdk-smoke\\tcompleted\\tsuccess\\n"
    )
    base = {"DEADLINE_MINUTES": "10", "EARLY_BAIL_SECONDS": "300", "POLL_SECONDS": "1"}

    def env_for(runs: str, **over: str) -> dict[str, str]:
        env = {"STUB_RUNS": runs, **base, **over}
        return env

    return [
        (
            "tag name that is not three numerics is refused",
            "refusing unexpected tag name",
            {"TAG_NAME": "v1.0.0-rc.1"},
            1,
        ),
        (
            "tag on a commit outside master is refused",
            "is not on master",
            {"TAG_NAME": "v1.1.0", "STUB_COMPARE": "ahead"},
            1,
        ),
        (
            "all five contexts green releases",
            "Merge pipeline is green",
            env_for(green, TAG_NAME="v1.1.0", DEADLINE_MINUTES="2"),
            0,
        ),
        (
            "running and absent both wait, and the list stays on one line",
            "gave up after 0m waiting for: macos-build / build-test: still running;"
            " sdk-smoke-windows / sdk-smoke: no check run on this commit",
            env_for(
                "100\\tlinux-build-and-test / build-test\\tcompleted\\tsuccess\\n"
                "101\\twindows-build-and-test / build-test\\tcompleted\\tsuccess\\n"
                "102\\tmacos-build / build-test\\tin_progress\\t-\\n"
                "103\\tsdk-smoke-linux / sdk-smoke\\tcompleted\\tsuccess\\n",
                TAG_NAME="v1.1.0",
                DEADLINE_MINUTES="0",
            ),
            1,
        ),
        (
            "a failed leg aborts and names its conclusion",
            "windows-build-and-test / build-test: failure",
            env_for(
                green.replace(
                    "101\\twindows-build-and-test / build-test\\tcompleted\\tsuccess",
                    "101\\twindows-build-and-test / build-test\\tcompleted\\tfailure",
                ),
                TAG_NAME="v1.1.0",
            ),
            1,
        ),
        (
            "a stale cancelled at a lower id cannot block the release",
            "Merge pipeline is green",
            env_for("50\\twindows-build-and-test / build-test\\tcompleted\\tcancelled\\n" + green, TAG_NAME="v1.1.0"),
            0,
        ),
        (
            "the newest attempt being red wins over an older green",
            "the merge pipeline is red on",
            env_for(
                "50\\tmacos-build / build-test\\tcompleted\\tsuccess\\n"
                + green.replace(
                    "102\\tmacos-build / build-test\\tcompleted\\tsuccess",
                    "102\\tmacos-build / build-test\\tcompleted\\ttimed_out",
                ),
                TAG_NAME="v1.1.0",
            ),
            1,
        ),
        (
            "the newest attempt being cancelled reports that conclusion",
            "macos-build / build-test: cancelled",
            env_for(
                "50\\tmacos-build / build-test\\tcompleted\\tsuccess\\n"
                + green.replace(
                    "102\\tmacos-build / build-test\\tcompleted\\tsuccess",
                    "102\\tmacos-build / build-test\\tcompleted\\tcancelled",
                ),
                TAG_NAME="v1.1.0",
            ),
            1,
        ),
        (
            "id comparison is numeric, not lexicographic",
            "Merge pipeline is green",
            env_for(
                "9\\tsdk-smoke-windows / sdk-smoke\\tcompleted\\tfailure\\n" + green,
                TAG_NAME="v1.1.0",
            ),
            0,
        ),
        (
            # The release workflow reports its own check runs against the tagged
            # commit, so an empty listing is not the signal; only ci.yml's entry
            # jobs prove the merge pipeline ever started here.
            "a commit the pipeline never ran on bails in seconds",
            "the merge pipeline never started on it",
            env_for(
                "200\\tversion-check\\tcompleted\\tsuccess\\n"
                "201\\tci-gate / tag-gate\\tin_progress\\t-\\n",
                TAG_NAME="v1.1.0",
                EARLY_BAIL_SECONDS="0",
            ),
            1,
        ),
        (
            "entry job present: no early bail, the five are still awaited",
            "gave up after 0m waiting for: linux-build-and-test / build-test: no check run on this commit",
            env_for(
                "200\\tstatic-analysis\\tcompleted\\tsuccess\\n"
                "201\\tversion-check\\tcompleted\\tsuccess\\n",
                TAG_NAME="v1.1.0",
                EARLY_BAIL_SECONDS="0",
                DEADLINE_MINUTES="0",
            ),
            1,
        ),
        (
            "an empty ENTRY_CHECKS is refused, not treated as 'never ran'",
            "ENTRY_CHECKS is not a comma-separated list",
            env_for(green, TAG_NAME="v1.1.0", ENTRY_CHECKS=""),
            1,
        ),
        (
            "a truncated context list stops the release instead of certifying it",
            "refusing to certify a release against a list that is not the pipeline's",
            {
                "TAG_NAME": "v1.1.0",
                "REQUIRED_CHECKS": "\n".join(REQUIRED_CHECKS.split("\n")[:3]),
                "STUB_RUNS": green,
                **base,
            },
            1,
        ),
        (
            "an empty context list cannot read as 'nothing pending'",
            "expected 5",
            {"TAG_NAME": "v1.1.0", "REQUIRED_CHECKS": "", "STUB_RUNS": green, **base},
            1,
        ),
        (
            "a blank timing knob is rejected, not read as zero",
            "DEADLINE_MINUTES is not a number",
            env_for(green, TAG_NAME="v1.1.0", DEADLINE_MINUTES=""),
            1,
        ),
        (
            "a blank line inside the list is skipped, not miscounted",
            "gave up after 0m waiting for: windows-build-and-test / build-test: no check run on this commit",
            {
                "TAG_NAME": "v1.1.0",
                "REQUIRED_CHECKS": REQUIRED_CHECKS.replace(
                    "windows-build-and-test / build-test",
                    "\nwindows-build-and-test / build-test",
                ),
                "STUB_RUNS": "100\\tlinux-build-and-test / build-test\\tcompleted\\tsuccess\\n",
                "DEADLINE_MINUTES": "0",
                "EARLY_BAIL_SECONDS": "300",
                "POLL_SECONDS": "1",
            },
            1,
        ),
        (
            "a branch dispatch has no tag to certify and passes through",
            "nothing to certify",
            {"TAG_NAME": "master", "REF_TYPE": "branch", "STUB_RUNS": "", **base},
            0,
        ),
    ]


def live_cases() -> list[tuple[str, str, dict[str, str], int]]:
    """v1.0.0 is the release that shipped red; the gate must say so from the record."""
    legacy_red = "\n".join(
        [
            "linux-build-and-test",
            "windows-build-and-test",
            "macos-build-and-test",
            "conan-create",
            "static-analysis",
        ]
    )
    legacy_green = "\n".join(
        [
            "linux-build-and-test",
            "macos-build-and-test",
            "conan-create",
            "static-analysis",
            "gitleaks",
        ]
    )
    base = {"TAG_NAME": "v1.0.0", "DEADLINE_MINUTES": "10", "EARLY_BAIL_SECONDS": "300", "POLL_SECONDS": "1"}
    return [
        (
            "live: v1.0.0 judged by today's five contexts",
            "no check run on this commit",
            {**base, "REQUIRED_CHECKS": REQUIRED_CHECKS, "DEADLINE_MINUTES": "0"},
            1,
        ),
        (
            "live: v1.0.0 judged by the contexts it actually reported",
            "windows-build-and-test: failure",
            {**base, "REQUIRED_CHECKS": legacy_red},
            1,
        ),
        (
            "live: an all-green subset of v1.0.0's contexts releases",
            "Merge pipeline is green",
            {**base, "REQUIRED_CHECKS": legacy_green},
            0,
        ),
    ]


def main() -> int:
    bash = shutil.which("bash")
    if not bash:
        raise SystemExit("bash is required to replay the gate (Git Bash on Windows)")
    missing = [tool for tool in ("awk", "gh") if not shutil.which(tool)]
    if "awk" in missing:
        raise SystemExit("awk is required: the verdict table is an awk program")
    live = "--live" in sys.argv[1:]
    if live and "gh" in missing:
        raise SystemExit("--live needs an authenticated gh on PATH")

    script_text = extract_gate_script(GATE_WORKFLOW)
    workspace = Path(tempfile.mkdtemp(prefix="tag-gate-scenarios-"))
    try:
        gate = workspace / "gate.sh"
        gate.write_text(script_text, encoding="utf-8", newline="\n")
        stub_dir = workspace / "stubbin"
        stub_dir.mkdir()
        stub = stub_dir / "gh"
        stub.write_text(STUB_GH, encoding="utf-8", newline="\n")
        stub.chmod(stub.stat().st_mode | 0o111)

        syntax = subprocess.run([bash, "-n", str(gate)], capture_output=True, text=True)
        if syntax.returncode != 0:
            print(syntax.stdout + syntax.stderr)
            raise SystemExit("the extracted gate script does not parse")
        print(f"extracted {len(script_text.splitlines())} lines from {GATE_WORKFLOW.name}; parses\n")

        env = dict(os.environ)
        env.update(
            {
                "REPOSITORY": "voidvec/drogon-pay",
                "REQUIRED_CHECKS": REQUIRED_CHECKS,
                "ENTRY_CHECKS": ENTRY_CHECKS,
                "REF_TYPE": "tag",
            }
        )
        if live:
            # The live cases talk to GitHub: the real client has to win over the
            # stub and no canned payload may be in scope.
            cases = live_cases()
        else:
            env["PATH"] = f"{stub_dir}{os.pathsep}{env['PATH']}"
            env["GH_TOKEN"] = "stub-token"
            cases = stubbed_cases()

        failures: list[str] = []
        for label, expect, over, want_code in cases:
            code, out = run(bash, workspace, gate, {**env, **over})
            check(failures, label, expect, code, out, want_code)

        print(f"\n{'FAILED' if failures else 'all passed'}: {len(cases) - len(failures)}/{len(cases)}")
        return 1 if failures else 0
    finally:
        shutil.rmtree(workspace, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
