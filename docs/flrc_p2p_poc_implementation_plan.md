# FLRC Point-to-Point PoC Implementation Plan

## 1. Status and Purpose

This document is the **active implementation plan**. It compacts the scope of the
[multi-node programme](flrc_multi_node_network_interface_implementation_plan.md) to a two-node
proof of concept. The multi-node programme document remains the long-term requirements baseline;
nothing in this plan contradicts its architecture, but crypto, enrollment, election, TDMA
scheduling, and platform hardening are **deferred**, not cancelled.

**PoC objective:** a reliably working two-node IP link over SX1280 FLRC, engineered to the
hardware's throughput ceiling, deployable between two tower sites ~10 km apart.

## 2. Locked Scope Decisions (2026-07-06)

| Decision | Choice |
|---|---|
| Node count | 2 (point-to-point), multi-node deferred |
| Throughput target | Maximize SX1280 ceiling; no hard pass/fail number. Design target ≥ 60 kB/s TCP goodput at the qualification profile, ~100 kB/s stretch |
| Antennas | 24+ dBi dishes at both towers |
| MAC | Adaptive TDD master/slave; symmetric peers — master by config flag, airtime split adapts from queue depth |
| Traffic profile | General TCP/IP (SSH, web, mixed); latency and bidirectional fairness matter |
| Wire format | Minimal P2P v3 header, version-gated for clean wire-incompatibility with future formats |
| Link MTU | 1280 bytes, fragmented over the air (IPv6 minimum MTU) |
| Address families | IPv4 and IPv6 (ULA) dual stack |
| PHY profiles | Runtime-selectable with coordinated switchover |
| Management | Versioned binary TLV on KISS command `0x0F`; ad-hoc text protocol frozen |
| Display | OLED status display retained |
| Host daemon | Rust (`pi-daemon-rust`) is the sole production bridge; Python tools frozen as diagnostics/reference |
| Security | None on the wire (deferred). The link is an open modem; any confidentiality comes from higher layers (e.g. WireGuard over the tunnel) |
| Acceptance bar | Bench smoke qualification (hours, not 24 h), then tower deployment and field iteration. Optional overnight bench soak before first tower visit |

## 3. Feasibility Baseline

### 3.1 Link budget (10 km, 2.48 GHz)

Free-space path loss at 10 km is ~120 dB. ETSI EN 300 328 caps **EIRP at 20 dBm**; receive
antenna gain is unregulated.

```text
RX level = 20 dBm EIRP − 120 dB FSPL + 24 dBi RX gain − feeder ≈ −78 dBm
```

| Profile | Bit rate / CR | Sensitivity (approx.) | Fade margin at −78 dBm |
|---|---|---:|---:|
| `FAST` | 1300 kbit/s, 3/4 | −94 dBm | ~16 dB |
| `ROBUST` | 325 kbit/s, 1/2 | −103 dBm | ~25 dB |
| `FAST_CR1` (bench stretch) | 1300 kbit/s, 1/1 | −91 dBm | ~13 dB |

`FAST` is the qualification profile. `ROBUST` is the weather/fade fallback. `FAST_CR1` is a
bench-only stretch profile unless tower measurements show margin.

Line of sight is required, with ≥ 10 m Fresnel-zone clearance at midpath (first-zone radius
~17.5 m at 10 km / 2.4 GHz; 60% clearance rule). This SHALL be verified before mounting.

**Conducted TX power:** with a 24 dBi dish, `conducted ≤ 20 − 24 + feeder_loss ≈ −4 dBm`
(plus feeder loss). The SX1280 range of −18 to +12.5 dBm covers this. The EIRP formula from the
programme document remains normative and the configuration SHALL enforce it from configured
antenna gain and feeder loss.

### 3.2 Throughput ceiling

A full 127-byte FLRC packet is ~1.15 ms on air at `FAST` (~0.85 ms at CR 1/1). With a ~12-byte
v3 header, each packet carries ~115 payload bytes.

- Raw fragment-payload ceiling at `FAST`: ~85–90 kB/s unidirectional.
- After TDD turnaround, ACK traffic, and TCP overhead at MTU 1280 (~3%): **60–80 kB/s TCP
  goodput expected; ≥ 60 kB/s is the design target.**
