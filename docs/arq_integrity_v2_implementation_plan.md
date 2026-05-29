# ARQ Integrity v2 Implementation Plan

## Problem Statement

Generic fragmented ARQ can currently deliver a reassembled KISS payload whose length is shorter
than the original transmitted payload. The raw fragmentation test showed received frames such as:

```text
bad_len=251 declared=359
bad_len=416 declared=480
bad_len=418 declared=481
```

Those results mean the receiver emitted a KISS frame after reassembly even though the embedded test
header declared a larger payload. This is a correctness failure in the ARQ/reassembly layer.

The likely mechanism is:

- The receiver tracks fragment presence with `received_mask`.
- `Reassembler::isComplete()` checks only whether all expected fragment indexes are present.
- The DATA header carries per-fragment `payload_len`, but not original frame length or frame CRC.
- `framingParseDataHeader()` accepts any `payload_len <= FRAMING_FRAG_DATA`.
- `finalizeReassembly()` concatenates `frag_len[]` and forwards the result without validating the
  original frame length or content.

ARQ Integrity v2 fixes this class of bug before changing MAC scheduling or ACK arbitration.

## Goals

- Never deliver a truncated reassembly to `rxQueue`.
- Detect corrupted or inconsistent fragment metadata before marking a fragment received.
- Validate the final reassembled payload length and CRC before forwarding it to KISS.
- Preserve selective-repeat ARQ semantics.
- Keep the change focused on one-way correctness before attempting bidirectional MAC changes.

## Non-Goals

- Solving ACK-vs-DATA collisions under bidirectional traffic.
- Piggybacking ACKs onto DATA frames.
- Reducing `RADIO_INTER_FRAG_DELAY_MS`.
- Introducing TDMA or slot timing.

## Wire Protocol v2

The DATA header should be expanded to carry frame-level integrity metadata.

Recommended DATA v2 header:

```text
Byte  0:     [version:4][type:4]       version=2, type=DATA
Byte  1:     flags                     bit 0 = round_end
Byte  2-3:   frame_seq                 uint16
Byte  4:     frag_index
Byte  5:     total_frags
Byte  6:     payload_len               bytes in this fragment
Byte  7-8:   frame_len                 original KISS payload length, uint16
Byte  9-12:  frame_crc32               CRC32 over original KISS payload, uint32
```

Consequences:

| Field | Current | ARQ Integrity v2 |
| --- | ---: | ---: |
| DATA header length | 7 bytes | 13 bytes |
| Fragment payload | 120 bytes | 114 bytes |
| Max payload with 9 fragments | 1080 bytes | 1026 bytes |
| Firmware payload cap | 1024 bytes | unchanged |

The 1024-byte firmware payload cap still fits in the 9-fragment ACK window.

The ACK-only frame may remain unchanged for this phase. The ACK frame can keep version `2` through
the shared version/type byte, but it does not need frame length or CRC fields.

## Validation Rules

The receiver must reject invalid fragments before setting the corresponding bit in
`received_mask`.

Fragment-level validation:

- `frame_len` must be greater than zero and no larger than `TNC_PAYLOAD_MAX_LEN`.
- `total_frags` must equal `ceil(frame_len / FRAMING_FRAG_DATA)`.
- `frag_index` must be less than `total_frags`.
- All fragments for the same `frame_seq` must carry identical `frame_len` and `frame_crc32`.
- Every non-final fragment must have `payload_len == FRAMING_FRAG_DATA`.
- The final fragment must have `payload_len == expected_final_len`, where `expected_final_len` is
  derived from `frame_len`.
- A duplicate fragment with the same index may be ignored if it is consistent with existing frame
  metadata.
- A duplicate fragment with conflicting metadata must increment a malformed-data or identity-reset
  counter and must not update the reassembler.

Final reassembly validation:

- `Reassembler::isComplete()` may still check the bitmap, but completion is not sufficient for
  delivery.
- Before forwarding to `rxQueue`, the receiver must verify `assembled_len == frame_len`.
- Before forwarding to `rxQueue`, the receiver must verify `crc32(assembled_payload) == frame_crc32`.
- If either final check fails, the receiver must drop the reassembly, increment a counter, and avoid
  delivering a corrupt KISS frame.

ACK behavior:

- ACK bitmaps must only include fragments that passed fragment-level validation.
- If a fragment has invalid length or inconsistent frame metadata, it must not be ACKed as received.
- The sender will retransmit unacked fragments in a later ARQ round.

## File-by-File Changes

### `firmware/src/framing/Framing.h`

