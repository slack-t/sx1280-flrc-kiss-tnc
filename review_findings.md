# Technical Code & Design Review: SX1280 FLRC KISS TNC

This document contains a comprehensive review of the implementation plan and the corresponding source code for the **SX1280 FLRC KISS TNC** project. 

The overall structure of the codebase is exceptionally clean, well-modularized, and demonstrates a strong understanding of FreeRTOS task division and standard network bridging principles. However, the deep review has revealed several high-risk concurrency bugs, hardware-software discrepancies, and protocol edge cases that could cause system lockups, packet corruption, high CPU usage, or device crashes.

---

## 1. Summary of Critical Findings

The most significant issues identified during this review are summarized below:

| Severity | Issue Category | Description | Primary Impact |
| :--- | :--- | :--- | :--- |
| 🔴 **Critical** | Concurrency / SPI | Unsynchronized SPI access to the shared `SX1280` instance by concurrent FreeRTOS tasks. | Hardware lockups, ESP32 panics, SPI register corruption. |
| 🔴 **Critical** | Interrupt Logic | DIO1 interrupt fires on both RX and TX complete, causing spurious packet reads and error increments on every TX. | Constant error count increments, display state flickering to `[ERR]` on TX. |
| 🔴 **Critical** | FreeRTOS Scheduling | `serialRxTask` uses a non-blocking spin-lock with `taskYIELD()`, saturating Core 0 and starving the display task. | Complete display update starvation (display stays static/blank). |
| 🟡 **Medium** | Daemon Robustness | Reading from Linux TUN device with a fixed small buffer (`DEFAULT_MTU`) raises a fatal `EMSGSIZE` exception. | Python daemon crash when receiving packets > 127 bytes. |
| 🟡 **Medium** | KISS Protocol | Stateful C++ KISS decoder resets to `IDLE` after a frame, losing every alternate frame on single-`FEND` hosts. | High packet loss with standard third-party KISS TNC hosts. |
| 🟡 **Medium** | Hardware / Pinout | Discrepancy in `DISPLAY_MOSI` pin definition (GPIO 19 in plan vs. GPIO 18 in code). | Blank display or hardware SPI malfunction. |
| 🟢 **Low** | CSMA Logic | The "CSMA" delay is applied unconditionally and does not actually perform carrier sensing (LBT). | Blind transmissions with unnecessary latency. |

---

## 2. Detailed Technical Analysis & Recommended Fixes

### 🔴 SPI Bus Concurrency Conflict (High Risk)
*   **Location**: `firmware/src/main.cpp` (`radioRxTask` and `radioTxTask`), `firmware/src/radio/Radio.cpp`
*   **The Problem**: 
    The `radioRxTask` (waiting on `rxSemaphore`) and `radioTxTask` (waiting on `txQueue`) both run concurrently on **Core 1** at the same priority level (`PRIO_RADIO = 4`). Both tasks make direct calls to the shared `radio` instance (and under the hood, the `SX1280` object via SPI) without any mutual exclusion. 
    If a packet is received via the radio at the exact moment a packet is queued from the serial port for transmission, both tasks will attempt to read/write SPI registers simultaneously. 
*   **Consequence**: 
    SPI transactions will collide and corrupt each other. This typically leads to the SX1280 entering an undefined state, lockups on the SPI bus (getting stuck waiting for the `BUSY` pin to go low), or hardware-level ESP32 panics.
*   **Recommended Fix**: 
    Protect all SPI access to the `_radio` module using a FreeRTOS mutex inside the `Radio` wrapper class, or coordinate both RX and TX flows within a single, sequential radio manager task instead of two concurrent tasks.
    ```cpp
    // Example Mutex Protection in Radio.h/Radio.cpp
    SemaphoreHandle_t spiMutex = xSemaphoreCreateMutex();
    // In Radio::transmit and Radio::readPacket:
    xSemaphoreTake(spiMutex, portMAX_DELAY);
    // ... SPI operations ...
    xSemaphoreGive(spiMutex);
    ```

---

