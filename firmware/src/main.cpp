#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "config.h"
#include "radio/Radio.h"
#include "kiss/Kiss.h"
#include "display/Display.h"
#include "stats/Stats.h"

// ── Globals ───────────────────────────────────────────────────────────────────
static Radio   radio;
static Display display;

static QueueHandle_t txQueue;   // Packet: SerialRX  → RadioTX
static QueueHandle_t rxQueue;   // Packet: RadioRX   → SerialTX

// ── Task: Radio RX ────────────────────────────────────────────────────────────
// Waits on DIO1 semaphore, reads SX1280 FIFO, pushes to rxQueue.
static void radioRxTask(void*) {
    Packet pkt;
    for (;;) {
        xSemaphoreTake(radio.rxSemaphore, portMAX_DELAY);

        int16_t err = radio.readPacket(pkt);

        auto& sm = StatsManager::instance();
        sm.lock();
        if (err == RADIOLIB_ERR_NONE && pkt.len > 0) {
            sm.get().rxCount++;
            sm.get().rxBytes   += pkt.len;
            sm.get().rssi       = pkt.rssi;
            sm.get().snr        = pkt.snr;
            sm.get().radioState = RadioState::RX;
            xQueueSend(rxQueue, &pkt, 0);
        } else {
            sm.get().errorCount++;
            sm.get().radioState = RadioState::ERROR;
        }
        sm.unlock();
    }
}

// ── Task: Radio TX ────────────────────────────────────────────────────────────
// Pops Packets from txQueue, applies CSMA backoff, transmits.
static void radioTxTask(void*) {
    Packet pkt;
    for (;;) {
        xQueueReceive(txQueue, &pkt, portMAX_DELAY);

        // Brief inter-frame gap — gives the remote node time to return to RX before
        // the next frame arrives. 2 ms is well above the SX1280 RX-turnaround time.
        vTaskDelay(pdMS_TO_TICKS(2));

        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().radioState = RadioState::TX;
        sm.unlock();

        int16_t err = radio.transmit(pkt);

        sm.lock();
        if (err == RADIOLIB_ERR_NONE) {
            sm.get().txCount++;
            sm.get().txBytes += pkt.len;
        } else {
            sm.get().errorCount++;
        }
        sm.get().radioState = RadioState::IDLE;
        sm.unlock();
    }
}

// ── Task: Serial RX ───────────────────────────────────────────────────────────
// Reads KISS bytes from USB CDC, decodes frames, pushes to txQueue.
static void serialRxTask(void*) {
    Kiss  decoder;
    Packet pkt;
    for (;;) {
        if (Serial.available()) {
            uint8_t b = Serial.read();
            if (decoder.decode(b, pkt)) {
                xQueueSend(txQueue, &pkt, pdMS_TO_TICKS(10));
            }
        } else {
            // Block for 1 tick so lower-priority tasks (displayTask) get CPU time.
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

// ── Task: Serial TX ───────────────────────────────────────────────────────────
// Takes Packets from rxQueue, KISS-encodes, writes to USB CDC.
static void serialTxTask(void*) {
    // Max encoded size: 2 bytes per payload byte (worst-case escaping) + 3 framing bytes
    static uint8_t encBuf[PACKET_MAX_LEN * 2 + 3];
    Packet pkt;
    for (;;) {
        xQueueReceive(rxQueue, &pkt, portMAX_DELAY);
        size_t encLen = Kiss::encode(pkt, encBuf, sizeof(encBuf));
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
    txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(Packet));
    rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(Packet));
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

    Serial.println("[main] System initialization complete. KISS TNC operational.");
}

void loop() {
    // All work is in FreeRTOS tasks — loop() is unused
    vTaskDelay(portMAX_DELAY);
}
