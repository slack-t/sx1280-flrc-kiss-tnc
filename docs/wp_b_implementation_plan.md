# WP-B Implementation Plan — v3 Framing and Selective-Repeat ARQ

Step-by-step execution plan for implementing WP-B of
`docs/flrc_p2p_poc_implementation_plan.md` (§4.3 wire format v3, §4.4
selective-repeat ARQ, §5 WP-B). Written 2026-07-07 for an implementing agent.

## Ground Rules (read before touching code)

- **Radio ownership is inviolable** (§4.1): `macTask` in `firmware/src/mac/Mac.cpp`
  is the sole caller of radio TX/RX state APIs. Nothing in WP-B adds a second
  caller. The DIO1 ISR stays timestamp/classify/notify only.
- **No dynamic allocation, no task-stack payload buffers.** All datagram
  storage comes from a fixed static pool (§4.4).
- **Host-testability is the point.** Every new module up to MAC integration
  must compile in the `native` PlatformIO env (`-std=c++14`, Unity framework,
  no `Arduino.h`/FreeRTOS includes). Time is injected as `uint32_t now`
  parameters; radio sends go through a caller-provided function
  interface — follow the pattern of `framing/Framing.h`, which is header-only
  and hardware-free.
- **USB CDC stays a pure KISS stream.** No console logging on the data path;
  new observability is counters in `Stats` (mutex-guarded, see
  `stats/Stats.h`) surfaced via `STATS`.
- **v3 is deliberately wire-incompatible with v2.** No runtime compatibility
  mode, no version negotiation. A v3 node silently discards v2 traffic (with a
  counter). Rollback is reflashing the tagged legacy firmware.
- **Both bench nodes must run identical firmware after every flash** and
  `kiss_tun.py` must be killed before flashing (established bench workflow).
- Project tooling rules in `CLAUDE.md`/`AGENTS.md` (GitNexus impact analysis
  before editing symbols, change detection before commits) apply. If the
  GitNexus DB is locked/read-only, fix that first or note the skip in the
  commit message.
- Build/test commands: `pio test -e native`, `pio run -e t3s3`,
  `pio run -t upload`, all from `firmware/`.

## Phase 0 — Baseline and rollback anchor

1. Confirm clean tree, `pio test -e native` green (35 cases as of `6a2d669`),
   `pio run -e t3s3` builds.
2. Create the rollback anchor the plan requires:
   `git tag legacy-v2-framing` on the current master HEAD, push the tag.
3. Create a working branch `wp-b-v3-framing`.
4. Read before writing: `framing/Framing.h` (v2 patterns to follow),
   `framing/Crc32.h`, `mac/Mac.cpp` (integration surface),
   `kiss/SerialIntegrity.h`, `test/test_kiss/test_kiss.cpp` (test
   conventions), `config.h`.

## Phase 1 — v3 wire format (`firmware/src/framing/FramingV3.h`)

Header-only, native-testable, little-endian explicit byte packing (no struct
casts — serialize field by field as v2 does).

1. Constants: `V3_VERSION = 3`; packet types `DATA`, `ACK`, `CONTROL`, `MGMT`;
   `V3_MAX_DATAGRAM = 1280`; `V3_MAX_FRAGS = 12`; fragment payload sized so
   header ≤ 13 B leaves ≥ 114 B payload within the 127 B FLRC packet
   (`PACKET_MAX_LEN` in `config.h`).
2. DATA header per §4.3, 12 B nominal: version+type (1 B), flags +
   queue-depth hint (1 B), datagram ID (2 B), fragment index (1 B), fragment
   count (1 B), datagram length (2 B), CRC32 over header+payload (4 B).
   Document the exact bit packing in the header comment — the serializer is
   the definition.
3. ACK packet: datagram ID, 16-bit fragment bitmap, receiver credits,
   failure status (§4.4). CONTROL: subtype octet (heartbeat / heartbeat-ack /
   link state; grant and profile-switch subtypes reserved for WP-C/WP-E, only
   enum values defined now). MGMT: opaque bounded payload (transport defined
   now, semantics in WP-D).
4. Every parser: bounds-check length before any field read, verify version
   octet first (mismatch → distinct return code so the MAC can count silent
   discards), verify CRC32 last. Parsers never write outside caller buffers;
   return codes, not asserts.
5. Tests in `firmware/test/test_framing_v3/`: round-trip every packet type;
   truncation at every byte boundary of a valid DATA packet; corrupt version,
   type, CRC; datagram length 0, 1, 114, 115, 1279, 1280, 1281 (reject);
   fragment count 0, 12, 13 (reject); bitmap edge values.

**Done when:** native suite green; no `Arduino.h` anywhere in the new header.

## Phase 2 — Fragmentation/reassembly (`firmware/src/framing/FragmentV3.h`)