### 🔴 Spurious RX Trigger on TX Done (High Risk)
*   **Location**: `firmware/src/radio/Radio.cpp` (`Radio::begin()`, `_dio1Isr`), `firmware/src/main.cpp` (`radioRxTask`)
*   **The Problem**: 
    In `Radio::begin()`, the hardware DIO1 line is attached to a generic interrupt service routine:
    ```cpp
    _radio.setDio1Action(_dio1Isr);
    ```
    On the SX1280, DIO1 is triggered by **both RX complete and TX complete** events. When a transmission finishes, DIO1 will pulse, triggering `_dio1Isr`, which immediately executes `xSemaphoreGiveFromISR(rxSemaphore, ...)`.
    This wakes up the `radioRxTask`, which immediately calls `radio.readPacket(pkt)`. Since the device just finished transmitting and did not actually receive a packet, `readPacket()` will fail (returning 0 length or an error).
*   **Consequence**: 
    Every single transmission will trigger a spurious RX sequence. This will increment the `errorCount` and change the UI's radio state to `[ERR]` (red) on every TX. The error counters will grow rapidly under load, and the display will constantly flicker into the error state.
*   **Recommended Fix**: 
    Inside `_dio1Isr` or `radioRxTask`, read the actual interrupt status register from the SX1280 using `_radio.getIrqStatus()` to verify if `RADIOLIB_SX128X_CLEAR_IRQ_RX_DONE` is set before processing the packet.
    Alternatively, block the RX task from running or temporarily detach/ignore the interrupt during active transmissions.

---

### 🔴 Serial RX Task CPU Saturation & Starvation (High Risk)
*   **Location**: `firmware/src/main.cpp` (`serialRxTask`)
*   **The Problem**: 
    The `serialRxTask` uses a tight polling loop to check for serial bytes:
    ```cpp
    for (;;) {
        if (Serial.available()) {
            // ...
        } else {
            taskYIELD();
        }
    }
    ```
    `taskYIELD()` only yields to other tasks of **equal or higher priority** that are ready to run. Since there are no other tasks at priority `PRIO_SERIAL = 3` or higher active on **Core 0**, the CPU will immediately re-schedule `serialRxTask`.
*   **Consequence**: 
    `serialRxTask` will spin-lock, consuming 100% of Core 0's CPU capacity. Because `displayTask` is configured at a lower priority (`PRIO_DISPLAY = 1`), it will be **completely starved of CPU cycles**. The display will never update and will remain frozen or blank.
*   **Recommended Fix**: 
    Add a small delay when no serial data is available to yield Core 0 to lower-priority tasks, or block on a blocking read:
    ```cpp
    if (Serial.available()) {
        // ...
    } else {
        vTaskDelay(pdMS_TO_TICKS(1)); // Block for 1 tick, allowing displayTask to run
    }
    ```

---

### 🟡 Daemon `EMSGSIZE` Read Crash (Medium Risk)
*   **Location**: `pi-daemon/kiss_tun.py` (`tun_to_radio`)
*   **The Problem**: 
    The daemon reads from the TUN file descriptor using a fixed size matching `DEFAULT_MTU` (127 bytes):
    ```python
    pkt = os.read(tun.fileno(), DEFAULT_MTU)
    ```
    Under Linux, if an application attempts to write a packet to the `tun0` interface that is larger than the buffer supplied to `os.read()`, the kernel cannot fit the packet into the buffer. Instead of truncating the packet silently, the read system call will fail and raise an `OSError` with `Errno 90: Message too long` (`EMSGSIZE`).
*   **Consequence**: 
    If a process sends a standard 1500-byte IP packet (or even a default 150-byte ping) to `tun0` before the MTU is fully respected or if MTU configuration fails, the Python daemon's transmission thread will throw an unhandled exception and immediately crash.
*   **Recommended Fix**: 
    Read a large buffer (e.g. 2048 or 65535 bytes) from the TUN device, and then perform length checks in Python to drop oversized packets gracefully and log a warning:
    ```python
    pkt = os.read(tun.fileno(), 2048)
    if len(pkt) > DEFAULT_MTU:
        print(f"[Warning] Dropped packet exceeding MTU ({len(pkt)} bytes)")
        continue
    ```

---

### 🟡 Stateful KISS Decoder Alternating Frame Loss (Medium Risk)
*   **Location**: `firmware/src/kiss/Kiss.cpp` (`Kiss::decode()`)
*   **The Problem**: 
    When `Kiss::decode()` successfully finishes decoding a packet upon encountering the trailing `KISS_FEND` byte, it resets its state to `State::IDLE`:
    ```cpp
    _state  = State::IDLE;
    return true;
    ```
    In standard KISS, a single `FEND` byte is often used to separate back-to-back packets (e.g., `FEND payload1 FEND payload2 FEND`).
    If a host transmits back-to-back packets using this single-`FEND` framing:
    1. The first frame is decoded successfully; state transitions to `IDLE`.
    2. The next incoming byte is the port/command byte of the second frame (e.g. `0x00`).
    3. Because the decoder is in `IDLE`, it ignores the port byte and all subsequent data bytes of the second frame because it is looking specifically for a leading `FEND`.
    4. It only enters `IN_FRAME` again when it sees the trailing `FEND` of the second frame.
