#pragma once
#include <stddef.h>
#include <stdint.h>
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
// DATA packet:
//   byte 0  [7:4]=version, [3:0]=type
//   byte 1  flags (bit 0 = round-end)
//   byte 2  frame sequence high byte
//   byte 3  frame sequence low byte
//   byte 4  fragment index
//   byte 5  total fragments
//   byte 6  payload length
//
// ACK packet:
//   byte 0  [7:4]=version, [3:0]=type
//   byte 1  reserved
//   byte 2  frame sequence high byte
//   byte 3  frame sequence low byte
//   byte 4  ACK window base fragment index
//   byte 5  total fragments
//   byte 6  bitmap byte count
//   byte 7  bitmap byte 0 (LSB)
//   byte 8  bitmap byte 1
//   byte 9  bitmap byte 2
//   byte 10 bitmap byte 3 (MSB)
static constexpr uint8_t  FRAMING_VERSION         = 1;
static constexpr uint8_t  FRAMING_VERSION_SHIFT   = 4;
static constexpr uint8_t  FRAMING_TYPE_MASK       = 0x0Fu;
static constexpr uint8_t  FRAMING_FLAG_ROUND_END  = 0x01u;
static constexpr uint8_t  FRAMING_DATA_HDR_LEN    = 7;
static constexpr uint8_t  FRAMING_ACK_HDR_LEN     = 11;
static constexpr uint8_t  FRAMING_ACK_MASK_BYTES  = 4;
static constexpr uint8_t  FRAMING_ACK_WINDOW_BITS = FRAMING_ACK_MASK_BYTES * 8u;
static constexpr uint16_t FRAMING_SEQ_UNSET       = 0xFFFF;
static constexpr uint16_t TNC_PAYLOAD_MAX_LEN     = 1024;

// Data bytes per fragment. Fragmented packets use a fixed 7-byte link header.
static constexpr uint8_t FRAMING_FRAG_DATA =
    static_cast<uint8_t>(PACKET_MAX_LEN - FRAMING_DATA_HDR_LEN); // 120
static constexpr uint8_t FRAMING_MAX_FRAGS = static_cast<uint8_t>(
    (TNC_PAYLOAD_MAX_LEN + FRAMING_FRAG_DATA - 1u) / FRAMING_FRAG_DATA); // 9

static_assert(FRAMING_MAX_FRAGS <= FRAMING_ACK_WINDOW_BITS,
              "ACK bitmap window must cover the maximum fragment count");

// ── Opaque payload frame (passed through FreeRTOS queues and KISS codec) ──────
struct PayloadFrame {
    uint8_t  data[TNC_PAYLOAD_MAX_LEN];
    uint16_t len  = 0;
    int8_t   rssi = 0;
};

struct AckFrame {
    uint16_t seq           = FRAMING_SEQ_UNSET;
    uint8_t  total_frags   = 0;
    uint8_t  window_base   = 0;
    uint32_t received_mask = 0;
};

struct DataFrameHeader {
    uint16_t seq         = FRAMING_SEQ_UNSET;
    uint8_t  frag_index  = 0;
    uint8_t  total_frags = 0;
    uint8_t  payload_len = 0;
    bool     round_end   = false;
};

inline uint32_t framingExpectedMask(uint8_t total_frags) {
    if (total_frags == 0 || total_frags > FRAMING_ACK_WINDOW_BITS) {
        return 0;
    }
    if (total_frags == FRAMING_ACK_WINDOW_BITS) {
        return 0xFFFFFFFFu;
    }
    return (1u << total_frags) - 1u;
}

inline uint32_t framingMaskThrough(uint8_t idx) {
    if (idx >= (FRAMING_ACK_WINDOW_BITS - 1u)) {
        return 0xFFFFFFFFu;
    }
    return (1u << (idx + 1u)) - 1u;
}

inline uint8_t framingEncodeVersionType(LinkPacketType type) {
    return static_cast<uint8_t>((FRAMING_VERSION << FRAMING_VERSION_SHIFT) |
                                (static_cast<uint8_t>(type) & FRAMING_TYPE_MASK));
}

inline bool framingHasValidVersion(const Packet& pkt) {
    return pkt.len >= 1 &&
           static_cast<uint8_t>(pkt.data[0] >> FRAMING_VERSION_SHIFT) == FRAMING_VERSION;
}

inline LinkPacketType framingPacketType(const Packet& pkt) {
    return static_cast<LinkPacketType>(pkt.data[0] & FRAMING_TYPE_MASK);
}