1. Pure functions: datagram (≤ 1280 B) → fragment descriptors (≤ 12); single
   `ReassemblyV3` state struct: datagram ID, fragment count, 16-bit
   received-bitmap, per-fragment lengths, completion check, total-length
   consistency check across fragments (mismatched duplicate metadata →
   reject + counter, as v2's integrity drops do).
2. Buffers are caller-provided (pool comes in Phase 3) — the module holds
   pointers, never owns storage.
3. Tests in `firmware/test/test_fragment_v3/`: property-style loop with a
   seeded PRNG — random lengths 1..1280, random arrival order, random
   duplication; reassembled bytes must equal input for all seeds; bitmap
   convergence (every permutation of a small fragment set reaches complete).

**Done when:** property tests pass for ≥ 10k seeded iterations natively.

## Phase 3 — Datagram pool and duplicate window (`firmware/src/arq/`)

1. `DatagramPool`: static array of 8 × 1280 B buffers + free bitmap.
   `acquire()`/`release()`, allocation-failure counter hook. No heap ever.
2. `DuplicateWindow`: 64-entry history of completed datagram IDs with
   serial-number (wraparound-safe, RFC 1982 style) comparison on the 16-bit
   ID space; answers "already delivered?" and stores the final ACK/failure
   status for re-ACKing duplicates without re-delivery (mirrors v2's
   completed-cache behaviour in `Mac.cpp`).
3. Tests in `firmware/test/test_arq_support/`: pool exhaustion returns
   failure (never blocks/crashes); release/re-acquire cycling; duplicate
   window hit/miss around ID wraparound (0xFFFE → 0x0001).

## Phase 4 — Selective-repeat ARQ engine (`firmware/src/arq/ArqEngine.h/.cpp`)

The core deliverable. A pure, event-driven state machine with **no** OS or
hardware includes; `Mac.cpp` will own one instance per link direction.

Inputs: `onTxDatagram(frame)`, `onRxPacket(parsed v3 packet, now)`,
`onTick(now)`, egress-capacity callback, packet-send callback (the MAC binds
this to the radio). Outputs: packets to send (ACK/CONTROL prioritized over
retransmit, retransmit over new DATA — same priority order WP-A established),
delivered datagrams, counters.

1. TX side: ≤ 4 outstanding datagrams; per-datagram state (fragment bitmap of
   un-ACKed fragments, attempt counter, next-retry deadline); max 8 attempts
   then fail with a machine-readable failure status and buffer release; retry
   timeout is a caller-supplied parameter expressed in abstract "cycles"
   (WP-B bench maps 1 cycle to a configured ms value; WP-C rebinds it to TDD
   cycles without engine changes).
2. RX side: per-datagram reassembly (≤ 4 concurrent, pool-backed); ACK
   generation with current bitmap + credits + failure status; **egress
   reservation invariant carried over from WP-A: never final-ACK a datagram
   without guaranteed egress capacity — withdraw credits instead** (USB
   blockage → backpressure, never acknowledged loss).
3. Credits: receiver advertises how many new datagrams it can accept
   (free pool buffers minus reserved egress); sender never opens a new
   datagram without a credit.
4. Counters for every §4.4 failure class: reset, retry exhaustion,
   saturation, malformed input, credit withdrawal, allocation failure.
5. Tests in `firmware/test/test_arq_engine/` — this is where the WP-A burst
   findings become regression tests. Simulate two engines connected by a
   lossy half-duplex channel (seeded PRNG drop model, including the
   "ACK lost while sender transmits" collision case identified in
   `docs/wp_a_usb_backpressure_20260707.md`):
   - convergence: all datagrams delivered exactly once under 0–30% loss;
   - zero duplication to egress across duplicate-window wraparound;
   - credit starvation halts new DATA but not ACK/retransmit;
   - egress blocked → credits withdrawn → sender retries → no acknowledged
     loss, then recovery when egress reopens;
   - retry exhaustion surfaces failure status and frees the buffer;
   - 30 × 1280 B zero-gap burst through the simulated channel delivers 30/30
     (the collision loss the bench showed must be solved by design here,
     before hardware).

**Done when:** all of the above green natively with multiple seeds.

## Phase 5 — Parser fuzz corpus (gate requirement)

1. `firmware/test/test_fuzz_v3/`: seeded deterministic fuzzer feeding the v3
   parsers ≥ 100k inputs per run: random bytes, truncated valid packets,
   bit-flipped valid packets, duplicated/reordered fragments into a live
   engine. Assertions: no crash, no out-of-bounds (index-check helpers), no
   accepted packet with bad CRC, engine counters account for every rejected
   input.
2. Add a crafted-vector corpus (hex arrays in the test file) for every parser
   rejection branch, so coverage survives PRNG changes.
3. Optional but recommended: a second native run with
   `-fsanitize=address,undefined` added to a `native-asan` env in
   `platformio.ini`; document the result either way.

## Phase 6 — MAC integration (`firmware/src/mac/Mac.cpp`)

The invasive step. Run GitNexus impact analysis on the touched MAC symbols
first; keep the event-driven single-owner structure; WP-C's TDD scheduling is
**out of scope** — the engine slots in behind the existing
event/deadline loop.

1. Raise `TNC_PAYLOAD_MAX_LEN` to 1280 (`framing/Framing.h`) or introduce a
   v3-specific constant and migrate consumers. Audit every dependent size:
   `PayloadFrame`, queue item sizes, `SERIAL_KISS_ENCODED_MAX`
   (~2 × 1288 + 3), `sendControlResponse`'s buffer, serial task stack sizes
   (`wrapperBuf` lives on `serialTxTask`'s stack — verify via the existing
   stack HWM telemetry `hwmSrx`/`hwmStx` after boot).
2. Replace the v2 generic-fragmented ARQ path (`s_tx`/`s_ra` state machines,
   warmup, `handleAckForTx`, `handleAckDue`) with two `ArqEngine` bindings;
   keep: LBT, staged config commands, watchdog feeding, `computeWaitMs`
   deadline aggregation (feed it the engine's next deadline), egress
   reservation via `rxQueue` capacity → engine credits.
3. Map heartbeat/link-state onto v3 CONTROL packets; preserve the current
   scheduling discipline (data-first in `serviceIdle`, heartbeat only when
   `isLinkIdle()`) — the burst analysis showed this ordering is correct.
4. Version-mismatch and unknown-type discards increment new counters; extend
   `STATS` with the v3/ARQ counter block (text command; TLV MGMT is WP-D).
5. Decide and document the fate of `TransportMode::NATIVE_PACKET`: keep as a
   debug path if it costs nothing, otherwise delete — but `GENERIC_FRAGMENTED`
   semantics are fully replaced by v3; no v2 fallback remains.
6. `pio run -e t3s3` and `-e t3s3-serial-wdt` must both build; native suite
   stays green (v2 framing tests are deleted together with the code they
   test — port any still-relevant cases to v3).

## Phase 7 — Bench tooling (minimum viable; Rust daemon is WP-D)

1. Update `pi-daemon/raw_fragment_test.py` and `serial_integrity.py` for
   1280 B payloads; add per-datagram SHA-256 (or CRC) echo verification and a
   duplicate detector to the listener — the gate is *zero corruption or
   duplication*, so the tool must positively detect both.
2. Add an induced-loss knob for the bench: lowest conducted TX power +
   antenna attenuation (bench convention: both boards at matching low power,
   as in the WP-A runs), or a `--interval-ms 0` abusive burst which the WP-A
   analysis showed reliably creates collision loss.
3. `kiss_tun.py` MTU stays 127-era until WP-D only if it doesn't interfere
   with bench scripts; otherwise bump its MTU constant to 1280 in passing and
   note it.

## Phase 8 — Hardware regression gate

1. Flash **both** nodes with the same image (kill `kiss_tun.py` first).
2. Runs (record all STATS before/after, note firmware commit hash):
   - 30 × 1280 B, zero-gap, each direction: expect materially better than the
     16–18/30 v2 baseline; **hard gate: everything delivered is delivered
     exactly once and uncorrupted** (delivery *rate* under abuse is
     WP-C's problem, but the Phase 4 channel tests should already have it
     near 30/30);
   - induced-loss paced run, both directions concurrently;
   - egress-blockage run (listener stops reading mid-burst): credits withdraw,
     no acknowledged loss, recovery on resume;
   - control-path probe (STATS/SET) responsive after every run.
3. Write `docs/wp_b_bench_<date>.md` in the style of the WP-A regression doc;
   update `changelog.md`.

## Phase 9 — Close-out

1. WP-B gate checklist (all must hold): native suite green including fuzz
   corpus; two-board 1280 B exchange under induced loss with zero
   corruption/duplication; no acknowledged loss under egress blockage; both
   env builds clean.
2. Merge `wp-b-v3-framing` to master, re-run `npx gitnexus analyze`, verify
   the `legacy-v2-framing` tag is pushed (rollback path).
3. Update `CLAUDE.md` architecture notes (v3 replaces v2 generic framing) and
   note WP-C as next.

## Explicit non-goals (do not build these now)

- TDD master/slave grants, airtime adaptation, burst scheduling (WP-C) — only
  the reserved CONTROL subtype enum values.
- PHY profile switching handshake (WP-E).
- Rust daemon, TLV management, data envelope on port 0 (WP-D).
- Multi-node addressing, encryption (deferred by programme scope).
