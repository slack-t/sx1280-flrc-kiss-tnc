# Changelog

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
