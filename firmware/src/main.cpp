#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "config.h"
#if SERIAL_TX_WDT_DIAGNOSTICS
#include <esp_task_wdt.h>
#endif
#include "radio/Radio.h"
#include "kiss/Kiss.h"
#include "framing/Framing.h"
#include "framing/Crc32.h"
#include "kiss/SerialIntegrity.h"
#include "display/Display.h"
#include "stats/Stats.h"
#include "config/ModemConfig.h"
#include "mac/Mac.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#if SERIAL_CONSOLE_LOGS
#define BOOT_LOG(...) Serial.printf(__VA_ARGS__)
#define BOOT_LOG_LN(msg) Serial.println(msg)
#else
#define BOOT_LOG(...) ((void)0)
#define BOOT_LOG_LN(msg) ((void)0)
#endif

// Set to 1 to log decoded KISS frame length and first 8 bytes on serial RX
#define DEBUG_KISS_SERIAL_RX 0

// ── Globals ───────────────────────────────────────────────────────────────────
static Radio   radio;
static Display display;
static ModemConfig modemConfig;
static ModemConfigSource modemConfigSource = ModemConfigSource::DEFAULTS;

static QueueHandle_t txQueue;   // PayloadFrame: SerialRX → MAC
static QueueHandle_t rxQueue;   // PayloadFrame: MAC → SerialTX
static SemaphoreHandle_t serialWriteMutex;
static uint32_t serialTxLastProgressMs = 0;

// HWCDC defaults to a 256-byte TX ring while a worst-case escaped generic KISS
// frame is over 2 KiB. Keep one complete frame in the ring and submit each KISS
// frame under one logical writer lock.
static constexpr size_t SERIAL_WRAPPED_MAX_LEN =
    TNC_PAYLOAD_MAX_LEN + SERIAL_INTEGRITY_HDR_LEN;
static constexpr size_t SERIAL_KISS_ENCODED_MAX =
    SERIAL_WRAPPED_MAX_LEN * 2u + 3u;
static constexpr uint32_t SERIAL_TX_TIMEOUT_MS = 1000;

static void refreshModemStats() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().freqMHz     = modemConfig.freqMHz;
    sm.get().bitrateKbps = static_cast<uint32_t>(modemConfig.bitrateKbps);
    sm.get().codingRate  = modemConfig.codingRate;
    sm.get().txPowerDbm  = modemConfig.txPowerDbm;
    sm.get().preambleBits = modemConfig.preambleBits;
    sm.get().btShaping   = (modemConfig.shaping == RADIOLIB_SHAPING_0_5) ? 0u : 1u;
    sm.get().syncWord =
        (static_cast<uint32_t>(modemConfig.syncWord[0]) << 24) |
        (static_cast<uint32_t>(modemConfig.syncWord[1]) << 16) |
        (static_cast<uint32_t>(modemConfig.syncWord[2]) << 8) |
        static_cast<uint32_t>(modemConfig.syncWord[3]);
    sm.get().lbtRssiThresholdDbm = modemConfig.lbtRssiThresholdDbm;
    sm.get().configCrc16 = modemConfigChecksum(modemConfig);
    sm.get().configVersion = MODEM_CONFIG_PROTOCOL_VERSION;
    sm.get().configSource = static_cast<uint8_t>(modemConfigSource);
    sm.get().transportMode = static_cast<uint8_t>(modemConfig.transportMode);
    sm.unlock();
}

static void noteKissMalformedFrame(bool oversize) {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().kissMalformedFrameCount++;
    if (oversize) {
        sm.get().kissOversizeDropCount++;
    }
    sm.unlock();
}

static void refreshHostBackpressureStats() {
    const uint32_t now = millis();
    auto& sm = StatsManager::instance();
    sm.lock();
    Stats& s = sm.get();
    if (txQueue != nullptr) {
        s.txQueueDepth = uxQueueMessagesWaiting(txQueue);
        s.txQueueFree = uxQueueSpacesAvailable(txQueue);
    }
    if (rxQueue != nullptr) {
        s.rxQueueDepth = uxQueueMessagesWaiting(rxQueue);
        s.rxQueueFree = uxQueueSpacesAvailable(rxQueue);
    }
    if (s.serialTxActive && serialTxLastProgressMs != 0) {
        s.serialTxLastProgressAgeMs = now - serialTxLastProgressMs;
        s.serialTxStallMs = s.serialTxLastProgressAgeMs;
    } else {
        s.serialTxLastProgressAgeMs = 0xFFFFFFFFu;
        s.serialTxStallMs = 0;
    }
    sm.unlock();
}