- Bump `FRAMING_VERSION` from `1` to `2`.
- Set `FRAMING_DATA_HDR_LEN` to `13`.
- Let `FRAMING_FRAG_DATA` continue deriving from `PACKET_MAX_LEN - FRAMING_DATA_HDR_LEN`.
- Add `frame_len` and `frame_crc32` fields to `DataFrameHeader`.
- Update `framingBuildDataPacket()` to include frame length and frame CRC.
- Update `framingParseDataHeader()` to parse the new fields and reject impossible values.
- Add helper functions:
  - `framingExpectedTotalFrags(frame_len)`
  - `framingExpectedFragmentLen(frame_len, frag_index, total_frags)`
  - `framingValidateDataFragment(header)`

### `firmware/src/framing/Crc32.h` or `firmware/src/framing/Framing.h`

- Add a small CRC32 helper for firmware payloads.
- Prefer a local helper over adding a heavy dependency.
- Use the same polynomial as Python `zlib.crc32` only if test tooling compares values directly.

### `firmware/src/main.cpp`

- Extend `Reassembler` state to store:
  - `frame_len`
  - `frame_crc32`
- When a new sequence starts, initialize those fields from the first valid fragment.
- When the same sequence continues, require those fields to match.
- Reject invalid fragment lengths before copying into `ra.buf`.
- In `finalizeReassembly()`, verify final length and CRC before `xQueueSend(rxQueue, ...)`.
- Add counters for final integrity failures and fragment metadata failures.

### `firmware/src/stats/Stats.h`

- Add counters:
  - `arqFragmentMetadataDrops`
  - `arqReassemblyIntegrityDrops`
  - optional `arqFrameCrcErrors`

### `firmware/src/display/Display.cpp`

- If screen space permits, expose at least one integrity-drop counter.
- If screen space is too tight, keep counters available through a future stats/control command.

### Tests

- Update native framing tests for DATA v2 roundtrip.
- Add tests for:
  - non-final fragment with short `payload_len` is rejected
  - final fragment length is derived correctly from `frame_len`
  - mismatched `frame_len` across fragments is rejected
  - mismatched `frame_crc32` across fragments is rejected
  - all bitmap bits set but assembled length too short is not delivered
  - CRC mismatch after reassembly is not delivered

### Tools and Docs

- Update `pi-daemon/raw_fragment_test.py` `FRAG_DATA` to `114`.
- Update `tests/link_validation.py` and `pi-daemon/ping_test.py` fragment constants.
- Update `docs/link_validation_matrix.md` boundaries.
- Update `changelog.md`.

## Implementation Order

1. Add CRC32 helper and native tests for known vectors.
2. Update `Framing.h` DATA header and build/parse tests.
3. Update reassembler metadata state and fragment-level validation.
4. Add final length/CRC validation before delivery.
5. Update stats counters.
6. Run `pio test -e native`.
7. Build firmware with `pio run`.
8. Flash both nodes.
9. Re-run `raw_fragment_test.py` one-way tests.
10. Update host tools and docs after the firmware behavior is confirmed.

## Detailed TODO

### Preparation

- [ ] Capture the current failing `raw_fragment_test.py` output in a dated note or report.
- [ ] Confirm both test boards are flashed from the same commit before firmware changes.
- [ ] Confirm the active transport mode is `GENERIC_FRAGMENTED` on both boards.
- [ ] Confirm `FRAMING_FRAG_DATA` is currently `120` and document the baseline boundary sizes.
- [ ] Run `pio test -e native` before changes to establish a clean baseline.
- [ ] Run `pio run` before changes to establish a clean firmware build baseline.

### CRC32 Helper

- [ ] Decide where the helper lives: `firmware/src/framing/Crc32.h` or `Framing.h`.
- [ ] Implement a small CRC32 function for byte buffers.
- [ ] Use a fixed, documented CRC32 variant.
- [ ] Add native unit tests for empty payload, short payload, and a 1024-byte payload.
- [ ] If matching Python `zlib.crc32`, add a test vector generated from Python.
- [ ] Keep the helper allocation-free and safe for repeated use in radio tasks.

### Wire Format

