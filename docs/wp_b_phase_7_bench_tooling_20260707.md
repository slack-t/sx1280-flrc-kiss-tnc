# WP-B Phase 7 Bench Tooling Notes - 2026-07-07

Phase 7 updates the Python host tools so the Phase 8 hardware gate can measure
1280-byte v3 generic transport without relying on the old v2 fragment limits.

## Tooling Changes

- `pi-daemon/serial_integrity.py` now defines a 1280-byte payload cap and
  rejects outbound or inbound serial-integrity payloads above that size. The
  USB KISS frame carries that payload plus the 8-byte integrity header.
- `pi-daemon/kiss_tun.py` derives `FIRMWARE_PAYLOAD_CAP` from the serial
  integrity cap and defaults its optional IP/TUN MTU to 1280. It now rejects
  a configured MTU above the firmware payload cap at startup.
- `pi-daemon/raw_fragment_test.py` uses the v3 fragment payload size for its
  fragment-count estimates and includes 1280-byte payloads in the default
  sweep.
- `raw_fragment_test.py listen` reports per-datagram CRC32 and a SHA-256
  prefix, tracks unique sequence numbers, and separates duplicate deliveries
  from corrupt payloads. The Phase 8 gate should require `bad=0`,
  `duplicates=0`, `duplicate_conflicts=0`, and `missing_in_range=0`.
- `raw_fragment_test.py send --stress-1280` is the induced-loss shortcut for
  the WP-A-style abusive burst: 30 x 1280-byte payloads with zero inter-frame
  gap.
- `pi-daemon/kiss_bench.py` documentation now reflects the 1271-byte generic
  bench payload ceiling after its 9-byte bench header.

## Phase 8 Bench Commands

Run the listener on the receiving node:

```sh
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 listen \
  --idle-timeout 20 --expected 30 --stats
```

Run the sender on the transmitting node:

```sh
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 send \
  --stress-1280 --stats
```

Equivalent explicit command:

```sh
python3 pi-daemon/raw_fragment_test.py --port /dev/ttyACM0 send \
  --sizes 1280 --count 30 --interval-ms 0 --stats
```

For paced boundary sweeps, omit `--stress-1280` and use the default size set.

## Verification

Host-only checks:

```sh
python3 -m py_compile \
  pi-daemon/raw_fragment_test.py \
  pi-daemon/serial_integrity.py \
  pi-daemon/kiss_tun.py \
  pi-daemon/kiss_bench.py

python3 -m unittest tests.test_kiss_conformance tests.test_serial_integrity
```

These checks validate parser syntax, KISS decoder behavior at the raised cap,
and serial integrity acceptance/rejection at the 1280-byte boundary. Hardware
delivery rate, corruption, duplication, egress blockage, and control-path
responsiveness remain Phase 8 board tests.
