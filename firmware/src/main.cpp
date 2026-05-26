#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <stdio.h>

#include "config.h"
#include "radio/Radio.h"
#include "kiss/Kiss.h"
#include "framing/Framing.h"
#include "display/Display.h"
#include "stats/Stats.h"

// ── Globals ───────────────────────────────────────────────────────────────────
static Radio   radio;
static Display display;

static QueueHandle_t txQueue;   // IpFrame: SerialRX  → RadioTX
static QueueHandle_t rxQueue;   // IpFrame: RadioRX   → SerialTX
static QueueHandle_t ackQueue;  // AckFrame: RadioRX  → RadioTX
static SemaphoreHandle_t serialWriteMutex;

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

static void finalizeReassembly(Reassembler& ra, CompletedFrameCache& completed) {
    IpFrame frame;
    frame.len = 0;
    for (uint8_t i = 0; i < ra.total_frags; i++) {
        memcpy(frame.data + frame.len,
               ra.buf + i * FRAMING_FRAG_DATA,
               ra.frag_len[i]);
        frame.len += ra.frag_len[i];
    }
    frame.rssi = ra.last_rssi;

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
    }
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
        noteRadioError();
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
            continue;
        }
        if (err != RADIOLIB_ERR_NONE || pkt.len < 1) {
            noteRadioError();
            continue;
        }

        if (framingPacketType(pkt) == LinkPacketType::ACK) {
            AckFrame ack;
            if (!framingParseAck(pkt, ack)) {
                noteRadioError();
                continue;
            }
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().arqAckRxCount++;
            sm.unlock();
            if (xQueueSend(ackQueue, &ack, 0) != pdPASS) {
                noteQueueDrop();
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
            noteRadioError();
            continue;
        }

        if (completed.seq == seq &&
            completed.total_frags == total_frags &&
            (now_ms - completed.tick_ms) <= RADIO_DUP_CACHE_MS) {
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
                noteRadioError();
            }
            continue;
        }

        if (ra.seq != FRAMING_SEQ_UNSET &&
            (now_ms - ra.last_tick_ms > reassemblyTimeoutMs(ra.total_frags))) {
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

        // LBT-CSMA: sense channel before the first fragment only.
        for (int lbt = 0; radio.isChannelBusy(); lbt++) {
            if (lbt >= RADIO_LBT_MAX_RETRIES) break;
            uint32_t backoff_ms = RADIO_LBT_BACKOFF_MIN_MS +
                (esp_random() % (RADIO_LBT_BACKOFF_MAX_MS - RADIO_LBT_BACKOFF_MIN_MS + 1));
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        }

        for (uint8_t round = 0; round < RADIO_ARQ_MAX_ROUNDS && pending_mask != 0; round++) {
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
                pending_mask &= static_cast<uint8_t>(~ack.received_mask);
                got_ack = true;
                break;
            }

            if (!got_ack) {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().arqAckTimeoutCount++;
                sm.unlock();
            }

            if (pending_mask == 0) {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().txCount++;
                sm.get().txBytes += frame.len;
                sm.unlock();
                delivered = true;
                break;
            }
        }

        if (!delivered) {
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().arqFramesFailed++;
            sm.unlock();
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
                    // Block indefinitely on queue to apply backpressure to USB CDC
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
        xSemaphoreTake(serialWriteMutex, portMAX_DELAY);
        size_t written = Serial.write(encBuf, encLen);
        xSemaphoreGive(serialWriteMutex);
        (void)written;
    }
}

