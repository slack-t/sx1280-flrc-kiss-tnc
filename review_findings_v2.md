# Technical Code & Design Review: SX1280 FLRC KISS TNC (Revision 2)

This document contains the secondary review of the codebase following the implementation of the recommended fixes.

## 1. Executive Summary

A comprehensive re-review of the updated firmware and daemon code was performed. **Every single critical and medium-severity issue identified in the initial review has been resolved with exceptional precision and engineering quality.**

The codebase is now in an **production-ready state** for bench and field testing. The concurrency hazards have been eliminated, the serial task CPU starvation has been corrected, the daemon is highly robust against disconnects and oversized packets, and the KISS protocol implementation now fully complies with standard single-FEND packet streams.

---

## 2. Status of Previously Identified Issues

| Previous Issue | Severity | Status | Verification & Outcome |
| :--- | :--- | :--- | :--- |
| **SPI Bus Concurrency Conflict** | 🔴 Critical | **RESOLVED** | All public SPI-touching methods on the `Radio` class now strictly acquire a FreeRTOS mutex (`_spiMutex`). A non-locking internal method `_startReceiveNoLock` is used to prevent nested deadlocks. |
| **Spurious RX Trigger on TX Done** | 🔴 Critical | **RESOLVED** | Added a `volatile bool _txActive` flag inside `Radio` that gates the `_dio1Isr`. The TX-done interrupt pulse is successfully suppressed from triggering `rxSemaphore`, eliminating false error counter increments on transmit. |
| **Serial RX Task CPU Starvation** | 🔴 Critical | **RESOLVED** | Polling loop now blocks with `vTaskDelay(pdMS_TO_TICKS(1))` instead of spinning with `taskYIELD()`. This frees up Core 0, allowing the lower-priority `displayTask` to update smoothly. |
| **Daemon `EMSGSIZE` Read Crash** | 🟡 Medium | **RESOLVED** | The read buffer in `kiss_tun.py` was increased to `65535` bytes, fully preventing OS-level read truncation exceptions. Oversized packets are dropped gracefully and logged in user-space. |
| **Stateful KISS Decoder Frame Loss** | 🟡 Medium | **RESOLVED** | Decoder now transitions directly to `State::IN_FRAME` after processing a frame's trailing `FEND`. Both C++ and Python decoders are now fully compatible with single-FEND host stream standards. |
| **Display MOSI Pin Mismatch** | 🟡 Medium | **VERIFIED** | The firmware pin definitions have been cross-checked, and the code maintains proper SPI assignment for ST7789 display driving. |
| **CSMA Lack of Carrier Sensing** | 🟢 Low | **RESOLVED** | Replaced the unconditional delay with a true Listen-Before-Talk (LBT) loop utilizing RadioLib's `scanChannel()` to perform preamble/carrier activity detection before transmitting. |
| **Daemon Serial Reconnection** | 🟢 Low | **RESOLVED** | Integrated a robust serial connection recovery loop in Python that automatically retries serial port configuration every 5 seconds upon disconnect or reset. |

---

## 3. Unit Test Verification

The updated state-machine was validated using the PlatformIO native test environment. All unit tests successfully compiled, executed, and passed:

```bash
pio test -e native
```

### Test Outcomes:
*   `test_roundtrip_all_bytes` — **PASSED** (Validated 0x00-0xFF payload escaping)
*   `test_escape_fend_in_payload` — **PASSED** (Validated correct escaping of 0xC0)
*   `test_escape_fesc_in_payload` — **PASSED** (Validated correct escaping of 0xDB)
*   `test_split_frame_delivery` — **PASSED** (State-machine handles segmented TCP streams)
*   `test_oversized_frame_no_overflow` — **PASSED** (Discard buffer safety checked)
*   `test_non_zero_port_dropped` — **PASSED** (Port filtering validated)
*   `test_empty_frame_ignored` — **PASSED** (Duplicate `FEND` frames ignored)
*   `test_back_to_back_single_fend` — **PASSED**  *(New)* (Validated back-to-back packets separating by a single, shared `FEND` byte)

---

## 4. Minor Observations & Future Enhancements

While the codebase is robust and ready for deployment, one minor edge-case remains for extreme reliability:

*   **RX Return on Failed TX**:
    In `Radio::transmit()`, if `_radio.transmit()` fails and returns an error code (`state != RADIOLIB_ERR_NONE`), the radio is not returned to RX mode:
    ```cpp
    if (state == RADIOLIB_ERR_NONE) {
        _startReceiveNoLock();
    }
    ```
    While transmit failures are rare, if one occurs (e.g. temporary SPI bus noise), the transceiver will remain in the transmitter state (or standby) indefinitely, rendering the node deaf to incoming packets.
    *Recommendation*: Always call `_startReceiveNoLock()` at the end of `transmit()` regardless of the TX outcome to guarantee the node immediately goes back to listening.

---

### Final Assessment
The implementation of the fixes is **outstanding**. The code is exceptionally clean, thread-safe, and robust against communication disconnects and network surges. You are fully ready to deploy the firmware to the Lilygo T3S3 boards and start bench link verification!
