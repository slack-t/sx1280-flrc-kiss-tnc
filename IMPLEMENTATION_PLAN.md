# Implementation Plan: Achieving 0% Packet Loss on Fragmented Pings via Spurious IRQ Detach Guards & Settling Pacing

We have made great progress with standby-TX fragmentation, but maximum MTU pings (`ping -s 476`, 4 fragments) still suffer from high packet loss. We have identified two critical timing/hardware root causes and have designed a production-grade solution to achieve a 100% reliable, 0% packet loss link.

## Root Cause Analysis

1. **Critical Spurious Interrupt Deafness Bug (Major Root Cause)**
   - In `Radio::readPacket`, when a spurious interrupt (such as a late/delayed `TX_DONE` edge) occurs while `_txActive` is `false`, it wakes up `radioRxTask`.
   - `radioRxTask` calls `readPacket`.
   - Since no packet is in the FIFO, `_radio.getPacketLength()` returns `0`.
   - `readPacket` sees `len == 0`, releases the SPI mutex, and **exits immediately without calling `clearIrqStatus()` or `_startReceiveNoLock()`**.
   - Because the radio was in `Standby/TX` mode when the spurious interrupt occurred, the radio is **permanently left in standby/TX and never returned to RX mode**. The node becomes completely deaf until it is rebooted or performs a transmission itself.
   
2. **Interrupt Edge Race Condition**
   - At high speed, even with the `_txActive` safety flag, the physical rising edge on the `RADIO_DIO1` pin during TX is registered by the ESP32 hardware interrupt controller.
   - If the interrupt is serviced slightly after `_txActive` is reset to `false` in `Radio::transmit()`, the ISR executes and spuriously signals `rxSemaphore`, triggering the deafness bug above.
   - We need a hardware-level way to completely block TX interrupts from registering during transmission.

3. **AGC/Carrier Settling Time**
   - In FLRC mode, the carrier is turned off between fragments (during the standby interval). When the next fragment starts, the receiver's AGC and carrier-tracking loops must lock on an extremely short preamble (32 bits = 49 µs at 650 kbps).
   - If the inter-fragment delay is too short (3 ms), the receiver's AGC does not have enough time to settle from high-signal saturation, leading to missed preambles and dropped fragments.

---

## Proposed Changes

### 1. Timing Optimization
#### [MODIFY] [config.h](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/config.h)
* Increase `RADIO_INTER_FRAG_DELAY_US` from `3000` (3 ms) to `5000` (5 ms) to guarantee robust AGC recovery and carrier relock on consecutive fragments.

---