- [ ] Bump `FRAMING_VERSION` from `1` to `2`.
- [ ] Change `FRAMING_DATA_HDR_LEN` from `7` to `13`.
- [ ] Verify `FRAMING_FRAG_DATA` derives to `114`.
- [ ] Verify `FRAMING_MAX_FRAGS` remains `9` for `TNC_PAYLOAD_MAX_LEN = 1024`.
- [ ] Add `frame_len` to `DataFrameHeader`.
- [ ] Add `frame_crc32` to `DataFrameHeader`.
- [ ] Update the DATA packet byte-layout comment.
- [ ] Keep ACK packet format unchanged except for the shared version nibble.
- [ ] Update `framingBuildDataPacket()` signature to accept `frame_len` and `frame_crc32`.
- [ ] Update `framingParseDataHeader()` to parse `frame_len` and `frame_crc32`.
- [ ] Reject DATA headers where `frame_len == 0`.
- [ ] Reject DATA headers where `frame_len > TNC_PAYLOAD_MAX_LEN`.
- [ ] Reject DATA headers where `total_frags != ceil(frame_len / FRAMING_FRAG_DATA)`.
- [ ] Reject DATA headers where `frag_index >= total_frags`.
- [ ] Reject DATA headers where `payload_len` is not the expected length for that fragment.
- [ ] Add helper `framingExpectedTotalFrags(frame_len)`.
- [ ] Add helper `framingExpectedFragmentLen(frame_len, frag_index, total_frags)`.
- [ ] Add helper `framingValidateDataFragment(header)`.

### Sender Path

- [ ] Compute `frame_crc32` once per outbound `PayloadFrame`.
- [ ] Compute `total_frags` from `frame.len` and the new `FRAMING_FRAG_DATA`.
- [ ] Pass `frame.len` and `frame_crc32` into every DATA fragment builder call.
- [ ] Verify the final fragment uses the exact remaining byte count.
- [ ] Verify all non-final fragments use exactly `FRAMING_FRAG_DATA` bytes.
- [ ] Update ARQ logging to include `frame_len` and optionally `frame_crc32`.
- [ ] Ensure ACK timeout scaling uses the new fragment count.
- [ ] Ensure reassembly timeout scaling uses the new fragment count.

### Receiver Metadata State

- [ ] Extend `Reassembler` with `frame_len`.
- [ ] Extend `Reassembler` with `frame_crc32`.
- [ ] Reset the new fields in `Reassembler::reset()`.
- [ ] Initialize `frame_len` and `frame_crc32` when accepting the first fragment of a sequence.
- [ ] Require subsequent fragments for the same sequence to match stored `frame_len`.
- [ ] Require subsequent fragments for the same sequence to match stored `frame_crc32`.
- [ ] Treat mismatched metadata as a malformed fragment or identity reset.
- [ ] Do not copy mismatched fragments into `ra.buf`.
- [ ] Do not set the received bit for mismatched fragments.
- [ ] Do not update `ack_pending` for rejected fragments.
- [ ] Do not update `last_tick_ms` for rejected fragments.
- [ ] Keep duplicate valid fragments idempotent.
- [ ] Treat duplicate fragments with conflicting metadata as errors.

### Receiver Fragment Validation

- [ ] Validate parsed DATA header before calculating the fragment bit.
- [ ] Reject any non-final fragment whose `payload_len != FRAMING_FRAG_DATA`.
- [ ] Reject any final fragment whose `payload_len` does not match the expected remainder.
- [ ] Reject impossible `total_frags` values before touching reassembler state.
- [ ] Reject fragments whose copy range would exceed `TNC_PAYLOAD_MAX_LEN`.
- [ ] Increment a metadata-drop counter for rejected fragment metadata.
- [ ] Ensure rejected fragments are absent from ACK bitmaps.
- [ ] Keep existing malformed packet counters for parse-level failures.

### Final Reassembly Validation

- [ ] Compute `assembled_len` while concatenating fragments.
- [ ] Verify `assembled_len == ra.frame_len`.
- [ ] Compute CRC32 over the assembled payload.
- [ ] Verify computed CRC equals `ra.frame_crc32`.
- [ ] If length check fails, drop the frame and increment `arqReassemblyIntegrityDrops`.
- [ ] If CRC check fails, drop the frame and increment `arqFrameCrcErrors`.
- [ ] Never call `xQueueSend(rxQueue, ...)` for a failed integrity check.
- [ ] Decide whether to cache failed completions for duplicate suppression; default should be no.
- [ ] Ensure successful completion still updates `CompletedFrameCache`.
- [ ] Update ARQ completion logs with final length and CRC result.

### ACK Semantics

- [ ] Confirm ACK bitmap includes only validated fragments.
- [ ] Confirm a rejected final fragment causes the sender to retransmit that fragment.
- [ ] Confirm a rejected middle fragment causes the sender to retransmit that fragment.
- [ ] Confirm duplicate valid fragments still produce a useful ACK.
- [ ] Confirm duplicate invalid fragments do not advance the received mask.
- [ ] Leave ACK-only wire format unchanged for this phase.

### Stats and Diagnostics

