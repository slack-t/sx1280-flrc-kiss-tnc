# MAC Arbiter v2 Implementation Plan

## Problem Statement

The current Generic fragmented ARQ implementation has two local transmit call paths:

- `radioTxTask()` transmits DATA fragments and waits for ACKs.
- `radioRxTask()` transmits ACK packets directly through `sendAckForReassembly()`.

The SPI mutex prevents local SPI corruption, but it does not provide a single local policy for what
gets transmitted next. During bidirectional traffic, this can produce ACK-vs-DATA contention and can
amplify over-the-air collisions between peers.

MAC Arbiter v2 makes one task responsible for all local radio transmissions. This phase should be
implemented after ARQ Integrity v2, because it addresses a different failure mode.

## Goals

- Make `radioTxTask` the only task that calls `radio.transmit()`.
- Remove direct TX calls from `radioRxTask`.
- Preserve selective-repeat ARQ behavior.
- Prioritize ACK delivery over new outbound DATA.
- Optionally piggyback reverse-direction ACK state onto outbound DATA frames.
- Reduce bidirectional collision collapse under ping/TUN traffic.

## Non-Goals

- Detecting truncated reassemblies. That belongs to ARQ Integrity v2.
- Reducing inter-fragment delay.
- Implementing TDMA or strict time slots.
- Solving all peer-to-peer simultaneous DATA-start collisions.

## Required Preconditions

Before this phase starts:

- ARQ Integrity v2 should be implemented and validated.
- One-way raw fragmentation should produce no malformed delivered payloads.
- The DATA header should already include frame length and frame CRC.

## Architecture

`radioTxTask` becomes the sole local transmit arbiter.

New flow:

```text
radioRxTask
  receives DATA fragments
  updates reassembler
  builds ACK request
  enqueues ACK request into txAckQueue

radioTxTask
  chooses next outbound transmission
  sends ACK-only packet, DATA packet, or DATA packet with piggybacked ACK
  remains the only caller of radio.transmit()
```

`radioRxTask` may still parse incoming ACKs and enqueue them into `ackQueue` for the current
outbound transaction. It must not call `radio.transmit()`.

## Queue Model

Existing queues:

- `txQueue`: host payloads waiting for outbound DATA transmission.
- `rxQueue`: validated inbound payloads waiting for KISS/USB delivery.
- `ackQueue`: inbound ACKs consumed by the current outbound ARQ sender.

New queue:

- `txAckQueue`: ACK requests created by RX/reassembly and consumed by `radioTxTask`.

Recommended constant:

```cpp
#define ACK_TX_QUEUE_DEPTH 4
```

ACK queueing should coalesce where possible. If multiple ACK requests exist for the same
`seq,total_frags`, the newest received mask should replace or dominate the older mask. Blindly
dropping ACKs under queue pressure should be avoided because it forces retransmission and can
increase congestion.

## ACK Priority Rules

`radioTxTask` should choose work in this order:

1. Standalone ACK that is already due and cannot be piggybacked quickly.
2. Current in-progress DATA retransmission round.
3. New outbound DATA from `txQueue`.

ACKs should not wait indefinitely for piggyback opportunities.

Recommended initial constant:

```cpp
#define ACK_PIGGYBACK_WAIT_MS 10
```

If an ACK is pending and no DATA arrives within this wait, `radioTxTask` should send an ACK-only
packet.

## Piggybacked ACK Wire Format

Piggybacking requires reverse-direction ACK metadata in DATA frames. This should extend the ARQ
Integrity v2 DATA header rather than replace any integrity fields.

Recommended fields:

```text
has_reverse_ack     1 bit in DATA flags
ack_seq             uint16, 0xFFFF = no piggyback
ack_total_frags     uint8
ack_received_mask   uint16 or uint32
```

Use `uint32` for `ack_received_mask` unless there is a strong reason to shrink it. The current ACK
frame already uses a 32-bit bitmap, and keeping the same type avoids edge-case conversions if
`FRAMING_MAX_FRAGS` changes.

The final DATA header layout should be decided after combining this phase with ARQ Integrity v2.
Do not remove `payload_len`, `frame_len`, or `frame_crc32` to make room for piggyback fields.

## ACK-Only Frame

The existing ACK-only frame should remain supported.

ACK-only frames are required when:

- the node has no outbound DATA to piggyback onto
- the piggyback wait expires
- a duplicate completed frame needs a quick re-ACK
- outbound DATA is blocked behind another transaction

## File-by-File Changes

### `firmware/src/config.h`

