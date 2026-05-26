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
//   [7:4]  SEQ   — 4-bit frame sequence number
//   [3:2]  TYPE  — DATA or ACK control packet
//   [1:0]  IDX   — fragment index 0–3 (ACK packets use 0)
// byte 1 (DATA):
//   [7:6]  TOTAL — total fragments minus one
//   [5]    END   — final packet in the current TX burst, receiver should ACK now
// byte 2 (DATA):
//   payload length (0–FRAMING_FRAG_DATA)
// byte 1 (ACK):
//   [7:6]  TOTAL — total fragments minus one
//   [3:0]  MASK  — bitmap of received fragments
// byte 2 (ACK):
//   reserved
static constexpr uint8_t  FRAMING_DATA_HDR_LEN    = 3;
static constexpr uint8_t  FRAMING_ACK_HDR_LEN     = 3;
static constexpr uint8_t  FRAMING_FLAG_ROUND_END  = 0x20;
static constexpr uint8_t  FRAMING_SEQ_UNSET  = 0xFF;
static constexpr uint8_t  FRAMING_MAX_FRAGS  = 4;
// Data bytes per fragment. Fragmented packets use a fixed 3-byte link header.
static constexpr uint8_t  FRAMING_FRAG_DATA  = PACKET_MAX_LEN - FRAMING_DATA_HDR_LEN; // 124
static constexpr uint16_t IP_MTU             =
    static_cast<uint16_t>(FRAMING_MAX_FRAGS) * FRAMING_FRAG_DATA;                 // 496

// ── IP-layer frame (passed through FreeRTOS queues and KISS codec) ────────────
struct IpFrame {
    uint8_t  data[IP_MTU];
    uint16_t len  = 0;
    int8_t   rssi = 0;
};

struct AckFrame {
    uint8_t seq           = FRAMING_SEQ_UNSET;
    uint8_t total_frags   = 0;
    uint8_t received_mask = 0;
};

inline uint8_t framingExpectedMask(uint8_t total_frags) {
    return (total_frags == 0 || total_frags > FRAMING_MAX_FRAGS)
        ? 0
        : static_cast<uint8_t>((1u << total_frags) - 1u);
}

inline uint8_t framingEncodeByte0(uint8_t seq, LinkPacketType type, uint8_t idx) {
    return static_cast<uint8_t>((seq << 4) |
                                ((static_cast<uint8_t>(type) & 0x03u) << 2) |
                                (idx & 0x03u));
}

inline uint8_t framingPacketSeq(const Packet& pkt) {
    return static_cast<uint8_t>(pkt.data[0] >> 4);
}

inline LinkPacketType framingPacketType(const Packet& pkt) {
    return static_cast<LinkPacketType>((pkt.data[0] >> 2) & 0x03u);
}

inline uint8_t framingFragmentIndex(const Packet& pkt) {
    return static_cast<uint8_t>(pkt.data[0] & 0x03u);
}

inline uint8_t framingTotalFrags(const Packet& pkt) {
    return static_cast<uint8_t>(((pkt.data[1] >> 6) & 0x03u) + 1u);
}

inline bool framingIsRoundEnd(const Packet& pkt) {
    return (pkt.data[1] & FRAMING_FLAG_ROUND_END) != 0;
}

inline uint8_t framingPayloadLen(const Packet& pkt) {
    return pkt.data[2];
}

inline bool framingParseAck(const Packet& pkt, AckFrame& ack) {
    if (pkt.len < FRAMING_ACK_HDR_LEN || framingPacketType(pkt) != LinkPacketType::ACK) {
        return false;
    }
    ack.seq           = framingPacketSeq(pkt);
    ack.total_frags   = framingTotalFrags(pkt);
    ack.received_mask = static_cast<uint8_t>(pkt.data[1] & 0x0Fu);
    return ack.total_frags >= 1 && ack.total_frags <= FRAMING_MAX_FRAGS;
}

inline void framingBuildAckPacket(Packet& pkt, const AckFrame& ack) {
    pkt.data[0] = framingEncodeByte0(ack.seq, LinkPacketType::ACK, 0);
    pkt.data[1] = static_cast<uint8_t>(((ack.total_frags - 1u) & 0x03u) << 6);
    pkt.data[1] |= static_cast<uint8_t>(ack.received_mask & 0x0Fu);
    pkt.data[2] = 0;
    pkt.len     = FRAMING_ACK_HDR_LEN;
}

inline void framingBuildDataPacket(Packet& pkt,
                                   uint8_t seq,
                                   uint8_t idx,
                                   uint8_t total_frags,
                                   bool round_end,
                                   const uint8_t* payload,
                                   uint8_t payload_len) {
    pkt.data[0] = framingEncodeByte0(seq, LinkPacketType::DATA, idx);
    pkt.data[1] = static_cast<uint8_t>(((total_frags - 1u) & 0x03u) << 6);
    if (round_end) {
        pkt.data[1] |= FRAMING_FLAG_ROUND_END;
    }
    pkt.data[2] = payload_len;
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
    uint8_t  seq           = FRAMING_SEQ_UNSET;
    uint8_t  buf[IP_MTU];
    uint16_t frag_len[FRAMING_MAX_FRAGS];
    uint8_t  received_mask = 0;
    uint8_t  total_frags   = 0;
    bool     ack_pending   = false;
    int8_t   last_rssi     = 0;
    uint32_t last_tick_ms  = 0;

    void reset() {
        seq           = FRAMING_SEQ_UNSET;
        received_mask = 0;
        total_frags   = 0;
        ack_pending   = false;
        last_rssi     = 0;
        last_tick_ms  = 0;
        memset(frag_len, 0, sizeof(frag_len));
    }

    bool isComplete() const {
        const uint8_t expected = framingExpectedMask(total_frags);
        return expected != 0 && (received_mask & expected) == expected;
    }
};
