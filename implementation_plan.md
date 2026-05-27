# Fix KISS Decoder Truncation Before Radio TX

## Summary

The current failure is not in ARQ reassembly. A `369`-byte IP packet is being truncated to `252` bytes before it ever reaches the radio, and the remote node then correctly echoes that shorter packet as `123 + 123 + 6`. The immediate goal is to make the firmware, Python bridge, and Rust bridge use the same strict delimiter-based KISS decoder semantics and prove, with regression tests, that sender-side decoded payload length is preserved exactly.

This plan does **not** change the on-air FLRC protocol, MTU, ARQ framing, or PHY settings.

---

## Symptom Confirmed, Root Cause TBD

### Confirmed symptom

- A host-side `369`-byte IP packet is sometimes decoded into a `252`-byte payload before radio transmission.
- The resulting `252`-byte payload is then fragmented correctly on air as `123 + 123 + 6`.
- Therefore the current defect is definitely in the host-to-firmware serial/KISS decode path, not in radio fragmentation or ARQ reassembly.

### Suspected causes

The exact decoder failure mechanism is still unproven. Plausible candidates include:

- delimiter carry-over / back-to-back `FEND` handling
- `FEND` received while in `ESCAPE`
- non-data port discard behavior perturbing later decode state
- overflow/discard resynchronization
- chunk-boundary handling in one or more decoders

The implementation work below is still valid because it hardens all of these cases, but the code change should not be shipped under a false claim that one specific mechanism is already proven.

---

## Verification Gate Before Decoder Changes

Before changing the decoder implementation, first capture and characterize the exact failing byte-stream pattern.

### Gate objectives

- Reproduce the `369 -> 252` truncation deterministically with a controlled test vector or captured serial stream.
- Classify which decoder state transition causes the truncation.
- Confirm whether the failure reproduces in:
  - firmware decoder only
  - Python decoder only
  - Rust decoder only
  - or multiple decoders

### Required gate outputs

- one reproducer stream or fixture that triggers the truncation
- expected decoded length for that stream
- observed decoded length on current code
- note identifying the first incorrect transition:
  - delimiter carry-over
  - invalid escape handling
  - non-data port discard
  - overflow resync
  - other

Once this reproducer exists, it becomes a permanent regression test and the implementation steps below can proceed.

---

## Required Decoder Semantics

All three decoders must implement the same rules.

### States

- `IDLE`: not currently inside a frame
- `IN_FRAME`: inside a frame, collecting unescaped bytes
- `ESCAPE`: inside a frame, previous byte was `FESC`

### Exact `FEND` behavior

- In `IDLE`:
  - `FEND` opens a new candidate frame
  - clear `_len`
  - clear `_overflow`
  - transition to `IN_FRAME`

- In `IN_FRAME`:
  - if `_len == 0`, this is an empty delimiter while already in-frame
    - clear `_overflow`
    - keep `_len = 0`
    - remain in `IN_FRAME`
    - emit no frame
  - if `_overflow == true`, discard the current frame
    - clear `_len`
    - clear `_overflow`
    - remain in `IN_FRAME`
    - emit no frame
  - if `_len > 0` and port byte is not `KISS_DATA_FRAME`, discard the frame
    - clear `_len`
    - clear `_overflow`
    - remain in `IN_FRAME`
    - emit no frame
  - if `_len > 0` and port byte is `KISS_DATA_FRAME`, emit the payload
    - copy bytes after the port byte into the output frame
    - set output length to `_len - 1`
    - clear `_len`
    - clear `_overflow`
    - remain in `IN_FRAME`
    - the current `FEND` is both the closing delimiter for the frame just emitted and the opening delimiter for the next candidate frame

### Exact `FESC` behavior

- In `IDLE`:
  - ignore it

- In `IN_FRAME`:
  - transition to `ESCAPE`

- In `ESCAPE`:
  - `TFEND` decodes to `FEND` and is appended
  - `TFESC` decodes to `FESC` and is appended
  - any other byte is appended literally
  - then transition back to `IN_FRAME`

### `FEND` while in `ESCAPE`

- Treat it as an invalid escape terminator and discard the partial frame
- clear `_len`
- clear `_overflow`
- transition to `IN_FRAME`
- emit no frame

This preserves resynchronization without requiring a second delimiter to recover.

### Non-data KISS ports

- Accept only `KISS_DATA_FRAME`
- Discard all other KISS ports silently
- Non-data port frames must not perturb subsequent decode alignment

### Overflow behavior

- Once `_overflow` is set, ignore all bytes until the next `FEND`
- The next `FEND` discards the oversized frame and immediately starts a fresh candidate frame

---

## Implementation Changes

### Firmware decoder

#### [MODIFY] `firmware/src/kiss/Kiss.cpp`

- Rewrite `Kiss::decode()` to implement the exact state semantics above.
- Keep encoder behavior unchanged.
- Remove comments that describe “single-FEND back-to-back compatibility” as a special case; the decoder should instead document the explicit delimiter semantics listed above.

#### [MODIFY] `firmware/src/kiss/Kiss.h`

