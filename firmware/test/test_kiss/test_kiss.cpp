#include <unity.h>
#include <string.h>

// Pull in KISS/framing implementation directly (no hardware deps)
#define PACKET_MAX_LEN 127
#include "../../src/kiss/Kiss.h"
#include "../../src/kiss/Kiss.cpp"
#include "../../src/framing/Crc32.h"
#include "../../src/kiss/SerialIntegrity.h"

void setUp() {}
void tearDown() {}

void test_roundtrip_all_bytes() {
    for (int val = 0; val <= 255; val++) {
        PayloadFrame original;
        original.len = 3;
        original.data[0] = 0xAA;
        original.data[1] = static_cast<uint8_t>(val);
        original.data[2] = 0x55;

        uint8_t encBuf[TNC_PAYLOAD_MAX_LEN * 2 + 3];
        size_t encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

        Kiss decoder;
        PayloadFrame decoded;
        bool complete = false;
        for (size_t i = 0; i < encLen; i++) {
            if (decoder.decode(encBuf[i], decoded)) {
                complete = true;
                break;
            }
        }

        TEST_ASSERT_TRUE_MESSAGE(complete, "Frame not completed");
        TEST_ASSERT_EQUAL_UINT16(original.len, decoded.len);
        TEST_ASSERT_EQUAL_MEMORY(original.data, decoded.data, original.len);
    }
}

void test_escape_fend_in_payload() {
    PayloadFrame pkt;
    pkt.len = 1;
    pkt.data[0] = KISS_FEND;

    uint8_t encBuf[32];
    size_t encLen = Kiss::encode(pkt, encBuf, sizeof(encBuf));

    TEST_ASSERT_EQUAL_UINT(5, encLen);
    TEST_ASSERT_EQUAL_HEX8(KISS_FEND, encBuf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, encBuf[1]);
    TEST_ASSERT_EQUAL_HEX8(KISS_FESC, encBuf[2]);
    TEST_ASSERT_EQUAL_HEX8(KISS_TFEND, encBuf[3]);
    TEST_ASSERT_EQUAL_HEX8(KISS_FEND, encBuf[4]);
}

void test_escape_fesc_in_payload() {
    PayloadFrame pkt;
    pkt.len = 1;
    pkt.data[0] = KISS_FESC;

    uint8_t encBuf[32];
    size_t encLen = Kiss::encode(pkt, encBuf, sizeof(encBuf));

    TEST_ASSERT_EQUAL_UINT(5, encLen);
    TEST_ASSERT_EQUAL_HEX8(KISS_FESC, encBuf[2]);
    TEST_ASSERT_EQUAL_HEX8(KISS_TFESC, encBuf[3]);
}

void test_split_frame_delivery() {
    PayloadFrame original;
    original.len = 4;
    original.data[0] = 0x01;
    original.data[1] = 0x02;
    original.data[2] = 0x03;
    original.data[3] = 0x04;

    uint8_t encBuf[TNC_PAYLOAD_MAX_LEN * 2 + 3];
    size_t encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

    Kiss decoder;
    PayloadFrame decoded;
    int completeCount = 0;

    for (size_t i = 0; i < encLen; i++) {
        bool done = decoder.decode(encBuf[i], decoded);
        if (done) {
            completeCount++;
        }
        if (i < encLen - 1) {
            TEST_ASSERT_FALSE_MESSAGE(done, "Completed too early");
        }
    }

    TEST_ASSERT_EQUAL_INT(1, completeCount);
    TEST_ASSERT_EQUAL_UINT16(original.len, decoded.len);
    TEST_ASSERT_EQUAL_MEMORY(original.data, decoded.data, original.len);
}

void test_oversized_frame_no_overflow() {
    Kiss decoder;
    PayloadFrame out;

    decoder.decode(KISS_FEND, out);
    decoder.decode(0x00, out);

    for (int i = 0; i < TNC_PAYLOAD_MAX_LEN + 20; i++) {
        bool done = decoder.decode(0xAA, out);
        TEST_ASSERT_FALSE(done);
    }

    bool done = decoder.decode(KISS_FEND, out);
    TEST_ASSERT_FALSE(done);
}

