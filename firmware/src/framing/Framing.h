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
};

enum class LinkPacketType : uint8_t {
    DATA = 0,
    ACK  = 1,
};

// ── Link header ────────────────────────────────────────────────────────────────
// byte 0:
//   [7:6]  TYPE  — DATA or ACK control packet
//   [5]    END   — final packet in the current TX burst, receiver should ACK now
//   [4]    RSV   — reserved for future coordinator MAC / traffic class
//   [3:2]  IDX   — fragment index 0–3 (ACK packets use 0)
//   [1:0]  TOTAL — total fragments minus one
// byte 1:
//   frame sequence high byte
// byte 2:
//   frame sequence low byte
// byte 3 (DATA):
//   payload length (0–FRAMING_FRAG_DATA)
// byte 3 (ACK):
//   received fragment bitmap
static constexpr uint8_t  FRAMING_DATA_HDR_LEN    = 4;
static constexpr uint8_t  FRAMING_ACK_HDR_LEN     = 4;
static constexpr uint8_t  FRAMING_TYPE_SHIFT      = 6;
static constexpr uint8_t  FRAMING_TYPE_MASK       = 0x03;
static constexpr uint8_t  FRAMING_FLAG_ROUND_END  = 0x20;
static constexpr uint8_t  FRAMING_IDX_SHIFT       = 2;
static constexpr uint8_t  FRAMING_IDX_MASK        = 0x03;
static constexpr uint8_t  FRAMING_TOTAL_MASK      = 0x03;
static constexpr uint16_t FRAMING_SEQ_UNSET       = 0xFFFF;
static constexpr uint8_t  FRAMING_MAX_FRAGS  = 4;
// Data bytes per fragment. Fragmented packets use a fixed 4-byte link header.
static constexpr uint8_t  FRAMING_FRAG_DATA  = PACKET_MAX_LEN - FRAMING_DATA_HDR_LEN; // 123
static constexpr uint16_t IP_MTU             =
    static_cast<uint16_t>(FRAMING_MAX_FRAGS) * FRAMING_FRAG_DATA;                 // 492

// ── IP-layer frame (passed through FreeRTOS queues and KISS codec) ────────────
struct IpFrame {
    uint8_t  data[IP_MTU];
    uint16_t len  = 0;
    int8_t   rssi = 0;
};

struct AckFrame {
    uint16_t seq          = FRAMING_SEQ_UNSET;
    uint8_t total_frags   = 0;
    uint8_t received_mask = 0;
};

inline uint8_t framingExpectedMask(uint8_t total_frags) {
    return (total_frags == 0 || total_frags > FRAMING_MAX_FRAGS)
        ? 0
        : static_cast<uint8_t>((1u << total_frags) - 1u);
}

inline uint8_t framingEncodeByte0(LinkPacketType type,
                                  uint8_t idx,
                                  uint8_t total_frags,
                                  bool round_end) {
    uint8_t byte0 = static_cast<uint8_t>((static_cast<uint8_t>(type) & FRAMING_TYPE_MASK)
                                         << FRAMING_TYPE_SHIFT);
    if (round_end) {
        byte0 |= FRAMING_FLAG_ROUND_END;
    }
    byte0 |= static_cast<uint8_t>((idx & FRAMING_IDX_MASK) << FRAMING_IDX_SHIFT);
    byte0 |= static_cast<uint8_t>((total_frags - 1u) & FRAMING_TOTAL_MASK);
    return byte0;
}

inline uint16_t framingPacketSeq(const Packet& pkt) {
    return static_cast<uint16_t>((static_cast<uint16_t>(pkt.data[1]) << 8) |
                                 static_cast<uint16_t>(pkt.data[2]));
}

inline LinkPacketType framingPacketType(const Packet& pkt) {
    return static_cast<LinkPacketType>((pkt.data[0] >> FRAMING_TYPE_SHIFT) & FRAMING_TYPE_MASK);
}

