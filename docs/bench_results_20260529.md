# Benchmark Results — 2026-05-29

Hardware: 2× Lilygo T3S3 SX1280, separated >1 m, 2.4 GHz band (indoor, WiFi/BT environment).  
Tool: `pi-daemon/kiss_bench.py` (PING/PONG RTT and delivery test, native transport mode).

## Configuration under test

| Parameter      | Value        |
|---------------|-------------|
| Bitrate        | 1300 kbps    |
| Coding rate    | CR4 (uncoded)|
| Frequency      | 2450 MHz     |
| TX power       | 0 dBm (default) |
| Transport      | Native (single 125-byte packet) |
| LBT            | Disabled     |
| Payload size   | 116 bytes    |
| Frames tested  | 500          |

## Sequential ping-pong results

```
Delivered   497/500  (99.4%)
Lost        3/500    (0.6%)
RTT min/mean/median/p95/max   11.1 / 13.8 / 12.6 / 22.3 / 102.4  ms
Eff. tput   67.2 kbps  (45724 bytes in 5.44s, including losses and timeouts)
```

## Theoretical limits

| Metric | Value |
|--------|-------|
| One-way TX time (125 B at 1300K CR4) | ~0.78 ms |
| Round-trip lower bound (2× TX + processing) | ~2–3 ms |
| Theoretical one-way throughput (125 B/packet) | ~720 kbps |
| Sequential benchmark ceiling (RTT-limited) | ~70 kbps at 11 ms RTT |

The sequential benchmark is RTT-bound. For real IP traffic the kernel TCP stack pipelines
multiple segments in flight simultaneously, so effective throughput over a real link is
significantly higher than the `67.2 kbps` figure above.

## Comparison across bitrate/CR combinations

| Bitrate | CR  | Delivery | RTT median | Eff. kbps |
|---------|-----|----------|-----------|-----------|
| 650K    | CR2 | ~56%     | —         | —         |
| 325K    | CR2 | 95%      | ~35 ms    | ~20 kbps  |
| 1300K   | CR4 | 99.4%    | 12.6 ms   | 67.2 kbps |

Notes:
- 650K CR2 failures were caused by the 50 ms `serialTxTask` drop timeout (fixed → 500 ms).
- 325K CR2 exhibited ~2 s burst gaps attributed to USB CDC backpressure, resolved by the same fix.
- 1300K CR4 is the recommended operating point for this hardware pair.
