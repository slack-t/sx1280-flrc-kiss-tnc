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


---

# Addendum: Layer-2 Fragmentation & Reassembly (Active)
# Implementation Plan: Debugging & Optimizing Layer-2 Fragmentation (ping -s 400)

We have made remarkable progress: single-packet pings and 2-fragment pings (`ping -s 150`) now work reliably! However, larger 4-fragment pings (`ping -s 400`) still fail to reassemble or receive. 

To solve this, we will execute a two-phased approach:
1. **Instrument Serial Diagnostics**: Add lightweight, clean serial tracking to print exactly what fragments are sent and received.
2. **Optimize SPI Transitions & Timing**:
   * **Skip RSSI/SNR on Intermediate Fragments**: Querying RSSI/SNR performs an expensive 5-byte SPI read stream on the SX1280. Skipping this on intermediate fragments saves ~300-500us of SPI time.
   * **Conditional Preamble Length Updates**: Calling `setPreambleLength` writes to multiple registers to reset the expected payload size. This is only necessary when transitioning from TX to RX (since TX changes the payload length register). Skipping this for RX-to-RX transitions avoids unnecessary SPI commands, shortening the RX turn-around time to <200us.
   * **Tick-Granularity Protection**: Enforce a minimum of 2 FreeRTOS ticks (20ms) of inter-fragment delay on 100Hz hosts to prevent tick-alignment from truncating the sleep duration below the receiver's turn-around time.

---

## User Review Required

> [!IMPORTANT]
> **SPI Turn-Around Optimization (Massive Link Latency / Speedup)**:
> In point-to-point links, the time it takes the receiver to return to RX mode after reading a packet is the primary bottleneck. If a new fragment arrives *before* the receiver is listening, it is permanently lost.
> * We are cutting the number of SPI transactions in the RX-to-RX cycle from **11 down to 4** by skipping RSSI/SNR reads on intermediate fragments, and avoiding `setPreambleLength` register rewrites.
> * This will make the receiver robust to much tighter pacing, maximizing throughput.

> [!WARNING]
> **Serial Diagnostic Print Overhead**:
> High-speed `Serial.printf` logs can block execution if the USB buffer gets full. We will keep the logs extremely concise and only output them during initial ping diagnostic verification. Once pings are 100% stable, we can strip them or disable them for `iperf3` performance runs.

---

## Proposed Changes

### 1. Radio Driver Layer

#### [MODIFY] [Radio.h](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/radio/Radio.h)
* Update `_startReceiveNoLock` to accept an optional `forceReset` boolean parameter.

```diff
-    void _startReceiveNoLock();
+    void _startReceiveNoLock(bool forceReset = false);
```

#### [MODIFY] [Radio.cpp](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/radio/Radio.cpp)
* Optimize `startReceive` and `transmit` to pass `forceReset = true` because they return from IDLE or TX modes where registers must be reset.
* Optimize `readPacket` to check the framing header in the received buffer first. If it is an intermediate fragment (i.e. `is_split && !is_last`), skip `getRSSI()` and `getSNR()` calls.
* Optimize `readPacket` to call `_startReceiveNoLock(false)` to avoid unnecessary SPI packet parameter rewrites when returning RX-to-RX.

