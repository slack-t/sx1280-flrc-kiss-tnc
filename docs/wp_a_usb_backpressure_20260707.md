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
