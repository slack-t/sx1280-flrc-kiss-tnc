#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// PACKET_MAX_LEN must be defined before including this header (via config.h).

// ── Radio-layer packet (one SX1280 FIFO burst, ≤ PACKET_MAX_LEN bytes) ────────
struct Packet {
    uint8_t data[PACKET_MAX_LEN];
    uint8_t len  = 0;
    int8_t  rssi = 0;
    float   snr  = 0.0f;
};

// ── Framing header (1 byte prepended to every radio packet) ───────────────────
//   [7:4]  SEQ   — 4-bit random sequence; identical across all fragments of one IP frame
//   [3:2]  IDX   — fragment index 0–3
//   [1]    LAST  — set on the final fragment; receiver derives total_frags = IDX + 1
//   [0]    SPLIT — 1 = fragmented; 0 = single-packet (no reassembly needed)
static constexpr uint8_t  FRAMING_HDR_LEN    = 1;
static constexpr uint8_t  FRAMING_FLAG_SPLIT = 0x01;
static constexpr uint8_t  FRAMING_FLAG_LAST  = 0x02;
static constexpr uint8_t  FRAMING_SEQ_UNSET  = 0xFF;
static constexpr uint8_t  FRAMING_MAX_FRAGS  = 4;
static constexpr uint8_t  FRAMING_FRAG_DATA  = PACKET_MAX_LEN - FRAMING_HDR_LEN;  // 126
static constexpr uint16_t IP_MTU             =
    static_cast<uint16_t>(FRAMING_MAX_FRAGS) * FRAMING_FRAG_DATA;                 // 504

// ── IP-layer frame (passed through FreeRTOS queues and KISS codec) ────────────
struct IpFrame {
    uint8_t  data[IP_MTU];
    uint16_t len  = 0;
    int8_t   rssi = 0;
    float    snr  = 0.0f;
};

// ── Fragment reassembler ───────────────────────────────────────────────────────
struct Reassembler {
    uint8_t  seq           = FRAMING_SEQ_UNSET;
    uint8_t  buf[IP_MTU];
    uint16_t frag_len[FRAMING_MAX_FRAGS];
    uint8_t  received_mask = 0;
    uint8_t  total_frags   = 0;
    int32_t  rssi_acc      = 0;
    float    snr_acc       = 0.0f;
    uint8_t  frag_count    = 0;
    uint32_t last_tick_ms  = 0;

    void reset() {
        seq           = FRAMING_SEQ_UNSET;
        received_mask = 0;
        total_frags   = 0;
        frag_count    = 0;
        rssi_acc      = 0;
        snr_acc       = 0.0f;
        memset(frag_len, 0, sizeof(frag_len));
    }

    bool isComplete() const {
        if (total_frags == 0) return false;
        const uint8_t expected = static_cast<uint8_t>((1u << total_frags) - 1u);
        return (received_mask & expected) == expected;
    }
};