```diff
-void Radio::_startReceiveNoLock() {
-    // Reset receiver's packet parameters to maximum size (255 bytes) before listening
-    _radio.setPreambleLength(RADIO_PREAMBLE_BITS);
-    _radio.startReceive();
-}
+void Radio::_startReceiveNoLock(bool forceReset) {
+    if (forceReset) {
+        // Reset receiver's packet parameters to maximum size (255 bytes) before listening
+        _radio.setPreambleLength(RADIO_PREAMBLE_BITS);
+    }
+    _radio.startReceive();
+}
 
 void Radio::startReceive() {
     xSemaphoreTake(_spiMutex, portMAX_DELAY);
-    _startReceiveNoLock();
+    _startReceiveNoLock(true);
     xSemaphoreGive(_spiMutex);
 }
 
 int16_t Radio::transmit(const Packet& pkt) {
     xSemaphoreTake(_spiMutex, portMAX_DELAY);
 
     _txActive = true;
     int16_t state = _radio.transmit(const_cast<uint8_t*>(pkt.data), pkt.len);
     _txActive = false;
 
     // Always return to RX — even on TX failure the node must not stay deaf.
-    _startReceiveNoLock();
+    _startReceiveNoLock(true);
 
     xSemaphoreGive(_spiMutex);
     return state;
 }
 
 int16_t Radio::readPacket(Packet& pkt) {
     xSemaphoreTake(_spiMutex, portMAX_DELAY);
 
     size_t len = _radio.getPacketLength();
     if (len == 0 || len > PACKET_MAX_LEN) {
-        _startReceiveNoLock();
+        _startReceiveNoLock(true);
         xSemaphoreGive(_spiMutex);
         return RADIOLIB_ERR_PACKET_TOO_LONG;
     }
 
     int16_t state = _radio.readData(pkt.data, len);
     pkt.len = (state == RADIOLIB_ERR_NONE) ? static_cast<uint8_t>(len) : 0;
 
-    _lastRssi = static_cast<int8_t>(_radio.getRSSI());
-    _lastSnr  = _radio.getSNR();
+    // Check the framing header to see if this is an intermediate fragment.
+    // If so, skip expensive RSSI/SNR SPI reads to maximize RX-to-RX speed!
+    bool shouldQueryRssi = true;
+    if (state == RADIOLIB_ERR_NONE && pkt.len > 0) {
+        const uint8_t header = pkt.data[0];
+        const bool is_split = (header & FRAMING_FLAG_SPLIT) != 0;
+        const bool is_last  = (header & FRAMING_FLAG_LAST) != 0;
+        if (is_split && !is_last) {
+            shouldQueryRssi = false;
+        }
+    }
+
+    if (shouldQueryRssi) {
+        _lastRssi = static_cast<int8_t>(_radio.getRSSI());
+        _lastSnr  = _radio.getSNR();
+    } else {
+        _lastRssi = 0;
+        _lastSnr  = 0.0f;
+    }
 
     pkt.rssi = _lastRssi;
     pkt.snr  = _lastSnr;
 
-    _startReceiveNoLock();
+    _startReceiveNoLock(false);
     xSemaphoreGive(_spiMutex);
     return state;
 }
```

---

### 2. Core Task Layer

#### [MODIFY] [main.cpp](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/main.cpp)
* **Pacing Delay**: Enforce tick boundary protection to guarantee a minimum 10-20ms gap even with bad FreeRTOS tick alignments.
* **Diagnostics**: Add concise serial logs in `radioRxTask` and `radioTxTask` to track every fragment.
* **RSSI/SNR Tracking**: Update reassembler processing to bypass `rssi_acc` averaging and directly adopt the final fragment's link quality metrics.

