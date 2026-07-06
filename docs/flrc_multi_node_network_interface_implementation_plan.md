# Multi-Node FLRC Network Interface Implementation Programme

> **Status (2026-07-06):** programme scope is on hold. Active development follows the compacted
> [two-node PoC plan](flrc_p2p_poc_implementation_plan.md) (no crypto, no election, adaptive TDD
> instead of TDMA). This document remains the long-term requirements baseline for multi-node,
> secured operation.

## 1. Status and Purpose

This document defines the implementation and qualification programme for developing the LilyGo
T3S3/SX1280 firmware into a deterministic, secure, dual-stack network interface. It is a plan and
requirements baseline; it does not claim that the described multi-node mode is implemented or
qualified in the current firmware.

The initial release target is a controlled three-node bench configuration consisting of one
coordinator and two endpoints. The design supports eight nodes, but neither eight-node operation nor
multi-kilometre tower operation is qualified by the initial release.

The key words `SHALL`, `SHALL NOT`, `MUST`, `MUST NOT`, `SHOULD`, `SHOULD NOT`, and `MAY` identify
normative requirements and recommendations for the proposed implementation.

## 2. Programme Objective and Requirements Baseline

Develop the firmware into a deterministic, secure, dual-stack network interface suitable for bench
qualification and eventual multi-kilometre tower links.

### 2.1 Mandatory system requirements

| ID | Requirement |
|---|---|
| SYS-001 | Support a coordinator-controlled TDMA star with coordinator-relayed endpoint traffic. |
| SYS-002 | Design for eight nodes; qualify one coordinator and two endpoints using the available hardware. |
| SYS-003 | Expose routed IPv4 and IPv6 through a production Rust TUN bridge. |
| SYS-004 | Provide runtime-selectable `FAST` and `ROBUST` PHY profiles. |
| SYS-005 | Achieve at least 150 kbit/s aggregate application goodput in the three-node `FAST` bench profile. |
| SYS-006 | Complete a 24-hour qualification run with at least 99.99% delivery and zero undetected corruption, duplication, or misrouting. |
| SYS-007 | Use authenticated encryption, replay protection, unique device identities, and PKI-authorized dynamic enrollment. |
| SYS-008 | Perform automatic coordinator election using an authenticated two-of-three quorum. |
| SYS-009 | Preserve the existing point-to-point protocol as an isolated legacy mode; mixed-wire operation is prohibited. |
| SYS-010 | Enforce ETSI/EU EIRP limits using configured conducted power, antenna gain, and feeder loss. |
| SYS-011 | Platform secure boot, encrypted storage, anti-rollback, and eFuse enforcement shall be designed for but deferred from the initial bench release. |
| SYS-012 | Multi-kilometre operation shall not be claimed as qualified until a separate tower field trial is completed. |

### 2.2 Critical implementation constraint

The existing `Radio::transmit()` boundary is classified **HIGH risk**, affecting the RX, ACK, and TX
execution paths. Timing or throughput optimization SHALL NOT begin until radio ownership and
acknowledged-delivery semantics are corrected.

Every implementation change to the radio boundary requires a fresh GitNexus upstream impact
analysis. A `HIGH` or `CRITICAL` result must be reported and reviewed before editing. Before every
commit, GitNexus change detection must confirm that only the intended symbols and execution flows
are affected.

## 3. Target Architecture and Public Interfaces

### 3.1 Radio and MAC architecture

`TDMA_STAR_V3` SHALL be introduced as a new, wire-incompatible MAC mode. Legacy point-to-point
framing and TDMA-v3 framing SHALL remain isolated; mixed-wire operation is prohibited.

One high-priority MAC task SHALL be the sole caller of radio transmit and receive-state transition
APIs. The DIO1 ISR SHALL be restricted to:

- capturing a timestamp;
- classifying the event;
- notifying the MAC task.

The ISR SHALL NOT access SPI, parse packets, wait on mutexes, or perform any other blocking
operation. A hardware timer SHALL drive superframe and slot boundaries.

Three coordinator-capable nodes SHALL be configured as voting members. Up to five additional nodes
MAY participate as non-voting endpoints.

The nominal three-node superframe is:

