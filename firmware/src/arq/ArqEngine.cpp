#ifndef PACKET_MAX_LEN
#include "../config.h"
#endif

#include "ArqEngine.h"

#include <string.h>

namespace arq {

namespace {

uint16_t expectedBitmap(uint8_t fragment_count) {
    return framing_v3::fragmentExpectedBitmap(fragment_count);
}

uint8_t clampCredits(uint8_t value) {
    return value > ARQ_MAX_OUTSTANDING ? ARQ_MAX_OUTSTANDING : value;
}

} // namespace

ArqEngine::ArqEngine() {
    ArqConfig config;
    ArqCallbacks callbacks;
    reset(config, callbacks);
}

void ArqEngine::reset(const ArqConfig& config, const ArqCallbacks& callbacks) {
    config_ = config;
    callbacks_ = callbacks;
    memset(&counters_, 0, sizeof(counters_));
    counters_.reset = 1;
    tx_pool_.reset();
    rx_pool_.reset();
    resetDuplicateWindow(duplicate_window_);
    memset(tx_slots_, 0, sizeof(tx_slots_));
    memset(rx_slots_, 0, sizeof(rx_slots_));
    memset(pending_acks_, 0, sizeof(pending_acks_));
    pending_ack_count_ = 0;
    remote_credits_ = clampCredits(config.initial_remote_credits);
    next_datagram_id_ = config.initial_datagram_id;
    credit_probe_armed_ = false;
    credit_probe_deadline_ = 0;
}

ArqResult ArqEngine::onTxDatagram(const uint8_t* data, uint16_t len) {
    if (data == nullptr) {
        counters_.tx_rejected++;
        return ArqResult::BAD_ARGUMENT;
    }
    if (len == 0 || len > framing_v3::V3_MAX_DATAGRAM) {
        counters_.tx_rejected++;
        return ArqResult::BAD_LENGTH;
    }

    TxSlot* slot = nullptr;
    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        if (tx_slots_[i].state == TxState::EMPTY) {
            slot = &tx_slots_[i];
            break;
        }
    }
    if (slot == nullptr) {
        counters_.saturation++;
        counters_.tx_rejected++;
        return ArqResult::NO_TX_SLOT;
    }

    DatagramLease lease;
    const PoolResult pool_result = acquireDatagram(tx_pool_, lease);
    if (pool_result != PoolResult::OK) {
        counters_.allocation_failure++;
        counters_.tx_rejected++;
        return ArqResult::ALLOCATION_FAILED;
    }

    memcpy(lease.data, data, len);
    slot->state = TxState::QUEUED;
    slot->handle = lease.handle;
    slot->data = lease.data;
    slot->len = len;
    slot->datagram_id = next_datagram_id_++;
    slot->fragment_count = framing_v3::expectedFragmentCount(len);
    slot->acked_bitmap = 0;
    slot->round_bitmap = 0;
    slot->next_fragment = 0;
    slot->attempts = 0;
    slot->retry_deadline = 0;
    return ArqResult::OK;
}