- `FAST_CR1` stretch: ~100–120 kB/s raw, ~90–100 kB/s TCP.

Rates materially above ~120 kB/s are physically unreachable on a single SX1280 FLRC link; that
finding is recorded here to close the question permanently.

## 4. Architecture

### 4.1 Radio ownership (unchanged critical constraint)

The programme document's Section 2.2 applies in full: the `Radio::transmit()` boundary is HIGH
risk, one high-priority MAC task SHALL be the sole caller of radio TX/RX state APIs, the DIO1
ISR SHALL only timestamp/classify/notify, and no throughput work begins before this is fixed.
GitNexus impact analysis before radio-boundary edits and change detection before commits remain
mandatory.

### 4.2 Adaptive TDD MAC

- One node is **master** by configuration flag (symmetric peers; no protocol asymmetry beyond
  who arbitrates airtime).
- The master transmits a bounded burst, ending with a **grant** giving the slave a bounded TX
  window; the slave's burst ends by returning control. Loss of a grant or burst-end is recovered
  by timeout at both ends.
- Each burst header carries the sender's queue depth; the master adapts the split (starting
  50/50) so the busier direction gets more airtime.
- Burst length is bounded (~25 ms per direction) so worst-case link RTT stays ~50–60 ms and TCP
  ACKs are never starved.
- Idle link: the existing heartbeat/link-state concepts (HEARTBEAT / DATA_PENDING semantics)
  carry over as TDD control frames; a dead peer is declared after N missed heartbeats and the
  link re-acquires without reboot.

### 4.3 Wire format v3 (P2P, minimal)

All v3 packets begin with a version/type octet. Version mismatch SHALL cause silent discard and
a counter increment; v3 is deliberately wire-incompatible with legacy framing (rollback is by
reflashing the tagged legacy firmware, not by runtime mode switching).

Header budget: **≤ 13 bytes, ≥ 114 bytes fragment payload.** Nominal DATA layout (serializer
defines exact packing; byte order little-endian):

| Field | Size |
|---|---:|
| Version + packet type | 1 B |
| Flags + queue-depth hint | 1 B |
| Datagram ID | 2 B |
| Fragment index | 1 B |
| Fragment count | 1 B |
| Datagram length | 2 B |
| Packet CRC32 (header + payload) | 4 B |

Packet types: `DATA`, `ACK`, `CONTROL` (grant/heartbeat/link state/profile switch), `MGMT`.
Max datagram 1280 B → ≤ 12 fragments → 16-bit selective-repeat bitmap.

### 4.4 Selective-repeat ARQ

Replaces stop-and-wait burst handling:

- ≤ 4 outstanding datagrams per direction; fixed pool of 8 × 1280 B datagram buffers per node,
  never on task stacks;
- ACK carries datagram ID, fragment bitmap, receiver credits, failure status; ACKs and CONTROL
  have scheduling priority over new DATA;
- 64-datagram duplicate window; retry timeouts expressed in TDD cycles; max 8 attempts;
- a receiver SHALL NOT final-ACK a datagram without guaranteed egress-buffer capacity. USB
  blockage withdraws credits and produces backpressure, never acknowledged loss;
- reset, retry exhaustion, saturation, malformed input, credit withdrawal, and allocation
  failure all produce machine-readable counters.

### 4.5 PHY profiles and switching

`FAST`, `ROBUST`, and `FAST_CR1` as defined in §3.1. Runtime switching uses a CONTROL
handshake: proposer announces, peer acknowledges, both switch at an agreed TDD cycle boundary,
with a timeout fallback to the previous profile if the peer is not heard after switching.
Frequency stays operator-configured (existing runtime ModemConfig/NVS mechanism).

### 4.6 Host interface

- Legacy KISS data semantics stay on port 0 with a slim data envelope: version, address family,
  payload length, CRC32 (8 B budget; exact packing defined by the serializer). Node addressing
  fields are omitted — this is a point-to-point link.
- Management: versioned binary TLV on KISS command `0x0F`. Existing read-only text commands
  keep working during migration; no new text commands.