```diff
@@ -67,10 +67,12 @@
             if (ra.seq != FRAMING_SEQ_UNSET &&
                 (millis() - ra.last_tick_ms > 500)) {
+                Serial.printf("[radio_rx] Stale partial reassembly discarded (seq=%d, mask=0x%02X)\n", ra.seq, ra.received_mask);
                 ra.reset();
             }
 
             // New sequence number: discard previous partial and start fresh.
             if (ra.seq != seq) {
+                if (ra.seq != FRAMING_SEQ_UNSET) {
+                    Serial.printf("[radio_rx] New seq %d received; abandoning old seq %d\n", seq, ra.seq);
+                }
                 ra.reset();
                 ra.seq = seq;
             }
 
             const uint16_t frag_data_len = pkt.len - 1;
             memcpy(ra.buf + idx * FRAMING_FRAG_DATA, pkt.data + 1, frag_data_len);
             ra.frag_len[idx]   = frag_data_len;
             ra.received_mask  |= static_cast<uint8_t>(1u << idx);
-            ra.rssi_acc       += pkt.rssi;
-            ra.snr_acc        += pkt.snr;
             ra.frag_count++;
             ra.last_tick_ms    = millis();
             if (is_last) ra.total_frags = idx + 1;
 
+            Serial.printf("[radio_rx] Received frag: seq=%d, idx=%d, is_last=%s, len=%d\n", 
+                          seq, idx, is_last ? "yes" : "no", frag_data_len);
+
             if (ra.isComplete()) {
                 IpFrame frame;
                 frame.len = 0;
                 for (uint8_t i = 0; i < ra.total_frags; i++) {
                     memcpy(frame.data + frame.len,
                            ra.buf + i * FRAMING_FRAG_DATA,
                            ra.frag_len[i]);
                     frame.len += ra.frag_len[i];
                 }
-                frame.rssi = static_cast<int8_t>(ra.rssi_acc / ra.frag_count);
-                frame.snr  = ra.snr_acc / ra.frag_count;
+                // Direct adopt of the final fragment's link quality
+                frame.rssi = pkt.rssi;
+                frame.snr  = pkt.snr;
 
                 auto& sm = StatsManager::instance();
                 sm.lock();
                 sm.get().rxCount++;
                 sm.get().rxBytes   += frame.len;
                 sm.get().rssi       = frame.rssi;
                 sm.get().snr        = frame.snr;
                 sm.get().radioState = RadioState::RX;
                 sm.unlock();
 
+                Serial.printf("[radio_rx] Frame fully reassembled! len=%d, seq=%d, RSSI=%d\n", frame.len, ra.seq, frame.rssi);
                 xQueueSend(rxQueue, &frame, 0);
                 ra.reset();
             }
@@ -124,10 +124,14 @@
     for (;;) {
         xQueueReceive(txQueue, &frame, portMAX_DELAY);
 
         seq = (seq + 1) & 0x0F;
         const bool    needs_split = (frame.len > FRAMING_FRAG_DATA);
         uint16_t      offset      = 0;
         uint8_t       idx         = 0;
+        uint8_t       total_frags = needs_split ? ((frame.len + FRAMING_FRAG_DATA - 1) / FRAMING_FRAG_DATA) : 1;
+
+        Serial.printf("[radio_tx] Sending frame: len=%d, seq=%d, frags=%d\n", frame.len, seq, total_frags);
 
         while (offset < frame.len) {
             const uint16_t chunk   = (frame.len - offset < FRAMING_FRAG_DATA)
                                      ? static_cast<uint16_t>(frame.len - offset)
                                      : static_cast<uint16_t>(FRAMING_FRAG_DATA);
             const bool     is_last = (offset + chunk >= frame.len);
 
             uint8_t header = static_cast<uint8_t>(seq << 4);
             if (needs_split) {
                 header |= FRAMING_FLAG_SPLIT;
                 header |= static_cast<uint8_t>(idx << 2);
                 if (is_last) header |= FRAMING_FLAG_LAST;
             }
 
             pkt.data[0] = header;
             memcpy(pkt.data + 1, frame.data + offset, chunk);
             pkt.len = static_cast<uint8_t>(chunk + 1);
 
             // Inter-fragment gap: gives the remote receiver ample time to process
             // the previous fragment, write it, and return to RX mode.
             // Only apply a delay between fragments (when idx > 0).
             if (idx > 0) {
-                vTaskDelay(pdMS_TO_TICKS(15));
+                // Enforce minimum of 2 ticks (20ms) on 100Hz systems to guard against tick truncation.
+                TickType_t delay_ticks = pdMS_TO_TICKS(15);
+                if (delay_ticks <= 1) {
+                    delay_ticks = 2; 
+                }
+                vTaskDelay(delay_ticks);
             }
 
             auto& sm = StatsManager::instance();
             sm.lock();
             sm.get().radioState = RadioState::TX;
             sm.unlock();
 
+            Serial.printf("[radio_tx] Transmitting frag %d/%d (seq=%d, len=%d)\n", idx + 1, total_frags, seq, pkt.len);
             int16_t err = radio.transmit(pkt);
 
             sm.lock();
             if (err == RADIOLIB_ERR_NONE) {
+                Serial.printf("[radio_tx] Transmit OK (frag %d/%d)\n", idx + 1, total_frags);
                 if (is_last) {
                     sm.get().txCount++;
                     sm.get().txBytes += frame.len;
                 }
             } else {
+                Serial.printf("[radio_tx] Transmit FAILED (frag %d/%d, err=%d)\n", idx + 1, total_frags, err);
                 sm.get().errorCount++;
             }
             sm.get().radioState = RadioState::IDLE;
             sm.unlock();
 
             offset += chunk;
             idx++;
         }
     }
 }
```

