#include "Mac.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include <esp_idf_version.h>
#include <string.h>

#include "../config.h"
#include "../arq/ArqEngine.h"
#include "../framing/Framing.h"
#include "../framing/FramingV3.h"
#include "../framing/Crc32.h"
#include "../stats/Stats.h"

// Set to 1 to log ARQ RX/TX diagnostics on the serial console
#define DEBUG_ARQ_DIAGNOSTICS 0

#if DEBUG_ARQ_DIAGNOSTICS
#define ARQ_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define ARQ_LOG(...) ((void)0)
#endif

namespace mac {
namespace {

// ── Wiring ────────────────────────────────────────────────────────────────────
Radio*        s_radio   = nullptr;
QueueHandle_t s_txQueue = nullptr;
QueueHandle_t s_rxQueue = nullptr;

// ── MAC wake sources ─────────────────────────────────────────────────────────
QueueSetHandle_t  s_waitSet    = nullptr;
SemaphoreHandle_t s_txWakeSema = nullptr;  // given by notifyTxWork()
QueueHandle_t     s_cmdQueue   = nullptr;  // MacCommand* (depth 1)

// ── Serialized command interface (other tasks → MAC) ─────────────────────────
struct MacCommand {
    enum class Type : uint8_t { APPLY_CONFIG, SCAN_BAND };
    Type        type = Type::APPLY_CONFIG;
    ModemConfig cfg;
    float       startMHz = 0, stopMHz = 0, stepMHz = 0;
    uint32_t    dwellUs = 0;
    int8_t*     rssiOut = nullptr;
    uint8_t     maxN = 0;
    int16_t     result = 0;
    uint8_t     scanCount = 0;
};
SemaphoreHandle_t s_cmdGate   = nullptr;  // serializes requesters
SemaphoreHandle_t s_cmdDone   = nullptr;
MacCommand        s_cmd;                  // owned by requester while gate held
MacCommand*       s_stagedCmd = nullptr;  // dequeued, awaiting an idle TX phase

// ── Link-health state (only touched from the MAC task) ───────────────────────
volatile uint32_t g_last_link_activity_ms      = 0;
volatile uint32_t g_last_payload_activity_ms   = 0;
volatile uint32_t g_last_bidirectional_ctrl_ms = 0;
volatile bool     g_link_ever_confirmed        = false;
uint16_t          s_control_seq                = 0;

uint32_t s_nextHbMs        = 0;
uint32_t s_lastHousekeepMs = 0;
uint32_t s_lastRadioEventMs = 0;

// Re-arm RX after this long without any radio event — defends against SX1280
// deaf states (see the receiver-lockout history in Radio.cpp).
constexpr uint32_t RADIO_REARM_IDLE_MS = 30000;

// ── RX-side state ─────────────────────────────────────────────────────────────
struct CompletedFrameCache {
    uint16_t seq         = FRAMING_SEQ_UNSET;
    uint8_t  total_frags = 0;
    uint16_t frame_len   = 0;
    uint32_t frame_crc32 = 0;
    bool     warmup      = false;
    uint32_t ack_mask    = 0;
    uint32_t tick_ms     = 0;
};
Reassembler         s_ra;         // lives in BSS, never on the task stack
CompletedFrameCache s_completed;
arq::ArqEngine      s_arq;
arq::ArqCounters    s_arqLastCounters;
PayloadFrame        s_arqDeliveryFrame;

// ── TX-side state machine ─────────────────────────────────────────────────────
enum class TxPhase : uint8_t {
    IDLE,         // no frame in flight; new work may start
    LBT_BACKOFF,  // channel busy; waiting random backoff before re-sensing
    WARMUP_WAIT,  // warmup fragment sent; waiting for its ACK
    SENDING,      // sending this round's pending fragments (inter-frag pacing)
    WAIT_ACK,     // round complete; waiting for the bitmap ACK
    HB_WAIT,      // idle heartbeat sent; waiting for HEARTBEAT_ACK
};

struct TxState {
    TxPhase      phase = TxPhase::IDLE;
    PayloadFrame frame;               // BSS, not on the task stack
    uint16_t     seq         = 0;
    uint8_t      total_frags = 0;
    uint32_t     frame_crc32 = 0;
    uint32_t     pending_mask = 0;
    uint32_t     sent_mask    = 0;
    uint8_t      round        = 0;
    uint8_t      next_idx     = 0;
    uint8_t      lbt_tries    = 0;
    uint16_t     hb_seq       = 0;
    uint32_t     deadline_ms  = 0;
};
TxState s_tx;

// ── Timing helpers ────────────────────────────────────────────────────────────
uint32_t ackFallbackDelayMs(uint8_t total_frags) {
    return RADIO_ACK_FALLBACK_DELAY_MS +
           static_cast<uint32_t>(total_frags > 0 ? (total_frags - 1u) * 2u : 0u);
}

uint32_t ackTurnaroundDelayMs(uint8_t total_frags) {
    return RADIO_ACK_TURNAROUND_DELAY_MS +
           static_cast<uint32_t>(total_frags > 0 ? (total_frags - 1u) : 0u);
}

uint32_t ackTimeoutMs(uint8_t total_frags) {
    return RADIO_ACK_TIMEOUT_MS +
           static_cast<uint32_t>(total_frags > 0 ? (total_frags - 1u) * RADIO_INTER_FRAG_DELAY_MS : 0u);
}

uint32_t reassemblyTimeoutMs(uint8_t total_frags) {
    return RADIO_REASSEMBLY_TIMEOUT_MS +
           static_cast<uint32_t>(total_frags > 0 ? (total_frags - 1u) * RADIO_INTER_FRAG_DELAY_MS * 2u : 0u);
}

uint32_t hbIntervalMs() {
    return RADIO_HEARTBEAT_INTERVAL_MS + (esp_random() % (RADIO_HEARTBEAT_JITTER_MS + 1u));
}

uint32_t lbtBackoffMs() {
    return RADIO_LBT_BACKOFF_MIN_MS +
           (esp_random() % (RADIO_LBT_BACKOFF_MAX_MS - RADIO_LBT_BACKOFF_MIN_MS + 1));
}

bool deadlinePassed(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

// ── Link-health helpers ───────────────────────────────────────────────────────
bool isLinkReady() {
    if (!g_link_ever_confirmed) return false;
    return (millis() - g_last_bidirectional_ctrl_ms) <= RADIO_LINK_READY_TTL_MS;
}

bool isLinkIdle() {
    return (millis() - g_last_link_activity_ms) >= RADIO_LINK_IDLE_MS;
}

bool isPayloadIdle() {
    if (g_last_payload_activity_ms == 0) return true;
    return (millis() - g_last_payload_activity_ms) >= RADIO_LINK_IDLE_MS;
}

void noteLinkActivity() {
    g_last_link_activity_ms = millis();
}

void notePayloadActivity() {
    const uint32_t now = millis();
    g_last_payload_activity_ms = now;
    g_last_link_activity_ms = now;
}

void noteBidirectionalControl() {
    const bool was_ready = isLinkReady();
    g_last_bidirectional_ctrl_ms = millis();
    g_link_ever_confirmed = true;
    noteLinkActivity();
    if (!was_ready) {
        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().linkReadyTransitions++;
        sm.unlock();
    }
}

// ── Stats note helpers ────────────────────────────────────────────────────────
void noteRadioError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().errorCount++;
    sm.get().radioState = RadioState::ERROR;
    sm.get().lastRadioErrorMs = millis();
    sm.unlock();
}

void noteQueueDrop() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqQueueDrops++;
    sm.unlock();
}

void noteIdentityReset() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqIdentityResets++;
    sm.unlock();
}

