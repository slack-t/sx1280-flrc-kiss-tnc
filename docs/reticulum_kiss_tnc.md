# Reticulum Integration Guide

This document describes how to integrate the general-purpose SX1280 FLRC KISS TNC with the **Reticulum Network Stack** (RNS).

## Integration Overview

Because Reticulum's stock `KISSInterface` transmits and receives raw KISS frames, it cannot talk directly to the TNC's hardware serial port in its standard reliable mode (`GENERIC_FRAGMENTED`), which expects an 8-byte serial-integrity envelope on the wire.

To solve this, a host-side Python bridge (`pi-daemon/reticulum_kiss_bridge.py`) creates a virtual pseudo-terminal (`PTY`) interface for Reticulum, intercepts outbound frames, adds the 8-byte integrity header, and sends them to the TNC (and vice-versa for inbound frames).

```
┌───────────┐                 ┌──────────────┐                  ┌─────────┐
│           │  Standard KISS  │  KISS PTY    │  Wrapped KISS    │  FLRC   │
│ Reticulum ├────────────────►│    Bridge    ├─────────────────►│   TNC   │
│           │  (No Envelope)  │   (Python)   │ (With Envelope)  │ (Board) │
│           │◄────────────────┤              │◄─────────────────┤         │
└───────────┘                 └──────────────┘                  └─────────┘
```

## Running the Bridge

Install `pyserial` if it is not already installed:
```bash
pip install pyserial
```

Start the bridge as a background daemon or inside a multiplexer (e.g., `tmux` or `screen`):
```bash
python3 pi-daemon/reticulum_kiss_bridge.py --port /dev/ttyACM0 --baud 921600 --virtual-port /tmp/kiss_reticulum
```

Arguments:
* `--port`: The physical serial path to the TNC (default: `/dev/ttyACM0`).
* `--baud`: The physical baud rate (default: `921600`).
* `--virtual-port`: The symlink path where the virtual serial port is exposed (default: `/tmp/kiss_reticulum`).
* `--mtu`: Maximum payload length (default: `1280`).
* `--debug`: Enable verbose hex log outputs for troubleshooting.

## Reticulum Configuration

Add the following interface section to your Reticulum configuration file (typically located at `~/.reticulum/config`):

```ini
[interface_spec]
type = KISSInterface
interface_enabled = true
port = /tmp/kiss_reticulum
speed = 115200
flow_control = false
mode = point-to-point
name = FLRC TNC via Bridge

```

## Parameter & Tuning Decisions

### 1. Serial Parameters
* **Baud Rate**: The virtual port is a PTY, so Reticulum's `speed` parameter is ignored by the OS kernel (set to `115200` for compatibility). The physical serial connection to the USB CDC device is opened at `921600` baud.
* **Flow Control**: Flow control must be disabled on both the physical serial port and in the Reticulum configuration (`flow_control = false`).

### 2. RNS Interface Mode (`mode = point-to-point` vs `mode = full`)
In Reticulum, the `mode` parameter specifies the **RNS routing and topology mode** for the interface (which controls how path announces and route discoveries are propagated), **not** the physical serial/radio duplexing.

The relevant modes are:
*   **`mode = full`**: Standard mesh interface mode. The node actively participates in path selection and propagates announces bidirectionally.
*   **`mode = point-to-point`** (or `ptp`): Optimized for dedicated point-to-point links (like this FLRC backhaul or virtual tunnels). It prevents redundant announce rebroadcasting back to the sending node, reducing wireless link overhead.

**Recommendation**: Since this SX1280 FLRC TNC setup is designed as a dedicated point-to-point wireless backhaul between exactly two units, setting **`mode = point-to-point`** (or `mode = ptp`) is the most appropriate and optimized configuration. The default `mode = full` is also functional but generates more announce traffic.

### 3. Duplexing & Carrier Sensing (CSMA/LBT)
Reticulum's `KISSInterface` can perform host-side CSMA collision avoidance (using timing parameters like `preamble`, `txtail`, `persistence`, and `slottime`). However, because the TNC firmware has built-in hardware-level **Listen-Before-Talk (LBT)** CSMA, it is much more efficient to delegate channel access control entirely to the TNC's dedicated MAC task.

To ensure the TNC manages channel access:
1. Do not configure host-side CSMA parameters (like `persistence` or `slottime`) in the Reticulum config, which allows Reticulum to push frames to the TNC immediately.
2. Enable LBT in the TNC firmware by setting the RSSI threshold to a active value (e.g., `-90` dBm). By default, `RADIO_LBT_RSSI_THRESHOLD_DBM` is `0` (disabled). You can enable it by:
   * Compiling the firmware with `#define RADIO_LBT_RSSI_THRESHOLD_DBM -90` in `src/config.h`.
   * Sending the command `SET lbt=-90` via the TNC's control port.

