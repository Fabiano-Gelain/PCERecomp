#!/usr/bin/env bash
# Builds PCERecomp standalone (Release) into build/ and, with -Test, runs its tests.
# (Inside a game project, the game's build.sh does the build.)
# Usage:  ./build.sh          -> build
#         ./build.sh -Test    -> build and run the tests
#         -Verbose            -> show all output

set -euo pipefail

TEST=false
VERBOSE=false

while [[ $# -gt 0 ]]; do
    case $1 in
        -Test) TEST=true ;;
        -Verbose) VERBOSE=true ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
    shift
done

CMAKE_BIN=$(command -v cmake)
if [[ -z "$CMAKE_BIN" ]]; then
    echo "CMake not found in PATH"
    exit 1
fi

CTEST_BIN=$(command -v ctest)
if [[ -z "$CTEST_BIN" ]]; then
    echo "ctest not found in PATH"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

BUILD_DIR="build"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    if [[ "$VERBOSE" == true ]]; then
        "$CMAKE_BIN" -S . -B "$BUILD_DIR" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
    else
        "$CMAKE_BIN" -S . -B "$BUILD_DIR" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release 2>&1 | grep -iE 'error|warning' || true
    fi
fi

if [[ "$VERBOSE" == true ]]; then
    "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --parallel
else
    "$CMAKE_BIN" --build "$BUILD_DIR" --config Release --parallel 2>&1 | grep -iE 'warning|error|fatal' || true
fi

echo "Build OK."

if [[ "$TEST" == true ]]; then
    NPROC=$(nproc)
    JOBS=$((NPROC / 2))
    [[ $JOBS -lt 1 ]] && JOBS=1

    CTEST_ARGS=("--test-dir" "$BUILD_DIR" "-C" "Release" "-j" "$JOBS")
    if [[ "$VERBOSE" == true ]]; then
        CTEST_ARGS+=("--output-on-failure")
    fi

    "$CTEST_BIN" "${CTEST_ARGS[@]}"
fi
