# WP-B Phase 6 MAC Integration Notes - 2026-07-07

Phase 6 binds the native WP-B v3 ARQ engine into the firmware MAC task. This
is the first checkpoint where flashed boards exercise v3 framing in generic
transport mode.

## Integration Summary

- `macTask` remains the sole owner of radio TX/RX state APIs.
- `TransportMode::GENERIC_FRAGMENTED` now routes RF DATA/ACK traffic through
  `framing_v3` and `arq::ArqEngine`.
- Legacy v2 RF traffic is not accepted in generic mode. Version mismatch and
  unknown v3 packet types are silently discarded with counters.
- `TransportMode::NATIVE_PACKET` is retained as a debug path. It continues to
  use the existing single-packet native framing and does not participate in v3
  ARQ.
- v3 CONTROL heartbeat and heartbeat-ack packets replace the old generic-mode
  heartbeat on RF. Reserved CONTROL subtypes remain parser-visible only.
- ARQ egress capacity is bound to `rxQueue` free space. When host delivery is
  blocked, the engine withdraws credits and withholds final ACK.
- MAC scheduling uses `ArqEngine::nextDeadline()` so retry waits do not spin the
  MAC loop.
- LBT remains inside the MAC-owned radio callback before v3 DATA transmission.
- `TNC_PAYLOAD_MAX_LEN` is now 1280. Serial integrity wrapping is still used on
  the USB/KISS host path for generic mode.
- The serial TX integrity wrapper buffer was moved from task stack storage to
  static storage.

## Observability

`STATS` now includes the v3/ARQ counter block:

- `v3VerDrop`
- `v3TypeDrop`
- `arqV3Retry`
- `arqV3Sat`
- `arqV3Bad`
- `arqV3Credit`
- `arqV3Alloc`
- `arqV3TxDone`

Existing counters such as `egress`, `arqDone`, `arqMetaDrop`, and
`arqDuplicateSuppressed` remain available.

## Verification

Commands run from `firmware/`:

```sh
pio test -e native
pio run -e t3s3
pio run -e t3s3-serial-wdt
```

Results:

- `pio test -e native`: 70/70 passed.
- `pio run -e t3s3`: passed.
- `pio run -e t3s3-serial-wdt`: passed.

## Next Step

Run the Phase 8 two-board hardware regression before treating WP-B as closed:
flash both nodes with the same image, kill `kiss_tun.py` before flashing, then
run the 1280-byte burst, induced-loss, egress-blockage, and STATS/SET control
checks.