### 2. Firmware Software-Based Hardware Interrupt Guard
#### [MODIFY] [Radio.cpp](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/radio/Radio.cpp)
* **TX Active Hold**: Keep `_txActive = true` in `Radio::transmit` until *after* the radio has been returned to receive mode (`_startReceiveNoLock(true)`) and `RADIO_DIO1` has been pulled low. This ensures any physical rising edge on DIO1 from transmission is ignored by the ISR while TX is active or while the radio is being set back to RX.
* **Physical Pin State Check (digitalRead Guard)**: In `Radio::_dio1Isr`, check `digitalRead(RADIO_DIO1) == HIGH`. Since SX1280 holds DIO1 high during a genuine RX event until it is read/cleared, checking the pin state allows us to immediately filter out any late-serviced `TX_DONE` interrupts (which will have been pulled low by `_startReceiveNoLock(true)` or RadioLib's own transmit cleanup).

---

### 3. Spurious IRQ & Deafness Prevention State Machine
#### [MODIFY] [Radio.cpp](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/radio/Radio.cpp)
* **Check RX_DONE Explicitly**: In `Radio::readPacket`, retrieve the hardware IRQ status. If `RX_DONE` is not set, treat the event as spurious, clear the interrupts on the chip, call `_startReceiveNoLock(true)` to guarantee we are back in RX, and return.
* **Safety Cleanup**: If `len == 0` or `len > PACKET_MAX_LEN`, clear interrupts and call `_startReceiveNoLock(true)` to reset RX mode before returning.

---

## Implementation Details

### Radio.cpp: Refined Interrupt Guard in `Radio::transmit`
```cpp
int16_t Radio::transmit(const Packet& pkt, bool isLastFragment) {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);

    _txActive = true;
    int16_t state = _radio.transmit(const_cast<uint8_t*>(pkt.data), pkt.len);

    // Keep _txActive set to true until we've returned the radio to RX mode
    // and pulled DIO1 low, so that the ISR completely ignores any late TX_DONE edges.
    if (isLastFragment || state != RADIOLIB_ERR_NONE) {
        _startReceiveNoLock(true);
    }

    _txActive = false;

    xSemaphoreGive(_spiMutex);
    return state;
}
```

### Radio.cpp: Pin State Check in `Radio::_dio1Isr`
```cpp
void IRAM_ATTR Radio::_dio1Isr() {
    // Suppress the TX-done DIO1 pulse — only signal on genuine RX events.
    if (_radioInstance && _radioInstance->rxSemaphore && !_radioInstance->_txActive) {
        // Fast hardware check: DIO1 must be physically HIGH for a genuine RX_DONE interrupt.
        // If the interrupt is serviced late (after TX is done and DIO1 is pulled low),
        // digitalRead will return LOW, allowing us to safely ignore the spurious event.
        if (digitalRead(RADIO_DIO1) == HIGH) {
            BaseType_t higher = pdFALSE;
            xSemaphoreGiveFromISR(_radioInstance->rxSemaphore, &higher);
            portYIELD_FROM_ISR(higher);
        }
    }
}
```

### Radio.cpp: Robust Spurious IRQ Prevention in `Radio::readPacket`
```cpp
// Define in Radio.h or config.h
#define ERR_SPURIOUS_IRQ -1000

int16_t Radio::readPacket(Packet& pkt) {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);

    // Verify that this is a genuine RX_DONE event on the SX1280 hardware.
    // This prevents spurious interrupts (like a late/delayed TX_DONE edge)
    // from leaving the radio deaf or triggering false reads.
    uint16_t irq = _radio.getIrqStatus();
    if (!(irq & RADIOLIB_SX128X_IRQ_RX_DONE)) {
        // Force the radio back into receive mode so it doesn't stay deaf.
        // _startReceiveNoLock(true) internally clears the chip's interrupt registers.
        _startReceiveNoLock(true);
        xSemaphoreGive(_spiMutex);
        return ERR_SPURIOUS_IRQ;
    }

    size_t len = _radio.getPacketLength();
    if (len == 0 || len > PACKET_MAX_LEN) {
        // Clear interrupts and force-reset RX mode to flush the FIFO.
        // Again, _startReceiveNoLock(true) handles clearing the chip's interrupts.
        _startReceiveNoLock(true);
        xSemaphoreGive(_spiMutex);
        return RADIOLIB_ERR_PACKET_TOO_LONG;
    }

    int16_t state = _radio.readData(pkt.data, len);
    pkt.len = (state == RADIOLIB_ERR_NONE) ? static_cast<uint8_t>(len) : 0;

    // Skip RSSI/SNR SPI reads for intermediate fragments — saves ~100µs of
    // turnaround time between consecutive fragments of the same IP frame.
    bool shouldQueryRssi = true;
    if (state == RADIOLIB_ERR_NONE && pkt.len > 0) {
        const uint8_t header   = pkt.data[0];
        const bool    is_split = (header & FRAMING_FLAG_SPLIT) != 0;
        const bool    is_last  = (header & FRAMING_FLAG_LAST)  != 0;
        if (is_split && !is_last) {
            shouldQueryRssi = false;
        }
    }

    if (shouldQueryRssi) {
        _lastRssi = static_cast<int8_t>(_radio.getRSSI());
        _lastSnr  = _radio.getSNR();
    }
    pkt.rssi = _lastRssi;
    pkt.snr  = _lastSnr;

    // RX-to-RX: skip setPreambleLength since packet params haven't changed.
    _startReceiveNoLock(false);
    xSemaphoreGive(_spiMutex);
    return state;
}
```

---

## Verification Plan

### Automated Tests
- Run `pio test -e native` to ensure KISS framing codec unit tests are 100% correct.
- Build the firmware using `pio run` to verify it compiles perfectly with the new changes.

### Manual Link Verification
1. Connect both Lilygo T3S3 boards and flash the timing-optimized and spurious-IRQ protected firmware:
   ```bash
   pio run -t upload
   ```
2. Start the Rust daemon on both nodes:
   - **Node A (10.0.0.1)**: `sudo ./pi-daemon-rust/target/release/kiss-tun-rs --port /dev/ttyACM0 --addr 10.0.0.1/30`
   - **Node B (10.0.0.2)**: `sudo ./pi-daemon-rust/target/release/kiss-tun-rs --port /dev/ttyACM0 --addr 10.0.0.2/30`
3. Verify basic connectivity with single-packet pings:
   ```bash
   ping -c 10 10.0.0.2
   ```
4. Verify fragmented performance at max MTU:
   ```bash
   ping -s 476 -c 50 10.0.0.2
   ```
   *Expected outcome*: Flawless responses with **0% packet loss** and stable RTTs (~65 ms).
5. Verify Link Recovery: Disconnect the serial/daemon on one node during active pings, wait, reconnect, and verify that the radio immediately recovers and continues receiving packets.
