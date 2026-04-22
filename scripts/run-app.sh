#!/bin/bash
# Build and launch the GLA Injector control app.
# Usage: ./run run-app [build-dir] [Release|Debug]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${1:-$REPO_ROOT/build}"
CONFIG="${2:-Debug}"
APP_BUNDLE="$BUILD_DIR/app/gla_app_artefacts/$CONFIG/GLA Injector.app"

"$SCRIPT_DIR/build-app.sh" "$BUILD_DIR" "$CONFIG"

echo "==> Running $APP_BUNDLE ..."
"$APP_BUNDLE/Contents/MacOS/GLA Injector"
