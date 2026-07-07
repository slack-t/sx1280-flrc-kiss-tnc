#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// PACKET_MAX_LEN must be defined before including this header (via config.h).

namespace framing_v3 {

// v3 packets use little-endian field packing. The serializer is the wire
// definition:
//
// Common byte 0: [7:4]=version, [3:0]=packet type.
//
// DATA, 12-byte header:
//   byte 0      version/type
//   byte 1      [7:4]=queue-depth hint, [3:0]=flags
//   bytes 2-3   datagram ID, little-endian
//   byte 4      fragment index
//   byte 5      fragment count
//   bytes 6-7   datagram length, little-endian
//   bytes 8-11  CRC32, little-endian
//   bytes 12..  fragment payload
//
// The DATA CRC32 is computed over bytes 0-7, four zero bytes in place of the
// CRC field, then the fragment payload. ACK, CONTROL, and MGMT use the same
// convention: their CRC field bytes are zero while computing the packet CRC.
//
// ACK:
//   byte 0      version/type
//   bytes 1-2   datagram ID, little-endian
//   bytes 3-4   received fragment bitmap, little-endian
//   byte 5      receiver credits
//   byte 6      failure status
//   bytes 7-10  CRC32, little-endian
//
// CONTROL:
//   byte 0      version/type
//   byte 1      subtype
//   bytes 2-5   CRC32, little-endian
//
// MGMT:
//   byte 0      version/type
//   byte 1      opaque payload length
//   bytes 2..N  opaque payload
//   final 4 B   CRC32, little-endian

static constexpr uint8_t V3_VERSION = 3;
static constexpr uint8_t V3_VERSION_SHIFT = 4;
static constexpr uint8_t V3_TYPE_MASK = 0x0Fu;
static constexpr uint8_t V3_MAX_NIBBLE = 0x0Fu;

static constexpr uint16_t V3_MAX_DATAGRAM = 1280;
static constexpr uint8_t V3_MAX_FRAGS = 12;
static constexpr uint8_t V3_DATA_HDR_LEN = 12;
static constexpr uint8_t V3_ACK_LEN = 11;
static constexpr uint8_t V3_CONTROL_LEN = 6;
static constexpr uint8_t V3_MGMT_HDR_LEN = 2;
static constexpr uint8_t V3_CRC_LEN = 4;
static constexpr uint8_t V3_FRAGMENT_PAYLOAD_MAX =
    static_cast<uint8_t>(PACKET_MAX_LEN - V3_DATA_HDR_LEN);
static constexpr uint8_t V3_MGMT_PAYLOAD_MAX =
    static_cast<uint8_t>(PACKET_MAX_LEN - V3_MGMT_HDR_LEN - V3_CRC_LEN);

static_assert(PACKET_MAX_LEN >= 127, "v3 framing expects 127-byte FLRC packets");
static_assert(V3_DATA_HDR_LEN <= 13, "v3 DATA header budget exceeded");
static_assert(V3_FRAGMENT_PAYLOAD_MAX >= 114, "v3 fragment payload too small");
static_assert(((V3_MAX_DATAGRAM + V3_FRAGMENT_PAYLOAD_MAX - 1u) /
               V3_FRAGMENT_PAYLOAD_MAX) <= V3_MAX_FRAGS,
              "v3 fragment limit cannot carry max datagram");

enum class PacketType : uint8_t {
    DATA = 0,
    ACK = 1,
    CONTROL = 2,
    MGMT = 3,
};

enum class ControlSubtype : uint8_t {
    HEARTBEAT = 0x01,
    HEARTBEAT_ACK = 0x02,
    LINK_STATE = 0x03,
    GRANT_RESERVED = 0x10,
    PROFILE_SWITCH_RESERVED = 0x20,
};

enum class FailureStatus : uint8_t {
    NONE = 0,
    RESET = 1,
    RETRY_EXHAUSTED = 2,
    SATURATION = 3,
    MALFORMED_INPUT = 4,
    CREDIT_WITHDRAWAL = 5,
    ALLOCATION_FAILURE = 6,
};

enum class ParseResult : uint8_t {
    OK = 0,
    TRUNCATED,
    VERSION_MISMATCH,
    UNKNOWN_TYPE,
    WRONG_TYPE,
    BAD_LENGTH,
    BAD_FRAGMENT,
    BAD_VALUE,
    BAD_CRC,
    OUTPUT_TOO_SMALL,
};

struct Packet {
    uint8_t data[PACKET_MAX_LEN];
    uint8_t len = 0;
};

struct DataHeader {
    uint8_t flags = 0;
    uint8_t queue_depth_hint = 0;
    uint16_t datagram_id = 0;
    uint8_t fragment_index = 0;
    uint8_t fragment_count = 0;
    uint16_t datagram_length = 0;
    uint8_t payload_len = 0;
};

struct AckFrame {
    uint16_t datagram_id = 0;
    uint16_t fragment_bitmap = 0;
    uint8_t receiver_credits = 0;
    FailureStatus failure = FailureStatus::NONE;
};

struct ControlFrame {
    ControlSubtype subtype = ControlSubtype::HEARTBEAT;
};

struct MgmtFrame {
    const uint8_t* payload = nullptr;
    uint8_t payload_len = 0;
};

struct MutableMgmtFrame {
    uint8_t* payload = nullptr;
    uint8_t payload_capacity = 0;
    uint8_t payload_len = 0;
};

inline uint8_t encodeVersionType(PacketType type) {
    return static_cast<uint8_t>((V3_VERSION << V3_VERSION_SHIFT) |
                                (static_cast<uint8_t>(type) & V3_TYPE_MASK));
}

inline uint8_t packetVersion(const Packet& pkt) {
    return static_cast<uint8_t>(pkt.data[0] >> V3_VERSION_SHIFT);
}

inline PacketType packetTypeUnchecked(const Packet& pkt) {
    return static_cast<PacketType>(pkt.data[0] & V3_TYPE_MASK);
}

inline void writeLe16(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
}

inline uint16_t readLe16(const uint8_t* src) {
    return static_cast<uint16_t>(src[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(src[1]) << 8);
}

inline void writeLe32(uint8_t* dst, uint32_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xFFu);
    dst[1] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    dst[2] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    dst[3] = static_cast<uint8_t>((value >> 24) & 0xFFu);
}

inline uint32_t readLe32(const uint8_t* src) {
    return static_cast<uint32_t>(src[0]) |
           (static_cast<uint32_t>(src[1]) << 8) |
           (static_cast<uint32_t>(src[2]) << 16) |
           (static_cast<uint32_t>(src[3]) << 24);
}

inline bool isKnownPacketType(uint8_t raw_type) {
    return raw_type <= static_cast<uint8_t>(PacketType::MGMT);
}

inline bool isKnownControlSubtype(ControlSubtype subtype) {
    switch (subtype) {
        case ControlSubtype::HEARTBEAT:
        case ControlSubtype::HEARTBEAT_ACK:
        case ControlSubtype::LINK_STATE:
        case ControlSubtype::GRANT_RESERVED:
        case ControlSubtype::PROFILE_SWITCH_RESERVED:
            return true;
    }
    return false;
}

inline bool isKnownFailureStatus(FailureStatus status) {
    switch (status) {
        case FailureStatus::NONE:
        case FailureStatus::RESET:
        case FailureStatus::RETRY_EXHAUSTED:
        case FailureStatus::SATURATION:
        case FailureStatus::MALFORMED_INPUT:
        case FailureStatus::CREDIT_WITHDRAWAL:
        case FailureStatus::ALLOCATION_FAILURE:
            return true;
    }
    return false;
}

inline uint8_t expectedFragmentCount(uint16_t datagram_length) {
    if (datagram_length == 0 || datagram_length > V3_MAX_DATAGRAM) {
        return 0;
    }
    return static_cast<uint8_t>((datagram_length + V3_FRAGMENT_PAYLOAD_MAX - 1u) /
                                V3_FRAGMENT_PAYLOAD_MAX);
}

inline uint8_t expectedFragmentPayloadLen(uint16_t datagram_length,
                                          uint8_t fragment_index,
                                          uint8_t fragment_count) {
    if (datagram_length == 0 || datagram_length > V3_MAX_DATAGRAM ||
        fragment_count == 0 || fragment_count > V3_MAX_FRAGS ||
        fragment_index >= fragment_count ||
        fragment_count != expectedFragmentCount(datagram_length)) {
        return 0;
    }
    if (fragment_index < static_cast<uint8_t>(fragment_count - 1u)) {
        return V3_FRAGMENT_PAYLOAD_MAX;
    }
    const uint16_t used_before =
        static_cast<uint16_t>(fragment_index) * V3_FRAGMENT_PAYLOAD_MAX;
    return static_cast<uint8_t>(datagram_length - used_before);
}

inline bool validateDataHeader(const DataHeader& header) {
    if (header.flags > V3_MAX_NIBBLE || header.queue_depth_hint > V3_MAX_NIBBLE) {
        return false;
    }
    if (header.datagram_length == 0 || header.datagram_length > V3_MAX_DATAGRAM) {
        return false;
    }
    if (header.fragment_count == 0 || header.fragment_count > V3_MAX_FRAGS) {
        return false;
    }
    if (header.fragment_count != expectedFragmentCount(header.datagram_length)) {
        return false;
    }
    if (header.fragment_index >= header.fragment_count) {
        return false;
    }
    return header.payload_len == expectedFragmentPayloadLen(header.datagram_length,
                                                           header.fragment_index,
                                                           header.fragment_count);
}

namespace detail {

inline uint32_t crc32Update(uint32_t crc, uint8_t value) {
    crc ^= value;
    for (uint8_t bit = 0; bit < 8; ++bit) {
        if ((crc & 1u) != 0) {
            crc = (crc >> 1) ^ 0xEDB88320u;
        } else {
            crc >>= 1;
        }
    }
    return crc;
}

inline uint32_t crc32UpdateBytes(uint32_t crc, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        crc = crc32Update(crc, data[i]);
    }
    return crc;
}

inline uint32_t crcWithZeroedField(const uint8_t* data,
                                   size_t len,
                                   size_t crc_offset) {
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32UpdateBytes(crc, data, crc_offset);
    for (uint8_t i = 0; i < V3_CRC_LEN; ++i) {
        crc = crc32Update(crc, 0);
    }
    crc = crc32UpdateBytes(crc,
                           data + crc_offset + V3_CRC_LEN,
                           len - crc_offset - V3_CRC_LEN);
    return ~crc;
}

inline ParseResult validateCommon(const Packet& pkt, PacketType expected_type) {
    if (pkt.len < 1) {
        return ParseResult::TRUNCATED;
    }
    if (packetVersion(pkt) != V3_VERSION) {
        return ParseResult::VERSION_MISMATCH;
    }
    const uint8_t raw_type = static_cast<uint8_t>(pkt.data[0] & V3_TYPE_MASK);
    if (!isKnownPacketType(raw_type)) {
        return ParseResult::UNKNOWN_TYPE;
    }
    if (static_cast<PacketType>(raw_type) != expected_type) {
        return ParseResult::WRONG_TYPE;
    }
    return ParseResult::OK;
}

} // namespace detail