void noteRadioTxError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().radioTxErrors++;
    sm.unlock();
    noteRadioError();
}

void noteRadioRxError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().radioRxErrors++;
    sm.get().lastRadioErr     = s_radio->lastRadioErr();
    sm.get().lastIrqStatus    = s_radio->lastIrqStatus();
    sm.get().lastPacketLength = s_radio->lastPacketLength();
    sm.unlock();
    noteRadioError();
}

void bumpCounter(uint32_t Stats::* field) {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().*field += 1;
    sm.unlock();
}

void noteReassemblyDrop()     { bumpCounter(&Stats::arqReassemblyDrops); }
void noteControlMalformed()   { bumpCounter(&Stats::controlMalformedDrops); }
void noteMalformedAck()       { bumpCounter(&Stats::rxMalformedAckCount); }
void noteMalformedData()      { bumpCounter(&Stats::rxMalformedDataCount); }
void noteNativeOversizeDrop() { bumpCounter(&Stats::nativeOversizeDropCount); }
void noteEgressDeferral()     { bumpCounter(&Stats::rxEgressDeferrals); }

bool isGenericTransport() {
    return s_radio->config().transportMode == TransportMode::GENERIC_FRAGMENTED;
}

void refreshArqDebugStats(uint32_t now) {
    uint32_t deadline = 0;
    const bool has_deadline = s_arq.nextDeadline(now, deadline);
    auto& sm = StatsManager::instance();
    sm.lock();
    Stats& stats = sm.get();
    stats.arqV3TxActive = s_arq.txActiveCount();
    stats.arqV3TxQueued = s_arq.txQueuedCount();
    stats.arqV3RxActive = s_arq.rxActiveCount();
    stats.arqV3PendingAck = s_arq.pendingAckCount();
    stats.arqV3RemoteCredits = s_arq.remoteCredits();
    stats.arqV3TxPoolFree = s_arq.txPoolFreeCount();
    stats.arqV3RxPoolFree = s_arq.rxPoolFreeCount();
    stats.arqV3NextDeadlineMs = has_deadline ?
        static_cast<uint32_t>(static_cast<int32_t>(deadline - now) < 0 ? 0 : deadline - now) :
        0xFFFFFFFFu;
    sm.unlock();
}

void syncArqCounters() {
    const arq::ArqCounters now = s_arq.counters();
    auto& sm = StatsManager::instance();
    sm.lock();
    Stats& stats = sm.get();
    stats.arqV3RetryExhaustion += now.retry_exhaustion - s_arqLastCounters.retry_exhaustion;
    stats.arqV3Saturation += now.saturation - s_arqLastCounters.saturation;
    stats.arqV3MalformedInput += now.malformed_input - s_arqLastCounters.malformed_input;
    stats.arqV3CreditWithdrawal += now.credit_withdrawal - s_arqLastCounters.credit_withdrawal;
    stats.arqV3AllocationFailure += now.allocation_failure - s_arqLastCounters.allocation_failure;
    stats.arqV3TxCompleted += now.tx_completed - s_arqLastCounters.tx_completed;
    stats.arqFramesFailed += now.retry_exhaustion - s_arqLastCounters.retry_exhaustion;
    stats.arqDuplicateSuppressed += now.duplicate_suppressed - s_arqLastCounters.duplicate_suppressed;
    stats.rxEgressDeferrals += now.credit_withdrawal - s_arqLastCounters.credit_withdrawal;
    sm.unlock();
    s_arqLastCounters = now;
    refreshArqDebugStats(millis());
}

bool waitForLbt(uint32_t now) {
    (void)now;
    if (s_radio->config().lbtRssiThresholdDbm == 0) {
        return true;
    }
    for (uint8_t tries = 0; tries < RADIO_LBT_MAX_RETRIES; ++tries) {
        if (!s_radio->isChannelBusy()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(lbtBackoffMs()));
    }
    return true;
}

bool sendArqRadioPacket(const framing_v3::Packet& v3pkt, void*) {
    framing_v3::PacketType type = framing_v3::PacketType::DATA;
    if (framing_v3::parsePacketType(v3pkt, type) != framing_v3::ParseResult::OK) {
        return false;
    }
    if (type == framing_v3::PacketType::DATA) {
        waitForLbt(millis());
    }

    Packet pkt;
    pkt.len = v3pkt.len;
    memcpy(pkt.data, v3pkt.data, v3pkt.len);

    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().radioState = RadioState::TX;
    if (type == framing_v3::PacketType::ACK) {
        sm.get().arqAckTxCount++;
    }
    sm.unlock();

    const int16_t err = s_radio->transmit(pkt, true);

    sm.lock();
    sm.get().radioState = RadioState::IDLE;
    if (err != RADIOLIB_ERR_NONE) {
        if (type == framing_v3::PacketType::ACK) {
            sm.get().arqAckTxErrors++;
        }
    }
    sm.unlock();

    if (err != RADIOLIB_ERR_NONE) {
        noteRadioTxError();
        return false;
    }
    // Only data-path packets count as payload activity. CONTROL replies
    // (e.g. HEARTBEAT_ACK) must not, or answering the peer's heartbeats
    // defers our own via the payload-idle gate and the schedules starve
    // each other.
    if (type == framing_v3::PacketType::DATA || type == framing_v3::PacketType::ACK) {
        notePayloadActivity();
    } else {
        noteLinkActivity();
    }
    if (type == framing_v3::PacketType::DATA) {
        vTaskDelay(pdMS_TO_TICKS(RADIO_INTER_FRAG_DELAY_MS));
    }
    return true;
}

bool deliverArqDatagram(const uint8_t* data, uint16_t len, void*) {
    if (data == nullptr || len == 0 || len > TNC_PAYLOAD_MAX_LEN) {
        return false;
    }
    s_arqDeliveryFrame.len = len;
    s_arqDeliveryFrame.rssi = s_radio->lastRssi();
    memcpy(s_arqDeliveryFrame.data, data, len);
    if (xQueueSend(s_rxQueue, &s_arqDeliveryFrame, 0) != pdPASS) {
        noteEgressDeferral();
        return false;
    }

    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxCount++;
    sm.get().rxBytes += len;
    sm.get().rssi = s_arqDeliveryFrame.rssi;
    sm.get().radioState = RadioState::RX;
    sm.get().arqFramesCompleted++;
    if (uxQueueSpacesAvailable(s_rxQueue) == 0) {
        sm.get().rxQueueWaitCount++;
    }
    sm.unlock();
    notePayloadActivity();
    // Datagram completion counts as bidirectional confirmation on the RX
    // side: beyond the first credit window the sender cannot keep opening
    // new datagrams unless our ACKs are reaching it.
    noteBidirectionalControl();
    return true;
}

uint8_t arqEgressCapacity(void*) {
    if (s_rxQueue == nullptr) {
        return 0;
    }
    const UBaseType_t spaces = uxQueueSpacesAvailable(s_rxQueue);
    return spaces > 255 ? 255 : static_cast<uint8_t>(spaces);
}

void resetArqEngine() {
    arq::ArqConfig cfg;
    cfg.retry_timeout_cycles = ackTimeoutMs(framing_v3::V3_MAX_FRAGS);
    cfg.max_attempts = 8;
    cfg.initial_remote_credits = arq::ARQ_MAX_OUTSTANDING;

    arq::ArqCallbacks callbacks;
    callbacks.send_packet = sendArqRadioPacket;
    callbacks.deliver_datagram = deliverArqDatagram;
    callbacks.egress_capacity = arqEgressCapacity;
    callbacks.user = nullptr;
    s_arq.reset(cfg, callbacks);
    s_arqLastCounters = s_arq.counters();
}

