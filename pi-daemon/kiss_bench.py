#!/usr/bin/env python3
"""
kiss_bench.py — Link quality and throughput benchmark for the SX1280 KISS TNC.

Run echo mode on one node and bench/sweep mode on the other.

  Echo node:   python kiss_bench.py --port /dev/ttyACM0 --echo
  Bench node:  python kiss_bench.py --port /dev/ttyACM1
  Bench node:  python kiss_bench.py --port /dev/ttyACM1 --size 116 --count 200
  Sweep node:  python kiss_bench.py --port /dev/ttyACM1 --sweep

Wire protocol (KISS data port 0):
  Bytes 0-3   magic  0xBEEFCAFE
  Byte  4     type   0x01=PING  0x02=PONG
  Bytes 5-6   seq    uint16 big-endian
  Bytes 7-8   size   uint16 big-endian payload length
  Bytes 9+    payload (deterministic pattern, size bytes)

Max bench payload:
  Native transport  116 bytes  (125 byte FLRC packet - 9 byte header)
  Generic/ARQ       1015 bytes (1024 byte firmware cap - 9 byte header)
"""

import argparse
import statistics
import struct
import sys
import time

import serial

from kiss_tun import FIRMWARE_PAYLOAD_CAP, KISS_DATA_PORT, KissDecoder, kiss_encode

# ── Protocol constants ────────────────────────────────────────────────────────
BENCH_MAGIC    = b"\xBE\xEF\xCA\xFE"
BENCH_PING     = 0x01
BENCH_PONG     = 0x02
BENCH_HDR_LEN  = 9   # magic(4) + type(1) + seq(2) + size(2)

# SX1280 FLRC single-packet capacity in native transport mode.
NATIVE_MAX_PAYLOAD = 125
BENCH_NATIVE_MAX   = NATIVE_MAX_PAYLOAD - BENCH_HDR_LEN   # 116

# Default sizes for sweep: covers the full native-mode range.
SWEEP_SIZES_DEFAULT = [16, 32, 48, 64, 96, BENCH_NATIVE_MAX]


# ── Serial helpers ────────────────────────────────────────────────────────────

def open_serial(port: str, baud: int) -> serial.Serial:
    ser = serial.Serial()
    ser.port     = port
    ser.baudrate = baud
    ser.timeout  = 0
    ser.dsrdtr   = False
    ser.rtscts   = False
    ser.dtr      = False
    ser.rts      = False
    ser.open()
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


# ── Frame builders / parsers ──────────────────────────────────────────────────

def _make_payload(size: int) -> bytes:
    return bytes(i & 0xFF for i in range(size))


def build_ping(seq: int, payload_size: int) -> bytes:
    hdr = BENCH_MAGIC + struct.pack(">BHH", BENCH_PING, seq, payload_size)
    return hdr + _make_payload(payload_size)


def build_pong(frame: bytes) -> bytes:
    """Flip type byte from PING to PONG; all other bytes unchanged."""
    return frame[:4] + struct.pack(">B", BENCH_PONG) + frame[5:]


def parse_bench_frame(data: bytes):
    """Return (type, seq, payload_size) or None if not a bench frame."""
    if len(data) < BENCH_HDR_LEN or data[:4] != BENCH_MAGIC:
        return None
    frame_type, seq, payload_size = struct.unpack_from(">BHH", data, 4)
    return frame_type, seq, payload_size


# ── KISS read helpers ─────────────────────────────────────────────────────────

def read_payloads(ser: serial.Serial, decoder: KissDecoder, deadline: float):
    """Yield KISS data-port payloads until deadline."""
    while time.monotonic() < deadline:
        waiting = ser.in_waiting
        if waiting:
            for port, payload in decoder.feed(ser.read(waiting)):
                if port == KISS_DATA_PORT:
                    yield payload
        else:
            time.sleep(0.001)


# ── Echo mode ─────────────────────────────────────────────────────────────────

def run_echo(ser: serial.Serial, verbose: bool) -> None:
    decoder = KissDecoder(FIRMWARE_PAYLOAD_CAP)
    echoed = 0
    echoed_bytes = 0
    foreign = 0

    print("Echo mode active — press Ctrl-C to stop.\n")
    try:
        while True:
            waiting = ser.in_waiting
            if not waiting:
                time.sleep(0.001)
                continue
            for port, payload in decoder.feed(ser.read(waiting)):
                if port != KISS_DATA_PORT:
                    continue
                parsed = parse_bench_frame(payload)
                if parsed is None:
                    foreign += 1
                    if verbose:
                        ts = time.strftime("%H:%M:%S")
                        print(f"  [{ts}] foreign frame len={len(payload)}")
                    continue
                frame_type, seq, payload_size = parsed
                if frame_type != BENCH_PING:
                    continue
                ser.write(kiss_encode(build_pong(payload)))
                echoed += 1
                echoed_bytes += len(payload)
                if verbose:
                    ts = time.strftime("%H:%M:%S")
                    print(f"  [{ts}] PING seq={seq:5d} size={payload_size:4d}B → PONG")
                else:
                    print(f"\r  Echoed {echoed} frames ({echoed_bytes} bytes)   ", end="", flush=True)
    except KeyboardInterrupt:
        print(f"\n\nEchoed {echoed} frames ({echoed_bytes} bytes).")
        if foreign:
            print(f"Ignored {foreign} non-bench frames.")


