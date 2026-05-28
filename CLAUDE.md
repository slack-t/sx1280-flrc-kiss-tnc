# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Summary

Firmware for the **Lilygo T3S3 SX1280** (ESP32-S3 + SX1280 2.4 GHz radio) implementing a KISS TNC
modem over FLRC. The T3S3 connects via USB CDC to a Raspberry Pi running a `kiss-tun` daemon that
bridges KISS frames to a Linux `tun0` interface for TCP/IP. Two units linked by Yagi antennas form
a point-to-point ISM-band backhaul. See `IMPLEMENTATION_PLAN.md` for the full design.

## Build & Flash (firmware)

```bash
cd firmware
pio run                        # build
pio run -t upload              # flash via USB
pio device monitor             # serial monitor (921600 baud)
```

## Tests

```bash
# Unit tests (host, no hardware needed)
pio test -e native

# On-device integration tests
pio test -e esp32s3
```

## Key Architecture

**FreeRTOS task layout:**
- `radioRxTask` / `radioTxTask` — pinned to core 1, own all SX1280 SPI access
- `serialRxTask` / `serialTxTask` — core 0, own USB CDC read/write
- `displayTask` — core 0, reads shared `Stats` struct every 500 ms

**Data flow:** USB CDC → `serialRxTask` → KISS decode → `txQueue` → `radioTxTask` → SX1280.
Reverse: DIO1 ISR → semaphore → `radioRxTask` → `rxQueue` → `serialTxTask` → KISS encode → USB CDC.

**KISS framing** lives entirely in `src/kiss/`; it has no hardware dependency and is fully unit-testable on host.

**Stats** (`src/stats/Stats.h`) is the only shared state between tasks. Always take the mutex before reading or writing.

## Configuration

All tunables are in `src/config.h`:
- Pin definitions (SPI, display, DIO1, BUSY, RST)
- FLRC parameters (frequency, bit rate, coding rate, sync word)
- `TX_POWER_DBM` — **keep EIRP ≤ 20 dBm (ETSI EN 300 328)**; with a 15 dBi Yagi set ≤ 5 dBm conducted

## Pi Daemon

```bash
cd pi-daemon
pip install -r requirements.txt
sudo python kiss_tun.py --port /dev/ttyACM0 --addr 10.0.0.1/30
```

The daemon creates `tun0`, sets MTU 127 (SX1280 FLRC max payload), and bridges KISS↔IP.
Install `systemd/kiss-tun.service` for auto-start.

## Hardware Pins (T3S3 SX1280 — verify against board revision)

SX1280: SCK=5, MISO=3, MOSI=6, NSS=7, RST=8, BUSY=36, DIO1=9  
Display (ST7789 170×320): CLK=17, MOSI=19, CS=13, DC=12, RST=10  
Battery ADC: GPIO1

<!-- gitnexus:start -->
# GitNexus — Code Intelligence

This project is indexed by GitNexus as **sx1280-flrc-kiss-tnc** (1032 symbols, 1829 relationships, 34 execution flows). Use the GitNexus MCP tools to understand code, assess impact, and navigate safely.

> If any GitNexus tool warns the index is stale, run `npx gitnexus analyze` in terminal first.

## Always Do

- **MUST run impact analysis before editing any symbol.** Before modifying a function, class, or method, run `gitnexus_impact({target: "symbolName", direction: "upstream"})` and report the blast radius (direct callers, affected processes, risk level) to the user.
- **MUST run `gitnexus_detect_changes()` before committing** to verify your changes only affect expected symbols and execution flows.
- **MUST warn the user** if impact analysis returns HIGH or CRITICAL risk before proceeding with edits.
- When exploring unfamiliar code, use `gitnexus_query({query: "concept"})` to find execution flows instead of grepping. It returns process-grouped results ranked by relevance.
- When you need full context on a specific symbol — callers, callees, which execution flows it participates in — use `gitnexus_context({name: "symbolName"})`.

## Never Do

- NEVER edit a function, class, or method without first running `gitnexus_impact` on it.
- NEVER ignore HIGH or CRITICAL risk warnings from impact analysis.
- NEVER rename symbols with find-and-replace — use `gitnexus_rename` which understands the call graph.
- NEVER commit changes without running `gitnexus_detect_changes()` to check affected scope.

## Resources

| Resource | Use for |
|----------|---------|
| `gitnexus://repo/sx1280-flrc-kiss-tnc/context` | Codebase overview, check index freshness |
| `gitnexus://repo/sx1280-flrc-kiss-tnc/clusters` | All functional areas |
| `gitnexus://repo/sx1280-flrc-kiss-tnc/processes` | All execution flows |
| `gitnexus://repo/sx1280-flrc-kiss-tnc/process/{name}` | Step-by-step execution trace |

## CLI

| Task | Read this skill file |
|------|---------------------|
| Understand architecture / "How does X work?" | `.claude/skills/gitnexus/gitnexus-exploring/SKILL.md` |
| Blast radius / "What breaks if I change X?" | `.claude/skills/gitnexus/gitnexus-impact-analysis/SKILL.md` |
| Trace bugs / "Why is X failing?" | `.claude/skills/gitnexus/gitnexus-debugging/SKILL.md` |
| Rename / extract / split / refactor | `.claude/skills/gitnexus/gitnexus-refactoring/SKILL.md` |
| Tools, resources, schema reference | `.claude/skills/gitnexus/gitnexus-guide/SKILL.md` |
| Index, status, clean, wiki CLI commands | `.claude/skills/gitnexus/gitnexus-cli/SKILL.md` |

<!-- gitnexus:end -->
