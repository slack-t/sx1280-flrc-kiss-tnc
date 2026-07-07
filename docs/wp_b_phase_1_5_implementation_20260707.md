# WP-B Phase 1-5 Implementation Notes - 2026-07-07

This documents the native-testable WP-B implementation work completed before
MAC integration. Hardware behavior is unchanged at this checkpoint because
`Mac.cpp` still uses the v2 framing/ARQ path.

## Scope Completed

### Phase 1 - v3 Wire Format

- Added `firmware/src/framing/FramingV3.h`.
- Defined wire-incompatible v3 packet types: `DATA`, `ACK`, `CONTROL`, `MGMT`.
- Implemented explicit little-endian serializers/parsers with no struct casts.
- DATA uses a 12-byte header and leaves 115 bytes for FLRC fragment payloads.
- Datagram limit is 1280 bytes with a maximum of 12 fragments.
- Parsers return distinct result codes for version mismatch, unknown type,
  truncation, bad length, bad fragment metadata, bad value, bad CRC, and output
  buffer exhaustion.
- All packet types carry CRC32 and verify CRC after bounds and metadata checks.

### Phase 2 - Fragmentation And Reassembly

- Added `firmware/src/framing/FragmentV3.h`.
- Added caller-owned fragment descriptors for datagrams up to 1280 bytes.
- Added `ReassemblyV3` with datagram metadata, 16-bit received bitmap,
  per-fragment lengths, and caller-provided output storage.
- Reassembly rejects mismatched duplicate metadata and counts duplicates and
  rejected fragments without allocating or owning buffers.

### Phase 3 - ARQ Support

- Added `firmware/src/arq/DatagramPool.h`.
- Added a fixed 8 x 1280-byte datagram pool with `acquireDatagram()` and
  `releaseDatagram()`.
- Added allocation-failure and invalid-release counters.
- Added a 64-entry duplicate window with 16-bit wraparound-aware serial
  comparison.
- Duplicate-window entries retain final ACK bitmap, receiver credits, and
  failure status for duplicate re-ACK without re-delivery.

### Phase 4 - Native ARQ Engine

- Added `firmware/src/arq/ArqEngine.h` and `firmware/src/arq/ArqEngine.cpp`.
- Implemented a pure, event-driven ARQ engine with caller-provided callbacks for
  packet send, datagram delivery, and egress capacity.
- Uses fixed internal storage only; no heap allocation.
- Prioritizes ACKs before retransmits and retransmits before opening new DATA.
- Implements retry deadlines in caller-supplied cycles and max-attempt retry
  exhaustion with buffer release.
- RX side uses pool-backed reassembly and duplicate-window suppression.
- Egress blockage withdraws credits and withholds final ACK, so blocked host
  delivery does not become acknowledged loss.
- Until WP-C adds TDD scheduling, the native engine conservatively opens one
  datagram at a time while still buffering up to four TX datagrams. This avoids
  final-ACK starvation in the simulated ACK-vs-DATA collision case.

### Phase 5 - Parser Fuzz Corpus

- Added `firmware/test/test_fuzz_v3/test_fuzz_v3.cpp`.
- Added crafted rejection vectors for each parser rejection class.
- Added deterministic 100,000-input parser fuzzing.
- Added bad-CRC acceptance checks for all packet types.
- Added a live-engine duplicate/reordered fragment fuzz scenario.
- Added `native-asan` PlatformIO environment for ASAN/UBSAN fuzz runs.

## Native Tests Added

- `firmware/test/test_framing_v3/`
- `firmware/test/test_fragment_v3/`
- `firmware/test/test_arq_support/`
- `firmware/test/test_arq_engine/`
- `firmware/test/test_fuzz_v3/`

## Verification

Commands run from `firmware/`:

```sh
pio test -e native
pio test -e native-asan -f test_fuzz_v3
pio run -e t3s3
```

Results:

- `pio test -e native`: 70/70 passed.
- `pio test -e native-asan -f test_fuzz_v3`: 4/4 passed.
- `pio run -e t3s3`: passed.
- `git diff --check`: clean.
- Host-testable WP-B modules contain no Arduino or FreeRTOS includes.

## Deferred

- Phase 6 MAC integration is not complete.
- `STATS` has not yet been extended with v3/ARQ counters.
- No two-board WP-B validation has been run because the new engine is not bound
  to the radio/MAC path yet.
