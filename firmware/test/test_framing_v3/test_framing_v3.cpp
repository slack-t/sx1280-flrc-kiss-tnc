#include <unity.h>
#include <string.h>

#define PACKET_MAX_LEN 127
#include "../../src/framing/FramingV3.h"

using namespace framing_v3;

void setUp() {}
void tearDown() {}

static void fillPayload(uint8_t* payload, uint8_t len, uint8_t seed) {
    for (uint8_t i = 0; i < len; ++i) {
        payload[i] = static_cast<uint8_t>(seed + i * 17u);
    }
}

static DataHeader makeHeader(uint16_t datagram_len,
                             uint8_t frag_index = 0,
                             uint8_t flags = 0x05,
                             uint8_t queue_depth = 0x0A) {
    DataHeader header;
    header.flags = flags;
    header.queue_depth_hint = queue_depth;
    header.datagram_id = 0xBEEF;
    header.fragment_index = frag_index;
    header.fragment_count = expectedFragmentCount(datagram_len);
    header.datagram_length = datagram_len;
    header.payload_len = expectedFragmentPayloadLen(datagram_len,
                                                   frag_index,
                                                   header.fragment_count);
    return header;
}

static void assertDataRoundTrip(uint16_t datagram_len, uint8_t frag_index = 0) {
    uint8_t payload[V3_FRAGMENT_PAYLOAD_MAX];
    DataHeader out = makeHeader(datagram_len, frag_index);
    fillPayload(payload, out.payload_len, static_cast<uint8_t>(datagram_len));

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildDataPacket(pkt, out, payload)));

    DataHeader in;
    const uint8_t* decoded_payload = nullptr;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(parseDataPacket(pkt, in, decoded_payload)));
    TEST_ASSERT_EQUAL_UINT8(out.flags, in.flags);
    TEST_ASSERT_EQUAL_UINT8(out.queue_depth_hint, in.queue_depth_hint);
    TEST_ASSERT_EQUAL_UINT16(out.datagram_id, in.datagram_id);
    TEST_ASSERT_EQUAL_UINT8(out.fragment_index, in.fragment_index);
    TEST_ASSERT_EQUAL_UINT8(out.fragment_count, in.fragment_count);
    TEST_ASSERT_EQUAL_UINT16(out.datagram_length, in.datagram_length);
    TEST_ASSERT_EQUAL_UINT8(out.payload_len, in.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, decoded_payload, out.payload_len);
}

void test_data_roundtrip_first_fragment() {
    assertDataRoundTrip(1280, 0);
}

void test_data_roundtrip_final_fragment() {
    assertDataRoundTrip(1280, 11);
}

void test_ack_roundtrip() {
    const uint16_t bitmaps[] = {0x0000u, 0x0001u, 0x8000u, 0xFFFFu};
    for (size_t i = 0; i < sizeof(bitmaps) / sizeof(bitmaps[0]); ++i) {
        AckFrame out;
        out.datagram_id = 0x1234;
        out.fragment_bitmap = bitmaps[i];
        out.receiver_credits = 7;
        out.failure = FailureStatus::CREDIT_WITHDRAWAL;

        Packet pkt;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                                static_cast<uint8_t>(buildAckPacket(pkt, out)));

        AckFrame in;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                                static_cast<uint8_t>(parseAckPacket(pkt, in)));
        TEST_ASSERT_EQUAL_UINT16(out.datagram_id, in.datagram_id);
        TEST_ASSERT_EQUAL_HEX16(out.fragment_bitmap, in.fragment_bitmap);
        TEST_ASSERT_EQUAL_UINT8(out.receiver_credits, in.receiver_credits);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(out.failure),
                                static_cast<uint8_t>(in.failure));
    }
}

void test_control_roundtrip_all_defined_subtypes() {
    const ControlSubtype subtypes[] = {
        ControlSubtype::HEARTBEAT,
        ControlSubtype::HEARTBEAT_ACK,
        ControlSubtype::LINK_STATE,
        ControlSubtype::GRANT_RESERVED,
        ControlSubtype::PROFILE_SWITCH_RESERVED,
    };

    for (size_t i = 0; i < sizeof(subtypes) / sizeof(subtypes[0]); ++i) {
        ControlFrame out;
        out.subtype = subtypes[i];

        Packet pkt;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                                static_cast<uint8_t>(buildControlPacket(pkt, out)));

        ControlFrame in;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                                static_cast<uint8_t>(parseControlPacket(pkt, in)));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(out.subtype),
                                static_cast<uint8_t>(in.subtype));
    }
}

