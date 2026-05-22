# Task: Debug & Optimize Layer-2 Fragmentation (ping -s 400)

- `[x]` Apply core firmware patches for fragmentation and buffer size
  - `[x]` Patch `Radio.cpp` to call `_radio.setPreambleLength(RADIO_PREAMBLE_BITS)` in `_startReceiveNoLock()`
  - `[x]` Patch `Kiss.h` to increase `_buf` size to `IP_MTU + 1`
  - `[x]` Patch `Kiss.cpp` to change boundary checks to `< IP_MTU + 1`
  - `[x]` Patch `main.cpp` to increase delay to 4ms and use monotonic sequential sequence ID
  - `[x]` Optimize inter-fragment delay to 15ms conditionally between fragments (`idx > 0`) for 100Hz tick systems
- `[x]` Implement Phased Diagnostics & SPI Speedup Optimizations
  - `[x]` Refactor `Radio.h` and `Radio.cpp` to optimize SPI RX-to-RX transitions:
    - `[x]` Skip expensive RSSI/SNR SPI transactions on intermediate fragments
    - `[x]` Avoid `setPreambleLength()` SPI transactions when staying in RX mode (passing `forceReset` boolean)
  - `[x]` Refactor `main.cpp` to implement Tick-granularity Protection:
    - `[x]` Ensure inter-fragment delay has a minimum of 2 ticks (10-20ms) on 100Hz systems
    - `[x]` Simplify Reassembler metrics by adopting the final fragment's link quality directly
  - `[x]` Refactor `main.cpp` to add lightweight thread-safe diagnostic logging to track fragment progress
  - `[x]` Refactor `main.cpp` to implement high-speed serial RX drainage & microsecond-polling to eliminate RTT latency and prevent buffer overruns
- `[x]` Build, Upload, and Verify Logs
  - `[x]` Compile firmware with `pio run` to verify no compile errors
  - `[x]` Upload firmware to `/dev/ttyACM0` via PlatformIO (`pio run -t upload`)
  - `[x]` Guide user on manual bench testing, checking `pio device monitor`, and verifying fragment receipt

# Task: Implement High-Performance Rust KISS-TUN Daemon

- `[x]` Create Rust project scaffold under `pi-daemon-rust/`
  - `[x]` Configure `Cargo.toml` with `clap`, `serialport`, `tun-rs`, and `anyhow`
- `[x]` Develop Stateful KISS Codec in Rust (`kiss.rs`)
  - `[x]` Write robust byte-stream state machine decoders matching ESP32 firmware
  - `[x]` Implement inline FEND/FESC escaping for data frames
  - `[x]` Write unit tests covering escaping, back-to-back single-FEND framing, and full round-trips
- `[x]` Implement Orchestration Engine (`main.rs`)
  - `[x]` Build CLI parsing matching `kiss_tun.py` args
  - `[x]` Parse and compute CIDR subnet boundaries dynamically
  - `[x]` Construct blocking synchronous TUN interface and bring it up/enabled
  - `[x]` Support graceful serial timeouts and automatic USB CDC reconnect loops
  - `[x]` Spawn independent hardware threads split for serial full-duplex I/O
- `[x]` Compile, Verify, and Package
  - `[x]` Verify that all 4 unit tests pass successfully
  - `[x]` Compile optimized release static binary (`cargo build --release`)
  - `[x]` Write standard systemd service template `kiss-tun-rs.service`
  - `[x]` Complete documentation updates inside `IMPLEMENTATION_PLAN.md` and `walkthrough.md`

