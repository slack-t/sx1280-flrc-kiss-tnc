# SX1280 FLRC KISS TNC

High-performance, point-to-point wireless backhaul firmware and bridge daemon for the **Lilygo T3S3 SX1280** (ESP32-S3 + SX1280 2.4 GHz transceiver). 

This project implements a KISS-compatible TNC modem operating in high-speed Fast Long Range Communication (FLRC) mode on the 2.4 GHz ISM band. The device connects via USB CDC to a host system (such as a Raspberry Pi), exposing a standard KISS serial interface. The companion Python bridge daemon (`kiss_tun.py`) bridges these KISS frames into a Linux `tun0` virtual network interface, enabling full bidirectional **TCP/IP over the 2.4 GHz radio link**.

---

## System Architecture

```
┌────────────────────────────────────────────────────────┐
│                  ESP32-S3 (FreeRTOS)                   │
│                                                        │
│  USB CDC ──► serialRxTask ──► txQueue                  │
│                                   │                    │
│                               radioTxTask ──► SX1280   │
│                                                   ▲    │
│  USB CDC ◄── serialTxTask ◄── rxQueue         DIO1     │
│                                   │                    │
│                               radioRxTask ◄── SX1280   │
│                                                        │
│  displayTask ◄── Stats Manager (Shared Mutex)          │
└────────────────────────────────────────────────────────┘
         │ USB CDC (/dev/ttyACM0)
         ▼
┌────────────────────────────────────────────────────────┐
│              Raspberry Pi (Host Daemon)                │
│                                                        │
│  pyserial ◄──► kiss_tun.py ◄──► pytun ◄──► Linux tun0  │
└────────────────────────────────────────────────────────┘
```

### Key Technical Features

*   **FreeRTOS Multicore Separation**:
    *   **Core 1**: Dedicated to timing-critical radio transactions (`radioRxTask` and `radioTxTask`) to prevent transmission jitter.
    *   **Core 0**: Handles serial USB CDC I/O (`serialRxTask` and `serialTxTask`) and the dynamic screen updater (`displayTask`).
*   **SPI Thread-Safety**: Radio registers and SPI bus are fully protected using a FreeRTOS mutex (`_spiMutex`) with non-locking internal states to eliminate deadlocks.
*   **Listen-Before-Talk (LBT) CSMA**: Transmissions scan the channel first using RadioLib's `scanChannel()` to check for existing carrier/preamble signals, backing off randomly if busy to avoid collisions.
*   **Spurious Interrupt Gate**: The dual RX/TX DIO1 interrupt pulse is gated via software checks (`_txActive` flag), ensuring only genuine packet reception triggers the RX queue.
*   **Stateful KISS Decoder**: Full compliance with single-FEND and double-FEND framing standards for maximum compatibility with standard TNC hosts.
*   **Robust Pi Bridge**: The host-side bridge features automatic serial reconnection loops and a 64KB read buffer to prevent Linux `EMSGSIZE` (message too long) kernel crash exceptions.
*   **Offscreen Buffered Screen Graphics**: Utilizes LovyanGFX for SSD1306 OLED screen updating with 1-bit double-buffering to eliminate visual tearing and flickering.
*   **Selective-Repeat ARQ**: Fragmented FLRC frames use bitmap ACKs and selective retransmission. The current wire format uses a 16-bit frame sequence number and a 4-byte fragment header to eliminate replay ambiguity from the old 4-bit sequence space. ACK packets are padded to full radio length for FLRC reliability, and a `CompletedFrameCache` suppresses duplicate frame delivery while re-ACKing retransmits within a 1.5 s window. Duplicate fragment arrivals are idempotent — only genuinely new fragment data is written to the reassembly buffer, preventing fallback-timer drift and spurious ACK floods.

---

## Hardware Reference — Lilygo T3S3 SX1280

The firmware is pre-configured for the standard **Lilygo T3S3 SX1280 V1.2 / V1.3** revision:

| Signal | ESP32-S3 GPIO Pin |
| :--- | :--- |
| **SX1280 SCK** | GPIO 5 |
| **SX1280 MISO** | GPIO 3 |
| **SX1280 MOSI** | GPIO 6 |
| **SX1280 NSS** | GPIO 7 |
| **SX1280 RESET** | GPIO 8 |
| **SX1280 BUSY** | GPIO 36 |
| **SX1280 DIO1** | GPIO 9 |
| **OLED SDA** | GPIO 18 (shared with I2C bus) |
| **OLED SCL** | GPIO 17 (shared with I2C bus) |
| **Battery ADC** | GPIO 1 |
| **On-Board LED** | GPIO 37 |

> [!NOTE]
> Standard Lilygo T3S3 models come standard with an I2C SSD1306 OLED display. The firmware is configured out-of-the-box to use this display (128x64 pixels, SSD1306 driver, I2C address 0x3C).


---

## RF Regulation & Compliance (ETSI EN 300 328)

Operating in the 2.4 GHz ISM band requires strict adherence to local RF exposure and transmission power guidelines:

*   **Operating Frequency**: Configured to **2440 MHz** by default. This avoids overlapping with typical 2.4 GHz Wi-Fi channel edges (Channel 1 and Channel 13).
*   **Max EIRP**: **20 dBm (100 mW)** under ETSI EN 300 328.
*   **Power Adjustment**: 
    *   With a standard low-gain antenna (e.g., +2 dBi rubber duck), set `TX_POWER_DBM` in `config.h` up to **+12.5 dBm** (the hardware max).
    *   If using high-gain directional antennas (e.g., a **15 dBi Yagi** for backhaul), you **MUST** reduce `TX_POWER_DBM` in `config.h` to **≤ +5 dBm** to account for antenna gain and avoid exceeding the 20 dBm EIRP legal limit.

---

## Firmware Compilation & Installation

The firmware is managed using **PlatformIO**.

### 1. Requirements
*   Install PlatformIO (CLI or via VS Code).
*   Connect the Lilygo T3S3 board via USB.

### 2. File Structure
*   `firmware/src/config.h`: Central hardware configurations, pins, and FLRC transmission presets (bitrate, bandwidth, sync words).
*   `firmware/src/radio/`: RadioLib SX1280 driver initialization and thread-safe control routines.
*   `firmware/src/kiss/`: State-machine encoder and decoder for serial encapsulation.

### 3. Build & Flash
Navigate to the `firmware/` directory:

```bash
# Build the project
pio run

# Flash the firmware to your board
pio run --target upload

# Open the serial monitor (baud rate is ignored by native USB CDC, but kept for compatibility)
pio device monitor
```

### 4. Running Unit Tests
Validate the KISS encoding and frame stream logic locally on your host machine without hardware:
```bash
pio test -e native
```

---

## Raspberry Pi Daemon Setup

The Python bridge script `kiss_tun.py` handles communication between Linux's virtual network interface and the USB CDC device.

### 1. Dependencies
Install the required system library and python modules:
```bash
sudo pip3 install pyserial python-pytun
```

### 2. Usage
Start the bridge as `root` (necessary to create a virtual network interface):

```bash
# On Unit A
sudo python3 pi-daemon/kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.1/30

# On Unit B
sudo python3 pi-daemon/kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.2/30
```

### 3. Arguments Reference
*   `--port`: Serial interface path (defaults to `/dev/ttyACM0`).
*   `--addr`: Virtual IP allocation and subnet mask (e.g. `10.0.0.1/30`).
*   `--name`: Linux interface name (defaults to `tun0`).
*   `--mtu`: Network MTU size. Must be `<= 1280` for WP-B generic ARQ; `kiss_tun.py` defaults to **1280**.

### 4. Testing the TCP/IP Link
Once both bridge daemons are active, verify the point-to-point link:

```bash
# From Host A
ping 10.0.0.2

# Run bandwidth/throughput tests
iperf3 -c 10.0.0.2
```

---

## Licensing
This project is open-source and available under the MIT License.