static void noteSerialWriteLock(bool held) {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().serialWriteLockHeld = held ? 1u : 0u;
    sm.unlock();
}

static void noteSerialTxStart(size_t encLen) {
    auto& sm = StatsManager::instance();
    sm.lock();
    Stats& s = sm.get();
    serialTxLastProgressMs = millis();
    s.serialTxActive = 1;
    s.serialTxFrameLen = encLen;
    s.serialTxOffset = 0;
    s.serialTxLastProgressAgeMs = 0;
    s.serialTxStallMs = 0;
    sm.unlock();
}

static void noteSerialTxProgress(size_t offset) {
    auto& sm = StatsManager::instance();
    sm.lock();
    Stats& s = sm.get();
    serialTxLastProgressMs = millis();
    s.serialTxOffset = offset;
    s.serialTxLastProgressAgeMs = 0;
    s.serialTxStallMs = 0;
    sm.unlock();
}

static void noteSerialTxDone() {
    auto& sm = StatsManager::instance();
    sm.lock();
    Stats& s = sm.get();
    s.serialTxActive = 0;
    s.serialTxOffset = s.serialTxFrameLen;
    s.serialTxLastProgressAgeMs = 0xFFFFFFFFu;
    s.serialTxStallMs = 0;
    sm.unlock();
}

static void sendControlResponse(const char* text) {
    static uint8_t encBuf[1024];
    const size_t len = strlen(text);
    const size_t encLen = Kiss::encodeFrame(KISS_CONTROL_FRAME,
                                            reinterpret_cast<const uint8_t*>(text),
                                            static_cast<uint16_t>(len),
                                            encBuf,
                                            sizeof(encBuf));
    if (encLen == 0) {
        return;
    }

    xSemaphoreTake(serialWriteMutex, portMAX_DELAY);
    noteSerialWriteLock(true);
    size_t offset = 0;
    while (offset < encLen) {
        size_t written = Serial.write(encBuf + offset, encLen - offset);
        if (written == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        offset += written;
    }
    xSemaphoreGive(serialWriteMutex);
    noteSerialWriteLock(false);
}

static bool parseFloatValue(const char* value, float& out) {
    char* end = nullptr;
    float parsed = strtof(value, &end);
    if (!end || *end != '\0') {
        return false;
    }
    out = parsed;
    return true;
}

static bool parseIntValue(const char* value, long& out) {
    char* end = nullptr;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0') {
        return false;
    }
    out = parsed;
    return true;
}

static bool modemResetButtonHeld() {
    pinMode(MODEM_RESET_BUTTON_PIN, INPUT_PULLUP);
    if (digitalRead(MODEM_RESET_BUTTON_PIN) != LOW) {
        return false;
    }
    delay(25);
    return digitalRead(MODEM_RESET_BUTTON_PIN) == LOW;
}