// ── RX-side ACK / delivery ────────────────────────────────────────────────────
// Transmit a bitmap ACK. Returns true on success. Counter bookkeeping only;
// callers add their own path-specific counters.
bool sendAckPacket(const AckFrame& ack) {
    Packet pkt;
    framingBuildAckPacket(pkt, ack);

    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqAckTxCount++;
    sm.unlock();

    if (s_radio->transmit(pkt, true) != RADIOLIB_ERR_NONE) {
        sm.lock();
        sm.get().arqAckTxErrors++;
        sm.unlock();
        noteRadioTxError();
        return false;
    }
    return true;
}

bool isReassemblyValid(const Reassembler& ra) {
    if (ra.frame_len == 0 || ra.frame_len > TNC_PAYLOAD_MAX_LEN) {
        return false;
    }
    uint16_t assembled_len = 0;
    for (uint8_t i = 0; i < ra.total_frags; i++) {
        assembled_len += ra.frag_len[i];
    }
    if (assembled_len != ra.frame_len) {
        bumpCounter(&Stats::arqReassemblyIntegrityDrops);
        return false;
    }
    uint32_t computed_crc = framing::computeCrc32(ra.buf, assembled_len);
    if (computed_crc != ra.frame_crc32) {
        bumpCounter(&Stats::arqFrameCrcErrors);
        return false;
    }
    return true;
}

void cacheCompleted(uint32_t now, uint32_t ack_mask) {
    s_completed.seq         = s_ra.seq;
    s_completed.total_frags = s_ra.total_frags;
    s_completed.frame_len   = s_ra.frame_len;
    s_completed.frame_crc32 = s_ra.frame_crc32;
    s_completed.warmup      = s_ra.warmup;
    s_completed.ack_mask    = ack_mask;
    s_completed.tick_ms     = now;
}

// Called when the receiver-side ACK timer is due. For a complete reassembly
// this delivers to the host queue BEFORE acknowledging (egress reservation):
// if rxQueue is full the ACK is withheld so the sender retries instead of the
// frame becoming acknowledged loss. Duplicate retransmissions after a
// successful delivery are re-ACKed from the completed cache without
// re-delivering.
void handleAckDue(uint32_t now) {
    if (!s_ra.ack_pending || s_ra.seq == FRAMING_SEQ_UNSET) {
        return;
    }

    if (!s_ra.isComplete()) {
        // Partial-round ACK: reports the bitmap, delivers nothing.
        AckFrame ack;
        ack.seq           = s_ra.seq;
        ack.total_frags   = s_ra.total_frags;
        ack.window_base   = 0;
        ack.received_mask = s_ra.received_mask;
        ARQ_LOG("[ARQ RX:ACK seq=%04x tot=%u mask=%08lx complete=0]\n",
                s_ra.seq, s_ra.total_frags, static_cast<unsigned long>(s_ra.received_mask));
        if (sendAckPacket(ack)) {
            notePayloadActivity();
            s_ra.ack_pending = false;
        } else {
            s_ra.ack_due_ms = now + RADIO_ACK_TURNAROUND_DELAY_MS;
        }
        return;
    }

    if (!isReassemblyValid(s_ra)) {
        // Cache an explicit fatal NACK: the sender's retransmission will be
        // answered with mask=0 from the duplicate cache, aborting its frame.
        cacheCompleted(now, 0);
        s_ra.reset();
        return;
    }

    if (!s_ra.warmup) {
        PayloadFrame frame;
        frame.len = 0;
        for (uint8_t i = 0; i < s_ra.total_frags; i++) {
            ARQ_LOG("[ARQ RX:ASSEMBLE seq=%04x i=%u flen=%u]\n",
                    s_ra.seq, i, s_ra.frag_len[i]);
            memcpy(frame.data + frame.len,
                   s_ra.buf + i * FRAMING_FRAG_DATA,
                   s_ra.frag_len[i]);
            frame.len += s_ra.frag_len[i];
        }
        frame.rssi = s_ra.last_rssi;

        if (xQueueSend(s_rxQueue, &frame, 0) != pdPASS) {
            // Egress full: withhold the ACK, retry delivery shortly.
            noteEgressDeferral();
            s_ra.ack_due_ms = now + RX_EGRESS_RETRY_MS;
            return;
        }

        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().rxCount++;
        sm.get().rxBytes   += frame.len;
        sm.get().rssi       = frame.rssi;
        sm.get().radioState = RadioState::RX;
        sm.get().arqFramesCompleted++;
        if (uxQueueSpacesAvailable(s_rxQueue) == 0) {
            sm.get().rxQueueWaitCount++;
        }
        sm.unlock();
    } else {
        bumpCounter(&Stats::arqWarmupRx);
        ARQ_LOG("[ARQ RX:WARMUP seq=%04x]\n", s_ra.seq);
    }

    AckFrame ack;
    ack.seq           = s_ra.seq;
    ack.total_frags   = s_ra.total_frags;
    ack.window_base   = 0;
    ack.received_mask = s_ra.received_mask;

    ARQ_LOG("[ARQ RX:DONE seq=%04x tot=%u mask=%08lx len=%u]\n",
            s_ra.seq, s_ra.total_frags,
            static_cast<unsigned long>(s_ra.received_mask), s_ra.frame_len);

    cacheCompleted(now, s_ra.received_mask);
    s_ra.reset();

    // ACK failure is recoverable: the sender retries and hits the duplicate
    // cache, which re-ACKs without re-delivering.
    if (sendAckPacket(ack)) {
        notePayloadActivity();
    }
}

// ── TX-side state machine ─────────────────────────────────────────────────────
void nextSeq() {
    s_tx.seq++;
    if (s_tx.seq == FRAMING_SEQ_UNSET) {
        s_tx.seq++;
    }
}

void postFrame(uint32_t now) {
    s_tx.phase = TxPhase::IDLE;
    s_nextHbMs = now + hbIntervalMs();
}

void startDataRounds(uint32_t now) {
    nextSeq();
    s_tx.pending_mask = framingExpectedMask(s_tx.total_frags);
    s_tx.sent_mask    = 0;
    s_tx.round        = 0;
    s_tx.next_idx     = 0;
    s_tx.phase        = TxPhase::SENDING;
    s_tx.deadline_ms  = now;

    bumpCounter(&Stats::arqFramesStarted);
    ARQ_LOG("[ARQ TX:START seq=%04x tot=%u len=%u]\n",
            s_tx.seq, s_tx.total_frags, s_tx.frame.len);
}

void failFrame(uint32_t now) {
    bumpCounter(&Stats::arqFramesFailed);
    ARQ_LOG("[ARQ TX:FAIL seq=%04x tot=%u pending=%08lx]\n",
            s_tx.seq, s_tx.total_frags, static_cast<unsigned long>(s_tx.pending_mask));
    noteRadioError();
    postFrame(now);
}

void nextRoundOrFail(uint32_t now) {
    s_tx.round++;
    if (s_tx.round >= RADIO_ARQ_MAX_ROUNDS) {
        failFrame(now);
        return;
    }
    ARQ_LOG("[ARQ TX:ROUND seq=%04x tot=%u round=%u pending=%08lx]\n",
            s_tx.seq, s_tx.total_frags, s_tx.round,
            static_cast<unsigned long>(s_tx.pending_mask));
    s_tx.sent_mask   = 0;
    s_tx.next_idx    = 0;
    s_tx.phase       = TxPhase::SENDING;
    s_tx.deadline_ms = now;
}

