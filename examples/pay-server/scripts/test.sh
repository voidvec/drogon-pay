#!/bin/bash
# Test runner for the Pay Plugin backend (CTest).
#
# POSIX twin of test.bat: same flags, same ctest invocations.
#
#   ./test.sh                 run everything
#   ./test.sh -l              list the registered tests
#   ./test.sh -r Idempotency  run tests matching a pattern
#   ./test.sh -v              verbose (already the default, kept for parity)
#   ./test.sh -o              also write test_results.log in the build dir
#   ./test.sh -debug          use the Debug preset instead of Release
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
        echo "Missing argument for -r (a test name pattern)" >&2
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
      echo "Usage: $0 [-l] [-r pattern] [-v] [-o] [-debug|-release]" >&2
      echo "  -l            List available tests" >&2
      echo "  -r <pattern>  Run tests matching pattern (e.g. -r Idempotency)" >&2
      echo "  -v            Verbose output (show test details)" >&2
      echo "  -o            Also save test_results.log in the build directory" >&2
      echo "  -debug        Use the Debug preset (default: $PRESET_RELEASE)" >&2
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

# ctest -N already prints the test names, so -v is not added there; -C is a
# no-op for the single-config Makefile generator these platforms use.
CTEST=(ctest --output-on-failure)
if [[ $VERBOSE -eq 1 ]]; then
  CTEST+=(-V)
fi
if [[ -n "$TEST_PATTERN" ]]; then
  CTEST+=(-R "$TEST_PATTERN")
fi

if [[ $LIST_TESTS -eq 1 ]]; then
  echo "========================================"
  echo "Available Tests"
  echo "========================================"
  ctest -N
  exit 0
fi

echo "========================================"
if [[ -n "$TEST_PATTERN" ]]; then
  echo "Running Tests Matching: $TEST_PATTERN"
else
  echo "Running All Tests"
fi
echo "========================================"
echo

if [[ $SAVE_LOG -eq 1 ]]; then
  # tee rather than `> file` + `type`: the run stays visible while it happens,
  # which is what test.bat's `-o` path does.
  "${CTEST[@]}" 2>&1 | tee test_results.log
  status=${PIPESTATUS[0]}
  echo "Results saved to: $PWD/test_results.log"
else
  status=0
  "${CTEST[@]}" || status=$?
fi

if [[ $status -ne 0 ]]; then
  echo
  echo "========================================"
  echo "TESTS FAILED"
  echo "========================================"
  exit "$status"
fi

echo
echo "========================================"
echo "ALL TESTS PASSED"
echo "========================================"
