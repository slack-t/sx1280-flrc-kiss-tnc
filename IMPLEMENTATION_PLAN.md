# SX1280 FLRC KISS TNC — Implementation Plan

## Overview

Firmware for the **Lilygo T3S3 SX1280** (ESP32-S3 + SX1280 2.4 GHz radio) that implements a
KISS-compatible TNC modem using SX1280 FLRC mode. The device connects via USB CDC to a
Raspberry Pi and exposes a standard KISS serial interface. The Pi runs a `kiss-tun` daemon
that bridges KISS frames into a Linux `tun0` interface, enabling full TCP/IP over the radio link.
Two units linked by Yagi antennas on the 2.4 GHz ISM band form a point-to-point wireless backhaul.

---

## Hardware Reference — Lilygo T3S3 SX1280

| Signal        | ESP32-S3 GPIO |
|---------------|---------------|
| SX1280 SCK    | 5             |
| SX1280 MISO   | 3             |
| SX1280 MOSI   | 6             |
| SX1280 NSS    | 7             |
| SX1280 RESET  | 8             |
| SX1280 BUSY   | 36            |
| SX1280 DIO1   | 9             |
| Display CLK   | 17            |
| Display MOSI  | 18            |
| Display CS    | 13            |
| Display DC    | 12            |
| Display RST   | 10            |
| Battery ADC   | 1             |

Display: ST7789, 170×320, SPI.

> Verify these pins against your specific board revision before flashing.

---

## ISM Band Compliance Note (ETSI EN 300 328)

- Operating frequency: **2440 MHz** (avoids overlap with 2.4 GHz WiFi channel edges)
- Max EIRP: **20 dBm (100 mW)**
- SX1280 max conducted TX: +12.5 dBm
- With a 15 dBi Yagi: 12.5 + 15 = 27.5 dBm EIRP — **exceeds limit**
- Required conducted power with 15 dBi antenna: **≤ 5 dBm** (account for cable loss)
- Set `TX_POWER_DBM` in `config.h` accordingly

---

## FLRC Parameter Targets

| Parameter      | Value                        | Rationale                              |
|----------------|------------------------------|----------------------------------------|
| Frequency      | 2440 MHz                     | ISM centre, clear of WiFi ch1 & ch13  |
| Bit rate       | 650 Kbps / 0.6 MHz BW        | Good sensitivity vs. throughput trade |
| Coding rate    | 3/4                          | Forward error correction headroom     |
| BT             | 1.0                          | Clean spectral shape                  |
| Preamble       | 32 bits                      | Reliable sync                         |
| Sync word      | 4-byte, e.g. `0xC3C3C3C3`   | Avoid collisions with other SX1280    |
| TX power       | 5 dBm conducted (with 15dBi) | EIRP ≤ 20 dBm                         |

Tune up to 1.04 Mbps after link verification.

---

## Architecture

```
┌──────────────────────────────────────────────────────┐
│                  ESP32-S3 (FreeRTOS)                 │
│                                                      │
│  USB CDC ──► SerialRX task ──► txQueue               │
│                                    │                 │
│                               RadioTX task ──► SX1280│
│                                                   ▲  │
│  USB CDC ◄── SerialTX task ◄── rxQueue        DIO1   │
│                                    │                 │
│                               RadioRX task ◄── SX1280│
│                                                      │
│  DisplayUpdate task ◄── Stats struct (shared)        │
└──────────────────────────────────────────────────────┘
         │ USB CDC (/dev/ttyACM0)
         ▼
┌────────────────────────────┐
│   Raspberry Pi             │
│   kiss-tun daemon          │
│   KISS ◄──► tun0 (IP)      │
│   Linux routing / firewall │
└────────────────────────────┘
```

### FreeRTOS Tasks

| Task           | Priority | Stack  | Responsibility                              |
|----------------|----------|--------|---------------------------------------------|
| `radioRxTask`  | High (4) | 4096   | Waits on DIO1 semaphore, reads SX1280 FIFO, pushes to `rxQueue` |
| `radioTxTask`  | High (4) | 4096   | Waits on `txQueue`, transmits packet, enforces CSMA wait |
| `serialRxTask` | Normal(3)| 4096   | Reads KISS bytes from USB CDC, decodes frame, pushes to `txQueue` |
| `serialTxTask` | Normal(3)| 4096   | Takes from `rxQueue`, KISS-encodes, writes to USB CDC |
| `displayTask`  | Low  (1) | 8192   | Refreshes display from `Stats` every 500 ms |

### Queues

| Queue      | Type         | Depth | Direction         |
|------------|--------------|-------|-------------------|
| `txQueue`  | `Packet`     | 8     | Serial→Radio      |
| `rxQueue`  | `Packet`     | 8     | Radio→Serial      |

### Shared State

`Stats` struct (protected by mutex):
- `rssi`, `snr` (last received packet)
- `txCount`, `rxCount`, `errorCount`
- `txBytes`, `rxBytes`
- `radioState` (IDLE / TX / RX)
- `frequency`, `bitrate`

---

## Project Structure

