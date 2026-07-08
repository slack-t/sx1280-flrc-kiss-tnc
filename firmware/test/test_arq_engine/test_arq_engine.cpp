#include <unity.h>
#include <string.h>

#define PACKET_MAX_LEN 127
#include "../../src/arq/ArqEngine.h"
#include "../../src/arq/ArqEngine.cpp"

using namespace arq;

void setUp() {}
void tearDown() {}

struct Lcg {
    uint32_t state;

    explicit Lcg(uint32_t seed) : state(seed) {}

    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state;
    }
};

struct Endpoint {
    ArqEngine engine;
    bool outgoing_valid = false;
    framing_v3::Packet outgoing;
    uint8_t egress_capacity = 32;
    uint16_t delivered = 0;
    uint16_t duplicates = 0;
    bool seen[256];
};

struct Sim {
    Endpoint a;
    Endpoint b;
    Lcg rng;
    uint32_t now = 0;
    uint16_t loss_permille = 0;
    bool ack_data_collision = true;

    explicit Sim(uint32_t seed) : rng(seed) {}
};

static uint16_t datagramTag(const uint8_t* data, uint16_t len) {
    if (len < 2) {
        return len > 0 ? data[0] : 0;
    }
    return static_cast<uint16_t>(data[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(data[1]) << 8);
}

static void fillDatagram(uint8_t* data, uint16_t len, uint16_t tag) {
    data[0] = static_cast<uint8_t>(tag & 0xFFu);
    if (len > 1) {
        data[1] = static_cast<uint8_t>(tag >> 8);
    }
    for (uint16_t i = 2; i < len; ++i) {
        data[i] = static_cast<uint8_t>((tag * 31u + i * 17u) & 0xFFu);
    }
}

static bool sendPacket(const framing_v3::Packet& packet, void* user) {
    Endpoint* endpoint = static_cast<Endpoint*>(user);
    if (endpoint->outgoing_valid) {
        return false;
    }
    endpoint->outgoing = packet;
    endpoint->outgoing_valid = true;
    return true;
}

static bool deliverDatagram(const uint8_t* data, uint16_t len, void* user) {
    Endpoint* endpoint = static_cast<Endpoint*>(user);
    if (endpoint->egress_capacity == 0) {
        return false;
    }
    const uint16_t tag = datagramTag(data, len);
    const uint8_t seen_index = static_cast<uint8_t>(tag & 0xFFu);
    if (endpoint->seen[seen_index]) {
        endpoint->duplicates++;
    } else {
        endpoint->seen[seen_index] = true;
        endpoint->delivered++;
    }
    return true;
}

static uint8_t egressCapacity(void* user) {
    Endpoint* endpoint = static_cast<Endpoint*>(user);
    return endpoint->egress_capacity;
}

static void resetEndpoint(Endpoint& endpoint,
                          uint16_t initial_datagram_id = 0,
                          uint8_t initial_remote_credits = ARQ_MAX_OUTSTANDING,
                          uint8_t max_attempts = 8,
                          uint32_t retry_timeout_cycles = 80) {
    memset(endpoint.seen, 0, sizeof(endpoint.seen));
    endpoint.outgoing_valid = false;
    endpoint.egress_capacity = 32;
    endpoint.delivered = 0;
    endpoint.duplicates = 0;

    ArqConfig config;
    config.retry_timeout_cycles = retry_timeout_cycles;
    config.ack_turnaround_cycles = 4;
    config.max_attempts = max_attempts;
    config.initial_remote_credits = initial_remote_credits;
    config.initial_datagram_id = initial_datagram_id;

    ArqCallbacks callbacks;
    callbacks.send_packet = sendPacket;
    callbacks.deliver_datagram = deliverDatagram;
    callbacks.egress_capacity = egressCapacity;
    callbacks.user = &endpoint;
    endpoint.engine.reset(config, callbacks);
}

static bool shouldDrop(Sim& sim) {
    if (sim.loss_permille == 0) {
        return false;
    }
    return (sim.rng.next() % 1000u) < sim.loss_permille;
}

static framing_v3::PacketType packetType(const framing_v3::Packet& packet) {
    framing_v3::PacketType type = framing_v3::PacketType::DATA;
    (void)framing_v3::parsePacketType(packet, type);
    return type;
}

static void deliverOne(Sim& sim, Endpoint& from, Endpoint& to) {
    if (!from.outgoing_valid) {
        return;
    }
    if (!shouldDrop(sim)) {
        to.engine.onRxPacket(from.outgoing, sim.now);
    }
}

static void simStep(Sim& sim) {
    sim.a.outgoing_valid = false;
    sim.b.outgoing_valid = false;

    sim.a.engine.onTick(sim.now);
    sim.b.engine.onTick(sim.now);

    if (sim.a.outgoing_valid && sim.b.outgoing_valid && sim.ack_data_collision) {
        const framing_v3::PacketType type_a = packetType(sim.a.outgoing);
        const framing_v3::PacketType type_b = packetType(sim.b.outgoing);
        if (type_a == framing_v3::PacketType::ACK &&
            type_b == framing_v3::PacketType::DATA) {
            deliverOne(sim, sim.b, sim.a);
        } else if (type_a == framing_v3::PacketType::DATA &&
                   type_b == framing_v3::PacketType::ACK) {
            deliverOne(sim, sim.a, sim.b);
        } else if ((sim.rng.next() & 1u) == 0) {
            deliverOne(sim, sim.a, sim.b);
        } else {
            deliverOne(sim, sim.b, sim.a);
        }
    } else {
        deliverOne(sim, sim.a, sim.b);
        deliverOne(sim, sim.b, sim.a);
    }

    sim.now++;
}

static void submitUntilFull(Endpoint& endpoint,
                            uint16_t& next_tag,
                            uint16_t end_tag,
                            uint16_t len) {
    uint8_t data[framing_v3::V3_MAX_DATAGRAM];
    while (next_tag < end_tag) {
        fillDatagram(data, len, next_tag);
        const ArqResult result = endpoint.engine.onTxDatagram(data, len);
        if (result != ArqResult::OK) {
            return;
        }
        next_tag++;
    }
}

static void runSim(Sim& sim, uint16_t max_cycles) {
    for (uint16_t i = 0; i < max_cycles; ++i) {
        simStep(sim);
    }
}

void test_convergence_under_30_percent_loss_bidirectional() {
    Sim sim(0xA110CA7Eu);
    resetEndpoint(sim.a);
    resetEndpoint(sim.b);
    sim.loss_permille = 300;

    uint16_t next_a = 0;
    uint16_t next_b = 64;
    const uint16_t end_a = 12;
    const uint16_t end_b = 76;

    for (uint16_t i = 0; i < 5000; ++i) {
        submitUntilFull(sim.a, next_a, end_a, 240);
        submitUntilFull(sim.b, next_b, end_b, 240);
        simStep(sim);
        if (next_a == end_a && next_b == end_b &&
            sim.a.delivered == 12 && sim.b.delivered == 12 &&
            !sim.a.engine.hasPendingWork() && !sim.b.engine.hasPendingWork()) {
            break;
        }
    }

    TEST_ASSERT_EQUAL_UINT16(12, sim.a.delivered);
    TEST_ASSERT_EQUAL_UINT16(12, sim.b.delivered);
    TEST_ASSERT_EQUAL_UINT16(0, sim.a.duplicates);
    TEST_ASSERT_EQUAL_UINT16(0, sim.b.duplicates);
    TEST_ASSERT_EQUAL_UINT32(0, sim.a.engine.counters().retry_exhaustion);
    TEST_ASSERT_EQUAL_UINT32(0, sim.b.engine.counters().retry_exhaustion);
}

void test_zero_duplication_across_duplicate_window_wraparound() {
    Sim sim(0xD00D1234u);
    resetEndpoint(sim.a, 0xFFFE);
    resetEndpoint(sim.b);
    sim.loss_permille = 120;

    uint16_t next = 0;
    const uint16_t end = 70;
    for (uint16_t i = 0; i < 8000; ++i) {
        submitUntilFull(sim.a, next, end, 32);
        simStep(sim);
        if (next == end && sim.b.delivered == end &&
            !sim.a.engine.hasPendingWork() && !sim.b.engine.hasPendingWork()) {
            break;
        }
    }

    TEST_ASSERT_EQUAL_UINT16(end, sim.b.delivered);
    TEST_ASSERT_EQUAL_UINT16(0, sim.b.duplicates);
    TEST_ASSERT_GREATER_THAN_UINT32(0, sim.b.engine.counters().duplicate_suppressed);
}

void test_credit_starvation_halts_new_data_but_not_retransmit_or_ack() {
    Sim sim(0xC0DEC0DEu);
    resetEndpoint(sim.a);
    resetEndpoint(sim.b);
    sim.b.egress_capacity = 0;

    uint16_t next = 0;
    submitUntilFull(sim.a, next, 4, 300);
    runSim(sim, 24);

    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.remoteCredits());
    TEST_ASSERT_GREATER_THAN_UINT32(0, sim.b.engine.counters().credit_withdrawal);
    TEST_ASSERT_GREATER_THAN_UINT8(0, sim.a.engine.txActiveCount());
    TEST_ASSERT_EQUAL_UINT16(0, sim.b.delivered);
}

