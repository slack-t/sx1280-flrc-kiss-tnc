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
    DATA    = 0,
    ACK     = 1,
    NATIVE  = 2,
    CONTROL = 3,
};

enum class ControlType : uint8_t {
    HEARTBEAT     = 0x01,
    HEARTBEAT_ACK = 0x02,
    DATA_PENDING  = 0x03,
    DATA_READY    = 0x04,
};

// Fixed-length control packet (bytes 0-11).
// Byte 0: [version:4][type:4], Byte 1: control_type, Bytes 2-3: seq,
// Byte 4: flags, Byte 5: reason, Byte 6: pending_frags, Byte 7: reserved,
// Bytes 8-11: token (uint32, big-endian).
static constexpr uint8_t FRAMING_CONTROL_LEN = 12;

struct ControlFrame {
    ControlType type           = ControlType::HEARTBEAT;
    uint16_t    seq            = 0;
    uint8_t     flags          = 0;
    uint8_t     reason         = 0;
    uint8_t     pending_frags  = 0;
    uint32_t    token          = 0;
};

// ── Link header ────────────────────────────────────────────────────────────────
// DATA packet:
//   byte 0  [7:4]=version, [3:0]=type
//   byte 1  flags (bit 0 = round-end, bit 1 = warmup/no-deliver)
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
static constexpr uint8_t  FRAMING_VERSION         = 2;
static constexpr uint8_t  FRAMING_VERSION_SHIFT   = 4;
static constexpr uint8_t  FRAMING_TYPE_MASK       = 0x0Fu;
static constexpr uint8_t  FRAMING_FLAG_ROUND_END  = 0x01u;
static constexpr uint8_t  FRAMING_FLAG_WARMUP     = 0x02u;
static constexpr uint8_t  FRAMING_DATA_HDR_LEN    = 13;
static constexpr uint8_t  FRAMING_ACK_HDR_LEN     = 11;
static constexpr uint8_t  FRAMING_ACK_MASK_BYTES  = 4;
static constexpr uint8_t  FRAMING_ACK_WINDOW_BITS = FRAMING_ACK_MASK_BYTES * 8u;
static constexpr uint16_t FRAMING_SEQ_UNSET       = 0xFFFF;
static constexpr uint16_t TNC_PAYLOAD_MAX_LEN     = 1280;

// Native packet: 2-byte header (version/type + payload_len), rest is payload.
// Maximum single-packet payload for native transport mode.
static constexpr uint8_t  FRAMING_NATIVE_HDR_LEN    = 2;
static constexpr uint8_t  FRAMING_NATIVE_MAX_PAYLOAD =
    static_cast<uint8_t>(PACKET_MAX_LEN - FRAMING_NATIVE_HDR_LEN);
static_assert(PACKET_MAX_LEN > FRAMING_NATIVE_HDR_LEN,
              "PACKET_MAX_LEN too small for native header");

// Data bytes per fragment. Fragmented packets use a fixed 13-byte link header.
static constexpr uint8_t FRAMING_FRAG_DATA =
    static_cast<uint8_t>(PACKET_MAX_LEN - FRAMING_DATA_HDR_LEN); // 114
