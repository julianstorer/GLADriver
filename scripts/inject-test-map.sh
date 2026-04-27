#!/bin/bash
# Push a test channel map to the driver for dev/testing (replaces the app IPC server).
# Usage: ./run inject-test-map
exec python3 "$(dirname "$0")/inject-test-map.py"
