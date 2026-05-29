# TDMA MAC Design for SX1280 FLRC

## 1. Status

This document is a proposed design specification. It defines a future TDMA/TDD MAC layer for this repository and does not describe the current firmware behavior.

The key words `MUST`, `MUST NOT`, `SHOULD`, `SHOULD NOT`, and `MAY` are to be interpreted as normative requirements for the proposed TDMA mode.

## 2. Purpose

The current firmware uses an opportunistic half-duplex link with fragmented selective-repeat ARQ. That design is effective for asymmetric traffic, but it does not provide deterministic transmit ownership when both peers have data queued at the same time.

The TDMA mode specified here divides time into fixed master and slave transmit slots. Its primary purpose is to prevent same-time peer transmissions without relying on LBT/CSMA in the 2.4 GHz band.

## 3. Scope

This specification applies only to a two-node point-to-point link.

The design covers:

- master/slave time ownership
- fixed frame and slot timing
- synchronization acquisition and loss handling
- slot-based radio ownership
- adaptation of fragmented ARQ to asynchronous slot scheduling
- hardware bench validation requirements

The design does not cover:

- multi-node TDMA scheduling
- automatic role election
- dynamic slot borrowing in the first implementation
- compatibility with the existing wire protocol during rollout
- power-saving receive duty cycling

## 4. Terminology

- `Frame`: One complete TDMA cycle containing a master transmit slot, a guard interval, a slave transmit slot, and a guard interval.
- `Slot 0`: The master-owned transmit slot.
- `Slot 1`: The slave-owned transmit slot.
- `Guard`: A period where neither peer is permitted to transmit.
- `Master`: The node that defines the frame clock.
- `Slave`: The node that derives its frame phase from master transmissions.
- `Beacon`: A master packet whose primary purpose is synchronization. A data packet transmitted by the master MAY also serve as a beacon if it carries the required synchronization semantics.
- `Acquisition`: Slave RX-only mode before stable synchronization is established.
- `Locked`: Slave state in which slot phase is valid and slave transmission is permitted.

## 5. Current Firmware Constraints

The current repository is optimized around a transactional burst model:

- `radioTxTask()` sends one or more fragments in a burst, then waits for a bitmap ACK.
- `radioRxTask()` reassembles fragments and schedules ACK transmission after end-of-round or fallback timeout.
- Link timing uses `millis()` and FreeRTOS task blocking rather than a hardware slot clock.
- The default modem profile prioritizes robustness: `325 kbps`, `CR 1/2`, and `8 ms` inter-fragment delay.

TDMA mode MUST be treated as a new MAC mode. It MUST NOT be implemented by only changing the current ARQ delay constants.

## 6. Design Goals

TDMA mode MUST:

- prevent master and slave transmissions from overlapping in the synchronized steady state
- provide explicit transmit ownership for each node
- fail safe by disabling slave transmission when synchronization is lost
- preserve support for SX1280 FLRC packets up to the 127-byte payload limit
- support fragmented payload delivery across multiple TDMA frames
- provide instrumentation sufficient to validate slot timing on hardware

TDMA mode SHOULD:

- reuse existing radio primitives where they remain compatible with slot ownership
- keep the legacy non-TDMA path available until TDMA is validated
- prefer conservative timing margins in the first implementation

TDMA mode MUST NOT:

- perform blocking SPI transactions inside hard ISR context
- let the slave transmit while acquisition or synchronization status is uncertain
- depend on software logs alone for timing validation

## 7. Frame Format

Each frame MUST contain one master transmit slot and one slave transmit slot separated by guard intervals.

```text
|<--------------------------- T_frame --------------------------->|
+-------------------+-------------+-------------------+-----------+
| Slot 0: Master TX | Guard G0    | Slot 1: Slave TX  | Guard G1  |
| Slave RX          | no TX       | Master RX         | no TX     |
+-------------------+-------------+-------------------+-----------+
```

Normative rules:

