# GLA Injector

Routes audio from USB audio bridges to CoreAudio virtual outputs on macOS. AVB (AVDECC) network discovery annotates each USB input channel with the name of the talker feeding it.

**Platform:** macOS arm64, deployment target 15.0, C++17.

---

## How it works

```
[Companion App]  ──IPC server──►  [HAL Driver]  ──►  CoreAudio
      │                                 ▲
      │  USB capture (CoreAudio)        │ ChannelMapUpdate + AudioData
      └─────────────────────────────────┘
```

1. The **companion app** captures a USB bridge device via CoreAudio, remaps channels according to the patchbay, and sends the audio to the driver over a Unix socket.
2. The **HAL driver** (`GLAInjector.driver`) presents a virtual CoreAudio device and feeds each output slot directly from the IPC stream — no remapping in the driver.
3. **AVDECC** discovery (L-Acoustics avdecc library) identifies which AVB talker is feeding each USB input, so the patchbay shows source names.

---

## Setup

### Prerequisites

- Xcode Command Line Tools
- CMake ≥ 3.29
- A code signing identity named `"GLA Audio Dev"` in your keychain (used to sign the driver)

### Build and install

```bash
# First-time configure + full build
cmake -S . -B build
cmake --build build --parallel $(sysctl -n hw.ncpu)

# Sign, install, and restart coreaudiod (requires sudo)
./run install-driver

# Launch the companion app
./run run-app
```

---

## Scripts

All scripts run via `./run <name>` (no args lists all):

| Script | Description |
|--------|-------------|
| `build-driver` | Build the HAL driver |
| `build-app` | Build the companion app |
| `build-monitor` | Build the terminal level monitor |
| `install-driver` | Build, sign, install driver, restart coreaudiod |
| `uninstall-driver` | Remove driver and restart coreaudiod |
| `recover-driver` | Emergency recovery — removes driver, restores clean coreaudiod state |
| `run-app` | Launch app with live driver syslog streamed to terminal |
| `run-monitor` | Terminal per-channel peak meter for the virtual device |

---

## Components

### `driver/`
CoreAudio HAL plugin loaded inside `coreaudiod`. Built on [libASPL](https://github.com/nicktindall/cycript). `GLAUnifiedDevice` exposes a single virtual device; channel count changes at runtime via `RequestConfigurationChange` (never remove+add — coreaudiod hangs on stale IDs). Each output slot has a `GLAResamplingFIFO`.

### `app/`
JUCE GUI companion app. `AppBackend` owns the IPC server, AVDECC controller, USB capture, and routing table. `GLAChannelMatrix` (in `common/`) holds a `(src, dst)` route list; all channel remapping happens here before audio is sent to the driver.

### `monitor_app/`
Standalone terminal app that attaches to the virtual CoreAudio device and displays a per-channel peak meter. Useful for verifying the signal path without opening a DAW.

### `common/`
Shared headers: IPC wire format (`GLA_IPCTypes.h`), channel matrix (`GLA_ChannelMatrix.h`).

---

## Logging

- Driver logs: `syslog` with prefix `GLA:` (streamed live by `./run run-app`)
- App diagnostic output: `DBG(...)` (JUCE macro → stderr/debugger console)

---

## Recovery

If the driver causes `coreaudiod` to hang:

```bash
sudo ./scripts/recover-driver.sh
```
