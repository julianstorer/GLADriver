#!/bin/bash
# Build and run the GLA terminal monitor.
# Usage: ./run run-monitor [build-dir] [Release|Debug]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR="${1:-$REPO_ROOT/build}"
CONFIG="${2:-Debug}"

"$SCRIPT_DIR/build-monitor.sh" "$BUILD_DIR" "$CONFIG"

echo "==> Running test_monitor..."
"$BUILD_DIR/monitor_app/test_monitor"
