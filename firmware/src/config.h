#pragma once

// ── SX1280 SPI pins (Lilygo T3S3 SX1280) ─────────────────────────────────────
// Verify against your specific board revision before flashing.
#define RADIO_SCK       5
#define RADIO_MISO      3
#define RADIO_MOSI      6
#define RADIO_NSS       7
#define RADIO_RST       8
#define RADIO_BUSY      36
#define RADIO_DIO1      9

// ── Display pins (SSD1306, 128x64 I2C) ──────────────────────────────────────
#define OLED_SDA        18
#define OLED_SCL        17
#define OLED_ADDR       0x3C
#define OLED_WIDTH      128
#define OLED_HEIGHT     64


// ── Battery ADC ──────────────────────────────────────────────────────────────
#define BATTERY_ADC_PIN 1

// ── Boot-time control pins ───────────────────────────────────────────────────
// Hold GPIO0 low during boot to clear the persisted modem config and force
// the built-in defaults for this session.
#define MODEM_RESET_BUTTON_PIN 0

// ── FLRC radio parameters ────────────────────────────────────────────────────
// Operating frequency: 2480 MHz (above WiFi channel 13 in EU band plans)
#define RADIO_FREQ_MHZ          2480.0f

// TX power (conducted, dBm).
// ETSI EN 300 328: max EIRP = 20 dBm (100 mW).
// With a 15 dBi Yagi: conducted TX must be ≤ 5 dBm to stay compliant.
// Increase only for bench/indoor testing with no antenna or attenuator.
#define TX_POWER_DBM            5

// Bit rate in kbps — RadioLib 6.x beginFLRC() accepts the numeric value directly.
//   Valid options: 260, 325, 520, 650, 1040, 1300
#define RADIO_BITRATE_KBPS      325.0f

// Coding rate denominator passed to beginFLRC():
//   3 = CR 3/4  (recommended — error correction with moderate overhead)
//   2 = CR 1/2  (more correction, lower effective throughput)
//   4 = CR 1/0  (uncoded, maximum throughput)
#define RADIO_CODING_RATE       2

// BT product (Gaussian pulse shaping) passed to beginFLRC().
//   RADIOLIB_SHAPING_1_0 = BT 1.0 (cleaner spectral shape)
//   RADIOLIB_SHAPING_0_5 = BT 0.5 (tighter spectrum)
#define RADIO_BT                RADIOLIB_SHAPING_1_0

// 4-byte sync word as a byte-array initialiser — passed to setSyncWord().
// Pick a value unique to this network to avoid collisions with other SX1280 devices.
#define RADIO_SYNC_WORD_BYTES   { 0x7E, 0xC5, 0xA2, 0x3D }
#define RADIO_SYNC_WORD_LEN     4

// Preamble length in bits (must be 4, 8, 12, 16, 20, 24, 28, or 32)
#define RADIO_PREAMBLE_BITS     32

// ── Packet / buffer limits ───────────────────────────────────────────────────
// SX1280 FLRC maximum payload = 127 bytes.
// The general-purpose KISS payload limit is defined in framing/Framing.h.
#define PACKET_MAX_LEN          127

// ── Serial console / KISS transport discipline ──────────────────────────────
// Set to 1 only when intentionally using the USB CDC port as a console rather
// than as a pure KISS transport stream.
#ifndef SERIAL_CONSOLE_LOGS
#define SERIAL_CONSOLE_LOGS     0
#endif

// Diagnostic-only: register serialTxTask with the task watchdog while it is
// actively draining a USB CDC frame, and compile in the host-backpressure
// telemetry (queue depths, serial write-lock state, TX progress/stall age in
// STATS and on the OLED). The telemetry takes the stats mutex on the serial TX
// hot path, so leave this off for deployment images.
#ifndef SERIAL_TX_WDT_DIAGNOSTICS
#define SERIAL_TX_WDT_DIAGNOSTICS 0
#endif

// ── FreeRTOS queue depths ────────────────────────────────────────────────────
#define TX_QUEUE_DEPTH          32
#define RX_QUEUE_DEPTH          8