- Rust daemon (`pi-daemon-rust`): TUN MTU 1280, IPv4 /30 + IPv6 ULA routes, bounded queues and
  explicit backpressure, chunked USB CDC writes, reconnect-safe loops, TOML configuration,
  systemd unit, status/metrics on a Unix socket.
- No broadcast or link-local multicast over RF.

## 5. Work Packages

Ordered; a later WP does not start before the prior WP's gate.

### WP-A — Radio ownership and MAC task foundation
Single-owner MAC task; ISR reduced to timestamp/classify/notify; non-blocking TX/RX state
transitions; buffer pool and priority queues (ACK/CONTROL > retransmit > new DATA); watchdog
supervision and task high-watermark telemetry; failure paths visible in counters.
**Gate:** existing native tests pass; hardware regression on current protocol; queue saturation
cannot produce acknowledged loss.

### WP-B — v3 framing and selective-repeat ARQ
Bounded parsers/serializers for all v3 packet types; fragmentation/reassembly at MTU 1280;
duplicate suppression; credits and egress reservation; native unit and property tests for
framing, bitmap convergence, and malformed input (fuzz the parser with truncated/duplicate/
random frames).
**Gate:** native test suite green including fuzz corpus; two-board bench exchange of 1280-byte
datagrams with induced loss shows zero corruption/duplication.

### WP-C — Adaptive TDD MAC
Master/slave grants, queue-depth adaptation, bounded bursts, idle heartbeat, link supervision,
re-acquisition after peer restart. Timing instrumented via firmware timestamps; GPIO probes
optional (no logic-analyser gate at PoC bar).
**Gate:** sustained bidirectional saturation with no deadlock, no unrecovered radio state, and
measured RTT within the §4.2 bound; peer restart recovers without operator action.

### WP-D — Host path (Rust daemon + management)
Envelope, MTU 1280 TUN, IPv4+IPv6 routing, TLV management client, metrics socket, systemd.
**Gate:** iperf3 TCP/UDP both directions and concurrent SSH through the tunnel; USB
disconnect/reconnect restores service < 10 s without reboot.

### WP-E — PHY profiles and switchover
Three profiles, coordinated runtime switching with fallback, display/TLV exposure of profile
and link stats.
**Gate:** repeated switchovers under traffic with zero acknowledged loss and no link drop.

### WP-F — Bench smoke qualification
Conducted setup with ~70–90 dB attenuation (shielded) to emulate the −78 dBm tower RX level.
Several hours of continuous bidirectional iperf3 + interactive SSH at `FAST`; record goodput,
RTT distribution, and all counters. Optional (recommended, zero engineering cost): leave the
rig running overnight before the first tower visit.
**Gate (PoC acceptance):**
- ≥ 3 h continuous bidirectional load: zero acknowledged loss, zero duplicate or corrupted
  delivery to TUN, zero deadlock/watchdog/unrecovered radio state;
- TCP goodput ≥ 60 kB/s aggregate at `FAST` (report the measured figure);
- SSH remains interactively usable under full load;
- USB reconnect < 10 s.

### WP-G — Tower deployment
LOS/Fresnel verification; EIRP configuration from measured antenna gain and feeder loss
(expect ~−4 dBm conducted); RSSI/fade-margin measurement against the §3.1 prediction (flag if
off by > 3 dB); profile fallback exercised; field throughput and stability report.
This is iterative field work, not a formal qualification gate. Multi-kilometre operation SHALL
NOT be represented as qualified beyond what this report demonstrates.

## 6. Deferred (unchanged from the programme document)

Crypto suite (identity, ECDH/HKDF/GCM, replay, enrollment), coordinator election, TDMA
multi-node scheduling, node addressing on the wire, secure boot / encrypted NVS / anti-rollback
/ eFuses, formal 24 h qualification, formal ETSI review, eight-node design validation. The
programme document governs all of these when they are picked up.

## 7. Carried-Over Constraints

- GitNexus impact analysis before radio-boundary edits; change detection before every commit.
- EIRP ≤ 20 dBm enforced from configured conducted power, antenna gain, and feeder loss.
- No optimization may weaken acknowledged-delivery guarantees or backpressure correctness.
- Both nodes always run identical firmware builds; legacy rollback is via tagged builds.