inline ParseResult parsePacketType(const Packet& pkt, PacketType& type) {
    if (pkt.len < 1) {
        return ParseResult::TRUNCATED;
    }
    if (packetVersion(pkt) != V3_VERSION) {
        return ParseResult::VERSION_MISMATCH;
    }
    const uint8_t raw_type = static_cast<uint8_t>(pkt.data[0] & V3_TYPE_MASK);
    if (!isKnownPacketType(raw_type)) {
        return ParseResult::UNKNOWN_TYPE;
    }
    type = static_cast<PacketType>(raw_type);
    return ParseResult::OK;
}

inline ParseResult buildDataPacket(Packet& pkt,
                                   const DataHeader& header,
                                   const uint8_t* payload) {
    if (!validateDataHeader(header)) {
        return ParseResult::BAD_FRAGMENT;
    }
    if (header.payload_len > 0 && payload == nullptr) {
        return ParseResult::BAD_VALUE;
    }
    const uint16_t packet_len =
        static_cast<uint16_t>(V3_DATA_HDR_LEN + header.payload_len);
    if (packet_len > PACKET_MAX_LEN) {
        return ParseResult::BAD_LENGTH;
    }

    pkt.data[0] = encodeVersionType(PacketType::DATA);
    pkt.data[1] = static_cast<uint8_t>((header.queue_depth_hint << 4) |
                                       (header.flags & V3_MAX_NIBBLE));
    writeLe16(pkt.data + 2, header.datagram_id);
    pkt.data[4] = header.fragment_index;
    pkt.data[5] = header.fragment_count;
    writeLe16(pkt.data + 6, header.datagram_length);
    memset(pkt.data + 8, 0, V3_CRC_LEN);
    if (header.payload_len > 0) {
        memcpy(pkt.data + V3_DATA_HDR_LEN, payload, header.payload_len);
    }
    pkt.len = static_cast<uint8_t>(packet_len);
    writeLe32(pkt.data + 8, detail::crcWithZeroedField(pkt.data, pkt.len, 8));
    return ParseResult::OK;
}