# ── Latency / delivery test ───────────────────────────────────────────────────

def run_latency(ser: serial.Serial,
                payload_size: int,
                count: int,
                timeout_s: float,
                gap_ms: int,
                verbose: bool) -> dict:
    """
    Send `count` PINGs of `payload_size` bytes sequentially, wait for each PONG.
    Returns result dict with delivery stats and RTT samples.
    """
    total_frame_size = BENCH_HDR_LEN + payload_size
    if total_frame_size > FIRMWARE_PAYLOAD_CAP:
        sys.exit(f"size {payload_size} + {BENCH_HDR_LEN}B header = {total_frame_size}B "
                 f"exceeds firmware cap ({FIRMWARE_PAYLOAD_CAP}B)")
    if payload_size > BENCH_NATIVE_MAX and not _warned_native[0]:
        print(f"  NOTE: size {payload_size}B > native-mode max ({BENCH_NATIVE_MAX}B) — "
              f"use generic/ARQ transport or reduce size", flush=True)
        _warned_native[0] = True

    decoder = KissDecoder(FIRMWARE_PAYLOAD_CAP)
    rtts: list[float] = []
    lost = 0
    seq = 0

    bar_width = 38
    t_start = time.monotonic()

    for i in range(count):
        seq = (seq + 1) & 0xFFFF
        ping_frame = build_ping(seq, payload_size)

        t_send = time.monotonic()
        ser.write(kiss_encode(ping_frame))
        deadline = t_send + timeout_s

        got_pong = False
        for payload in read_payloads(ser, decoder, deadline):
            parsed = parse_bench_frame(payload)
            if parsed is None:
                continue
            frame_type, rx_seq, _ = parsed
            if frame_type == BENCH_PONG and rx_seq == seq:
                rtt_ms = (time.monotonic() - t_send) * 1000.0
                rtts.append(rtt_ms)
                got_pong = True
                break

        if not got_pong:
            lost += 1

        if verbose:
            if got_pong:
                print(f"  seq={seq:5d}  RTT={rtts[-1]:6.1f}ms")
            else:
                print(f"  seq={seq:5d}  LOST (>{timeout_s:.1f}s)")
        else:
            done  = i + 1
            filled = int(bar_width * done / count)
            bar = "#" * filled + "-" * (bar_width - filled)
            print(f"\r  [{bar}] {done}/{count}", end="", flush=True)

        if gap_ms > 0 and i < count - 1:
            time.sleep(gap_ms / 1000.0)

    if not verbose:
        print()  # newline after progress bar

    t_end = time.monotonic()

    return {
        "size":       payload_size,
        "count":      count,
        "delivered":  count - lost,
        "lost":       lost,
        "rtts":       rtts,
        "wall_time":  t_end - t_start,
    }


_warned_native = [False]


def print_latency_result(r: dict) -> None:
    count     = r["count"]
    delivered = r["delivered"]
    lost      = r["lost"]
    rtts      = r["rtts"]
    wall      = r["wall_time"]
    size      = r["size"]

    pct = delivered / count * 100 if count else 0.0
    print(f"  Delivered   {delivered}/{count}  ({pct:.1f}%)")
    if lost:
        print(f"  Lost        {lost}/{count}  ({100 - pct:.1f}%)")

    if rtts:
        rtt_sorted = sorted(rtts)
        mean_ms    = statistics.mean(rtts)
        med_ms     = statistics.median(rtts)
        p95_ms     = rtt_sorted[max(0, int(len(rtts) * 0.95) - 1)]
        print(f"  RTT min/mean/median/p95/max  "
              f"{min(rtts):.1f} / {mean_ms:.1f} / {med_ms:.1f} / {p95_ms:.1f} / {max(rtts):.1f}  ms")

        total_bytes = delivered * (size + BENCH_HDR_LEN)
        eff_kbps    = total_bytes * 8 / wall / 1000 if wall > 0 else 0.0
        print(f"  Eff. tput   {eff_kbps:.1f} kbps  "
              f"({total_bytes} bytes in {wall:.2f}s, including losses and timeouts)")
    else:
        print("  (no echoes received — is the remote node in echo mode?)")


# ── Sweep mode ────────────────────────────────────────────────────────────────