static constexpr uint8_t FRAMING_MAX_FRAGS = static_cast<uint8_t>(
    (TNC_PAYLOAD_MAX_LEN + FRAMING_FRAG_DATA - 1u) / FRAMING_FRAG_DATA); // 12

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
    uint16_t frame_len   = 0;
    uint32_t frame_crc32 = 0;
    bool     round_end   = false;
    bool     warmup      = false;
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
    header.frame_len   = static_cast<uint16_t>((static_cast<uint16_t>(pkt.data[7]) << 8) | pkt.data[8]);
    header.frame_crc32 = (static_cast<uint32_t>(pkt.data[9]) << 24) |
                         (static_cast<uint32_t>(pkt.data[10]) << 16) |
                         (static_cast<uint32_t>(pkt.data[11]) << 8) |
                          static_cast<uint32_t>(pkt.data[12]);
    header.round_end   = (pkt.data[1] & FRAMING_FLAG_ROUND_END) != 0;
    header.warmup      = (pkt.data[1] & FRAMING_FLAG_WARMUP) != 0;

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
                                   uint8_t payload_len,
                                   uint16_t frame_len,
                                   uint32_t frame_crc32,
                                   bool warmup = false) {
    pkt.data[0] = framingEncodeVersionType(LinkPacketType::DATA);
    pkt.data[1] = (round_end ? FRAMING_FLAG_ROUND_END : 0) |
                  (warmup ? FRAMING_FLAG_WARMUP : 0);
    pkt.data[2] = static_cast<uint8_t>(seq >> 8);
    pkt.data[3] = static_cast<uint8_t>(seq & 0xFFu);
    pkt.data[4] = idx;
    pkt.data[5] = total_frags;
    pkt.data[6] = payload_len;
    pkt.data[7] = static_cast<uint8_t>(frame_len >> 8);
    pkt.data[8] = static_cast<uint8_t>(frame_len & 0xFFu);
    pkt.data[9] = static_cast<uint8_t>(frame_crc32 >> 24);
    pkt.data[10] = static_cast<uint8_t>((frame_crc32 >> 16) & 0xFFu);
    pkt.data[11] = static_cast<uint8_t>((frame_crc32 >> 8) & 0xFFu);
    pkt.data[12] = static_cast<uint8_t>(frame_crc32 & 0xFFu);
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

// Native packet:
//   byte 0  [7:4]=version, [3:0]=type (NATIVE=2)
//   byte 1  payload length
//   bytes 2..126  payload (zero-padded to PACKET_MAX_LEN)

inline void framingBuildNativePacket(Packet& pkt, const uint8_t* payload, uint8_t payload_len) {
    pkt.data[0] = framingEncodeVersionType(LinkPacketType::NATIVE);
    pkt.data[1] = payload_len;
    if (payload_len > 0) {
        memcpy(pkt.data + FRAMING_NATIVE_HDR_LEN, payload, payload_len);
    }
    if (payload_len < FRAMING_NATIVE_MAX_PAYLOAD) {
        memset(pkt.data + FRAMING_NATIVE_HDR_LEN + payload_len, 0,
               FRAMING_NATIVE_MAX_PAYLOAD - payload_len);
    }
    pkt.len = PACKET_MAX_LEN;
}

inline bool framingParseNativePayload(const Packet& pkt, uint8_t& payload_len) {
    if (pkt.len < FRAMING_NATIVE_HDR_LEN) { return false; }
    if (!framingHasValidVersion(pkt) || framingPacketType(pkt) != LinkPacketType::NATIVE) {
        return false;
    }
    payload_len = pkt.data[1];
    if (payload_len > FRAMING_NATIVE_MAX_PAYLOAD) { return false; }
    return pkt.len >= static_cast<uint8_t>(FRAMING_NATIVE_HDR_LEN + payload_len);
}

// ── Validation Helpers ────────────────────────────────────────────────────────

inline uint8_t framingExpectedTotalFrags(uint16_t frame_len) {
    if (frame_len == 0) return 0;
    return static_cast<uint8_t>((frame_len + FRAMING_FRAG_DATA - 1) / FRAMING_FRAG_DATA);
}

inline uint8_t framingExpectedFragmentLen(uint16_t frame_len, uint8_t frag_index, uint8_t total_frags) {
    if (frag_index >= total_frags) return 0;
    if (frag_index < total_frags - 1) {
        return FRAMING_FRAG_DATA;
    }
    uint8_t remainder = static_cast<uint8_t>(frame_len % FRAMING_FRAG_DATA);
    return remainder == 0 ? FRAMING_FRAG_DATA : remainder;
}

inline bool framingValidateDataFragment(const DataFrameHeader& header) {
    if (header.frame_len == 0 || header.frame_len > TNC_PAYLOAD_MAX_LEN) {
        return false;
    }
    if (header.total_frags != framingExpectedTotalFrags(header.frame_len)) {
        return false;
    }
    if (header.frag_index >= header.total_frags) {
        return false;
    }
    if (header.payload_len != framingExpectedFragmentLen(header.frame_len, header.frag_index, header.total_frags)) {
        return false;
    }
    return true;
}

inline void framingBuildControlPacket(Packet& pkt, const ControlFrame& ctrl) {
    pkt.data[0]  = framingEncodeVersionType(LinkPacketType::CONTROL);
    pkt.data[1]  = static_cast<uint8_t>(ctrl.type);
    pkt.data[2]  = static_cast<uint8_t>(ctrl.seq >> 8);
    pkt.data[3]  = static_cast<uint8_t>(ctrl.seq & 0xFFu);
    pkt.data[4]  = ctrl.flags;
    pkt.data[5]  = ctrl.reason;
    pkt.data[6]  = ctrl.pending_frags;
    pkt.data[7]  = 0;
    pkt.data[8]  = static_cast<uint8_t>(ctrl.token >> 24);
    pkt.data[9]  = static_cast<uint8_t>((ctrl.token >> 16) & 0xFFu);
    pkt.data[10] = static_cast<uint8_t>((ctrl.token >> 8) & 0xFFu);
    pkt.data[11] = static_cast<uint8_t>(ctrl.token & 0xFFu);
    memset(pkt.data + FRAMING_CONTROL_LEN, 0, PACKET_MAX_LEN - FRAMING_CONTROL_LEN);
    pkt.len = PACKET_MAX_LEN;
}

inline bool framingParseControl(const Packet& pkt, ControlFrame& ctrl) {
    if (pkt.len < FRAMING_CONTROL_LEN) return false;
    if (!framingHasValidVersion(pkt)) return false;
    if (framingPacketType(pkt) != LinkPacketType::CONTROL) return false;
    const ControlType type = static_cast<ControlType>(pkt.data[1]);
    if (type != ControlType::HEARTBEAT &&
        type != ControlType::HEARTBEAT_ACK &&
        type != ControlType::DATA_PENDING &&
        type != ControlType::DATA_READY) {
        return false;
    }
    ctrl.type          = type;
    ctrl.seq           = static_cast<uint16_t>(
                             (static_cast<uint16_t>(pkt.data[2]) << 8) | pkt.data[3]);
    ctrl.flags         = pkt.data[4];
    ctrl.reason        = pkt.data[5];
    ctrl.pending_frags = pkt.data[6];
    ctrl.token         = (static_cast<uint32_t>(pkt.data[8])  << 24) |
                         (static_cast<uint32_t>(pkt.data[9])  << 16) |
                         (static_cast<uint32_t>(pkt.data[10]) << 8)  |
                          static_cast<uint32_t>(pkt.data[11]);
    return true;
}

// ── Fragment reassembler ───────────────────────────────────────────────────────
struct Reassembler {
    uint16_t seq           = FRAMING_SEQ_UNSET;
    uint8_t  buf[TNC_PAYLOAD_MAX_LEN];
    uint16_t frag_len[FRAMING_MAX_FRAGS];
    uint32_t received_mask = 0;
    uint8_t  total_frags   = 0;
    uint16_t frame_len     = 0;
    uint32_t frame_crc32   = 0;
    bool     warmup        = false;
    bool     ack_pending   = false;
    int8_t   last_rssi     = 0;
    uint32_t last_tick_ms  = 0;
    uint32_t ack_due_ms    = 0;

    void reset() {
        seq           = FRAMING_SEQ_UNSET;
        received_mask = 0;
        total_frags   = 0;
        frame_len     = 0;
        frame_crc32   = 0;
        warmup        = false;
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
