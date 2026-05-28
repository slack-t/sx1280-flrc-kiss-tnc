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


def log_info(message: str, quiet: bool = False) -> None:
    if not quiet:
        print(message, flush=True)


def log_error(message: str) -> None:
    print(message, flush=True)


class TraceLogger:
    def __init__(self, path: str | None):
        self._start_ns = time.monotonic_ns()
        self._fh = open(path, "a", buffering=1) if path else None
        self._lock = threading.Lock()
        if self._fh:
            self.write("trace_start", "-", 0, "bridge=python")

    def write(self, event: str, direction: str, length: int, detail: str = "") -> None:
        if not self._fh:
            return
        detail = detail.replace("\t", " ").replace("\n", " ")
        elapsed_ns = time.monotonic_ns() - self._start_ns
        line = f"{elapsed_ns}\t{event}\t{direction}\t{length}\t{detail}\n"
        with self._lock:
            self._fh.write(line)

    def close(self) -> None:
        if self._fh:
            self.write("trace_stop", "-", 0)
            self._fh.close()
            self._fh = None


class _KissState:
    IDLE     = 0
    IN_FRAME = 1
    ESCAPE   = 2


class KissDecoder:
    """Stateful byte-stream KISS decoder."""

    def __init__(self, mtu: int):
        self._state    = _KissState.IDLE
        self._max_len  = mtu
        self._buf: bytearray = bytearray()
        self._overflow = False

    def feed(self, data: bytes):
        """Yield (port, payload) for KISS data frames."""
        for b in data:
            if self._state == _KissState.IDLE:
                if b == FEND:
                    self._buf.clear()
                    self._overflow = False
                    self._state    = _KissState.IN_FRAME

            elif self._state == _KissState.IN_FRAME:
                if b == FEND:
                    if len(self._buf) == 0:
                        # Empty frame: ignore and require a fresh FEND.
                        self._overflow = False
                        self._state = _KissState.IDLE
                    elif self._overflow:
                        # Oversized frame: discard and require a fresh FEND.
                        self._buf.clear()
                        self._overflow = False
                        self._state = _KissState.IDLE
                    elif self._buf[0] != KISS_DATA_PORT:
                        # Non-data port: discard silently and require a fresh FEND.
                        self._buf.clear()
                        self._overflow = False
                        self._state = _KissState.IDLE
                    else:
                        # Valid data frame: emit payload (strip port byte).
                        payload = bytes(self._buf[1:])
                        self._buf.clear()
                        self._overflow = False
                        self._state = _KissState.IDLE
                        yield (KISS_DATA_PORT, payload)
                elif b == FESC:
                    self._state = _KissState.ESCAPE
                else:
                    if len(self._buf) < self._max_len + 1:
                        self._buf.append(b)
                    else:
                        self._overflow = True

            else:  # ESCAPE
                if b not in (TFEND, TFESC):
                    # Invalid escape: discard partial frame and require a fresh FEND.
                    self._buf.clear()
                    self._overflow = False
                    self._state    = _KissState.IDLE
                else:
                    if b == TFEND:
                        b = FEND
                    else:
                        b = FESC
                    if len(self._buf) < self._max_len + 1:
                        self._buf.append(b)
                    else:
                        self._overflow = True
                    self._state = _KissState.IN_FRAME


def tun_to_radio(tun, ser, mtu: int, stop_event: threading.Event, debug_ip: bool,
                 quiet: bool, trace: TraceLogger):
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
            trace.write("tun_read", Direction.TUN_TO_RADIO.value, len(pkt), describe_ipv4_packet(pkt))
            if len(pkt) > mtu:
                log_error(f"[kiss_tun] WARN dropped oversized packet ({len(pkt)} > {mtu} bytes)")
                trace.write("oversize_drop", Direction.TUN_TO_RADIO.value, len(pkt), f"mtu={mtu}")
                continue
            log_info(f"[kiss_tun] tun0 -> radio: sending {len(pkt)} bytes", quiet)
            log_packet(Direction.TUN_TO_RADIO, pkt, debug_ip)
            encoded = kiss_encode(pkt)
            written = ser.write(encoded)
            trace.write("serial_write_done", Direction.TUN_TO_RADIO.value, len(pkt),
                        f"encoded_len={len(encoded)} written={written}")
            # Pacing: A small 5ms delay prevents the host from flooding the USB CDC
            # buffers and the TNC board's internal queue during high-throughput benchmarks.
            time.sleep(0.005)
        except (serial.SerialException, OSError) as e:
            log_error(f"[kiss_tun] tun->radio error: {e}")
            trace.write("tun_to_radio_error", Direction.TUN_TO_RADIO.value, 0, str(e))
            stop_event.set()