void test_egress_blocked_recovers_without_acknowledged_loss() {
    Sim sim(0x12345678u);
    resetEndpoint(sim.a);
    resetEndpoint(sim.b);
    sim.b.egress_capacity = 0;

    uint16_t next = 0;
    submitUntilFull(sim.a, next, 1, 400);
    runSim(sim, 24);

    TEST_ASSERT_EQUAL_UINT16(0, sim.b.delivered);
    TEST_ASSERT_GREATER_THAN_UINT32(0, sim.b.engine.counters().credit_withdrawal);
    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.remoteCredits());
    TEST_ASSERT_GREATER_THAN_UINT8(0, sim.a.engine.txActiveCount());

    sim.b.egress_capacity = 32;
    runSim(sim, 300);

    TEST_ASSERT_EQUAL_UINT16(1, sim.b.delivered);
    TEST_ASSERT_EQUAL_UINT16(0, sim.b.duplicates);
    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.txActiveCount());
    TEST_ASSERT_EQUAL_UINT8(DATAGRAM_POOL_CAPACITY, sim.a.engine.txPoolFreeCount());
}

void test_retry_exhaustion_surfaces_failure_and_frees_buffer() {
    Sim sim(0x9999u);
    resetEndpoint(sim.a, 0, ARQ_MAX_OUTSTANDING, 3, 6);
    resetEndpoint(sim.b);
    sim.loss_permille = 1000;

    uint8_t data[64];
    fillDatagram(data, sizeof(data), 0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArqResult::OK),
                            static_cast<uint8_t>(sim.a.engine.onTxDatagram(data, sizeof(data))));
    runSim(sim, 100);

    TEST_ASSERT_EQUAL_UINT32(1, sim.a.engine.counters().retry_exhaustion);
    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.txActiveCount());
    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.txQueuedCount());
    TEST_ASSERT_EQUAL_UINT8(DATAGRAM_POOL_CAPACITY, sim.a.engine.txPoolFreeCount());
}

