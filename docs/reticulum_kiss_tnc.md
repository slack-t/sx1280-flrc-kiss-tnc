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
mode = full
name = FLRC TNC via Bridge
```

## Parameter & Tuning Decisions

### 1. Serial Parameters
* **Baud Rate**: The virtual port is a PTY, so Reticulum's `speed` parameter is ignored by the OS kernel (set to `115200` for compatibility). The physical serial connection to the USB CDC device is opened at `921600` baud.
* **Flow Control**: Flow control must be disabled on both the physical serial port and in the Reticulum configuration (`flow_control = false`).

### 2. Duplex Mode & Carrier Sensing (`mode = full`)
We use **`mode = full`** in Reticulum because the TNC firmware has built-in hardware-level **Listen-Before-Talk (LBT)** CSMA. Configuring `mode = full` delegates all channel scheduling and backoff logic to the TNC itself.

> [!IMPORTANT]
> The TNC's default LBT threshold (`RADIO_LBT_RSSI_THRESHOLD_DBM` in `src/config.h`) is `0` (disabled). You must configure the TNC's LBT threshold to an active value (e.g., `-90` dBm) either by:
> 1. Compiling the firmware with `#define RADIO_LBT_RSSI_THRESHOLD_DBM -90` in `src/config.h`.
> 2. Sending the command `SET lbt=-90` to the TNC's control port.