void ArqEngine::onRxPacket(const framing_v3::Packet& packet, uint32_t now) {
    framing_v3::PacketType type = framing_v3::PacketType::DATA;
    framing_v3::ParseResult result = framing_v3::parsePacketType(packet, type);
    if (result != framing_v3::ParseResult::OK) {
        counters_.malformed_input++;
        return;
    }

    if (type == framing_v3::PacketType::DATA) {
        framing_v3::DataHeader header;
        const uint8_t* payload = nullptr;
        result = framing_v3::parseDataPacket(packet, header, payload);
        if (result != framing_v3::ParseResult::OK) {
            counters_.malformed_input++;
            return;
        }
        handleData(header, payload, now);
        return;
    }

    if (type == framing_v3::PacketType::ACK) {
        framing_v3::AckFrame ack;
        result = framing_v3::parseAckPacket(packet, ack);
        if (result != framing_v3::ParseResult::OK) {
            counters_.malformed_input++;
            return;
        }
        handleAck(ack, now);
        return;
    }

    if (type == framing_v3::PacketType::CONTROL) {
        framing_v3::ControlFrame ctrl;
        result = framing_v3::parseControlPacket(packet, ctrl);
        if (result != framing_v3::ParseResult::OK) {
            counters_.malformed_input++;
        }
        return;
    }

    if (type == framing_v3::PacketType::MGMT) {
        uint8_t payload[framing_v3::V3_MGMT_PAYLOAD_MAX];
        framing_v3::MutableMgmtFrame mgmt;
        mgmt.payload = payload;
        mgmt.payload_capacity = sizeof(payload);
        result = framing_v3::parseMgmtPacket(packet, mgmt);
        if (result != framing_v3::ParseResult::OK) {
            counters_.malformed_input++;
        }
    }
}

void ArqEngine::onTick(uint32_t now) {
    if (tryCompleteRxSlots()) {
        sendPendingAck();
        return;
    }
    if (sendPendingAck()) {
        return;
    }
    if (trySendRetransmit(now)) {
        return;
    }
    if (trySendOpenData(now)) {
        return;
    }
    (void)tryOpenQueued(now);
}

uint8_t ArqEngine::txActiveCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        if (tx_slots_[i].state == TxState::OPEN) {
            count++;
        }
    }
    return count;
}

uint8_t ArqEngine::txQueuedCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        if (tx_slots_[i].state == TxState::QUEUED) {
            count++;
        }
    }
    return count;
}

uint8_t ArqEngine::rxActiveCount() const {
    uint8_t count = 0;
    for (uint8_t i = 0; i < ARQ_MAX_RX_DATAGRAMS; ++i) {
        if (rx_slots_[i].active) {
            count++;
        }
    }
    return count;
}

bool ArqEngine::hasPendingWork() const {
    return pending_ack_count_ > 0 || txActiveCount() > 0 || txQueuedCount() > 0 ||
           rxActiveCount() > 0;
}

bool ArqEngine::nextDeadline(uint32_t now, uint32_t& deadline) const {
    if (pending_ack_count_ > 0) {
        deadline = now;
        return true;
    }
    for (uint8_t i = 0; i < ARQ_MAX_RX_DATAGRAMS; ++i) {
        if (rx_slots_[i].active && rx_slots_[i].complete_waiting_egress) {
            deadline = now;
            return true;
        }
    }
    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        const TxSlot& slot = tx_slots_[i];
        if (slot.state == TxState::OPEN && slot.round_bitmap != 0) {
            deadline = now;
            return true;
        }
    }
    if (txQueuedCount() > 0 && remote_credits_ > 0 && txActiveCount() == 0) {
        deadline = now;
        return true;
    }

    bool found = false;
    uint32_t best = 0;
    if (credit_probe_armed_ && remote_credits_ == 0 &&
        txQueuedCount() > 0 && txActiveCount() == 0) {
        best = credit_probe_deadline_;
        found = true;
    }
    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        const TxSlot& slot = tx_slots_[i];
        if (slot.state != TxState::OPEN) {
            continue;
        }
        const uint16_t full = expectedBitmap(slot.fragment_count);
        if ((slot.acked_bitmap & full) == full) {
            best = now;
            found = true;
            break;
        }
        if (slot.round_bitmap == 0) {
            if (!found || static_cast<int32_t>(slot.retry_deadline - best) < 0) {
                best = slot.retry_deadline;
                found = true;
            }
        }
    }
    if (found) {
        deadline = best;
    }
    return found;
}

