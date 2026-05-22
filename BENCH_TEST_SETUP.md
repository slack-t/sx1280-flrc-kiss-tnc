# Bench Test & Flashing Guide: SX1280 FLRC KISS TNC

This guide provides step-by-step instructions to flash the firmware and set up a bidirectional point-to-point wireless TCP/IP bench link using two **Lilygo T3S3 SX1280** units.

---

## ⚠️ Critical RF Safety Warnings

> [!CAUTION]
> **NEVER POWER ON THE BOARDS WITHOUT ANTENNAS ATTACHED.**
> Running a 2.4 GHz RF transceiver (especially during active transmissions) without an antenna or a 50-ohm dummy load will reflect RF energy back into the power amplifier (PA). This will permanently degrade or burn out the SX1280 radio chip.

> [!WARNING]
> **PREVENT RECEIVER FRONT-END SATURATION.**
> At close range (e.g., sitting on the same desk, < 2 meters apart), the RF signal will be extremely strong. If the transmit power is too high, it will saturate the receiver's Low Noise Amplifier (LNA), leading to high Packet Error Rates (PER), CRC errors, or potential hardware damage. 
> *   **Action**: Always reduce the TX power to the minimum setting for bench testing, or use inline SMA coaxial attenuators (20 dB or 30 dB).

---

## Prerequisites & Setup Layout

### Hardware Required
*   2× **Lilygo T3S3 SX1280** boards.
*   2× 2.4 GHz antennas (standard dual-band Wi-Fi/Bluetooth whip/stubby antennas are perfect).
*   2× USB-C data cables.
*   2× Linux machines (e.g., Raspberry Pi units or a single Linux PC with two USB ports).

### Software Installed on Hosts
Ensure the Python modules are installed on the host machines:
```bash
sudo pip3 install pyserial python-pytun iperf3
```

---

## Step 1: Configure Firmware for Bench Testing

Before compiling, adjust the TX power in the configuration header to a safe, low level suitable for close-range bench testing.

1.  Open [config.h](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/config.h) in your editor.
2.  Locate the `TX_POWER_DBM` definition (around line 33).
3.  Change the power level from the high-power preset (`5` dBm conducted) to a safe close-range setting such as `-10` dBm or `-18` dBm:
    ```cpp
    #define TX_POWER_DBM            -10
    ```
4.  Ensure that the operating frequency (`RADIO_FREQ_MHZ`) is identical on both units (default: `2440.0f`).
5.  Ensure that the sync word (`RADIO_SYNC_WORD`) matches on both units (default: `0xC3C3C3C3UL`).

---

## Step 2: Compile & Flash the Firmware

We will flash the compiled binary onto both boards.

1.  Connect **Unit A** to your computer via USB-C.
2.  Navigate to the `firmware/` directory in your terminal:
    ```bash
    cd firmware/
    ```
3.  Compile and upload the firmware:
    ```bash
    pio run -t upload
    ```
    *Note: The ESP32-S3's native USB CDC should automatically put the chip into bootloader mode and upload. If PlatformIO fails to connect, hold the physical **BOOT** button on the Lilygo board, press the **RST** button, release the **BOOT** button, and retry the upload command.*
4.  Once flashing completes, disconnect Unit A, connect **Unit B**, and repeat the upload command:
    ```bash
    pio run -t upload
    ```

---

## Step 3: Verify Hardware Boot & Screen Diagnostics

Upon booting, the ST7789 display should initialize immediately, draw the static layout grid, and display current status.

1.  Keep the boards connected to their USB ports.
2.  Verify the on-screen diagnostics:
    *   **Freq**: Should display `2440.0 MHz`.
    *   **Rate**: Should display `650 Kbps`.
    *   **State**: Should show `[IDLE]` in white.
    *   **Counters (TX/RX/Err)**: All should read `0`.
3.  Open the serial monitor on one of the units to verify SPI and RadioLib startup:
    ```bash
    pio device monitor
    ```
    You should see no initialization failures. If the radio fails to initialize, the screen will freeze and the firmware will halt inside `setup()`.

---

## Step 4: Launch the host-side `kiss-tun` Daemons

You can test this using two separate Raspberry Pi boards (representing Node A and Node B) or a single Linux computer using two USB ports.

### Scenario A: Bench Testing with Two Separate Linux Hosts (Recommended)
1.  Connect **Unit A** to **Host A**. Launch the daemon:
    ```bash
    sudo python3 pi-daemon/kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.1/30
    ```
2.  Connect **Unit B** to **Host B**. Launch the daemon:
    ```bash
    sudo python3 pi-daemon/kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.2/30
    ```

### Scenario B: Bench Testing with a Single Linux Host PC
If you only have one computer, you can bridge both devices on the same OS by assigning them different interface names and USB ports.

1.  Connect **both units** to your PC. They will typically enumerate as `/dev/ttyACM0` and `/dev/ttyACM1`.
2.  Open **Terminal 1** and launch Unit A (assigning interface `tun0`):
    ```bash
    sudo python3 pi-daemon/kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.1/30 --name tun0
    ```
3.  Open **Terminal 2** and launch Unit B (assigning interface `tun1`):
    ```bash
    sudo python3 pi-daemon/kiss_tun.py --port /dev/ttyACM1 --addr 10.0.0.2/30 --name tun1
    ```

---

## Step 5: Verify Link and ICMP Ping

Now, we will test the virtual network link.

1.  On **Host A** (or Terminal 1), ping **Host B**:
    ```bash
    ping -c 10 10.0.0.2
    ```
2.  Verify the following behaviors:
    *   ICMP echo replies should return successfully with a stable round-trip-time (RTT).
    *   **On-Screen Counters**: The `TX pkts` counter on Unit A and the `RX pkts` counter on Unit B should increment in lockstep.
    *   **Signal Strength**: The display on Unit B should show the real-time **RSSI** and **SNR** of the received packets. RSSI on a bench should typically register between `-40 dBm` and `-60 dBm`, and SNR should register high positive values (e.g. `+8.0 dB` to `+13.0 dB`).
    *   **State Indicator**: The state on the screen should flash dynamically to `[TX]` (yellow) and `[RX]` (green) as packets flow.

---

## Step 6: Throughput & Performance Verification (iperf3)

To push the FLRC link to its limits and measure throughput:

1.  On **Host B** (the receiver/target), launch the `iperf3` server:
    ```bash
    iperf3 -s
    ```
2.  On **Host A** (the sender), run the `iperf3` client:
    ```bash
    iperf3 -c 10.0.0.2 -t 10
    ```
3.  **Expected Outcome**: Under the default preset (650 Kbps bitrate / 3/4 coding rate), you should see a sustained throughput of **280 Kbps to 350 Kbps**. 
4.  **Error Check**: Verify that the `Errors` count on the screen remains at `0` or does not increase by more than 1% of the packet count, validating the LBT collision avoidance and SPI safety mechanisms.

---

## Bench Troubleshooting

*   **100% Packet Loss / No Ping**:
    *   Ensure the host daemons are running as `root` (sudo).
    *   Verify that `pyserial` connected successfully. If the daemon logs `Connection lost — retrying...`, the USB-C cable might be charge-only or the ESP32 is bootlooping.
*   **High Packet Error Rate (Errors > 2%)**:
    *   The units might be too close. Increase physical separation to at least 2 meters, or orient the antennas perpendicular to each other to attenuate the signal.
    *   Verify that no other heavy 2.4 GHz transmitters (e.g., active Wi-Fi routers or microwave ovens) are in close proximity, or switch `RADIO_FREQ_MHZ` to a quieter channel (e.g., `2480.0f`).
