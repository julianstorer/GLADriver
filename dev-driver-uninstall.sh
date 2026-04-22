#!/bin/bash
# Remove the GLA HAL driver and restart coreaudiod.
# Does not touch the app or any other installation.
set -e

HAL_DIR="/Library/Audio/Plug-Ins/HAL"
DRIVER_DST="$HAL_DIR/GLAInjector.driver"

echo "==> Removing driver..."
sudo rm -rf "$DRIVER_DST"
echo "    Removed $DRIVER_DST"
sudo killall coreaudiod
echo "    coreaudiod restarted."
echo "Done."
