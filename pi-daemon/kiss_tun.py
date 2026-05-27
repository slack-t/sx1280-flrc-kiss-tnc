#!/usr/bin/env python3
"""
kiss_tun.py — KISS TNC ↔ Linux TUN bridge

Reads KISS frames from the T3S3 over USB CDC (/dev/ttyACM0) and injects the
payload as raw IP packets into a Linux tun interface, and vice versa.

Usage:
    sudo python3 kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.1/30

Requirements:
    pip install pyserial python-pytun
"""

import argparse
import enum
import ipaddress
import os
import select
import sys
import threading
import time
import errno
import serial

try:
    import pytun
except ImportError:
    sys.exit("python-pytun not found — run: pip install python-pytun")

# ── KISS constants ────────────────────────────────────────────────────────────
FEND  = 0xC0
FESC  = 0xDB
TFEND = 0xDC
TFESC = 0xDD
KISS_DATA_PORT = 0x00

# IP MTU after layer-2 fragmentation/reassembly with a 4-byte radio link header.
# Must match firmware IP_MTU = FRAMING_MAX_FRAGS * (PACKET_MAX_LEN - 4) = 492.
DEFAULT_MTU = 492

# Seconds to wait before retrying a lost serial connection
RECONNECT_DELAY_S = 5


def kiss_encode(payload: bytes) -> bytes:
    """Wrap payload bytes in a KISS data frame (port 0)."""
    out = bytearray()
    out.append(FEND)
    out.append(0x00)  # port 0, data frame
    for b in payload:
        if b == FEND:
            out.append(FESC)
            out.append(TFEND)
        elif b == FESC:
            out.append(FESC)
            out.append(TFESC)
        else:
            out.append(b)
    out.append(FEND)
    return bytes(out)


class Direction(enum.Enum):
    TUN_TO_RADIO = "tun→radio"
    RADIO_TO_TUN = "radio→tun"


def describe_ipv4_packet(pkt: bytes) -> str:
    if len(pkt) < 20:
        return f"non-ip len={len(pkt)}"

    version = pkt[0] >> 4
    ihl = (pkt[0] & 0x0F) * 4
    if version != 4 or ihl < 20 or len(pkt) < ihl:
        return f"non-ip len={len(pkt)}"

    total_len = int.from_bytes(pkt[2:4], "big")
    ident = int.from_bytes(pkt[4:6], "big")
    frag_field = int.from_bytes(pkt[6:8], "big")
    flags = frag_field >> 13
    frag_offset = (frag_field & 0x1FFF) * 8
    proto = pkt[9]

    proto_name = {
        1: "ICMP",
        6: "TCP",
        17: "UDP",
    }.get(proto, str(proto))
    summary = f"ipv4 len={total_len} id=0x{ident:04x} proto={proto_name}"

    if flags & 0x2:
        summary += " DF"
    if flags & 0x1:
        summary += " MF"
    if frag_offset:
        summary += f" frag_off={frag_offset}"

    if proto == 1 and len(pkt) >= ihl + 2:
        icmp_type = pkt[ihl]
        icmp_code = pkt[ihl + 1]
        summary += f" icmp={icmp_type}/{icmp_code}"

    return summary


def log_packet(direction: Direction, pkt: bytes, debug_ip: bool) -> None:
    if debug_ip:
        print(f"[kiss_tun] {direction.value}: {describe_ipv4_packet(pkt)}", flush=True)


class _KissState:
    IDLE     = 0
    IN_FRAME = 1
    ESCAPE   = 2