uint8_t ArqEngine::currentCredits() const {
    const uint8_t egress = callbacks_.egress_capacity != nullptr ?
        callbacks_.egress_capacity(callbacks_.user) : ARQ_MAX_RX_DATAGRAMS;
    uint8_t free_slots = 0;
    for (uint8_t i = 0; i < ARQ_MAX_RX_DATAGRAMS; ++i) {
        if (!rx_slots_[i].active) {
            free_slots++;
        }
    }
    uint8_t credits = poolFreeCount(rx_pool_);
    if (credits > free_slots) {
        credits = free_slots;
    }
    if (credits > egress) {
        credits = egress;
    }
    return clampCredits(credits);
}

bool ArqEngine::enqueueAck(const framing_v3::AckFrame& ack) {
    for (uint8_t i = 0; i < pending_ack_count_; ++i) {
        if (pending_acks_[i].valid &&
            pending_acks_[i].ack.datagram_id == ack.datagram_id) {
            pending_acks_[i].ack = ack;
            return true;
        }
    }

    if (pending_ack_count_ >= ARQ_PENDING_ACK_CAPACITY) {
        counters_.saturation++;
        return false;
    }
    pending_acks_[pending_ack_count_].valid = true;
    pending_acks_[pending_ack_count_].ack = ack;
    pending_ack_count_++;
    return true;
}

bool ArqEngine::sendPendingAck() {
    if (pending_ack_count_ == 0 || callbacks_.send_packet == nullptr) {
        return false;
    }

    framing_v3::Packet packet;
    if (framing_v3::buildAckPacket(packet, pending_acks_[0].ack) !=
        framing_v3::ParseResult::OK) {
        counters_.malformed_input++;
        return false;
    }
    if (!callbacks_.send_packet(packet, callbacks_.user)) {
        return false;
    }

    for (uint8_t i = 1; i < pending_ack_count_; ++i) {
        pending_acks_[i - 1] = pending_acks_[i];
    }
    pending_ack_count_--;
    pending_acks_[pending_ack_count_].valid = false;
    return true;
}

bool ArqEngine::sendDataFragment(TxSlot& slot, uint8_t fragment_index) {
    if (callbacks_.send_packet == nullptr) {
        return false;
    }

    const uint16_t fragment_bit = static_cast<uint16_t>(1u << fragment_index);
    const uint8_t flags =
        (slot.round_bitmap & static_cast<uint16_t>(~fragment_bit)) == 0 ?
        framing_v3::V3_DATA_FLAG_ROUND_END : 0;

    framing_v3::FragmentDescriptor fragments[framing_v3::V3_MAX_FRAGS];
    uint8_t fragment_count = 0;
    if (framing_v3::describeDatagram(slot.data,
                                     slot.len,
                                     slot.datagram_id,
                                     flags,
                                     txQueuedCount(),
                                     fragments,
                                     framing_v3::V3_MAX_FRAGS,
                                     fragment_count) != framing_v3::FragmentResult::OK) {
        counters_.malformed_input++;
        return false;
    }
    if (fragment_index >= fragment_count) {
        counters_.malformed_input++;
        return false;
    }

    framing_v3::Packet packet;
    if (framing_v3::buildDataPacket(packet,
                                    fragments[fragment_index].header,
                                    fragments[fragment_index].payload) !=
        framing_v3::ParseResult::OK) {
        counters_.malformed_input++;
        return false;
    }
    return callbacks_.send_packet(packet, callbacks_.user);
}

