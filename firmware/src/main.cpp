#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "config.h"
#include "radio/Radio.h"
#include "kiss/Kiss.h"
#include "framing/Framing.h"
#include "display/Display.h"
#include "stats/Stats.h"

#if SERIAL_CONSOLE_LOGS
#define BOOT_LOG(...) Serial.printf(__VA_ARGS__)
#define BOOT_LOG_LN(msg) Serial.println(msg)
#else
#define BOOT_LOG(...) ((void)0)
#define BOOT_LOG_LN(msg) ((void)0)
#endif

// Set to 1 to log decoded KISS frame length and first 8 bytes on serial RX
#define DEBUG_KISS_SERIAL_RX 0
// Set to 1 to log ARQ RX/TX diagnostics on the serial console
#define DEBUG_ARQ_DIAGNOSTICS 0

#if DEBUG_ARQ_DIAGNOSTICS
#define ARQ_LOG(...) Serial.printf(__VA_ARGS__)
#else
#define ARQ_LOG(...) ((void)0)
#endif

// ── Globals ───────────────────────────────────────────────────────────────────
static Radio   radio;
static Display display;

static QueueHandle_t txQueue;   // IpFrame: SerialRX  → RadioTX
static QueueHandle_t rxQueue;   // IpFrame: RadioRX   → SerialTX
static QueueHandle_t ackQueue;  // AckFrame: RadioRX  → RadioTX

struct CompletedFrameCache {
    uint16_t seq         = FRAMING_SEQ_UNSET;
    uint8_t  total_frags = 0;
    uint8_t  ack_mask    = 0;
    uint32_t tick_ms     = 0;
};

static uint32_t ackFallbackDelayMs(uint8_t total_frags) {
    return RADIO_ACK_FALLBACK_DELAY_MS +
           static_cast<uint32_t>(total_frags > 0 ? (total_frags - 1u) * 2u : 0u);
}

static uint32_t ackTurnaroundDelayMs(uint8_t total_frags) {
    return RADIO_ACK_TURNAROUND_DELAY_MS +
           static_cast<uint32_t>(total_frags > 0 ? (total_frags - 1u) : 0u);
}

static uint32_t ackTimeoutMs(uint8_t total_frags) {
    return RADIO_ACK_TIMEOUT_MS +
           static_cast<uint32_t>(total_frags > 0 ? (total_frags - 1u) * RADIO_INTER_FRAG_DELAY_MS : 0u);
}

static uint32_t reassemblyTimeoutMs(uint8_t total_frags) {
    return RADIO_REASSEMBLY_TIMEOUT_MS +
           static_cast<uint32_t>(total_frags > 0 ? (total_frags - 1u) * RADIO_INTER_FRAG_DELAY_MS * 2u : 0u);
}

static void noteRadioError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().errorCount++;
    sm.get().radioState = RadioState::ERROR;
    sm.unlock();
}

static void noteQueueDrop() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqQueueDrops++;
    sm.unlock();
}

static void noteIdentityReset() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqIdentityResets++;
    sm.unlock();
}

static void noteRadioTxError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().radioTxErrors++;
    sm.unlock();
    noteRadioError();
}

static void noteRadioRxError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().radioRxErrors++;
    sm.get().lastRadioErr     = radio.lastRadioErr();
    sm.get().lastIrqStatus    = radio.lastIrqStatus();
    sm.get().lastPacketLength = radio.lastPacketLength();
    sm.unlock();
    noteRadioError();
}

static void noteRadioRxSpuriousIrq() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxSpuriousIrqCount++;
    sm.unlock();
}

static void noteRadioRxInvalidLength() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxInvalidLengthCount++;
    sm.unlock();
}

static void noteRadioRxReadDataError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxReadDataErrorCount++;
    sm.unlock();
}

static void noteMalformedAck() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxMalformedAckCount++;
    sm.unlock();
}

static void noteMalformedData() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxMalformedDataCount++;
    sm.unlock();
}

static void noteAckQueueDrop() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqAckQueueDrops++;
    sm.unlock();
    noteQueueDrop();
}

