#include <unity.h>
#include <string.h>

#define PACKET_MAX_LEN 127
#include "../../src/arq/DatagramPool.h"

using namespace arq;

void setUp() {}
void tearDown() {}

static DuplicateAckState makeAck(uint16_t bitmap,
                                 uint8_t credits,
                                 framing_v3::FailureStatus failure) {
    DuplicateAckState ack;
    ack.fragment_bitmap = bitmap;
    ack.receiver_credits = credits;
    ack.failure = failure;
    return ack;
}

void test_pool_exhaustion_returns_failure_without_blocking() {
    DatagramPool pool;
    pool.reset();

    DatagramLease leases[DATAGRAM_POOL_CAPACITY];
    for (uint8_t i = 0; i < DATAGRAM_POOL_CAPACITY; ++i) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::OK),
                                static_cast<uint8_t>(acquireDatagram(pool, leases[i])));
        TEST_ASSERT_TRUE(leases[i].handle.isValid());
        TEST_ASSERT_NOT_NULL(leases[i].data);
        TEST_ASSERT_EQUAL_UINT16(framing_v3::V3_MAX_DATAGRAM, leases[i].capacity);
        leases[i].data[0] = i;
    }

    TEST_ASSERT_EQUAL_UINT8(0, poolFreeCount(pool));
    TEST_ASSERT_EQUAL_UINT8(DATAGRAM_POOL_CAPACITY, pool.used_count);

    DatagramLease extra;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::EXHAUSTED),
                            static_cast<uint8_t>(acquireDatagram(pool, extra)));
    TEST_ASSERT_FALSE(extra.handle.isValid());
    TEST_ASSERT_NULL(extra.data);
    TEST_ASSERT_EQUAL_UINT16(0, extra.capacity);
    TEST_ASSERT_EQUAL_UINT32(1, pool.allocation_failures);
}

void test_pool_release_and_reacquire_cycles_same_slot() {
    DatagramPool pool;
    pool.reset();

    DatagramLease first;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::OK),
                            static_cast<uint8_t>(acquireDatagram(pool, first)));
    const uint8_t first_index = first.handle.index;
    first.data[0] = 0xA5;

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::OK),
                            static_cast<uint8_t>(releaseDatagram(pool, first.handle)));
    TEST_ASSERT_EQUAL_UINT8(0, pool.used_count);
    TEST_ASSERT_EQUAL_UINT8(DATAGRAM_POOL_CAPACITY, poolFreeCount(pool));

    DatagramLease second;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::OK),
                            static_cast<uint8_t>(acquireDatagram(pool, second)));
    TEST_ASSERT_EQUAL_UINT8(first_index, second.handle.index);
    TEST_ASSERT_EQUAL_PTR(first.data, second.data);
    TEST_ASSERT_EQUAL_HEX8(0xA5, second.data[0]);
}

void test_pool_rejects_invalid_and_double_release() {
    DatagramPool pool;
    pool.reset();

    DatagramHandle invalid;
    invalid.index = 99;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::INVALID_HANDLE),
                            static_cast<uint8_t>(releaseDatagram(pool, invalid)));
    TEST_ASSERT_EQUAL_UINT32(1, pool.invalid_releases);

    DatagramLease lease;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::OK),
                            static_cast<uint8_t>(acquireDatagram(pool, lease)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::OK),
                            static_cast<uint8_t>(releaseDatagram(pool, lease.handle)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(PoolResult::DOUBLE_RELEASE),
                            static_cast<uint8_t>(releaseDatagram(pool, lease.handle)));
    TEST_ASSERT_EQUAL_UINT32(2, pool.invalid_releases);
}