*   **Consequence**: 
    Every alternate packet will be completely lost if the transmitting host uses a single-`FEND` frame delimiter.
*   **Recommended Fix**: 
    When a trailing `FEND` is received and a valid packet is returned, transition directly to `State::IN_FRAME` (with `_len = 0`) instead of `State::IDLE`, so the decoder is immediately ready to treat the next incoming non-`FEND` byte as the start of the next frame.

---

### 🟡 Display MOSI Pin Mismatch (Medium Risk)
*   **Location**: `firmware/src/config.h` vs `IMPLEMENTATION_PLAN.md`
*   **The Problem**: 
    The pinout table in the implementation plan specifies:
    ```markdown
    | Display MOSI  | 19            |
    ```
    However, the firmware configuration header file defines:
    ```cpp
    #define DISPLAY_MOSI    18
    ```
*   **Consequence**: 
    If the board is wired according to the implementation plan (GPIO 19), the software (configured to use GPIO 18) will fail to communicate with the display, resulting in a blank screen.
*   **Recommended Fix**: 
    Verify the physical layout of the Lilygo T3S3 hardware revision and align the pin configuration. (Note: On the standard T3S3 board, GPIO 18 is typically the I2C SDA pin for the built-in OLED, whereas GPIO 19 is often used for external display or USB. Ensure that the correct pins are utilized based on whether an external ST7789 TFT display is being wired via SPI).

---

### 🟢 CSMA Blind Transmit Delay (Low Risk)
*   **Location**: `firmware/src/main.cpp` (`radioTxTask`)
*   **The Problem**: 
    The configuration header describes the CSMA implementation as:
    ```cpp
    // TX task waits a random interval in [0, CSMA_MAX_BACKOFF_MS] if the radio
    // reports the channel busy before transmitting.
    ```
    However, in `radioTxTask`, the random delay is applied **unconditionally** before every single transmit, and no carrier sensing (RSSI checks or preamble scans) is performed.
*   **Consequence**: 
    This adds unnecessary latency (up to 10 ms per packet) to every transmission without offering any actual protection against packet collisions if another node is actively transmitting.
*   **Recommended Fix**: 
    To implement true Carrier Sense Multiple Access (CSMA) or Listen-Before-Talk (LBT):
    1. Read the current channel RSSI using `_radio.getRSSI()` or trigger a Carrier Activity Detection (CAD) scan.
    2. If the channel is busy, apply the random backoff delay and check again.
    3. If the channel is clear, transmit immediately without adding unconditional delay.

---

## 3. Code Optimization & Robustness Notes

1.  **Display Flash Blocking Delays**:
    The functions `Display::flashTx()` and `Display::flashRx()` contain a blocking `delay(100)` call. While currently unused (dead code), if these functions are integrated in the future, calling them from high-priority communication tasks will halt those tasks for 100 ms, causing severe packet loss and serial buffer overflows. It is recommended to implement these flashes asynchronously (e.g., using a timer or checking elapsed time in the main display loop).
2.  **No Serial Reconnection Logic**:
    If the USB connection between the Raspberry Pi and the ESP32-S3 is interrupted or if the ESP32 is rebooted, the `/dev/ttyACM0` serial port will disconnect. The python script `kiss_tun.py` will throw a serial communication exception and exit immediately. Adding an automatic reconnection loop in the python daemon would significantly improve field reliability.
3.  **Core Pinning Strategy**:
    Pinning `radioRxTask` and `radioTxTask` to Core 1 and serial tasks to Core 0 is excellent. It ensures that heavy serial formatting/escaping does not jitter the precise timing required by the radio module. Keep this architecture!

---

### Conclusion & Suggested Next Steps
The codebase is highly functional, but the **SPI concurrency conflict**, **spurious RX triggers**, and **CPU starvation of Core 0** are critical bugs that should be resolved before bench testing begins. Applying the recommended fixes outlined in Section 2 will establish a solid, highly robust, and performant base for point-to-point wireless backhaul.