- The master MUST own Slot 0.
- The slave MUST own Slot 1 only while locked.
- Both peers MUST remain silent during guard intervals.
- The master MUST transmit either a beacon or a synchronization-bearing data packet often enough for the slave to maintain lock.
- The slave MUST derive frame phase from valid master-slot packets.

The first implementation SHOULD use the following initial timing profile:

- `T_frame = 20 ms`
- `T_slot = 8 ms`
- `T_guard = 2 ms` for each guard interval

These values are starting requirements for development and instrumentation. They MAY be changed after hardware measurements demonstrate better margins or a different throughput/range tradeoff.

## 8. Roles

### 8.1 Master Requirements

The master:

- MUST start the TDMA frame clock at boot when TDMA mode is enabled.
- MUST transmit only during Slot 0.
- MUST switch the radio to RX before Slot 1 opens.
- MUST send a synchronization-bearing packet when no payload data is queued and the slave needs timing maintenance.
- SHOULD expose counters for missed slave slots, received slave packets, and guard margin violations.

### 8.2 Slave Requirements

The slave:

- MUST boot into acquisition mode.
- MUST keep the radio in continuous RX while in acquisition mode.
- MUST NOT transmit until lock is established.
- MUST transmit only during Slot 1 while locked.
- MUST return to acquisition mode after synchronization loss.
- SHOULD expose counters for lock acquisitions, missed master slots, phase corrections, and loss-of-sync events.

## 9. Modem Profile

The current default profile (`325 kbps`, `CR 1/2`) is optimized for robustness, not short slots. TDMA mode SHOULD define a separate modem profile.

The initial TDMA modem profile SHOULD be:

- `1300 kbps`
- `CR 3/4`
- existing 127-byte SX1280 packet limit

The implementation MUST NOT assume paper airtime estimates are sufficient. Before the profile is treated as stable, the team MUST measure:

- packet time on air for representative packet sizes
- TX-to-RX turnaround time
- RX-to-TX turnaround time
- DIO1 ISR timestamp latency
- end-to-end slot guard margin

If range testing shows unacceptable link margin regression, TDMA mode MAY define multiple profiles and choose a slower profile for longer links.

## 10. Synchronization

### 10.1 Time Origin

The slave MUST use successful reception of a valid master-slot packet as its frame phase reference. The timing event SHOULD be the RX-complete signal on `DIO1`, not task-level packet processing time.

### 10.2 Timestamp Capture

The radio ISR path MUST capture a microsecond-resolution timestamp as close as practical to the RX event. The ISR MUST remain minimal and SHOULD only:

- validate that the interrupt is relevant
- capture the timestamp
- signal a higher-priority control path

The ISR MUST NOT perform packet parsing, SPI transactions, or slot-state transitions that can block.

### 10.3 Lock Acquisition

The slave MUST remain in acquisition mode until it receives a configured number of consecutive valid master-slot packets. The initial lock threshold SHOULD be `3` consecutive packets.

When lock is acquired:

- the slave MAY enable Slot 1 transmission
- the slave MUST reset missed-beacon counters
- the slave SHOULD reset or bound accumulated drift-correction state

### 10.4 Drift Correction

The slave MUST apply bounded correction to its local frame phase. It MUST NOT blindly reset its frame timer on every received master packet if doing so would create slot jitter.

The first implementation SHOULD use a proportional controller:

- compute `phase_error = actual_rx_time - expected_rx_time`
- clamp `phase_error` to a configured maximum correction
- apply the correction to a future frame boundary

A PI controller MAY be added after measurement shows persistent bias that cannot be handled by proportional correction alone.

### 10.5 Loss of Sync

The slave MUST declare synchronization loss after a configured number of missed master slots. The initial threshold SHOULD be `5` consecutive missed master packets.

On synchronization loss, the slave MUST:

- immediately disable transmit eligibility
- return the radio to continuous RX
- transition to acquisition mode
- reset drift-correction state

The slave MUST NOT transmit based on extrapolated timing after synchronization loss.

## 11. MAC State Machine

The TDMA control module MUST implement an explicit state machine. The initial state set SHOULD be:

- `ACQUIRE`
- `MASTER_SLOT`
- `GUARD_TO_SLAVE`
- `SLAVE_SLOT`
- `GUARD_TO_MASTER`

### 11.1 Master State Behavior

In `MASTER_SLOT`, the master MUST transmit only packets that fit inside the remaining slot budget.

In `GUARD_TO_SLAVE`, the master MUST stop transmitting and transition the radio to RX before `SLAVE_SLOT`.

In `SLAVE_SLOT`, the master MUST receive slave packets and ACK-bearing packets.

In `GUARD_TO_MASTER`, the master MUST prepare the next Slot 0 transmission without transmitting early.

### 11.2 Slave State Behavior

In `ACQUIRE`, the slave MUST remain RX-only.

In `MASTER_SLOT`, the slave MUST receive master traffic and update synchronization state from valid master packets.

In `GUARD_TO_SLAVE`, the slave MAY prepare a transmission only if it is locked and the queued packet fits the slot budget.

In `SLAVE_SLOT`, the slave MUST transmit only while locked and only within the slot budget.

In `GUARD_TO_MASTER`, the slave MUST return the radio to RX before the next `MASTER_SLOT`.

### 11.3 State Invariants

The implementation MUST enforce these invariants:

- At most one peer may be transmitting in any slot.
- No peer may transmit during a guard interval.
- Slave transmission eligibility must be false in `ACQUIRE`.
- Any detected guard violation must be counted and exposed through diagnostics.

## 12. Radio Control Architecture

TDMA mode SHOULD introduce a dedicated TDMA control module responsible for slot timing and radio ownership.

The preferred architecture is:

- DIO1 ISR captures RX timing and signals control logic.
- A hardware timer provides frame and slot boundary events.
- A high-priority TDMA control task owns slot transitions.
- Packet TX/RX work remains outside hard ISR context.
- Existing `Radio::transmit()` and receive primitives are reused only where they satisfy slot timing requirements.

Blocking FreeRTOS mutex waits MUST NOT occur in timer callbacks or hard ISR context. If existing radio APIs require blocking locks, TDMA mode MUST route radio operations through a task context that can safely take those locks.

## 13. ARQ Adaptation

The existing `GENERIC_FRAGMENTED` mode blocks around burst transmission and bitmap ACK receipt. TDMA mode MUST replace that with asynchronous slot scheduling.

TDMA ARQ MUST:

- schedule fragment transmission by slot budget
- process ACK state asynchronously when packets arrive
- schedule retransmissions into future owned slots
- preserve selective retransmission of missing fragments
- avoid blocking all TX progress while waiting for one ACK response

During an owned slot, a node MAY send multiple fragments if the total transmission fits inside the remaining slot budget. A node MUST stop scheduling new packets before the guard interval would be violated.

ACK information SHOULD be sent in the opposite direction's next owned slot. ACKs MAY be dedicated control packets or piggybacked onto data packets; the packet format decision is left open.

## 14. Wire Protocol Impact

TDMA mode SHOULD be represented as a separate MAC or transport mode in configuration. It SHOULD NOT overload `GENERIC_FRAGMENTED` semantics in a way that makes legacy and TDMA behavior ambiguous.

The TDMA wire format MUST provide enough information to distinguish:

- synchronization beacons
- data fragments
- ACK-bearing packets
- sequence identity across multi-frame delivery

The exact packet format is deferred until the slot scheduler and ACK strategy are finalized.

## 15. Implementation Plan

### 15.1 Phase 1: Instrumentation and Timing Ground Truth

Implementation MUST first add measurement support before changing ARQ behavior.

Required work:

- capture RX-complete timestamps
- add debug GPIO probes for slot start, TX active, RX window, and lock state
- measure real packet airtime and radio turnaround on hardware
- validate or revise the initial `20/8/2 ms` frame geometry

Exit criteria:

- timing traces exist for representative packet sizes
- measured guard margins are known
- the TDMA timing profile is based on measurement rather than estimate

### 15.2 Phase 2: TDMA Clock and Acquisition

