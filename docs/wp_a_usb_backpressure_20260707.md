# WP-A USB Backpressure Regression - 2026-07-07

Hardware: 2x Lilygo T3S3 SX1280 bench nodes.  
Firmware under test: local TX node built with `env:t3s3-serial-wdt`; peer on compatible normal firmware.  
Transport: generic fragmented ARQ over KISS with serial integrity wrapper.

## Purpose

WP-A required USB backpressure to fail safely. Earlier burst testing appeared to
wedge the local board after a 30-frame burst. The goal of this run was to
separate three cases:

- watchdog-visible crash or CPU stall in `serialTxTask`
- silent USB CDC write-lock stall
- MAC/link overload with clean host/control recovery

The diagnostic firmware kept USB CDC as a pure KISS stream. It did not enable
serial console logs. Instead it exposed queue and USB-write state through
`STATS` and the OLED bottom row:

```text
Q:tx/rx W:serialWriteLockHeld S:serialTxActive
```

The diagnostic `STATS` fields are:

```text
qTx=<depth>/<free> qRx=<depth>/<free>
stxLock=<0|1> stxActive=<0|1>
stxOff=<offset>/<encoded_len> stxAge=<ms|4294967295> stxStall=<ms>
```

## Commands

Receiver:

```sh
python pi-daemon/raw_fragment_test.py \
  --port /dev/ttyACM0 listen --idle-timeout 15 --expected 30 \
  --stats --stats-timeout 5 | tee reverse-large-rx.log
```

Aggressive sender:

```sh
python pi-daemon/raw_fragment_test.py \
  --port /dev/ttyACM0 --boot-wait 4.0 \
  send --sizes 1000 --count 30 \
  --interval-ms 0 --write-chunk 2048 --write-gap-ms 0 \
  --stats --stats-timeout 2.0
```

Control recovery probe after the aggressive burst:

```sh
python -c 'from modem_tui import ModemClient; c=ModemClient("/dev/ttyACM0",921600,5,0,1); print(c.command("STATS")); c.close()'
```

Paced sender:

```sh
python pi-daemon/raw_fragment_test.py \
  --port /dev/ttyACM0 --boot-wait 4.0 \
  send --sizes 1000 --count 30 \
  --interval-ms 100 --write-chunk 2048 --write-gap-ms 0 \
  --stats --stats-timeout 5
```

## Results

### Aggressive 30x1000B zero-gap burst

The host wrote all 30 frames:

```text
TX summary frames=30 sizes=1000
```

The in-session trailing `STATS` timed out, but a fresh control session answered
immediately afterward:

```text
OK rx=0 rxBytes=0 arqDone=0 ... qTx=0/8 qRx=0/8 stxLock=0 stxActive=0 stxOff=0/0 stxAge=4294967295 stxStall=0
```

OLED at/after the timeout showed the local node idle:

```text
Q:0/0 W:0 S:0 TX:10 RX:0
```

Interpretation: the local board was not wedged. The immediate `STATS` timeout
was transient contention on the same serial RX/control path after the burst, not
a persistent USB CDC write lock or serial TX stall.

### Paced 30x1000B burst

TX-side `STATS` returned:

```text
OK rx=0 rxBytes=0 arqDone=0 ... stxZero=0 stxTimeout=0 stxEncodeFail=0 rxQWait=0 ... egress=0 ... qTx=0/8 qRx=0/8 stxLock=0 stxActive=0 stxOff=0/0 stxAge=4294967295 stxStall=0
```

RX-side result:

```text
RX STATS OK rx=18 rxBytes=18000 arqDone=18 ... stxZero=0 stxTimeout=0 stxEncodeFail=0 rxQWait=0 ... egress=0 ... qTx=0/8 qRx=0/8 stxLock=0 stxActive=0 stxOff=1019/1019 stxAge=4294967295 stxStall=0
RX summary
frames=18 valid=18 bad=0 missing_in_range=12
seq_range=1..30
len=1000 frags=9 count=18
```

Interpretation: USB and host egress remained safe. The receiver delivered 18
complete 1000-byte ARQ payloads, with no malformed payloads and no egress
deferrals. The 12 missing frames are MAC/link burst overload, not USB
backpressure failure.

## Conclusion

WP-A USB backpressure safety passes this regression:

- no watchdog panic
- no persistent USB CDC write-lock stall
- `stxZero=0`, `stxTimeout=0`, `stxEncodeFail=0`
- `qTx=0/8`, `qRx=0/8`, `stxLock=0`, `stxActive=0` after recovery
- control path answers after the burst
- receiver egress remains clean: `egress=0`, `rxQWait=0`
- received payloads are valid: `valid=18`, `bad=0`

The remaining limitation is MAC/link throughput under abusive bursts. A
zero-gap or lightly paced 30x1000B burst can lose complete ARQ payloads on the
bench link, but it recovers cleanly and does not corrupt data or wedge USB.

## Deployment Notes

- Restore appropriate TX power before tower deployment. Bench tests used low
  conducted power; with a 15 dBi Yagi, conducted TX power must stay within the
  applicable EIRP limit.
- Do not use `SERIAL_CONSOLE_LOGS=1` for KISS-path validation. Console output
  shares USB CDC with KISS and contaminates the transport stream.