static bool ipv4FrameLooksComplete(const IpFrame& frame) {
    if (frame.len < 20) {
        return false;
    }
    const uint8_t version = static_cast<uint8_t>(frame.data[0] >> 4);
    const uint8_t ihl = static_cast<uint8_t>((frame.data[0] & 0x0Fu) * 4u);
    if (version != 4 || ihl < 20 || frame.len < ihl) {
        return false;
    }
    const uint16_t total_len = static_cast<uint16_t>((static_cast<uint16_t>(frame.data[2]) << 8) |
                                                     static_cast<uint16_t>(frame.data[3]));
    return total_len >= ihl && total_len <= frame.len;
}

static void finalizeReassembly(Reassembler& ra, CompletedFrameCache& completed) {
    IpFrame frame;
    frame.len = 0;
    for (uint8_t i = 0; i < ra.total_frags; i++) {
        ARQ_LOG("[ARQ RX:ASSEMBLE seq=%04x i=%u flen=%u]\n",
                ra.seq, i, ra.frag_len[i]);
        memcpy(frame.data + frame.len,
               ra.buf + i * FRAMING_FRAG_DATA,
               ra.frag_len[i]);
        frame.len += ra.frag_len[i];
    }
    frame.rssi = ra.last_rssi;

    if (!ipv4FrameLooksComplete(frame)) {
        ARQ_LOG("[ARQ RX:FAIL seq=%04x tot=%u mask=%02x reason=ipv4_check len=%u]\n",
                ra.seq, ra.total_frags, ra.received_mask, frame.len);
        noteRadioError();
        ra.reset();
        return;
    }

    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxCount++;
    sm.get().rxBytes   += frame.len;
    sm.get().rssi       = frame.rssi;
    sm.get().radioState = RadioState::RX;
    sm.get().arqFramesCompleted++;
    sm.unlock();

    if (xQueueSend(rxQueue, &frame, pdMS_TO_TICKS(RX_QUEUE_TIMEOUT_MS)) != pdPASS) {
        noteQueueDrop();
        noteRadioError();
    } else {
        // Any successful bounded send could still have waited; count a possible
        // backpressure point if the queue is full immediately after enqueue.
        if (uxQueueSpacesAvailable(rxQueue) == 0) {
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().rxQueueWaitCount++;
            sm.unlock();
        }
    }
    ARQ_LOG("[ARQ RX:DONE seq=%04x tot=%u mask=%02x len=%u]\n",
            ra.seq, ra.total_frags, ra.received_mask, frame.len);
    completed.seq         = ra.seq;
    completed.total_frags = ra.total_frags;
    completed.ack_mask    = ra.received_mask;
    completed.tick_ms     = millis();
    ra.reset();
}

static void sendAckForReassembly(Reassembler& ra) {
    if (ra.seq == FRAMING_SEQ_UNSET || ra.total_frags == 0 || !ra.ack_pending) {
        return;
    }

    AckFrame ack;
    ack.seq           = ra.seq;
    ack.total_frags   = ra.total_frags;
    ack.received_mask = ra.received_mask;

    ARQ_LOG("[ARQ RX:ACK seq=%04x tot=%u mask=%02x complete=%d]\n",
            ra.seq, ra.total_frags, ra.received_mask, (int)ra.isComplete());

    Packet pkt;
    framingBuildAckPacket(pkt, ack);

    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqAckTxCount++;
    sm.unlock();

    int16_t err = radio.transmit(pkt, true);
    if (err != RADIOLIB_ERR_NONE) {
        sm.lock();
        sm.get().arqAckTxErrors++;
        sm.unlock();
        noteRadioTxError();
        return;
    }

    ra.ack_pending = false;
}

static void noteReassemblyDrop() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqReassemblyDrops++;
    sm.unlock();
}

