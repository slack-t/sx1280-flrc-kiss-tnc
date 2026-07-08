#pragma once

#include <stddef.h>
#include <stdint.h>

#include "DatagramPool.h"
#include "../framing/FragmentV3.h"
#include "../framing/FramingV3.h"

namespace arq {

static constexpr uint8_t ARQ_MAX_OUTSTANDING = 4;
static constexpr uint8_t ARQ_MAX_RX_DATAGRAMS = 4;
static constexpr uint8_t ARQ_PENDING_ACK_CAPACITY = 16;
static constexpr uint8_t ARQ_MAX_ATTEMPTS_DEFAULT = 8;

struct ArqConfig {
    uint32_t retry_timeout_cycles = 8;
    uint32_t credit_stall_timeout_cycles = 64;
    uint8_t max_attempts = ARQ_MAX_ATTEMPTS_DEFAULT;
    uint8_t initial_remote_credits = ARQ_MAX_OUTSTANDING;
    uint16_t initial_datagram_id = 0;
};

struct ArqCounters {
    uint32_t reset = 0;
    uint32_t retry_exhaustion = 0;
    uint32_t saturation = 0;
    uint32_t malformed_input = 0;
    uint32_t credit_withdrawal = 0;
    uint32_t allocation_failure = 0;
    uint32_t duplicate_suppressed = 0;
    uint32_t delivered = 0;
    uint32_t tx_completed = 0;
    uint32_t tx_rejected = 0;
    uint32_t credit_stall_probes = 0;
};

struct ArqCallbacks {
    bool (*send_packet)(const framing_v3::Packet& packet, void* user) = nullptr;
    bool (*deliver_datagram)(const uint8_t* data, uint16_t len, void* user) = nullptr;
    uint8_t (*egress_capacity)(void* user) = nullptr;
    void* user = nullptr;
};

enum class ArqResult : uint8_t {
    OK = 0,
    BAD_ARGUMENT,
    BAD_LENGTH,
    NO_TX_SLOT,
    ALLOCATION_FAILED,
};

class ArqEngine {
public:
    ArqEngine();

    void reset(const ArqConfig& config, const ArqCallbacks& callbacks);
    ArqResult onTxDatagram(const uint8_t* data, uint16_t len);
    void onRxPacket(const framing_v3::Packet& packet, uint32_t now);
    void onTick(uint32_t now);

    const ArqCounters& counters() const { return counters_; }
    uint8_t remoteCredits() const { return remote_credits_; }
    uint8_t txActiveCount() const;
    uint8_t txQueuedCount() const;
    uint8_t rxActiveCount() const;
    uint8_t pendingAckCount() const { return pending_ack_count_; }
    uint8_t txPoolFreeCount() const { return poolFreeCount(tx_pool_); }
    uint8_t rxPoolFreeCount() const { return poolFreeCount(rx_pool_); }
    bool hasPendingWork() const;
    bool nextDeadline(uint32_t now, uint32_t& deadline) const;

private:
    enum class TxState : uint8_t {
        EMPTY,
        QUEUED,
        OPEN,
    };

    struct TxSlot {
        TxState state = TxState::EMPTY;
        DatagramHandle handle;
        uint8_t* data = nullptr;
        uint16_t len = 0;
        uint16_t datagram_id = 0;
        uint8_t fragment_count = 0;
        uint16_t acked_bitmap = 0;
        uint16_t round_bitmap = 0;
        uint8_t next_fragment = 0;
        uint8_t attempts = 0;
        uint32_t retry_deadline = 0;
    };

    struct RxSlot {
        bool active = false;
        DatagramHandle handle;
        framing_v3::ReassemblyV3 reassembly;
        bool complete_waiting_egress = false;
    };

    struct PendingAck {
        bool valid = false;
        framing_v3::AckFrame ack;
    };

    ArqConfig config_;
    ArqCallbacks callbacks_;
    ArqCounters counters_;
    DatagramPool tx_pool_;
    DatagramPool rx_pool_;
    DuplicateWindow duplicate_window_;
    TxSlot tx_slots_[ARQ_MAX_OUTSTANDING];
    RxSlot rx_slots_[ARQ_MAX_RX_DATAGRAMS];
    PendingAck pending_acks_[ARQ_PENDING_ACK_CAPACITY];
    uint8_t pending_ack_count_ = 0;
    uint8_t remote_credits_ = 0;
    uint16_t next_datagram_id_ = 0;
    bool credit_probe_armed_ = false;
    uint32_t credit_probe_deadline_ = 0;

    uint8_t currentCredits() const;
    bool enqueueAck(const framing_v3::AckFrame& ack);
    bool sendPendingAck();
    bool sendDataFragment(TxSlot& slot, uint8_t fragment_index);
    bool trySendRetransmit(uint32_t now);
    bool trySendOpenData(uint32_t now);
    bool tryOpenQueued(uint32_t now);
    void failTxSlot(TxSlot& slot);
    void completeTxSlot(TxSlot& slot);
    void clearTxSlot(TxSlot& slot);
    void handleAck(const framing_v3::AckFrame& ack, uint32_t now);
    void handleData(const framing_v3::DataHeader& header,
                    const uint8_t* payload,
                    uint32_t now);
    RxSlot* findRxSlot(uint16_t datagram_id);
    RxSlot* allocateRxSlot(const framing_v3::DataHeader& header);
    void releaseRxSlot(RxSlot& slot);
    bool tryCompleteRxSlot(RxSlot& slot);
    bool tryCompleteRxSlots();
    void queueRxAck(const RxSlot& slot, framing_v3::FailureStatus failure);
    void queueFailureAck(uint16_t datagram_id,
                         uint16_t bitmap,
                         framing_v3::FailureStatus failure);
};

} // namespace arq
