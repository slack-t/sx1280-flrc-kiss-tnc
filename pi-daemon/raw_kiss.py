#!/usr/bin/env python3
"""
raw_kiss.py — Raw data-port utility for the generic SX1280 KISS TNC.

Use this tool when you want to exercise the TNC with arbitrary binary payloads
instead of the optional IP/TUN adapters.
"""

import argparse
import string
import sys
import time

import serial
import serial_integrity
try:
    import termios
except ImportError:  # pragma: no cover - non-POSIX hosts
    termios = None

from kiss_tun import (
    FIRMWARE_PAYLOAD_CAP,
    KISS_DATA_PORT,
    KissDecoder,
    configure_serial_safety,
    kiss_encode,
    write_kiss_frame,
)


def format_ascii(data: bytes) -> str:
    printable = set(string.printable.encode("ascii"))
    return "".join(chr(b) if b in printable and b not in b"\r\n\t\x0b\x0c" else "." for b in data)


def open_serial(port: str, baud: int, timeout: float) -> serial.Serial:
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baud
    ser.timeout = timeout
    ser.dsrdtr = False
    ser.rtscts = False
    ser.dtr = False
    ser.rts = False
    configure_serial_safety(ser)
    ser.open()
    ser.dtr = False
    ser.rts = False
    if termios is not None and hasattr(ser, "fileno"):
        attrs = termios.tcgetattr(ser.fileno())
        attrs[2] &= ~termios.HUPCL
        termios.tcsetattr(ser.fileno(), termios.TCSANOW, attrs)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return ser


def parse_payload(args: argparse.Namespace) -> bytes | None:
    if args.send_hex:
        return bytes.fromhex(args.send_hex)
    if args.send_text is not None:
        return args.send_text.encode("utf-8")
    if args.stdin:
        return sys.stdin.buffer.read()
    return None


def print_frame(prefix: str, payload: bytes, show_ascii: bool) -> None:
    line = f"{prefix} len={len(payload)} hex={payload.hex()}"
    if show_ascii:
        line += f' ascii="{format_ascii(payload)}"'
    print(line, flush=True)


def receive_frames(ser: serial.Serial,
                   decoder: KissDecoder,
                   idle_timeout_s: float | None,
                   show_ascii: bool) -> int:
    received = 0
    deadline = None if idle_timeout_s is None else (time.monotonic() + idle_timeout_s)
    while True:
        waiting = ser.in_waiting
        if waiting:
            data = ser.read(waiting)
            for port, payload in decoder.feed(data):
                if port == KISS_DATA_PORT:
                    try:
                        if not getattr(decoder, "no_wrap", False):
                            payload = serial_integrity.unwrap_payload(payload)
                    except ValueError as e:
                        print(f"RX serial integrity drop: {e}")
                        continue
                    print_frame("RX", payload, show_ascii)
                    received += 1
                    if deadline is not None:
                        deadline = time.monotonic() + idle_timeout_s
            continue

        if deadline is not None and time.monotonic() >= deadline:
            return received
        time.sleep(0.01)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Raw data-port utility for the generic SX1280 KISS TNC"
    )
    parser.add_argument("--port", default="/dev/ttyACM0", help="Serial port")
    parser.add_argument("--baud", type=int, default=921600, help="Baud rate")
    parser.add_argument("--boot-wait", type=float, default=0.0,
                        help="Seconds to wait after opening the USB CDC port")
    parser.add_argument("--timeout", type=float, default=1.0,
                        help="Idle receive timeout after sending (0 disables receive)")
    parser.add_argument("--listen", action="store_true",
                        help="Keep listening for data frames until Ctrl-C")
    parser.add_argument("--ascii", action="store_true",
                        help="Show a printable ASCII view next to the hex dump")
    parser.add_argument("--send-hex", default=None,
                        help="Hex payload to send on KISS data port 0")
    parser.add_argument("--send-text", default=None,
                        help="UTF-8 text payload to send on KISS data port 0")
    parser.add_argument("--stdin", action="store_true",
                        help="Read the outbound payload from stdin")
    parser.add_argument("--no-wrap", action="store_true",
                        help="Do not wrap/unwrap payloads in the serial integrity header (for NATIVE mode)")
    args = parser.parse_args()

    payload = parse_payload(args)
    if payload is not None and len(payload) > FIRMWARE_PAYLOAD_CAP:
        sys.exit(
            f"payload too large ({len(payload)} > firmware payload cap {FIRMWARE_PAYLOAD_CAP})"
        )

    try:
        ser = open_serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as exc:
        sys.exit(f"cannot open {args.port}: {exc}")

    if args.boot_wait > 0:
        time.sleep(args.boot_wait)

    cap = FIRMWARE_PAYLOAD_CAP if args.no_wrap else FIRMWARE_PAYLOAD_CAP + serial_integrity.SERIAL_INTEGRITY_HDR_LEN
    decoder = KissDecoder(cap)
    decoder.no_wrap = args.no_wrap
    try:
        if payload is not None:
            if not args.no_wrap:
                wrapped = serial_integrity.wrap_payload(payload)
            else:
                wrapped = payload
            frame = kiss_encode(wrapped)
            write_kiss_frame(ser, frame)
            print_frame("TX", payload, args.ascii)

        if args.listen:
            receive_frames(ser, decoder, None, args.ascii)
        elif args.timeout > 0:
            receive_frames(ser, decoder, args.timeout, args.ascii)
    except KeyboardInterrupt:
        return 0
    finally:
        ser.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