- Add `ACK_TX_QUEUE_DEPTH`.
- Add `ACK_PIGGYBACK_WAIT_MS`.
- Do not change `RADIO_INTER_FRAG_DELAY_MS` in this phase.

### `firmware/src/main.cpp`

Setup:

- Add `QueueHandle_t txAckQueue`.
- Create it in `setup()`.
- Include it in queue creation failure checks.

ACK request handling:

- Refactor `sendAckForReassembly()` so it does not call `radio.transmit()`.
- Rename if useful, for example `queueAckForReassembly()`.
- Enqueue `AckFrame` into `txAckQueue`.
- Only clear `ra.ack_pending` after the ACK is accepted into the outbound ACK path.
- If the ACK queue is full, coalesce or replace a compatible older ACK instead of silently dropping.

Duplicate completed frame path:

- Replace direct duplicate re-ACK transmission with a queued ACK request.
- Preserve duplicate-delivery suppression.

Inbound piggyback extraction:

- When `radioRxTask()` receives a DATA frame with reverse ACK metadata, validate the ACK fields.
- Push the extracted `AckFrame` into `ackQueue`.
- Count malformed piggyback ACK metadata separately from malformed DATA if possible.

Transmit scheduler:

- Keep all `radio.transmit()` calls in `radioTxTask`.
- Add a scheduler wrapper that can send ACK-only packets, DATA packets, or DATA-with-piggyback.
- Ensure ACK-only sends return the radio to RX.
- Preserve current selective-repeat round logic for outbound DATA.

### `firmware/src/framing/Framing.h`

- Add piggyback ACK fields to the DATA header produced by ARQ Integrity v2.
- Add builder/parser tests for:
  - DATA without piggyback
  - DATA with piggyback
  - invalid piggyback ACK metadata

### `firmware/src/stats/Stats.h`

Add counters:

- `arqAckQueuedCount`
- `arqAckCoalescedCount`
- `arqAckQueueFullCount`
- `arqPiggybackAckTxCount`
- `arqPiggybackAckRxCount`
- optional `arqMalformedPiggybackAckCount`

### Tests

- Unit-test DATA piggyback fields.
- Unit-test ACK coalescing behavior if it is factored into a helper.
- Add a bench test that runs ping while a low-rate reverse-direction raw or UDP stream is active.

## Implementation Order

1. Add constants and `txAckQueue`.
2. Refactor duplicate re-ACK and normal ACK paths to enqueue ACK requests.
3. Make `radioTxTask` send ACK-only packets from `txAckQueue`.
4. Verify there are no remaining `radio.transmit()` calls outside `radioTxTask` and lower-level
   radio wrappers.
5. Add piggyback ACK fields to DATA frames.
6. Extract piggybacked ACKs into `ackQueue`.
7. Add counters and tests.
8. Run `pio test -e native`.
9. Build firmware with `pio run`.
10. Flash both nodes and run bidirectional validation.

## Acceptance Criteria

Source-level criteria:

- `radioRxTask` does not call `radio.transmit()`.
- Duplicate re-ACK path does not call `radio.transmit()`.
- All firmware-originated ACK transmissions pass through `radioTxTask`.

One-way regression criteria:

- `raw_fragment_test.py` still has `bad=0` after ARQ Integrity v2.
- Delivery rate should not regress significantly on a quiet bench.

Bidirectional criteria:

- Ping/TUN tests should no longer collapse when both sides have traffic.
- `arqAckTimeoutCount` and `arqFramesFailed` should fall compared with the pre-arbiter firmware.
- Piggyback counters should increase during bidirectional traffic if piggybacking is enabled.

Suggested validation:

```sh
# idle one-way raw fragmentation
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 listen --idle-timeout 20 --expected 60
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 send --count 5 --interval-ms 250

# bidirectional IP path after one-way tests pass
python3 tests/link_validation.py 10.0.0.2 --mtu 240 --skip-iperf
```

## Risks

- A single arbiter can become more complex than the current split RX/TX design.
- ACK queue coalescing must be correct or retransmission behavior can become confusing.
- Piggybacking increases DATA header size, reducing payload per fragment.
- This still does not guarantee peer-level medium ownership; two peers can still start DATA bursts
  at the same time. TDMA or a lighter turn-taking rule is needed for deterministic collision
  avoidance.

## Recommendation

Implement MAC Arbiter v2 only after ARQ Integrity v2 is stable. Keep pacing constants unchanged
during this phase so any behavior change can be attributed to transmit arbitration and ACK routing.
