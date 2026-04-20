#!/bin/bash
set -e

echo "=== GLA Injector Uninstaller ==="

LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
PLIST="$LAUNCH_AGENTS/com.greenlight.gla-daemon.plist"

# Stop and unload daemon.
if [ -f "$PLIST" ]; then
    launchctl unload "$PLIST" 2>/dev/null || true
    rm -f "$PLIST"
    echo "Daemon unloaded."
fi

# Remove daemon binary.
sudo rm -f /usr/local/bin/gla_daemon

# Remove HAL driver.
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/GLAInjector.driver"
echo "HAL driver removed."

# Restart coreaudiod.
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || true

echo "Uninstall complete."
