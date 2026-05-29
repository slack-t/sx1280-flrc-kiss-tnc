# Idle Heartbeat Control Plan

## Status

Draft.

## Problem Statement

Generic fragmented ARQ still has an idle-start failure mode:

- Small one-fragment payloads are reliable.
- Mixed-size test runs work substantially better when a small payload appears before larger
  multi-fragment payloads.
- Large-only test runs after idle can fail to deliver any frames.
- Sending a real small payload first acts as a reliable primer for subsequent large payloads.

This indicates that the effective primer is not only RF energy or a duplicate fragment. The working
primer exercises the full bidirectional path:

1. Sender transmits a DATA packet.
2. Receiver receives and validates it.
3. Receiver transmits an ACK.
4. Sender receives the ACK.
5. Both radios return to a known TX/RX turnaround state.

The protocol needs an explicit link-layer idle heartbeat and data-pending handshake so user payloads
do not have to serve as accidental primers.

## Goals

- Prime the bidirectional radio path before the first multi-fragment ARQ burst after idle.
- Keep control packets out of the host KISS payload stream.
- Preserve existing ARQ DATA and ACK semantics for user payloads.
- Avoid moving directly to full TDMA scheduling.
- Make idle link health observable through stats counters.
- Provide a host-visible link readiness status based on recent bidirectional peer confirmation.
- Keep overhead low during active traffic.

## Non-Goals

- Replacing selective-repeat ARQ.
- Solving simultaneous bidirectional DATA contention completely.
- Implementing fixed-slot TDMA.
- Carrying host payload data in heartbeat packets.
- Using fake user payloads as primers.

## Requirements Language

The key words MUST, MUST NOT, SHOULD, SHOULD NOT, and MAY are to be interpreted as normative
requirements for this plan.

## Wire Protocol

The link layer SHOULD add a new packet type:

```cpp
enum class LinkPacketType : uint8_t {
    DATA    = 0,
    ACK     = 1,
    NATIVE  = 2,
    CONTROL = 3,
};
```

CONTROL packets MUST use the existing version/type byte. They MUST NOT be forwarded to `rxQueue`
or to the host KISS stream.

Recommended CONTROL packet format:

```text
Byte  0:    [version:4][type:4]       version=2, type=CONTROL
Byte  1:    control_type
Byte  2-3:  control_seq               uint16, increments per transmitted control request
Byte  4:    flags
Byte  5:    idle_class_or_reason
Byte  6:    pending_frags             0 when no pending DATA
Byte  7:    reserved
Byte  8-11: optional nonce/token       uint32
```

The CONTROL packet length SHOULD be fixed at 12 bytes initially. The remaining SX1280 FIFO bytes
MUST NOT be interpreted.

Recommended control types:

| Type | Name | Direction | Meaning |
| ---: | --- | --- | --- |
| `0x01` | `HEARTBEAT` | either node -> peer | Idle link check. |
| `0x02` | `HEARTBEAT_ACK` | peer -> requester | Response to `HEARTBEAT`. |
| `0x03` | `DATA_PENDING` | sender -> receiver | Sender is about to start DATA after idle. |
| `0x04` | `DATA_READY` | receiver -> sender | Receiver is ready for the pending DATA burst. |

`DATA_PENDING` MAY reuse the same response handling as `HEARTBEAT`, but a distinct
`DATA_READY` type is clearer for diagnostics.

## State Machine

Each node SHOULD track a small link-idle state independent of host payload queues:

```text
ACTIVE
  Recent DATA, ACK, or CONTROL exchange observed.

IDLE
  No radio exchange observed for RADIO_LINK_IDLE_MS.

PROBING
  HEARTBEAT or DATA_PENDING has been sent and the node is waiting for a response.
```

The node MUST update `last_link_activity_ms` after any successful radio RX or TX of DATA, ACK,
NATIVE, or CONTROL.

### Idle Heartbeat

When the link is IDLE and there is no queued user payload:

- A node SHOULD transmit `HEARTBEAT` every `RADIO_HEARTBEAT_INTERVAL_MS`.
- A node SHOULD add random jitter to avoid both nodes probing at the same instant.
- A node receiving `HEARTBEAT` MUST reply with `HEARTBEAT_ACK` as soon as practical.
- A node receiving `HEARTBEAT_ACK` for its outstanding `control_seq` MUST return to ACTIVE.
- A node MUST NOT forward heartbeat packets to the host.

Recommended initial values:

```cpp
#define RADIO_LINK_IDLE_MS              500
#define RADIO_HEARTBEAT_INTERVAL_MS     1000
#define RADIO_HEARTBEAT_JITTER_MS       250
#define RADIO_CONTROL_ACK_TIMEOUT_MS    40
#define RADIO_CONTROL_MAX_RETRIES       1
#define RADIO_LINK_READY_TTL_MS         3000
```

Heartbeat traffic SHOULD be suppressed while ARQ DATA is active.

### Link Ready Status

The firmware MUST expose a host-visible link readiness state. Readiness MUST be based on recent
bidirectional peer confirmation, not only local radio initialization, RSSI, or successful TX.

