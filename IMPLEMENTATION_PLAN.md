# Implementation Plan: Phase 2 — Ultra-Reliability & Maximum-Range Link Optimization

We have successfully stabilized the FLRC link by resolving the critical spurious interrupt deafness
and implementing the noise-immune high-entropy sync word `{ 0x7E, 0xC5, 0xA2, 0x3D }`.

Based on the design preference to prioritize **absolute reliability (0% packet loss) and maximum
range** over low latency, Phase 2 tunes the FLRC physical layer for maximum link budget, extreme
error correction, and robust timing margins.

---

## Diagnostic Procedure

Before applying any changes, establish a per-fragment-count baseline to isolate whether the root
cause is RF interference or a firmware inter-fragment timing issue:

```bash
ping -c 50 10.0.0.2           # ~84 bytes  — 1 radio fragment
ping -s 100 -c 50 10.0.0.2   # ~128 bytes — 2 radio fragments
ping -s 240 -c 50 10.0.0.2   # ~268 bytes — 3 radio fragments
ping -s 400 -c 50 10.0.0.2   # ~428 bytes — 4 radio fragments
```

Run the serial monitor in parallel on the receiving node to observe fragment arrivals:

```bash
pio device monitor
```

Watch for `[rx] stale seq`, `[rx] seq abandoned`, and which `idx` values appear in fragment log
lines. If loss only appears at ≥3 fragments and the missing index is always 2 or 3, the cause is
firmware timing. If loss is present even at 1–2 fragments, the cause is RF interference.

---

## Proposed Reliability Optimizations

### 1. Robust Coding Rate (CR 1/2) for Maximum Error Correction

* **The Concept**: The current `CR 3/4` provides moderate error correction overhead.
* **The Optimization**: Change `RADIO_CODING_RATE` to `2` (CR 1/2).
* **The Impact**: Enables the SX1280 FLRC hardware's most robust FEC, doubling the correction
  bits per packet. The receiver can reconstruct packets even when multiple bits are corrupted by
  path loss or brief interference.

### 2. Decrease Bitrate to 325 kbps for +3.5 dBm Sensitivity

* **The Concept**: Receiver sensitivity is directly tied to modulation bandwidth and data rate. A
  lower bitrate requires less SNR to decode successfully.
* **The Optimization**: Reduce `RADIO_BITRATE_KBPS` from `650.0f` to `325.0f`.
* **The Impact**:
  * Improves receiver sensitivity by approximately **+3.5 dBm** vs 650 kbps, and **+6.5 dBm**
    vs 1300 kbps.
  * +3 dBm ≈ 2× received power, providing a substantial safety margin against signal fades and
    increased obstacle penetration.
  * Note: at 325 kbps with CR 1/2, each 127-byte fragment occupies **~6.35 ms on air** (the
    coding overhead doubles the transmitted bits). Plan timing margins accordingly.

### 3. Highly Bounded 8 ms Inter-Fragment Pacing

* **The Optimization**: Set `RADIO_INTER_FRAG_DELAY_US` to `8000` (8 ms).
* **The Impact**: At ~6.35 ms fragment airtime + 8 ms gap, the receiver has approximately
  **7.7 ms of slack** after completing all SPI work (~300 µs) before the next fragment begins.
  This eliminates any FreeRTOS scheduling jitter or SPI bus contention as a loss source.

### 4. Move Operating Frequency to 2480 MHz

* **The Concept**: 2440 MHz sits between WiFi channels 6 and 7 — the most congested part of the
  2.4 GHz ISM band. A concurrent WiFi transmission during a fragment's ~6.35 ms on-air window
  causes a CRC error regardless of sync word. The high-entropy sync word prevents false lock-on
  but does not protect fragments from being corrupted by simultaneous WiFi energy.
* **The Optimization**: Change `RADIO_FREQ_MHZ` from `2440.0` to `2480.0`.
* **The Impact**: 2480 MHz is above WiFi channel 13 (2472 MHz) in EU band plans — significantly
  less WiFi traffic. Reduces per-fragment collision probability, directly improving reliability on
  fragmented packets.