```
sx1280-flrc-kiss-tnc/
├── firmware/
│   ├── platformio.ini              # ESP32-S3, Arduino framework, deps
│   ├── src/
│   │   ├── main.cpp                # setup(), loop(), task launch
│   │   ├── config.h                # all pin defs, FLRC params, tunables
│   │   ├── radio/
│   │   │   ├── Radio.h
│   │   │   └── Radio.cpp           # RadioLib SX1280 init, tx(), rx(), ISR
│   │   ├── kiss/
│   │   │   ├── Kiss.h
│   │   │   └── Kiss.cpp            # encode/decode, FEND/FESC escaping
│   │   ├── display/
│   │   │   ├── Display.h
│   │   │   └── Display.cpp         # LovyanGFX init, drawStatus()
│   │   └── stats/
│   │       ├── Stats.h
│   │       └── Stats.cpp           # shared stats struct + mutex helpers
│   └── test/
│       ├── test_kiss/
│       │   └── test_kiss.cpp       # Unity tests for KISS encode/decode
│       └── test_packet/
│           └── test_packet.cpp     # Packet struct edge cases
├── pi-daemon/
│   ├── kiss_tun.py                 # KISS ↔ tun0 bridge
│   ├── requirements.txt            # pyserial, python-pytun
│   └── systemd/
│       └── kiss-tun.service        # systemd unit for auto-start
└── docs/
    ├── hardware-pinout.md
    ├── flrc-tuning.md
    └── pi-network-setup.md
```

---

## Implementation Phases & TODOs

### Phase 1 — Project Scaffold ✅