Required work:

- add hardware-timer-backed frame and slot events
- add the TDMA control task
- implement master beacon timing
- implement slave acquisition and lock tracking without slave transmission

Exit criteria:

- master emits stable frame timing
- slave acquires and loses lock as specified
- slave remains RX-only until locked

### 15.3 Phase 3: Slot-Based Radio Ownership

Required work:

- move radio TX/RX transitions under TDMA control
- enforce guard intervals
- enforce slave transmit eligibility
- expose guard violation diagnostics

Exit criteria:

- both boards maintain non-overlapping transmit ownership in long bench runs
- synchronization loss disables slave TX within the configured threshold

### 15.4 Phase 4: ARQ Integration

Required work:

- convert fragment scheduling to slot-budgeted transmission
- make ACK handling asynchronous
- preserve selective retransmission behavior
- validate simultaneous bidirectional traffic

Exit criteria:

- bidirectional fragmented traffic works under simultaneous offered load
- retransmissions occur only in owned slots
- guard violations remain zero in bench validation

### 15.5 Phase 5: Mode Integration

Required work:

- expose TDMA as an explicit selectable mode
- document role configuration
- keep the current non-TDMA mode available until TDMA passes validation

Exit criteria:

- an operator can select legacy mode or TDMA mode explicitly
- TDMA mode has documented timing, role, and modem profile settings

## 16. Validation Requirements

Software logs are not sufficient for acceptance. TDMA mode MUST be validated with external timing measurement.

Required probe signals:

- `GPIO_SLOT_ACTIVE`
- `GPIO_TX_ACTIVE`
- `GPIO_RX_WINDOW`
- `GPIO_SYNC_LOCKED`

Required pass criteria:

- Node A TX pulses fall inside Node B RX windows for the corresponding slot.
- Node B TX pulses fall inside Node A RX windows for the corresponding slot.
- No TX pulse overlaps a guard interval.
- Guard margin remains positive during long-duration testing.
- Slave TX stops within the configured miss threshold after master loss.
- Slave resumes TX only after reacquiring stable lock.

Required test cases:

- idle beacons only
- unidirectional saturated traffic
- bidirectional saturated traffic
- master restart
- slave restart
- reduced signal margin or attenuated link
- long-duration drift test

## 17. Risks and Mitigations

### 17.1 Timing Complexity

Risk: The current firmware is task-driven and millisecond-paced. TDMA introduces a stricter timing model.

Mitigation: Centralize slot ownership in one TDMA control module, keep ISR work minimal, and validate timing with probe signals.

### 17.2 Throughput Regression

Risk: Static slots waste capacity when only one peer has traffic.

Mitigation: Accept fixed slots for the first implementation. Demand-assigned or borrowable slots MAY be considered after basic TDMA is validated.

### 17.3 Range Regression

Risk: A faster FLRC profile may reduce link margin relative to `325 kbps`, `CR 1/2`.

Mitigation: Validate the high-rate profile against the intended antenna and range. Keep the legacy profile available for comparison.

### 17.4 Protocol Migration Cost

Risk: The current ARQ loop is tightly coupled to burst/ACK rounds.

Mitigation: Implement TDMA as a separate mode and avoid destabilizing the current production path during development.

## 18. Open Questions

- Should ACK information use dedicated control packets, piggybacking, or both?
- Should the master transmit a beacon every Slot 0, or only when no payload is queued?
- Is `1300 kbps`, `CR 3/4` acceptable for the intended range and antenna setup?
- Should TDMA be exposed as a transport mode or as a lower MAC mode beneath existing transports?
- What maximum phase correction per frame should be allowed after real drift data is collected?

## 19. Recommendation

TDMA development SHOULD proceed as a separate, instrumented mode. The first milestone MUST establish measured timing margins and slave fail-safe behavior before integrating fragmented ARQ.

The implementation MUST NOT retrofit TDMA by changing only existing ARQ task delays. The current implementation and the proposed TDMA system have different timing ownership models, and the code should preserve that boundary.