| Interval | Duration | Owner and purpose |
|---|---:|---|
| Beacon/control | 2.0 ms | Coordinator; authenticated synchronization and control |
| Downlink/relay | 5.5 ms | Coordinator |
| Guard | 0.5 ms | No transmission |
| Endpoint A uplink | 5.5 ms | Endpoint A |
| Guard | 0.5 ms | No transmission |
| Endpoint B uplink | 5.5 ms | Endpoint B |
| Recovery guard | 0.5 ms | No transmission |
| **Total** | **20.0 ms** | One superframe |

The scheduler SHALL refuse to start a packet unless measured worst-case airtime and turnaround fit
before the applicable guard interval. Schedules for more than three nodes MAY extend to 50 ms. Such
schedules are design-supported but are not qualified in the initial release.

### 3.2 PHY profiles

The initial PHY profiles are:

| Profile | Bit rate | Coding rate | BT | Preamble |
|---|---:|---:|---:|---:|
| `FAST` | 1300 kbit/s | 3/4 | 1.0 | 16 bits initially |
| `ROBUST` | 325 kbit/s | 1/2 | 1.0 | 32 bits |

Profile changes SHALL occur only at an authenticated schedule epoch boundary after every active node
acknowledges the transition. Frequency remains operator-configured; automatic frequency hopping is
excluded from the initial release.

Transmit power SHALL be capped such that:

```text
conducted power + antenna gain - feeder loss <= 20 dBm EIRP
```

This cap is subject to formal ETSI review and does not by itself establish legal compliance.

### 3.3 MAC v3 framing

Every MAC-v3 protected packet SHALL contain a fixed 24-byte authenticated common header:

| Field | Size |
|---|---:|
| Protocol version and packet type | implementation-defined packed field |
| Flags | implementation-defined packed field |
| Network ID | 16 bits |
| Source node ID | 8 bits |
| Destination node ID | 8 bits |
| Key epoch | 16 bits |
| Boot nonce | 64 bits |
| Packet counter | 32 bits |
| Superframe number | 32 bits |

The serializer SHALL define the exact byte allocation, byte order, and reserved-bit handling while
preserving the fixed 24-byte total. Reserved bits SHALL be transmitted as zero and rejected or
ignored according to the versioned parser policy.

Data packets add a 12-byte fragment header:

| Field | Size |
|---|---:|
| Datagram ID | 32 bits |
| Original datagram length | 16 bits |
| Fragment index | 8 bits |
| Fragment count | 8 bits |
| CRC32 of the complete plaintext datagram | 32 bits |

Each protected packet carries a 16-byte AES-256-GCM authentication tag. With the common and fragment
headers, 75 bytes remain for fragment data within the SX1280 127-byte packet limit.

The maximum supported datagram is 1280 bytes. It therefore requires no more than 18 fragments and
fits within a 32-bit selective-repeat bitmap.

ACK packets SHALL include:

- datagram identity;
- fragment count;
- received-fragment bitmap;
- receiver credits;
- failure status.

ACK packets SHALL receive scheduling priority over new data.

### 3.4 Reliability and buffering

Stop-and-wait burst handling SHALL be replaced by asynchronous selective-repeat ARQ.

The implementation SHALL:

- permit four outstanding datagrams per peer and eight globally;
- use a fixed pool of sixteen 1280-byte datagram buffers;
- prohibit large datagram buffers on task stacks;
- maintain a 64-datagram duplicate window per peer;
- express retry timeouts in superframes;
- default to a maximum of eight transmission attempts.

A receiver SHALL NOT send final acknowledgement until the completed datagram owns guaranteed
egress-buffer capacity. USB blockage SHALL withdraw receive credits and cause retransmission or
backpressure; it SHALL NOT produce acknowledged loss.

Reset, retry exhaustion, queue saturation, malformed traffic, credit withdrawal, and allocation
failure SHALL produce machine-readable counters.

### 3.5 Coordinator election

The election protocol SHALL:

- persist a monotonically increasing election term;
- declare coordinator loss after five missed authenticated beacons;
- suspend all user-data transmission during an election;
- use a randomized 250–750 ms candidate election delay;
- require two authenticated votes from the three configured voting nodes for promotion;
- keep a solitary or partitioned node receive-only;
- immediately demote a recovered coordinator when it receives a valid higher-term message.

Voting-membership changes require operator-signed authorization and SHALL NOT occur during an active
election.