inline ParseResult parseDataPacket(const Packet& pkt,
                                   DataHeader& header,
                                   const uint8_t*& payload) {
    ParseResult result = detail::validateCommon(pkt, PacketType::DATA);
    if (result != ParseResult::OK) {
        return result;
    }
    if (pkt.len < V3_DATA_HDR_LEN) {
        return ParseResult::TRUNCATED;
    }

    DataHeader decoded;
    decoded.flags = static_cast<uint8_t>(pkt.data[1] & V3_MAX_NIBBLE);
    decoded.queue_depth_hint = static_cast<uint8_t>(pkt.data[1] >> 4);
    decoded.datagram_id = readLe16(pkt.data + 2);
    decoded.fragment_index = pkt.data[4];
    decoded.fragment_count = pkt.data[5];
    decoded.datagram_length = readLe16(pkt.data + 6);

    if (decoded.datagram_length == 0 || decoded.datagram_length > V3_MAX_DATAGRAM) {
        return ParseResult::BAD_LENGTH;
    }
    if (decoded.fragment_count == 0 || decoded.fragment_count > V3_MAX_FRAGS ||
        decoded.fragment_count != expectedFragmentCount(decoded.datagram_length) ||
        decoded.fragment_index >= decoded.fragment_count) {
        return ParseResult::BAD_FRAGMENT;
    }

    decoded.payload_len = expectedFragmentPayloadLen(decoded.datagram_length,
                                                     decoded.fragment_index,
                                                     decoded.fragment_count);
    const uint16_t expected_len =
        static_cast<uint16_t>(V3_DATA_HDR_LEN + decoded.payload_len);
    if (pkt.len < expected_len) {
        return ParseResult::TRUNCATED;
    }
    if (pkt.len != expected_len) {
        return ParseResult::BAD_LENGTH;
    }
    if (readLe32(pkt.data + 8) != detail::crcWithZeroedField(pkt.data, pkt.len, 8)) {
        return ParseResult::BAD_CRC;
    }

    header = decoded;
    payload = pkt.data + V3_DATA_HDR_LEN;
    return ParseResult::OK;
}

