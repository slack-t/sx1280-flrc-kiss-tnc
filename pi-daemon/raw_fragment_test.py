#!/usr/bin/env python3
"""
raw_fragment_test.py - One-way raw KISS fragmentation test helper.

Run `listen` on one node and `send` on the other. This intentionally avoids
the IP/TUN path so Generic fragmented ARQ can be tested without ping replies or
bidirectional host traffic creating extra contention.
"""

import argparse
import collections
import struct
import time
import zlib

import serial
import serial_integrity

from kiss_tun import (
    FIRMWARE_PAYLOAD_CAP,
    KISS_DATA_PORT,
    KissDecoder,
    kiss_encode,
    write_kiss_frame,
)
from raw_kiss import open_serial


MAGIC = b"RKFT"
VERSION = 1
HEADER = struct.Struct(">4sBIHI")
HEADER_LEN = HEADER.size
FRAG_DATA = 114
SERIAL_HDR_LEN = serial_integrity.SERIAL_INTEGRITY_HDR_LEN
DEFAULT_SIZES = "105,106,107,219,220,221,333,334,335,447,448,449"


def parse_sizes(text: str) -> list[int]:
    sizes = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        size = int(part, 10)
        if size < HEADER_LEN:
            raise ValueError(f"size {size} is smaller than test header {HEADER_LEN}")
        if size > FIRMWARE_PAYLOAD_CAP:
            raise ValueError(f"size {size} exceeds firmware payload cap {FIRMWARE_PAYLOAD_CAP}")
        sizes.append(size)
    if not sizes:
        raise ValueError("at least one size is required")
    return sizes


def fragment_count(size: int) -> int:
    return (size + SERIAL_HDR_LEN + FRAG_DATA - 1) // FRAG_DATA


def pattern(seq: int, body_len: int) -> bytes:
    return bytes(((seq * 31 + i * 17) & 0xFF) for i in range(body_len))


def build_payload(seq: int, size: int) -> bytes:
    body_len = size - HEADER_LEN
    body = pattern(seq, body_len)
    crc = zlib.crc32(body) & 0xFFFFFFFF
    return HEADER.pack(MAGIC, VERSION, seq, size, crc) + body


def parse_payload(payload: bytes) -> tuple[int, int, bool, str]:
    if len(payload) < HEADER_LEN:
        return (0, len(payload), False, "short")

    magic, version, seq, declared_size, expected_crc = HEADER.unpack_from(payload, 0)
    if magic != MAGIC:
        return (seq, len(payload), False, "foreign")
    if version != VERSION:
        return (seq, len(payload), False, f"version={version}")
    if declared_size != len(payload):
        return (seq, len(payload), False, f"len={len(payload)} declared={declared_size}")

    body = payload[HEADER_LEN:]
    actual_crc = zlib.crc32(body) & 0xFFFFFFFF
    if actual_crc != expected_crc:
        return (seq, len(payload), False, "crc")
    if body != pattern(seq, len(body)):
        return (seq, len(payload), False, "pattern")
    return (seq, len(payload), True, "ok")


def run_send(args: argparse.Namespace) -> int:
    sizes = parse_sizes(args.sizes)
    try:
        ser = open_serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as exc:
        raise SystemExit(f"cannot open {args.port}: {exc}") from exc

    if args.boot_wait > 0:
        time.sleep(args.boot_wait)

    seq = args.start_seq
    sent = 0
    try:
        for _ in range(args.count):
            for size in sizes:
                payload = build_payload(seq, size)
                wrapped = serial_integrity.wrap_payload(payload)
                frame = kiss_encode(wrapped)
                written = write_kiss_frame(
                    ser,
                    frame,
                    chunk_size=args.write_chunk,
                    gap_s=args.write_gap_ms / 1000.0,
                )
                sent += 1
                print(
                    f"TX seq={seq} len={size} frags={fragment_count(size)} "
                    f"encoded={len(frame)} written={written}",
                    flush=True,
                )
                seq += 1
                if args.interval_ms > 0:
                    time.sleep(args.interval_ms / 1000.0)
    finally:
        ser.close()

    print(f"TX summary frames={sent} sizes={','.join(str(s) for s in sizes)}", flush=True)
    return 0


