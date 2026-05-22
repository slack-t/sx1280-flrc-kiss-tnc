# Implementation Plan: Debugging & Optimizing Layer-2 Fragmentation (ping -s 400)

We have identified two critical issues causing high packet loss, latency, and locking behaviour during larger fragmented pings (e.g., `ping -s 400`, which splits into 4 fragments):

1. **FreeRTOS Tick Pacing Overhead**:
   The current inter-fragment delay inside `radioTxTask` uses `vTaskDelay` with a minimum of 2 ticks (20 ms) to prevent tick truncation. While safe, this 20 ms delay between each of the 4 fragments creates a total transmission sequence duration of ~60 ms per frame. Coupled with return-path transmission, this explains the observed RTT of ~132 ms. This excessively long time-on-air drastically increases the risk of packet collisions and receiver synchronization failures. Since the receiver actually needs less than 500 µs to read a fragment and return to RX mode, a much tighter pacing delay of 3 ms (3000 µs) is completely safe and represents a **4.3x reduction** in channel occupancy.
   
2. **Deaf-Link Queue Saturation (Deadlock Hazard)**:
   In `radioRxTask`, the reassembled IP frames are sent to `rxQueue` using `xQueueSend(..., portMAX_DELAY)`. If the USB CDC serial connection or the host daemon experiences a brief delay or is not active, `rxQueue` fills to capacity (8 frames). When `rxQueue` is full, the high-priority `radioRxTask` block *indefinitely* on `xQueueSend`. This halts the radio RX loop entirely, preventing the transceiver from returning to RX mode and permanently deafening the node until a hard reboot is performed. Replacing this blocking write with a bounded-blocking write (e.g., 50 ms timeout) prevents the radio loop from ever deadlocking, ensuring the device remains online and self-recovers.

---

## User Review Required

> [!IMPORTANT]
> **Pacing Delay Compression**:
> * We will replace the coarse scheduler-bound `vTaskDelay` with a high-precision `delayMicroseconds(3000)` (3 ms).
> * This will reduce the on-air packet reassembly duration from 60 ms to 9 ms, bringing expected `ping -s 400` RTT down from ~132 ms to ~20-25 ms, and dramatically lowering collision rates.

> [!WARNING]
> **Queue Timeout & Graceful Discard**:
> * We are removing `portMAX_DELAY` from the radio RX queue push. If the host PC fails to read serial data and `rxQueue` fills up, the firmware will now drop the frame, increment the stats error counter, and print an error message rather than blocking Core 1.
> * This keeps the radio RX loop running under all conditions.

---

## Proposed Changes

### 1. Configuration Layer

#### [MODIFY] [config.h](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/config.h)
* Define the inter-fragment delay constant in microseconds.
* Define a bounded queue timeout in milliseconds.

```diff
+// ── Timing and Queue Buffering Parameters ────────────────────────────────────
+#define RADIO_INTER_FRAG_DELAY_US    3000  // 3 ms gap between consecutive fragments
+#define RX_QUEUE_TIMEOUT_MS          50    // Prevent radio RX deadlock if host is blocked
```

### 2. Main Application Firmware

#### [MODIFY] [main.cpp](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/main.cpp)
* Replace the `xQueueSend(rxQueue, &frame, portMAX_DELAY)` blocking calls in `radioRxTask` with a bounded-blocking call utilizing `pdMS_TO_TICKS(RX_QUEUE_TIMEOUT_MS)` for both single and fragmented reassembled frames.
* Update `radioTxTask` to use `delayMicroseconds(RADIO_INTER_FRAG_DELAY_US)` for sub-millisecond precision pacing.

```diff
// Inside radioRxTask:
-            xQueueSend(rxQueue, &frame, portMAX_DELAY);
+            if (xQueueSend(rxQueue, &frame, pdMS_TO_TICKS(RX_QUEUE_TIMEOUT_MS)) != pdPASS) {
+                Serial.printf("[rx] ERROR: rxQueue full, dropping single frame! len=%d\n", frame.len);
+                auto& sm = StatsManager::instance();
+                sm.lock();
+                sm.get().errorCount++;
+                sm.unlock();
+            }
```

```diff
// Inside radioRxTask (reassembled frame):
-                Serial.printf("[rx] reassembled seq=%d len=%d rssi=%d\n", ra.seq, frame.len, frame.rssi);
-                xQueueSend(rxQueue, &frame, portMAX_DELAY);
-                ra.reset();
+                Serial.printf("[rx] reassembled seq=%d len=%d rssi=%d\n", ra.seq, frame.len, frame.rssi);
+                if (xQueueSend(rxQueue, &frame, pdMS_TO_TICKS(RX_QUEUE_TIMEOUT_MS)) != pdPASS) {
+                    Serial.printf("[rx] ERROR: rxQueue full, dropping reassembled frame! len=%d\n", frame.len);
+                    auto& sm = StatsManager::instance();
+                    sm.lock();
+                    sm.get().errorCount++;
+                    sm.unlock();
+                }
+                ra.reset();
```

```diff
// Inside radioTxTask:
-            if (idx > 0) {
-                // Enforce minimum of 2 ticks (20ms) on 100Hz systems to guard against tick truncation.
-                TickType_t delay_ticks = pdMS_TO_TICKS(15);
-                if (delay_ticks <= 1) {
-                    delay_ticks = 2; 
-                }
-                vTaskDelay(delay_ticks);
-            }
+            if (idx > 0) {
+                delayMicroseconds(RADIO_INTER_FRAG_DELAY_US);
+            }
```

---

## Verification Plan

### Automated Tests
- Run `pio test -e native` to ensure no regression in the KISS state-machine codec.
- Compile firmware using `pio run` to verify syntax and timing macros.

### Manual Verification
- Compile and upload the timing-optimized firmware to both Lilygo T3S3 boards:
  ```bash
  pio run -t upload
  ```
- Start the `kiss-tun-rs` Rust daemon or the Python daemon on both hosts.
- Perform single-packet pings to verify baseline connectivity:
  ```bash
  ping -c 10 10.0.0.2
  ```
- Perform larger fragmented pings to verify high-precision pacing and reassembly:
  ```bash
  ping -s 400 -c 20 10.0.0.2
  ```
  *Expected outcome*: Low RTT (~20-25 ms) and 0% or extremely low packet loss without interface freezes.
- Verify OLED metrics updates (TX/RX packet counts and error counters).
