#include <unity.h>
#include <string.h>

#define PACKET_MAX_LEN 127
#include "../../src/framing/FragmentV3.h"

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

    uint16_t range(uint16_t limit) {
        return static_cast<uint16_t>(next() % limit);
    }
};

static void fillDatagram(uint8_t* data, uint16_t len, uint32_t seed) {
    Lcg rng(seed);
    for (uint16_t i = 0; i < len; ++i) {
        data[i] = static_cast<uint8_t>((rng.next() >> 24) ^ i);
    }
}

static void shuffle(uint8_t* order, uint8_t count, Lcg& rng) {
    for (uint8_t i = static_cast<uint8_t>(count - 1u); i > 0; --i) {
        const uint8_t j = static_cast<uint8_t>(rng.range(static_cast<uint16_t>(i + 1u)));
        const uint8_t tmp = order[i];
        order[i] = order[j];
        order[j] = tmp;
    }
}

void test_describe_datagram_edges() {
    uint8_t datagram[V3_MAX_DATAGRAM];
    FragmentDescriptor fragments[V3_MAX_FRAGS];
    uint8_t count = 0;

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::BAD_LENGTH),
                            static_cast<uint8_t>(describeDatagram(datagram,
                                                                  0,
                                                                  0x1001,
                                                                  0,
                                                                  0,
                                                                  fragments,
                                                                  V3_MAX_FRAGS,
                                                                  count)));
    TEST_ASSERT_EQUAL_UINT8(0, count);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::BAD_LENGTH),
                            static_cast<uint8_t>(describeDatagram(datagram,
                                                                  V3_MAX_DATAGRAM + 1u,
                                                                  0x1001,
                                                                  0,
                                                                  0,
                                                                  fragments,
                                                                  V3_MAX_FRAGS,
                                                                  count)));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OUTPUT_TOO_SMALL),
                            static_cast<uint8_t>(describeDatagram(datagram,
                                                                  1280,
                                                                  0x1001,
                                                                  0,
                                                                  0,
                                                                  fragments,
                                                                  11,
                                                                  count)));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                            static_cast<uint8_t>(describeDatagram(datagram,
                                                                  1280,
                                                                  0x1001,
                                                                  0x0F,
                                                                  0x0E,
                                                                  fragments,
                                                                  V3_MAX_FRAGS,
                                                                  count)));
    TEST_ASSERT_EQUAL_UINT8(12, count);
    TEST_ASSERT_EQUAL_UINT8(115, fragments[0].header.payload_len);
    TEST_ASSERT_EQUAL_UINT8(15, fragments[11].header.payload_len);
    TEST_ASSERT_EQUAL_PTR(datagram, fragments[0].payload);
    TEST_ASSERT_EQUAL_PTR(datagram + 1265, fragments[11].payload);
}

void test_reassembly_rejects_mismatched_metadata_and_counts_it() {
    uint8_t datagram[256];
    uint8_t output[256];
    fillDatagram(datagram, sizeof(datagram), 0xA5A5u);

    FragmentDescriptor fragments[V3_MAX_FRAGS];
    uint8_t count = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                            static_cast<uint8_t>(describeDatagram(datagram,
                                                                  sizeof(datagram),
                                                                  0x2222,
                                                                  0,
                                                                  0,
                                                                  fragments,
                                                                  V3_MAX_FRAGS,
                                                                  count)));

    ReassemblyV3 state;
    resetReassembly(state, output, sizeof(output));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                            static_cast<uint8_t>(acceptFragment(state,
                                                               fragments[0].header,
                                                               fragments[0].payload)));

    DataHeader mismatched = fragments[0].header;
    mismatched.datagram_id++;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::METADATA_MISMATCH),
                            static_cast<uint8_t>(acceptFragment(state,
                                                               mismatched,
                                                               fragments[0].payload)));
    TEST_ASSERT_EQUAL_UINT32(1, state.metadata_mismatches);
    TEST_ASSERT_EQUAL_UINT32(1, state.rejected_fragments);
}

void test_reassembly_duplicate_same_metadata_is_not_redelivered() {
    uint8_t datagram[32];
    uint8_t output[32];
    fillDatagram(datagram, sizeof(datagram), 0x3333u);

    FragmentDescriptor fragments[V3_MAX_FRAGS];
    uint8_t count = 0;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                            static_cast<uint8_t>(describeDatagram(datagram,
                                                                  sizeof(datagram),
                                                                  0x3333,
                                                                  0,
                                                                  0,
                                                                  fragments,
                                                                  V3_MAX_FRAGS,
                                                                  count)));
    TEST_ASSERT_EQUAL_UINT8(1, count);

    ReassemblyV3 state;
    resetReassembly(state, output, sizeof(output));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                            static_cast<uint8_t>(acceptFragment(state,
                                                               fragments[0].header,
                                                               fragments[0].payload)));
    TEST_ASSERT_TRUE(reassemblyIsComplete(state));

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::DUPLICATE),
                            static_cast<uint8_t>(acceptFragment(state,
                                                               fragments[0].header,
                                                               fragments[0].payload)));
    TEST_ASSERT_EQUAL_UINT32(1, state.duplicate_fragments);
    TEST_ASSERT_EQUAL_UINT32(0, state.rejected_fragments);
    TEST_ASSERT_EQUAL_MEMORY(datagram, output, sizeof(datagram));
}