void test_non_zero_port_dropped() {
    Kiss decoder;
    PayloadFrame out;
    memset(out.data, 0, sizeof(out.data));
    out.len = 0;

    uint8_t frame[] = { KISS_FEND, 0x10, 0xDE, 0xAD, KISS_FEND };
    bool complete = false;
    for (uint8_t b : frame) {
        if (decoder.decode(b, out)) {
            complete = true;
        }
    }

    TEST_ASSERT_FALSE(complete);
    TEST_ASSERT_EQUAL_UINT16(0, out.len);
}

void test_empty_frame_ignored() {
    Kiss decoder;
    PayloadFrame out;
    out.len = 99;

    bool c1 = decoder.decode(KISS_FEND, out);
    bool c2 = decoder.decode(KISS_FEND, out);

    TEST_ASSERT_FALSE(c1);
    TEST_ASSERT_FALSE(c2);
}

void test_link_data_packet_roundtrip() {
    Packet pkt;
    uint8_t payload[FRAMING_FRAG_DATA];
    for (uint8_t i = 0; i < FRAMING_FRAG_DATA; i++) {
        payload[i] = static_cast<uint8_t>(i);
    }

    framingBuildDataPacket(pkt, 0x1234, 2, 9, true, payload, 114, 943, 0xCAFEBABEu);

    DataFrameHeader decoded;
    TEST_ASSERT_TRUE(framingParseDataHeader(pkt, decoded));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LinkPacketType::DATA),
                            static_cast<uint8_t>(framingPacketType(pkt)));
    TEST_ASSERT_EQUAL_UINT16(0x1234, decoded.seq);
    TEST_ASSERT_EQUAL_UINT8(2, decoded.frag_index);
    TEST_ASSERT_EQUAL_UINT8(9, decoded.total_frags);
    TEST_ASSERT_TRUE(decoded.round_end);
    TEST_ASSERT_EQUAL_UINT8(114, decoded.payload_len);
    TEST_ASSERT_EQUAL_UINT16(943, decoded.frame_len);
    TEST_ASSERT_EQUAL_HEX32(0xCAFEBABEu, decoded.frame_crc32);
    TEST_ASSERT_EQUAL_MEMORY(payload, pkt.data + FRAMING_DATA_HDR_LEN, 114);
}

void test_framing_validation() {
    DataFrameHeader hdr;
    hdr.seq = 0x1111;
    hdr.total_frags = 9;
    hdr.frag_index = 8;
    hdr.payload_len = 31;
    hdr.frame_len = 943;
    hdr.frame_crc32 = 0;
    
    // Valid final
    TEST_ASSERT_TRUE(framingValidateDataFragment(hdr));
    
    // Valid non-final
    hdr.frag_index = 2;
    hdr.payload_len = 114;
    TEST_ASSERT_TRUE(framingValidateDataFragment(hdr));
    
    // Invalid non-final length
    hdr.payload_len = 31;
    TEST_ASSERT_FALSE(framingValidateDataFragment(hdr));
    
    // Invalid total frags
    hdr.payload_len = 114;
    hdr.total_frags = 8;
    TEST_ASSERT_FALSE(framingValidateDataFragment(hdr));
    
    // Invalid frame length bounds
    hdr.total_frags = 9;
    hdr.frame_len = 10000;
    TEST_ASSERT_FALSE(framingValidateDataFragment(hdr));
}


void test_link_ack_packet_roundtrip() {
    Packet pkt;
    AckFrame ack;
    ack.seq = 0xBEEF;
    ack.total_frags = 9;
    ack.window_base = 0;
    ack.received_mask = 0x0000010Bu;

    framingBuildAckPacket(pkt, ack);

    AckFrame decoded;
    TEST_ASSERT_TRUE(framingParseAck(pkt, decoded));
    TEST_ASSERT_EQUAL_UINT16(ack.seq, decoded.seq);
    TEST_ASSERT_EQUAL_UINT8(ack.total_frags, decoded.total_frags);
    TEST_ASSERT_EQUAL_UINT8(ack.window_base, decoded.window_base);
    TEST_ASSERT_EQUAL_HEX32(ack.received_mask, decoded.received_mask);
}

