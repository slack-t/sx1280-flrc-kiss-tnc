# Reticulum Integration Guide

This document describes how to integrate the general-purpose SX1280 FLRC KISS TNC with the **Reticulum Network Stack** (RNS).

## Integration Overview

Use `pi-daemon/ReticulumFLRCInterface.py` for Reticulum integration. It is the supported RNS interface for this firmware and talks directly to the TNC USB CDC port.

Reticulum's stock `KISSInterface` transmits and receives raw KISS frames. It cannot talk directly to the TNC in reliable `GENERIC_FRAGMENTED` mode because the firmware expects the 8-byte serial-integrity envelope on each KISS data frame. `ReticulumFLRCInterface.py` handles that envelope, sets appropriate link pacing, and keeps the physical serial port under one reconnecting interface instead of layering a PTY bridge under stock KISS.

```
┌───────────┐                 ┌───────────────────────┐                  ┌─────────┐
│           │  RNS Interface  │ ReticulumFLRCInterface │  Wrapped KISS    │ FLRC    │
│ Reticulum ├────────────────►│  serial integrity      ├─────────────────►│ TNC     │
│           │                 │  pacing + reconnect    │                  │ Board   │
│           │◄────────────────┤                       │◄─────────────────┤         │
└───────────┘                 └───────────────────────┘                  └─────────┘
```

## Installing the Interface

Install `pyserial` if it is not already installed:

```bash
pip install pyserial
```

Copy or symlink `pi-daemon/ReticulumFLRCInterface.py` into Reticulum's interface module search path for the node. Keep the file in sync with the firmware revision because it owns the serial-integrity envelope and pacing behavior.

## Reticulum Configuration

Add the following interface section to your Reticulum configuration file (typically located at `~/.reticulum/config`):

```ini
[[FLRC TNC]]
type = ReticulumFLRCInterface
interface_enabled = true
port = /dev/ttyACM0
speed = 921600
mode = ptp
name = FLRC TNC
bitrate = 150000
```

## Parameter & Tuning Decisions

### 1. Serial Parameters
* **Baud Rate**: The ESP32-S3 USB CDC device ignores baud rate at the USB layer, but use `speed = 921600` so host-side tooling and logs reflect the intended physical TNC link.
* **Flow Control**: Do not enable host-side RTS/CTS or DSR/DTR. `ReticulumFLRCInterface.py` opens the CDC device with hardware flow control disabled and keeps DTR/RTS deasserted.
* **Bitrate**: `bitrate = 150000` is a conservative effective FLRC goodput estimate used by RNS for pacing and airtime budgeting. Do not set this to the USB serial baud rate.

### 2. RNS Interface Mode (`mode = ptp` vs `mode = full`)
In Reticulum, the `mode` parameter specifies the **RNS routing and topology mode** for the interface (which controls how path announces and route discoveries are propagated), **not** the physical serial/radio duplexing.

The relevant modes are:
*   **`mode = full`**: Standard mesh interface mode. The node actively participates in path selection and propagates announces bidirectionally.
*   **`mode = ptp`** (or `pointtopoint`): Optimized for dedicated point-to-point links (like this FLRC backhaul or virtual tunnels). It prevents redundant announce rebroadcasting back to the sending node, reducing wireless link overhead. Note: RNS only recognizes the spellings `ptp` and `pointtopoint` — the hyphenated form `point-to-point` is silently ignored and falls back to the default mode.

**Recommendation**: Since this SX1280 FLRC TNC setup is designed as a dedicated point-to-point wireless backhaul between exactly two units, setting **`mode = ptp`** (or `mode = pointtopoint`) is the most appropriate and optimized configuration. The default `mode = full` is also functional but generates more announce traffic.

### 3. Duplexing & Carrier Sensing (CSMA/LBT)
Reticulum's `KISSInterface` can perform host-side CSMA collision avoidance (using timing parameters like `preamble`, `txtail`, `persistence`, and `slottime`). However, because the TNC firmware has built-in hardware-level **Listen-Before-Talk (LBT)** CSMA, it is much more efficient to delegate channel access control entirely to the TNC's dedicated MAC task.

To ensure the TNC manages channel access:
1. Do not configure host-side CSMA parameters (like `persistence` or `slottime`) in the Reticulum config, which allows Reticulum to push frames to the TNC immediately.
2. Enable LBT in the TNC firmware by setting the RSSI threshold to a active value (e.g., `-90` dBm). By default, `RADIO_LBT_RSSI_THRESHOLD_DBM` is `0` (disabled). You can enable it by:
   * Compiling the firmware with `#define RADIO_LBT_RSSI_THRESHOLD_DBM -90` in `src/config.h`.
   * Sending the command `SET lbt=-90` via the TNC's control port.

### 4. Small-Datagram RTT Troubleshooting
Small Reticulum handshakes and announces are especially sensitive to ARQ ACK latency. If 1-2 fragment echo tests deliver reliably but sit at a flat ~215 ms RTT, the sender is probably missing the receiver's fast ACK and waiting for `RADIO_ACK_TIMEOUT_MS` on each direction.

The current firmware defers round-complete ACK transmission until the radio has had a short TX-to-RX turnaround window. On the tested LilyGo T3-S3/SX1280 boards, `RADIO_ACK_TURNAROUND_DELAY_MS = 10` produced stable 64-byte echo RTTs:

```text
Delivered 12/12 (100.0%)
RTT min/mean/median/p95/max: 26.7 / 27.4 / 27.2 / 28.0 / 28.5 ms
```

For troubleshooting, run the hardware echo test with one board in echo mode and the other sending spaced pings:

```bash
python3 pi-daemon/kiss_bench.py --port /dev/ttyACM0 --boot-wait 3 --echo
python3 pi-daemon/kiss_bench.py --port /dev/ttyACM1 --boot-wait 1 --size 64 --count 12 --gap-ms 1000 --timeout 2 --verbose
```

If occasional RTTs still land near 215 ms while most samples are ~20-30 ms, increase `RADIO_ACK_TURNAROUND_DELAY_MS` slightly and retest. If all samples remain near 215 ms, verify both boards are flashed with the same current firmware and that the ARQ fast-ACK path is enabled.