// ── Task: Radio RX ────────────────────────────────────────────────────────────
// Receives radio packets, reassembles fragments into IP frames, pushes to rxQueue.
static void radioRxTask(void*) {
    Packet pkt;
    static Reassembler ra;   // static: lives in BSS, not on the task stack
    static CompletedFrameCache completed;

    for (;;) {
        uint32_t wait_ms = 30000;
        if (ra.seq != FRAMING_SEQ_UNSET) {
            uint32_t idle_ms = millis() - ra.last_tick_ms;
            const uint32_t reassembly_ms = reassemblyTimeoutMs(ra.total_frags);
            if (ra.ack_pending) {
                const uint32_t now_ms = millis();
                if (now_ms >= ra.ack_due_ms) {
                    sendAckForReassembly(ra);
                    if (!ra.ack_pending && ra.isComplete()) {
                        finalizeReassembly(ra, completed);
                    }
                    continue;
                }
                wait_ms = ra.ack_due_ms - now_ms;
            } else if (!ra.isComplete() && idle_ms >= reassembly_ms) {
                noteReassemblyDrop();
                ra.reset();
                continue;
            } else if (!ra.isComplete()) {
                wait_ms = reassembly_ms - idle_ms;
            }
        }

        BaseType_t got = xSemaphoreTake(radio.rxSemaphore, pdMS_TO_TICKS(wait_ms));
        if (got == pdFALSE) {
            if (ra.seq != FRAMING_SEQ_UNSET && ra.ack_pending) {
                sendAckForReassembly(ra);
                if (!ra.ack_pending && ra.isComplete()) {
                    finalizeReassembly(ra, completed);
                }
                continue;
            }
            if (ra.seq != FRAMING_SEQ_UNSET && !ra.isComplete()) {
                ARQ_LOG("[ARQ RX:DROP seq=%04x tot=%u mask=%02x reason=idle]\n",
                        ra.seq, ra.total_frags, ra.received_mask);
                noteReassemblyDrop();
                ra.reset();
                continue;
            }
            radio.startReceive();
            continue;
        }

        int16_t err = radio.readPacket(pkt);
        if (err == ERR_SPURIOUS_IRQ) {
            // Spurious interrupt caught and handled. No action needed, do not increment error counts.
            noteRadioRxSpuriousIrq();
            continue;
        }
        if (err != RADIOLIB_ERR_NONE || pkt.len < 1) {
            if (err == ERR_INVALID_PACKET_LEN || pkt.len < 1) {
                noteRadioRxInvalidLength();
            } else {
                noteRadioRxReadDataError();
            }
            noteRadioRxError();
            continue;
        }

        if (framingPacketType(pkt) == LinkPacketType::ACK) {
            AckFrame ack;
            if (!framingParseAck(pkt, ack)) {
                noteMalformedAck();
                noteRadioRxError();
                continue;
            }
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().arqAckRxCount++;
            sm.unlock();
            if (xQueueSend(ackQueue, &ack, 0) != pdPASS) {
                noteAckQueueDrop();
                noteRadioError();
            }
            continue;
        }

        const uint16_t seq          = framingPacketSeq(pkt);
        const uint8_t idx           = framingFragmentIndex(pkt);
        const uint8_t total_frags   = framingTotalFrags(pkt);
        const uint8_t frag_data_len = framingPayloadLen(pkt);
        const bool    round_end     = framingIsRoundEnd(pkt);
        const uint32_t now_ms       = millis();

        if (total_frags == 0 || total_frags > FRAMING_MAX_FRAGS ||
            idx >= total_frags || frag_data_len > FRAMING_FRAG_DATA) {
            noteMalformedData();
            noteRadioRxError();
            continue;
        }

        if (completed.seq == seq &&
            completed.total_frags == total_frags &&
            (now_ms - completed.tick_ms) <= RADIO_DUP_CACHE_MS) {
            ARQ_LOG("[ARQ RX:DUP seq=%04x tot=%u mask=%02x]\n",
                    seq, total_frags, completed.ack_mask);
            // Re-ACK without re-delivering. Use AckFrame + Packet directly to
            // avoid a large Reassembler on the task stack.
            AckFrame dupAck;
            dupAck.seq           = completed.seq;
            dupAck.total_frags   = completed.total_frags;
            dupAck.received_mask = completed.ack_mask;
            Packet ackPkt;
            framingBuildAckPacket(ackPkt, dupAck);
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().arqDuplicateSuppressed++;
            sm.get().arqAckTxCount++;
            sm.unlock();
            if (radio.transmit(ackPkt, true) != RADIOLIB_ERR_NONE) {
                sm.lock();
                sm.get().arqAckTxErrors++;
                sm.unlock();
                noteRadioTxError();
            }
            continue;
        }

        if (ra.seq != FRAMING_SEQ_UNSET &&
            (now_ms - ra.last_tick_ms > reassemblyTimeoutMs(ra.total_frags))) {
            ARQ_LOG("[ARQ RX:DROP seq=%04x tot=%u mask=%02x reason=timeout]\n",
                    ra.seq, ra.total_frags, ra.received_mask);
            noteReassemblyDrop();
            ra.reset();
        }

        if (ra.seq != seq) {
            if (ra.seq != FRAMING_SEQ_UNSET) {
                noteIdentityReset();
                noteReassemblyDrop();
            }
            ra.reset();
            ra.seq         = seq;
            ra.total_frags = total_frags;
        } else if (ra.total_frags != total_frags) {
            noteIdentityReset();
            noteReassemblyDrop();
            ra.reset();
            ra.seq         = seq;
            ra.total_frags = total_frags;
        }

        const uint8_t bit          = static_cast<uint8_t>(1u << idx);
        const bool    is_new_frag  = !(ra.received_mask & bit);

        if (is_new_frag) {
            memcpy(ra.buf + idx * FRAMING_FRAG_DATA, pkt.data + FRAMING_DATA_HDR_LEN, frag_data_len);
            ra.frag_len[idx]  = frag_data_len;
            ra.received_mask |= bit;
            ra.last_tick_ms   = now_ms;
            ra.last_rssi      = pkt.rssi;
            ra.ack_pending    = true;
            ra.ack_due_ms     = now_ms + ackFallbackDelayMs(total_frags);
        }

        ARQ_LOG("[ARQ RX:FRAG seq=%04x tot=%u idx=%u new=%d re=%d mask=%02x flen=%u]\n",
                seq, total_frags, idx, (int)is_new_frag, (int)round_end,
                ra.received_mask, frag_data_len);

        if (round_end) {
            // Sender signals end of its TX burst. Schedule a short turnaround
            // delay before ACKing so the remote side has time to switch back
            // into RX after the last fragment of a multi-fragment burst.
            ra.ack_pending = true;
            ra.last_tick_ms = now_ms;
            ra.ack_due_ms = now_ms + ackTurnaroundDelayMs(total_frags);
        }
    }
}