inline uint16_t framingPacketSeq(const Packet& pkt) {
    return static_cast<uint16_t>((static_cast<uint16_t>(pkt.data[2]) << 8) |
                                 static_cast<uint16_t>(pkt.data[3]));
}

inline bool framingParseDataHeader(const Packet& pkt, DataFrameHeader& header) {
    if (pkt.len < FRAMING_DATA_HDR_LEN) {
        return false;
    }
    if (!framingHasValidVersion(pkt) || framingPacketType(pkt) != LinkPacketType::DATA) {
        return false;
    }

    header.seq         = framingPacketSeq(pkt);
    header.frag_index  = pkt.data[4];
    header.total_frags = pkt.data[5];
    header.payload_len = pkt.data[6];
    header.round_end   = (pkt.data[1] & FRAMING_FLAG_ROUND_END) != 0;

    if (header.total_frags == 0 || header.total_frags > FRAMING_MAX_FRAGS) {
        return false;
    }
    if (header.frag_index >= header.total_frags) {
        return false;
    }
    if (header.payload_len > FRAMING_FRAG_DATA) {
        return false;
    }
    return pkt.len >= static_cast<uint8_t>(FRAMING_DATA_HDR_LEN + header.payload_len);
}

inline bool framingParseAck(const Packet& pkt, AckFrame& ack) {
    if (pkt.len < FRAMING_ACK_HDR_LEN) {
        return false;
    }
    if (!framingHasValidVersion(pkt) || framingPacketType(pkt) != LinkPacketType::ACK) {
        return false;
    }

    ack.seq         = framingPacketSeq(pkt);
    ack.window_base = pkt.data[4];
    ack.total_frags = pkt.data[5];
    const uint8_t bitmap_bytes = pkt.data[6];
    if (bitmap_bytes != FRAMING_ACK_MASK_BYTES) {
        return false;
    }
    if (ack.total_frags == 0 || ack.total_frags > FRAMING_ACK_WINDOW_BITS) {
        return false;
    }
    if (ack.window_base >= ack.total_frags) {
        return false;
    }

    ack.received_mask =
        static_cast<uint32_t>(pkt.data[7]) |
        (static_cast<uint32_t>(pkt.data[8]) << 8) |
        (static_cast<uint32_t>(pkt.data[9]) << 16) |
        (static_cast<uint32_t>(pkt.data[10]) << 24);
    return true;
}

inline void framingBuildAckPacket(Packet& pkt, const AckFrame& ack) {
    pkt.data[0] = framingEncodeVersionType(LinkPacketType::ACK);
    pkt.data[1] = 0;
    pkt.data[2] = static_cast<uint8_t>(ack.seq >> 8);
    pkt.data[3] = static_cast<uint8_t>(ack.seq & 0xFFu);
    pkt.data[4] = ack.window_base;
    pkt.data[5] = ack.total_frags;
    pkt.data[6] = FRAMING_ACK_MASK_BYTES;
    pkt.data[7] = static_cast<uint8_t>(ack.received_mask & 0xFFu);
    pkt.data[8] = static_cast<uint8_t>((ack.received_mask >> 8) & 0xFFu);
    pkt.data[9] = static_cast<uint8_t>((ack.received_mask >> 16) & 0xFFu);
    pkt.data[10] = static_cast<uint8_t>((ack.received_mask >> 24) & 0xFFu);
    memset(pkt.data + FRAMING_ACK_HDR_LEN, 0, PACKET_MAX_LEN - FRAMING_ACK_HDR_LEN);
    pkt.len = PACKET_MAX_LEN;
}

inline void framingBuildDataPacket(Packet& pkt,
                                   uint16_t seq,
                                   uint8_t idx,
                                   uint8_t total_frags,
                                   bool round_end,
                                   const uint8_t* payload,
                                   uint8_t payload_len) {
    pkt.data[0] = framingEncodeVersionType(LinkPacketType::DATA);
    pkt.data[1] = round_end ? FRAMING_FLAG_ROUND_END : 0;
    pkt.data[2] = static_cast<uint8_t>(seq >> 8);
    pkt.data[3] = static_cast<uint8_t>(seq & 0xFFu);
    pkt.data[4] = idx;
    pkt.data[5] = total_frags;
    pkt.data[6] = payload_len;
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
    uint8_t  buf[TNC_PAYLOAD_MAX_LEN];
    uint16_t frag_len[FRAMING_MAX_FRAGS];
    uint32_t received_mask = 0;
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
        const uint32_t expected = framingExpectedMask(total_frags);
        return expected != 0 && (received_mask & expected) == expected;
    }
};
