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

// ── Globals ───────────────────────────────────────────────────────────────────
static Radio   radio;
static Display display;

static QueueHandle_t txQueue;   // IpFrame: SerialRX  → RadioTX
static QueueHandle_t rxQueue;   // IpFrame: RadioRX   → SerialTX

// ── Task: Radio RX ────────────────────────────────────────────────────────────
// Receives radio packets, reassembles fragments into IP frames, pushes to rxQueue.
static void radioRxTask(void*) {
    Packet pkt;
    static Reassembler ra;   // static: lives in BSS, not on the task stack

    for (;;) {
        xSemaphoreTake(radio.rxSemaphore, portMAX_DELAY);

        int16_t err = radio.readPacket(pkt);
        if (err != RADIOLIB_ERR_NONE || pkt.len < 1) {
            Serial.printf("[rx] spurious/err: err=%d pkt.len=%d\n", err, pkt.len);
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().errorCount++;
            sm.get().radioState = RadioState::ERROR;
            sm.unlock();
            continue;
        }

        const uint8_t header   = pkt.data[0];
        const bool    is_split = (header & FRAMING_FLAG_SPLIT) != 0;

        if (!is_split) {
            // Single-packet frame: strip the 1-byte framing header, deliver.
            IpFrame frame;
            frame.len  = pkt.len - 1;
            Serial.printf("[rx] single pkt_len=%d frame_len=%d rssi=%d\n",
                          pkt.len, frame.len, pkt.rssi);
            frame.rssi = pkt.rssi;
            frame.snr  = pkt.snr;
            memcpy(frame.data, pkt.data + 1, frame.len);

            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().rxCount++;
            sm.get().rxBytes   += frame.len;
            sm.get().rssi       = frame.rssi;
            sm.get().snr        = frame.snr;
            sm.get().radioState = RadioState::RX;
            sm.unlock();

            xQueueSend(rxQueue, &frame, 0);

        } else {
            // Fragmented frame — run reassembler.
            const uint8_t seq     = header >> 4;
            const uint8_t idx     = (header >> 2) & 0x03;
            const bool    is_last = (header & FRAMING_FLAG_LAST) != 0;

            // Discard stale partial reassembly after 500 ms silence.
            if (ra.seq != FRAMING_SEQ_UNSET &&
                (millis() - ra.last_tick_ms > 500)) {
                Serial.printf("[radio_rx] Stale partial reassembly discarded (seq=%d, mask=0x%02X)\n", ra.seq, ra.received_mask);
                ra.reset();
            }

            // New sequence number: discard previous partial and start fresh.
            if (ra.seq != seq) {
                if (ra.seq != FRAMING_SEQ_UNSET) {
                    Serial.printf("[radio_rx] New seq %d received; abandoning old seq %d\n", seq, ra.seq);
                }
                ra.reset();
                ra.seq = seq;
            }

            const uint16_t frag_data_len = pkt.len - 1;
            memcpy(ra.buf + idx * FRAMING_FRAG_DATA, pkt.data + 1, frag_data_len);
            ra.frag_len[idx]   = frag_data_len;
            ra.received_mask  |= static_cast<uint8_t>(1u << idx);
            ra.frag_count++;
            ra.last_tick_ms    = millis();
            if (is_last) ra.total_frags = idx + 1;

            Serial.printf("[radio_rx] Received frag: seq=%d, idx=%d, is_last=%s, len=%d\n", 
                          seq, idx, is_last ? "yes" : "no", frag_data_len);

            if (ra.isComplete()) {
                IpFrame frame;
                frame.len = 0;
                for (uint8_t i = 0; i < ra.total_frags; i++) {
                    memcpy(frame.data + frame.len,
                           ra.buf + i * FRAMING_FRAG_DATA,
                           ra.frag_len[i]);
                    frame.len += ra.frag_len[i];
                }
                
                // Adopt final fragment's metrics directly (skipped on intermediate frags anyway)
                frame.rssi = pkt.rssi;
                frame.snr  = pkt.snr;

                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().rxCount++;
                sm.get().rxBytes   += frame.len;
                sm.get().rssi       = frame.rssi;
                sm.get().snr        = frame.snr;
                sm.get().radioState = RadioState::RX;
                sm.unlock();

                Serial.printf("[radio_rx] Frame fully reassembled! len=%d, seq=%d, RSSI=%d\n", frame.len, ra.seq, frame.rssi);
                xQueueSend(rxQueue, &frame, 0);
                ra.reset();
            }
        }
    }
}