inline uint8_t framingFragmentIndex(const Packet& pkt) {
    return static_cast<uint8_t>((pkt.data[0] >> FRAMING_IDX_SHIFT) & FRAMING_IDX_MASK);
}

inline uint8_t framingTotalFrags(const Packet& pkt) {
    return static_cast<uint8_t>((pkt.data[0] & FRAMING_TOTAL_MASK) + 1u);
}

inline bool framingIsRoundEnd(const Packet& pkt) {
    return (pkt.data[0] & FRAMING_FLAG_ROUND_END) != 0;
}

inline uint8_t framingPayloadLen(const Packet& pkt) {
    return pkt.data[3];
}

inline bool framingParseAck(const Packet& pkt, AckFrame& ack) {
    if (pkt.len < FRAMING_ACK_HDR_LEN || framingPacketType(pkt) != LinkPacketType::ACK) {
        return false;
    }
    ack.seq           = framingPacketSeq(pkt);
    ack.total_frags   = framingTotalFrags(pkt);
    ack.received_mask = static_cast<uint8_t>(pkt.data[3] & 0x0Fu);
    return ack.total_frags >= 1 && ack.total_frags <= FRAMING_MAX_FRAGS;
}

inline void framingBuildAckPacket(Packet& pkt, const AckFrame& ack) {
    pkt.data[0] = framingEncodeByte0(LinkPacketType::ACK, 0, ack.total_frags, false);
    pkt.data[1] = static_cast<uint8_t>(ack.seq >> 8);
    pkt.data[2] = static_cast<uint8_t>(ack.seq & 0xFFu);
    pkt.data[3] = static_cast<uint8_t>(ack.received_mask & 0x0Fu);
    memset(pkt.data + FRAMING_ACK_HDR_LEN, 0, PACKET_MAX_LEN - FRAMING_ACK_HDR_LEN);
    pkt.len     = PACKET_MAX_LEN;
}

inline void framingBuildDataPacket(Packet& pkt,
                                   uint16_t seq,
                                   uint8_t idx,
                                   uint8_t total_frags,
                                   bool round_end,
                                   const uint8_t* payload,
                                   uint8_t payload_len) {
    pkt.data[0] = framingEncodeByte0(LinkPacketType::DATA, idx, total_frags, round_end);
    pkt.data[1] = static_cast<uint8_t>(seq >> 8);
    pkt.data[2] = static_cast<uint8_t>(seq & 0xFFu);
    pkt.data[3] = payload_len;
    if (payload_len > 0) {
        memcpy(pkt.data + FRAMING_DATA_HDR_LEN, payload, payload_len);
    }
    if (payload_len < FRAMING_FRAG_DATA) {
        memset(pkt.data + FRAMING_DATA_HDR_LEN + payload_len, 0,
               FRAMING_FRAG_DATA - payload_len);
        pkt.len = PACKET_MAX_LEN;
    } else {
        pkt.len = static_cast<uint8_t>(FRAMING_DATA_HDR_LEN + payload_len);
    }
}

// ── Fragment reassembler ───────────────────────────────────────────────────────
struct Reassembler {
    uint16_t seq           = FRAMING_SEQ_UNSET;
    uint8_t  buf[IP_MTU];
    uint16_t frag_len[FRAMING_MAX_FRAGS];
    uint8_t  received_mask = 0;
    uint8_t  total_frags   = 0;
    bool     ack_pending   = false;
    int8_t   last_rssi     = 0;
    uint32_t last_tick_ms  = 0;
    uint32_t ack_due_ms    = 0;

    void reset() {
        seq           = FRAMING_SEQ_UNSET;
        received_mask = 0;
        total_frags   = 0;
        ack_pending   = false;
        last_rssi     = 0;
        last_tick_ms  = 0;
        ack_due_ms    = 0;
        memset(frag_len, 0, sizeof(frag_len));
    }

    bool isComplete() const {
        const uint8_t expected = framingExpectedMask(total_frags);
        return expected != 0 && (received_mask & expected) == expected;
    }
};
