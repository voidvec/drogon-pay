#!/bin/bash
# Build script for the Pay Plugin backend (Conan 2 + CMake presets workflow).
#
# POSIX twin of build.bat: same flags, same steps, same artifacts, with the
# preset chosen by host so one script covers Linux and macOS.
#
#   ./build.sh              Release build (linux-release / macos-arm64)
#   ./build.sh -release     same, explicit
#   ./build.sh -debug       Debug build (linux-debug / macos-debug)
#
# Note -debug is a plain Debug build. The instrumented coverage build is a
# different preset (linux-coverage) with its own recipe in TECH_SPECS.md.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

case "$(uname -s)" in
  Linux)
    PRESET_RELEASE=linux-release
    PRESET_DEBUG=linux-debug
    JOBS="$(nproc)"
    ;;
  Darwin)
    PRESET_RELEASE=macos-arm64
    PRESET_DEBUG=macos-debug
    JOBS="$(sysctl -n hw.ncpu)"
    ;;
  *)
    echo "Unsupported platform: $(uname -s) (use build.bat on Windows)" >&2
    exit 1
    ;;
esac

echo "========================================"
echo "Pay Plugin Backend Build Script"
echo "========================================"

# A stale PayServer holds the output binary open on Windows and steals the
# listener port on Unix, so both flavours kill it first.
if pkill -x PayServer 2>/dev/null; then
  echo "Killed running PayServer process"
  sleep 1
else
  echo "No running PayServer process found"
fi

cd "$REPO_ROOT"
echo "Working directory: $PWD"

BUILD_TYPE=Release
PRESET="$PRESET_RELEASE"

while [[ $# -gt 0 ]]; do
  case "$1" in
    -debug)
      BUILD_TYPE=Debug
      PRESET="$PRESET_DEBUG"
      ;;
    -release)
      BUILD_TYPE=Release
      PRESET="$PRESET_RELEASE"
      ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [-debug|-release]" >&2
      echo "  -debug     Build debug version (preset $PRESET_DEBUG)" >&2
      echo "  -release   Build release version (default, preset $PRESET_RELEASE)" >&2
      exit 1
      ;;
  esac
  shift
done

# macOS needs the arch setting on the Conan side too, matching the preset's
# CMAKE_OSX_ARCHITECTURES; the Windows preset carries it in `architecture`.
CONAN_EXTRA=()
if [[ "$(uname -s)" == "Darwin" ]]; then
  CONAN_EXTRA+=(-s arch=armv8)
fi

echo "Building with configuration:"
echo "  Build Type: $BUILD_TYPE"
echo "  Preset:     $PRESET"
echo

# Install dependencies (generates build/$PRESET/.../conan_toolchain.cmake, which
# the preset's CMAKE_TOOLCHAIN_FILE points at).
echo "Installing dependencies via Conan..."
conan install . --output-folder="build/$PRESET" -s build_type="$BUILD_TYPE" \
  -s compiler.cppstd=17 "${CONAN_EXTRA[@]}" --build=missing

echo "Configuring project..."
cmake --preset "$PRESET"

echo "Building project..."
cmake --build --preset "$PRESET" -j"$JOBS"

# Unix Makefiles is single-config, so the binaries sit next to the CMake cache
# (the MSVC twin of this script nests them under Release/ or Debug/).
OUT_DIR="build/$PRESET/examples/pay-server"
TEST_OUT_DIR="build/$PRESET/tests"
HOST_DIR=examples/pay-server

# The server and the test binary read config.json/.env from their working
# directory, so both output trees get a copy (build.bat's robocopy step).
echo "Copying configuration files..."
for dest in "$OUT_DIR" "$TEST_OUT_DIR"; do
  if [[ ! -d "$dest" ]]; then
    continue
  fi
  for file in config.json .env; do
    if [[ -f "$HOST_DIR/$file" ]]; then
      cp -f "$HOST_DIR/$file" "$dest/"
    fi
  done
  if [[ -d "$HOST_DIR/certs" ]]; then
    mkdir -p "$dest/certs"
    cp -rf "$HOST_DIR/certs/." "$dest/certs/"
  fi
done

echo "========================================"
echo "Build complete!"
echo "  Server: $OUT_DIR/PayServer"
echo "  Tests:  $TEST_OUT_DIR/PayBackendTests"
echo "  Run:    ctest --test-dir build/$PRESET --output-on-failure"
echo "========================================"
