#!/bin/bash
set -e

BUILD_DIR="${1:-build}"

echo "=== GLA Injector Installer ==="

# Install HAL driver.
DRIVER="$BUILD_DIR/output/GLAInjector.driver"
if [ ! -d "$DRIVER" ]; then
    echo "ERROR: driver bundle not found at $DRIVER"
    echo "Build with: cmake --build $BUILD_DIR && cmake --install $BUILD_DIR"
    exit 1
fi

HAL_DIR="/Library/Audio/Plug-Ins/HAL"
echo "Installing HAL driver to $HAL_DIR ..."
sudo cp -R "$DRIVER" "$HAL_DIR/"
sudo chown -R root:wheel "$HAL_DIR/GLAInjector.driver"

# Kill and restart coreaudiod to pick up the new driver.
echo "Restarting coreaudiod ..."
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || \
    sudo kill $(cat /var/run/coreaudiod.pid 2>/dev/null) 2>/dev/null || true
sleep 2

echo ""
echo "Installation complete!"
echo "Verify with: system_profiler SPAudioDataType | grep -A5 'GLA Injector'"
echo "Launch 'GLA Injector.app' to enable AVB routing."
