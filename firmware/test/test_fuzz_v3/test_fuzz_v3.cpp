#include <unity.h>
#include <string.h>

#define PACKET_MAX_LEN 127
#include "../../src/arq/ArqEngine.h"
#include "../../src/arq/ArqEngine.cpp"

using namespace arq;
using namespace framing_v3;

void setUp() {}
void tearDown() {}

struct Lcg {
    uint32_t state;

    explicit Lcg(uint32_t seed) : state(seed) {}

    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }

    uint8_t byte() {
        return static_cast<uint8_t>(next() >> 24);
    }
};

struct FuzzSink {
    uint32_t sent = 0;
    uint32_t delivered = 0;
    uint32_t duplicates = 0;
    bool seen = false;
};

static bool sendPacket(const Packet& packet, void* user) {
    (void)packet;
    FuzzSink* sink = static_cast<FuzzSink*>(user);
    sink->sent++;
    return true;
}

static bool deliverDatagram(const uint8_t* data, uint16_t len, void* user) {
    (void)data;
    (void)len;
    FuzzSink* sink = static_cast<FuzzSink*>(user);
    if (sink->seen) {
        sink->duplicates++;
    } else {
        sink->seen = true;
        sink->delivered++;
    }
    return true;
}

static uint8_t egressCapacity(void* user) {
    (void)user;
    return 8;
}

static void initEngine(ArqEngine& engine, FuzzSink& sink) {
    ArqConfig config;
    config.retry_timeout_cycles = 16;
    config.max_attempts = 8;
    config.initial_remote_credits = ARQ_MAX_OUTSTANDING;

    ArqCallbacks callbacks;
    callbacks.send_packet = sendPacket;
    callbacks.deliver_datagram = deliverDatagram;
    callbacks.egress_capacity = egressCapacity;
    callbacks.user = &sink;
    engine.reset(config, callbacks);
}

static ParseResult parseByType(const Packet& packet) {
    PacketType type = PacketType::DATA;
    ParseResult result = parsePacketType(packet, type);
    if (result != ParseResult::OK) {
        return result;
    }

    if (type == PacketType::DATA) {
        DataHeader header;
        const uint8_t* payload = nullptr;
        return parseDataPacket(packet, header, payload);
    }
    if (type == PacketType::ACK) {
        AckFrame ack;
        return parseAckPacket(packet, ack);
    }
    if (type == PacketType::CONTROL) {
        ControlFrame ctrl;
        return parseControlPacket(packet, ctrl);
    }

    uint8_t payload[V3_MGMT_PAYLOAD_MAX];
    MutableMgmtFrame mgmt;
    mgmt.payload = payload;
    mgmt.payload_capacity = sizeof(payload);
    return parseMgmtPacket(packet, mgmt);
}

static void rewriteDataCrc(Packet& packet) {
    writeLe32(packet.data + 8, detail::crcWithZeroedField(packet.data, packet.len, 8));
}

static void rewriteAckCrc(Packet& packet) {
    writeLe32(packet.data + 7, detail::crcWithZeroedField(packet.data, V3_ACK_LEN, 7));
}

static void rewriteControlCrc(Packet& packet) {
    writeLe32(packet.data + 2, detail::crcWithZeroedField(packet.data, V3_CONTROL_LEN, 2));
}

static void rewriteMgmtCrc(Packet& packet) {
    const uint8_t payload_len = packet.data[1];
    const size_t crc_offset = static_cast<size_t>(V3_MGMT_HDR_LEN + payload_len);
    writeLe32(packet.data + crc_offset,
              detail::crcWithZeroedField(packet.data, crc_offset + V3_CRC_LEN, crc_offset));
}

static Packet validDataPacket(uint16_t datagram_len = 115) {
    uint8_t payload[V3_FRAGMENT_PAYLOAD_MAX];
    for (uint8_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = static_cast<uint8_t>(i * 13u);
    }

    DataHeader header;
    header.flags = 0;
    header.queue_depth_hint = 0;
    header.datagram_id = 0x1001;
    header.fragment_index = 0;
    header.fragment_count = expectedFragmentCount(datagram_len);
    header.datagram_length = datagram_len;
    header.payload_len = expectedFragmentPayloadLen(datagram_len, 0, header.fragment_count);

    Packet packet;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildDataPacket(packet, header, payload)));
    return packet;
}

static Packet validAckPacket() {
    AckFrame ack;
    ack.datagram_id = 0x2002;
    ack.fragment_bitmap = 0x0FFFu;
    ack.receiver_credits = 3;
    ack.failure = FailureStatus::NONE;

    Packet packet;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildAckPacket(packet, ack)));
    return packet;
}

static Packet validControlPacket() {
    ControlFrame ctrl;
    ctrl.subtype = ControlSubtype::HEARTBEAT;

    Packet packet;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildControlPacket(packet, ctrl)));
    return packet;
}

static Packet validMgmtPacket() {
    const uint8_t bytes[] = {0x01, 0x02, 0xA5, 0x5A};
    MgmtFrame mgmt;
    mgmt.payload = bytes;
    mgmt.payload_len = sizeof(bytes);

    Packet packet;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(buildMgmtPacket(packet, mgmt)));
    return packet;
}