void test_back_to_back_double_fend() {
    PayloadFrame a;
    PayloadFrame b;
    a.len = 2;
    a.data[0] = 0x11;
    a.data[1] = 0x22;
    b.len = 2;
    b.data[0] = 0x33;
    b.data[1] = 0x44;

    uint8_t encA[32];
    uint8_t encB[32];
    size_t lenA = Kiss::encode(a, encA, sizeof(encA));
    size_t lenB = Kiss::encode(b, encB, sizeof(encB));

    Kiss decoder;
    PayloadFrame out;
    int count = 0;
    PayloadFrame results[2];

    for (size_t i = 0; i < lenA; i++) {
        if (decoder.decode(encA[i], out)) {
            results[count++] = out;
        }
    }
    for (size_t i = 0; i < lenB; i++) {
        if (decoder.decode(encB[i], out)) {
            results[count++] = out;
        }
    }

    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_UINT16(a.len, results[0].len);
    TEST_ASSERT_EQUAL_MEMORY(a.data, results[0].data, a.len);
    TEST_ASSERT_EQUAL_UINT16(b.len, results[1].len);
    TEST_ASSERT_EQUAL_MEMORY(b.data, results[1].data, b.len);
}

void test_fend_inside_escape_discards_frame_and_resyncs() {
    uint8_t stream[] = {
        KISS_FEND, 0x00, 0xAA, KISS_FESC, KISS_FEND,
        KISS_FEND, 0x00, 0xBB, KISS_FEND
    };

    Kiss decoder;
    PayloadFrame out;
    int count = 0;
    PayloadFrame results[2];

    for (uint8_t b : stream) {
        if (decoder.decode(b, out)) {
            if (count < 2) {
                results[count] = out;
            }
            count++;
        }
    }

    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_UINT16(1, results[0].len);
    TEST_ASSERT_EQUAL_HEX8(0xBB, results[0].data[0]);
}

void test_non_zero_port_then_valid_frame_resyncs_cleanly() {
    uint8_t stream[] = {
        KISS_FEND, 0x10, 0xDE, 0xAD, KISS_FEND,
        KISS_FEND, 0x00, 0x42, KISS_FEND
    };

    Kiss decoder;
    PayloadFrame out;
    int count = 0;
    PayloadFrame results[2];

    for (uint8_t b : stream) {
        if (decoder.decode(b, out)) {
            if (count < 2) {
                results[count] = out;
            }
            count++;
        }
    }

    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_UINT16(1, results[0].len);
    TEST_ASSERT_EQUAL_HEX8(0x42, results[0].data[0]);
}

void test_oversized_frame_then_valid_frame_resyncs_cleanly() {
    Kiss decoder;
    PayloadFrame out;

    decoder.decode(KISS_FEND, out);
    decoder.decode(0x00, out);

    for (int i = 0; i < TNC_PAYLOAD_MAX_LEN + 5; i++) {
        decoder.decode(0xAA, out);
    }

    bool done = decoder.decode(KISS_FEND, out);
    TEST_ASSERT_FALSE(done);

    decoder.decode(KISS_FEND, out);
    decoder.decode(0x00, out);
    decoder.decode(0x77, out);
    done = decoder.decode(KISS_FEND, out);

    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT16(1, out.len);
    TEST_ASSERT_EQUAL_HEX8(0x77, out.data[0]);
}

