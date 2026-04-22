#!/bin/bash
# Build the GLA driver target only.
# Usage: ./run build-driver [--clean] [build-dir]
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

if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    echo "==> Configuring..."
    cmake -S "$REPO_ROOT" -B "$BUILD_DIR"
fi

CLEAN_FLAG=""
[ $CLEAN -eq 1 ] && CLEAN_FLAG="--clean-first"

echo "==> Building driver..."
cmake --build "$BUILD_DIR" $CLEAN_FLAG --parallel "$(sysctl -n hw.ncpu)" --target gla_driver
