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

# IP MTU after layer-2 fragmentation/reassembly (4 × 126-byte FLRC fragments).
# Must match firmware IP_MTU = FRAMING_MAX_FRAGS * (PACKET_MAX_LEN - 1) = 504.
DEFAULT_MTU = 504

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


class KissDecoder:
    """Stateful byte-stream KISS decoder."""

    def __init__(self):
        self._buf: bytearray = bytearray()
        self._in_frame = False
        self._escape   = False
        self._overflow = False

    def feed(self, data: bytes):
        """Yield complete (port, payload) tuples as frames arrive."""
        for b in data:
            if not self._in_frame:
                if b == FEND:
                    self._in_frame = True
                    self._buf.clear()
                    self._escape   = False
                    self._overflow = False
            else:
                if b == FEND:
                    if not self._overflow and len(self._buf) > 1:
                        port    = self._buf[0]
                        payload = bytes(self._buf[1:])
                        yield (port, payload)
                    # Trailing FEND stays in IN_FRAME — acts as leading FEND of
                    # the next frame for single-FEND back-to-back streams.
                    self._buf.clear()
                    self._escape   = False
                    self._overflow = False
                elif b == FESC:
                    self._escape = True
                else:
                    if self._escape:
                        self._escape = False
                        if b == TFEND:
                            b = FEND
                        elif b == TFESC:
                            b = FESC
                    if len(self._buf) < DEFAULT_MTU + 1:
                        self._buf.append(b)
                    else:
                        self._overflow = True


def tun_to_radio(tun, ser, mtu: int, stop_event: threading.Event):
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
            ser.write(kiss_encode(pkt))
            # Pacing: A small 5ms delay prevents the host from flooding the USB CDC
            # buffers and the TNC board's internal queue during high-throughput benchmarks.
            time.sleep(0.005)
        except (serial.SerialException, OSError) as e:
            print(f"[kiss_tun] tun→radio error: {e}", flush=True)
            stop_event.set()


def radio_to_tun(tun, ser, stop_event: threading.Event):
    """Read KISS frames from serial and inject IP packets into tun0."""
    decoder = KissDecoder()
    while not stop_event.is_set():
        try:
            waiting = ser.in_waiting
            if waiting:
                data = ser.read(waiting)
                for port, payload in decoder.feed(data):
                    if port == 0x00 and payload:
                        try:
                            print(f"[kiss_tun] radio → tun0: injecting {len(payload)} bytes", flush=True)
                            os.write(tun.fileno(), payload)
                        except OSError as e:
                            if e.errno == errno.EINVAL:
                                # Malformed packet received on serial (e.g. bootloader logs/garbage bytes)
                                # and rejected by the kernel. Ignore it to prevent connection drop.
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


def run_bridge(tun, ser, mtu: int) -> threading.Event:
    """Start bridge threads; returns the stop_event they share."""
    stop = threading.Event()
    t1 = threading.Thread(target=tun_to_radio,  args=(tun, ser, mtu, stop), daemon=True)
    t2 = threading.Thread(target=radio_to_tun,  args=(tun, ser, stop),      daemon=True)
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
                run_bridge(tun, ser, args.mtu)

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