inline ParseResult buildAckPacket(Packet& pkt, const AckFrame& ack) {
    if (!isKnownFailureStatus(ack.failure)) {
        return ParseResult::BAD_VALUE;
    }
    pkt.data[0] = encodeVersionType(PacketType::ACK);
    writeLe16(pkt.data + 1, ack.datagram_id);
    writeLe16(pkt.data + 3, ack.fragment_bitmap);
    pkt.data[5] = ack.receiver_credits;
    pkt.data[6] = static_cast<uint8_t>(ack.failure);
    memset(pkt.data + 7, 0, V3_CRC_LEN);
    pkt.len = V3_ACK_LEN;
    writeLe32(pkt.data + 7, detail::crcWithZeroedField(pkt.data, pkt.len, 7));
    return ParseResult::OK;
}

inline ParseResult parseAckPacket(const Packet& pkt, AckFrame& ack) {
    ParseResult result = detail::validateCommon(pkt, PacketType::ACK);
    if (result != ParseResult::OK) {
        return result;
    }
    if (pkt.len < V3_ACK_LEN) {
        return ParseResult::TRUNCATED;
    }
    if (pkt.len != V3_ACK_LEN) {
        return ParseResult::BAD_LENGTH;
    }
    const FailureStatus status = static_cast<FailureStatus>(pkt.data[6]);
    if (!isKnownFailureStatus(status)) {
        return ParseResult::BAD_VALUE;
    }
    if (readLe32(pkt.data + 7) != detail::crcWithZeroedField(pkt.data, pkt.len, 7)) {
        return ParseResult::BAD_CRC;
    }
    ack.datagram_id = readLe16(pkt.data + 1);
    ack.fragment_bitmap = readLe16(pkt.data + 3);
    ack.receiver_credits = pkt.data[5];
    ack.failure = status;
    return ParseResult::OK;
}

