#!/bin/bash
# Test runner for the Pay Plugin backend (CTest).
#
# POSIX twin of test.bat: same flags, same behaviour.
#
#   ./test.sh                          run everything (ctest)
#   ./test.sh -l                       list exact test names
#   ./test.sh -r WechatPayClient_X     run one test by exact name
#   ./test.sh -v                       verbose (already the default, kept for parity)
#   ./test.sh -o                       also write test_results.log in the build dir
#   ./test.sh -debug                   use the Debug preset instead of Release
#
# -l/-r call the test binary rather than `ctest -R`: only one ctest case is
# registered, so a ctest name filter would match nothing and exit 0.
#
# The suite needs Postgres + Redis on 127.0.0.1 and reads its credentials from
# the .env that build.sh copies next to the test binary, so nothing here sets a
# password (test.bat's DB_PASS line was dead code: no source file reads it).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s)" in
  Linux)
    PRESET_RELEASE=linux-release
    PRESET_DEBUG=linux-debug
    ;;
  Darwin)
    PRESET_RELEASE=macos-arm64
    PRESET_DEBUG=macos-debug
    ;;
  *)
    echo "Unsupported platform: $(uname -s) (use test.bat on Windows)" >&2
    exit 1
    ;;
esac

LIST_TESTS=0
TEST_PATTERN=""
# Verbose by default, matching test.bat (which sets VERBOSE=1 up front, so `-v`
# is accepted there as well but is not what turns the detail output on).
VERBOSE=1
SAVE_LOG=0
PRESET="$PRESET_RELEASE"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -l) LIST_TESTS=1 ;;
    -r)
      shift
      if [[ $# -eq 0 ]]; then
        echo "Error: -r needs an exact test name (run -l to list them)" >&2
        exit 1
      fi
      TEST_PATTERN="$1"
      ;;
    -v) VERBOSE=1 ;;
    -o) SAVE_LOG=1 ;;
    -debug) PRESET="$PRESET_DEBUG" ;;
    -release) PRESET="$PRESET_RELEASE" ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [-l] [-r ExactTestName] [-v] [-o] [-debug|-release]" >&2
      echo "  -l             List available tests" >&2
      echo "  -r <name>      Run one test by its exact name (see -l)" >&2
      echo "  -v             Verbose output (show test details)" >&2
      echo "  -o             Also save test_results.log in the build directory" >&2
      echo "  -debug         Use the Debug preset (default: $PRESET_RELEASE)" >&2
      exit 1
      ;;
  esac
  shift
done

BUILD_DIR="$REPO_ROOT/build/$PRESET"
if [[ ! -d "$BUILD_DIR" ]]; then
  echo "Error: build/$PRESET does not exist" >&2
  echo "Build the project first: examples/pay-server/scripts/build.sh" >&2
  exit 1
fi

cd "$BUILD_DIR"
echo "Build directory: $PWD"
echo

if [[ ! -f CTestTestfile.cmake && ! -f tests/CTestTestfile.cmake ]]; then
  echo "Error: no CTest configuration under $PWD" >&2
  echo "Expected CTestTestfile.cmake or tests/CTestTestfile.cmake - configure" >&2
  echo "and build before testing (build.sh does both)." >&2
  exit 1
fi

# tests/CMakeLists.txt registers exactly ONE ctest case that runs the whole
# suite, so `ctest -N` lists one line and `ctest -R Foo` matches nothing yet
# still exits 0 - a filter that reads as a pass. -l and -r therefore drive the
# test binary directly: its -r takes an exact DROGON_TEST name and exits 1 when
# no such case exists. The binary has to run from its own directory so the
# ./config.json and ./.env that build.sh copies there resolve; -C is a no-op for
# the single-config generator these platforms use.
TEST_EXE=""
for candidate in tests/PayBackendTests tests/Release/PayBackendTests; do
  if [[ -f "$candidate" ]]; then
    TEST_EXE="$candidate"
    break
  fi
done

if [[ $LIST_TESTS -eq 1 ]]; then
  if [[ -z "$TEST_EXE" ]]; then
    echo "Error: no PayBackendTests binary under $PWD/tests" >&2
    echo "Build the tests first: examples/pay-server/scripts/build.sh" >&2
    exit 1
  fi
  echo "========================================"
  echo "Available Tests (exact names accepted by -r)"
  echo "========================================"
  status=0
  ( cd "$(dirname "$TEST_EXE")" && "./$(basename "$TEST_EXE")" -l ) || status=$?
  exit "$status"
fi

CTEST=(ctest --output-on-failure)
if [[ $VERBOSE -eq 1 ]]; then
  CTEST+=(-V)
fi

if [[ -n "$TEST_PATTERN" ]]; then
  if [[ -z "$TEST_EXE" ]]; then
    echo "Error: no PayBackendTests binary under $PWD/tests" >&2
    echo "Build the tests first: examples/pay-server/scripts/build.sh" >&2
    exit 1
  fi
  RUN_DIR="$(dirname "$TEST_EXE")"
  RUN_CMD=("./$(basename "$TEST_EXE")" -r "$TEST_PATTERN")
  RUN_LABEL="Running Test: $TEST_PATTERN"
else
  # An all-tests run also has to prove there is something to run: ctest exits 0
  # when it has no cases registered, so a build that skipped the test target
  # would otherwise print "ALL TESTS PASSED" over an empty suite.
  TEST_COUNT="$(ctest -N 2>/dev/null | sed -n 's/^Total Tests:[[:space:]]*\([0-9][0-9]*\)$/\1/p' || true)"
  if [[ -z "$TEST_COUNT" || "$TEST_COUNT" -eq 0 ]]; then
    echo "Error: ctest lists no tests under $PWD" >&2
    echo "An empty suite exits 0, so refuse to call it a pass - build the" >&2
    echo "PayBackendTests target first (build.sh)." >&2
    exit 1
  fi
  RUN_DIR="."
  RUN_CMD=("${CTEST[@]}")
  RUN_LABEL="Running All Tests ($TEST_COUNT ctest case(s))"
fi

echo "========================================"
echo "$RUN_LABEL"
echo "========================================"
echo

if [[ $SAVE_LOG -eq 1 ]]; then
  # tee rather than `> file` + `cat`: the run stays visible while it happens,
  # which is what test.bat's `-o` path does. The subshell moves the test's cwd;
  # tee keeps writing into the build directory, which is where `-o` always put
  # the log.
  ( cd "$RUN_DIR" && "${RUN_CMD[@]}" ) 2>&1 | tee test_results.log
  status=${PIPESTATUS[0]}
  echo "Results saved to: $PWD/test_results.log"
else
  status=0
  ( cd "$RUN_DIR" && "${RUN_CMD[@]}" ) || status=$?
fi

if [[ $status -ne 0 ]]; then
  echo
  echo "========================================"
  echo "TESTS FAILED"
  echo "========================================"
  if [[ -n "$TEST_PATTERN" ]]; then
    echo "If -r was used, the name may not exist: run '$0 -l' for the exact case names." >&2
  fi
  exit "$status"
fi

echo
echo "========================================"
if [[ -n "$TEST_PATTERN" ]]; then
  echo "TEST PASSED: $TEST_PATTERN"
else
  echo "ALL TESTS PASSED"
fi
echo "========================================"