void test_mgmt_roundtrip_opaque_payload() {
    uint8_t payload[V3_MGMT_PAYLOAD_MAX];
    fillPayload(payload, sizeof(payload), 0x31);

    MgmtFrame out;
    out.payload = payload;
    out.payload_len = sizeof(payload);

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildMgmtPacket(pkt, out)));

    uint8_t decoded[V3_MGMT_PAYLOAD_MAX];
    MutableMgmtFrame in;
    in.payload = decoded;
    in.payload_capacity = sizeof(decoded);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(parseMgmtPacket(pkt, in)));
    TEST_ASSERT_EQUAL_UINT8(out.payload_len, in.payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, decoded, sizeof(payload));
}

void test_data_truncation_at_every_byte_boundary() {
    uint8_t payload[V3_FRAGMENT_PAYLOAD_MAX];
    DataHeader out = makeHeader(1280, 0);
    fillPayload(payload, out.payload_len, 0x44);

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildDataPacket(pkt, out, payload)));

    for (uint8_t len = 0; len < pkt.len; ++len) {
        Packet truncated = pkt;
        truncated.len = len;
        DataHeader in;
        const uint8_t* decoded_payload = nullptr;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::TRUNCATED),
                                static_cast<uint8_t>(parseDataPacket(truncated,
                                                                     in,
                                                                     decoded_payload)));
    }
}

void test_data_corrupt_version_returns_distinct_code() {
    uint8_t payload[1] = {0x42};
    DataHeader out = makeHeader(1);

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildDataPacket(pkt, out, payload)));
    pkt.data[0] = static_cast<uint8_t>((2u << V3_VERSION_SHIFT) |
                                       static_cast<uint8_t>(PacketType::DATA));

    DataHeader in;
    const uint8_t* decoded_payload = nullptr;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::VERSION_MISMATCH),
                            static_cast<uint8_t>(parseDataPacket(pkt, in, decoded_payload)));
}

void test_unknown_packet_type_rejected() {
    Packet pkt;
    pkt.len = 1;
    pkt.data[0] = static_cast<uint8_t>((V3_VERSION << V3_VERSION_SHIFT) | 0x0Fu);

    PacketType type = PacketType::DATA;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::UNKNOWN_TYPE),
                            static_cast<uint8_t>(parsePacketType(pkt, type)));
}

void test_data_corrupt_crc_rejected() {
    uint8_t payload[1] = {0x42};
    DataHeader out = makeHeader(1);

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildDataPacket(pkt, out, payload)));
    // Corrupt the payload byte (logical packet), not the trailing radio padding.
    pkt.data[V3_DATA_HDR_LEN] ^= 0x80u;

    DataHeader in;
    const uint8_t* decoded_payload = nullptr;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_CRC),
                            static_cast<uint8_t>(parseDataPacket(pkt, in, decoded_payload)));
}

void test_all_packet_types_padded_to_min_radio_len() {
    uint8_t payload[1] = {0x42};
    DataHeader header = makeHeader(1);
    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildDataPacket(pkt, header, payload)));
    TEST_ASSERT_EQUAL_UINT8(V3_MIN_RADIO_LEN, pkt.len);

    AckFrame ack;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildAckPacket(pkt, ack)));
    TEST_ASSERT_EQUAL_UINT8(V3_MIN_RADIO_LEN, pkt.len);

    ControlFrame ctrl;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildControlPacket(pkt, ctrl)));
    TEST_ASSERT_EQUAL_UINT8(V3_MIN_RADIO_LEN, pkt.len);

    MgmtFrame mgmt;
    mgmt.payload = payload;
    mgmt.payload_len = 1;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildMgmtPacket(pkt, mgmt)));
    TEST_ASSERT_EQUAL_UINT8(V3_MIN_RADIO_LEN, pkt.len);
}

void test_parse_tolerates_any_padding_length_and_content() {
    AckFrame out;
    out.datagram_id = 0x4242;
    out.fragment_bitmap = 0x0007u;
    out.receiver_credits = 5;
    out.failure = FailureStatus::NONE;

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildAckPacket(pkt, out)));

    for (uint8_t len = V3_ACK_LEN; len <= V3_MIN_RADIO_LEN; ++len) {
        Packet received = pkt;
        received.len = len;
        if (len > V3_ACK_LEN) {
            // Padding content must not matter — it is not CRC-protected.
            received.data[len - 1] = 0xA5u;
        }
        AckFrame in;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                                static_cast<uint8_t>(parseAckPacket(received, in)));
        TEST_ASSERT_EQUAL_UINT16(out.datagram_id, in.datagram_id);
    }

    Packet truncated = pkt;
    truncated.len = V3_ACK_LEN - 1u;
    AckFrame in;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::TRUNCATED),
                            static_cast<uint8_t>(parseAckPacket(truncated, in)));
}