- [ ] Add `arqFragmentMetadataDrops` to `Stats`.
- [ ] Add `arqReassemblyIntegrityDrops` to `Stats`.
- [ ] Add `arqFrameCrcErrors` to `Stats`.
- [ ] Add helper functions to increment the new counters.
- [ ] Update OLED display only if there is enough space to do so cleanly.
- [ ] Consider adding the new counters to a future machine-readable stats command.
- [ ] Ensure `lastPacketLength` remains useful for diagnosing radio packet lengths.

### Unit Tests

- [ ] Update existing DATA packet roundtrip tests for header v2.
- [ ] Update ACK packet roundtrip tests for version v2.
- [ ] Test `framingExpectedTotalFrags()` at lengths `1`, `114`, `115`, `228`, `1024`.
- [ ] Test final fragment expected length at exact and non-exact boundaries.
- [ ] Test build/parse roundtrip for 1-fragment payload.
- [ ] Test build/parse roundtrip for multi-fragment payload.
- [ ] Test non-final short fragment rejection.
- [ ] Test final fragment oversized rejection.
- [ ] Test mismatched `total_frags` rejection.
- [ ] Test zero `frame_len` rejection.
- [ ] Test oversized `frame_len` rejection.
- [ ] Test mismatched metadata across fragments in a reassembly helper if factored out.
- [ ] Test all bitmap bits set but assembled length short is not delivered if testable on host.
- [ ] Test reassembled CRC mismatch is not delivered if testable on host.

### Host Tools

- [ ] Update `pi-daemon/raw_fragment_test.py` `FRAG_DATA` from `120` to `114`.
- [ ] Update default raw test sizes around the new boundaries.
- [ ] Update `tests/link_validation.py` fragment constant.
- [ ] Update `pi-daemon/ping_test.py` fragment constant.
- [ ] Check Rust host tools for any hard-coded fragment assumptions.
- [ ] Update any MTU examples that assume 120-byte fragment payloads.

### Documentation

- [ ] Update `docs/link_validation_matrix.md` packet size reference.
- [ ] Update `docs/general_purpose_kiss_tnc.md` if it mentions fragment size or wire version.
- [ ] Update `README.md` if it mentions outdated MTU/header details.
- [ ] Update `changelog.md` with ARQ Integrity v2 behavior.
- [ ] Document that both nodes must be flashed together for version 2 DATA frames.

### Build and Bench Validation

- [ ] Run `pio test -e native`.
- [ ] Run `pio run`.
- [ ] Flash both nodes.
- [ ] Reset persisted modem config if needed so both nodes use the same profile.
- [ ] Run one-way raw default sweep with `raw_fragment_test.py`.
- [ ] Run one-way raw `--sizes 480 --count 20 --interval-ms 1000`.
- [ ] Run reverse-direction one-way raw test.
- [ ] Confirm `bad=0` in both directions.
- [ ] Record missing sequence counts separately from bad payload counts.
- [ ] Check OLED or stats counters for metadata drops and integrity drops.
- [ ] Only after `bad=0`, run ping/TUN tests to characterize remaining MAC contention.

### Stop Conditions

- [ ] Stop and investigate if any malformed payload is delivered to the listener.
- [ ] Stop and investigate if ACKs include rejected fragments.
- [ ] Stop and investigate if `FRAMING_MAX_FRAGS` exceeds the ACK bitmap capacity.
- [ ] Stop and investigate if the final fragment length calculation differs between sender and receiver.
- [ ] Stop and investigate if one-way raw delivery still produces `bad_len=... declared=...`.

## Acceptance Criteria

One-way raw fragmentation must not deliver corrupt or truncated payloads.

Minimum bench validation:

```sh
# receiver
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 listen --idle-timeout 20 --expected 60

# sender
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 send --count 5 --interval-ms 250
```

Pass criteria:

- `bad=0`
- no `bad_len=... declared=...` entries
- no corrupt KISS payload is emitted
- any remaining packet loss appears as missing sequence numbers, not truncated delivered frames

Stress validation:

```sh
# receiver
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 listen --idle-timeout 20 --expected 20

# sender
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 send --sizes 480 --count 20 --interval-ms 1000
```

Pass criteria:

- `bad=0`
- valid count should be high on a quiet bench
- any loss must be visible as missing frames, not malformed delivered frames

## Risks

- Header expansion reduces fragment payload from 120 to 114 bytes.
- All nodes must be flashed together because the DATA wire format changes.
- CRC32 adds some CPU work, but payloads are capped at 1024 bytes and this is acceptable for
  correctness.

## Recommendation

Implement this phase before any MAC arbiter or piggybacked ACK work. It directly targets the
observed one-way raw fragmentation failure and prevents silent payload corruption from reaching the
host.