void test_369_byte_regression_reproducer() {
    PayloadFrame original;
    original.len = 369;
    for (uint16_t i = 0; i < original.len; i++) {
        original.data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint8_t encBuf[TNC_PAYLOAD_MAX_LEN * 2 + 3];
    size_t encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

    Kiss decoder;
    PayloadFrame decoded;
    bool complete = false;

    for (size_t offset = 0; offset < encLen && !complete;) {
        size_t chunk = (encLen - offset < 64) ? (encLen - offset) : 64;
        for (size_t i = 0; i < chunk && !complete; i++) {
            if (decoder.decode(encBuf[offset + i], decoded)) {
                complete = true;
            }
        }
        offset += chunk;
    }

    TEST_ASSERT_TRUE_MESSAGE(complete, "369-byte frame not completed");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(369, decoded.len, "369-byte frame truncated");
    TEST_ASSERT_EQUAL_MEMORY(original.data, decoded.data, original.len);
}

void test_369_byte_frame_after_noise_prefix() {
    PayloadFrame original;
    original.len = 369;
    for (uint16_t i = 0; i < original.len; i++) {
        original.data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint8_t encBuf[TNC_PAYLOAD_MAX_LEN * 2 + 3];
    size_t encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

    Kiss decoder;
    PayloadFrame decoded;
    bool complete = false;

    const uint8_t noise_prefix[] = {
        0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A,
        0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32
    };
    for (uint8_t b : noise_prefix) {
        TEST_ASSERT_FALSE(decoder.decode(b, decoded));
    }

    const size_t chunk_sizes[] = { 1, 7, 19, 3, 64, 11, 5, 128, 17, 256 };
    size_t offset = 0;
    size_t chunk_idx = 0;
    while (offset < encLen && !complete) {
        size_t chunk = chunk_sizes[chunk_idx % (sizeof(chunk_sizes) / sizeof(chunk_sizes[0]))];
        if (chunk > encLen - offset) {
            chunk = encLen - offset;
        }
        for (size_t i = 0; i < chunk && !complete; i++) {
            if (decoder.decode(encBuf[offset + i], decoded)) {
                complete = true;
            }
        }
        offset += chunk;
        chunk_idx++;
    }

    TEST_ASSERT_TRUE_MESSAGE(complete, "369-byte frame after noise not completed");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(369, decoded.len, "369-byte frame after noise truncated");
    TEST_ASSERT_EQUAL_MEMORY(original.data, decoded.data, original.len);
}

void test_large_frame_roundtrip() {
    PayloadFrame original;
    original.len = TNC_PAYLOAD_MAX_LEN;
    for (uint16_t i = 0; i < original.len; i++) {
        original.data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint8_t encBuf[TNC_PAYLOAD_MAX_LEN * 2 + 3];
    size_t encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

    Kiss decoder;
    PayloadFrame decoded;
    bool complete = false;
    for (size_t i = 0; i < encLen; i++) {
        if (decoder.decode(encBuf[i], decoded)) {
            complete = true;
            break;
        }
    }

    TEST_ASSERT_TRUE_MESSAGE(complete, "Large frame not completed");
    TEST_ASSERT_EQUAL_UINT16(original.len, decoded.len);
    TEST_ASSERT_EQUAL_MEMORY(original.data, decoded.data, original.len);
}

void test_native_packet_roundtrip() {
    uint8_t payload[32];
    for (uint8_t i = 0; i < sizeof(payload); i++) { payload[i] = i; }

    Packet pkt;
    framingBuildNativePacket(pkt, payload, sizeof(payload));

    TEST_ASSERT_EQUAL_UINT8(PACKET_MAX_LEN, pkt.len);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LinkPacketType::NATIVE),
                            static_cast<uint8_t>(framingPacketType(pkt)));

    uint8_t decoded_len = 0;
    TEST_ASSERT_TRUE(framingParseNativePayload(pkt, decoded_len));
    TEST_ASSERT_EQUAL_UINT8(sizeof(payload), decoded_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, pkt.data + FRAMING_NATIVE_HDR_LEN, sizeof(payload));
}

void test_native_packet_max_payload() {
    uint8_t payload[FRAMING_NATIVE_MAX_PAYLOAD];
    for (uint8_t i = 0; i < FRAMING_NATIVE_MAX_PAYLOAD; i++) { payload[i] = i; }

    Packet pkt;
    framingBuildNativePacket(pkt, payload, FRAMING_NATIVE_MAX_PAYLOAD);

    uint8_t decoded_len = 0;
    TEST_ASSERT_TRUE(framingParseNativePayload(pkt, decoded_len));
    TEST_ASSERT_EQUAL_UINT8(FRAMING_NATIVE_MAX_PAYLOAD, decoded_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, pkt.data + FRAMING_NATIVE_HDR_LEN, FRAMING_NATIVE_MAX_PAYLOAD);
}

void test_native_packet_oversize_rejected() {
    Packet pkt;
    pkt.data[0] = static_cast<uint8_t>((FRAMING_VERSION << FRAMING_VERSION_SHIFT) |
                                       static_cast<uint8_t>(LinkPacketType::NATIVE));
    pkt.data[1] = FRAMING_NATIVE_MAX_PAYLOAD + 1;  // oversize
    pkt.len = PACKET_MAX_LEN;

    uint8_t out = 0;
    TEST_ASSERT_FALSE(framingParseNativePayload(pkt, out));
}

void test_native_parse_rejects_data_packet() {
    Packet pkt;
    framingBuildDataPacket(pkt, 0x0001, 0, 1, true, nullptr, 0, 0, 0);

    uint8_t out = 0;
    TEST_ASSERT_FALSE(framingParseNativePayload(pkt, out));
}

void test_crc32() {
    // Empty payload
    uint8_t empty[] = {};
    TEST_ASSERT_EQUAL_HEX32(0x0, framing::computeCrc32(empty, 0));

    // Short payload
    uint8_t short_payload[] = "hello";
    TEST_ASSERT_EQUAL_HEX32(0x3610a686, framing::computeCrc32(short_payload, 5));

    // 1024-byte payload
    uint8_t large_payload[1024];
    memset(large_payload, 'a', 1024);
    TEST_ASSERT_EQUAL_HEX32(0x7c5597b9, framing::computeCrc32(large_payload, 1024));
}

void test_serial_integrity() {
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    uint32_t crc = framing::computeCrc32(payload, sizeof(payload));
    uint8_t wrapper[12];
    buildSerialIntegrityHeader(wrapper, sizeof(payload), crc);
    memcpy(wrapper + 8, payload, sizeof(payload));
    
    SerialIntegrityHeader hdr;
    TEST_ASSERT_TRUE(parseSerialIntegrityHeader(wrapper, sizeof(wrapper), hdr));
    TEST_ASSERT_EQUAL_HEX16(SERIAL_INTEGRITY_MAGIC, hdr.magic);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), hdr.payload_len);
    TEST_ASSERT_EQUAL_HEX32(crc, hdr.payload_crc32);
    
    // Corrupt length
    uint8_t bad_wrapper[12];
    memcpy(bad_wrapper, wrapper, sizeof(wrapper));
    bad_wrapper[3] ^= 0xFF;
    SerialIntegrityHeader bad_hdr;
    TEST_ASSERT_TRUE(parseSerialIntegrityHeader(bad_wrapper, sizeof(bad_wrapper), bad_hdr));
    TEST_ASSERT_NOT_EQUAL(sizeof(payload), bad_hdr.payload_len);
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip_all_bytes);
    RUN_TEST(test_escape_fend_in_payload);
    RUN_TEST(test_escape_fesc_in_payload);
    RUN_TEST(test_split_frame_delivery);
    RUN_TEST(test_oversized_frame_no_overflow);
    RUN_TEST(test_non_zero_port_dropped);
    RUN_TEST(test_empty_frame_ignored);
    RUN_TEST(test_link_data_packet_roundtrip);
    RUN_TEST(test_framing_validation);
    RUN_TEST(test_link_ack_packet_roundtrip);
    RUN_TEST(test_back_to_back_double_fend);
    RUN_TEST(test_large_frame_roundtrip);
    RUN_TEST(test_fend_inside_escape_discards_frame_and_resyncs);
    RUN_TEST(test_non_zero_port_then_valid_frame_resyncs_cleanly);
    RUN_TEST(test_oversized_frame_then_valid_frame_resyncs_cleanly);
    RUN_TEST(test_369_byte_regression_reproducer);
    RUN_TEST(test_369_byte_frame_after_noise_prefix);
    RUN_TEST(test_native_packet_roundtrip);
    RUN_TEST(test_native_packet_max_payload);
    RUN_TEST(test_native_packet_oversize_rejected);
    RUN_TEST(test_native_parse_rejects_data_packet);
    RUN_TEST(test_crc32);
    RUN_TEST(test_serial_integrity);
    return UNITY_END();
}
