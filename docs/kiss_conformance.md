# KISS Conformance Specification

This repository uses a strict, deterministic KISS framing profile for all
firmware and host-side decoders.

## Scope

- Supported frame type: KISS data frame on port `0x00`
- Unsupported frames: all non-zero port/command bytes
- Transport stream requirement: data-only KISS byte stream; no diagnostics or
  side-channel text may share the same serial path

## Encoder Rules

An encoded frame is:

`FEND` `0x00` `escaped payload bytes...` `FEND`

Where:

- `FEND` = `0xC0`
- `FESC` = `0xDB`
- `TFEND` = `0xDC`
- `TFESC` = `0xDD`

Payload escaping rules:

- raw `0xC0` becomes `0xDB 0xDC`
- raw `0xDB` becomes `0xDB 0xDD`
- all other bytes are emitted unchanged

## Decoder State Machine

The decoder has exactly three states:

- `IDLE`
- `IN_FRAME`
- `ESCAPE`

### IDLE

- Ignore all bytes except `FEND`
- On `FEND`: clear partial state, clear overflow state, transition to `IN_FRAME`

### IN_FRAME

- On `FEND`:
  - if no bytes were buffered: discard empty frame, transition to `IDLE`
  - if overflow occurred: discard partial frame, transition to `IDLE`
  - if first buffered byte is not `0x00`: discard frame, transition to `IDLE`
  - otherwise emit payload bytes after the port byte, then transition to `IDLE`
- On `FESC`: transition to `ESCAPE`
- On any other byte:
  - append while buffer length is within `max_len + 1`
  - if the buffer would exceed `max_len + 1`, set overflow and continue
    discarding until closing `FEND`

### ESCAPE

- On `TFEND`: append decoded `FEND`, transition to `IN_FRAME`
- On `TFESC`: append decoded `FESC`, transition to `IN_FRAME`
- On any other byte, including raw `FEND`:
  - treat as invalid escape
  - discard the partial frame
  - clear overflow state
  - transition to `IDLE`

## Resynchronization Rules

- A closing `FEND` never implicitly opens the next frame
- After any emitted frame, discarded frame, invalid escape, or overflow
  discard, the decoder returns to `IDLE`
- A new frame only begins after a fresh `FEND`

## Buffer and Error Rules

- Maximum accepted payload length is implementation-specific but must match the
  configured MTU for that decoder
- Oversized frames must never emit partial payload
- Invalid escape sequences must never emit partial payload
- Unsupported non-zero port frames must never emit payload
- Arbitrary out-of-frame noise must never emit payload

## Cross-Implementation Requirement

Firmware, Python, and Rust decoders must implement identical behavior for the
same input byte stream. Conformance tests should use shared byte-stream vectors
for:

- round-trip payload coverage
- empty frames
- back-to-back frames with double `FEND`
- non-zero port discard and resync
- invalid escape discard and resync
- overflow discard and resync
- arbitrary chunking
- captured regression streams