### 5. Force Full RX Reset Between All Fragment Receptions

* **The Concept**: After reading each fragment, `readPacket()` calls `_startReceiveNoLock(false)`
  which skips `setPreambleLength()` to save ~20 µs per SPI round-trip. With an 8 ms inter-fragment
  gap this saving is irrelevant, and skipping the full reset risks leaving the SX1280 in a
  partially-configured state after variable-length FLRC packets (known dynamic payload length
  lockout mode, obs 256).
* **The Optimization**: Change the RX-to-RX path in `readPacket()` to always call
  `_startReceiveNoLock(true)`.
* **The Impact**: Forces `setPreambleLength()` + full `startReceive()` on every packet read,
  ensuring the radio is fully re-armed regardless of the previous packet's payload length.

---

## Proposed Changes

### [MODIFY] `firmware/src/config.h`

```diff
- #define RADIO_FREQ_MHZ          2440.0f
+ #define RADIO_FREQ_MHZ          2480.0f

- #define RADIO_BITRATE_KBPS      650.0f
+ #define RADIO_BITRATE_KBPS      325.0f

- #define RADIO_CODING_RATE       3
+ #define RADIO_CODING_RATE       2

- #define RADIO_INTER_FRAG_DELAY_US    5000  // 5 ms gap between consecutive fragments
+ #define RADIO_INTER_FRAG_DELAY_US    8000  // 8 ms gap between consecutive fragments
```

### [MODIFY] `firmware/src/radio/Radio.cpp`

In `Radio::readPacket()`, change the final RX restart to always force a full reset:

```diff
-    // RX-to-RX: skip setPreambleLength since packet params haven't changed.
-    _startReceiveNoLock(false);
+    _startReceiveNoLock(true);
```

> **Important**: Both nodes must be flashed with identical `RADIO_FREQ_MHZ`, `RADIO_BITRATE_KBPS`,
> and `RADIO_CODING_RATE`. A mismatch on any of these produces a completely silent link with no
> error indication.

---

## Verification Plan

### 1. Automated Verification

Compile to confirm RadioLib accepts the new parameters:

```bash
pio run
```

### 2. Per-Fragment Reliability Baseline

Flash both nodes, then run the four-step ping ladder:

```bash
ping -c 50 10.0.0.2
ping -s 100 -c 50 10.0.0.2
ping -s 240 -c 50 10.0.0.2
ping -s 400 -c 50 10.0.0.2
```

*Expected outcome*: 0% loss at all sizes with stable RTT (~110 ms for 4-fragment pings).

### 3. Extended Stress Test

```bash
ping -s 400 -c 200 10.0.0.2
```

*Expected outcome*: 0% packet loss across 200 packets, RTT variance < 10 ms.

### 4. Distance & Obstacle Test

Verify that the link remains 100% reliable through walls and at increased distance, exploiting the
+3.5 dBm sensitivity gain from the bitrate reduction.

---

## Future Work: Fragment-Level Erasure Coding (Phase 3)

If RF interference at 2480 MHz is still a source of loss (confirmed via serial monitor showing CRC
errors), the most robust structural fix is XOR parity at the fragment level. The transmitter sends
N data fragments plus 1 parity fragment (XOR of all N). The receiver can reconstruct any single
lost fragment:

```
parity_frag = frag[0] XOR frag[1] XOR frag[2] XOR frag[3]
```

For a 4-fragment IP packet this adds one extra transmission (25% overhead) but reduces the
effective packet loss rate from `1-(1-p)^4` to `1-(1-p)^4 - 4*p*(1-p)^3` (tolerates any single
fragment loss). At 39% per-fragment loss rate this drops IP-level packet loss from ~86% to ~44%.
Combined with the frequency move and bitrate reduction (lower per-fragment loss rate), this would
be close to 0%.

This requires a `FRAMING_FLAG_PARITY` bit in the existing framing header byte and changes to both
the transmit loop in `radioTxTask` and the `Reassembler`.
