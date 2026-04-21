#!/bin/bash
# Fast dev-cycle script: build driver, sign, install, restart coreaudiod.
# Usage: ./dev-driver-install.sh [build-dir]
# Requires sudo (will prompt once).
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${1:-$SCRIPT_DIR/build}"
DRIVER_SRC="$BUILD_DIR/output/GLAInjector.driver"
HAL_DIR="/Library/Audio/Plug-Ins/HAL"
DRIVER_DST="$HAL_DIR/GLAInjector.driver"

# Build driver target only.
echo "==> Building..."
cmake --build "$BUILD_DIR" --parallel "$(sysctl -n hw.ncpu)" --target gla_driver

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

echo "==> Verifying (waiting up to 15s for driver to appear)..."
FOUND=0
for i in $(seq 1 15); do
    sleep 1
    if system_profiler SPAudioDataType 2>/dev/null | grep -q "GreenLight AVB\|GLA Injector"; then
        FOUND=1
        break
    fi
    printf "    %ds...\n" "$i"
done

if [ $FOUND -eq 1 ]; then
    echo "    Driver is visible: GreenLight AVB found in CoreAudio."
    echo "Done."
else
    echo "    ERROR: driver did not appear after 15s."
    echo "    GLA driver syslog (last 30s):"
    /usr/bin/log show --last 30s --info --predicate 'process == "coreaudiod"' 2>/dev/null \
        | grep -iE "GLA|GLAInjector|GreenLight" | tail -20 || true
    echo "    coreaudiod errors (last 30s):"
    /usr/bin/log show --last 30s --info --predicate 'process == "coreaudiod"' 2>/dev/null \
        | grep -iE "error|fail|unable|invalid|crash" | tail -10 || true
    exit 1
fi