// ── Task: Radio TX ────────────────────────────────────────────────────────────
// Dequeues IP frames, fragments into ≤126-byte radio packets, transmits each.
static void radioTxTask(void*) {
    IpFrame frame;
    Packet  pkt;
    static uint8_t seq = 0;

    for (;;) {
        xQueueReceive(txQueue, &frame, portMAX_DELAY);

        seq = (seq + 1) & 0x0F;
        const bool    needs_split = (frame.len > FRAMING_FRAG_DATA);
        const uint8_t total_frags = needs_split
            ? static_cast<uint8_t>((frame.len + FRAMING_FRAG_DATA - 1) / FRAMING_FRAG_DATA)
            : 1;
        uint16_t      offset      = 0;
        uint8_t       idx         = 0;

        Serial.printf("[radio_tx] Sending frame: len=%d, seq=%d, frags=%d\n", frame.len, seq, total_frags);

        while (offset < frame.len) {
            const uint16_t chunk   = (frame.len - offset < FRAMING_FRAG_DATA)
                                     ? static_cast<uint16_t>(frame.len - offset)
                                     : static_cast<uint16_t>(FRAMING_FRAG_DATA);
            const bool     is_last = (offset + chunk >= frame.len);

            uint8_t header = static_cast<uint8_t>(seq << 4);
            if (needs_split) {
                header |= FRAMING_FLAG_SPLIT;
                header |= static_cast<uint8_t>(idx << 2);
                if (is_last) header |= FRAMING_FLAG_LAST;
            }

            pkt.data[0] = header;
            memcpy(pkt.data + 1, frame.data + offset, chunk);
            pkt.len = static_cast<uint8_t>(chunk + 1);

            // Inter-fragment gap: gives the remote receiver ample time to process
            // the previous fragment, write it, and return to RX mode.
            // Only apply a delay between fragments (when idx > 0).
            if (idx > 0) {
                // Enforce minimum of 2 ticks (20ms) on 100Hz systems to guard against tick truncation.
                TickType_t delay_ticks = pdMS_TO_TICKS(15);
                if (delay_ticks <= 1) {
                    delay_ticks = 2; 
                }
                vTaskDelay(delay_ticks);
            }

            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().radioState = RadioState::TX;
            sm.unlock();

            Serial.printf("[radio_tx] Transmitting frag %d/%d (seq=%d, len=%d)\n", idx + 1, total_frags, seq, pkt.len);
            int16_t err = radio.transmit(pkt);

            sm.lock();
            if (err == RADIOLIB_ERR_NONE) {
                Serial.printf("[radio_tx] Transmit OK (frag %d/%d)\n", idx + 1, total_frags);
                if (is_last) {
                    sm.get().txCount++;
                    sm.get().txBytes += frame.len;
                }
            } else {
                Serial.printf("[radio_tx] Transmit FAILED (frag %d/%d, err=%d)\n", idx + 1, total_frags, err);
                sm.get().errorCount++;
            }
            sm.get().radioState = RadioState::IDLE;
            sm.unlock();

            offset += chunk;
            idx++;
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
        Serial.write(encBuf, encLen);
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
        sm.get().bitrateKbps = 650;   // matches RADIO_BITRATE_PRESET default
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
    if (txQueue == nullptr || rxQueue == nullptr) {
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

    taskStatus = xTaskCreatePinnedToCore(displayTask,  "display",  STACK_DISPLAY,   nullptr, PRIO_DISPLAY, nullptr, 0);
    Serial.printf("[main] Spawn task 'display' on Core 0 -> %s\n", taskStatus == pdPASS ? "OK" : "FAILED");

    Serial.printf("[main] IP MTU: %u bytes (%u fragments x %u bytes)\n",
                  IP_MTU, FRAMING_MAX_FRAGS, FRAMING_FRAG_DATA);
    Serial.println("[main] System initialization complete. KISS TNC operational.");
}

void loop() {
    // All work is in FreeRTOS tasks — loop() is unused
    vTaskDelay(portMAX_DELAY);
}