class KissDecoder:
    """Stateful byte-stream KISS decoder."""

    def __init__(self):
        self._state    = _KissState.IDLE
        self._buf: bytearray = bytearray()
        self._overflow = False
        self._log_buf: bytearray = bytearray()  # collects out-of-frame ESP32 text

    def feed(self, data: bytes):
        """Yield (port, payload) for KISS data frames, or ('log', line) for ESP32 text."""
        for b in data:
            if self._state == _KissState.IDLE:
                if b == FEND:
                    if self._log_buf:
                        line = self._log_buf.decode('ascii', errors='replace').rstrip('\r')
                        if line:
                            yield ('log', line + ' ~trunc')
                        self._log_buf.clear()
                    self._buf.clear()
                    self._overflow = False
                    self._state    = _KissState.IN_FRAME
                elif b == ord('\n'):
                    line = self._log_buf.decode('ascii', errors='replace').rstrip('\r')
                    if line:
                        yield ('log', line)
                    self._log_buf.clear()
                elif b != ord('\r'):
                    self._log_buf.append(b)

            elif self._state == _KissState.IN_FRAME:
                if b == FEND:
                    if len(self._buf) == 0:
                        # Empty delimiter: fresh start, no frame emitted.
                        self._overflow = False
                    elif self._overflow:
                        # Oversized frame: discard and start fresh candidate.
                        self._buf.clear()
                        self._overflow = False
                    elif self._buf[0] != KISS_DATA_PORT:
                        # Non-data port: discard silently.
                        self._buf.clear()
                        self._overflow = False
                    else:
                        # Valid data frame: emit payload (strip port byte).
                        payload = bytes(self._buf[1:])
                        self._buf.clear()
                        self._overflow = False
                        yield (KISS_DATA_PORT, payload)
                    # Trailing FEND closes this frame and opens the next candidate.
                    # _state stays IN_FRAME.
                elif b == FESC:
                    self._state = _KissState.ESCAPE
                else:
                    if len(self._buf) < DEFAULT_MTU + 1:
                        self._buf.append(b)
                    else:
                        self._overflow = True

            else:  # ESCAPE
                if b == FEND:
                    # FEND while in escape: invalid sequence, discard partial frame.
                    self._buf.clear()
                    self._overflow = False
                    self._state    = _KissState.IN_FRAME
                else:
                    if b == TFEND:
                        b = FEND
                    elif b == TFESC:
                        b = FESC
                    if len(self._buf) < DEFAULT_MTU + 1:
                        self._buf.append(b)
                    else:
                        self._overflow = True
                    self._state = _KissState.IN_FRAME


def tun_to_radio(tun, ser, mtu: int, stop_event: threading.Event, debug_ip: bool):
    """Read IP packets from tun0 and send as KISS frames over serial."""
    while not stop_event.is_set():
        try:
            r, _, _ = select.select([tun], [], [], 0.1)
            if tun not in r:
                continue
            # Read with a large buffer; Linux raises EMSGSIZE if buffer < packet.
            pkt = os.read(tun.fileno(), 65535)
            if not pkt:
                continue
            if len(pkt) > mtu:
                print(f"[kiss_tun] WARN dropped oversized packet ({len(pkt)} > {mtu} bytes)",
                      flush=True)
                continue
            print(f"[kiss_tun] tun0 → radio: sending {len(pkt)} bytes", flush=True)
            log_packet(Direction.TUN_TO_RADIO, pkt, debug_ip)
            ser.write(kiss_encode(pkt))
            # Pacing: A small 5ms delay prevents the host from flooding the USB CDC
            # buffers and the TNC board's internal queue during high-throughput benchmarks.
            time.sleep(0.005)
        except (serial.SerialException, OSError) as e:
            print(f"[kiss_tun] tun→radio error: {e}", flush=True)
            stop_event.set()


