# MAC v2 Planning Index

The observed Generic-mode failures are two separate problems and should be fixed in separate phases.

## Phase 1: ARQ Integrity v2

Read: `docs/arq_integrity_v2_implementation_plan.md`

Purpose:

- Prevent delivery of truncated or corrupted reassembled KISS payloads.
- Add original frame length and frame CRC to the fragmented DATA wire format.
- Reject malformed fragments before they enter the reassembly bitmap.
- Verify final reassembly before forwarding to `rxQueue`.

This phase directly addresses the `raw_fragment_test.py` result where frames such as `declared=480`
were delivered with shorter received lengths.

## Phase 2: MAC Arbiter v2

Read: `docs/mac_arbiter_v2_implementation_plan.md`

Purpose:

- Make `radioTxTask` the only task that calls `radio.transmit()`.
- Queue ACK requests from `radioRxTask` instead of transmitting from the RX path.
- Add optional ACK piggybacking onto outbound DATA frames.
- Reduce ACK-vs-DATA collision collapse during bidirectional traffic.

This phase addresses the separate half-duplex ownership problem seen with ping/TUN and other
bidirectional traffic.

## Phase 3: Idle Heartbeat Control

Read: `docs/idle_heartbeat_control_plan.md`

Purpose:

- Add link-layer `HEARTBEAT` and `DATA_PENDING` control packets.
- Prime the full bidirectional TX/RX/ACK turnaround path before the first multi-fragment ARQ burst
  after idle.
- Avoid using real host payloads as accidental link primers.
- Keep control traffic below the KISS host interface.

This phase addresses the confirmed failure mode where the first multi-fragment frame after idle is
unreliable unless a small one-fragment payload is sent first.

## Recommended Order

Implement ARQ Integrity v2 first. It fixes a correctness bug that can corrupt one-way raw payload
delivery. Implement Idle Heartbeat Control next if large-only fragmented tests still need a manual
small primer. Implement MAC Arbiter v2 when bidirectional DATA/ACK contention becomes the dominant
remaining problem.