// ── Task: Serial Stats ────────────────────────────────────────────────────────
// Emits periodic ARQ counters on a dedicated KISS control port.
static void serialStatsTask(void*) {
    static char statsBuf[256];
    static uint8_t encBuf[sizeof(statsBuf) * 2 + 4];

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATS_FRAME_INTERVAL_MS));

        auto& sm = StatsManager::instance();
        sm.lock();
        Stats snapshot = sm.get();
        sm.unlock();

        int len = snprintf(
            statsBuf, sizeof(statsBuf),
            "stats tx=%lu rx=%lu err=%lu fs=%lu fc=%lu ff=%lu rty=%lu ato=%lu "
            "acktx=%lu ackrx=%lu ackerr=%lu dup=%lu drop=%lu id=%lu qdrop=%lu rssi=%d",
            snapshot.txCount,
            snapshot.rxCount,
            snapshot.errorCount,
            snapshot.arqFramesStarted,
            snapshot.arqFramesCompleted,
            snapshot.arqFramesFailed,
            snapshot.arqRetryCount,
            snapshot.arqAckTimeoutCount,
            snapshot.arqAckTxCount,
            snapshot.arqAckRxCount,
            snapshot.arqAckTxErrors,
            snapshot.arqDuplicateSuppressed,
            snapshot.arqReassemblyDrops,
            snapshot.arqIdentityResets,
            snapshot.arqQueueDrops,
            snapshot.rssi
        );
        if (len <= 0) {
            continue;
        }
        if (len >= static_cast<int>(sizeof(statsBuf))) {
            len = sizeof(statsBuf) - 1;
        }

        size_t encLen = Kiss::encodeRaw(
            KISS_STATS_FRAME,
            reinterpret_cast<const uint8_t*>(statsBuf),
            static_cast<size_t>(len),
            encBuf,
            sizeof(encBuf)
        );
        xSemaphoreTake(serialWriteMutex, portMAX_DELAY);
        size_t written = Serial.write(encBuf, encLen);
        xSemaphoreGive(serialWriteMutex);
        (void)written;
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

    // Wait up to 2 seconds for a serial monitor to connect so the board can boot standalone
    for (int i = 0; i < 200 && !Serial; i++) {
        delay(10);
    }

    // Give serial CDC time to fully establish if connected, then print banner
    delay(100);
    Serial.println("\n\n=== SX1280 KISS TNC BOOTING ===");

    // Populate initial stats with config values
    {
        Serial.println("[main] Initializing statistics tracker...");
        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().freqMHz     = RADIO_FREQ_MHZ;
        sm.get().bitrateKbps = (uint32_t)RADIO_BITRATE_KBPS;
        sm.unlock();
        Serial.println("[main] Statistics tracker ready.");
    }

    Serial.println("[main] Initializing SSD1306 OLED display...");
    display.begin();
    Serial.println("[main] Display initialized successfully.");

    // Small delay to let the boot message be visible to the user
    delay(500);

    Serial.println("[main] Initializing SX1280 radio transceiver...");
    int16_t radioErr = radio.begin();
    if (radioErr != RADIOLIB_ERR_NONE) {
        Serial.printf("[main] CRITICAL: Radio initialization failed! Error code: %d\n", radioErr);
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
    Serial.println("[main] Radio transceiver ready.");

    Serial.println("[main] Activating continuous RX mode...");
    radio.startReceive();
    Serial.println("[main] Continuous RX mode active.");

    Serial.println("[main] Creating FreeRTOS communication queues...");
    txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(IpFrame));
    rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(IpFrame));
    ackQueue = xQueueCreate(8, sizeof(AckFrame));
    serialWriteMutex = xSemaphoreCreateMutex();
    if (txQueue == nullptr || rxQueue == nullptr || ackQueue == nullptr || serialWriteMutex == nullptr) {
        Serial.println("[main] CRITICAL: Failed to create FreeRTOS queues!");
        display.showError("Queue Create Fail", -99);
        while (true) { delay(1000); }
    }
    Serial.println("[main] FreeRTOS queues created successfully.");

    Serial.println("[main] Spawning FreeRTOS tasks...");

    BaseType_t taskStatus;

    taskStatus = xTaskCreatePinnedToCore(radioRxTask,  "radioRx",  STACK_RADIO_RX,  nullptr, PRIO_RADIO,  nullptr, 1);
    Serial.printf("[main] Spawn task 'radioRx' on Core 1 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(radioTxTask,  "radioTx",  STACK_RADIO_TX,  nullptr, PRIO_RADIO,  nullptr, 1);
    Serial.printf("[main] Spawn task 'radioTx' on Core 1 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(serialRxTask, "serialRx", STACK_SERIAL_RX, nullptr, PRIO_SERIAL, nullptr, 0);
    Serial.printf("[main] Spawn task 'serialRx' on Core 0 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(serialTxTask, "serialTx", STACK_SERIAL_TX, nullptr, PRIO_SERIAL, nullptr, 0);
    Serial.printf("[main] Spawn task 'serialTx' on Core 0 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(serialStatsTask, "serialStats", STACK_SERIAL_STATS, nullptr, PRIO_SERIAL_STATS, nullptr, 0);
    Serial.printf("[main] Spawn task 'serialStats' on Core 0 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    taskStatus = xTaskCreatePinnedToCore(displayTask,  "display",  STACK_DISPLAY,   nullptr, PRIO_DISPLAY, nullptr, 0);
    Serial.printf("[main] Spawn task 'display' on Core 0 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    Serial.printf("[main] IP MTU: %u bytes (%u fragments x %u bytes)\n",
                  IP_MTU, FRAMING_MAX_FRAGS, FRAMING_FRAG_DATA);
    Serial.printf("[main] ARQ: %u rounds, ACK timeout %u ms, fallback ACK %u ms\n",
                  RADIO_ARQ_MAX_ROUNDS, RADIO_ACK_TIMEOUT_MS, RADIO_ACK_FALLBACK_DELAY_MS);
    Serial.println("[main] System initialization complete. KISS TNC operational.");
}

void loop() {
    // All work is in FreeRTOS tasks — loop() is unused
    vTaskDelay(portMAX_DELAY);
}