bool ArqEngine::trySendRetransmit(uint32_t now) {
    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        TxSlot& slot = tx_slots_[i];
        if (slot.state != TxState::OPEN || slot.round_bitmap != 0) {
            continue;
        }
        const uint16_t full = expectedBitmap(slot.fragment_count);
        if ((slot.acked_bitmap & full) == full) {
            continue;
        }
        if (static_cast<int32_t>(now - slot.retry_deadline) < 0) {
            continue;
        }
        if (slot.attempts >= config_.max_attempts) {
            failTxSlot(slot);
            return false;
        }

        slot.attempts++;
        slot.round_bitmap = static_cast<uint16_t>(full & ~slot.acked_bitmap);
        slot.next_fragment = 0;
        while (slot.next_fragment < slot.fragment_count &&
               (slot.round_bitmap & static_cast<uint16_t>(1u << slot.next_fragment)) == 0) {
            slot.next_fragment++;
        }
        if (slot.next_fragment < slot.fragment_count &&
            sendDataFragment(slot, slot.next_fragment)) {
            slot.round_bitmap = static_cast<uint16_t>(
                slot.round_bitmap & ~static_cast<uint16_t>(1u << slot.next_fragment));
            slot.next_fragment++;
            if (slot.round_bitmap == 0) {
                slot.retry_deadline = now + config_.retry_timeout_cycles;
            }
            return true;
        }
    }
    return false;
}

bool ArqEngine::trySendOpenData(uint32_t now) {
    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        TxSlot& slot = tx_slots_[i];
        if (slot.state != TxState::OPEN || slot.round_bitmap == 0) {
            continue;
        }
        while (slot.next_fragment < slot.fragment_count &&
               (slot.round_bitmap & static_cast<uint16_t>(1u << slot.next_fragment)) == 0) {
            slot.next_fragment++;
        }
        if (slot.next_fragment >= slot.fragment_count) {
            slot.round_bitmap = 0;
            slot.retry_deadline = now + config_.retry_timeout_cycles;
            continue;
        }
        if (sendDataFragment(slot, slot.next_fragment)) {
            slot.round_bitmap = static_cast<uint16_t>(
                slot.round_bitmap & ~static_cast<uint16_t>(1u << slot.next_fragment));
            slot.next_fragment++;
            if (slot.round_bitmap == 0) {
                slot.retry_deadline = now + config_.retry_timeout_cycles;
            }
            return true;
        }
    }
    return false;
}

bool ArqEngine::tryOpenQueued(uint32_t now) {
    if (txActiveCount() != 0) {
        return false;
    }
    if (remote_credits_ == 0) {
        // Credit-stall probe. With zero credits, queued work, and nothing in
        // flight, no ACK can ever arrive to restore credits (the last ACK's
        // zero-credit snapshot is the receiver's final word). After a stall
        // timeout, open one datagram anyway: the receiver's reply — fresh
        // credits, a withdrawal, or an allocation failure — resynchronizes.
        if (txQueuedCount() == 0) {
            credit_probe_armed_ = false;
            return false;
        }
        if (!credit_probe_armed_) {
            credit_probe_armed_ = true;
            credit_probe_deadline_ = now + config_.credit_stall_timeout_cycles;
            return false;
        }
        if (static_cast<int32_t>(now - credit_probe_deadline_) < 0) {
            return false;
        }
        counters_.credit_stall_probes++;
    }
    credit_probe_armed_ = false;

    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        TxSlot& slot = tx_slots_[i];
        if (slot.state != TxState::QUEUED) {
            continue;
        }
        slot.state = TxState::OPEN;
        slot.acked_bitmap = 0;
        slot.round_bitmap = expectedBitmap(slot.fragment_count);
        slot.next_fragment = 0;
        slot.attempts = 1;
        slot.retry_deadline = now + config_.retry_timeout_cycles;
        if (remote_credits_ > 0) {
            remote_credits_--;
        }
        return trySendOpenData(now);
    }
    return false;
}

void ArqEngine::failTxSlot(TxSlot& slot) {
    counters_.retry_exhaustion++;
    clearTxSlot(slot);
}

void ArqEngine::completeTxSlot(TxSlot& slot) {
    counters_.tx_completed++;
    clearTxSlot(slot);
}

void ArqEngine::clearTxSlot(TxSlot& slot) {
    if (slot.handle.isValid()) {
        (void)releaseDatagram(tx_pool_, slot.handle);
    }
    slot = TxSlot();
}