### 3.6 Link security and enrollment

The initial bench release SHALL use:

- P-256 device identity certificates;
- operator-signed, short-lived enrollment tokens;
- ephemeral P-256 ECDH;
- HKDF-SHA-256 directional key derivation;
- AES-256-GCM per-hop encryption and authentication;
- separate coordinator-to-endpoint TX and RX keys;
- a 64-packet replay window;
- mandatory rekey before packet-counter exhaustion.

Endpoint-to-endpoint traffic SHALL be decrypted, authorized, and re-encrypted by the coordinator.
The coordinator is therefore a trusted per-hop security boundary, not an end-to-end opaque relay.

Unknown, revoked, replayed, expired-token, or authentication-failed traffic SHALL be discarded before
it consumes reassembly buffers.

Dynamic enrollment SHALL use authenticated join windows announced by the coordinator. Enrollment
tokens SHALL bind:

- network ID;
- device-certificate fingerprint;
- requested node ID;
- permissions;
- validity period.

Enrollment SHALL be rejected if coordinator time is not trusted.

### 3.7 Host interfaces

Legacy KISS data SHALL remain on port 0. Multi-node data SHALL use KISS port 1 and a 14-byte host
envelope containing:

- protocol version;
- source node ID;
- destination node ID;
- address family;
- traffic class;
- payload length;
- CRC32.

The serializer SHALL specify the exact packed field layout and byte order while preserving the
14-byte envelope length.

Host buffers SHALL accommodate a 1280-byte IP datagram, the envelope, KISS command byte, frame
delimiters, and worst-case KISS escaping.

New management functionality SHALL use a versioned binary TLV protocol on KISS command `0x0F`.
Existing read-only management commands SHALL be preserved during migration. New functionality SHALL
NOT extend the ad-hoc text protocol.

The Rust bridge SHALL be qualified as the production daemon and SHALL provide:

- bounded queues and explicit backpressure;
- chunked USB CDC writes;
- reconnect-safe, nonblocking TUN and serial loops;
- TOML configuration;
- systemd hardening;
- status and metrics through a root-owned Unix socket.

IPv4 and IPv6 destinations SHALL use explicit prefix-to-node mappings. IPv6 SHALL use ULA addressing
and `/128` routes.

General broadcast and link-local multicast SHALL NOT traverse the RF link. Configured multicast
groups MAY be replicated by the coordinator with explicit rate limits.

Python tools SHALL remain protocol references and diagnostic utilities, not production services.

## 4. Development Work Packages and Gates

Work packages are ordered. A later work package SHALL NOT use optimization or feature work to bypass
an unmet safety, reliability, or qualification gate.

### 4.1 WP0 — Baseline and configuration control

Deliverables:

- freeze the current functional baseline and record build hashes;
- pin PlatformIO, Espressif32, RadioLib, LovyanGFX, Rust, and Python dependency versions;
- establish requirements traceability, protocol versioning, a risk register, SBOM generation, and
  qualification-report templates;
- capture current native and fragmented performance as the comparison baseline.

**Gate SRR:** requirements, threat model, regulatory assumptions, interfaces, and acceptance criteria
are approved.

### 4.2 WP1 — Radio ownership and lossless delivery foundation

Deliverables:

- introduce the single-owner MAC task and priority queues for ACK, control, retransmission, and new
  data;
- remove every direct radio transmission from RX processing;
- introduce the fixed buffer pool, receiver credits, egress reservation, and backpressure rules;
- make task-creation, semaphore-allocation, radio-state, stack, and USB failure paths visible and
  deterministic in recovery;
- add watchdog supervision and task high-watermark telemetry.

**Gate PDR-1:** legacy mode passes existing tests and hardware regression; queue saturation cannot
produce acknowledged loss.

### 4.3 WP2 — MAC v3 framing and cryptographic foundation

Deliverables:

- implement bounded parsers and serializers for common, `DATA`, `ACK`, `BEACON`, `ELECTION`, `JOIN`,
  and `KEY` packets;
- implement directional key derivation, nonce generation, replay windows, key epochs, and
  zeroization;
- add the host data envelope and binary management protocol;
- keep legacy and v3 parsers completely isolated.

**Gate PDR-2:** known-answer crypto tests, malformed-input tests, replay tests, nonce-uniqueness
tests, and parser fuzzing pass.