void deliverFrame(uint32_t now) {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().txCount++;
    sm.get().txBytes += s_tx.frame.len;
    sm.unlock();
    ARQ_LOG("[ARQ TX:DONE seq=%04x tot=%u rounds=%u]\n",
            s_tx.seq, s_tx.total_frags, s_tx.round + 1);
    postFrame(now);
}

// Send at most one fragment per call; inter-fragment pacing is enforced by
// the deadline so the MAC loop stays responsive between fragments.
void sendNextFragment(uint32_t now) {
    while (s_tx.next_idx < s_tx.total_frags &&
           (s_tx.pending_mask & (1u << s_tx.next_idx)) == 0) {
        s_tx.next_idx++;
    }
    if (s_tx.next_idx >= s_tx.total_frags) {
        // Round exhausted. If nothing went out (all transmits failed), move
        // straight to the next round as the legacy loop did; otherwise wait
        // for the bitmap ACK (the receiver's fallback timer covers a lost
        // round-end flag).
        if (s_tx.sent_mask == 0) {
            nextRoundOrFail(now);
        } else {
            s_tx.phase       = TxPhase::WAIT_ACK;
            s_tx.deadline_ms = now + ackTimeoutMs(s_tx.total_frags);
        }
        return;
    }

    const uint8_t  idx    = s_tx.next_idx;
    const uint32_t bit    = 1u << idx;
    const uint16_t offset = static_cast<uint16_t>(idx) * FRAMING_FRAG_DATA;
    const uint8_t  chunk  = static_cast<uint8_t>(
        (s_tx.frame.len - offset < FRAMING_FRAG_DATA)
            ? (s_tx.frame.len - offset)
            : FRAMING_FRAG_DATA);
    const uint32_t later_pending = s_tx.pending_mask & ~framingMaskThrough(idx);
    const bool     round_end     = later_pending == 0;

    Packet pkt;
    framingBuildDataPacket(pkt, s_tx.seq, idx, s_tx.total_frags, round_end,
                           s_tx.frame.data + offset, chunk,
                           s_tx.frame.len, s_tx.frame_crc32);

    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().radioState = RadioState::TX;
    if (s_tx.round > 0) {
        sm.get().arqRetryCount++;
    }
    sm.unlock();

    const int16_t err = s_radio->transmit(pkt, round_end);

    sm.lock();
    sm.get().radioState = RadioState::IDLE;
    if (err != RADIOLIB_ERR_NONE) {
        sm.get().errorCount++;
        sm.get().radioTxErrors++;
    }
    sm.unlock();

    const uint32_t after = millis();
    s_tx.next_idx++;
    if (err == RADIOLIB_ERR_NONE) {
        notePayloadActivity();
        s_tx.sent_mask |= bit;
        if (round_end) {
            s_tx.phase       = TxPhase::WAIT_ACK;
            s_tx.deadline_ms = after + ackTimeoutMs(s_tx.total_frags);
            return;
        }
    }
    s_tx.deadline_ms = after + (s_tx.sent_mask != 0 ? RADIO_INTER_FRAG_DELAY_MS : 0);
}

void startWarmup(uint32_t now) {
    nextSeq();

    static constexpr uint8_t warmup_payload[1] = {0};
    const uint32_t warmup_crc32 =
        framing::computeCrc32(warmup_payload, sizeof(warmup_payload));
    Packet pkt;
    framingBuildDataPacket(pkt, s_tx.seq, 0, 1, true,
                           warmup_payload, sizeof(warmup_payload),
                           sizeof(warmup_payload), warmup_crc32, true);

    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().radioState = RadioState::TX;
    sm.unlock();

    const int16_t err = s_radio->transmit(pkt, true);

    sm.lock();
    sm.get().radioState = RadioState::IDLE;
    if (err == RADIOLIB_ERR_NONE) {
        sm.get().arqWarmupTx++;
    } else {
        sm.get().errorCount++;
        sm.get().radioTxErrors++;
    }
    sm.unlock();

    if (err == RADIOLIB_ERR_NONE) {
        notePayloadActivity();
        s_tx.phase       = TxPhase::WARMUP_WAIT;
        s_tx.deadline_ms = millis() + ackTimeoutMs(1);
    } else {
        startDataRounds(now);
    }
}

// Transmit path for native single-packet mode (no ARQ).
void sendNative(uint32_t now) {
    if (s_tx.frame.len > FRAMING_NATIVE_MAX_PAYLOAD) {
        noteNativeOversizeDrop();
        postFrame(now);
        return;
    }
    Packet pkt;
    framingBuildNativePacket(pkt, s_tx.frame.data,
                             static_cast<uint8_t>(s_tx.frame.len));

    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().radioState = RadioState::TX;
    sm.unlock();

    const int16_t err = s_radio->transmit(pkt, true);

    sm.lock();
    sm.get().radioState = RadioState::IDLE;
    if (err == RADIOLIB_ERR_NONE) {
        sm.get().nativeTxCount++;
        sm.get().txCount++;
        sm.get().txBytes += s_tx.frame.len;
    } else {
        sm.get().errorCount++;
        sm.get().radioTxErrors++;
        sm.get().radioState = RadioState::ERROR;
    }
    sm.unlock();

    if (err == RADIOLIB_ERR_NONE) {
        notePayloadActivity();
    }
    postFrame(millis());
}

// LBT has cleared (or was skipped/exhausted): hand the staged frame to the
// appropriate transmit path.
void dispatchFrame(uint32_t now) {
    if (s_radio->config().transportMode == TransportMode::NATIVE_PACKET) {
        sendNative(now);
        return;
    }

    s_tx.total_frags = framingExpectedTotalFrags(s_tx.frame.len);
    s_tx.frame_crc32 = framing::computeCrc32(s_tx.frame.data, s_tx.frame.len);

    // Warm up idle multi-fragment bursts with a real one-fragment ARQ DATA
    // exchange: CONTROL handshakes do not exercise the receiver reassembly +
    // ACK path.
    if (s_tx.total_frags > 1 && isPayloadIdle()) {
        startWarmup(now);
    } else {
        startDataRounds(now);
    }
}

// New frame dequeued into s_tx.frame: validate, run LBT, dispatch.
void startFrame(uint32_t now) {
    if (s_tx.frame.len == 0 || s_tx.frame.len > TNC_PAYLOAD_MAX_LEN) {
        noteRadioError();
        return;
    }

    // LBT-CSMA: skip entirely when disabled to avoid the 500µs dwell + SPI
    // read overhead on every TX.
    s_tx.lbt_tries = 0;
    if (s_radio->config().lbtRssiThresholdDbm != 0 && s_radio->isChannelBusy()) {
        s_tx.phase       = TxPhase::LBT_BACKOFF;
        s_tx.deadline_ms = now + lbtBackoffMs();
        return;
    }
    dispatchFrame(now);
}

void startHeartbeat(uint32_t now) {
    s_control_seq++;
    if (isGenericTransport()) {
        framing_v3::ControlFrame ctrl;
        ctrl.subtype = framing_v3::ControlSubtype::HEARTBEAT;
        framing_v3::Packet pkt;
        if (framing_v3::buildControlPacket(pkt, ctrl) != framing_v3::ParseResult::OK) {
            return;
        }
        bumpCounter(&Stats::controlHeartbeatTx);
        if (sendArqRadioPacket(pkt, nullptr)) {
            noteLinkActivity();
            s_tx.phase       = TxPhase::HB_WAIT;
            s_tx.deadline_ms = millis() + RADIO_CONTROL_ACK_TIMEOUT_MS;
        } else {
            s_nextHbMs = now + hbIntervalMs();
        }
        return;
    }

    ControlFrame ctrl;
    ctrl.type = ControlType::HEARTBEAT;
    ctrl.seq  = s_control_seq;
    Packet pkt;
    framingBuildControlPacket(pkt, ctrl);

    bumpCounter(&Stats::controlHeartbeatTx);

    if (s_radio->transmit(pkt, true) == RADIOLIB_ERR_NONE) {
        noteLinkActivity();
        s_tx.hb_seq      = s_control_seq;
        s_tx.phase       = TxPhase::HB_WAIT;
        s_tx.deadline_ms = millis() + RADIO_CONTROL_ACK_TIMEOUT_MS;
    } else {
        noteRadioTxError();
        s_nextHbMs = now + hbIntervalMs();
    }
}