def run_sweep(ser: serial.Serial,
              sizes: list[int],
              count: int,
              timeout_s: float,
              gap_ms: int) -> None:
    hdr = f"{'size':>6}  {'delivery':>9}  {'rtt_min':>8}  {'rtt_mean':>9}  {'rtt_p95':>8}  {'eff_kbps':>9}"
    print(hdr)
    print("-" * len(hdr))

    for size in sizes:
        print(f"  size={size}B ...", end="", flush=True)
        # Flush stale bytes between runs.
        _ = ser.read(ser.in_waiting)
        r = run_latency(ser, size, count, timeout_s, gap_ms, verbose=False)
        rtts      = r["rtts"]
        delivered = r["delivered"]
        pct       = delivered / count * 100 if count else 0.0
        wall      = r["wall_time"]

        if rtts:
            rtt_sorted = sorted(rtts)
            rtt_min    = min(rtts)
            rtt_mean   = statistics.mean(rtts)
            rtt_p95    = rtt_sorted[max(0, int(len(rtts) * 0.95) - 1)]
            tb         = delivered * (size + BENCH_HDR_LEN)
            eff_kbps   = tb * 8 / wall / 1000 if wall > 0 else 0.0
            print(f"\r{size:>6}  {pct:>8.1f}%  {rtt_min:>7.1f}ms  "
                  f"{rtt_mean:>8.1f}ms  {rtt_p95:>7.1f}ms  {eff_kbps:>8.1f}")
        else:
            print(f"\r{size:>6}  {pct:>8.1f}%"
                  + "  " + "-" * 42)


# ── Main ──────────────────────────────────────────────────────────────────────

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Link quality and throughput benchmark for the SX1280 KISS TNC",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "examples:\n"
            "  # echo server on remote node:\n"
            "  python kiss_bench.py --port /dev/ttyACM0 --echo\n\n"
            "  # 100-frame latency test at 64 bytes:\n"
            "  python kiss_bench.py --port /dev/ttyACM1 --size 64 --count 100\n\n"
            "  # full sweep across native-mode sizes:\n"
            "  python kiss_bench.py --port /dev/ttyACM1 --sweep\n\n"
            "  # sweep with custom sizes (generic/ARQ mode example):\n"
            "  python kiss_bench.py --port /dev/ttyACM1 --sweep --sweep-sizes 64,256,512,1015"
        ),
    )
    parser.add_argument("--port", default="/dev/ttyACM0",
                        help="Serial port (default: /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=921600,
                        help="Baud rate (ignored by USB CDC; default: 921600)")
    parser.add_argument("--boot-wait", type=float, default=0.5,
                        help="Seconds to wait after opening port (default: 0.5)")

    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--echo", action="store_true",
                      help="Echo server: reflect every PING back as a PONG (run on remote node)")
    mode.add_argument("--sweep", action="store_true",
                      help="Sweep multiple payload sizes and print a results table")

    parser.add_argument("--size", type=int, default=64,
                        help="Payload bytes for single test (default: 64; native max: 116)")
    parser.add_argument("--count", type=int, default=100,
                        help="Frames per test run (default: 100)")
    parser.add_argument("--timeout", type=float, default=2.0,
                        help="Per-frame echo timeout in seconds (default: 2.0)")
    parser.add_argument("--gap-ms", type=int, default=0,
                        help="Inter-frame gap in ms (default: 0 — send as fast as TNC accepts)")
    parser.add_argument("--sweep-sizes", default=None,
                        help="Comma-separated payload sizes for --sweep "
                             "(default: 16,32,48,64,96,116)")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Print one line per frame instead of a progress bar")

    args = parser.parse_args()

    try:
        ser = open_serial(args.port, args.baud)
    except serial.SerialException as exc:
        sys.exit(f"cannot open {args.port}: {exc}")

    if args.boot_wait > 0:
        time.sleep(args.boot_wait)

    try:
        if args.echo:
            run_echo(ser, verbose=args.verbose)

        elif args.sweep:
            if args.sweep_sizes:
                try:
                    sizes = [int(s.strip()) for s in args.sweep_sizes.split(",")]
                except ValueError:
                    sys.exit("--sweep-sizes must be a comma-separated list of integers")
            else:
                sizes = SWEEP_SIZES_DEFAULT

            bad = [s for s in sizes if BENCH_HDR_LEN + s > FIRMWARE_PAYLOAD_CAP]
            if bad:
                sys.exit(f"sizes {bad} + {BENCH_HDR_LEN}B header exceed firmware cap "
                         f"({FIRMWARE_PAYLOAD_CAP}B)")

            print(f"Sweep: {len(sizes)} sizes, count={args.count}/size, "
                  f"timeout={args.timeout}s, gap={args.gap_ms}ms\n")
            run_sweep(ser, sizes, args.count, args.timeout, args.gap_ms)

        else:
            print(f"Latency test: port={args.port}  size={args.size}B  "
                  f"count={args.count}  timeout={args.timeout}s  gap={args.gap_ms}ms\n")
            r = run_latency(ser, args.size, args.count, args.timeout,
                            args.gap_ms, verbose=args.verbose)
            print_latency_result(r)

    except KeyboardInterrupt:
        print("\nInterrupted.")
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
