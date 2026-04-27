#!/bin/bash
# Build the GLA terminal monitor app.
# Usage: ./run build-monitor [--clean] [build-dir] [Release|Debug]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CLEAN=0
POSITIONAL=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        *) POSITIONAL+=("$1"); shift ;;
    esac
done

BUILD_DIR="${POSITIONAL[0]:-$REPO_ROOT/build}"
CONFIG="${POSITIONAL[1]:-Debug}"

CLEAN_FLAG=""
[ $CLEAN -eq 1 ] && CLEAN_FLAG="--clean-first"

echo "==> Building test_monitor ($CONFIG)..."
cmake --build "$BUILD_DIR" --config "$CONFIG" $CLEAN_FLAG --parallel "$(sysctl -n hw.ncpu)" --target test_monitor