### 4.4 WP3 — TDMA scheduler and three-node operation

Deliverables:

- add timer-driven superframes, GPIO timing probes, authenticated beacons, acquisition, lock
  tracking, and guard enforcement;
- implement coordinator scheduling and two endpoint uplink slots;
- measure real packet airtime, TX/RX turnaround, ISR latency, clock drift, and guard margin with a
  logic analyser;
- adjust slot constants only from controlled timing evidence while preserving the defined slot
  roles.

**Gate CDR-1:** zero measured guard violations during an eight-hour timing run; endpoints fail silent
on synchronization loss.

### 4.5 WP4 — Sliding-window ARQ and coordinator relay

Deliverables:

- implement per-peer asynchronous transmit and reassembly contexts;
- add selective retransmission, duplicate suppression, receiver credits, retry exhaustion, and
  coordinator two-hop forwarding;
- prioritize ACK and control traffic without starving payload queues;
- support 1280-byte IPv4/IPv6 datagrams and simultaneous bidirectional flows.

**Gate CDR-2:** no corruption, duplicate delivery, misrouting, or deadlock occurs during
fault-injected three-node traffic.

### 4.6 WP5 — Election, enrollment, and operational management

Deliverables:

- implement quorum election, persistent terms, stale-coordinator demotion, and partition behavior;
- implement certificate validation, signed enrollment tokens, join windows, revocation, pairwise
  session establishment, and key rotation;
- complete Rust daemon routing, configuration, observability, service hardening, and management CLI;
- provide operator procedures for node addition, revocation, coordinator recovery, key rotation, and
  legacy rollback.

**Gate TRR:** all automated, security, fault-injection, and timing prerequisites pass before
qualification testing.

### 4.7 WP6 — Performance optimization and qualification

Deliverables:

- tune SPI transactions, packet batching, inter-packet timing, slot utilization, USB chunking, and
  scheduler queues;
- optimize `FAST` first while retaining the unchanged `ROBUST` qualification profile;
- run the formal 24-hour three-node test and produce a signed evidence package.

**Gate QAR:** all initial-release acceptance criteria pass without waivers.

### 4.8 WP7 — Deferred operational hardening and tower qualification

Deliverables:

- add signed firmware, secure boot, encrypted NVS/flash, anti-rollback, protected key storage, and
  controlled eFuse provisioning;
- validate recovery and manufacturing procedures before burning irreversible security fuses on
  development units;
- complete ETSI compliance review and laboratory testing;
- conduct sector-hub/directional-spoke tower trials over the intended multi-kilometre paths;
- demonstrate at least 10 dB measured fade margin in `ROBUST`, document EIRP and weather exposure,
  and complete a 24-hour field soak before operational qualification.

WP7 is outside the initial bench release. Completion of WP6 SHALL NOT be represented as completion of
WP7.

## 5. Verification and Qualification Plan

### 5.1 Automated verification

Automated verification SHALL:

- unit-test every framing boundary, bitmap size, datagram limit, queue transition, schedule
  transition, election term, and configuration migration;
- property-test encoding/decoding and selective-repeat convergence;
- fuzz every radio and USB parser with malformed lengths, invalid types, truncated frames, duplicate
  fragments, and random byte streams;
- run cryptographic known-answer, altered-header, altered-ciphertext, replay, stale-key,
  expired-token, revocation, and counter-wrap tests;
- require clean firmware builds, native tests, Python conformance tests, Rust tests, Clippy, and
  dependency audits in CI.

### 5.2 Hardware-in-the-loop verification

Hardware-in-the-loop verification SHALL use:

- three T3S3 boards running the same controlled build;
- logic-analyser probes for slot active, TX active, RX window, and synchronization lock.

The test matrix SHALL cover:

- idle operation;
- unidirectional saturation;
- bidirectional saturation;
- mixed IPv4/IPv6 traffic;
- coordinator relay;
- queue saturation;
- USB disconnect and reconnect;
- radio errors;
- endpoint restart;
- coordinator restart;
- asymmetric attenuation.

Each role SHALL undergo 100 controlled power cycles.

Coordinator partition tests SHALL demonstrate:

- a two-node majority elects exactly one coordinator;
- an isolated node remains silent;
- a healed partition converges to the highest term.

### 5.3 Initial qualification thresholds