// ── Timing and Queue Buffering Parameters ────────────────────────────────────
// Gap between consecutive fragments.  vTaskDelay(pdMS_TO_TICKS(N)) is used so
// Core 1 yields during the gap — delayMicroseconds() would busy-spin and starve radioRxTask.
#define RADIO_INTER_FRAG_DELAY_MS    8     // ms gap between consecutive fragments
#define RX_QUEUE_TIMEOUT_MS          50    // Prevent radio RX deadlock if host is blocked
#define RADIO_ARQ_MAX_ROUNDS         6     // Initial TX + selective retransmit rounds
#define RADIO_ACK_TIMEOUT_MS         110   // Sender waits this long for a bitmap ACK
#define RADIO_ACK_TURNAROUND_DELAY_MS 4    // Receiver waits briefly after round-end before ACKing
#define RADIO_ACK_FALLBACK_DELAY_MS  45    // Receiver waits this long before ACKing if END was lost
#define RADIO_REASSEMBLY_TIMEOUT_MS  500   // Drop stale partial frames after silence
#define RADIO_DUP_CACHE_MS           1500  // Re-ACK recently delivered frames without re-delivering
#define RADIO_LINK_IDLE_MS              500   // ms without any link activity before link is considered idle
#define RADIO_HEARTBEAT_INTERVAL_MS     1000  // ms between idle heartbeat probes
#define RADIO_HEARTBEAT_JITTER_MS       250   // random jitter added to heartbeat interval
#define RADIO_CONTROL_ACK_TIMEOUT_MS    40    // ms to wait for HEARTBEAT_ACK or DATA_READY
#define RADIO_CONTROL_MAX_RETRIES       1     // DATA_PENDING retries after timeout
#define RADIO_LINK_READY_TTL_MS         3000  // ms without bidirectional confirmation → READY ends (PROBING)
#define RADIO_LINK_PROBE_TTL_MS         10000 // ms without bidirectional confirmation → LINK_DOWN
#define RADIO_ERROR_HOLD_MS             3000  // report/display ERROR only this long after the last radio error
#define RADIO_ARQ_CREDIT_PROBE_MS       500   // ms at zero credits with queued data before probing the receiver


// ── Task stack sizes (bytes) ─────────────────────────────────────────────────
#define STACK_MAC               6144
#define STACK_SERIAL_RX         8192
#define STACK_SERIAL_TX         8192
#define STACK_DISPLAY           8192

// ── MAC task (single radio owner) ────────────────────────────────────────────
// Longest sleep per loop iteration; must stay well below MAC_WDT_TIMEOUT_S.
#define MAC_MAX_WAIT_MS         1000
// Task watchdog timeout for the MAC task.
#define MAC_WDT_TIMEOUT_S       15
// Retry interval when a completed frame cannot be delivered to the host queue
// (rxQueue full). The final ACK is withheld until delivery succeeds so queue
// saturation causes retransmission, never acknowledged loss.
#define RX_EGRESS_RETRY_MS      20

// ── Task priorities ──────────────────────────────────────────────────────────
#define PRIO_RADIO              4
#define PRIO_SERIAL             3
#define PRIO_DISPLAY            1

// ── LBT (Listen-Before-Talk) CSMA parameters ─────────────────────────────────
// Channel is considered busy when instantaneous RSSI exceeds this level (dBm).
// Set to 0 to disable CSMA backoff entirely — appropriate for a dedicated 2-node
// P2P link where the protocol is already naturally half-duplex and the 2.4 GHz
// ISM band noise floor often exceeds -85 dBm, causing spurious backoff that adds
// ~240 ms latency per packet with no collision-avoidance benefit.
// To re-enable: set to -70 dBm (ignores WiFi background, detects remote Yagi node).
#define RADIO_LBT_RSSI_THRESHOLD_DBM    0
// Dwell time in RX before reading RSSI — allows AGC and RSSI circuit to settle.
#define RADIO_LBT_SENSE_US              500
// Maximum busy-channel retries before forcing a TX anyway (starvation guard).
#define RADIO_LBT_MAX_RETRIES           10
// Random backoff window [MIN, MAX] ms between retries.
#define RADIO_LBT_BACKOFF_MIN_MS        2
#define RADIO_LBT_BACKOFF_MAX_MS        20

// ── Display refresh interval (ms) ────────────────────────────────────────────
#define DISPLAY_REFRESH_MS      500
