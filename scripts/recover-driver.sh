#!/bin/bash
# Emergency recovery: removes GLA driver and restores coreaudiod to a clean state.
# Run as: sudo ./scripts/recover-driver.sh
# Works from a second Terminal window or via SSH if the desktop is frozen.
set -e

DRIVER="/Library/Audio/Plug-Ins/HAL/GLAInjector.driver"

echo "==> Force-killing coreaudiod (SIGKILL)..."
sudo killall -9 coreaudiod 2>/dev/null || true
# launchd respawns immediately; the new instance may still load our plugin
# from disk before we delete it, so we kill it once more after deletion.
sleep 0.5

echo "==> Removing driver from disk..."
if [ -e "$DRIVER" ]; then
    sudo rm -rf "$DRIVER"
    echo "    Removed $DRIVER"
else
    echo "    $DRIVER not found (already removed?)"
fi

echo "==> Force-killing once more so the final respawn loads without the plugin..."
sudo killall -9 coreaudiod 2>/dev/null || true

echo "    Done. coreaudiod will respawn without GLAInjector. Audio restored in ~2s."