// ── Task: Radio TX ────────────────────────────────────────────────────────────
// Dequeues IP frames, fragments into ≤123-byte radio packets, retransmits missing
// fragments using selective-repeat ARQ until the receiver ACK bitmap is complete.
static void radioTxTask(void*) {
    IpFrame frame;
    static uint16_t seq = 0;

    for (;;) {
        xQueueReceive(txQueue, &frame, portMAX_DELAY);

        xQueueReset(ackQueue);
        seq++;
        if (seq == FRAMING_SEQ_UNSET) {
            seq++;
        }
        const uint8_t total_frags = static_cast<uint8_t>(
            (frame.len + FRAMING_FRAG_DATA - 1) / FRAMING_FRAG_DATA);
        uint8_t pending_mask = framingExpectedMask(total_frags);
        bool delivered = false;

        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().arqFramesStarted++;
        sm.unlock();

        ARQ_LOG("[ARQ TX:START seq=%04x tot=%u len=%u]\n",
                seq, total_frags, frame.len);

        // LBT-CSMA: sense channel before the first fragment only.
        for (int lbt = 0; radio.isChannelBusy(); lbt++) {
            if (lbt >= RADIO_LBT_MAX_RETRIES) break;
            uint32_t backoff_ms = RADIO_LBT_BACKOFF_MIN_MS +
                (esp_random() % (RADIO_LBT_BACKOFF_MAX_MS - RADIO_LBT_BACKOFF_MIN_MS + 1));
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }

        for (uint8_t round = 0; round < RADIO_ARQ_MAX_ROUNDS && pending_mask != 0; round++) {
            ARQ_LOG("[ARQ TX:ROUND seq=%04x tot=%u round=%u pending=%02x]\n",
                    seq, total_frags, round, pending_mask);
            uint8_t sent_mask = 0;
            for (uint8_t idx = 0; idx < total_frags; idx++) {
                const uint8_t bit = static_cast<uint8_t>(1u << idx);
                if ((pending_mask & bit) == 0) {
                    continue;
                }

                const uint16_t offset = static_cast<uint16_t>(idx) * FRAMING_FRAG_DATA;
                const uint8_t chunk = static_cast<uint8_t>(
                    (frame.len - offset < FRAMING_FRAG_DATA)
                        ? (frame.len - offset)
                        : FRAMING_FRAG_DATA);
                const uint8_t later_pending =
                    static_cast<uint8_t>(pending_mask & ~static_cast<uint8_t>((1u << (idx + 1)) - 1u));
                const bool round_end = later_pending == 0;

                if (sent_mask != 0) {
                    vTaskDelay(pdMS_TO_TICKS(RADIO_INTER_FRAG_DELAY_MS));
                }

                Packet pkt;
                framingBuildDataPacket(pkt, seq, idx, total_frags, round_end,
                                       frame.data + offset, chunk);

                sm.lock();
                sm.get().radioState = RadioState::TX;
                if (round > 0) {
                    sm.get().arqRetryCount++;
                }
                sm.unlock();

                int16_t err = radio.transmit(pkt, round_end);

                sm.lock();
                sm.get().radioState = RadioState::IDLE;
                if (err != RADIOLIB_ERR_NONE) {
                    sm.get().errorCount++;
                    sm.get().radioTxErrors++;
                }
                sm.unlock();

                if (err == RADIOLIB_ERR_NONE) {
                    sent_mask |= bit;
                }
            }

            if (sent_mask == 0) {
                continue;
            }

            AckFrame ack;
            bool got_ack = false;
            const uint32_t ack_deadline = millis() + ackTimeoutMs(total_frags);
            while (millis() < ack_deadline) {
                uint32_t now = millis();
                if (now >= ack_deadline) {
                    break;
                }
                TickType_t wait_ticks = pdMS_TO_TICKS(ack_deadline - now);
                if (xQueueReceive(ackQueue, &ack, wait_ticks) != pdPASS) {
                    break;
                }
                if (ack.seq != seq || ack.total_frags != total_frags) {
                    continue;
                }
                ARQ_LOG("[ARQ TX:ACK seq=%04x tot=%u ack_mask=%02x pend=%02x->%02x]\n",
                        seq, total_frags, ack.received_mask, pending_mask,
                        static_cast<uint8_t>(pending_mask & ~ack.received_mask));
                pending_mask &= static_cast<uint8_t>(~ack.received_mask);
                got_ack = true;
                break;
            }

            if (!got_ack) {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().arqAckTimeoutCount++;
                sm.unlock();
                ARQ_LOG("[ARQ TX:TIMEOUT seq=%04x tot=%u round=%u pending=%02x]\n",
                        seq, total_frags, round, pending_mask);
            }

            if (pending_mask == 0) {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().txCount++;
                sm.get().txBytes += frame.len;
                sm.unlock();
                delivered = true;
                ARQ_LOG("[ARQ TX:DONE seq=%04x tot=%u rounds=%u]\n",
                        seq, total_frags, round + 1);
                break;
            }
        }

        if (!delivered) {
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().arqFramesFailed++;
            sm.unlock();
            ARQ_LOG("[ARQ TX:FAIL seq=%04x tot=%u pending=%02x]\n",
                    seq, total_frags, pending_mask);
            noteRadioError();
        }
    }
}

