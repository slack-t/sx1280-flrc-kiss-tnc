#!/usr/bin/env python3
"""
modem_tui.py - runtime SX1280 FLRC modem configuration TUI.

Stop kiss_tun before running this tool. It uses private KISS command 0x0F on
the same serial port, so it must have exclusive access to the TNC.
"""

import argparse
import curses
import time
import serial

FEND = 0xC0
FESC = 0xDB
TFEND = 0xDC
TFESC = 0xDD
KISS_CONTROL_FRAME = 0x0F

FIELDS = [
    ("freq", "Frequency MHz", "2400.000..2500.000"),
    ("bitrate", "Bitrate kbps", "260, 325, 520, 650, 1040, 1300"),
    ("cr", "Coding rate", "2=1/2, 3=3/4, 4=uncoded"),
    ("power", "TX power dBm", "-18..13"),
    ("preamble", "Preamble bits", "4, 8, 12, 16, 20, 24, 28, 32"),
    ("bt", "BT shaping", "0=0.5, 1=1.0"),
    ("sync", "Sync word", "8 hex chars, e.g. 7ec5a23d"),
    ("lbt", "LBT RSSI dBm", "0 disables LBT"),
]


def kiss_encode(command: int, payload: bytes) -> bytes:
    out = bytearray([FEND, command])
    for byte in payload:
        if byte == FEND:
            out.extend([FESC, TFEND])
        elif byte == FESC:
            out.extend([FESC, TFESC])
        else:
            out.append(byte)
    out.append(FEND)
    return bytes(out)


class KissDecoder:
    def __init__(self):
        self.state = "idle"
        self.buf = bytearray()

    def feed(self, data: bytes):
        for byte in data:
            if self.state == "idle":
                if byte == FEND:
                    self.buf.clear()
                    self.state = "frame"
            elif self.state == "frame":
                if byte == FEND:
                    if self.buf:
                        command = self.buf[0]
                        payload = bytes(self.buf[1:])
                        self.buf.clear()
                        self.state = "idle"
                        yield command, payload
                    else:
                        self.state = "idle"
                elif byte == FESC:
                    self.state = "escape"
                else:
                    self.buf.append(byte)
            else:
                if byte == TFEND:
                    self.buf.append(FEND)
                    self.state = "frame"
                elif byte == TFESC:
                    self.buf.append(FESC)
                    self.state = "frame"
                else:
                    self.buf.clear()
                    self.state = "idle"


class ModemClient:
    def __init__(self, port: str, baud: int, timeout: float,
                 boot_wait: float, retries: int):
        # Configure control lines before opening. Opening the ESP32-S3 USB CDC
        # device can reset the board if the OS/driver asserts DTR/RTS first.
        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = baud
        self.ser.timeout = 0
        self.ser.rtscts = False
        self.ser.dsrdtr = False
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        if boot_wait > 0:
            time.sleep(boot_wait)
        self.ser.reset_input_buffer()
        self.timeout = timeout
        self.retries = max(1, retries)
        self.decoder = KissDecoder()

    def close(self):
        self.ser.close()

    def command(self, text: str) -> str:
        frame = kiss_encode(KISS_CONTROL_FRAME, text.encode("ascii"))
        for attempt in range(self.retries):
            self.ser.reset_input_buffer()
            self.decoder = KissDecoder()
            self.ser.write(frame)
            self.ser.flush()
            deadline = time.monotonic() + self.timeout
            while time.monotonic() < deadline:
                waiting = self.ser.in_waiting
                if waiting:
                    for command, payload in self.decoder.feed(self.ser.read(waiting)):
                        if command == KISS_CONTROL_FRAME:
                            return payload.decode("ascii", errors="replace")
                time.sleep(0.01)
            if attempt + 1 < self.retries:
                time.sleep(0.25)
        raise TimeoutError("no response from TNC")