void ArqEngine::handleAck(const framing_v3::AckFrame& ack, uint32_t now) {
    remote_credits_ = clampCredits(ack.receiver_credits);
    for (uint8_t i = 0; i < ARQ_MAX_OUTSTANDING; ++i) {
        TxSlot& slot = tx_slots_[i];
        if (slot.state != TxState::OPEN || slot.datagram_id != ack.datagram_id) {
            continue;
        }
        if (ack.failure == framing_v3::FailureStatus::CREDIT_WITHDRAWAL) {
            slot.acked_bitmap = static_cast<uint16_t>(slot.acked_bitmap & ack.fragment_bitmap);
            slot.round_bitmap = 0;
            slot.next_fragment = 0;
            slot.attempts = 1;
            slot.retry_deadline = now + config_.credit_stall_timeout_cycles;
            return;
        }
        slot.acked_bitmap = static_cast<uint16_t>(slot.acked_bitmap | ack.fragment_bitmap);
        const uint16_t full = expectedBitmap(slot.fragment_count);
        if ((slot.acked_bitmap & full) == full &&
            ack.failure == framing_v3::FailureStatus::NONE) {
            completeTxSlot(slot);
        }
        return;
    }
}

void ArqEngine::handleData(const framing_v3::DataHeader& header,
                           const uint8_t* payload,
                           uint32_t now) {
    (void)now;
    DuplicateAckState duplicate_ack;
    if (duplicateWindowFind(duplicate_window_, header.datagram_id, &duplicate_ack)) {
        counters_.duplicate_suppressed++;
        framing_v3::AckFrame ack;
        ack.datagram_id = header.datagram_id;
        ack.fragment_bitmap = duplicate_ack.fragment_bitmap;
        ack.receiver_credits = currentCredits();
        ack.failure = duplicate_ack.failure;
        (void)enqueueAck(ack);
        return;
    }

    RxSlot* slot = findRxSlot(header.datagram_id);
    if (slot == nullptr) {
        slot = allocateRxSlot(header);
    }
    if (slot == nullptr) {
        queueFailureAck(header.datagram_id,
                        0,
                        framing_v3::FailureStatus::ALLOCATION_FAILURE);
        counters_.allocation_failure++;
        return;
    }

    const framing_v3::FragmentResult accepted =
        framing_v3::acceptFragment(slot->reassembly, header, payload);
    if (accepted == framing_v3::FragmentResult::METADATA_MISMATCH ||
        accepted == framing_v3::FragmentResult::BAD_ARGUMENT ||
        accepted == framing_v3::FragmentResult::BAD_LENGTH ||
        accepted == framing_v3::FragmentResult::OUTPUT_TOO_SMALL) {
        counters_.malformed_input++;
        queueFailureAck(header.datagram_id,
                        slot->reassembly.received_bitmap,
                        framing_v3::FailureStatus::MALFORMED_INPUT);
        return;
    }

    if (framing_v3::reassemblyIsComplete(slot->reassembly)) {
        if (tryCompleteRxSlot(*slot)) {
            return;
        }
        slot->complete_waiting_egress = true;
        counters_.credit_withdrawal++;
        queueRxAck(*slot, framing_v3::FailureStatus::CREDIT_WITHDRAWAL);
        return;
    }

    if ((header.flags & framing_v3::V3_DATA_FLAG_ROUND_END) != 0) {
        queueRxAck(*slot, framing_v3::FailureStatus::NONE);
    }
}

ArqEngine::RxSlot* ArqEngine::findRxSlot(uint16_t datagram_id) {
    for (uint8_t i = 0; i < ARQ_MAX_RX_DATAGRAMS; ++i) {
        if (rx_slots_[i].active &&
            rx_slots_[i].reassembly.datagram_id == datagram_id) {
            return &rx_slots_[i];
        }
    }
    return nullptr;
}

