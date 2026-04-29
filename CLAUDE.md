# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build commands

All scripts are dispatched via `./run` (run with no args to list available targets).

```bash
# First-time CMake configure + full build
cmake -S . -B build
cmake --build build --parallel $(sysctl -n hw.ncpu)

# Build individual targets
./run build-driver [--clean]
./run build-app [--clean] [build-dir] [Release|Debug]
./run build-monitor [--clean]

# Build, sign, and install the driver (requires sudo)
./run install-driver

# Launch the companion app with live syslog streaming
./run run-app
```

## Tests

```bash
# Build and run all tests
cd build && ctest --output-on-failure

# Run a single test binary directly (after building)
./build/tests/test_ipc_protocol
./build/tests/test_ring_buffer
```

## Architecture

This system routes audio from USB audio bridges to CoreAudio virtual outputs on macOS, with AVDECC (AVB networking) used to discover which network-connected talker is feeding each USB input channel.

**Three processes communicate over a Unix socket (`/var/tmp/`):**

```
[Companion App]  ──IPC server──►  [HAL Driver]  ──►  CoreAudio
      │                                 ▲
      │  USB capture (CoreAudio)        │ ChannelMapUpdate + AudioData
      └─────────────────────────────────┘
```

### Driver (`driver/`)
- A macOS CoreAudio HAL plugin loaded inside `coreaudiod`, installed at `/Library/Audio/Plug-Ins/HAL/GLAInjector.driver`
- Built on **libASPL** (`third_party/libASPL`); the main class `GLADriver` derives from `aspl::Driver`
- Acts as IPC **client**: receives `ChannelMapUpdate` (channel names + count) and `AudioData` (pre-mapped interleaved float32 PCM) from the app
- `GLAUnifiedDevice` creates a single virtual CoreAudio device whose channel count can change at runtime via `RequestConfigurationChange`
- Each output channel has a `GLAResamplingFIFO`; the IPC thread writes into them, the HAL IO thread reads out
- **The driver does zero channel remapping** — channel N in the incoming audio packet goes directly to output slot N

### Companion App (`app/`)
- JUCE GUI app running in user space; owns the IPC **server**
- `AppBackend` holds: the IPC server, AVDECC controller, USB capture, and the routing table
- `GLAUSBCapture` captures a USB bridge device via CoreAudio as raw interleaved float32
- `AppBackend` applies `GLAChannelMatrix` to remap the raw USB channels into driver-slot order before sending `AudioData` — **all channel mapping happens here, not in the driver**
- `GLAChannelMatrix` (in `common/`) is a simple `(src, dst)` route list; `rebuildMatrix()` is called whenever routing changes
- The patchbay UI (`GLA_MainComponent.h`) calls `initializeSlots()` / `setSlot()` / `clearSlot()` on the backend, which atomically updates the routing table, rebuilds the matrix, and broadcasts a `ChannelMapUpdate` to the driver

### IPC protocol (`common/GLA_IPCTypes.h`)
Wire format: `[type: u32 LE][payload]`

Key message types:
- `ChannelMapUpdate` (1): app → driver — array of `GLAChannelEntry {entityId, displayName[64]}` (one per output slot)
- `AudioData` (30): app → driver — `[channelCount: u32][frameCount: u32][sourceRate: f64][samples: float32[]]` — channels already in slot order

### AVDECC (`GLA_AVDECCController.h`)
- Uses L-Acoustics avdecc library (`third_party/avdecc`) to discover AVB talkers on a chosen network interface
- `getListenerConnections(bridgeEntityId)` returns which talker streams feed each input of the USB bridge, allowing the app to annotate USB channels with source names

## Key constraints

- **macOS arm64 only**, deployment target 15.0, C++17
- Driver install requires ad-hoc code signing (identity `"GLA Audio Dev"`) and kills/restarts `coreaudiod`
- `install-driver.sh` has a 15-second verification timeout and auto-recovery if the driver fails to initialize
- Both driver and app log via syslog with `"GLA:"` prefix; `./run run-app` streams these live
- On Debug builds, set `AVDECC_NO_WATCHDOG_ASSERT=1` to suppress a benign lock-order assertion during ADP startup (already set in `run-app.sh`)
- Never do `RemoveDevice` + `AddDevice` when channel count changes — use `RequestConfigurationChange` + `updateChannelMap` in-place to avoid coreaudiod hanging on stale `AudioDeviceID`s