void test_partial_ack_is_deferred_until_round_end() {
    Endpoint a;
    Endpoint b;
    resetEndpoint(a);
    resetEndpoint(b);

    uint8_t data[300];
    fillDatagram(data, sizeof(data), 0x1234);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArqResult::OK),
                            static_cast<uint8_t>(a.engine.onTxDatagram(data, sizeof(data))));

    a.engine.onTick(0);
    TEST_ASSERT_TRUE(a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::DATA),
                            static_cast<uint8_t>(packetType(a.outgoing)));
    b.engine.onRxPacket(a.outgoing, 0);
    a.outgoing_valid = false;
    b.engine.onTick(0);
    TEST_ASSERT_FALSE(b.outgoing_valid);

    a.engine.onTick(1);
    TEST_ASSERT_TRUE(a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::DATA),
                            static_cast<uint8_t>(packetType(a.outgoing)));
    b.engine.onRxPacket(a.outgoing, 1);
    a.outgoing_valid = false;
    b.engine.onTick(1);
    TEST_ASSERT_FALSE(b.outgoing_valid);

    a.engine.onTick(2);
    TEST_ASSERT_TRUE(a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::DATA),
                            static_cast<uint8_t>(packetType(a.outgoing)));
    b.engine.onRxPacket(a.outgoing, 2);
    a.outgoing_valid = false;
    b.engine.onTick(2);
    TEST_ASSERT_FALSE(b.outgoing_valid);
    b.engine.onTick(6);
    TEST_ASSERT_TRUE(b.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::ACK),
                            static_cast<uint8_t>(packetType(b.outgoing)));
    TEST_ASSERT_EQUAL_UINT16(1, b.delivered);
}