---

## Verification Plan

### Automated Verification
1. Compile firmware with PlatformIO to verify there are no compilation errors:
   ```bash
   pio run
   ```

### Manual Verification & Log Inspection
1. Build and flash the new diagnostic-enabled firmware to `/dev/ttyACM0`.
2. Stop the local `kiss_tun.py` process.
3. Start PlatformIO's serial device monitor to capture physical transceiver activity:
   ```bash
   pio device monitor
   ```
4. Send a standard ping, a `-s 150` (2 fragments) ping, and a `-s 400` (4 fragments) ping from the *remote* system.
5. Capture and inspect the serial monitor output! We will instantly see:
   * Exactly which fragments (1/4, 2/4, 3/4, 4/4) are transmitted.
   * Exactly which fragments are successfully received by the target.
   * If any specific fragment index (e.g. fragment index 1 or 2) is missing or dropped, allowing us to dial in the pacing delay perfectly.


---

# Addendum: High-Performance Rust KISS-TUN Daemon (Active)

We are implementing a high-performance, ultra-efficient Rust version of the Python `kiss_tun.py` bridge. The Rust version will reside in a new `pi-daemon-rust/` directory.

## Technical Design & Rationale

1. **No GIL (True Parallelism)**: Rust handles bidirectionality concurrently using native OS threads, eliminating Python's Global Interpreter Lock (GIL) and lowering latency.
2. **0% Idle CPU Usage**: By utilizing blocking read timeouts on the serial and TUN descriptors, the Rust daemon consumes exactly 0% CPU when idle (no polling loops or busy-sleep intervals).
3. **Zero Allocations in Critical Path**: Frame parsing operates on a static stack buffer, avoiding memory allocation overhead and garbage collection latency.
4. **Self-Contained Deployment**: Compiles to a single static binary with no external runtimes (no virtualenv or python library conflicts).

## Proposed File Structure

```
pi-daemon-rust/
├── Cargo.toml                      # Build config and dependencies
└── src/
    ├── main.rs                     # CLI entry, device setup, threads orchestration
    └── kiss.rs                     # Rust port of the stateful KISS encoder/decoder
```

## Proposed Changes

### [NEW] [Cargo.toml](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/pi-daemon-rust/Cargo.toml)
Add basic manifest with `clap` (CLI parser), `serialport` (serial port communication), `tun-rs` (cross-platform TUN device), and `anyhow` (error management).

### [NEW] [kiss.rs](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/pi-daemon-rust/src/kiss.rs)
Port of the robust stateful KISS decoder and escaping encoder matching our firmware spec.

### [NEW] [main.rs](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/pi-daemon-rust/src/main.rs)
- Parses args: `--port` (default `/dev/ttyACM0`), `--addr` (e.g. `10.0.0.1/30`), `--mtu` (default `504`), `--name` (default `tun0`), `--baud` (default `921600`).
- Computes IP and netmask from CIDR format.
- Opens the `tun` device, sets MTU and IP, and puts it UP.
- Implements robust reconnection loop: opens serial, clones file descriptors, and spawns parallel threads for bidirectionality.

## Verification Plan

### Automated Tests
- Add unit tests for the KISS encoder/decoder in `kiss.rs` and verify via `cargo test`.
- Compile the binary with `cargo build --release`.

### Manual Verification
- Run the Rust daemon: `sudo target/release/kiss-tun-rs --addr 10.0.0.1/30`
- Perform standard pings and verify low, stable latency and 0% idle CPU.
- Run `iperf3` to compare throughput.