static void handleControlCommand(const uint8_t* data, uint16_t len) {
    char cmd[256];
    if (len >= sizeof(cmd)) {
        sendControlResponse("ERR command too long");
        return;
    }
    memcpy(cmd, data, len);
    cmd[len] = '\0';

    char* save = nullptr;
    char* verb = strtok_r(cmd, " \t\r\n", &save);
    if (!verb) {
        sendControlResponse("ERR empty command");
        return;
    }

    if (strcasecmp(verb, "GET") == 0) {
        char body[192];
        char response[255];
        modemFormatConfig(modemConfig, body, sizeof(body));
        snprintf(response, sizeof(response),
                 "OK ver=%u cfgcrc=%04x source=%s %s",
                 MODEM_CONFIG_PROTOCOL_VERSION,
                 modemConfigChecksum(modemConfig),
                 modemConfigSourceName(modemConfigSource),
                 body);
        sendControlResponse(response);
        return;
    }

    if (strcasecmp(verb, "STATS") == 0) {
        mac::refreshLinkStats();
        refreshHostBackpressureStats();
        auto& sm = StatsManager::instance();
        sm.lock();
        Stats snapshot = sm.get();
        sm.unlock();

        const char* lsName = "DOWN";
        if (snapshot.linkState == static_cast<uint8_t>(LinkState::READY))   lsName = "READY";
        else if (snapshot.linkState == static_cast<uint8_t>(LinkState::PROBING)) lsName = "PROBING";

        char response[768];
        snprintf(response, sizeof(response),
                 "OK rx=%lu rxBytes=%lu arqDone=%lu arqMetaDrop=%lu arqIntDrop=%lu arqCrc=%lu "
                 "stxZero=%lu stxTimeout=%lu stxEncodeFail=%lu rxQWait=%lu "
                 "linkReady=%u linkState=%s linkAgeMs=%lu "
                 "hbTx=%lu hbAckRx=%lu dpTx=%lu drRx=%lu primerTO=%lu cqDrop=%lu "
                 "wuTx=%lu wuRx=%lu wuAck=%lu wuTO=%lu "
                 "egress=%lu hwmMac=%lu hwmSrx=%lu hwmStx=%lu "
                 "qTx=%lu/%lu qRx=%lu/%lu stxLock=%u stxActive=%u "
                 "stxOff=%lu/%lu stxAge=%lu stxStall=%lu",
                 snapshot.rxCount,
                 snapshot.rxBytes,
                 snapshot.arqFramesCompleted,
                 snapshot.arqFragmentMetadataDrops,
                 snapshot.arqReassemblyIntegrityDrops,
                 snapshot.arqFrameCrcErrors,
                 snapshot.serialTxZeroWrites,
                 snapshot.serialTxTimeouts,
                 snapshot.serialTxEncodeFails,
                 snapshot.rxQueueWaitCount,
                 snapshot.linkReady,
                 lsName,
                 snapshot.linkAgeMs,
                 snapshot.controlHeartbeatTx,
                 snapshot.controlHeartbeatAckRx,
                 snapshot.controlDataPendingTx,
                 snapshot.controlDataReadyRx,
                 snapshot.controlPrimerTimeouts,
                 snapshot.controlQueueDrops,
                 snapshot.arqWarmupTx,
                 snapshot.arqWarmupRx,
                 snapshot.arqWarmupAckRx,
                 snapshot.arqWarmupTimeouts,
                 snapshot.rxEgressDeferrals,
                 snapshot.macStackHwm,
                 snapshot.serialRxStackHwm,
                 snapshot.serialTxStackHwm,
                 snapshot.txQueueDepth,
                 snapshot.txQueueFree,
                 snapshot.rxQueueDepth,
                 snapshot.rxQueueFree,
                 snapshot.serialWriteLockHeld,
                 snapshot.serialTxActive,
                 snapshot.serialTxOffset,
                 snapshot.serialTxFrameLen,
                 snapshot.serialTxLastProgressAgeMs,
                 snapshot.serialTxStallMs);
        sendControlResponse(response);
        return;
    }

    if (strcasecmp(verb, "DEFAULTS") == 0) {
        ModemConfig next = modemDefaultConfig();
        char error[80];
        int16_t state = mac::requestApplyConfig(next);
        if (state != RADIOLIB_ERR_NONE) {
            snprintf(error, sizeof(error), "ERR radio=%d", state);
            sendControlResponse(error);
            return;
        }
        modemConfig = next;
        modemSaveConfig(modemConfig);
        modemConfigSource = ModemConfigSource::DEFAULTS;
        refreshModemStats();
        sendControlResponse("OK defaults applied");
        return;
    }

    if (strcasecmp(verb, "SCAN") == 0) {
        float startMHz = 2400.0f;
        float stopMHz  = 2500.0f;
        float stepMHz  = 5.0f;
        long  dwellUs  = 1000;

        for (char* token = strtok_r(nullptr, " \t\r\n", &save);
             token != nullptr;
             token = strtok_r(nullptr, " \t\r\n", &save)) {
            char* eq = strchr(token, '=');
            if (!eq) { sendControlResponse("ERR expected key=value"); return; }
            *eq = '\0';
            const char* key   = token;
            const char* value = eq + 1;
            if (strcasecmp(key, "start") == 0) {
                if (!parseFloatValue(value, startMHz)) { sendControlResponse("ERR invalid start"); return; }
            } else if (strcasecmp(key, "stop") == 0) {
                if (!parseFloatValue(value, stopMHz)) { sendControlResponse("ERR invalid stop"); return; }
            } else if (strcasecmp(key, "step") == 0) {
                if (!parseFloatValue(value, stepMHz)) { sendControlResponse("ERR invalid step"); return; }
            } else if (strcasecmp(key, "dwell") == 0) {
                if (!parseIntValue(value, dwellUs)) { sendControlResponse("ERR invalid dwell"); return; }
            } else {
                sendControlResponse("ERR unknown scan key");
                return;
            }
        }

        if (startMHz < 2400.0f || stopMHz > 2500.0f || stepMHz < 1.0f || startMHz >= stopMHz) {
            sendControlResponse("ERR scan range: start/stop must be 2400..2500, step >= 1");
            return;
        }

        constexpr uint8_t SCAN_MAX_N = 50;
        int8_t rssiSamples[SCAN_MAX_N];
        uint8_t n = mac::requestScanBand(startMHz, stopMHz, stepMHz,
                                         static_cast<uint32_t>(dwellUs), rssiSamples, SCAN_MAX_N);
        if (n == 0) {
            sendControlResponse("ERR scan failed");
            return;
        }

        float bestFreq  = startMHz;
        int8_t bestRssi = rssiSamples[0];
        for (uint8_t i = 1; i < n; i++) {
            if (rssiSamples[i] < bestRssi) {
                bestRssi = rssiSamples[i];
                bestFreq = startMHz + i * stepMHz;
            }
        }

        char response[255];
        int pos = snprintf(response, sizeof(response),
                           "OK SCAN start=%.0f stop=%.0f step=%.0f n=%u best=%.0f rssi=",
                           startMHz, stopMHz, stepMHz, (unsigned)n, bestFreq);
        for (uint8_t i = 0; i < n && pos < (int)sizeof(response) - 5; i++) {
            if (i > 0) response[pos++] = ',';
            pos += snprintf(response + pos, sizeof(response) - pos, "%d", (int)rssiSamples[i]);
        }
        sendControlResponse(response);
        return;
    }

    if (strcasecmp(verb, "SET") != 0) {
        sendControlResponse("ERR expected GET, SET, SCAN, or DEFAULTS");
        return;
    }

    ModemConfig next = modemConfig;
    for (char* token = strtok_r(nullptr, " \t\r\n", &save);
         token != nullptr;
         token = strtok_r(nullptr, " \t\r\n", &save)) {
        char* eq = strchr(token, '=');
        if (!eq) {
            sendControlResponse("ERR expected key=value");
            return;
        }
        *eq = '\0';
        const char* key = token;
        const char* value = eq + 1;
        long ivalue = 0;

        if (strcasecmp(key, "freq") == 0) {
            if (!parseFloatValue(value, next.freqMHz)) {
                sendControlResponse("ERR invalid freq");
                return;
            }
        } else if (strcasecmp(key, "bitrate") == 0) {
            if (!parseFloatValue(value, next.bitrateKbps)) {
                sendControlResponse("ERR invalid bitrate");
                return;
            }
        } else if (strcasecmp(key, "cr") == 0) {
            if (!parseIntValue(value, ivalue)) {
                sendControlResponse("ERR invalid cr");
                return;
            }
            next.codingRate = static_cast<uint8_t>(ivalue);
        } else if (strcasecmp(key, "power") == 0) {
            if (!parseIntValue(value, ivalue)) {
                sendControlResponse("ERR invalid power");
                return;
            }
            next.txPowerDbm = static_cast<int8_t>(ivalue);
        } else if (strcasecmp(key, "preamble") == 0) {
            if (!parseIntValue(value, ivalue)) {
                sendControlResponse("ERR invalid preamble");
                return;
            }
            next.preambleBits = static_cast<uint8_t>(ivalue);
        } else if (strcasecmp(key, "bt") == 0) {
            if (!parseIntValue(value, ivalue)) {
                sendControlResponse("ERR invalid bt");
                return;
            }
            next.shaping = (ivalue == 0) ? RADIOLIB_SHAPING_0_5 : RADIOLIB_SHAPING_1_0;
        } else if (strcasecmp(key, "sync") == 0) {
            if (!modemParseSyncWord(value, next.syncWord)) {
                sendControlResponse("ERR invalid sync");
                return;
            }
        } else if (strcasecmp(key, "lbt") == 0) {
            if (!parseIntValue(value, ivalue)) {
                sendControlResponse("ERR invalid lbt");
                return;
            }
            next.lbtRssiThresholdDbm = static_cast<int16_t>(ivalue);
        } else if (strcasecmp(key, "transport") == 0) {
            if (strcasecmp(value, "native") == 0) {
                next.transportMode = TransportMode::NATIVE_PACKET;
            } else if (strcasecmp(value, "generic") == 0) {
                next.transportMode = TransportMode::GENERIC_FRAGMENTED;
            } else {
                sendControlResponse("ERR invalid transport (use native or generic)");
                return;
            }
        } else {
            sendControlResponse("ERR unknown key");
            return;
        }
    }

    char error[96];
    if (!modemValidateConfig(next, error, sizeof(error))) {
        char response[128];
        snprintf(response, sizeof(response), "ERR %s", error);
        sendControlResponse(response);
        return;
    }

    int16_t state = mac::requestApplyConfig(next);
    if (state != RADIOLIB_ERR_NONE) {
        char response[48];
        snprintf(response, sizeof(response), "ERR radio=%d", state);
        sendControlResponse(response);
        return;
    }
    modemConfig = next;
    if (!modemSaveConfig(modemConfig)) {
        sendControlResponse("ERR save failed");
        return;
    }
    modemConfigSource = ModemConfigSource::NVS;
    refreshModemStats();
    sendControlResponse("OK saved");
}