void test_single_fragment_completion_ack_flushes_after_turnaround() {
    Endpoint a;
    Endpoint b;
    resetEndpoint(a);
    resetEndpoint(b);

    uint8_t data[64];
    fillDatagram(data, sizeof(data), 0x51A1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArqResult::OK),
                            static_cast<uint8_t>(a.engine.onTxDatagram(data, sizeof(data))));

    a.engine.onTick(0);
    TEST_ASSERT_TRUE(a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::DATA),
                            static_cast<uint8_t>(packetType(a.outgoing)));

    b.engine.onRxPacket(a.outgoing, 0);
    TEST_ASSERT_FALSE(b.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(1, b.engine.pendingAckCount());
    TEST_ASSERT_FALSE(b.engine.flushPendingAck(3));
    TEST_ASSERT_TRUE(b.engine.flushPendingAck(4));
    TEST_ASSERT_TRUE(b.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::ACK),
                            static_cast<uint8_t>(packetType(b.outgoing)));
    TEST_ASSERT_EQUAL_UINT16(1, b.delivered);

    a.outgoing_valid = false;
    a.engine.onRxPacket(b.outgoing, 0);
    TEST_ASSERT_EQUAL_UINT8(0, a.engine.txActiveCount());
    TEST_ASSERT_EQUAL_UINT32(1, a.engine.counters().tx_completed);
}

void test_30_by_1280_zero_gap_burst_delivers_all() {
    Sim sim(0xFEEDBEEFu);
    resetEndpoint(sim.a);
    resetEndpoint(sim.b);
    sim.loss_permille = 0;

    uint16_t next = 0;
    const uint16_t end = 30;
    for (uint16_t i = 0; i < 10000; ++i) {
        submitUntilFull(sim.a, next, end, framing_v3::V3_MAX_DATAGRAM);
        simStep(sim);
        if (next == end && sim.b.delivered == end &&
            !sim.a.engine.hasPendingWork() && !sim.b.engine.hasPendingWork()) {
            break;
        }
    }

    TEST_ASSERT_EQUAL_UINT16(end, sim.b.delivered);
    TEST_ASSERT_EQUAL_UINT16(0, sim.b.duplicates);
    TEST_ASSERT_EQUAL_UINT32(0, sim.a.engine.counters().retry_exhaustion);
    TEST_ASSERT_EQUAL_UINT32(0, sim.b.engine.counters().malformed_input);
}