// ── Task: Serial RX ───────────────────────────────────────────────────────────
// Reads KISS bytes from USB CDC, decodes frames, pushes to txQueue.
static void serialRxTask(void*) {
    Kiss     decoder;
    IpFrame  frame;
    uint32_t last_rx_ms = 0;

    for (;;) {
        int avail = Serial.available();
        if (avail > 0) {
            last_rx_ms = millis();
            for (int i = 0; i < avail; i++) {
                int c = Serial.read();
                if (c < 0) break;
                if (decoder.decode(static_cast<uint8_t>(c), frame)) {
#if DEBUG_KISS_SERIAL_RX
                    Serial.printf("[KISS RX] len=%u hex=", frame.len);
                    for (int _i = 0; _i < 8 && _i < frame.len; _i++) {
                        Serial.printf("%02x ", frame.data[_i]);
                    }
                    Serial.printf("\n");
#endif
                    // Block indefinitely on queue to apply backpressure to USB CDC
                    if (uxQueueSpacesAvailable(txQueue) == 0) {
                        auto& sm = StatsManager::instance();
                        sm.lock();
                        sm.get().txQueueWaitCount++;
                        sm.unlock();
                    }
                    xQueueSend(txQueue, &frame, portMAX_DELAY);
                }
            }
        } else {
            // No bytes available right now.
            if (millis() - last_rx_ms < 20) {
                // Microsecond poll to prevent adding FreeRTOS tick sleep overhead in active stream
                delayMicroseconds(100);
            } else {
                // Sleep for at least 1 tick when completely idle so displayTask can run
                TickType_t delay_ticks = pdMS_TO_TICKS(1);
                if (delay_ticks == 0) {
                    delay_ticks = 1;
                }
                vTaskDelay(delay_ticks);
            }
        }
    }
}