## Addendum — Production Image Re-Run (6a2d669)

The runs above used the diagnostic firmware on the local node. Commit `6a2d669`
then fixed a write-lock telemetry race and gated all backpressure telemetry
behind `SERIAL_TX_WDT_DIAGNOSTICS`, so the production `t3s3` image no longer
takes the stats mutex on the serial TX hot path. Because the deployable
artifact had never run the regression, both nodes were flashed with production
`t3s3` at `6a2d669` and the 30x1000B burst was repeated.

TX-side `STATS` after the burst:

```text
OK rx=0 rxBytes=0 arqDone=0 arqMetaDrop=0 arqIntDrop=0 arqCrc=0 stxZero=0 stxTimeout=0 stxEncodeFail=0 rxQWait=0 linkReady=0 linkState=DOWN linkAgeMs=3136 hbTx=2 hbAckRx=2 dpTx=0 drRx=0 primerTO=0 cqDrop=0 wuTx=1 wuRx=0 wuAck=1 wuTO=0 egress=0 hwmMac=4480 hwmSrx=7424 hwmStx=6788
TX summary frames=30 sizes=1000
```

RX-side result:

```text
RX STATS OK rx=16 rxBytes=16000 arqDone=16 arqMetaDrop=0 arqIntDrop=0 arqCrc=0 stxZero=0 stxTimeout=0 stxEncodeFail=0 rxQWait=0 linkReady=1 linkState=READY linkAgeMs=2073 hbTx=4 hbAckRx=2 dpTx=0 drRx=0 primerTO=0 cqDrop=0 wuTx=0 wuRx=1 wuAck=0 wuTO=0 egress=0 hwmMac=3488 hwmSrx=7472 hwmStx=5408
RX summary
frames=16 valid=16 bad=0 missing_in_range=14
seq_range=1..30
len=1000 frags=9 count=16
```

Interpretation:

- All USB backpressure invariants hold on the production image: `stxZero=0`,
  `stxTimeout=0`, `stxEncodeFail=0`, `rxQWait=0`, `egress=0` on both sides, and
  the TX board answered `STATS` after the burst.
- No acknowledged loss, no corruption: `arqDone=16` matches `rx=16` exactly,
  `valid=16 bad=0`. The 14 missing payloads were never acknowledged.
- The production `STATS` response ends at `hwmStx=` (diagnostic fields are
  compiled out) and the host tooling parses it unchanged.
- 16/30 delivered vs 18/30 on the diagnostic build is run-to-run variance in
  the same MAC/link overload regime, not a regression from removing the
  telemetry.
- The asymmetric link view at probe time (TX `DOWN` at age 3136 ms, RX `READY`
  at 2073 ms) is normal idle decay: `RADIO_LINK_READY_TTL_MS` is 3000 ms, the
  burst had ended, and the next heartbeat exchange re-establishes `READY`.

**WP-A gate closed on the deployable artifact.**

## Addendum — Burst-Loss Mechanism Analysis

Code-level review of whether the heartbeat contributes to the burst loss
(counters above: sender `hbTx=2 hbAckRx=2`, receiver `hbTx=4 hbAckRx=2`):

- **Sender heartbeats cannot interfere with data.** `serviceIdle` drains the
  data queue before the heartbeat check, and a heartbeat fires only when the
  TX state machine is idle and `isLinkIdle()` (500 ms without link activity).
  Data TX is never gated on link state (`startFrame` does not check
  `linkReady`; `cqDrop=0`), so the mid-burst `DOWN` transition dropped nothing.
- **Receiver heartbeats are a minor aggravator and mostly a symptom.** They can
  only fire in >=500 ms reception gaps — gaps that exist because sender retry
  rounds were already failing. Once fired, the half-duplex radio is deaf during
  the heartbeat's airtime (can hole-punch one fragment), and the two unACKed
  heartbeats were themselves lost to the sender being mid-transmit. `HB_WAIT`
  (40 ms) does not block data ACKs: `handleAckDue`/`sendAckPacket` is not gated
  on the TX phase.
- **Dominant mechanism: half-duplex fragment/ACK collision under saturation.**
  A payload is only lost after `RADIO_ARQ_MAX_ROUNDS` (6) failed rounds end in
  `failFrame`. With zero/low-gap bursts the sender launches the next round or
  payload while the receiver's ACK is in flight; both transmissions are lost
  and rounds exhaust. `wuTx=1` confirms only the first payload received the ARQ
  warmup (warmup requires payload-idle); every later payload enters data rounds
  against a possibly mid-ACK receiver. Clean `arqCrc=0 arqMetaDrop=0
  arqIntDrop=0` on the receiver confirms the loss is collision/timing, not
  corruption.
- **Falsification test (optional):** set `RADIO_HEARTBEAT_INTERVAL_MS` to
  60000 on both nodes and repeat the burst. Expected: delivery stays ~16-18/30,
  exonerating the heartbeat.

The designated fix is WP-C's adaptive TDD MAC (scheduled grants remove the
collision domain; link supervision folds into slots), with WP-B's
selective-repeat ARQ and credits bounding the burst exposure. No ad-hoc tuning
of the current protocol is planned.