static void assertEngineMalformedForRejected(const Packet& packet) {
    TEST_ASSERT_NOT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                                static_cast<uint8_t>(parseByType(packet)));

    FuzzSink sink;
    memset(&sink, 0, sizeof(sink));
    ArqEngine engine;
    initEngine(engine, sink);
    const uint32_t before = engine.counters().malformed_input;
    engine.onRxPacket(packet, 0);
    TEST_ASSERT_EQUAL_UINT32(before + 1u, engine.counters().malformed_input);
}

void test_crafted_parser_rejection_vectors() {
    Packet packet;
    memset(&packet, 0, sizeof(packet));
    packet.len = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::TRUNCATED),
                            static_cast<uint8_t>(parseByType(packet)));
    assertEngineMalformedForRejected(packet);

    packet.len = 1;
    packet.data[0] = static_cast<uint8_t>((2u << V3_VERSION_SHIFT) |
                                          static_cast<uint8_t>(PacketType::DATA));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::VERSION_MISMATCH),
                            static_cast<uint8_t>(parseByType(packet)));
    assertEngineMalformedForRejected(packet);

    packet.data[0] = static_cast<uint8_t>((V3_VERSION << V3_VERSION_SHIFT) | 0x0Fu);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::UNKNOWN_TYPE),
                            static_cast<uint8_t>(parseByType(packet)));
    assertEngineMalformedForRejected(packet);

    Packet data = validDataPacket();
    AckFrame ack;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::WRONG_TYPE),
                            static_cast<uint8_t>(parseAckPacket(data, ack)));

    Packet short_data = data;
    short_data.len = static_cast<uint8_t>(V3_DATA_HDR_LEN - 1u);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::TRUNCATED),
                            static_cast<uint8_t>(parseByType(short_data)));
    assertEngineMalformedForRejected(short_data);

    // Bytes past the logical packet are radio padding: any padding length
    // and content must parse cleanly.
    Packet padded = validDataPacket(1);
    TEST_ASSERT_EQUAL_UINT8(V3_MIN_RADIO_LEN, padded.len);
    padded.len = static_cast<uint8_t>(V3_DATA_HDR_LEN + 1u + 3u);
    padded.data[padded.len - 1u] = 0xEEu;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                            static_cast<uint8_t>(parseByType(padded)));

    Packet too_large = data;
    writeLe16(too_large.data + 6, 1281);
    rewriteDataCrc(too_large);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_LENGTH),
                            static_cast<uint8_t>(parseByType(too_large)));
    assertEngineMalformedForRejected(too_large);

    Packet bad_fragment = data;
    bad_fragment.data[5] = 0;
    rewriteDataCrc(bad_fragment);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_FRAGMENT),
                            static_cast<uint8_t>(parseByType(bad_fragment)));
    assertEngineMalformedForRejected(bad_fragment);

    Packet bad_data_crc = data;
    bad_data_crc.data[8] ^= 0x01u;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_CRC),
                            static_cast<uint8_t>(parseByType(bad_data_crc)));
    assertEngineMalformedForRejected(bad_data_crc);

    Packet bad_ack = validAckPacket();
    bad_ack.data[6] = 0xEEu;
    rewriteAckCrc(bad_ack);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_VALUE),
                            static_cast<uint8_t>(parseByType(bad_ack)));
    assertEngineMalformedForRejected(bad_ack);

    Packet bad_ctrl = validControlPacket();
    bad_ctrl.data[1] = 0xEEu;
    rewriteControlCrc(bad_ctrl);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_VALUE),
                            static_cast<uint8_t>(parseByType(bad_ctrl)));
    assertEngineMalformedForRejected(bad_ctrl);

    Packet mgmt = validMgmtPacket();
    uint8_t too_small[2];
    MutableMgmtFrame out;
    out.payload = too_small;
    out.payload_capacity = sizeof(too_small);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OUTPUT_TOO_SMALL),
                            static_cast<uint8_t>(parseMgmtPacket(mgmt, out)));

    Packet bad_mgmt_crc = mgmt;
    bad_mgmt_crc.data[V3_MGMT_HDR_LEN] ^= 0x80u;  // payload byte, CRC-covered
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_CRC),
                            static_cast<uint8_t>(parseByType(bad_mgmt_crc)));
    assertEngineMalformedForRejected(bad_mgmt_crc);
}

void test_bad_crc_is_never_accepted_for_each_packet_type() {
    Packet packets[] = {
        validDataPacket(),
        validAckPacket(),
        validControlPacket(),
        validMgmtPacket(),
    };
    // Last byte of each logical packet (the final CRC byte); bytes beyond
    // are radio padding, which is not CRC-protected.
    const uint8_t logical_lens[] = {
        PACKET_MAX_LEN,
        V3_ACK_LEN,
        V3_CONTROL_LEN,
        static_cast<uint8_t>(V3_MGMT_HDR_LEN + 4u + V3_CRC_LEN),
    };

    for (size_t i = 0; i < sizeof(packets) / sizeof(packets[0]); ++i) {
        packets[i].data[logical_lens[i] - 1u] ^= 0x40u;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::BAD_CRC),
                                static_cast<uint8_t>(parseByType(packets[i])));
    }
}