// ── Task: Serial TX ───────────────────────────────────────────────────────────
// Takes IP frames from rxQueue, KISS-encodes, writes to USB CDC.
static void serialTxTask(void*) {
    // Worst-case KISS encoded size: 2 bytes per payload byte + 3 framing bytes
    static uint8_t encBuf[IP_MTU * 2 + 3];
    IpFrame frame;
    for (;;) {
        xQueueReceive(rxQueue, &frame, portMAX_DELAY);
        size_t encLen = Kiss::encode(frame, encBuf, sizeof(encBuf));
        if (encLen == 0) {
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().serialTxEncodeFails++;
            sm.unlock();
            noteRadioError();
            continue;
        }
        size_t offset = 0;
        uint32_t retry_start_ms = 0;
        bool in_retry = false;

        while (offset < encLen) {
            size_t written = Serial.write(encBuf + offset, encLen - offset);
            if (written > 0) {
                offset += written;
                in_retry = false; // Reset retry window on successful write progress
            } else {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().serialTxZeroWrites++;
                sm.unlock();
                if (!in_retry) {
                    retry_start_ms = millis();
                    in_retry = true;
                } else if (millis() - retry_start_ms > 50) { // 50 ms timeout
                    sm.lock();
                    sm.get().serialTxTimeouts++;
                    sm.unlock();
                    noteRadioError();
                    break;
                }
                // Yield to allow other tasks to run and allow USB CDC buffer to drain
                TickType_t delay_ticks = pdMS_TO_TICKS(1);
                if (delay_ticks == 0) {
                    delay_ticks = 1;
                }
                vTaskDelay(delay_ticks);
            }
        }
    }
}

// ── Task: Display ─────────────────────────────────────────────────────────────
static void displayTask(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS));

        auto& sm = StatsManager::instance();
        sm.lock();
        Stats snapshot = sm.get();
        sm.unlock();

        display.update(snapshot);
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
    Serial.begin(0);   // USB CDC — baud rate is ignored by native USB

#if SERIAL_CONSOLE_LOGS
    // Wait briefly for a serial monitor only when the CDC port is being used
    // as a console rather than a pure KISS transport stream.
    for (int i = 0; i < 200 && !Serial; i++) {
        delay(10);
    }
    delay(100);
    BOOT_LOG_LN("\n\n=== SX1280 KISS TNC BOOTING ===");
