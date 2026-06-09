# MAC v2 Planning Index

The observed Generic-mode failures are two separate problems and should be fixed in separate phases.

## Status

| Phase | State | Landed in |
| --- | --- | --- |
| Phase 1 — ARQ Integrity v2 | ✅ done | `1d24a57 feat: add ARQ integrity v2 framing`, `99b7aab fix: harden ARQ integrity handling` |
| Phase 3 — Idle Heartbeat Control | ✅ done | `ac72bc2 feat: implement idle heartbeat and DATA_PENDING/DATA_READY link control`, `2358150 fix: tighten link control readiness handling`, `4c74b05 fix: use ARQ warmup for idle fragmented bursts` |
| Phase 2 — MAC Arbiter v2 | ⏳ next | — |

Phases 1 and 3 were implemented out of the original recommended order. Phase 1 is correctness; the
acceptance gate is a clean `raw_fragment_test.py` one-way sweep (`bad=0`, any loss visible only as
missing sequence numbers).

## Phase 1: ARQ Integrity v2 ✅

Read: `docs/arq_integrity_v2_implementation_plan.md`

Purpose:

- Prevent delivery of truncated or corrupted reassembled KISS payloads.
- Add original frame length and frame CRC to the fragmented DATA wire format.
- Reject malformed fragments before they enter the reassembly bitmap.
- Verify final reassembly before forwarding to `rxQueue`.

This phase directly addresses the `raw_fragment_test.py` result where frames such as `declared=480`
were delivered with shorter received lengths.

## Phase 2: MAC Arbiter v2 ⏳

Read: `docs/mac_arbiter_v2_implementation_plan.md`

Purpose:

- Make `radioTxTask` the only task that calls `radio.transmit()`.
- Queue ACK requests from `radioRxTask` instead of transmitting from the RX path.
- Add optional ACK piggybacking onto outbound DATA frames.
- Reduce ACK-vs-DATA collision collapse during bidirectional traffic.

This phase addresses the separate half-duplex ownership problem seen with ping/TUN and other
bidirectional traffic.

## Phase 3: Idle Heartbeat Control ✅

Read: `docs/idle_heartbeat_control_plan.md`

Purpose:

- Add link-layer `HEARTBEAT` and `DATA_PENDING` control packets.
- Prime the full bidirectional TX/RX/ACK turnaround path before the first multi-fragment ARQ burst
  after idle.
- Avoid using real host payloads as accidental link primers.
- Keep control traffic below the KISS host interface.

This phase addresses the confirmed failure mode where the first multi-fragment frame after idle is
unreliable unless a small one-fragment payload is sent first.

## Recommended Order (historical)

Original recommendation was ARQ Integrity v2 first (correctness), Idle Heartbeat Control next (idle
priming), MAC Arbiter v2 last (bidirectional collisions). Actual order followed the first two; only
MAC Arbiter v2 remains.