// Regression for the credit-starvation wedge observed on hardware 2026-07-08:
// a completion ACK whose receiver_credits snapshot is zero completes the last
// OPEN slot, leaving only QUEUED slots. Nothing is in flight to elicit another
// ACK, so without the stall probe the engine reports no deadline and hangs
// forever.
void test_zero_credit_completion_ack_stall_recovers_via_probe() {
    Sim sim(0xC4ED17u);
    resetEndpoint(sim.a);

    uint8_t data[64];
    fillDatagram(data, sizeof(data), 0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArqResult::OK),
                            static_cast<uint8_t>(sim.a.engine.onTxDatagram(data, sizeof(data))));
    fillDatagram(data, sizeof(data), 1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArqResult::OK),
                            static_cast<uint8_t>(sim.a.engine.onTxDatagram(data, sizeof(data))));

    sim.a.outgoing_valid = false;
    sim.a.engine.onTick(sim.now);
    TEST_ASSERT_TRUE(sim.a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(1, sim.a.engine.txActiveCount());

    framing_v3::AckFrame ack;
    ack.datagram_id = 0;
    ack.fragment_bitmap = 0x0001;
    ack.receiver_credits = 0;
    ack.failure = framing_v3::FailureStatus::NONE;
    framing_v3::Packet ack_packet;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::ParseResult::OK),
                            static_cast<uint8_t>(framing_v3::buildAckPacket(ack_packet, ack)));
    sim.a.engine.onRxPacket(ack_packet, sim.now);

    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.txActiveCount());
    TEST_ASSERT_EQUAL_UINT8(1, sim.a.engine.txQueuedCount());
    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.remoteCredits());

    // First tick arms the probe; the engine must publish a wake-up deadline.
    sim.a.outgoing_valid = false;
    sim.a.engine.onTick(sim.now);
    TEST_ASSERT_FALSE(sim.a.outgoing_valid);
    uint32_t deadline = 0;
    TEST_ASSERT_TRUE(sim.a.engine.nextDeadline(sim.now, deadline));

    // Before the stall timeout: still silent.
    sim.now++;
    sim.a.engine.onTick(sim.now);
    TEST_ASSERT_FALSE(sim.a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT32(0, sim.a.engine.counters().credit_stall_probes);

    // At the deadline: the probe opens the queued datagram and transmits.
    sim.now = deadline;
    sim.a.engine.onTick(sim.now);
    TEST_ASSERT_TRUE(sim.a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::DATA),
                            static_cast<uint8_t>(packetType(sim.a.outgoing)));
    TEST_ASSERT_EQUAL_UINT8(1, sim.a.engine.txActiveCount());
    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.txQueuedCount());
    TEST_ASSERT_EQUAL_UINT32(1, sim.a.engine.counters().credit_stall_probes);
}

void test_credit_withdrawal_paces_retries_without_exhaustion() {
    Sim sim(0xBACC9E55u);
    resetEndpoint(sim.a, 0, 1, 3, 5);

    uint8_t data[64];
    fillDatagram(data, sizeof(data), 0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArqResult::OK),
                            static_cast<uint8_t>(sim.a.engine.onTxDatagram(data, sizeof(data))));

    sim.a.outgoing_valid = false;
    sim.a.engine.onTick(sim.now);
    TEST_ASSERT_TRUE(sim.a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(1, sim.a.engine.txActiveCount());

    framing_v3::AckFrame ack;
    ack.datagram_id = 0;
    ack.fragment_bitmap = 0;
    ack.receiver_credits = 0;
    ack.failure = framing_v3::FailureStatus::CREDIT_WITHDRAWAL;
    framing_v3::Packet ack_packet;

    for (uint8_t i = 0; i < 8; ++i) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::ParseResult::OK),
                                static_cast<uint8_t>(framing_v3::buildAckPacket(ack_packet, ack)));
        sim.a.engine.onRxPacket(ack_packet, sim.now);
        TEST_ASSERT_EQUAL_UINT8(1, sim.a.engine.txActiveCount());
        TEST_ASSERT_EQUAL_UINT32(0, sim.a.engine.counters().retry_exhaustion);

        sim.a.outgoing_valid = false;
        sim.a.engine.onTick(sim.now);
        TEST_ASSERT_FALSE(sim.a.outgoing_valid);

        uint32_t deadline = 0;
        TEST_ASSERT_TRUE(sim.a.engine.nextDeadline(sim.now, deadline));
        sim.now = deadline;
        sim.a.engine.onTick(sim.now);
        TEST_ASSERT_TRUE(sim.a.outgoing_valid);
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::DATA),
                                static_cast<uint8_t>(packetType(sim.a.outgoing)));
    }

    ack.fragment_bitmap = 0x0001;
    ack.receiver_credits = 1;
    ack.failure = framing_v3::FailureStatus::NONE;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::ParseResult::OK),
                            static_cast<uint8_t>(framing_v3::buildAckPacket(ack_packet, ack)));
    sim.a.engine.onRxPacket(ack_packet, sim.now);

    TEST_ASSERT_EQUAL_UINT8(0, sim.a.engine.txActiveCount());
    TEST_ASSERT_EQUAL_UINT32(1, sim.a.engine.counters().tx_completed);
    TEST_ASSERT_EQUAL_UINT32(0, sim.a.engine.counters().retry_exhaustion);
}