ArqEngine::RxSlot* ArqEngine::allocateRxSlot(const framing_v3::DataHeader& header) {
    for (uint8_t i = 0; i < ARQ_MAX_RX_DATAGRAMS; ++i) {
        RxSlot& slot = rx_slots_[i];
        if (slot.active) {
            continue;
        }

        DatagramLease lease;
        if (acquireDatagram(rx_pool_, lease) != PoolResult::OK) {
            return nullptr;
        }
        slot.active = true;
        slot.handle = lease.handle;
        slot.complete_waiting_egress = false;
        framing_v3::resetReassembly(slot.reassembly, lease.data, lease.capacity);
        (void)header;
        return &slot;
    }
    counters_.saturation++;
    return nullptr;
}

void ArqEngine::releaseRxSlot(RxSlot& slot) {
    if (slot.handle.isValid()) {
        (void)releaseDatagram(rx_pool_, slot.handle);
    }
    slot = RxSlot();
}

bool ArqEngine::tryCompleteRxSlot(RxSlot& slot) {
    if (!framing_v3::reassemblyIsComplete(slot.reassembly)) {
        return false;
    }

    const uint8_t egress = callbacks_.egress_capacity != nullptr ?
        callbacks_.egress_capacity(callbacks_.user) : 1;
    if (egress == 0 || callbacks_.deliver_datagram == nullptr ||
        !callbacks_.deliver_datagram(slot.reassembly.output,
                                     slot.reassembly.datagram_length,
                                     callbacks_.user)) {
        return false;
    }

    DuplicateAckState duplicate_ack;
    duplicate_ack.fragment_bitmap = expectedBitmap(slot.reassembly.fragment_count);
    duplicate_ack.receiver_credits = currentCredits();
    duplicate_ack.failure = framing_v3::FailureStatus::NONE;
    duplicateWindowStore(duplicate_window_, slot.reassembly.datagram_id, duplicate_ack);

    framing_v3::AckFrame ack;
    ack.datagram_id = slot.reassembly.datagram_id;
    ack.fragment_bitmap = duplicate_ack.fragment_bitmap;
    ack.receiver_credits = currentCredits();
    ack.failure = framing_v3::FailureStatus::NONE;
    (void)enqueueAck(ack);

    counters_.delivered++;
    releaseRxSlot(slot);
    return true;
}

bool ArqEngine::tryCompleteRxSlots() {
    for (uint8_t i = 0; i < ARQ_MAX_RX_DATAGRAMS; ++i) {
        if (rx_slots_[i].active && rx_slots_[i].complete_waiting_egress &&
            tryCompleteRxSlot(rx_slots_[i])) {
            return true;
        }
    }
    return false;
}

void ArqEngine::queueRxAck(const RxSlot& slot, framing_v3::FailureStatus failure) {
    framing_v3::AckFrame ack;
    ack.datagram_id = slot.reassembly.datagram_id;
    ack.fragment_bitmap = slot.reassembly.received_bitmap;
    if (failure == framing_v3::FailureStatus::CREDIT_WITHDRAWAL) {
        const uint16_t full = expectedBitmap(slot.reassembly.fragment_count);
        if ((ack.fragment_bitmap & full) == full && full != 0) {
            ack.fragment_bitmap = static_cast<uint16_t>(ack.fragment_bitmap & ~(full & ~(full - 1u)));
        }
    }
    ack.receiver_credits = failure == framing_v3::FailureStatus::CREDIT_WITHDRAWAL ?
        0 : currentCredits();
    ack.failure = failure;
    (void)enqueueAck(ack);
}

void ArqEngine::queueFailureAck(uint16_t datagram_id,
                                uint16_t bitmap,
                                framing_v3::FailureStatus failure) {
    framing_v3::AckFrame ack;
    ack.datagram_id = datagram_id;
    ack.fragment_bitmap = bitmap;
    ack.receiver_credits = currentCredits();
    ack.failure = failure;
    (void)enqueueAck(ack);
}

} // namespace arq