// Feed a received bitmap ACK into the TX state machine.
void handleAckForTx(const AckFrame& ack) {
    const uint32_t now = millis();
    switch (s_tx.phase) {
    case TxPhase::WARMUP_WAIT:
        if (ack.seq == s_tx.seq && ack.total_frags == 1 &&
            ack.window_base == 0 && (ack.received_mask & 0x01u) != 0) {
            bumpCounter(&Stats::arqWarmupAckRx);
            notePayloadActivity();
            startDataRounds(now);
        }
        break;

    case TxPhase::SENDING:
    case TxPhase::WAIT_ACK: {
        if (ack.seq != s_tx.seq || ack.total_frags != s_tx.total_frags ||
            ack.window_base != 0) {
            break;
        }
        const uint32_t expected_mask = framingExpectedMask(s_tx.total_frags);
        const uint32_t ack_mask      = ack.received_mask & expected_mask;
        if (ack_mask == 0) {
            // Explicit fatal NACK: the receiver rejected the whole frame.
            ARQ_LOG("[ARQ TX:NACK seq=%04x tot=%u round=%u pending=%08lx]\n",
                    s_tx.seq, s_tx.total_frags, s_tx.round,
                    static_cast<unsigned long>(s_tx.pending_mask));
            failFrame(now);
            break;
        }
        ARQ_LOG("[ARQ TX:ACK seq=%04x tot=%u ack_mask=%08lx pend=%08lx->%08lx]\n",
                s_tx.seq, s_tx.total_frags, static_cast<unsigned long>(ack_mask),
                static_cast<unsigned long>(s_tx.pending_mask),
                static_cast<unsigned long>(s_tx.pending_mask & ~ack_mask));
        s_tx.pending_mask &= ~ack_mask;
        if (s_tx.pending_mask == 0) {
            deliverFrame(now);
        } else if (s_tx.phase == TxPhase::WAIT_ACK) {
            nextRoundOrFail(now);
        }
        break;
    }

    default:
        break;
    }
}

void handleV3Control(const framing_v3::ControlFrame& ctrl) {
    if (ctrl.subtype == framing_v3::ControlSubtype::HEARTBEAT) {
        bumpCounter(&Stats::controlHeartbeatRx);
        framing_v3::ControlFrame ackCtrl;
        ackCtrl.subtype = framing_v3::ControlSubtype::HEARTBEAT_ACK;
        framing_v3::Packet ackPkt;
        if (framing_v3::buildControlPacket(ackPkt, ackCtrl) == framing_v3::ParseResult::OK &&
            sendArqRadioPacket(ackPkt, nullptr)) {
            bumpCounter(&Stats::controlHeartbeatAckTx);
            noteLinkActivity();
        }
        return;
    }
    if (ctrl.subtype == framing_v3::ControlSubtype::HEARTBEAT_ACK) {
        if (s_tx.phase == TxPhase::HB_WAIT) {
            bumpCounter(&Stats::controlHeartbeatAckRx);
            noteBidirectionalControl();
            refreshLinkStats();
            postFrame(millis());
        }
        return;
    }
    if (ctrl.subtype == framing_v3::ControlSubtype::LINK_STATE) {
        noteBidirectionalControl();
    }
}

void handleV3RadioPacket(const Packet& pkt) {
    framing_v3::Packet v3pkt;
    v3pkt.len = pkt.len;
    memcpy(v3pkt.data, pkt.data, pkt.len);

    framing_v3::PacketType type = framing_v3::PacketType::DATA;
    const framing_v3::ParseResult typeResult = framing_v3::parsePacketType(v3pkt, type);
    if (typeResult == framing_v3::ParseResult::VERSION_MISMATCH) {
        bumpCounter(&Stats::v3VersionDrops);
        return;
    }
    if (typeResult == framing_v3::ParseResult::UNKNOWN_TYPE) {
        bumpCounter(&Stats::v3UnknownTypeDrops);
        return;
    }
    if (typeResult != framing_v3::ParseResult::OK) {
        bumpCounter(&Stats::rxMalformedDataCount);
        return;
    }

    noteLinkActivity();
    if (type == framing_v3::PacketType::CONTROL) {
        framing_v3::ControlFrame ctrl;
        if (framing_v3::parseControlPacket(v3pkt, ctrl) != framing_v3::ParseResult::OK) {
            noteControlMalformed();
            return;
        }
        handleV3Control(ctrl);
        return;
    }

    if (type == framing_v3::PacketType::ACK) {
        bumpCounter(&Stats::arqAckRxCount);
        // A parseable ACK from the peer is bidirectional proof: it heard our
        // DATA and we heard its reply. Keeps the link READY during transfers,
        // when heartbeats are suppressed by the idle gate.
        noteBidirectionalControl();
    }
    if (type == framing_v3::PacketType::DATA || type == framing_v3::PacketType::ACK) {
        notePayloadActivity();
    }
    s_arq.onRxPacket(v3pkt, millis());
    syncArqCounters();
}

