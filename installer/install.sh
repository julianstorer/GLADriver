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

# Install daemon.
echo "Installing daemon ..."
sudo cp "$BUILD_DIR/bin/gla_daemon" /usr/local/bin/
sudo chmod 755 /usr/local/bin/gla_daemon

# Install launchd plist (user agent, so no sudo needed for the plist itself).
PLIST="$BUILD_DIR/../daemon/plist/com.greenlight.gla-daemon.plist"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
mkdir -p "$LAUNCH_AGENTS"
cp "$PLIST" "$LAUNCH_AGENTS/"
launchctl unload "$LAUNCH_AGENTS/com.greenlight.gla-daemon.plist" 2>/dev/null || true
launchctl load "$LAUNCH_AGENTS/com.greenlight.gla-daemon.plist"

echo ""
echo "Installation complete!"
echo "Verify with: system_profiler SPAudioDataType | grep -A5 'GLA Injector'"
echo "Daemon logs: tail -f /tmp/gla-daemon.log"