void test_seeded_parser_fuzzer_100000_inputs() {
    Lcg rng(0xF00D2026u);
    FuzzSink sink;
    memset(&sink, 0, sizeof(sink));
    ArqEngine engine;
    initEngine(engine, sink);

    uint32_t parsed_inputs = 0;
    uint32_t rejected_inputs = 0;
    uint32_t engine_malformed_before = engine.counters().malformed_input;

    while (parsed_inputs < 100000u) {
        Packet packet;
        memset(&packet, 0, sizeof(packet));

        const uint8_t mode = static_cast<uint8_t>(rng.next() % 4u);
        if (mode == 0) {
            packet.len = static_cast<uint8_t>(rng.next() % (PACKET_MAX_LEN + 1u));
            for (uint8_t i = 0; i < packet.len; ++i) {
                packet.data[i] = rng.byte();
            }
        } else if (mode == 1) {
            packet = validDataPacket(1280);
            packet.len = static_cast<uint8_t>(rng.next() % packet.len);
        } else if (mode == 2) {
            Packet choices[] = {
                validDataPacket(),
                validAckPacket(),
                validControlPacket(),
                validMgmtPacket(),
            };
            const uint8_t logical_lens[] = {
                PACKET_MAX_LEN,
                V3_ACK_LEN,
                V3_CONTROL_LEN,
                static_cast<uint8_t>(V3_MGMT_HDR_LEN + 4u + V3_CRC_LEN),
            };
            const uint32_t pick = rng.next() % 4u;
            packet = choices[pick];
            // Corrupt the final CRC byte of the logical packet (padding is
            // not CRC-protected).
            packet.data[logical_lens[pick] - 1u] ^=
                static_cast<uint8_t>(1u + (rng.byte() & 0x7Fu));
        } else {
            packet = validDataPacket(1280);
            packet.data[static_cast<uint8_t>(rng.next() % packet.len)] ^= rng.byte();
        }

        const ParseResult expected = parseByType(packet);
        parsed_inputs++;
        if (expected != ParseResult::OK) {
            rejected_inputs++;
            const uint32_t before = engine.counters().malformed_input;
            engine.onRxPacket(packet, parsed_inputs);
            TEST_ASSERT_EQUAL_UINT32(before + 1u, engine.counters().malformed_input);
        }
    }

    TEST_ASSERT_EQUAL_UINT32(100000u, parsed_inputs);
    TEST_ASSERT_GREATER_THAN_UINT32(90000u, rejected_inputs);
    TEST_ASSERT_EQUAL_UINT32(rejected_inputs,
                             engine.counters().malformed_input - engine_malformed_before);
}

void test_engine_duplicate_and_reordered_fragment_fuzz() {
    uint8_t datagram[V3_MAX_DATAGRAM];
    for (uint16_t i = 0; i < sizeof(datagram); ++i) {
        datagram[i] = static_cast<uint8_t>((i * 29u + 7u) & 0xFFu);
    }

    FragmentDescriptor fragments[V3_MAX_FRAGS];
    uint8_t fragment_count = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                            static_cast<uint8_t>(describeDatagram(datagram,
                                                                  sizeof(datagram),
                                                                  0x4444,
                                                                  0,
                                                                  0,
                                                                  fragments,
                                                                  V3_MAX_FRAGS,
                                                                  fragment_count)));

    FuzzSink sink;
    memset(&sink, 0, sizeof(sink));
    ArqEngine engine;
    initEngine(engine, sink);

    const uint8_t order[] = {5, 0, 11, 3, 3, 8, 1, 7, 2, 10, 4, 6, 9, 11, 0};
    for (size_t i = 0; i < sizeof(order); ++i) {
        Packet packet;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ParseResult::OK),
                                static_cast<uint8_t>(buildDataPacket(packet,
                                                                     fragments[order[i]].header,
                                                                     fragments[order[i]].payload)));
        engine.onRxPacket(packet, static_cast<uint32_t>(i));
        engine.onTick(static_cast<uint32_t>(i));
    }

    for (uint8_t i = 0; i < 20; ++i) {
        engine.onTick(100u + i);
    }

    TEST_ASSERT_EQUAL_UINT32(1, sink.delivered);
    TEST_ASSERT_EQUAL_UINT32(0, sink.duplicates);
    TEST_ASSERT_GREATER_THAN_UINT32(0, engine.counters().duplicate_suppressed);
    TEST_ASSERT_EQUAL_UINT32(1, engine.counters().delivered);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_crafted_parser_rejection_vectors);
    RUN_TEST(test_bad_crc_is_never_accepted_for_each_packet_type);
    RUN_TEST(test_seeded_parser_fuzzer_100000_inputs);
    RUN_TEST(test_engine_duplicate_and_reordered_fragment_fuzz);
    return UNITY_END();
}