def radio_to_tun(tun, ser, stop_event: threading.Event, debug_ip: bool):
    """Read KISS frames from serial and inject IP packets into tun0."""
    decoder = KissDecoder()
    while not stop_event.is_set():
        try:
            waiting = ser.in_waiting
            if waiting:
                data = ser.read(waiting)
                for port, payload in decoder.feed(data):
                    if port == 'log':
                        print(f"[esp32] {payload}", flush=True)
                        continue
                    if port == KISS_DATA_PORT and payload:
                        try:
                            print(f"[kiss_tun] radio → tun0: injecting {len(payload)} bytes", flush=True)
                            log_packet(Direction.RADIO_TO_TUN, payload, debug_ip)
                            os.write(tun.fileno(), payload)
                        except OSError as e:
                            if e.errno == errno.EINVAL:
                                print(f"[kiss_tun] radio→tun: dropped invalid IP packet (len={len(payload)})", flush=True)
                            else:
                                raise
            else:
                time.sleep(0.001)
        except (serial.SerialException, OSError) as e:
            print(f"[kiss_tun] radio→tun error: {e}", flush=True)
            stop_event.set()


def configure_tun(tun, addr_with_prefix: str, mtu: int):
    """Set IP address, netmask, and MTU on the tun interface."""
    interface   = ipaddress.ip_interface(addr_with_prefix)
    tun.addr    = str(interface.ip)
    tun.netmask = str(interface.netmask)
    tun.mtu     = mtu
    tun.up()
    print(f"[kiss_tun] {tun.name} up — {interface.ip}/{interface.network.prefixlen}  MTU {mtu}",
          flush=True)


def run_bridge(tun, ser, mtu: int, debug_ip: bool) -> threading.Event:
    """Start bridge threads; returns the stop_event they share."""
    stop = threading.Event()
    t1 = threading.Thread(target=tun_to_radio,  args=(tun, ser, mtu, stop, debug_ip), daemon=True)
    t2 = threading.Thread(target=radio_to_tun,  args=(tun, ser, stop, debug_ip),      daemon=True)
    t1.start()
    t2.start()
    t1.join()
    t2.join()
    return stop


def main():
    parser = argparse.ArgumentParser(description="KISS TNC ↔ tun0 bridge for SX1280 FLRC TNC")
    parser.add_argument("--port",  default="/dev/ttyACM0",
                        help="Serial port (default: /dev/ttyACM0)")
    parser.add_argument("--baud",  type=int, default=921600,
                        help="Baud rate (ignored by ESP32-S3 native USB CDC)")
    parser.add_argument("--addr",  required=True,
                        help="IP/prefix for tun interface, e.g. 10.0.0.1/30")
    parser.add_argument("--mtu",   type=int, default=DEFAULT_MTU,
                        help=f"MTU (default: {DEFAULT_MTU}, must match firmware IP_MTU)")
    parser.add_argument("--name",  default="tun0",
                        help="TUN interface name (default: tun0)")
    parser.add_argument("--debug-ip", action="store_true",
                        help="Log IPv4 header details for packets sent to and from the TUN")
    args = parser.parse_args()

    tun = pytun.TunTapDevice(name=args.name, flags=pytun.IFF_TUN | pytun.IFF_NO_PI)
    configure_tun(tun, args.addr, args.mtu)

    print("[kiss_tun] Running — Ctrl-C to stop", flush=True)
    try:
        while True:
            try:
                print(f"[kiss_tun] Connecting to {args.port} ...", flush=True)
                ser = serial.Serial(args.port, args.baud, timeout=0)
                print(f"[kiss_tun] Connected.", flush=True)

                # run_bridge blocks until a thread sets stop (serial error) or
                # KeyboardInterrupt propagates up through join().
                run_bridge(tun, ser, args.mtu, args.debug_ip)

                ser.close()
                print(f"[kiss_tun] Connection lost — retrying in {RECONNECT_DELAY_S} s ...",
                      flush=True)
                time.sleep(RECONNECT_DELAY_S)

            except (serial.SerialException, OSError) as e:
                print(f"[kiss_tun] Cannot open {args.port}: {e} — "
                      f"retrying in {RECONNECT_DELAY_S} s ...", flush=True)
                time.sleep(RECONNECT_DELAY_S)

    except KeyboardInterrupt:
        print("\n[kiss_tun] Stopping ...", flush=True)
        tun.down()
        print("[kiss_tun] Done.")


if __name__ == "__main__":
    main()
