# FLRC Link Validation Matrix

This matrix validates the link in layers: KISS transport, single-packet latency,
fragment-count behavior, and sustained UDP load. Run each test on a quiet bench
setup with both nodes flashed from the same commit.

## Baseline Setup

Use the quiet bridge mode for benchmark runs:

```sh
python pi-daemon/kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.1/30 --mtu 246 --quiet
```

On the peer, use the matching address:

```sh
python pi-daemon/kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.2/30 --mtu 246 --quiet
```

Recommended host queue settings for the validation run:

```sh
sudo ip link set dev tun0 mtu 246
sudo ip link set dev tun0 txqueuelen 10
sudo tc qdisc replace dev tun0 root fq_codel
sudo iptables -t mangle -A OUTPUT -o tun0 -p tcp --tcp-flags SYN,RST SYN -j TCPMSS --set-mss 206
```

For diagnostic runs only, add `--debug-ip` and remove `--quiet`.

## Automated Runner

After both bridges are running, the matrix can be executed from node A with:

```sh
python3 tests/link_validation.py 10.0.0.2
```

Before running the UDP sweep, start `iperf3` in server mode on node B:

```sh
iperf3 -s
```

Useful variants:

```sh
python3 tests/link_validation.py 10.0.0.2 --mtu 246
python3 tests/link_validation.py 10.0.0.2 --skip-iperf
python3 tests/link_validation.py 10.0.0.2 --ping-count 20 --skip-conformance
python3 tests/link_validation.py 10.0.0.2 --mtu 492 --ping-sizes 56,95,157,218,280,341,403,464
```

Reports are written to `reports/link_validation_<timestamp>.md`.

## Packet Size Reference

The current radio fragment payload is `123` bytes.

| Fragment count | Max IP size | Ping `-s` | UDP payload `iperf3 -l` |
| --- | ---: | ---: | ---: |
| 1F | 123 | 95 | 95 |
| 2F | 246 | 218 | 218 |
| 3F | 369 | 341 | 341 |
| 4F | 492 | 464 | 464 |

Notes:

- Ping `-s` is ICMP payload. IP size is `-s + 28`.
- UDP `iperf3 -l` is UDP payload. IP size is `-l + 28`.
- While the link is being stabilized, production traffic should stay at `MTU 246`
  unless the `3F` and `4F` tests pass reliably.

## Phase 1: KISS Conformance

Run on the development machine:

```sh
python3 -m unittest tests.test_kiss_conformance
cd pi-daemon-rust && cargo test
```

Pass criteria:

- All Python conformance tests pass.
- All Rust KISS tests pass.
- No generated `__pycache__` files are included in commits.

## Phase 2: Idle Ping Sanity

Run from node A to node B:

```sh
ping -c 50 -i 1.0 -s 56 10.0.0.2
ping -c 50 -i 1.0 -s 95 10.0.0.2
ping -c 50 -i 1.0 -s 157 10.0.0.2
ping -c 50 -i 1.0 -s 218 10.0.0.2
```

Pass criteria:

- `1F` and `2F` packet loss should be `0%` on a bench setup.
- No duplicate replies.
- RTT should not show multi-second tails under idle load.

## Phase 3: Fragment Boundary Characterization

Run after Phase 2 passes:

```sh
ping -c 30 -i 1.5 -s 280 10.0.0.2
ping -c 30 -i 1.5 -s 341 10.0.0.2
ping -c 30 -i 2.0 -s 403 10.0.0.2
ping -c 30 -i 2.0 -s 464 10.0.0.2
```

Pass criteria:

- Record loss and RTT for `3F` and `4F`; do not treat these as production-safe
  until they are consistently below `1%` loss on the bench.
- If `3F` is worse than `4F`, capture a diagnostic run with `--debug-ip` because
  that pattern suggests a deterministic state or framing issue.

## Phase 4: UDP Sustainable Rate Sweep

Test `1F` first:

```sh
iperf3 -c 10.0.0.2 -u -l 95 -b 8k
iperf3 -c 10.0.0.2 -u -l 95 -b 16k
iperf3 -c 10.0.0.2 -u -l 95 -b 24k
iperf3 -c 10.0.0.2 -u -l 95 -b 32k
```

Then test `2F`:

```sh
iperf3 -c 10.0.0.2 -u -l 218 -b 8k
iperf3 -c 10.0.0.2 -u -l 218 -b 16k
iperf3 -c 10.0.0.2 -u -l 218 -b 24k
iperf3 -c 10.0.0.2 -u -l 218 -b 32k
```

Pass criteria:

- Use the receiver result, not the sender summary.
- Stable means `0%` loss over 10 seconds on the bench.
- The highest passing bitrate is the current production ceiling for that packet
  size.

## Phase 5: Bidirectional Stress

Run ping while a low-rate UDP test is active:

```sh
ping -c 60 -i 1.0 -s 56 10.0.0.2
```

In another shell:

```sh
iperf3 -c 10.0.0.2 -u -l 95 -b 16k
```

Pass criteria:

- Ping must not show duplicate replies.
- Ping loss should remain `0%`.
- RTT tails should remain bounded and recover immediately after UDP stops.

## Reporting Template

Record each run with:

- Git commit on both nodes
- Firmware build date
- Bridge command line and MTU
- Test command
- Sender result
- Receiver result
- Loss percentage
- RTT min/avg/max for ping
- Notes on duplicates, malformed packets, or reconnects
