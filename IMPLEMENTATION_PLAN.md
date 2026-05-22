# Implementation Plan: Eliminating Packet Loss on Fragmented Pings via Standby-TX Pacing

We have made great progress with low-latency pacing, but `ping -s 476` (maximum MTU, 4 fragments) still suffers from high packet loss. 

## Root Cause Analysis
During fragmented packet transmission inside `radioTxTask`, the current code calls `radio.transmit(pkt)` for each fragment. Inside `Radio::transmit`, the radio is unconditionally returned to receive mode (`_startReceiveNoLock(true)`) after every single fragment.

This creates a devastating RF environment:
1. **PLL/Synthesizer Splatter**: The transmitter constantly unlocks its PLL, switches to the RX frequency, turns on the LNA, and then immediately unlocks again, switches to the TX frequency, and ramps up the PA for the next fragment.
2. **Frequency and Phase Instability**: These high-speed toggles create thermal and electrical transients, leading to frequency drift and phase noise.
3. **Receiver Sync Loss**: The remote receiver's AGC (Automatic Gain Control) and preamble detectors get confused by the rapid toggles and carrier drops, causing it to fail to synchronize on subsequent fragments.

## Proposed Solution
We will modify the transmitter to remain in a clean **Standby/TX state** between fragments, and only transition back to RX mode after the **final** fragment of the frame has been fully transmitted. This eliminates PLL toggling and PA transients, ensuring a highly stable continuous RF signal for the remote receiver to lock onto.

---

## User Review Required

> [!IMPORTANT]
> **Eliminating Inter-Fragment RX Toggles**:
> * We are updating `Radio::transmit` to accept an optional `isLastFragment` parameter.
> * The radio will only re-enter RX mode on the final fragment of an IP frame.
> * This preserves PLL lock, stabilizes the PA, and ensures a clean RF signal, which should resolve the remaining packet drops.

---

## Proposed Changes

### 1. Radio Driver Layer

#### [MODIFY] [Radio.h](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/radio/Radio.h)
* Update `transmit` to accept a `bool isLastFragment = true` parameter.

```diff
-    // Transmit a packet. Blocks until TX complete, then returns to RX.
-    // Returns RADIOLIB_ERR_NONE on success.
-    int16_t transmit(const Packet& pkt);
+    // Transmit a packet. Blocks until TX complete.
+    // If isLastFragment is true, it returns to RX mode. Otherwise, it stays in standby.
+    // Returns RADIOLIB_ERR_NONE on success.
+    int16_t transmit(const Packet& pkt, bool isLastFragment = true);
```

#### [MODIFY] [Radio.cpp](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/radio/Radio.cpp)
* Update `transmit` implementation to only call `_startReceiveNoLock(true)` if `isLastFragment` is true (or if transmission failed, as a safety fallback).

```diff
-int16_t Radio::transmit(const Packet& pkt) {
+int16_t Radio::transmit(const Packet& pkt, bool isLastFragment) {
     xSemaphoreTake(_spiMutex, portMAX_DELAY);
 
     _txActive = true;
     int16_t state = _radio.transmit(const_cast<uint8_t*>(pkt.data), pkt.len);
     _txActive = false;
 
-    // Always return to RX — even on TX failure the node must not stay deaf.
-    _startReceiveNoLock(true);
+    // Only return to RX on the final fragment (or if TX failed, as a safety fallback)
+    if (isLastFragment || state != RADIOLIB_ERR_NONE) {
+        _startReceiveNoLock(true);
+    }
 
     xSemaphoreGive(_spiMutex);
     return state;
 }
```

### 2. Main Application Firmware

#### [MODIFY] [main.cpp](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/main.cpp)
* Update the transmit call inside `radioTxTask` to pass `is_last`.

```diff
-            int16_t err = radio.transmit(pkt);
+            int16_t err = radio.transmit(pkt, is_last);
```

---

## Verification Plan

### Automated Tests
- Run `pio test -e native` to ensure KISS state-machine functionality is perfect.
- Compile firmware using `pio run` to verify syntax and timing macros.

### Manual Verification
- Flash both boards:
  ```bash
  pio run -t upload
  ```
- Run Rust daemon on both ends and execute fragmented pings:
  ```bash
  ping -s 476 -c 30 10.0.0.2
  ```
  *Expected outcome*: Low RTT (~60 ms total including serial and processing) and **0% packet loss** with clean, continuous RF lock.