Recommended states:

```text
LINK_DOWN
  No valid peer response has been received inside RADIO_LINK_READY_TTL_MS.

LINK_PROBING
  HEARTBEAT or DATA_PENDING has been transmitted and the node is waiting for a peer response.

LINK_READY
  A valid HEARTBEAT_ACK or DATA_READY was received inside RADIO_LINK_READY_TTL_MS.
```

The readiness predicate SHOULD be:

```text
link_ready = now_ms - last_bidirectional_control_ms <= RADIO_LINK_READY_TTL_MS
```

`last_bidirectional_control_ms` MUST be updated when this node receives a valid response to one of
its own link-control requests:

- `HEARTBEAT_ACK` matching an outstanding `HEARTBEAT` `control_seq`.
- `DATA_READY` matching an outstanding `DATA_PENDING` `control_seq`.

Receiving a peer-initiated `HEARTBEAT` or `DATA_PENDING` proves the peer can transmit, but it does
not prove this node can complete a bidirectional exchange. Therefore, peer-initiated requests SHOULD
update `last_link_activity_ms` but MUST NOT by themselves set `LINK_READY`.

The firmware SHOULD expose readiness through the existing KISS control interface:

```text
STATS -> OK ... linkReady=1 linkState=READY linkAgeMs=842 hbTx=10 hbAckRx=9 dpTx=3 drRx=3
```

The firmware MAY also emit asynchronous KISS control-port events when readiness changes:

```text
EVENT link READY ageMs=0
EVENT link DOWN ageMs=3001
```

Asynchronous events MUST use the KISS control frame, not the KISS data frame. They MUST NOT be
inserted into the host payload stream.

Initial implementation SHOULD treat `LINK_READY` as advisory. Firmware SHOULD continue accepting
host payloads when the link is not ready and use `DATA_PENDING` to probe/prime before sending a
multi-fragment ARQ burst. Host tools MAY display link readiness and MAY warn the user before
starting large tests while the link is down.

### Data-Pending Primer

Before transmitting a multi-fragment DATA frame after idle:

- The sender MUST check whether the link is idle.
- If the link is idle, the sender MUST send `DATA_PENDING`.
- The sender SHOULD include the upcoming `pending_frags` count.
- The receiver MUST reply with `DATA_READY` unless it is currently busy with another reassembly.
- The sender SHOULD wait up to `RADIO_CONTROL_ACK_TIMEOUT_MS` for `DATA_READY`.
- If `DATA_READY` arrives, the sender MUST start the normal ARQ DATA round immediately.
- If `DATA_READY` is missed, the sender MAY retry `DATA_PENDING` once.
- If all retries fail, the sender MAY either start DATA anyway or fail the frame. Initial
  implementation SHOULD start DATA anyway to avoid a new hard-deadlock mode.

Single-fragment frames MAY skip `DATA_PENDING`, because testing indicates they already work as
natural primers.

## Collision Handling

This plan does not fully solve simultaneous bidirectional DATA contention, but CONTROL packets
SHOULD avoid making it worse:

- CONTROL transmissions SHOULD use a short random backoff when the node has recently transmitted
  another CONTROL packet.
- If a node receives `DATA_PENDING` while it also has queued DATA, it SHOULD respond `DATA_READY`
  first and defer its own DATA until the peer's ARQ transaction completes or times out.
- If both nodes send `DATA_PENDING` simultaneously and both miss responses, the existing ARQ timeout
  path remains the fallback.

The MAC Arbiter v2 plan can later centralize DATA, ACK, and CONTROL transmit arbitration.

## Firmware Changes

### `firmware/src/framing/Framing.h`

- Add `LinkPacketType::CONTROL`.
- Add `FRAMING_CONTROL_HDR_LEN`.
- Add a `ControlFrame` struct.
- Add helpers:
  - `framingParseControl()`
  - `framingBuildControlPacket()`
  - `framingControlTypeName()` if useful for diagnostics.

### `firmware/src/main.cpp`

- Add link activity state:
  - `last_link_activity_ms`
  - `last_bidirectional_control_ms`
  - `last_control_tx_ms`
  - `control_seq`
  - `link_state`
  - optional outstanding control request state.
- Update activity timestamp after successful radio TX/RX.
- Update link readiness after valid matched `HEARTBEAT_ACK` or `DATA_READY`.
- Downgrade readiness to `LINK_DOWN` when `RADIO_LINK_READY_TTL_MS` expires.
- Extend `radioRxTask`:
  - Parse `CONTROL` before DATA/NATIVE handling.
  - Reply to `HEARTBEAT` with `HEARTBEAT_ACK`.
  - Reply to `DATA_PENDING` with `DATA_READY`.
  - Forward valid `HEARTBEAT_ACK` or `DATA_READY` to a small control queue.
- Extend `radioTxTask`:
  - Before multi-fragment DATA after idle, call `primeLinkForData(total_frags)`.
  - Send `DATA_PENDING` and wait briefly for `DATA_READY`.
  - Start normal ARQ regardless of primer timeout in the first implementation.