void test_randomized_reassembly_property_10000_iterations() {
    uint8_t datagram[V3_MAX_DATAGRAM];
    uint8_t output[V3_MAX_DATAGRAM];
    FragmentDescriptor fragments[V3_MAX_FRAGS];
    uint8_t order[V3_MAX_FRAGS];

    for (uint16_t iteration = 0; iteration < 10000; ++iteration) {
        Lcg rng(static_cast<uint32_t>(0xC001D00Du + iteration * 97u));
        const uint16_t len = static_cast<uint16_t>(1u + rng.range(V3_MAX_DATAGRAM));
        fillDatagram(datagram, len, rng.next());

        uint8_t count = 0;
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                                static_cast<uint8_t>(describeDatagram(datagram,
                                                                      len,
                                                                      iteration,
                                                                      static_cast<uint8_t>(rng.range(16)),
                                                                      static_cast<uint8_t>(rng.range(16)),
                                                                      fragments,
                                                                      V3_MAX_FRAGS,
                                                                      count)));
        for (uint8_t i = 0; i < count; ++i) {
            order[i] = i;
        }
        shuffle(order, count, rng);

        memset(output, 0, sizeof(output));
        ReassemblyV3 state;
        resetReassembly(state, output, sizeof(output));

        for (uint8_t i = 0; i < count; ++i) {
            const uint8_t idx = order[i];
            if ((rng.next() & 0x03u) == 0) {
                TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                                        static_cast<uint8_t>(acceptFragment(state,
                                                                           fragments[idx].header,
                                                                           fragments[idx].payload)));
                TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::DUPLICATE),
                                        static_cast<uint8_t>(acceptFragment(state,
                                                                           fragments[idx].header,
                                                                           fragments[idx].payload)));
            } else {
                TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                                        static_cast<uint8_t>(acceptFragment(state,
                                                                           fragments[idx].header,
                                                                           fragments[idx].payload)));
            }
        }

        TEST_ASSERT_TRUE(reassemblyIsComplete(state));
        TEST_ASSERT_EQUAL_UINT16(len, reassembledLength(state));
        TEST_ASSERT_EQUAL_UINT16(fragmentExpectedBitmap(count), state.received_bitmap);
        TEST_ASSERT_EQUAL_MEMORY(datagram, output, len);

        const uint8_t duplicate_idx = static_cast<uint8_t>(rng.range(count));
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::DUPLICATE),
                                static_cast<uint8_t>(acceptFragment(state,
                                                                   fragments[duplicate_idx].header,
                                                                   fragments[duplicate_idx].payload)));
    }
}

void test_bitmap_converges_for_every_four_fragment_permutation() {
    uint8_t datagram[400];
    uint8_t output[sizeof(datagram)];
    FragmentDescriptor fragments[V3_MAX_FRAGS];
    uint8_t count = 0;
    fillDatagram(datagram, sizeof(datagram), 0x4444u);

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                            static_cast<uint8_t>(describeDatagram(datagram,
                                                                  sizeof(datagram),
                                                                  0x4444,
                                                                  0,
                                                                  0,
                                                                  fragments,
                                                                  V3_MAX_FRAGS,
                                                                  count)));
    TEST_ASSERT_EQUAL_UINT8(4, count);

    for (uint8_t a = 0; a < count; ++a) {
        for (uint8_t b = 0; b < count; ++b) {
            if (b == a) { continue; }
            for (uint8_t c = 0; c < count; ++c) {
                if (c == a || c == b) { continue; }
                for (uint8_t d = 0; d < count; ++d) {
                    if (d == a || d == b || d == c) { continue; }
                    const uint8_t order[4] = {a, b, c, d};

                    memset(output, 0, sizeof(output));
                    ReassemblyV3 state;
                    resetReassembly(state, output, sizeof(output));
                    for (uint8_t i = 0; i < count; ++i) {
                        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FragmentResult::OK),
                                                static_cast<uint8_t>(acceptFragment(
                                                    state,
                                                    fragments[order[i]].header,
                                                    fragments[order[i]].payload)));
                    }

                    TEST_ASSERT_TRUE(reassemblyIsComplete(state));
                    TEST_ASSERT_EQUAL_UINT16(0x000Fu, state.received_bitmap);
                    TEST_ASSERT_EQUAL_MEMORY(datagram, output, sizeof(datagram));
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_describe_datagram_edges);
    RUN_TEST(test_reassembly_rejects_mismatched_metadata_and_counts_it);
    RUN_TEST(test_reassembly_duplicate_same_metadata_is_not_redelivered);
    RUN_TEST(test_randomized_reassembly_property_10000_iterations);
    RUN_TEST(test_bitmap_converges_for_every_four_fragment_permutation);
    return UNITY_END();
}
