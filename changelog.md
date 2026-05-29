# Changelog

## 2026-05-29

### Frequency scan (SCAN command)

- Added `Radio::scanBand()` which sweeps a frequency range, dwelling `dwellUs` at each step, and
  returns instantaneous RSSI samples. Holds the SPI mutex across the entire sweep and restores the
  original modem config on return.
- Added `SX1280Ext` subclass of `SX1280` exposing `getInstantRssi()` via direct SPI call to
  `CMD_GET_RSSI_INST` (0x1F). RadioLib's `getRSSI()` reads last-packet status and returns 0
  between packets; the instantaneous register is updated by the AGC continuously while in RX mode.
- Fixed `isChannelBusy()` to use `getInstantRssi()` instead of `getRSSI()` for the same reason.
- Added firmware `SCAN` control command: `SCAN start=<MHz> stop=<MHz> step=<MHz> dwell=<µs>`.
  Response: `OK SCAN start=... stop=... step=... n=... best=... rssi=v0,v1,...`
- Added `--scan` flag to `modem_tui.py` with ASCII bar chart output and `<-- best` annotation.
- Increased `--timeout` default in `modem_tui.py` from 2 s to 30 s (full 100-step scan ≈ 200 ms).

### Performance fixes

- **LBT skip when disabled**: wrapped the Listen-Before-Talk retry loop in
  `if (modemConfig.lbtRssiThresholdDbm != 0)` so zero-threshold configs avoid ~1.2 ms of
  unnecessary SPI overhead per TX attempt. Median RTT dropped from ~15 ms to ~12.6 ms.
- **`serialTxTask` drop timeout**: increased from 50 ms to 500 ms. The 50 ms limit caused silent
  frame drops when USB CDC back-pressure briefly stalled `Serial.write()` at high bitrates,
  accounting for the ~44% loss rate observed at 650K CR2.
- **Native-mode ARQ guard**: added an early `continue` in `radioRxTask` so DATA-type ARQ packets
  received while in native transport mode are discarded instead of running through the ARQ state
  machine, which would corrupt sequence counters and trigger spurious ACKs.

### Benchmark results

Best operating point (1300K, CR4, 116-byte payload, 500 frames): **99.4% delivery, 12.6 ms median
RTT, 67.2 kbps sequential effective throughput**. Theoretical one-way max ≈ 720 kbps. See
`docs/bench_results_20260529.md` for full data.

## 2026-05-26

### Link protocol hardening

- Replaced the old 4-bit on-air frame sequence with a 16-bit frame sequence number.
- Expanded the FLRC link header from 3 bytes to 4 bytes to carry the wider frame identity cleanly.
- Reduced `IP_MTU` from `496` to `492` bytes to match the new fragment payload size.
- Kept bitmap ACK selective-repeat ARQ, ACK padding, and duplicate-delivery suppression.

### ARQ stability fixes

- Preserved the duplicate-fragment idempotence fix so retransmitted fragments do not reset fallback timing unless they add new data.
- Preserved the stack-safety fix in the duplicate re-ACK path by avoiding large temporary reassembler allocations on the RX task stack.
- Added wider ARQ counters for:
  - frames started/completed/failed
  - ACK TX/RX and ACK TX errors
  - duplicate suppression
  - queue drops
  - identity resets
- Reworked sender/receiver timing helpers so ACK and reassembly windows scale with fragment count.
- Added a short ACK turnaround delay and increased the sender ACK timeout base to reduce missed ACKs after longer bursts.

### Host tooling

- Updated both host bridges to default to MTU `492`.
- Updated `ping_test.py` to the current `123`-byte fragment payload and `492`-byte IP MTU.
- Corrected the default ping-size ladder so fragment-count labels match the current wire format.

### Documentation

- Updated the README to document the 16-bit frame sequence number, 4-byte fragment header, and MTU `492`.

### Host-path debugging and cleanup

- Removed the temporary KISS telemetry/control-port path again to keep the USB CDC/KISS stream data-only.
- Added host-side IPv4 packet inspection logging in `kiss_tun.py` so injected and transmitted packets now show:
  - IPv4 total length
  - IP ID
  - protocol
  - fragmentation flags and offset
  - ICMP type/code when applicable
- Hardened firmware USB CDC transmit handling so KISS frames are written in a loop until fully drained.
- Added an IPv4 completeness check before forwarding reassembled frames to the USB/KISS path, so malformed short frames are dropped in firmware instead of being injected into `tun0`.