- Add an idle heartbeat task or fold heartbeat scheduling into `radioTxTask`.
- Extend the `STATS` control response with `linkReady`, `linkState`, `linkAgeMs`, and control
  packet counters.

The implementation SHOULD avoid letting both `radioRxTask` and `radioTxTask` call
`radio.transmit()` at the same instant. If this becomes awkward, implement the MAC Arbiter v2 plan
first or combine these changes with it.

### `firmware/src/stats/Stats.h`

Add counters:

- `controlHeartbeatTx`
- `controlHeartbeatRx`
- `controlHeartbeatAckTx`
- `controlHeartbeatAckRx`
- `controlDataPendingTx`
- `controlDataPendingRx`
- `controlDataReadyTx`
- `controlDataReadyRx`
- `controlPrimerTimeouts`
- `controlMalformedDrops`
- `linkReadyTransitions`
- `linkDownTransitions`

Add fields:

- `linkReady`
- `linkState`
- `linkAgeMs`

### Host Tools

Host tools do not need to generate or parse CONTROL packets because CONTROL remains below the KISS
payload interface.

Useful optional additions:

- Add `raw_kiss.py --control STATS` display of `linkReady`, `linkState`, `linkAgeMs`, and the new
  counters once firmware exposes them.
- Add a `link_status.py` helper or `raw_kiss.py --link-status` alias if repeated manual `STATS`
  polling becomes common.
- Add `raw_fragment_test.py` output that distinguishes serial-integrity drops from missing frames,
  which already exists in current testing.

## Test Plan

### Unit Tests

- CONTROL packet build/parse roundtrip.
- Reject malformed CONTROL packets.
- Reject unknown control types.
- Verify CONTROL packets are not parsed as DATA or ACK.
- Verify matched `HEARTBEAT_ACK` sets `LINK_READY`.
- Verify unmatched `HEARTBEAT_ACK` does not set `LINK_READY`.
- Verify peer-initiated `HEARTBEAT` updates activity but does not by itself set `LINK_READY`.
- Verify readiness expires after `RADIO_LINK_READY_TTL_MS`.
- Verify `STATS` includes link readiness fields.

### Bench Tests

1. Flash both nodes from the same commit.
2. Confirm small-only remains clean:

```sh
python pi-daemon/raw_fragment_test.py --port /dev/ttyACM1 send --count 10 --sizes 105,106,107,219,220,221 --interval-ms 750 --write-gap-ms 5
```

3. Confirm large-only after idle works without a manual small primer:

```sh
python pi-daemon/raw_fragment_test.py --port /dev/ttyACM1 send --count 10 --sizes 333,334,335,447,448,449 --interval-ms 750 --write-gap-ms 5
```

4. Confirm default mixed run remains at least as good as current behavior:

```sh
python pi-daemon/raw_fragment_test.py --port /dev/ttyACM1 send --count 5 --interval-ms 250
```

Pass criteria:

- Large-only runs MUST produce received frames without requiring a manual small primer.
- CONTROL packets MUST NOT appear as host payloads.
- `STATS` MUST report `linkReady=1` after successful heartbeat or data-pending exchange.
- `STATS` MUST report `linkReady=0` after peer responses stop for longer than
  `RADIO_LINK_READY_TTL_MS`.
- ARQ integrity MUST still prevent truncated payload delivery.
- Any remaining serial truncation MUST show as serial-integrity drops, not bad delivered payloads.

## Implementation Order

1. Add CONTROL packet format and native tests.
2. Add stats counters.
3. Add receiver handling for `HEARTBEAT` and `DATA_PENDING`.
4. Add a control response queue for `HEARTBEAT_ACK` and `DATA_READY`.
5. Add link readiness state and expose it through `STATS`.
6. Add sender-side `DATA_PENDING` before multi-fragment DATA after idle.
7. Add optional periodic idle heartbeat.
8. Run `pio test -e native`.
9. Run `pio run`.
10. Flash both nodes.
11. Verify `STATS` reports link readiness transitions correctly.
12. Re-run large-only raw fragmentation test without manual primer.
13. Tune timeout/jitter values based on hardware results.

## Open Questions

- Should the first implementation start DATA after primer timeout, or fail the frame immediately?
- Should periodic heartbeats be enabled by default, or should only `DATA_PENDING` be implemented
  first?
- Should CONTROL transmission wait for the MAC Arbiter v2 queue, or is direct TX acceptable for the
  first focused experiment?
- What heartbeat interval gives useful readiness without wasting too much airtime?
- Should firmware emit asynchronous `EVENT link READY/DOWN` messages by default, or should host
  tools poll `STATS` only?

## Recommendation

Implement `DATA_PENDING`/`DATA_READY` first. It directly targets the confirmed failure while adding
minimal idle traffic. Add link readiness status at the same time because it falls out of the same
matched response tracking. Add periodic `HEARTBEAT` only after the data-pending primer proves that
the large-only idle-start failure is fixed.