def parse_config(response: str) -> dict[str, str]:
    if not response.startswith("OK "):
        raise RuntimeError(response)
    values: dict[str, str] = {}
    for token in response[3:].split():
        if "=" in token:
            key, value = token.split("=", 1)
            values[key] = value
    return values


def prompt(stdscr, y: int, label: str, current: str) -> str:
    curses.echo()
    stdscr.move(y, 0)
    stdscr.clrtoeol()
    stdscr.addstr(y, 0, f"{label} [{current}]: ")
    value = stdscr.getstr(y, len(label) + len(current) + 5, 32).decode("ascii", errors="ignore").strip()
    curses.noecho()
    return value or current


def draw(stdscr, values: dict[str, str], selected: int, status: str):
    stdscr.erase()
    stdscr.addstr(0, 0, "SX1280 FLRC Modem Config", curses.A_BOLD)
    stdscr.addstr(1, 0, "Stop kiss_tun while using this tool. Changes apply immediately and are saved in NVS.")
    for idx, (key, label, hint) in enumerate(FIELDS):
        attr = curses.A_REVERSE if idx == selected else curses.A_NORMAL
        stdscr.addstr(3 + idx, 0, f"{label:<16}", attr)
        stdscr.addstr(3 + idx, 18, f"{values.get(key, ''):<16}", attr)
        stdscr.addstr(3 + idx, 37, hint)
    stdscr.addstr(13, 0, "Keys: up/down select  enter edit  s save/apply  r reload  d defaults  q quit")
    stdscr.addstr(15, 0, status[:curses.COLS - 1])
    stdscr.refresh()


def run_tui(stdscr, client: ModemClient):
    curses.curs_set(0)
    selected = 0
    status = "Loading config..."
    values = parse_config(client.command("GET"))
    status = "Config loaded."

    while True:
        draw(stdscr, values, selected, status)
        key = stdscr.getch()
        if key in (ord("q"), 27):
            return
        if key == curses.KEY_UP:
            selected = (selected - 1) % len(FIELDS)
        elif key == curses.KEY_DOWN:
            selected = (selected + 1) % len(FIELDS)
        elif key in (10, 13):
            field, label, _ = FIELDS[selected]
            values[field] = prompt(stdscr, 16, label, values.get(field, ""))
            status = f"Edited {field}; press s to apply."
        elif key == ord("r"):
            try:
                values = parse_config(client.command("GET"))
                status = "Reloaded from TNC."
            except Exception as exc:
                status = f"Reload failed: {exc}"
        elif key == ord("d"):
            try:
                status = client.command("DEFAULTS")
                values = parse_config(client.command("GET"))
            except Exception as exc:
                status = f"Defaults failed: {exc}"
        elif key == ord("s"):
            command = "SET " + " ".join(f"{key}={values.get(key, '')}" for key, _, _ in FIELDS)
            try:
                status = client.command(command)
                values = parse_config(client.command("GET"))
            except Exception as exc:
                status = f"Save failed: {exc}"


def main():
    parser = argparse.ArgumentParser(description="TUI for runtime SX1280 FLRC modem settings")
    parser.add_argument("--port", default="/dev/ttyACM0", help="Serial port (default: /dev/ttyACM0)")
    parser.add_argument("--baud", type=int, default=921600,
                        help="Baud rate (ignored by ESP32-S3 native USB CDC)")
    parser.add_argument("--timeout", type=float, default=2.0, help="Command timeout in seconds")
    parser.add_argument("--boot-wait", type=float, default=3.0,
                        help="Seconds to wait after opening serial before first command")
    parser.add_argument("--retries", type=int, default=5,
                        help="Control command retries before failing")
    parser.add_argument("--probe", action="store_true",
                        help="Send GET, print the response, and exit without curses")
    args = parser.parse_args()

    client = ModemClient(args.port, args.baud, args.timeout, args.boot_wait, args.retries)
    try:
        if args.probe:
            print(client.command("GET"))
        else:
            curses.wrapper(run_tui, client)
    finally:
        client.close()


if __name__ == "__main__":
    main()
