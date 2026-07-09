# Serial Throughput and HWCDC Stability — 2026-07-09

RNS file transfers ran at ~5 KB/s and failed partway through with the
receiving side apparently locked up. One day of investigation found four
stacked problems: an obsolete pacing constant, two distinct defects in the
Arduino-ESP32 2.0.17 HWCDC driver, and a dead display counter. All are fixed
across commits `f7142fe`, `0ef4821`, and `e7f9fc9`. The original workload —
a 550 KB RNS file transfer — now completes at ~7 KB/s with zero lockups.

## 1. Inter-fragment gap dominated airtime (`f7142fe`)

`RADIO_INTER_FRAG_DELAY_MS = 8` dated from the 325 kbps era. At the current
bitrate a 127-byte frame takes ~1 ms of air, so the channel idled ~8 ms for
every 1 ms of transmission — roughly 75 % of a multi-fragment round was
silence. A hardware sweep (echo benches, count=100) measured:

| Gap  | Delivery (512/1015/1271 B) | 1015 B mean RTT | Effective   |
|------|----------------------------|-----------------|-------------|
| 8 ms | 100 %                      | 278 ms          | ~29 kbps    |
| 4 ms | mixed                      | 230 ms          | ~35 kbps    |
| 2 ms | 100 %                      | 188 ms          | ~40–45 kbps |
| 1 ms | collapses under load       | —               | unstable    |

2 ms is the validated floor (~1.5× throughput); at 1 ms the receiver runs out
of RX re-arm headroom under sustained load (fine for 30 frames, 50 % delivery
over 100). The ACK timeout derives from the same constant and scaled
automatically.

## 2. HWCDC RX ingest wedge (`0ef4821`, root-caused in `e7f9fc9`)

Under sustained host→board flood the board stopped draining the USB CDC OUT
direction: the host process blocked forever in `serial.write()` while the
radio side stayed perfectly healthy (link READY, heartbeats flowing, zero
error counters). Deterministic repro: `kiss_bench.py --echo` on one board,
`--sweep --sweep-sizes 64,256 --count 100` on the other — wedged ~120–380
frames in, on both 8 ms and 2 ms firmware (pre-existing, independent of
pacing).

`0ef4821` restructured `serialRxTask` so ingest never stops consuming CDC
data (backlog ring between KISS decode and `txQueue`, watchdog supervision,
`srxReadNeg` anomaly counter, `srx*` STATS telemetry). Its interim
drop-on-overflow policy was later removed — the serial path is lossless
again (`srxDrop` exists as a counter and must stay 0).

## 3. HWCDC TX truncation (`e7f9fc9`)

With ingest fixed, the 400×1280 B one-way repro still failed by ~1 frame in
400: the listener's firmware counted `rx=400 arqDone=400` and clean serial-TX
counters (`stxZero=0 stxTimeout=0 stxEncodeFail=0`), yet the host received a
truncated frame that failed serial-integrity unwrap. Characterization over
five runs: 13 truncated frames across 3 failing runs. The bytes were lost in
the device TX path *after* firmware had cleanly submitted them — a second,
independent defect in the same 2.0.17 HWCDC driver.

**Fix: platform upgraded to pioarduino `espressif32@55.3.39`
(Arduino-ESP32 3.3.9 / ESP-IDF 5.5.4), keeping HWCDC mode
(`ARDUINO_USB_MODE=1`).** Both wedge directions were driver bugs; the 3.x
HWCDC rework fixes both. Note for future maintainers:

- The no-`flush()` rule in `serialTxTask` still applies on core 3.x — the
  flush-timeout path can still clear queued bytes (see comment in
  `main.cpp`).
- Rebuilding any old branch without the new platform pin regresses both
  driver bugs.

## 4. Display TX counter dead in v3 mode (`e7f9fc9`)

The OLED renders `Stats.txCount`, which only the legacy transport paths
incremented. The v3 ARQ engine tracks completions in `arqV3TxCompleted` and
never bumped `txCount`, so the display showed TX:0 forever in the transport
mode actually used. The v3 counter sync in `mac/Mac.cpp` now updates
`txCount` as well.

## Validation

- `pio test -e native`: 79/79 pass (on the new platform).
- 400×1280 B one-way (`raw_fragment_test.py`): 3/3 runs
  `valid_unique=400 bad=0 duplicates=0 missing_in_range=0`, `srxDrop=0`,
  same-session STATS responsive throughout.
- Echo flood 64/256 B count=100 (the RX-wedge repro): 3/3 runs, 100 %
  delivery.
- Large sweep 512/1015/1271 B count=100: 100 % delivery, 1015 B mean RTT
  182.6 ms.
- End-to-end: 550 KB RNS file transfer at ~7 KB/s average, no lockups —
  the workload that originally failed.

## Open leads

- **Throughput ceiling is now protocol, not bugs.** Each ARQ round pays a
  fixed ACK-turnaround cycle and RNS windowing sits on top; radio-side
  ceiling at current settings is ~25 kB/s. Burst scheduling across rounds is
  WP-C adaptive-MAC territory (`docs/flrc_p2p_poc_implementation_plan.md`).
- **linkState DOWN-latch (unconfirmed).** During one wedge cascade a board
  sat in `linkState=DOWN` for ~5 minutes while the peer was READY and
  heartbeat exchange demonstrably worked, recovering only on reset. Likely
  downstream of the (now fixed) wedge, but if a board is ever observed stuck
  in DOWN while its peer is READY, capture STATS from both sides before
  resetting.
