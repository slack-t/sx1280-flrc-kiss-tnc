# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Summary

Firmware for the **Lilygo T3S3 SX1280** (ESP32-S3 + SX1280 2.4 GHz radio) implementing a KISS TNC
modem over FLRC. The T3S3 connects via USB CDC to a Raspberry Pi running a `kiss-tun` daemon that
bridges KISS frames to a Linux `tun0` interface for TCP/IP. Two units linked by Yagi antennas form
a point-to-point ISM-band backhaul. See `IMPLEMENTATION_PLAN.md` for the full design.

## Build & Flash (firmware)

```bash
cd firmware
pio run                        # build
pio run -t upload              # flash via USB
pio device monitor             # serial monitor (921600 baud)
```

## Tests

```bash
# Unit tests (host, no hardware needed)
pio test -e native

# On-device integration tests
pio test -e esp32s3
```

## Key Architecture

**FreeRTOS task layout:**
- `radioRxTask` / `radioTxTask` — pinned to core 1, own all SX1280 SPI access
- `serialRxTask` / `serialTxTask` — core 0, own USB CDC read/write
- `displayTask` — core 0, reads shared `Stats` struct every 500 ms

**Data flow:** USB CDC → `serialRxTask` → KISS decode → `txQueue` → `radioTxTask` → SX1280.
Reverse: DIO1 ISR → semaphore → `radioRxTask` → `rxQueue` → `serialTxTask` → KISS encode → USB CDC.

**KISS framing** lives entirely in `src/kiss/`; it has no hardware dependency and is fully unit-testable on host.

**Stats** (`src/stats/Stats.h`) is the only shared state between tasks. Always take the mutex before reading or writing.

## Configuration

All tunables are in `src/config.h`:
- Pin definitions (SPI, display, DIO1, BUSY, RST)
- FLRC parameters (frequency, bit rate, coding rate, sync word)
- `TX_POWER_DBM` — **keep EIRP ≤ 20 dBm (ETSI EN 300 328)**; with a 15 dBi Yagi set ≤ 5 dBm conducted

## Pi Daemon

```bash
cd pi-daemon
pip install -r requirements.txt
sudo python kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.1/30
```

The daemon creates `tun0`, sets MTU 127 (SX1280 FLRC max payload), and bridges KISS↔IP.
Install `systemd/kiss-tun.service` for auto-start.

## Hardware Pins (T3S3 SX1280 — verify against board revision)

SX1280: SCK=5, MISO=3, MOSI=6, NSS=7, RST=8, BUSY=36, DIO1=9  
Display (ST7789 170×320): CLK=17, MOSI=19, CS=13, DC=12, RST=10  
Battery ADC: GPIO1