| Category | Acceptance threshold |
|---|---|
| Duration | At least 24 continuous hours |
| Volume | At least 1,000,000 delivered datagrams across mixed sizes |
| Delivery | At least 99.99% at the declared sustainable offered load |
| Integrity | Zero undetected corruption |
| Semantics | Zero duplicate or wrong-destination delivery |
| Stability | Zero deadlocks, watchdog resets, unbounded queues, or unrecovered radio states |
| Timing | Zero guard violations |
| Election | Coordinator selected within 3 seconds; payload service restored within 5 seconds |
| Reconnection | USB service restored within 10 seconds without reflashing or rebooting |
| `FAST` goodput | At least 150 kbit/s aggregate receiver-measured application goodput |
| `FAST` latency | At 70% sustainable load, p95 <= 250 ms and p99 <= 500 ms |
| `ROBUST` latency | p95 <= 1 second under its declared sustainable load |
| Security | 100% rejection of invalid authentication, replay, revoked identity, stale key, and unauthorized enrollment test vectors |

All qualification reports SHALL record:

- firmware hash;
- dependency-lock hashes;
- configuration checksum;
- RF settings;
- antenna and feeder assumptions;
- exact commands;
- raw logs;
- packet counts;
- latency distribution;
- error counters;
- timing captures.

## 6. Requirements Traceability

The following matrix identifies the primary implementation and verification ownership. Detailed
test-case identifiers SHALL be added during WP0 and maintained under configuration control.

| Requirement | Primary work package(s) | Primary verification |
|---|---|---|
| SYS-001 | WP1, WP3, WP4 | Three-node HIL traffic and relay tests |
| SYS-002 | WP3, WP4 | Three-node qualification; design review for eight-node limits |
| SYS-003 | WP2, WP5 | Mixed IPv4/IPv6 routing and reconnect tests |
| SYS-004 | WP2, WP3 | Profile transition and schedule-epoch tests |
| SYS-005 | WP6 | Receiver-measured `FAST` goodput test |
| SYS-006 | WP4, WP6 | Formal 24-hour qualification run |
| SYS-007 | WP2, WP5 | Crypto KATs, replay, enrollment, revocation, and fault tests |
| SYS-008 | WP5 | Majority, isolation, restart, and healed-partition HIL tests |
| SYS-009 | WP1, WP2 | Legacy regression and mixed-wire rejection tests |
| SYS-010 | WP0, WP5, WP7 | Configuration validation and formal regulatory review |
| SYS-011 | WP0, WP7 | Security architecture review and field-release verification |
| SYS-012 | WP7 | Tower trial, fade-margin evidence, and field soak |

## 7. Assumptions, Constraints, and Release Policy

- Initial physical qualification is limited to the three available nodes.
- Eight-node capacity is design-supported but remains unqualified until additional hardware testing.
- The initial release is a controlled bench release, not an operationally hardened defence product.
- Secure boot, encrypted storage, anti-rollback, and eFuse enforcement are mandatory for the later
  field release.
- Tower deployment assumes a sector antenna at the coordinator and directional antennas at
  endpoints.
- ETSI/EU is the governing regulatory baseline; legal compliance requires formal review and testing.
- IPv4 and IPv6 routed unicast are qualified. Transparent Ethernet, unrestricted broadcast, and
  general multicast are excluded.
- Legacy point-to-point mode remains available for rollback and diagnostics but cannot share a
  network with TDMA-v3 nodes.
- Every implementation change to the radio boundary requires fresh GitNexus impact analysis. Every
  commit requires change-impact detection and review of affected execution flows.
- No performance optimization may weaken authentication, replay protection, guard enforcement,
  acknowledged-delivery guarantees, or fail-silent election behavior.

## 8. Release Evidence and Claims

The initial bench release may be declared qualified only after Gate QAR passes without waivers and
the evidence package demonstrates every threshold in Section 5.3.

Release documentation SHALL clearly distinguish:

- implemented from planned functionality;
- tested from untested node counts and traffic patterns;
- bench-qualified from field-qualified operation;
- configured EIRP calculations from formal regulatory compliance;
- software cryptographic controls from deferred platform key and boot protections.

No release based only on this programme or on bench qualification may claim multi-kilometre,
eight-node, operational security, or regulatory qualification.