// ── RX event handling ─────────────────────────────────────────────────────────
void handleRadioEvent() {
    static Packet pkt;  // BSS, not on the task stack

    s_lastRadioEventMs = millis();
    int16_t err = s_radio->readPacket(pkt);
    if (err == ERR_SPURIOUS_IRQ) {
        bumpCounter(&Stats::rxSpuriousIrqCount);
        return;
    }
    if (err != RADIOLIB_ERR_NONE || pkt.len < 1) {
        const uint16_t irq = s_radio->lastIrqStatus();
        if (err == ERR_RX_TIMEOUT || (irq & RADIOLIB_SX128X_IRQ_RX_TX_TIMEOUT)) {
            bumpCounter(&Stats::rxTimeoutCount);
        } else if (err == ERR_SYNCWORD || (irq & RADIOLIB_SX128X_IRQ_SYNC_WORD_ERROR)) {
            bumpCounter(&Stats::rxSyncWordErrorCount);
        } else if (err == RADIOLIB_ERR_CRC_MISMATCH || (irq & RADIOLIB_SX128X_IRQ_CRC_ERROR)) {
            bumpCounter(&Stats::rxCrcErrorCount);
        } else if (err == ERR_HEADER || (irq & RADIOLIB_SX128X_IRQ_HEADER_ERROR)) {
            bumpCounter(&Stats::rxHeaderErrorCount);
        } else if (err == ERR_INVALID_PACKET_LEN || (err == RADIOLIB_ERR_NONE && pkt.len < 1)) {
            bumpCounter(&Stats::rxInvalidLengthCount);
        } else {
            bumpCounter(&Stats::rxReadDataErrorCount);
        }
        noteRadioRxError();
        return;
    }

    if (isGenericTransport()) {
        handleV3RadioPacket(pkt);
        return;
    }

    // ── CONTROL packet dispatch ──────────────────────────────────────────────
    if (framingPacketType(pkt) == LinkPacketType::CONTROL) {
        noteLinkActivity();
        ControlFrame ctrl;
        if (!framingParseControl(pkt, ctrl)) {
            noteControlMalformed();
            return;
        }
        if (ctrl.type == ControlType::HEARTBEAT) {
            bumpCounter(&Stats::controlHeartbeatRx);
            ControlFrame ackCtrl;
            ackCtrl.type = ControlType::HEARTBEAT_ACK;
            ackCtrl.seq  = ctrl.seq;
            Packet ackPkt;
            framingBuildControlPacket(ackPkt, ackCtrl);
            if (s_radio->transmit(ackPkt, true) == RADIOLIB_ERR_NONE) {
                bumpCounter(&Stats::controlHeartbeatAckTx);
                noteLinkActivity();
            } else {
                noteRadioTxError();
            }
        } else if (ctrl.type == ControlType::DATA_PENDING) {
            bumpCounter(&Stats::controlDataPendingRx);
            ControlFrame ready;
            ready.type = ControlType::DATA_READY;
            ready.seq  = ctrl.seq;
            Packet readyPkt;
            framingBuildControlPacket(readyPkt, ready);
            if (s_radio->transmit(readyPkt, true) == RADIOLIB_ERR_NONE) {
                bumpCounter(&Stats::controlDataReadyTx);
                noteLinkActivity();
            } else {
                noteRadioTxError();
            }
        } else if (ctrl.type == ControlType::HEARTBEAT_ACK) {
            if (s_tx.phase == TxPhase::HB_WAIT && ctrl.seq == s_tx.hb_seq) {
                bumpCounter(&Stats::controlHeartbeatAckRx);
                noteBidirectionalControl();
                refreshLinkStats();
                postFrame(millis());
            }
        } else if (ctrl.type == ControlType::DATA_READY) {
            bumpCounter(&Stats::controlDataReadyRx);
        }
        return;
    }

    if (framingPacketType(pkt) == LinkPacketType::ACK) {
        AckFrame ack;
        if (!framingParseAck(pkt, ack)) {
            noteMalformedAck();
            noteRadioRxError();
            return;
        }
        notePayloadActivity();
        bumpCounter(&Stats::arqAckRxCount);
        handleAckForTx(ack);
        return;
    }

    // ── Native single-packet RX path ─────────────────────────────────────────
    if (framingPacketType(pkt) == LinkPacketType::NATIVE) {
        notePayloadActivity();
        uint8_t payload_len = 0;
        if (!framingParseNativePayload(pkt, payload_len)) {
            noteMalformedData();
            noteRadioRxError();
            return;
        }
        static PayloadFrame rxFrame;  // BSS, not on the task stack
        rxFrame.len  = payload_len;
        rxFrame.rssi = pkt.rssi;
        if (payload_len > 0) {
            memcpy(rxFrame.data, pkt.data + FRAMING_NATIVE_HDR_LEN, payload_len);
        }
        {
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().nativeRxCount++;
            sm.get().rxCount++;
            sm.get().rxBytes   += payload_len;
            sm.get().rssi       = rxFrame.rssi;
            sm.get().radioState = RadioState::RX;
            sm.unlock();
        }
        if (xQueueSend(s_rxQueue, &rxFrame, pdMS_TO_TICKS(RX_QUEUE_TIMEOUT_MS)) != pdPASS) {
            noteQueueDrop();
            noteRadioError();
        }
        return;
    }

    // ── Generic fragmented ARQ RX path ───────────────────────────────────────
    // In native mode, only NATIVE-type packets are expected. A DATA packet
    // here means the remote node is in generic mode — discard rather than
    // running ARQ reassembly.
    if (s_radio->config().transportMode == TransportMode::NATIVE_PACKET) {
        noteRadioRxError();
        return;
    }

    DataFrameHeader dataHdr;
    if (!framingParseDataHeader(pkt, dataHdr)) {
        noteMalformedData();
        noteRadioRxError();
        return;
    }

    if (!framingValidateDataFragment(dataHdr)) {
        bumpCounter(&Stats::arqFragmentMetadataDrops);
        return;
    }

    const uint16_t seq          = dataHdr.seq;
    const uint8_t  idx          = dataHdr.frag_index;
    const uint8_t  total_frags  = dataHdr.total_frags;
    const uint8_t  frag_data_len = dataHdr.payload_len;
    const uint16_t frame_len    = dataHdr.frame_len;
    const uint32_t frame_crc32  = dataHdr.frame_crc32;
    const bool     round_end    = dataHdr.round_end;
    const bool     warmup       = dataHdr.warmup;
    const uint32_t now_ms       = millis();

    if (s_completed.seq == seq && (now_ms - s_completed.tick_ms) <= RADIO_DUP_CACHE_MS) {
        if (s_completed.total_frags == total_frags &&
            s_completed.frame_len == frame_len &&
            s_completed.frame_crc32 == frame_crc32 &&
            s_completed.warmup == warmup) {
            ARQ_LOG("[ARQ RX:DUP seq=%04x tot=%u mask=%08lx]\n",
                    seq, total_frags, static_cast<unsigned long>(s_completed.ack_mask));
            // Re-ACK without re-delivering.
            AckFrame dupAck;
            dupAck.seq           = s_completed.seq;
            dupAck.total_frags   = s_completed.total_frags;
            dupAck.window_base   = 0;
            dupAck.received_mask = s_completed.ack_mask;
            bumpCounter(&Stats::arqDuplicateSuppressed);
            if (sendAckPacket(dupAck)) {
                notePayloadActivity();
            }
        } else {
            bumpCounter(&Stats::arqFragmentMetadataDrops);
        }
        return;
    }

    if (s_ra.seq != FRAMING_SEQ_UNSET &&
        (now_ms - s_ra.last_tick_ms > reassemblyTimeoutMs(s_ra.total_frags))) {
        ARQ_LOG("[ARQ RX:DROP seq=%04x tot=%u mask=%08lx reason=timeout]\n",
                s_ra.seq, s_ra.total_frags, static_cast<unsigned long>(s_ra.received_mask));
        noteReassemblyDrop();
        s_ra.reset();
    }

    if (s_ra.seq != seq) {
        if (s_ra.seq != FRAMING_SEQ_UNSET) {
            noteIdentityReset();
            noteReassemblyDrop();
        }
        s_ra.reset();
        s_ra.seq         = seq;
        s_ra.total_frags = total_frags;
        s_ra.frame_len   = frame_len;
        s_ra.frame_crc32 = frame_crc32;
        s_ra.warmup      = warmup;
    } else if (s_ra.total_frags != total_frags ||
               s_ra.frame_len != frame_len ||
               s_ra.frame_crc32 != frame_crc32 ||
               s_ra.warmup != warmup) {
        bumpCounter(&Stats::arqFragmentMetadataDrops);
        return;
    }

    const uint32_t bit        = 1u << idx;
    const bool    is_new_frag = (s_ra.received_mask & bit) == 0;

    if (is_new_frag) {
        notePayloadActivity();
        memcpy(s_ra.buf + idx * FRAMING_FRAG_DATA,
               pkt.data + FRAMING_DATA_HDR_LEN, frag_data_len);
        s_ra.frag_len[idx]  = frag_data_len;
        s_ra.received_mask |= bit;
        s_ra.last_tick_ms   = now_ms;
        s_ra.last_rssi      = pkt.rssi;
        s_ra.ack_pending    = true;
        s_ra.ack_due_ms     = now_ms + ackFallbackDelayMs(total_frags);
    }

    ARQ_LOG("[ARQ RX:FRAG seq=%04x tot=%u idx=%u new=%d re=%d mask=%08lx flen=%u]\n",
            seq, total_frags, idx, (int)is_new_frag, (int)round_end,
            static_cast<unsigned long>(s_ra.received_mask), frag_data_len);

    if (round_end) {
        // Sender signals end of its TX burst. Schedule a short turnaround
        // delay before ACKing so the remote side has time to switch back
        // into RX after the last fragment of a multi-fragment burst.
        s_ra.ack_pending  = true;
        s_ra.last_tick_ms = now_ms;
        s_ra.ack_due_ms   = now_ms + ackTurnaroundDelayMs(total_frags);
    }
}