void test_credit_restore_via_any_ack_short_circuits_probe() {
    Sim sim(0x5EEDC0DEu);
    resetEndpoint(sim.a);

    uint8_t data[64];
    fillDatagram(data, sizeof(data), 0);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArqResult::OK),
                            static_cast<uint8_t>(sim.a.engine.onTxDatagram(data, sizeof(data))));
    fillDatagram(data, sizeof(data), 1);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ArqResult::OK),
                            static_cast<uint8_t>(sim.a.engine.onTxDatagram(data, sizeof(data))));

    sim.a.outgoing_valid = false;
    sim.a.engine.onTick(sim.now);
    TEST_ASSERT_TRUE(sim.a.outgoing_valid);

    framing_v3::AckFrame ack;
    ack.datagram_id = 0;
    ack.fragment_bitmap = 0x0001;
    ack.receiver_credits = 0;
    ack.failure = framing_v3::FailureStatus::NONE;
    framing_v3::Packet ack_packet;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::ParseResult::OK),
                            static_cast<uint8_t>(framing_v3::buildAckPacket(ack_packet, ack)));
    sim.a.engine.onRxPacket(ack_packet, sim.now);

    sim.a.outgoing_valid = false;
    sim.a.engine.onTick(sim.now);  // arms the probe
    TEST_ASSERT_FALSE(sim.a.outgoing_valid);

    // A later ACK (e.g. a duplicate-window re-ACK) restores credits; the
    // engine must resume immediately instead of waiting out the stall timer.
    ack.datagram_id = 999;
    ack.fragment_bitmap = 0;
    ack.receiver_credits = 2;
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::ParseResult::OK),
                            static_cast<uint8_t>(framing_v3::buildAckPacket(ack_packet, ack)));
    sim.a.engine.onRxPacket(ack_packet, sim.now);
    TEST_ASSERT_EQUAL_UINT8(2, sim.a.engine.remoteCredits());

    uint32_t deadline = 0;
    TEST_ASSERT_TRUE(sim.a.engine.nextDeadline(sim.now, deadline));
    TEST_ASSERT_EQUAL_UINT32(sim.now, deadline);

    sim.now++;
    sim.a.engine.onTick(sim.now);
    TEST_ASSERT_TRUE(sim.a.outgoing_valid);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(framing_v3::PacketType::DATA),
                            static_cast<uint8_t>(packetType(sim.a.outgoing)));
    TEST_ASSERT_EQUAL_UINT32(0, sim.a.engine.counters().credit_stall_probes);
    TEST_ASSERT_EQUAL_UINT8(1, sim.a.engine.remoteCredits());
}

int main(int argc, char** argv) {
    UNITY_BEGIN();
    RUN_TEST(test_convergence_under_30_percent_loss_bidirectional);
    RUN_TEST(test_zero_duplication_across_duplicate_window_wraparound);
    RUN_TEST(test_credit_starvation_halts_new_data_but_not_retransmit_or_ack);
    RUN_TEST(test_egress_blocked_recovers_without_acknowledged_loss);
    RUN_TEST(test_retry_exhaustion_surfaces_failure_and_frees_buffer);
    RUN_TEST(test_partial_ack_is_deferred_until_round_end);
    RUN_TEST(test_single_fragment_completion_ack_flushes_after_turnaround);
    RUN_TEST(test_30_by_1280_zero_gap_burst_delivers_all);
    RUN_TEST(test_zero_credit_completion_ack_stall_recovers_via_probe);
    RUN_TEST(test_credit_withdrawal_paces_retries_without_exhaustion);
    RUN_TEST(test_credit_restore_via_any_ack_short_circuits_probe);
    return UNITY_END();
}
