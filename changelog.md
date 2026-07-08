# Changelog

## 2026-07-08

### Link-state stability

- Link readiness is now refreshed by data-path traffic, not only heartbeats:
  a valid v3 ACK from the peer and each completed inbound datagram count as
  bidirectional confirmation. The link no longer reports DOWN during active
  transfers.
- Heartbeat scheduling gates on payload idleness instead of general link
  activity, and CONTROL transmissions no longer mark payload activity —
  fixing mutual heartbeat starvation that delayed post-transfer heartbeat
  resume by ~10 s.
- `linkState` now reports READY → PROBING → DOWN with hysteresis (new
  `RADIO_LINK_PROBE_TTL_MS`, 10 s); the OLED ERROR label requires a radio
  error within the last `RADIO_ERROR_HOLD_MS` (3 s) instead of being sticky.
- Verified on both boards: 30 × 1000 B zero-gap burst with a draining host —
  READY across all 18 STATS polls during and after the transfer, 30/30 frames
  delivered clean. Details in `docs/link_state_stability_20260708.md`.
- Fixed the pre-existing ARQ credit-starvation deadlock triggered by a
  completion ACK carrying `receiver_credits=0`: when queued TX work remains and
  nothing is in flight, the sender now arms a delayed zero-credit probe deadline
  and opens one datagram after `RADIO_ARQ_CREDIT_PROBE_MS`. Added native
  regression coverage and surfaced `arqV3Probe` in `STATS`.
- Committed and flashed the fix as `2ba7b54` on both attached boards, then
  reran the two-board 30 x 1280-byte zero-gap hardware gate in both directions.
  Both directions delivered 30/30 unique frames with no corruption,
  duplicates, missing frames, ARQ retry exhaustion, credit withdrawal,
  allocation failure, or probe increments. Details appended to
  `docs/wp_b_bench_20260707.md`.
- Remaining pre-WP-C hardware follow-up: deliberately reproduce host egress
  blockage and confirm the new `arqV3Probe` path unwedges the sender under
  blocked-then-draining receiver conditions. Initial no-read-host attempts did
  not produce a reliable proof harness: one run completed without credit
  pressure, one queried stats before recovery, and one lost the useful counter
  epoch. The next step is a deterministic receiver egress-block diagnostic mode
  or command.
- Added that diagnostic as a KISS control command: `DIAG egress=open`,
  `DIAG egress=blocked`, and `DIAG egress=oneshot`. The one-shot mode allows
  one receiver delivery, then advertises zero credits, which reproduces the
  zero-credit completion ACK needed to exercise the delayed probe branch.
  Hardware run confirmed `arqV3Probe` increments (`arqV3Probe=3` for 4 frames;
  `arqV3Probe=1` for 2 frames with receiver reopened during the sender hold).
  Full post-clear delivery completion is still open: the captured recovery run
  returned `arqV3TxDone=0`, so the next focus is receiver release/completion
  after a one-shot zero-credit ACK.
- Closed the deterministic one-shot recovery issue. Generic-mode `serviceTx()`
  now continues ticking the v3 ARQ engine while waiting for heartbeat ACKs, and
  v3 `CREDIT_WITHDRAWAL` ACKs are treated as receiver backpressure instead of
  packet loss: the sender clears the current round, waits the credit-stall
  interval before probing again, and does not exhaust the slot while the peer is
  actively replying. Added native regression coverage for repeated withdrawal
  ACKs beyond the previous max-attempt failure point. Final held-open hardware
  retest delivered both 1280-byte datagrams after `DIAG egress=open`:
  receiver `rx=2 arqDone=2 ackTx=65 ackTxErr=2 arqV3Credit=18`; sender
  `ackRx=19 arqV3Retry=0 arqV3TxDone=2`.

## 2026-07-07

### WP-B bench tooling

- Raised the Python host-side wrapped payload cap to 1280 bytes and made
  `kiss_tun.py` default its optional TUN MTU to that cap.
- Updated `raw_fragment_test.py` for v3 fragment sizing, 1280-byte default
  sweeps, CRC32/SHA-256 logging, explicit duplicate detection, and a
  `--stress-1280` shortcut for the 30 x 1280-byte zero-gap bench run.
- Added serial-integrity boundary tests and
  `docs/wp_b_phase_7_bench_tooling_20260707.md` with Phase 8 command lines.
- Fixed the firmware KISS decoder capacity for generic-mode max payloads:
  decoded KISS frames can now carry `1280 + 8` bytes for the serial-integrity
  envelope while `PayloadFrame` remains capped at 1280 bytes. Added native
  regression coverage for this wrapped max-frame case.
- Completed the Phase 8 two-board hardware gate. During the gate, fixed v3
  hardware convergence by deferring partial ARQ ACKs to round end, pacing v3 DATA
  sends with the existing inter-fragment delay, increasing the host-to-radio
  queue depth to 32 frames, and chunking USB CDC serial TX writes. Final
  forward and reverse 30 x 1280-byte zero-gap runs delivered 30/30 unique
  frames with no corruption, duplicates, missing frames, serial-integrity drops,
  or ARQ retry-exhaustion counters. Documented results in
  `docs/wp_b_bench_20260707.md`.