def radio_to_tun(tun, ser, mtu: int, stop_event: threading.Event, debug_ip: bool,
                 quiet: bool, trace: TraceLogger):
    """Read KISS frames from serial and inject IP packets into tun0."""
    decoder = KissDecoder(mtu)
    while not stop_event.is_set():
        try:
            waiting = ser.in_waiting
            if waiting:
                data = ser.read(waiting)
                trace.write("serial_read", Direction.RADIO_TO_TUN.value, len(data))
                for port, payload in decoder.feed(data):
                    if port == KISS_DATA_PORT and payload:
                        try:
                            trace.write("kiss_frame", Direction.RADIO_TO_TUN.value, len(payload),
                                        describe_ipv4_packet(payload))
                            log_info(f"[kiss_tun] radio -> tun0: injecting {len(payload)} bytes", quiet)
                            log_packet(Direction.RADIO_TO_TUN, payload, debug_ip)
                            os.write(tun.fileno(), payload)
                            trace.write("tun_write_done", Direction.RADIO_TO_TUN.value, len(payload),
                                        describe_ipv4_packet(payload))
                        except OSError as e:
                            if e.errno == errno.EINVAL:
                                log_error(f"[kiss_tun] radio->tun: dropped invalid IP packet (len={len(payload)})")
                                trace.write("tun_write_invalid", Direction.RADIO_TO_TUN.value,
                                            len(payload), str(e))
                            else:
                                raise
            else:
                time.sleep(0.001)
        except (serial.SerialException, OSError) as e:
            log_error(f"[kiss_tun] radio->tun error: {e}")
            trace.write("radio_to_tun_error", Direction.RADIO_TO_TUN.value, 0, str(e))
            stop_event.set()


def configure_tun(tun, addr_with_prefix: str, mtu: int):
    """Set IP address, netmask, and MTU on the tun interface."""
    interface   = ipaddress.ip_interface(addr_with_prefix)
    tun.addr    = str(interface.ip)
    tun.netmask = str(interface.netmask)
    tun.mtu     = mtu
    tun.up()
    return interface


def run_bridge(tun, ser, mtu: int, debug_ip: bool, quiet: bool,
               trace: TraceLogger) -> threading.Event:
    """Start bridge threads; returns the stop_event they share."""
    stop = threading.Event()
    t1 = threading.Thread(target=tun_to_radio,  args=(tun, ser, mtu, stop, debug_ip, quiet, trace), daemon=True)
    t2 = threading.Thread(target=radio_to_tun,  args=(tun, ser, mtu, stop, debug_ip, quiet, trace), daemon=True)
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
    parser.add_argument("--quiet", action="store_true",
                        help="Suppress per-packet and lifecycle logs for benchmark runs")
    parser.add_argument("--trace-file", default=None,
                        help="Write timestamped bridge events to this TSV file")
    args = parser.parse_args()

    try:
        import pytun
    except ImportError:
        sys.exit("python-pytun not found — run: pip install python-pytun")

    tun = pytun.TunTapDevice(name=args.name, flags=pytun.IFF_TUN | pytun.IFF_NO_PI)
    interface = configure_tun(tun, args.addr, args.mtu)

    log_info(f"[kiss_tun] {tun.name} up - {interface.ip}/{interface.network.prefixlen}  MTU {args.mtu}", args.quiet)
    log_info("[kiss_tun] Running - Ctrl-C to stop", args.quiet)
    trace = TraceLogger(args.trace_file)
    try:
        while True:
            try:
                log_info(f"[kiss_tun] Connecting to {args.port} ...", args.quiet)
                ser = serial.Serial(args.port, args.baud, timeout=0)
                log_info("[kiss_tun] Connected.", args.quiet)

                # run_bridge blocks until a thread sets stop (serial error) or
                # KeyboardInterrupt propagates up through join().
                run_bridge(tun, ser, args.mtu, args.debug_ip, args.quiet, trace)

                ser.close()
                log_error(f"[kiss_tun] Connection lost - retrying in {RECONNECT_DELAY_S} s ...")
                time.sleep(RECONNECT_DELAY_S)

            except (serial.SerialException, OSError) as e:
                log_error(f"[kiss_tun] Cannot open {args.port}: {e} - "
                          f"retrying in {RECONNECT_DELAY_S} s ...")
                time.sleep(RECONNECT_DELAY_S)

    except KeyboardInterrupt:
        log_info("\n[kiss_tun] Stopping ...", args.quiet)
        tun.down()
        log_info("[kiss_tun] Done.", args.quiet)
    finally:
        trace.close()


if __name__ == "__main__":
    main()