// ── Periodic services ─────────────────────────────────────────────────────────
void serviceRx(uint32_t now) {
    if (isGenericTransport()) {
        (void)now;
        return;
    }
    if (s_ra.seq == FRAMING_SEQ_UNSET) {
        return;
    }
    if (s_ra.ack_pending) {
        if (deadlinePassed(now, s_ra.ack_due_ms)) {
            handleAckDue(now);
        }
    } else if (!s_ra.isComplete()) {
        if (now - s_ra.last_tick_ms >= reassemblyTimeoutMs(s_ra.total_frags)) {
            ARQ_LOG("[ARQ RX:DROP seq=%04x tot=%u mask=%08lx reason=idle]\n",
                    s_ra.seq, s_ra.total_frags,
                    static_cast<unsigned long>(s_ra.received_mask));
            noteReassemblyDrop();
            s_ra.reset();
        }
    }
}

void serviceTx(uint32_t now) {
    if (isGenericTransport()) {
        if (s_tx.phase == TxPhase::HB_WAIT) {
            if (deadlinePassed(now, s_tx.deadline_ms)) {
                refreshLinkStats();
                postFrame(now);
            }
            return;
        }
        s_arq.onTick(now);
        syncArqCounters();
        return;
    }

    switch (s_tx.phase) {
    case TxPhase::IDLE:
        break;

    case TxPhase::LBT_BACKOFF:
        if (deadlinePassed(now, s_tx.deadline_ms)) {
            s_tx.lbt_tries++;
            if (s_tx.lbt_tries >= RADIO_LBT_MAX_RETRIES || !s_radio->isChannelBusy()) {
                dispatchFrame(now);
            } else {
                s_tx.deadline_ms = now + lbtBackoffMs();
            }
        }
        break;

    case TxPhase::WARMUP_WAIT:
        if (deadlinePassed(now, s_tx.deadline_ms)) {
            bumpCounter(&Stats::arqWarmupTimeouts);
            startDataRounds(now);
        }
        break;

    case TxPhase::SENDING:
        if (deadlinePassed(now, s_tx.deadline_ms)) {
            sendNextFragment(now);
        }
        break;

    case TxPhase::WAIT_ACK:
        if (deadlinePassed(now, s_tx.deadline_ms)) {
            bumpCounter(&Stats::arqAckTimeoutCount);
            ARQ_LOG("[ARQ TX:TIMEOUT seq=%04x tot=%u round=%u pending=%08lx]\n",
                    s_tx.seq, s_tx.total_frags, s_tx.round,
                    static_cast<unsigned long>(s_tx.pending_mask));
            nextRoundOrFail(now);
        }
        break;

    case TxPhase::HB_WAIT:
        if (deadlinePassed(now, s_tx.deadline_ms)) {
            refreshLinkStats();
            postFrame(now);
        }
        break;
    }
}

void executeStagedCommand() {
    MacCommand* cmd = s_stagedCmd;
    s_stagedCmd = nullptr;
    switch (cmd->type) {
    case MacCommand::Type::APPLY_CONFIG:
        cmd->result = s_radio->applyConfig(cmd->cfg);
        if (cmd->result == RADIOLIB_ERR_NONE) {
            s_ra.reset();
            s_tx = TxState{};
            resetArqEngine();
        }
        break;
    case MacCommand::Type::SCAN_BAND:
        cmd->scanCount = s_radio->scanBand(cmd->startMHz, cmd->stopMHz,
                                           cmd->stepMHz, cmd->dwellUs,
                                           cmd->rssiOut, cmd->maxN);
        cmd->result = RADIOLIB_ERR_NONE;
        break;
    }
    xSemaphoreGive(s_cmdDone);
}

void serviceIdle(uint32_t now) {
    // Housekeeping: task telemetry and link-state refresh once per second.
    if (now - s_lastHousekeepMs >= 1000) {
        s_lastHousekeepMs = now;
        const uint32_t hwm = uxTaskGetStackHighWaterMark(nullptr);
        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().macStackHwm = hwm;
        sm.unlock();
        refreshLinkStats();
        if (isGenericTransport()) {
            refreshArqDebugStats(now);
        }
    }

    if (s_tx.phase != TxPhase::IDLE) {
        return;
    }

    if (s_stagedCmd != nullptr) {
        executeStagedCommand();
        return;
    }

    if (isGenericTransport()) {
        const uint8_t tx_slots_used =
            static_cast<uint8_t>(s_arq.txActiveCount() + s_arq.txQueuedCount());
        if (tx_slots_used < arq::ARQ_MAX_OUTSTANDING &&
            xQueueReceive(s_txQueue, &s_tx.frame, 0) == pdPASS) {
            if (s_tx.frame.len == 0 || s_tx.frame.len > TNC_PAYLOAD_MAX_LEN) {
                noteRadioError();
                return;
            }
            const arq::ArqResult result = s_arq.onTxDatagram(s_tx.frame.data, s_tx.frame.len);
            if (result == arq::ArqResult::OK) {
                bumpCounter(&Stats::arqFramesStarted);
            } else {
                noteQueueDrop();
                noteRadioError();
            }
            syncArqCounters();
            return;
        }

        if (s_arq.hasPendingWork()) {
            return;
        }

        if (deadlinePassed(now, s_nextHbMs)) {
            // Gate on payload idleness, not general link activity: the peer's
            // own heartbeats otherwise defer ours and the two boards can
            // mutually starve their schedules after a transfer.
            if (isPayloadIdle()) {
                startHeartbeat(now);
            } else {
                s_nextHbMs = now + hbIntervalMs();
            }
            return;
        }

        if (now - s_lastRadioEventMs >= RADIO_REARM_IDLE_MS) {
            s_lastRadioEventMs = now;
            s_radio->startReceive();
        }
        return;
    }

    if (xQueueReceive(s_txQueue, &s_tx.frame, 0) == pdPASS) {
        startFrame(now);
        return;
    }

    if (deadlinePassed(now, s_nextHbMs)) {
        if (isPayloadIdle()) {
            startHeartbeat(now);
        } else {
            s_nextHbMs = now + hbIntervalMs();
        }
        return;
    }

    // Defensive RX re-arm: only when the MAC is fully idle (no frame in
    // flight, no active reassembly) so an in-flight packet can never be
    // destroyed by the RX state reset.
    if (s_ra.seq == FRAMING_SEQ_UNSET &&
        now - s_lastRadioEventMs >= RADIO_REARM_IDLE_MS) {
        s_lastRadioEventMs = now;
        s_radio->startReceive();
    }
}