- [x] Create `firmware/platformio.ini` — ESP32-S3, Arduino framework, RadioLib 6.6, LovyanGFX 1.1, native test env
- [x] Add dependencies: `RadioLib`, `LovyanGFX`
- [x] Enable native USB CDC (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`)
- [x] Set `board_build.partitions` to `default_8MB.csv` (T3S3 has 8 MB flash)
- [x] Create `config.h` with all pin definitions and FLRC defaults
- [x] Create `Packet` struct: `uint8_t data[127]`, `uint8_t len`, `int8_t rssi`, `float snr` (max 127 bytes — SX1280 FLRC limit)
- [ ] **[MANUAL]** Verify USB CDC enumerates on Pi: `lsusb`, `ls /dev/ttyACM*`

> Note: `Packet` max len corrected to 127 (SX1280 FLRC hardware limit) from the originally planned 255.

### Phase 2 — Radio Layer

- [x] Implement `Radio::begin()`: SX1280 SPI init via RadioLib, set FLRC mode, frequency, power, sync word
- [x] Implement `Radio::startReceive()`: put radio into continuous RX, attach DIO1 ISR
- [x] Implement `Radio::transmit(Packet&)`: switch to TX, send, switch back to RX
- [x] Implement DIO1 ISR → give semaphore to `radioRxTask`
- [x] Implement `Radio::readPacket(Packet&)`: read payload + RSSI/SNR from RadioLib
- [x] Add `Radio::lastRssi()`, `Radio::lastSnr()`
- [ ] **[MANUAL]** Verify TX/RX loopback between two T3S3 units on bench

### Phase 3 — KISS Layer

- [x] Define KISS constants: `FEND=0xC0`, `FESC=0xDB`, `TFEND=0xDC`, `TFESC=0xDD`
- [x] Implement `Kiss::encode(const Packet&, uint8_t* out, size_t outBufLen)`: wrap with FEND, escape payload
- [x] Implement `Kiss::decode(uint8_t byte, Packet& out)`: stateful byte-by-byte decoder
- [x] Handle port byte: accept port 0 (data), silently drop others
- [x] Handle oversized frames: discard without buffer overflow
- [x] Unit test: encode then decode round-trip for all 256 byte values (T1.1)
- [x] Unit test: partial frame split across multiple calls (T1.4)
- [x] Unit test: frames containing `FEND` and `FESC` bytes (T1.2, T1.3)

### Phase 4 — Display Layer

- [x] Implement `Display::begin()`: LovyanGFX init for ST7789 170×320
- [x] Status screen layout: freq, bit rate, RSSI, SNR, TX/RX/error counters, radio state indicator
- [x] Implement `Display::update(const Stats&)`: diff-based redraw (only changed fields)
- [x] Add TX/RX flash indicator (title bar colour flash for 100 ms on packet event)

### Phase 5 — FreeRTOS Integration ✅

- [x] Create `txQueue` and `rxQueue` in `main.cpp`
- [x] Implement `radioRxTask`: pend on DIO1 semaphore → `Radio::readPacket()` → push to `rxQueue` → update Stats
- [x] Implement `radioTxTask`: pop from `txQueue` → CSMA random backoff → `Radio::transmit()` → update Stats
- [x] Implement `serialRxTask`: read USB CDC bytes → feed to `Kiss::decode()` → push complete frames to `txQueue`
- [x] Implement `serialTxTask`: pop from `rxQueue` → `Kiss::encode()` → write to USB CDC
- [x] Implement `displayTask`: sleep 500 ms → lock Stats mutex → `Display::update()` → unlock
- [x] Pin `radioRxTask` and `radioTxTask` to core 1; serial/display tasks on core 0

### Phase 6 — Raspberry Pi Daemon (`kiss_tun.py`) ✅

- [x] Open `/dev/ttyACM0` with `pyserial`
- [x] Create `tun0` interface using `pytun`
- [x] Read loop (thread): KISS decode from serial → write raw IP to `tun0`
- [x] Write loop (thread): read IP packet from `tun0` → KISS encode → write to serial
- [x] Handle `FEND` framing edge cases: skip empty frames, overflow protection
- [x] `--mtu` flag (default 127, matching firmware `PACKET_MAX_LEN`)
- [x] `--port`, `--addr`, `--name` CLI flags
- [x] `systemd/kiss-tun.service` for auto-start on Pi boot
- [ ] **[MANUAL]** Configure `tun0` IP addresses and routes on both Pi units:
  ```bash
  # Pi A — handled automatically by kiss_tun.py --addr 10.0.0.1/30
  # Pi B — kiss_tun.py --addr 10.0.0.2/30
  # Add static routes if forwarding beyond the /30 subnet
  ```

---

## Test Plan

### T1 — Unit Tests (firmware, runs on host via PlatformIO native env)

| ID   | Test                                     | Pass Criteria                                 |
|------|------------------------------------------|-----------------------------------------------|
| T1.1 | KISS encode round-trip                   | Decode(Encode(pkt)) == pkt for 0x00–0xFF      |
| T1.2 | KISS escape: FEND in payload             | 0xC0 → 0xDB 0xDC in encoded output           |
| T1.3 | KISS escape: FESC in payload             | 0xDB → 0xDB 0xDD in encoded output           |
| T1.4 | KISS decode: split delivery              | Complete=false mid-frame, true on FEND        |
| T1.5 | KISS oversized frame                     | `errorCount` increments, no buffer overflow   |
| T1.6 | KISS port filtering                      | Port≠0 frames silently dropped               |

### T2 — Hardware Loopback (single-unit, SPI loopback)

| ID   | Test                                     | Pass Criteria                                 |
|------|------------------------------------------|-----------------------------------------------|
| T2.1 | SX1280 SPI comms                         | RadioLib `begin()` returns `RADIOLIB_ERR_NONE`|
| T2.2 | FLRC TX without error                    | `transmit()` returns no error                 |
| T2.3 | USB CDC enumerates on Pi                 | `/dev/ttyACM0` appears, readable at 921600    |

### T3 — Two-Unit RF Bench Test (units 0.5 m apart, attenuated or low power)

| ID   | Test                                     | Pass Criteria                                 |
|------|------------------------------------------|-----------------------------------------------|
| T3.1 | Packet RX at bench distance              | RX RSSI > −70 dBm, SNR > +5 dB               |
| T3.2 | Bidirectional packet exchange            | 1000 packets each way, PER < 1%              |
| T3.3 | KISS end-to-end (Pi → Pi)               | `ping 10.0.0.2` responds from Pi A           |
| T3.4 | TCP throughput                           | `iperf3` ≥ 300 Kbps sustained                |
| T3.5 | Display accuracy                         | RSSI on display matches RadioLib value ±2 dBm |

### T4 — Field Test (Yagi antennas, target link distance)

| ID   | Test                                     | Pass Criteria                                 |
|------|------------------------------------------|-----------------------------------------------|
| T4.1 | Link establishment at distance           | RSSI > −85 dBm, SNR > 0 dB                   |
| T4.2 | PER under load                           | PER < 2% at full bit rate                    |
| T4.3 | TCP sustained throughput                 | iperf3 ≥ 200 Kbps over 5 minutes             |
| T4.4 | EIRP verification                        | Measured EIRP ≤ 20 dBm (ETSI EN 300 328)    |
| T4.5 | Reboot recovery                          | Link re-establishes within 10 s of power cycle|

---

## Key Dependencies

| Library     | Version  | Purpose                              |
|-------------|----------|--------------------------------------|
| RadioLib    | ≥ 6.4.0  | SX1280 FLRC driver                   |
| LovyanGFX   | ≥ 1.1.0  | Display driver (ST7789)              |
| pyserial    | ≥ 3.5    | Pi daemon serial port                |
| python-pytun| ≥ 2.3    | Pi daemon TUN interface              |

---

## Open Questions Before Implementation

1. **T3S3 board revision** — confirm display pin mapping matches hardware in hand
2. **Max packet size** — SX1280 FLRC max payload is 127 bytes; MTU on tun0 must match
3. **CSMA strategy** — pure random backoff vs. listen-before-talk with carrier detect (SX1280 supports RSSI scan before TX)
4. **Config persistence** — store FLRC params in NVS (ESP32 non-volatile storage) to survive reboot, or hardcode in `config.h`
5. **USB baud rate** — USB CDC ignores baud on ESP32-S3 native USB; the Pi daemon should open at any rate; confirm `kissattach` compatibility if AX.25 path is later needed
