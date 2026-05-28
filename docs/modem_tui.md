# Runtime Modem Configuration TUI

The firmware supports private KISS control frames on command byte `0x0F`.
Normal opaque data traffic stays on KISS command `0x00`. This keeps the data
path generic while allowing the host to read, apply, and persist SX1280 FLRC
modem settings without rebuilding firmware.

Stop `kiss_tun` before using the TUI because both tools need exclusive access
to the same serial port:

```sh
python3 pi-daemon/modem_tui.py --port /dev/ttyACM0
```

Opening the ESP32-S3 USB CDC port may reset the board on some hosts. The TUI
waits after opening and retries commands by default. If your board boots slowly,
increase the wait:

```sh
python3 pi-daemon/modem_tui.py --port /dev/ttyACM0 --boot-wait 5 --timeout 3
```

To test the control channel without starting curses:

```sh
python3 pi-daemon/modem_tui.py --port /dev/ttyACM0 --probe --boot-wait 5 --timeout 3
```

Settings are applied immediately and saved in ESP32 NVS.

Configurable fields:

- `freq`: RF frequency in MHz, `2400.000..2500.000`
- `bitrate`: FLRC bitrate, one of `260, 325, 520, 650, 1040, 1300`
- `cr`: FLRC coding rate denominator, `2`, `3`, or `4`
- `power`: TX power in dBm, `-18..13`
- `preamble`: preamble length in bits, `4, 8, 12, 16, 20, 24, 28, 32`
- `bt`: Gaussian shaping, `0` for BT 0.5 or `1` for BT 1.0
- `sync`: 4-byte sync word as 8 hex characters
- `lbt`: LBT RSSI threshold in dBm, or `0` to disable LBT

The same protocol can be scripted by sending KISS command `0x0F` with ASCII
payloads:

```text
GET
SET freq=2450.000 bitrate=260 cr=2 power=0 preamble=32 bt=1 sync=7ec5a23d lbt=0
DEFAULTS
```
