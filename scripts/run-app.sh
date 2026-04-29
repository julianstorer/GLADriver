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

# Driver logs (syslog from GLAInjector.driver loaded inside coreaudiod).
# App DBG() output goes to stderr and appears directly in this terminal.
log stream --predicate 'senderImagePath CONTAINS "GLAInjector" AND message CONTAINS "GLA:"' --level info --style compact &
DRV_LOG_PID=$!

trap "kill $DRV_LOG_PID 2>/dev/null" EXIT
# Suppress the la-avdecc watchdog assert: the state machine thread and the bridge
# completion-handler thread acquire the Manager lock and bridge lock in opposite
# order on startup, triggering the watchdog in Debug builds. The deadlock resolves
# itself once initial ADP discovery settles; this just prevents the abort.
AVDECC_NO_WATCHDOG_ASSERT=1 "$APP_BUNDLE/Contents/MacOS/GLA Injector"