void test_duplicate_window_hit_miss_and_ack_metadata() {
    DuplicateWindow window;
    resetDuplicateWindow(window);

    DuplicateAckState ack = makeAck(0x0FFFu, 3, framing_v3::FailureStatus::NONE);
    duplicateWindowStore(window, 0x1234, ack);

    DuplicateAckState found;
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 0x1234, &found));
    TEST_ASSERT_EQUAL_HEX16(ack.fragment_bitmap, found.fragment_bitmap);
    TEST_ASSERT_EQUAL_UINT8(ack.receiver_credits, found.receiver_credits);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ack.failure),
                            static_cast<uint8_t>(found.failure));
    TEST_ASSERT_FALSE(duplicateWindowFind(window, 0x1235));

    DuplicateAckState updated =
        makeAck(0x0001u, 0, framing_v3::FailureStatus::CREDIT_WITHDRAWAL);
    duplicateWindowStore(window, 0x1234, updated);
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 0x1234, &found));
    TEST_ASSERT_EQUAL_HEX16(updated.fragment_bitmap, found.fragment_bitmap);
    TEST_ASSERT_EQUAL_UINT8(updated.receiver_credits, found.receiver_credits);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(updated.failure),
                            static_cast<uint8_t>(found.failure));
    TEST_ASSERT_EQUAL_UINT8(1, window.count);
}

void test_duplicate_window_wraparound_fffe_to_0001() {
    DuplicateWindow window;
    resetDuplicateWindow(window);

    duplicateWindowStore(window, 0xFFFE, makeAck(0x0001u, 1, framing_v3::FailureStatus::NONE));
    duplicateWindowStore(window, 0xFFFF, makeAck(0x0003u, 2, framing_v3::FailureStatus::NONE));
    duplicateWindowStore(window, 0x0000, makeAck(0x0007u, 3, framing_v3::FailureStatus::NONE));
    duplicateWindowStore(window, 0x0001, makeAck(0x000Fu, 4, framing_v3::FailureStatus::NONE));

    DuplicateAckState found;
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 0xFFFE, &found));
    TEST_ASSERT_EQUAL_HEX16(0x0001u, found.fragment_bitmap);
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 0xFFFF));
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 0x0000));
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 0x0001, &found));
    TEST_ASSERT_EQUAL_HEX16(0x000Fu, found.fragment_bitmap);
    TEST_ASSERT_EQUAL_UINT8(4, found.receiver_credits);
    TEST_ASSERT_EQUAL_UINT16(0x0001u, window.newest_id);
}

void test_duplicate_window_keeps_64_recent_ids_across_wrap() {
    DuplicateWindow window;
    resetDuplicateWindow(window);

    for (uint16_t i = 0; i < 70; ++i) {
        const uint16_t id = static_cast<uint16_t>(0xFFFEu + i);
        duplicateWindowStore(window,
                             id,
                             makeAck(i, static_cast<uint8_t>(i & 0x0Fu),
                                     framing_v3::FailureStatus::NONE));
    }

    TEST_ASSERT_EQUAL_UINT8(DUPLICATE_WINDOW_CAPACITY, window.count);
    TEST_ASSERT_EQUAL_UINT16(0x0043u, window.newest_id);
    TEST_ASSERT_FALSE(duplicateWindowFind(window, 0xFFFE));
    TEST_ASSERT_FALSE(duplicateWindowFind(window, 0xFFFF));
    TEST_ASSERT_FALSE(duplicateWindowFind(window, 0x0000));
    TEST_ASSERT_FALSE(duplicateWindowFind(window, 0x0003));
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 0x0004));
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 0x0043));
}

void test_duplicate_window_late_old_id_does_not_advance_newest() {
    DuplicateWindow window;
    resetDuplicateWindow(window);

    duplicateWindowStore(window, 10, makeAck(0x0001u, 1, framing_v3::FailureStatus::NONE));
    duplicateWindowStore(window, 9, makeAck(0x0001u, 1, framing_v3::FailureStatus::NONE));

    TEST_ASSERT_EQUAL_UINT16(10, window.newest_id);
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 9));
    TEST_ASSERT_TRUE(duplicateWindowFind(window, 10));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_pool_exhaustion_returns_failure_without_blocking);
    RUN_TEST(test_pool_release_and_reacquire_cycles_same_slot);
    RUN_TEST(test_pool_rejects_invalid_and_double_release);
    RUN_TEST(test_duplicate_window_hit_miss_and_ack_metadata);
    RUN_TEST(test_duplicate_window_wraparound_fffe_to_0001);
    RUN_TEST(test_duplicate_window_keeps_64_recent_ids_across_wrap);
    RUN_TEST(test_duplicate_window_late_old_id_does_not_advance_newest);
    return UNITY_END();
}
