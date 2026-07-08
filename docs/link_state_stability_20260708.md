# Link-State Stability Fixes — 2026-07-08

The modem's reported link state flapped between READY, ERROR, and DOWN on both
the OLED and `STATS`, even while ARQ data transfer was flawless. This work
makes the reported state track reality without adding any airtime: all changes
are bookkeeping in the MAC and display layers; the wire protocol is untouched.

## Root Causes

1. **Only heartbeat ACKs counted as link proof.** `isLinkReady()` requires a
   bidirectional control exchange within `RADIO_LINK_READY_TTL_MS` (3 s), and
   only a HEARTBEAT_ACK to our own heartbeat (or a LINK_STATE control frame)
   refreshed that timestamp. A perfect ARQ exchange — DATA one way, bitmap
   ACKs the other — refreshed nothing.
2. **Data traffic suppresses heartbeats.** Heartbeats only fire when the link
   is idle, and the v3 pump returns early while ARQ has pending work. So any
   transfer longer than 3 s forced `linkState=DOWN` while data was flowing.
3. **Mutual heartbeat starvation after transfers.** The heartbeat gate used
   `isLinkIdle()` (any link activity within 500 ms defers), and
   `sendArqRadioPacket()` marked *payload* activity for every packet type it
   sent — including CONTROL replies. Net effect: answering the peer's
   heartbeats deferred our own, and the two boards could starve each other's
   heartbeat schedules for ~10 s after a burst (observed on hardware).
4. **Sticky ERROR label.** The display showed ERROR whenever
   `radioState == ERROR`, which any transient radio error sets and which
   persists until the next TX/RX state transition overwrites it.
5. **No intermediate state.** `LinkState::PROBING` existed in the enum but was
   never set; the reported state jumped straight READY → DOWN after one 3 s
   confirmation gap (2–3 lost heartbeat exchanges on a noisy 2480 MHz channel).

## Changes (`firmware/src/`)

- `mac/Mac.cpp` — a parseable v3 ACK from the peer now calls
  `noteBidirectionalControl()`: it proves the peer heard our DATA and we heard
  its reply. Completing an inbound datagram (`deliverArqDatagram`) counts too:
  beyond the first credit window the sender cannot keep opening new datagrams
  unless our ACKs reach it. Together these hold READY through transfers.
- `mac/Mac.cpp` — heartbeat gates now use `isPayloadIdle()` instead of
  `isLinkIdle()`, and `sendArqRadioPacket()` only marks payload activity for
  DATA/ACK types (CONTROL marks link activity). Control chatter can no longer
  defer the heartbeat schedule; heartbeats resume within ~1.5 s of a transfer
  ending instead of ~10 s.
- `mac/Mac.cpp` (`refreshLinkStats`) — three-state reporting with hysteresis:
  READY within `RADIO_LINK_READY_TTL_MS` (3 s), then PROBING until
  `RADIO_LINK_PROBE_TTL_MS` (10 s, new), then DOWN. `linkReady` semantics are
  unchanged (still the 3 s bound). `STATS` can now report
  `linkState=PROBING`.
- `stats/Stats.h`, `mac/Mac.cpp` — new `lastRadioErrorMs` records when the
  last radio error occurred.
- `display/Display.cpp` — the ERROR label now requires a radio error within
  `RADIO_ERROR_HOLD_MS` (3 s, new); stale errors no longer pin the header.
- `config.h` — added `RADIO_LINK_PROBE_TTL_MS` (10000) and
  `RADIO_ERROR_HOLD_MS` (3000).

## Verification (two T3S3 boards on this host)

`pio test -e native`: 73/73 pass. Both `t3s3` and `t3s3-serial-wdt` build.

Final gate (diagnostic image, both boards): 30 × 1000 B zero-gap burst from
`/dev/ttyACM0` to a continuously draining listener on `/dev/ttyACM1`, with the
sender polling `STATS` every 2 s through the transfer and 30 s beyond:

- 18/18 polls reported `linkState=READY`; `linkAgeMs` never exceeded 1.6 s.
- Receiver delivered 30/30 unique frames, zero corruption/duplicates/drops.
- Heartbeats exchanged steadily (~1/s) after the transfer with no starvation.

Before the fixes, the same procedure showed DOWN during transfers and ~10 s
READY/DOWN flaps after them.

Note on test methodology: polling STATS from a host that does not also drain
data frames produces stale, pipelined STATS responses on a board with queued
USB egress — receiver-side numbers from such a poller are time-smeared and
unreliable. Use a draining listener (as `raw_fragment_test.py listen` does)
when reading receiver-side stats.

## Known Issue Discovered (pre-existing, unrelated to these changes)

One test run wedged the sender permanently mid-burst (`arqV3TxDone` frozen at
16/30, heartbeats dead, RX path alive) when the receiving host drained USB
slowly: the receiver's `rxQueue` momentarily filled (`rxQWait=1`), an ACK
carried zero egress credits, and the sender then had **no retry deadline and
no credit-recovery mechanism** — `hasPendingWork()` stayed true forever, which
also blocks heartbeats in `serviceIdle`. This is the ARQ credit-starvation
deadlock the WP-B "egress-blockage recovery gate" was meant to catch; that
gate is still outstanding. Fix candidates: sender-side credit-probe timer
when credits are exhausted, and/or receiver-initiated credit refresh when
egress capacity returns. Tracked as the top WP-B follow-up before WP-C.