- Reran the induced-loss profile at `power=-18` on both boards and confirmed a
  clean 30/30 zero-gap burst with no corruption, duplication, or retry
  exhaustion. Also exercised larger host-blocked and concurrent-transmit pressure
  runs; those are documented in `docs/wp_b_bench_20260707.md` as follow-up
  validation because they did not yet produce a clean receiver-side blocked-
  egress artifact.

### WP-B MAC integration

- Bound the v3 `ArqEngine` into `macTask` for `GENERIC_FRAGMENTED` transport
  while preserving the radio single-owner invariant.
- Generic RF DATA/ACK traffic now uses v3 framing and selective-repeat ARQ;
  legacy v2 RF traffic is silently discarded via version/type counters.
- Mapped generic-mode heartbeat/heartbeat-ack to v3 CONTROL packets and kept
  `NATIVE_PACKET` as the single-packet debug path.
- Raised `TNC_PAYLOAD_MAX_LEN` to 1280 and moved the serial TX integrity wrapper
  buffer out of task stack storage.
- Extended `STATS` with v3/ARQ counters for version/type drops, retry
  exhaustion, saturation, malformed input, credit withdrawal, allocation
  failure, and TX completion.
- Added `docs/wp_b_phase_6_mac_integration_20260707.md`. Verification:
  `pio test -e native`, `pio run -e t3s3`, and
  `pio run -e t3s3-serial-wdt` passed.

### WP-B v3 framing and ARQ native implementation

- Added native-testable v3 framing (`FramingV3.h`) with DATA, ACK, CONTROL, and
  MGMT packet serializers/parsers, explicit little-endian packing, CRC32 checks,
  1280-byte datagrams, and 12-fragment selective-repeat bitmap coverage.
- Added v3 fragmentation/reassembly helpers with caller-owned buffers,
  per-fragment metadata validation, duplicate detection, and bitmap completion
  checks.
- Added ARQ support primitives: fixed 8 x 1280-byte datagram pool and a
  64-entry wraparound-aware duplicate window retaining final ACK/failure state.
- Added a pure native `ArqEngine` with fixed storage, callback-driven sends and
  delivery, credit withdrawal on blocked egress, retry exhaustion handling, and
  duplicate re-ACK without re-delivery.
- Added deterministic native tests for v3 framing, fragmentation, pool/window
  support, lossy half-duplex ARQ simulation, and parser fuzzing. Verification:
  `pio test -e native` passed 70/70, `pio test -e native-asan -f test_fuzz_v3`
  passed 4/4, and `pio run -e t3s3` built successfully.
- Added `docs/wp_b_phase_1_5_implementation_20260707.md` documenting scope,
  verification, and deferred MAC integration.

### WP-A closure on production image

- Fixed a serial write-lock telemetry race: `serialWriteLockHeld` is now
  cleared before `serialWriteMutex` is released, so `stxLock` can no longer
  read back a stale 0 while another writer holds the lock.
- Gated all host-backpressure telemetry (queue depths, write-lock state, TX
  progress/stall age in `STATS` and the OLED bottom row) behind
  `SERIAL_TX_WDT_DIAGNOSTICS`. Production images drop the per-chunk stats-mutex
  traffic on the serial TX hot path; the `t3s3-serial-wdt` env keeps the full
  diagnostic surface. Production `STATS` ends at `hwmStx=`.
- Re-ran the 30x1000B burst on production `t3s3` at `6a2d669`, both nodes: all
  USB backpressure invariants hold (`stxZero=0`, `stxTimeout=0`, `rxQWait=0`,
  `egress=0`), 16/30 payloads delivered with `valid=16 bad=0` and no
  acknowledged loss. WP-A gate closed on the deployable artifact.
- Documented the burst-loss mechanism: heartbeats are a minor aggravator only
  (receiver heartbeats fire solely in >=500 ms gaps and never block data ACKs);
  the dominant cause is half-duplex fragment/ACK collision exhausting the 6 ARQ
  rounds under zero-gap bursts. Designated fix is WP-B/WP-C, not tuning of the
  current protocol. Analysis appended to
  `docs/wp_a_usb_backpressure_20260707.md`.

### WP-A USB backpressure regression

- Added a pure-KISS diagnostic firmware environment (`t3s3-serial-wdt`) that
  keeps USB CDC free of console logs while optionally registering
  `serialTxTask` with the task watchdog during frame drains.
- Extended `STATS` and the OLED status row with host queue and serial TX
  telemetry: queue depths, serial write-lock state, serial TX activity, frame
  offset, and last-progress/stall age.
- Re-ran the 30x1000B burst regression. The board recovered cleanly with
  `qTx=0/8`, `qRx=0/8`, `stxLock=0`, `stxActive=0`, no serial TX zero writes or
  timeouts, and valid receiver egress. The remaining loss is documented as
  MAC/link burst overload rather than USB backpressure wedge.
- Added `docs/wp_a_usb_backpressure_20260707.md` with commands, STATS output,
  interpretation, and deployment notes.

## 2026-05-29

### ARQ Integrity v2

- Expanded generic DATA fragment header from 7 to 13 bytes (`FRAMING_VERSION` 2).
- Added `frame_len` and `frame_crc32` to per-fragment headers to prevent delivery of corrupted/truncated payloads.
- Added strict length and exact-fragment-remainder validation to receiver logic.
- Fragment payloads are reduced from 123 to 114 bytes (MTU 114 * 4 = 456).
- Added tracking counters for metadata drops and integrity drops in Stats.h.

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