void test_datagram_length_edges() {
    const uint16_t valid_lengths[] = {1, 114, 115, 1279, 1280};
    for (size_t i = 0; i < sizeof(valid_lengths) / sizeof(valid_lengths[0]); ++i) {
        assertDataRoundTrip(valid_lengths[i],
                            static_cast<uint8_t>(expectedFragmentCount(valid_lengths[i]) - 1u));
    }

    DataHeader zero = makeHeader(1);
    zero.datagram_length = 0;
    zero.fragment_count = 0;
    zero.payload_len = 0;
    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_FRAGMENT),
                            static_cast<uint8_t>(buildDataPacket(pkt, zero, nullptr)));

    DataHeader too_large = makeHeader(1280);
    too_large.datagram_length = 1281;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_FRAGMENT),
                            static_cast<uint8_t>(buildDataPacket(pkt, too_large, nullptr)));
}

void test_parse_rejects_datagram_length_zero_and_1281() {
    uint8_t payload[1] = {0x42};
    DataHeader out = makeHeader(1);

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildDataPacket(pkt, out, payload)));

    writeLe16(pkt.data + 6, 0);
    writeLe32(pkt.data + 8, detail::crcWithZeroedField(pkt.data, pkt.len, 8));
    DataHeader in;
    const uint8_t* decoded_payload = nullptr;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_LENGTH),
                            static_cast<uint8_t>(parseDataPacket(pkt, in, decoded_payload)));

    writeLe16(pkt.data + 6, 1281);
    writeLe32(pkt.data + 8, detail::crcWithZeroedField(pkt.data, pkt.len, 8));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_LENGTH),
                            static_cast<uint8_t>(parseDataPacket(pkt, in, decoded_payload)));
}

void test_fragment_count_edges() {
    uint8_t payload[V3_FRAGMENT_PAYLOAD_MAX];
    DataHeader twelve = makeHeader(1280, 0);
    fillPayload(payload, twelve.payload_len, 0x55);
    TEST_ASSERT_EQUAL_UINT8(12, twelve.fragment_count);

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildDataPacket(pkt, twelve, payload)));

    DataHeader in;
    const uint8_t* decoded_payload = nullptr;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(parseDataPacket(pkt, in, decoded_payload)));

    DataHeader zero = makeHeader(1);
    zero.fragment_count = 0;
    zero.payload_len = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_FRAGMENT),
                            static_cast<uint8_t>(buildDataPacket(pkt, zero, nullptr)));

    DataHeader thirteen = makeHeader(1280);
    thirteen.fragment_count = 13;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_FRAGMENT),
                            static_cast<uint8_t>(buildDataPacket(pkt, thirteen, payload)));
}

void test_mgmt_output_too_small() {
    uint8_t payload[3] = {1, 2, 3};
    MgmtFrame out;
    out.payload = payload;
    out.payload_len = sizeof(payload);

    Packet pkt;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildMgmtPacket(pkt, out)));

    uint8_t decoded[2];
    MutableMgmtFrame in;
    in.payload = decoded;
    in.payload_capacity = sizeof(decoded);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OUTPUT_TOO_SMALL),
                            static_cast<uint8_t>(parseMgmtPacket(pkt, in)));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_data_roundtrip_first_fragment);
    RUN_TEST(test_data_roundtrip_final_fragment);
    RUN_TEST(test_ack_roundtrip);
    RUN_TEST(test_control_roundtrip_all_defined_subtypes);
    RUN_TEST(test_mgmt_roundtrip_opaque_payload);
    RUN_TEST(test_data_truncation_at_every_byte_boundary);
    RUN_TEST(test_data_corrupt_version_returns_distinct_code);
    RUN_TEST(test_unknown_packet_type_rejected);
    RUN_TEST(test_data_corrupt_crc_rejected);
    RUN_TEST(test_all_packet_types_padded_to_min_radio_len);
    RUN_TEST(test_parse_tolerates_any_padding_length_and_content);
    RUN_TEST(test_datagram_length_edges);
    RUN_TEST(test_parse_rejects_datagram_length_zero_and_1281);
    RUN_TEST(test_fragment_count_edges);
    RUN_TEST(test_mgmt_output_too_small);
    return UNITY_END();
}
