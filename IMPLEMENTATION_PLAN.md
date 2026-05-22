# Implementation Plan: Phase 2 — Ultra-Reliability & Maximum-Range Link Optimization

We have successfully stabilized the FLRC link by resolving the critical spurious interrupt deafness and implementing the noise-immune high-entropy sync word `{ 0x7E, 0xC5, 0xA2, 0x3D }`. 

Based on your design preference to prioritize **absolute reliability (0% packet loss) and maximum range** over low latency, we have completely revised our Phase 2 plan. Instead of pushing for maximum throughput, we will tune the FLRC physical layer for maximum link budget, extreme error correction, and robust timing margins.

This plan details three key optimizations to achieve a bulletproof, 0% packet loss link at maximum possible range.

---

## Proposed Reliability Optimizations

### 1. Robust Coding Rate (CR 1/2) for Maximum Error Correction
* **The Concept**: We are currently using `CR 3/4` (Coding Rate 3/4) which provides moderate error correction. 
* **The Optimization**: Change `RADIO_CODING_RATE` in `config.h` to `2` (`CR 1/2`).
* **The Impact**: 
  * This enables the SX1280 FLRC hardware's **most robust forward error correction (FEC)**.
  * It doubles the mathematical correction bits sent, allowing the receiver to reconstruct packets even when multiple bits are corrupted by range path-loss or brief local interference.

### 2. Decrease Bitrate to 325 kbps for +3.5 dBm Sensitivity (Huge Range/Reliability Boost)
* **The Concept**: Receiver sensitivity is directly tied to the modulation bandwidth and data rate. A lower bitrate requires less signal-to-noise ratio (SNR) to decode successfully.
* **The Optimization**: Reduce `RADIO_BITRATE_KBPS` from `650.0f` to `325.0f`.
* **The Impact**:
  * **Improves receiver sensitivity by approximately +3.5 dBm** compared to 650 kbps, and **+6.5 dBm** compared to 1300 kbps.
  * In RF terms, a +3 dBm improvement effectively **doubles the signal strength** or significantly increases wall/obstacle penetration and maximum range.
  * Provides a massive safety margin against deep signal fades.

### 3. Highly Bounded 8 ms Inter-Fragment Pacing
* **The Concept**: With a lower bitrate, the physical airtime of each fragment increases (a 127-byte fragment takes ~3.1 ms at 325 kbps). 
* **The Optimization**: Set the inter-fragment delay `RADIO_INTER_FRAG_DELAY_US` to `8000` (8 ms).
* **The Impact**:
  * This generous pacing window guarantees that the receiver has more than enough time to process the fragment, write it to the serial hardware buffer, and return to clean RX mode before the next fragment starts.
  * It completely eliminates any risk of serial buffer overrun or CPU scheduling delays on the host computer or the Lilygo's FreeRTOS queues.

---

## Proposed Changes

### [MODIFY] [config.h](file:///home/l33chy/Documents/Projects/sx1280-flrc-kiss-tnc/firmware/src/config.h)
We will tune the modem parameters for max-reliability performance:

```diff
- #define RADIO_BITRATE_KBPS      650.0f
+ #define RADIO_BITRATE_KBPS      325.0f

- #define RADIO_CODING_RATE       3
+ // Configure CR 1/2 for maximum error correction robustness
+ #define RADIO_CODING_RATE       2

- #define RADIO_INTER_FRAG_DELAY_US    5000  // 5 ms gap between consecutive fragments
+ #define RADIO_INTER_FRAG_DELAY_US    8000  // 8 ms gap between consecutive fragments
```

---

## Verification Plan

### 1. Automated Verification
* Compile the firmware using `pio run` to verify that RadioLib accepts the 325 kbps bitrate and CR 1/2 parameters.

### 2. Manual Stability Diagnostics
1. Flash both boards with the ultra-reliability firmware:
   ```bash
   pio run -t upload
   ```
2. Start the Rust `kiss-tun-rs` daemons on both endpoints.
3. Test link stability with a high-volume fragmented ping test:
   ```bash
   ping -s 400 -c 200 10.0.0.2
   ```
   *Expected Outcome*: Flawless **0% packet loss** across 200 packets with an extremely stable, uniform latency curve (RTT ~110 ms, but highly predictable with no spikes or drops).
4. Distance & Obstacle Test: Verify that the link remains 100% reliable even when placing walls, floors, or additional distance between the two nodes, thanks to the +3.5 dBm sensitivity gain.
