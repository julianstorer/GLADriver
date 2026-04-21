#!/bin/bash
# Build and launch the GLA Injector control app.
# Usage: ./dev-run-app.sh [build-dir] [Release|Debug]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${1:-$SCRIPT_DIR/build}"
CONFIG="${2:-Debug}"
APP_BUNDLE="$BUILD_DIR/app/gla_app_artefacts/$CONFIG/GLA Injector.app"

echo "==> Building gla_app ($CONFIG)..."
cmake --build "$BUILD_DIR" --config "$CONFIG" --parallel "$(sysctl -n hw.ncpu)" --target gla_app

echo "==> Launching $APP_BUNDLE ..."
open "$APP_BUNDLE"
