#!/bin/bash
set -e

echo "=== GLA Injector Uninstaller ==="

# Remove HAL driver.
sudo rm -rf "/Library/Audio/Plug-Ins/HAL/GLAInjector.driver"
echo "HAL driver removed."

# Restart coreaudiod.
sudo launchctl kickstart -k system/com.apple.audio.coreaudiod 2>/dev/null || true

echo "Uninstall complete."