- No interface change required unless helper comments need to be updated.
- Keep `bool decode(uint8_t byte, IpFrame& frame)` as the public API.

### Firmware serial RX instrumentation

#### [MODIFY] `firmware/src/main.cpp`

- Add a compile-time flag:
  - `#define DEBUG_KISS_SERIAL_RX 0`
- In `serialRxTask`, when this flag is enabled and `decoder.decode(...)` returns a frame:
  - print decoded payload length
  - print first 8 payload bytes in hex
- Do not log per-byte activity.
- Keep this disabled by default.
- Do not place any runtime logging on the KISS TX path or radio path.

### Python bridge decoder

#### [MODIFY] `pi-daemon/kiss_tun.py`

- Refactor `KissDecoder.feed()` so its state transitions match firmware exactly.
- Keep `KISS_DATA_PORT` as the only accepted port.
- Continue to drop invalid/non-data frames silently.
- Keep `--debug-ip` behavior unchanged.

### Rust bridge decoder

#### [MODIFY] `pi-daemon-rust/src/kiss.rs`

- Refactor `KissDecoder::feed()` so its state transitions match firmware exactly.
- Keep only port `0x00` accepted.
- Keep current API shape if possible; no host-facing behavior change is required beyond decoder correctness.

---

## Regression Test Requirements

### Shared regression vectors

Create one shared conceptual set of byte-stream scenarios and implement them in:

- `firmware/test/test_kiss/test_kiss.cpp`
- `pi-daemon-rust/src/kiss.rs` tests
- Python decoder tests if the repo has no test harness, add at least one minimal script-level decoder test or keep the Python implementation line-by-line aligned with the firmware semantics

The required scenarios are:

1. Standard full-frame round-trip
2. Escaped `FEND` inside payload
3. Escaped `FESC` inside payload
4. Two complete frames back-to-back using normal KISS framing:
   - `FEND port payload FEND FEND port payload FEND`
5. Consecutive empty delimiters:
   - `FEND FEND FEND`
6. Non-zero port frame followed by a valid data frame
7. Oversized frame followed by a valid frame
8. Split delivery at every possible byte boundary for one valid frame
9. `FEND` received while in `ESCAPE`
10. Exact reproducer for the observed truncation:
   - a `369`-byte payload encoded by the normal KISS encoder
   - delivered with realistic serial chunk boundaries
   - decoded length must remain `369`

### C++ firmware tests

#### [MODIFY] `firmware/test/test_kiss/test_kiss.cpp`

- Replace `test_back_to_back_single_fend()` with a standard double-FEND back-to-back frame test.
- Add `test_fend_inside_escape_discards_frame_and_resyncs()`.
- Add `test_non_zero_port_then_valid_frame_resyncs_cleanly()`.
- Add `test_oversized_frame_then_valid_frame_resyncs_cleanly()`.
- Add `test_369_byte_regression_reproducer()`.

### Rust tests

#### [MODIFY] `pi-daemon-rust/src/kiss.rs`

- Update the current back-to-back test to use full double-FEND boundaries.
- Add the same edge-case tests as the firmware decoder where practical.
- Add a `369`-byte reproducer test.

### Python validation

#### [MODIFY] `pi-daemon/kiss_tun.py`

- If no formal Python test harness exists, add a small local self-check block or keep the decoder logic intentionally parallel to the firmware implementation and validate it manually using a short raw-byte fixture script.
- The Python decoder must be verified against:
  - back-to-back double-FEND frames
  - oversized frame recovery
  - non-zero port discard followed by valid data
  - `369`-byte reproducer

---

## Verification Plan

### Automated verification

Run:

```bash
pio test -e native
cd pi-daemon-rust && cargo test
python3 -m py_compile pi-daemon/kiss_tun.py
```

### Bench verification

1. First satisfy the verification gate and preserve the reproducer as a test.
2. Enable `DEBUG_KISS_SERIAL_RX` on one firmware build only if needed.
3. Send a `369`-byte IP packet from host to firmware:
   - `ping -s 341`
4. Confirm on the sender-side firmware that the decoded KISS payload length is `369`, not `252`.
5. Repeat for:
   - `ping -s 403`
   - `ping -s 464`
6. Confirm that the sender-side fragment lengths now match the original payload geometry:
   - `369` -> `123 + 123 + 123`
   - `492` -> `123 + 123 + 123 + 123`

### Acceptance criteria

The fix is complete when all of the following are true:

- A `369`-byte host packet is decoded as exactly `369` bytes before radio transmission.
- The `369 -> 252` truncation can no longer be reproduced.
- No malformed short final fragment appears unless it is mathematically correct from the original payload length.
- Firmware and Rust KISS tests pass with the updated delimiter semantics.
- End-to-end packet loss for `3F` improves materially relative to the current broken state.

End-to-end `0%` loss is **not** required to accept this decoder fix, because other large-frame issues may still remain after the truncation bug is removed.

---

## Non-Goals

- No FLRC PHY tuning
- No ARQ redesign
- No FEC/parity work
- No host pacing changes
- No new in-band KISS telemetry

This change is only to make KISS framing correct and consistent across the stack.