// Shortest time until the next deadline, capped so the watchdog is always fed.
uint32_t computeWaitMs(uint32_t now) {
    uint32_t wait = MAC_MAX_WAIT_MS;
    auto consider = [&](uint32_t due) {
        int32_t delta = static_cast<int32_t>(due - now);
        if (delta < 0) delta = 0;
        if (static_cast<uint32_t>(delta) < wait) wait = static_cast<uint32_t>(delta);
    };

    if (isGenericTransport()) {
        uint32_t arq_deadline = 0;
        if (s_arq.nextDeadline(now, arq_deadline)) {
            consider(arq_deadline);
        }
        if (s_tx.phase != TxPhase::IDLE) {
            consider(s_tx.deadline_ms);
        } else if (s_stagedCmd != nullptr) {
            wait = 0;
        } else if (uxQueueMessagesWaiting(s_txQueue) > 0 &&
                   (s_arq.txActiveCount() + s_arq.txQueuedCount()) < arq::ARQ_MAX_OUTSTANDING) {
            wait = 0;
        } else if (!s_arq.hasPendingWork()) {
            consider(s_nextHbMs);
        }
        return wait;
    }

    if (s_ra.seq != FRAMING_SEQ_UNSET) {
        if (s_ra.ack_pending) {
            consider(s_ra.ack_due_ms);
        } else if (!s_ra.isComplete()) {
            consider(s_ra.last_tick_ms + reassemblyTimeoutMs(s_ra.total_frags));
        }
    }

    if (s_tx.phase != TxPhase::IDLE) {
        consider(s_tx.deadline_ms);
    } else if (s_stagedCmd != nullptr || uxQueueMessagesWaiting(s_txQueue) > 0) {
        wait = 0;  // work is ready; do not sleep on it
    } else {
        consider(s_nextHbMs);
    }
    return wait;
}

void macTask(void*) {
    // Watchdog supervision: loop waits are capped at MAC_MAX_WAIT_MS and the
    // longest in-loop operation (band scan) finishes in ~2 s, both well
    // inside the timeout.
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_task_wdt_config_t wdtCfg = {};
    wdtCfg.timeout_ms    = MAC_WDT_TIMEOUT_S * 1000u;
    wdtCfg.idle_core_mask = 0;
    wdtCfg.trigger_panic = true;
    if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) {
        esp_task_wdt_init(&wdtCfg);
    }
#else
    esp_task_wdt_init(MAC_WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(nullptr);

    s_radio->startReceive();
    s_nextHbMs = millis() + hbIntervalMs();
    resetArqEngine();

    for (;;) {
        esp_task_wdt_reset();

        const uint32_t waitMs = computeWaitMs(millis());
        QueueSetMemberHandle_t member =
            xQueueSelectFromSet(s_waitSet, pdMS_TO_TICKS(waitMs));

        if (member == reinterpret_cast<QueueSetMemberHandle_t>(s_radio->rxSemaphore)) {
            xSemaphoreTake(s_radio->rxSemaphore, 0);
            handleRadioEvent();
        } else if (member == reinterpret_cast<QueueSetMemberHandle_t>(s_txWakeSema)) {
            xSemaphoreTake(s_txWakeSema, 0);
        } else if (member == reinterpret_cast<QueueSetMemberHandle_t>(s_cmdQueue)) {
            MacCommand* cmd = nullptr;
            if (xQueueReceive(s_cmdQueue, &cmd, 0) == pdPASS) {
                s_stagedCmd = cmd;
            }
        }

        const uint32_t now = millis();
        serviceRx(now);
        serviceTx(now);
        serviceIdle(now);
    }
}

}  // namespace

// ── Public API ────────────────────────────────────────────────────────────────
bool start(const Init& init) {
    s_radio   = init.radio;
    s_txQueue = init.txQueue;
    s_rxQueue = init.rxQueue;
    if (s_radio == nullptr || s_txQueue == nullptr || s_rxQueue == nullptr) {
        return false;
    }

    s_txWakeSema = xSemaphoreCreateCounting(TX_QUEUE_DEPTH, 0);
    s_cmdQueue   = xQueueCreate(1, sizeof(MacCommand*));
    s_cmdGate    = xSemaphoreCreateMutex();
    s_cmdDone    = xSemaphoreCreateBinary();
    // Set size = sum of event capacities of all members.
    s_waitSet    = xQueueCreateSet(16 + TX_QUEUE_DEPTH + 1);
    if (s_txWakeSema == nullptr || s_cmdQueue == nullptr ||
        s_cmdGate == nullptr || s_cmdDone == nullptr || s_waitSet == nullptr) {
        return false;
    }

    // rxSemaphore must be empty here: the radio is not yet in RX mode
    // (startReceive happens inside macTask), so no DIO1 events have fired.
    if (xQueueAddToSet(s_radio->rxSemaphore, s_waitSet) != pdPASS ||
        xQueueAddToSet(s_txWakeSema, s_waitSet) != pdPASS ||
        xQueueAddToSet(s_cmdQueue, s_waitSet) != pdPASS) {
        return false;
    }

    return xTaskCreatePinnedToCore(macTask, "mac", STACK_MAC, nullptr,
                                   PRIO_RADIO, nullptr, 1) == pdPASS;
}

void notifyTxWork() {
    if (s_txWakeSema != nullptr) {
        xSemaphoreGive(s_txWakeSema);  // full semaphore = wake already pending
    }
}

int16_t requestApplyConfig(const ModemConfig& cfg) {
    if (s_cmdGate == nullptr) {
        return ERR_MAC_NOT_STARTED;
    }
    xSemaphoreTake(s_cmdGate, portMAX_DELAY);
    s_cmd = MacCommand{};
    s_cmd.type = MacCommand::Type::APPLY_CONFIG;
    s_cmd.cfg  = cfg;
    MacCommand* p = &s_cmd;
    xQueueSend(s_cmdQueue, &p, portMAX_DELAY);
    xSemaphoreTake(s_cmdDone, portMAX_DELAY);
    const int16_t result = s_cmd.result;
    xSemaphoreGive(s_cmdGate);
    return result;
}

uint8_t requestScanBand(float startMHz, float stopMHz, float stepMHz,
                        uint32_t dwellUs, int8_t* rssiOut, uint8_t maxN) {
    if (s_cmdGate == nullptr) {
        return 0;
    }
    xSemaphoreTake(s_cmdGate, portMAX_DELAY);
    s_cmd = MacCommand{};
    s_cmd.type     = MacCommand::Type::SCAN_BAND;
    s_cmd.startMHz = startMHz;
    s_cmd.stopMHz  = stopMHz;
    s_cmd.stepMHz  = stepMHz;
    s_cmd.dwellUs  = dwellUs;
    s_cmd.rssiOut  = rssiOut;
    s_cmd.maxN     = maxN;
    MacCommand* p = &s_cmd;
    xQueueSend(s_cmdQueue, &p, portMAX_DELAY);
    xSemaphoreTake(s_cmdDone, portMAX_DELAY);
    const uint8_t count = s_cmd.scanCount;
    xSemaphoreGive(s_cmdGate);
    return count;
}

void refreshLinkStats() {
    const bool ready = isLinkReady();
    const uint32_t age_ms = g_link_ever_confirmed
        ? (millis() - g_last_bidirectional_ctrl_ms)
        : 0xFFFFFFFFu;
    // READY → PROBING → DOWN with hysteresis: a confirmation gap past the
    // READY TTL means heartbeats are being attempted, not that the link is
    // gone; only a gap past the probe TTL demotes to DOWN.
    LinkState state = LinkState::DOWN;
    if (g_link_ever_confirmed) {
        if (ready) {
            state = LinkState::READY;
        } else if (age_ms <= RADIO_LINK_PROBE_TTL_MS) {
            state = LinkState::PROBING;
        }
    }
    auto& sm = StatsManager::instance();
    sm.lock();
    const bool was_ready = (sm.get().linkReady != 0);
    sm.get().linkReady = ready ? 1u : 0u;
    sm.get().linkState = static_cast<uint8_t>(state);
    sm.get().linkAgeMs = age_ms;
    if (was_ready && !ready) {
        sm.get().linkDownTransitions++;
    }
    sm.unlock();
}

}  // namespace mac