inline ParseResult buildControlPacket(Packet& pkt, const ControlFrame& ctrl) {
    if (!isKnownControlSubtype(ctrl.subtype)) {
        return ParseResult::BAD_VALUE;
    }
    pkt.data[0] = encodeVersionType(PacketType::CONTROL);
    pkt.data[1] = static_cast<uint8_t>(ctrl.subtype);
    memset(pkt.data + 2, 0, V3_CRC_LEN);
    pkt.len = V3_CONTROL_LEN;
    writeLe32(pkt.data + 2, detail::crcWithZeroedField(pkt.data, pkt.len, 2));
    return ParseResult::OK;
}

inline ParseResult parseControlPacket(const Packet& pkt, ControlFrame& ctrl) {
    ParseResult result = detail::validateCommon(pkt, PacketType::CONTROL);
    if (result != ParseResult::OK) {
        return result;
    }
    if (pkt.len < V3_CONTROL_LEN) {
        return ParseResult::TRUNCATED;
    }
    if (pkt.len != V3_CONTROL_LEN) {
        return ParseResult::BAD_LENGTH;
    }
    const ControlSubtype subtype = static_cast<ControlSubtype>(pkt.data[1]);
    if (!isKnownControlSubtype(subtype)) {
        return ParseResult::BAD_VALUE;
    }
    if (readLe32(pkt.data + 2) != detail::crcWithZeroedField(pkt.data, pkt.len, 2)) {
        return ParseResult::BAD_CRC;
    }
    ctrl.subtype = subtype;
    return ParseResult::OK;
}

inline ParseResult buildMgmtPacket(Packet& pkt, const MgmtFrame& mgmt) {
    if (mgmt.payload_len > V3_MGMT_PAYLOAD_MAX) {
        return ParseResult::BAD_LENGTH;
    }
    if (mgmt.payload_len > 0 && mgmt.payload == nullptr) {
        return ParseResult::BAD_VALUE;
    }
    pkt.data[0] = encodeVersionType(PacketType::MGMT);
    pkt.data[1] = mgmt.payload_len;
    if (mgmt.payload_len > 0) {
        memcpy(pkt.data + V3_MGMT_HDR_LEN, mgmt.payload, mgmt.payload_len);
    }
    const size_t crc_offset = static_cast<size_t>(V3_MGMT_HDR_LEN + mgmt.payload_len);
    memset(pkt.data + crc_offset, 0, V3_CRC_LEN);
    pkt.len = static_cast<uint8_t>(crc_offset + V3_CRC_LEN);
    writeLe32(pkt.data + crc_offset,
              detail::crcWithZeroedField(pkt.data, pkt.len, crc_offset));
    return ParseResult::OK;
}

inline ParseResult parseMgmtPacket(const Packet& pkt, MutableMgmtFrame& mgmt) {
    ParseResult result = detail::validateCommon(pkt, PacketType::MGMT);
    if (result != ParseResult::OK) {
        return result;
    }
    if (pkt.len < static_cast<uint8_t>(V3_MGMT_HDR_LEN + V3_CRC_LEN)) {
        return ParseResult::TRUNCATED;
    }
    const uint8_t payload_len = pkt.data[1];
    if (payload_len > V3_MGMT_PAYLOAD_MAX) {
        return ParseResult::BAD_LENGTH;
    }
    const uint8_t expected_len =
        static_cast<uint8_t>(V3_MGMT_HDR_LEN + payload_len + V3_CRC_LEN);
    if (pkt.len < expected_len) {
        return ParseResult::TRUNCATED;
    }
    if (pkt.len != expected_len) {
        return ParseResult::BAD_LENGTH;
    }
    const size_t crc_offset = static_cast<size_t>(V3_MGMT_HDR_LEN + payload_len);
    if (readLe32(pkt.data + crc_offset) !=
        detail::crcWithZeroedField(pkt.data, pkt.len, crc_offset)) {
        return ParseResult::BAD_CRC;
    }
    if (payload_len > mgmt.payload_capacity) {
        return ParseResult::OUTPUT_TOO_SMALL;
    }
    if (payload_len > 0) {
        if (mgmt.payload == nullptr) {
            return ParseResult::OUTPUT_TOO_SMALL;
        }
        memcpy(mgmt.payload, pkt.data + V3_MGMT_HDR_LEN, payload_len);
    }
    mgmt.payload_len = payload_len;
    return ParseResult::OK;
}

} // namespace framing_v3