def run_listen(args: argparse.Namespace) -> int:
    try:
        ser = open_serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as exc:
        raise SystemExit(f"cannot open {args.port}: {exc}") from exc

    if args.boot_wait > 0:
        time.sleep(args.boot_wait)

    decoder = KissDecoder(FIRMWARE_PAYLOAD_CAP + serial_integrity.SERIAL_INTEGRITY_HDR_LEN)
    by_size: dict[int, int] = collections.defaultdict(int)
    bad_reasons: dict[str, int] = collections.defaultdict(int)
    seen: set[int] = set()
    total = 0
    bad = 0
    first_seq = None
    last_seq = None
    deadline = time.monotonic() + args.idle_timeout

    try:
        while True:
            waiting = ser.in_waiting
            if waiting:
                data = ser.read(waiting)
                for port, payload in decoder.feed(data):
                    if port != KISS_DATA_PORT:
                        continue
                    try:
                        payload = serial_integrity.unwrap_payload(payload)
                    except ValueError as e:
                        bad += 1
                        bad_reasons[f"serial_{e.__class__.__name__}"] += 1
                        print(f"RX serial integrity drop: {e}", flush=True)
                        continue
                        
                    seq, size, ok, reason = parse_payload(payload)
                    total += 1
                    if ok:
                        seen.add(seq)
                        by_size[size] += 1
                        first_seq = seq if first_seq is None else min(first_seq, seq)
                        last_seq = seq if last_seq is None else max(last_seq, seq)
                    else:
                        bad += 1
                        bad_reasons[reason] += 1
                    print(
                        f"RX seq={seq} len={size} frags={fragment_count(size)} "
                        f"status={reason}",
                        flush=True,
                    )
                    deadline = time.monotonic() + args.idle_timeout

            if args.expected and total >= args.expected:
                break
            if time.monotonic() >= deadline:
                break
            time.sleep(0.01)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    missing = 0
    if first_seq is not None and last_seq is not None:
        missing = sum(1 for seq in range(first_seq, last_seq + 1) if seq not in seen)

    print("RX summary", flush=True)
    print(f"  frames={total} valid={len(seen)} bad={bad} missing_in_range={missing}", flush=True)
    if first_seq is not None:
        print(f"  seq_range={first_seq}..{last_seq}", flush=True)
    for size in sorted(by_size):
        print(f"  len={size} frags={fragment_count(size)} count={by_size[size]}", flush=True)
    for reason in sorted(bad_reasons):
        print(f"  bad_{reason}={bad_reasons[reason]}", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="One-way raw KISS fragmentation test helper"
    )
    parser.add_argument("--port", default="/dev/ttyACM0", help="Serial port")
    parser.add_argument("--baud", type=int, default=921600, help="Baud rate")
    parser.add_argument("--boot-wait", type=float, default=0.0,
                        help="Seconds to wait after opening USB CDC")

    sub = parser.add_subparsers(dest="command", required=True)

    send = sub.add_parser("send", help="Send deterministic raw KISS payloads")
    send.add_argument("--sizes", default=DEFAULT_SIZES,
                      help=f"Comma-separated payload sizes (default: {DEFAULT_SIZES})")
    send.add_argument("--count", type=int, default=5,
                      help="Number of sweeps over --sizes")
    send.add_argument("--interval-ms", type=float, default=250.0,
                      help="Delay between sent frames")
    send.add_argument("--start-seq", type=int, default=1,
                      help="First test sequence number")
    send.add_argument("--write-chunk", type=int, default=32,
                      help="Serial write chunk size for encoded KISS frames")
    send.add_argument("--write-gap-ms", type=float, default=2.0,
                      help="Delay between serial write chunks")

    listen = sub.add_parser("listen", help="Receive and validate raw KISS payloads")
    listen.add_argument("--idle-timeout", type=float, default=10.0,
                        help="Stop after this many idle seconds")
    listen.add_argument("--expected", type=int, default=0,
                        help="Stop after receiving this many frames")

    args = parser.parse_args()
    if args.command == "send":
        return run_send(args)
    return run_listen(args)


if __name__ == "__main__":
    raise SystemExit(main())