#endif

    // Populate initial stats with config values
    {
        BOOT_LOG_LN("[main] Initializing statistics tracker...");
        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().freqMHz     = RADIO_FREQ_MHZ;
        sm.get().bitrateKbps = (uint32_t)RADIO_BITRATE_KBPS;
        sm.unlock();
        BOOT_LOG_LN("[main] Statistics tracker ready.");
    }

    BOOT_LOG_LN("[main] Initializing SSD1306 OLED display...");
    display.begin();
    BOOT_LOG_LN("[main] Display initialized successfully.");

#if SERIAL_CONSOLE_LOGS
    // Small delay to let the boot message be visible to the user.
    delay(500);
#endif

    BOOT_LOG_LN("[main] Initializing SX1280 radio transceiver...");
    int16_t radioErr = radio.begin();
    if (radioErr != RADIOLIB_ERR_NONE) {
        BOOT_LOG("[main] CRITICAL: Radio initialization failed! Error code: %d\n", radioErr);
        display.showError("Radio Init Fail", radioErr);

        // Halt state - flash the onboard LED (GPIO 37) as an additional visual indicator
        pinMode(37, OUTPUT);
        for (;;) {
            digitalWrite(37, HIGH);
            delay(100);
            digitalWrite(37, LOW);
            delay(100);
        }
    }
    BOOT_LOG_LN("[main] Radio transceiver ready.");

    BOOT_LOG_LN("[main] Activating continuous RX mode...");
    radio.startReceive();
    BOOT_LOG_LN("[main] Continuous RX mode active.");

    BOOT_LOG_LN("[main] Creating FreeRTOS communication queues...");
    txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(IpFrame));
    rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(IpFrame));
    ackQueue = xQueueCreate(8, sizeof(AckFrame));
    if (txQueue == nullptr || rxQueue == nullptr || ackQueue == nullptr) {
        BOOT_LOG_LN("[main] CRITICAL: Failed to create FreeRTOS queues!");
        display.showError("Queue Create Fail", -99);
        while (true) { delay(1000); }
    }
    BOOT_LOG_LN("[main] FreeRTOS queues created successfully.");

    BOOT_LOG_LN("[main] Spawning FreeRTOS tasks...");

    BaseType_t taskStatus;

    taskStatus = xTaskCreatePinnedToCore(radioRxTask,  "radioRx",  STACK_RADIO_RX,  nullptr, PRIO_RADIO,  nullptr, 1);
    BOOT_LOG("[main] Spawn task 'radioRx' on Core 1 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(radioTxTask,  "radioTx",  STACK_RADIO_TX,  nullptr, PRIO_RADIO,  nullptr, 1);
    BOOT_LOG("[main] Spawn task 'radioTx' on Core 1 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(serialRxTask, "serialRx", STACK_SERIAL_RX, nullptr, PRIO_SERIAL, nullptr, 0);
    BOOT_LOG("[main] Spawn task 'serialRx' on Core 0 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(serialTxTask, "serialTx", STACK_SERIAL_TX, nullptr, PRIO_SERIAL, nullptr, 0);
    BOOT_LOG("[main] Spawn task 'serialTx' on Core 0 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(displayTask,  "display",  STACK_DISPLAY,   nullptr, PRIO_DISPLAY, nullptr, 0);
    BOOT_LOG("[main] Spawn task 'display' on Core 0 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    BOOT_LOG("[main] IP MTU: %u bytes (%u fragments x %u bytes)\n",
             IP_MTU, FRAMING_MAX_FRAGS, FRAMING_FRAG_DATA);
    BOOT_LOG("[main] ARQ: %u rounds, ACK timeout %u ms, fallback ACK %u ms\n",
             RADIO_ARQ_MAX_ROUNDS, RADIO_ACK_TIMEOUT_MS, RADIO_ACK_FALLBACK_DELAY_MS);
    BOOT_LOG_LN("[main] System initialization complete. KISS TNC operational.");
}

void loop() {
    // All work is in FreeRTOS tasks — loop() is unused
    vTaskDelay(portMAX_DELAY);
}
