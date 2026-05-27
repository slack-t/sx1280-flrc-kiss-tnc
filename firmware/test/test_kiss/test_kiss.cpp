#include <unity.h>
#include <string.h>

// Pull in KISS implementation directly (no hardware deps)
#define PACKET_MAX_LEN 127
#include "../../src/kiss/Kiss.h"
#include "../../src/kiss/Kiss.cpp"

void setUp() {}
void tearDown() {}

// ── T1.1: round-trip encode→decode for every byte value ──────────────────────
void test_roundtrip_all_bytes() {
    for (int val = 0; val <= 255; val++) {
        IpFrame original;
        original.len = 3;
        original.data[0] = 0xAA;
        original.data[1] = static_cast<uint8_t>(val);
        original.data[2] = 0x55;

        uint8_t encBuf[IP_MTU * 2 + 3];
        size_t  encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

        Kiss    decoder;
        IpFrame decoded;
        bool    complete = false;
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

// ── T1.2: FEND (0xC0) in payload is escaped correctly ────────────────────────
void test_escape_fend_in_payload() {
    IpFrame pkt;
    pkt.len     = 1;
    pkt.data[0] = KISS_FEND;

    uint8_t encBuf[32];
    size_t  encLen = Kiss::encode(pkt, encBuf, sizeof(encBuf));

    // Expected: FEND  port  FESC  TFEND  FEND
    TEST_ASSERT_EQUAL_UINT(5, encLen);
    TEST_ASSERT_EQUAL_HEX8(KISS_FEND,  encBuf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00,       encBuf[1]);  // port byte
    TEST_ASSERT_EQUAL_HEX8(KISS_FESC,  encBuf[2]);
    TEST_ASSERT_EQUAL_HEX8(KISS_TFEND, encBuf[3]);
    TEST_ASSERT_EQUAL_HEX8(KISS_FEND,  encBuf[4]);
}

// ── T1.3: FESC (0xDB) in payload is escaped correctly ────────────────────────
void test_escape_fesc_in_payload() {
    IpFrame pkt;
    pkt.len     = 1;
    pkt.data[0] = KISS_FESC;

    uint8_t encBuf[32];
    size_t  encLen = Kiss::encode(pkt, encBuf, sizeof(encBuf));

    // Expected: FEND  port  FESC  TFESC  FEND
    TEST_ASSERT_EQUAL_UINT(5, encLen);
    TEST_ASSERT_EQUAL_HEX8(KISS_FESC,  encBuf[2]);
    TEST_ASSERT_EQUAL_HEX8(KISS_TFESC, encBuf[3]);
}

// ── T1.4: decoder handles frame split across multiple calls ───────────────────
void test_split_frame_delivery() {
    IpFrame original;
    original.len    = 4;
    original.data[0] = 0x01;
    original.data[1] = 0x02;
    original.data[2] = 0x03;
    original.data[3] = 0x04;

    uint8_t encBuf[IP_MTU * 2 + 3];
    size_t  encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

    Kiss    decoder;
    IpFrame decoded;
    int     completeCount = 0;

    // Feed one byte at a time
    for (size_t i = 0; i < encLen; i++) {
        bool done = decoder.decode(encBuf[i], decoded);
        if (done) completeCount++;
        if (i < encLen - 1) {
            TEST_ASSERT_FALSE_MESSAGE(done, "Completed too early");
        }
    }

    TEST_ASSERT_EQUAL_INT(1, completeCount);
    TEST_ASSERT_EQUAL_UINT16(original.len, decoded.len);
    TEST_ASSERT_EQUAL_MEMORY(original.data, decoded.data, original.len);
}

// ── T1.5: oversized frame does not overflow buffer ───────────────────────────
void test_oversized_frame_no_overflow() {
    Kiss    decoder;
    IpFrame out;

    decoder.decode(KISS_FEND, out);  // start frame
    decoder.decode(0x00, out);       // port byte

    // Feed more bytes than IP_MTU
    for (int i = 0; i < IP_MTU + 20; i++) {
        bool done = decoder.decode(0xAA, out);
        TEST_ASSERT_FALSE(done);     // must never claim complete while overflowing
    }

    // Closing FEND must return false (overflow → discard)
    bool done = decoder.decode(KISS_FEND, out);
    TEST_ASSERT_FALSE(done);
}

// ── T1.6: non-zero port frames are silently dropped ───────────────────────────
void test_non_zero_port_dropped() {
    Kiss    decoder;
    IpFrame out;
    memset(out.data, 0, sizeof(out.data));
    out.len = 0;

    // Frame with port byte = 0x10 (port 1, set command)
    uint8_t frame[] = { KISS_FEND, 0x10, 0xDE, 0xAD, KISS_FEND };
    bool complete = false;
    for (uint8_t b : frame) {
        if (decoder.decode(b, out)) complete = true;
    }

    TEST_ASSERT_FALSE(complete);
    TEST_ASSERT_EQUAL_UINT16(0, out.len);
}

// ── T1.7: empty frame (back-to-back FEND) is ignored ─────────────────────────
void test_empty_frame_ignored() {
    Kiss    decoder;
    IpFrame out;
    out.len = 99;

    bool c1 = decoder.decode(KISS_FEND, out);
    bool c2 = decoder.decode(KISS_FEND, out);

    TEST_ASSERT_FALSE(c1);
    TEST_ASSERT_FALSE(c2);
}

// ── T1.8a: link-layer data header round-trips correctly ──────────────────────
void test_link_data_packet_roundtrip() {
    Packet pkt;
    uint8_t payload[FRAMING_FRAG_DATA];
    for (uint8_t i = 0; i < FRAMING_FRAG_DATA; i++) {
        payload[i] = static_cast<uint8_t>(i);
    }

    framingBuildDataPacket(pkt, 0x1234, 2, 4, true, payload, 31);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(LinkPacketType::DATA),
                            static_cast<uint8_t>(framingPacketType(pkt)));
    TEST_ASSERT_EQUAL_UINT16(0x1234, framingPacketSeq(pkt));
    TEST_ASSERT_EQUAL_UINT8(2, framingFragmentIndex(pkt));
    TEST_ASSERT_EQUAL_UINT8(4, framingTotalFrags(pkt));
    TEST_ASSERT_TRUE(framingIsRoundEnd(pkt));
    TEST_ASSERT_EQUAL_UINT8(31, framingPayloadLen(pkt));
    TEST_ASSERT_EQUAL_MEMORY(payload, pkt.data + FRAMING_DATA_HDR_LEN, 31);
}

// ── T1.8b: ACK packets preserve frame sequence and bitmap ────────────────────
void test_link_ack_packet_roundtrip() {
    Packet pkt;
    AckFrame ack;
    ack.seq           = 0xBEEF;
    ack.total_frags   = 4;
    ack.received_mask = 0x0B;

    framingBuildAckPacket(pkt, ack);

    AckFrame decoded;
    TEST_ASSERT_TRUE(framingParseAck(pkt, decoded));
    TEST_ASSERT_EQUAL_UINT16(ack.seq, decoded.seq);
    TEST_ASSERT_EQUAL_UINT8(ack.total_frags, decoded.total_frags);
    TEST_ASSERT_EQUAL_UINT8(ack.received_mask, decoded.received_mask);
}

// ── T1.8: back-to-back frames with standard double-FEND boundary ─────────────
void test_back_to_back_double_fend() {
    IpFrame a, b;
    a.len = 2; a.data[0] = 0x11; a.data[1] = 0x22;
    b.len = 2; b.data[0] = 0x33; b.data[1] = 0x44;

    uint8_t encA[32], encB[32];
    size_t  lenA = Kiss::encode(a, encA, sizeof(encA));
    size_t  lenB = Kiss::encode(b, encB, sizeof(encB));

    // Standard stream: FEND portA dataA FEND FEND portB dataB FEND
    Kiss    decoder;
    IpFrame out;
    int     count = 0;
    IpFrame results[2];

    for (size_t i = 0; i < lenA; i++) {
        if (decoder.decode(encA[i], out)) results[count++] = out;
    }
    for (size_t i = 0; i < lenB; i++) {
        if (decoder.decode(encB[i], out)) results[count++] = out;
    }

    TEST_ASSERT_EQUAL_INT(2, count);
    TEST_ASSERT_EQUAL_UINT16(a.len, results[0].len);
    TEST_ASSERT_EQUAL_MEMORY(a.data, results[0].data, a.len);
    TEST_ASSERT_EQUAL_UINT16(b.len, results[1].len);
    TEST_ASSERT_EQUAL_MEMORY(b.data, results[1].data, b.len);
}

// ── T1.10: invalid escape discards partial frame and requires fresh FEND ─────
void test_fend_inside_escape_discards_frame_and_resyncs() {
    // Build stream: FEND 0x00 0xAA FESC FEND FEND 0x00 0xBB FEND
    //   Frame 1: starts, gets port+0xAA, then FESC followed by FEND (invalid escape)
    //            -> partial frame discarded
    //   Frame 2: fresh FEND 0x00 0xBB FEND -> should decode cleanly
    uint8_t stream[] = {
        KISS_FEND, 0x00, 0xAA, KISS_FESC, KISS_FEND,   // invalid escape at FEND
        KISS_FEND, 0x00, 0xBB, KISS_FEND
    };

    Kiss    decoder;
    IpFrame out;
    int     count = 0;
    IpFrame results[2];

    for (uint8_t b : stream) {
        if (decoder.decode(b, out)) {
            if (count < 2) results[count] = out;
            count++;
        }
    }

    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_UINT16(1, results[0].len);
    TEST_ASSERT_EQUAL_HEX8(0xBB, results[0].data[0]);
}

// ── T1.11: non-zero port frame followed by valid data frame resyncs cleanly ───
void test_non_zero_port_then_valid_frame_resyncs_cleanly() {
    // Frame 1: port=0x10 (SET_TXDELAY), payload=0xDE 0xAD  -> discarded
    // Frame 2: port=0x00, payload=0x42  -> should decode
    uint8_t stream[] = {
        KISS_FEND, 0x10, 0xDE, 0xAD, KISS_FEND,
        KISS_FEND, 0x00, 0x42, KISS_FEND
    };

    Kiss    decoder;
    IpFrame out;
    int     count = 0;
    IpFrame results[2];

    for (uint8_t b : stream) {
        if (decoder.decode(b, out)) {
            if (count < 2) results[count] = out;
            count++;
        }
    }

    TEST_ASSERT_EQUAL_INT(1, count);
    TEST_ASSERT_EQUAL_UINT16(1, results[0].len);
    TEST_ASSERT_EQUAL_HEX8(0x42, results[0].data[0]);
}

// ── T1.12: oversized frame followed by valid frame resyncs cleanly ────────────
void test_oversized_frame_then_valid_frame_resyncs_cleanly() {
    Kiss    decoder;
    IpFrame out;

    decoder.decode(KISS_FEND, out);  // open frame
    decoder.decode(0x00, out);       // port byte

    // Overflow the buffer
    for (int i = 0; i < IP_MTU + 5; i++) {
        decoder.decode(0xAA, out);
    }

    // Closing FEND discards oversized frame and returns to IDLE
    bool done = decoder.decode(KISS_FEND, out);
    TEST_ASSERT_FALSE(done);

    // Now feed a valid small frame with a fresh FEND opener
    decoder.decode(KISS_FEND, out);
    decoder.decode(0x00, out);    // port byte
    decoder.decode(0x77, out);    // payload
    done = decoder.decode(KISS_FEND, out);

    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT16(1, out.len);
    TEST_ASSERT_EQUAL_HEX8(0x77, out.data[0]);
}

// ── T1.13: 369-byte payload decodes without truncation ───────────────────────
void test_369_byte_regression_reproducer() {
    IpFrame original;
    original.len = 369;
    for (uint16_t i = 0; i < original.len; i++) {
        original.data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint8_t encBuf[IP_MTU * 2 + 3];
    size_t  encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

    Kiss    decoder;
    IpFrame decoded;
    bool    complete = false;

    // Deliver in 64-byte chunks to simulate realistic serial chunking
    for (size_t offset = 0; offset < encLen && !complete; ) {
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

// ── T1.14: captured-noise prefix cannot truncate following 369-byte frame ────
void test_369_byte_frame_after_noise_prefix() {
    IpFrame original;
    original.len = 369;
    for (uint16_t i = 0; i < original.len; i++) {
        original.data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint8_t encBuf[IP_MTU * 2 + 3];
    size_t encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

    Kiss decoder;
    IpFrame decoded;
    bool complete = false;

    // Feed a prefix that previously confused host-side logging and could expose
    // decoder boundary bugs before the actual framed payload begins.
    const uint8_t noise_prefix[] = {
        0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A,
        0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32
    };
    for (uint8_t b : noise_prefix) {
        TEST_ASSERT_FALSE(decoder.decode(b, decoded));
    }

    // Deliver the valid frame in irregular chunks to exercise boundary handling.
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

// ── T1.9: large frame (IP_MTU bytes) round-trips through KISS encode/decode ──
void test_large_frame_roundtrip() {
    IpFrame original;
    original.len = IP_MTU;
    for (uint16_t i = 0; i < original.len; i++) {
        original.data[i] = static_cast<uint8_t>(i & 0xFF);
    }

    uint8_t encBuf[IP_MTU * 2 + 3];
    size_t  encLen = Kiss::encode(original, encBuf, sizeof(encBuf));

    Kiss    decoder;
    IpFrame decoded;
    bool    complete = false;
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
    RUN_TEST(test_link_ack_packet_roundtrip);
    RUN_TEST(test_back_to_back_double_fend);
    RUN_TEST(test_large_frame_roundtrip);
    RUN_TEST(test_fend_inside_escape_discards_frame_and_resyncs);
    RUN_TEST(test_non_zero_port_then_valid_frame_resyncs_cleanly);
    RUN_TEST(test_oversized_frame_then_valid_frame_resyncs_cleanly);
    RUN_TEST(test_369_byte_regression_reproducer);
    RUN_TEST(test_369_byte_frame_after_noise_prefix);
    return UNITY_END();
}