// Stack high-watermark telemetry for the calling task.
static void snapshotStackHwm(uint32_t Stats::* field) {
    const uint32_t hwm = uxTaskGetStackHighWaterMark(nullptr);
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().*field = hwm;
    sm.unlock();
}

// Rate-limited variant for per-frame call sites.
static void updateStackHwm(uint32_t Stats::* field, uint32_t& last_ms) {
    const uint32_t now = millis();
    if (now - last_ms < 1000) {
        return;
    }
    last_ms = now;
    snapshotStackHwm(field);
}

// ── Task: Serial RX ───────────────────────────────────────────────────────────
// Reads KISS bytes from USB CDC, decodes frames, pushes to txQueue.
static void serialRxTask(void*) {
    static Kiss     decoder;
    static KissFrame kissFrame;
    static PayloadFrame frame;
    uint32_t last_rx_ms = 0;
    uint32_t last_hwm_ms = 0;
    snapshotStackHwm(&Stats::serialRxStackHwm);

    for (;;) {
        int avail = Serial.available();
        if (avail > 0) {
            last_rx_ms = millis();
            for (int i = 0; i < avail; i++) {
                int c = Serial.read();
                if (c < 0) break;
                const KissDecodeResult result = decoder.decodeFrameEx(static_cast<uint8_t>(c), kissFrame);
                if (result == KissDecodeResult::OVERSIZE_DROP) {
                    noteKissMalformedFrame(true);
                    continue;
                }
                if (result == KissDecodeResult::INVALID_ESCAPE_DROP) {
                    noteKissMalformedFrame(false);
                    continue;
                }
                if (result != KissDecodeResult::FRAME) {
                    continue;
                }
                if (kissFrame.command == KISS_CONTROL_FRAME) {
                    handleControlCommand(kissFrame.data, kissFrame.len);
                    continue;
                }
                if (kissFrame.command != KISS_DATA_FRAME) {
                    continue;
                }
                if (modemConfig.transportMode == TransportMode::GENERIC_FRAGMENTED) {
                    SerialIntegrityHeader hdr;
                    if (!parseSerialIntegrityHeader(kissFrame.data, kissFrame.len, hdr) ||
                        hdr.magic != SERIAL_INTEGRITY_MAGIC ||
                        hdr.payload_len != kissFrame.len - SERIAL_INTEGRITY_HDR_LEN ||
                        framing::computeCrc32(kissFrame.data + SERIAL_INTEGRITY_HDR_LEN, hdr.payload_len) != hdr.payload_crc32) {
                        auto& sm = StatsManager::instance();
                        sm.lock();
                        sm.get().serialRxIntegrityDrops++;
                        sm.unlock();
                        continue;
                    }
                    memcpy(frame.data, kissFrame.data + SERIAL_INTEGRITY_HDR_LEN, hdr.payload_len);
                    frame.len = hdr.payload_len;
                } else {
                    memcpy(frame.data, kissFrame.data, kissFrame.len);
                    frame.len = kissFrame.len;
                }
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
                mac::notifyTxWork();
                updateStackHwm(&Stats::serialRxStackHwm, last_hwm_ms);
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
// Takes opaque payload frames from rxQueue, KISS-encodes, writes to USB CDC.
static void serialTxTask(void*) {
    static uint8_t encBuf[SERIAL_KISS_ENCODED_MAX];
    PayloadFrame frame;
    uint32_t last_hwm_ms = 0;
    snapshotStackHwm(&Stats::serialTxStackHwm);
    for (;;) {
        xQueueReceive(rxQueue, &frame, portMAX_DELAY);
        updateStackHwm(&Stats::serialTxStackHwm, last_hwm_ms);
        size_t encLen = 0;
        if (modemConfig.transportMode == TransportMode::GENERIC_FRAGMENTED) {
            uint8_t wrapperBuf[TNC_PAYLOAD_MAX_LEN + SERIAL_INTEGRITY_HDR_LEN];
            uint32_t crc = framing::computeCrc32(frame.data, frame.len);
            buildSerialIntegrityHeader(wrapperBuf, frame.len, crc);
            memcpy(wrapperBuf + SERIAL_INTEGRITY_HDR_LEN, frame.data, frame.len);
            encLen = Kiss::encodeFrame(KISS_DATA_FRAME, wrapperBuf, frame.len + SERIAL_INTEGRITY_HDR_LEN, encBuf, sizeof(encBuf));
        } else {
            encLen = Kiss::encode(frame, encBuf, sizeof(encBuf));
        }
        if (encLen == 0) {
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().serialTxEncodeFails++;
            sm.unlock();
            continue;
        }
        noteSerialTxStart(encLen);
        size_t offset = 0;
        uint32_t stall_report_ms = 0;
#if SERIAL_TX_WDT_DIAGNOSTICS
        const bool serialTxWdtActive = (esp_task_wdt_add(nullptr) == ESP_OK);
        if (serialTxWdtActive) {
            esp_task_wdt_reset();
        }
#endif

        // Submit the complete KISS frame under one logical write lock. Do not
        // call HWCDC::flush(): in the pinned Arduino core a flush timeout marks
        // CDC disconnected and silently discards later frames. write() already
        // triggers the USB ISR and blocks as needed while the enlarged ring
        // drains.
        xSemaphoreTake(serialWriteMutex, portMAX_DELAY);
        noteSerialWriteLock(true);
        while (offset < encLen) {
            size_t written = Serial.write(encBuf + offset, encLen - offset);
            if (written > 0) {
                offset += written;
                stall_report_ms = 0;
                noteSerialTxProgress(offset);
#if SERIAL_TX_WDT_DIAGNOSTICS
                if (serialTxWdtActive) {
                    esp_task_wdt_reset();
                }
#endif
            } else {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().serialTxZeroWrites++;
                sm.unlock();

                const uint32_t now_ms = millis();
                if (stall_report_ms == 0) {
                    stall_report_ms = now_ms;
                } else if (now_ms - stall_report_ms > 500) {
                    sm.lock();
                    sm.get().serialTxTimeouts++;
                    sm.unlock();
                    stall_report_ms = now_ms;
                }
                // Yield to allow other tasks to run and allow USB CDC buffer to drain
                TickType_t delay_ticks = pdMS_TO_TICKS(1);
                if (delay_ticks == 0) {
                    delay_ticks = 1;
                }
                vTaskDelay(delay_ticks);
            }
        }
        xSemaphoreGive(serialWriteMutex);
        noteSerialWriteLock(false);
        noteSerialTxDone();
#if SERIAL_TX_WDT_DIAGNOSTICS
        if (serialTxWdtActive) {
            esp_task_wdt_delete(nullptr);
        }
#endif
    }
}

// ── Task: Display ─────────────────────────────────────────────────────────────
static void displayTask(void*) {
    TickType_t lastWake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(DISPLAY_REFRESH_MS));

        refreshHostBackpressureStats();
        auto& sm = StatsManager::instance();
        sm.lock();
        Stats snapshot = sm.get();
        sm.unlock();

        display.update(snapshot);
    }
}

// Fatal boot error: show it and halt with a blinking LED so the failure is
// deterministic and visible instead of a half-running modem.
static void haltBoot(const char* what, int code) {
    BOOT_LOG("[main] CRITICAL: %s (code %d)\n", what, code);
    display.showError(what, code);
    pinMode(37, OUTPUT);
    for (;;) {
        digitalWrite(37, HIGH);
        delay(100);
        digitalWrite(37, LOW);
        delay(100);
    }
}

// ── Arduino entry points ──────────────────────────────────────────────────────
void setup() {
    serialWriteMutex = xSemaphoreCreateMutex();
    const size_t serialTxBufferSize =
        Serial.setTxBufferSize(SERIAL_KISS_ENCODED_MAX);
    Serial.setTxTimeoutMs(SERIAL_TX_TIMEOUT_MS);
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
        if (modemResetButtonHeld()) {
            BOOT_LOG_LN("[main] GPIO0 held low: clearing persisted modem config.");
            modemClearConfig();
            modemConfig = modemDefaultConfig();
            modemConfigSource = ModemConfigSource::RESET_HELD;
        } else {
            modemConfigSource = modemLoadConfig(modemConfig);
        }
        refreshModemStats();
        BOOT_LOG("[main] Active modem config source=%s crc=%04x\n",
                 modemConfigSourceName(modemConfigSource),
                 modemConfigChecksum(modemConfig));
        BOOT_LOG_LN("[main] Statistics tracker ready.");
    }

    BOOT_LOG_LN("[main] Initializing SSD1306 OLED display...");
    display.begin();
    BOOT_LOG_LN("[main] Display initialized successfully.");
    if (serialWriteMutex == nullptr ||
        serialTxBufferSize < SERIAL_KISS_ENCODED_MAX) {
        haltBoot("USB TX Init Fail", -94);
    }

#if SERIAL_CONSOLE_LOGS
    // Small delay to let the boot message be visible to the user.
    delay(500);
#endif

    BOOT_LOG_LN("[main] Initializing SX1280 radio transceiver...");
    int16_t radioErr = radio.begin(modemConfig);
    if (radioErr != RADIOLIB_ERR_NONE) {
        haltBoot("Radio Init Fail", radioErr);
    }
    BOOT_LOG_LN("[main] Radio transceiver ready.");

    BOOT_LOG_LN("[main] Creating FreeRTOS communication queues...");
    txQueue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(PayloadFrame));
    rxQueue = xQueueCreate(RX_QUEUE_DEPTH, sizeof(PayloadFrame));
    if (txQueue == nullptr || rxQueue == nullptr) {
        haltBoot("Queue Create Fail", -99);
    }
    BOOT_LOG_LN("[main] FreeRTOS queues created successfully.");

    // The MAC task owns the radio from here on; it enters RX mode itself.
    BOOT_LOG_LN("[main] Starting MAC task (single radio owner)...");
    mac::Init macInit;
    macInit.radio   = &radio;
    macInit.txQueue = txQueue;
    macInit.rxQueue = rxQueue;
    if (!mac::start(macInit)) {
        haltBoot("MAC Start Fail", -98);
    }
    BOOT_LOG_LN("[main] MAC task running.");

    BOOT_LOG_LN("[main] Spawning FreeRTOS tasks...");

    if (xTaskCreatePinnedToCore(serialRxTask, "serialRx", STACK_SERIAL_RX, nullptr, PRIO_SERIAL, nullptr, 0) != pdPASS) {
        haltBoot("Task Spawn Fail", -97);
    }
    if (xTaskCreatePinnedToCore(serialTxTask, "serialTx", STACK_SERIAL_TX, nullptr, PRIO_SERIAL, nullptr, 0) != pdPASS) {
        haltBoot("Task Spawn Fail", -96);
    }
    if (xTaskCreatePinnedToCore(displayTask, "display", STACK_DISPLAY, nullptr, PRIO_DISPLAY, nullptr, 0) != pdPASS) {
        haltBoot("Task Spawn Fail", -95);
    }

    BOOT_LOG("[main] Payload MTU: %u bytes (%u fragments max, %u bytes/frag)\n",
             TNC_PAYLOAD_MAX_LEN, FRAMING_MAX_FRAGS, FRAMING_FRAG_DATA);
    BOOT_LOG("[main] ARQ: %u rounds, ACK timeout %u ms, fallback ACK %u ms\n",
             RADIO_ARQ_MAX_ROUNDS, RADIO_ACK_TIMEOUT_MS, RADIO_ACK_FALLBACK_DELAY_MS);
    BOOT_LOG_LN("[main] System initialization complete. KISS TNC operational.");
}

void loop() {
    // All work is in FreeRTOS tasks — loop() is unused
    vTaskDelay(portMAX_DELAY);
}
