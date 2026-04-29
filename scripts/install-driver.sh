#!/bin/bash
# Fast dev-cycle script: build driver, sign, install, restart coreaudiod.
# Usage: ./run install-driver [--clean] [build-dir]
# Requires sudo (will prompt once).
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
DRIVER_SRC="$BUILD_DIR/output/GLAInjector.driver"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"
DRIVER_DST="$HAL_DIR/GLAInjector.driver"

# Predicate that isolates our plugin's syslog calls from all other coreaudiod noise.
# senderImagePath identifies the dylib that called syslog(), not the process.
GLA_PRED='senderImagePath CONTAINS "GLAInjector" AND eventMessage CONTAINS "GLA:"'

CLEAN_FLAG=""
[ $CLEAN -eq 1 ] && CLEAN_FLAG="--clean"
"$SCRIPT_DIR/build-driver.sh" $CLEAN_FLAG "$BUILD_DIR"

# Ad-hoc sign.
echo "==> Signing..."
codesign --force --deep --sign "GLA Audio Dev" --options runtime "$DRIVER_SRC"

# Install (requires sudo; run each command directly so launchctl has correct context).
echo "==> Installing (requires sudo)..."
sudo rm -rf "$DRIVER_DST"
sudo cp -R "$DRIVER_SRC" "$HAL_DIR/"
echo "    Installed to $DRIVER_DST"
sudo killall coreaudiod
echo "    coreaudiod restarted (launchd will respawn it)"

echo "==> Verifying (waiting up to 15s for driver to initialise)..."
FOUND=0
for i in $(seq 1 15); do
    sleep 1
    if /usr/bin/log show --last "${i}s" --info \
            --predicate 'senderImagePath CONTAINS "GLAInjector" AND eventMessage CONTAINS "GLA: driver initialized"' \
            2>/dev/null | grep -q "GLA: driver initialized"; then
        FOUND=1
        break
    fi
    printf "    %ds...\n" "$i"
done

if [ $FOUND -eq 1 ]; then
    echo "    Driver initialised successfully."
    echo "Done."
else
    echo "    ERROR: driver did not initialise after 15s."
    echo "    GLA driver syslog (last 30s):"
    /usr/bin/log show --last 30s --info --predicate "$GLA_PRED" 2>/dev/null | tail -20 || true
    echo "    coreaudiod crashes/errors (last 30s):"
    /usr/bin/log show --last 30s --info \
        --predicate 'process == "coreaudiod" AND (eventMessage CONTAINS "error" OR eventMessage CONTAINS "crash" OR eventMessage CONTAINS "fault")' \
        2>/dev/null | tail -10 || true
    echo ""
    echo "==> Auto-recovering: removing broken driver to restore audio..."
    "$SCRIPT_DIR/recover-driver.sh"
    exit 1
fi
