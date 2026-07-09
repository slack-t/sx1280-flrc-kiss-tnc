# General-Purpose KISS TNC Architecture

The firmware is now a general-purpose KISS TNC.

Core contract:

- KISS command `0x00`: opaque data payloads
- KISS command `0x0F`: private modem-control channel
- Firmware payload cap: `1280` bytes per KISS data frame
- Over-air transport: internal fragmentation plus selective-repeat ARQ

What the firmware does:

- Accepts arbitrary KISS data payloads without interpreting them as IPv4.
- Fragments payloads into SX1280 FLRC packets internally.
- Reassembles received fragments back into the original payload bytes.
- Returns the reassembled payload on KISS data port `0x00`.

What the firmware does not do:

- It does not require payloads to be IP packets.
- It does not validate payload content by protocol.
- It does not emit side-channel text on the KISS stream.

Host-side tools:

- `pi-daemon/kiss_tun.py`: optional Python IP/TUN adapter
- `pi-daemon-rust/src/main.rs`: optional Rust IP/TUN adapter
- `pi-daemon/raw_kiss.py`: raw payload utility for non-IP protocols
- `pi-daemon/modem_tui.py`: runtime modem-control UI over KISS command `0x0F`
- `pi-daemon/ReticulumFLRCInterface.py`: direct Reticulum (RNS) interface (see [Reticulum Integration Guide](reticulum_kiss_tnc.md))


Examples:

```sh
python3 pi-daemon/raw_kiss.py --port /dev/ttyACM0 --send-text "hello"
python3 pi-daemon/raw_kiss.py --port /dev/ttyACM0 --send-hex "01 02 03 c0 db" --ascii
python3 pi-daemon/raw_kiss.py --port /dev/ttyACM0 --listen
```

For IP benchmarking, keep using the TUN bridges, but treat them as adapters on
top of the generic transport rather than as the TNC's core behavior.
