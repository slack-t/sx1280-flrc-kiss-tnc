#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "config.h"
#include "radio/Radio.h"
#include "kiss/Kiss.h"
#include "framing/Framing.h"
#include "framing/Crc32.h"
#include "kiss/SerialIntegrity.h"
#include "display/Display.h"
#include "stats/Stats.h"
#include "config/ModemConfig.h"
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
static ModemConfig modemConfig;
static ModemConfigSource modemConfigSource = ModemConfigSource::DEFAULTS;

static QueueHandle_t txQueue;       // PayloadFrame: SerialRX → RadioTX
static QueueHandle_t rxQueue;       // PayloadFrame: RadioRX  → SerialTX
static QueueHandle_t ackQueue;      // AckFrame:     RadioRX  → RadioTX
static QueueHandle_t controlQueue;  // ControlFrame: RadioRX  → RadioTX

// Link-health state. Written by radioTxTask; last_link_activity_ms also
// written by radioRxTask. Both tasks live on Core 1 so volatile 32-bit
// aligned writes are sufficient.
static volatile uint32_t g_last_link_activity_ms        = 0;
static volatile uint32_t g_last_bidirectional_ctrl_ms   = 0;
static volatile bool     g_link_ever_confirmed          = false;
static uint16_t          g_control_seq                  = 0;  // only incremented by radioTxTask

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

static inline bool isLinkReady() {
    if (!g_link_ever_confirmed) return false;
    return (millis() - g_last_bidirectional_ctrl_ms) <= RADIO_LINK_READY_TTL_MS;
}

static inline bool isLinkIdle() {
    return (millis() - g_last_link_activity_ms) >= RADIO_LINK_IDLE_MS;
}

static void noteLinkActivity() {
    g_last_link_activity_ms = millis();
}

static void noteControlMalformed() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().controlMalformedDrops++;
    sm.unlock();
}

static void noteBidirectionalControl() {
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

static void updateLinkStats() {
    const bool ready = isLinkReady();
    const uint32_t age_ms = g_link_ever_confirmed
        ? (millis() - g_last_bidirectional_ctrl_ms)
        : 0xFFFFFFFFu;
    auto& sm = StatsManager::instance();
    sm.lock();
    const bool was_ready = (sm.get().linkReady != 0);
    sm.get().linkReady = ready ? 1u : 0u;
    sm.get().linkState = ready
        ? static_cast<uint8_t>(LinkState::READY)
        : static_cast<uint8_t>(LinkState::DOWN);
    sm.get().linkAgeMs = age_ms;
    if (was_ready && !ready) {
        sm.get().linkDownTransitions++;
    }
    sm.unlock();
}

struct CompletedFrameCache {
    uint16_t seq         = FRAMING_SEQ_UNSET;
    uint8_t  total_frags = 0;
    uint16_t frame_len   = 0;
    uint32_t frame_crc32 = 0;
    uint32_t ack_mask    = 0;
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

static void noteRadioRxCrcError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxCrcErrorCount++;
    sm.unlock();
}

static void noteRadioRxHeaderError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxHeaderErrorCount++;
    sm.unlock();
}

static void noteRadioRxSyncWordError() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxSyncWordErrorCount++;
    sm.unlock();
}

static void noteRadioRxTimeout() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().rxTimeoutCount++;
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

static void noteNativeOversizeDrop() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().nativeOversizeDropCount++;
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
    size_t offset = 0;
    while (offset < encLen) {
        size_t written = Serial.write(encBuf + offset, encLen - offset);
        if (written == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        offset += written;
    }
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
        updateLinkStats();
        auto& sm = StatsManager::instance();
        sm.lock();
        Stats snapshot = sm.get();
        sm.unlock();

        const char* lsName = "DOWN";
        if (snapshot.linkState == static_cast<uint8_t>(LinkState::READY))   lsName = "READY";
        else if (snapshot.linkState == static_cast<uint8_t>(LinkState::PROBING)) lsName = "PROBING";

        char response[400];
        snprintf(response, sizeof(response),
                 "OK rx=%lu rxBytes=%lu arqDone=%lu arqMetaDrop=%lu arqIntDrop=%lu arqCrc=%lu "
                 "stxZero=%lu stxTimeout=%lu stxEncodeFail=%lu rxQWait=%lu "
                 "linkReady=%u linkState=%s linkAgeMs=%lu "
                 "hbTx=%lu hbAckRx=%lu dpTx=%lu drRx=%lu primerTO=%lu",
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
                 snapshot.controlPrimerTimeouts);
        sendControlResponse(response);
        return;
    }

    if (strcasecmp(verb, "DEFAULTS") == 0) {
        ModemConfig next = modemDefaultConfig();
        char error[80];
        int16_t state = radio.applyConfig(next);
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
        uint8_t n = radio.scanBand(startMHz, stopMHz, stepMHz,
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

    int16_t state = radio.applyConfig(next);
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

static void noteAckQueueDrop() {
    auto& sm = StatsManager::instance();
    sm.lock();
    sm.get().arqAckQueueDrops++;
    sm.unlock();
    noteQueueDrop();
}

static void finalizeReassembly(Reassembler& ra, CompletedFrameCache& completed) {
    PayloadFrame frame;
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
    ARQ_LOG("[ARQ RX:DONE seq=%04x tot=%u mask=%08lx len=%u]\n",
            ra.seq, ra.total_frags, static_cast<unsigned long>(ra.received_mask), frame.len);
    completed.seq         = ra.seq;
    completed.total_frags = ra.total_frags;
    completed.frame_len   = ra.frame_len;
    completed.frame_crc32 = ra.frame_crc32;
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
    ack.window_base   = 0;
    ack.received_mask = ra.received_mask;

    ARQ_LOG("[ARQ RX:ACK seq=%04x tot=%u mask=%08lx complete=%d]\n",
            ra.seq, ra.total_frags, static_cast<unsigned long>(ra.received_mask), (int)ra.isComplete());

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

static bool isReassemblyValid(const Reassembler& ra) {
    if (ra.frame_len == 0 || ra.frame_len > TNC_PAYLOAD_MAX_LEN) {
        return false;
    }
    uint16_t assembled_len = 0;
    for (uint8_t i = 0; i < ra.total_frags; i++) {
        assembled_len += ra.frag_len[i];
    }
    if (assembled_len != ra.frame_len) {
        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().arqReassemblyIntegrityDrops++;
        sm.unlock();
        return false;
    }
    uint32_t computed_crc = framing::computeCrc32(ra.buf, assembled_len);
    if (computed_crc != ra.frame_crc32) {
        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().arqFrameCrcErrors++;
        sm.unlock();
        return false;
    }
    return true;
}

// ── Task: Radio RX ────────────────────────────────────────────────────────────
// Receives radio packets, reassembles fragments into opaque payloads, pushes to rxQueue.
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
                    if (ra.isComplete() && !isReassemblyValid(ra)) {
                        completed.seq         = ra.seq;
                        completed.total_frags = ra.total_frags;
                        completed.frame_len   = ra.frame_len;
                        completed.frame_crc32 = ra.frame_crc32;
                        completed.ack_mask    = 0;  // Explicit fatal NACK for this completed sequence.
                        completed.tick_ms     = millis();
                        ra.reset();
                        continue;
                    }
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
                if (ra.isComplete() && !isReassemblyValid(ra)) {
                    completed.seq         = ra.seq;
                    completed.total_frags = ra.total_frags;
                    completed.frame_len   = ra.frame_len;
                    completed.frame_crc32 = ra.frame_crc32;
                    completed.ack_mask    = 0;
                    completed.tick_ms     = millis();
                    ra.reset();
                    continue;
                }
                sendAckForReassembly(ra);
                if (!ra.ack_pending && ra.isComplete()) {
                    finalizeReassembly(ra, completed);
                }
                continue;
            }
            if (ra.seq != FRAMING_SEQ_UNSET && !ra.isComplete()) {
                ARQ_LOG("[ARQ RX:DROP seq=%04x tot=%u mask=%08lx reason=idle]\n",
                        ra.seq, ra.total_frags, static_cast<unsigned long>(ra.received_mask));
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
            const uint16_t irq = radio.lastIrqStatus();
            if (err == ERR_RX_TIMEOUT || (irq & RADIOLIB_SX128X_IRQ_RX_TX_TIMEOUT)) {
                noteRadioRxTimeout();
            } else if (err == ERR_SYNCWORD || (irq & RADIOLIB_SX128X_IRQ_SYNC_WORD_ERROR)) {
                noteRadioRxSyncWordError();
            } else if (err == RADIOLIB_ERR_CRC_MISMATCH || (irq & RADIOLIB_SX128X_IRQ_CRC_ERROR)) {
                noteRadioRxCrcError();
            } else if (err == ERR_HEADER || (irq & RADIOLIB_SX128X_IRQ_HEADER_ERROR)) {
                noteRadioRxHeaderError();
            } else if (err == ERR_INVALID_PACKET_LEN || (err == RADIOLIB_ERR_NONE && pkt.len < 1)) {
                noteRadioRxInvalidLength();
            } else {
                noteRadioRxReadDataError();
            }
            noteRadioRxError();
            continue;
        }

        // ── CONTROL packet dispatch ──────────────────────────────────────────
        if (framingPacketType(pkt) == LinkPacketType::CONTROL) {
            noteLinkActivity();
            ControlFrame ctrl;
            if (!framingParseControl(pkt, ctrl)) {
                noteControlMalformed();
                continue;
            }
            if (ctrl.type == ControlType::HEARTBEAT) {
                {
                    auto& sm = StatsManager::instance();
                    sm.lock();
                    sm.get().controlHeartbeatRx++;
                    sm.unlock();
                }
                ControlFrame ack;
                ack.type = ControlType::HEARTBEAT_ACK;
                ack.seq  = ctrl.seq;
                Packet ackPkt;
                framingBuildControlPacket(ackPkt, ack);
                {
                    auto& sm = StatsManager::instance();
                    sm.lock();
                    sm.get().controlHeartbeatAckTx++;
                    sm.unlock();
                }
                radio.transmit(ackPkt, true);
                noteLinkActivity();
            } else if (ctrl.type == ControlType::DATA_PENDING) {
                {
                    auto& sm = StatsManager::instance();
                    sm.lock();
                    sm.get().controlDataPendingRx++;
                    sm.unlock();
                }
                ControlFrame ready;
                ready.type = ControlType::DATA_READY;
                ready.seq  = ctrl.seq;
                Packet readyPkt;
                framingBuildControlPacket(readyPkt, ready);
                {
                    auto& sm = StatsManager::instance();
                    sm.lock();
                    sm.get().controlDataReadyTx++;
                    sm.unlock();
                }
                radio.transmit(readyPkt, true);
                noteLinkActivity();
            } else if (ctrl.type == ControlType::HEARTBEAT_ACK ||
                       ctrl.type == ControlType::DATA_READY) {
                xQueueSend(controlQueue, &ctrl, 0);
            }
            continue;
        }

        if (framingPacketType(pkt) == LinkPacketType::ACK) {
            AckFrame ack;
            if (!framingParseAck(pkt, ack)) {
                noteMalformedAck();
                noteRadioRxError();
                continue;
            }
            noteLinkActivity();
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

        // ── Native single-packet RX path ─────────────────────────────────────
        if (framingPacketType(pkt) == LinkPacketType::NATIVE) {
            noteLinkActivity();
            uint8_t payload_len = 0;
            if (!framingParseNativePayload(pkt, payload_len)) {
                noteMalformedData();
                noteRadioRxError();
                continue;
            }
            PayloadFrame rxFrame;
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
            if (xQueueSend(rxQueue, &rxFrame, pdMS_TO_TICKS(RX_QUEUE_TIMEOUT_MS)) != pdPASS) {
                noteQueueDrop();
                noteRadioError();
            }
            continue;
        }

        // ── Generic fragmented ARQ RX path ────────────────────────────────────
        // In native mode, only NATIVE-type packets are expected. A DATA packet
        // here means the remote node is in generic mode — discard rather than
        // running ARQ reassembly, which would corrupt the wait_ms timing loop
        // and send spurious ACK transmissions.
        if (modemConfig.transportMode == TransportMode::NATIVE_PACKET) {
            noteRadioRxError();
            continue;
        }

        DataFrameHeader dataHdr;
        if (!framingParseDataHeader(pkt, dataHdr)) {
            noteMalformedData();
            noteRadioRxError();
            continue;
        }

        if (!framingValidateDataFragment(dataHdr)) {
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().arqFragmentMetadataDrops++;
            sm.unlock();
            continue;
        }

        const uint16_t seq          = dataHdr.seq;
        const uint8_t idx           = dataHdr.frag_index;
        const uint8_t total_frags   = dataHdr.total_frags;
        const uint8_t frag_data_len = dataHdr.payload_len;
        const uint16_t frame_len    = dataHdr.frame_len;
        const uint32_t frame_crc32  = dataHdr.frame_crc32;
        const bool    round_end     = dataHdr.round_end;
        const uint32_t now_ms       = millis();

        if (completed.seq == seq && (now_ms - completed.tick_ms) <= RADIO_DUP_CACHE_MS) {
            if (completed.total_frags == total_frags &&
                completed.frame_len == frame_len &&
                completed.frame_crc32 == frame_crc32) {
                ARQ_LOG("[ARQ RX:DUP seq=%04x tot=%u mask=%08lx]\n",
                        seq, total_frags, static_cast<unsigned long>(completed.ack_mask));
                // Re-ACK without re-delivering. Use AckFrame + Packet directly to
                // avoid a large Reassembler on the task stack.
                AckFrame dupAck;
                dupAck.seq           = completed.seq;
                dupAck.total_frags   = completed.total_frags;
                dupAck.window_base   = 0;
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
            } else {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().arqFragmentMetadataDrops++;
                sm.unlock();
            }
            continue;
        }

        if (ra.seq != FRAMING_SEQ_UNSET &&
            (now_ms - ra.last_tick_ms > reassemblyTimeoutMs(ra.total_frags))) {
            ARQ_LOG("[ARQ RX:DROP seq=%04x tot=%u mask=%08lx reason=timeout]\n",
                    ra.seq, ra.total_frags, static_cast<unsigned long>(ra.received_mask));
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
            ra.frame_len   = frame_len;
            ra.frame_crc32 = frame_crc32;
        } else if (ra.total_frags != total_frags || ra.frame_len != frame_len || ra.frame_crc32 != frame_crc32) {
            auto& sm = StatsManager::instance();
            sm.lock();
            sm.get().arqFragmentMetadataDrops++;
            sm.unlock();
            continue;
        }

        const uint32_t bit         = 1u << idx;
        const bool    is_new_frag  = !(ra.received_mask & bit);

        if (is_new_frag) {
            noteLinkActivity();
            memcpy(ra.buf + idx * FRAMING_FRAG_DATA, pkt.data + FRAMING_DATA_HDR_LEN, frag_data_len);
            ra.frag_len[idx]  = frag_data_len;
            ra.received_mask |= bit;
            ra.last_tick_ms   = now_ms;
            ra.last_rssi      = pkt.rssi;
            ra.ack_pending    = true;
            ra.ack_due_ms     = now_ms + ackFallbackDelayMs(total_frags);
        }

        ARQ_LOG("[ARQ RX:FRAG seq=%04x tot=%u idx=%u new=%d re=%d mask=%08lx flen=%u]\n",
                seq, total_frags, idx, (int)is_new_frag, (int)round_end,
                static_cast<unsigned long>(ra.received_mask), frag_data_len);

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
// Dequeues opaque payload frames, fragments into radio packets, retransmits missing
// fragments using selective-repeat ARQ until the receiver ACK bitmap is complete.
// When idle, sends periodic HEARTBEAT probes to maintain link-ready status.
static void radioTxTask(void*) {
    PayloadFrame frame;
    static uint16_t seq = 0;

    for (;;) {
        // Wait for a frame; wake periodically to probe idle link health.
        const uint32_t jitter_ms = esp_random() % (RADIO_HEARTBEAT_JITTER_MS + 1u);
        const TickType_t wait_ticks = pdMS_TO_TICKS(RADIO_HEARTBEAT_INTERVAL_MS + jitter_ms);

        if (xQueueReceive(txQueue, &frame, wait_ticks) != pdPASS) {
            // Timeout: send a heartbeat if the link has been idle long enough.
            if (isLinkIdle()) {
                g_control_seq++;
                const uint16_t cseq = g_control_seq;
                ControlFrame ctrl;
                ctrl.type = ControlType::HEARTBEAT;
                ctrl.seq  = cseq;
                Packet ctrlPkt;
                framingBuildControlPacket(ctrlPkt, ctrl);
                {
                    auto& sm = StatsManager::instance();
                    sm.lock();
                    sm.get().controlHeartbeatTx++;
                    sm.unlock();
                }
                if (radio.transmit(ctrlPkt, true) == RADIOLIB_ERR_NONE) {
                    noteLinkActivity();
                    ControlFrame resp;
                    const uint32_t hb_deadline = millis() + RADIO_CONTROL_ACK_TIMEOUT_MS;
                    while (millis() < hb_deadline) {
                        const uint32_t rem = hb_deadline - millis();
                        if (xQueueReceive(controlQueue, &resp, pdMS_TO_TICKS(rem)) == pdPASS) {
                            if (resp.type == ControlType::HEARTBEAT_ACK && resp.seq == cseq) {
                                auto& sm = StatsManager::instance();
                                sm.lock();
                                sm.get().controlHeartbeatAckRx++;
                                sm.unlock();
                                noteBidirectionalControl();
                                break;
                            }
                        }
                    }
                } else {
                    noteRadioTxError();
                }
                updateLinkStats();
            }
            continue;
        }

        if (frame.len == 0 || frame.len > TNC_PAYLOAD_MAX_LEN) {
            noteRadioError();
            continue;
        }

        // LBT-CSMA: skip entirely when disabled to avoid the 500µs dwell + SPI
        // read overhead on every TX.
        if (modemConfig.lbtRssiThresholdDbm != 0) {
            for (int lbt = 0; radio.isChannelBusy(); lbt++) {
                if (lbt >= RADIO_LBT_MAX_RETRIES) break;
                uint32_t backoff_ms = RADIO_LBT_BACKOFF_MIN_MS +
                    (esp_random() % (RADIO_LBT_BACKOFF_MAX_MS - RADIO_LBT_BACKOFF_MIN_MS + 1));
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            }
        }

        // ── Native single-packet TX path ─────────────────────────────────────
        if (modemConfig.transportMode == TransportMode::NATIVE_PACKET) {
            if (frame.len > FRAMING_NATIVE_MAX_PAYLOAD) {
                noteNativeOversizeDrop();
                continue;
            }
            Packet pkt;
            framingBuildNativePacket(pkt, frame.data, static_cast<uint8_t>(frame.len));
            {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().radioState = RadioState::TX;
                sm.unlock();
            }
            int16_t err = radio.transmit(pkt, true);
            {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().radioState = RadioState::IDLE;
                if (err == RADIOLIB_ERR_NONE) {
                    sm.get().nativeTxCount++;
                    sm.get().txCount++;
                    sm.get().txBytes += frame.len;
                    noteLinkActivity();
                } else {
                    sm.get().errorCount++;
                    sm.get().radioTxErrors++;
                    sm.get().radioState = RadioState::ERROR;
                }
                sm.unlock();
            }
            continue;
        }

        // ── Generic fragmented ARQ path ───────────────────────────────────────
        xQueueReset(ackQueue);
        xQueueReset(controlQueue);
        seq++;
        if (seq == FRAMING_SEQ_UNSET) {
            seq++;
        }
        const uint8_t total_frags = framingExpectedTotalFrags(frame.len);
        const uint32_t frame_crc32 = framing::computeCrc32(frame.data, frame.len);
        uint32_t pending_mask = framingExpectedMask(total_frags);
        bool delivered = false;

        auto& sm = StatsManager::instance();
        sm.lock();
        sm.get().arqFramesStarted++;
        sm.unlock();

        ARQ_LOG("[ARQ TX:START seq=%04x tot=%u len=%u]\n",
                seq, total_frags, frame.len);

        // DATA_PENDING/DATA_READY primer before multi-fragment bursts after idle.
        // Single-fragment frames skip this because they act as natural primers.
        if (total_frags > 1 && isLinkIdle()) {
            g_control_seq++;
            const uint16_t cseq = g_control_seq;
            ControlFrame ctrl;
            ctrl.type          = ControlType::DATA_PENDING;
            ctrl.seq           = cseq;
            ctrl.pending_frags = total_frags;
            Packet ctrlPkt;
            framingBuildControlPacket(ctrlPkt, ctrl);
            {
                auto& sm2 = StatsManager::instance();
                sm2.lock();
                sm2.get().controlDataPendingTx++;
                sm2.unlock();
            }
            bool primed = false;
            for (int retry = 0; retry <= RADIO_CONTROL_MAX_RETRIES && !primed; retry++) {
                if (radio.transmit(ctrlPkt, true) != RADIOLIB_ERR_NONE) {
                    noteRadioTxError();
                    break;
                }
                noteLinkActivity();
                ControlFrame resp;
                const uint32_t dp_deadline = millis() + RADIO_CONTROL_ACK_TIMEOUT_MS;
                while (millis() < dp_deadline) {
                    const uint32_t rem = dp_deadline - millis();
                    if (xQueueReceive(controlQueue, &resp, pdMS_TO_TICKS(rem)) == pdPASS) {
                        if (resp.type == ControlType::DATA_READY && resp.seq == cseq) {
                            auto& sm2 = StatsManager::instance();
                            sm2.lock();
                            sm2.get().controlDataReadyRx++;
                            sm2.unlock();
                            noteBidirectionalControl();
                            updateLinkStats();
                            primed = true;
                            break;
                        }
                    }
                }
                if (!primed) {
                    auto& sm2 = StatsManager::instance();
                    sm2.lock();
                    sm2.get().controlPrimerTimeouts++;
                    sm2.unlock();
                }
            }
            // Start DATA regardless of primer outcome per plan recommendation.
        }

        for (uint8_t round = 0; round < RADIO_ARQ_MAX_ROUNDS && pending_mask != 0; round++) {
            ARQ_LOG("[ARQ TX:ROUND seq=%04x tot=%u round=%u pending=%08lx]\n",
                    seq, total_frags, round, static_cast<unsigned long>(pending_mask));
            uint32_t sent_mask = 0;
            bool fatal_nack = false;
            for (uint8_t idx = 0; idx < total_frags; idx++) {
                const uint32_t bit = 1u << idx;
                if ((pending_mask & bit) == 0) {
                    continue;
                }

                const uint16_t offset = static_cast<uint16_t>(idx) * FRAMING_FRAG_DATA;
                const uint8_t chunk = static_cast<uint8_t>(
                    (frame.len - offset < FRAMING_FRAG_DATA)
                        ? (frame.len - offset)
                        : FRAMING_FRAG_DATA);
                const uint32_t later_pending = pending_mask & ~framingMaskThrough(idx);
                const bool round_end = later_pending == 0;

                if (sent_mask != 0) {
                    vTaskDelay(pdMS_TO_TICKS(RADIO_INTER_FRAG_DELAY_MS));
                }

                Packet pkt;
                framingBuildDataPacket(pkt, seq, idx, total_frags, round_end,
                                       frame.data + offset, chunk, frame.len, frame_crc32);

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
                    noteLinkActivity();
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
                if (ack.seq != seq || ack.total_frags != total_frags || ack.window_base != 0) {
                    continue;
                }
                const uint32_t expected_mask = framingExpectedMask(total_frags);
                const uint32_t ack_mask = ack.received_mask & expected_mask;
                if (ack_mask == 0) {
                    fatal_nack = true;
                    got_ack = true;
                    ARQ_LOG("[ARQ TX:NACK seq=%04x tot=%u round=%u pending=%08lx]\n",
                            seq, total_frags, round, static_cast<unsigned long>(pending_mask));
                    break;
                }
                ARQ_LOG("[ARQ TX:ACK seq=%04x tot=%u ack_mask=%08lx pend=%08lx->%08lx]\n",
                        seq, total_frags, static_cast<unsigned long>(ack_mask),
                        static_cast<unsigned long>(pending_mask),
                        static_cast<unsigned long>(pending_mask & ~ack_mask));
                pending_mask &= ~ack_mask;
                got_ack = true;
                break;
            }

            if (!got_ack) {
                auto& sm = StatsManager::instance();
                sm.lock();
                sm.get().arqAckTimeoutCount++;
                sm.unlock();
                ARQ_LOG("[ARQ TX:TIMEOUT seq=%04x tot=%u round=%u pending=%08lx]\n",
                        seq, total_frags, round, static_cast<unsigned long>(pending_mask));
            }

            if (fatal_nack) {
                break;
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
            ARQ_LOG("[ARQ TX:FAIL seq=%04x tot=%u pending=%08lx]\n",
                    seq, total_frags, static_cast<unsigned long>(pending_mask));
            noteRadioError();
        }
    }
}

// ── Task: Serial RX ───────────────────────────────────────────────────────────
// Reads KISS bytes from USB CDC, decodes frames, pushes to txQueue.
static void serialRxTask(void*) {
    static Kiss     decoder;
    static KissFrame kissFrame;
    static PayloadFrame frame;
    uint32_t last_rx_ms = 0;

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
    // Worst-case KISS encoded size: 2 bytes per payload byte + 3 framing bytes
    static constexpr size_t SERIAL_TX_CHUNK = 64;
    static uint8_t encBuf[TNC_PAYLOAD_MAX_LEN * 2 + 3];
    PayloadFrame frame;
    for (;;) {
        xQueueReceive(rxQueue, &frame, portMAX_DELAY);
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
            noteRadioError();
            continue;
        }
        size_t offset = 0;
        uint32_t stall_report_ms = 0;

        while (offset < encLen) {
            const size_t chunk = (encLen - offset > SERIAL_TX_CHUNK)
                ? SERIAL_TX_CHUNK
                : (encLen - offset);
            size_t written = Serial.write(encBuf + offset, chunk);
            if (written > 0) {
                offset += written;
                stall_report_ms = 0;
                if (offset < encLen) {
                    vTaskDelay(1);
                }
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
    }
}

// ── Task: Display ─────────────────────────────────────────────────────────────
static void displayTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(DISPLAY_REFRESH_MS));

    auto& sm = StatsManager::instance();
    sm.lock();
    Stats snapshot = sm.get();
    sm.unlock();

    display.update(snapshot);
    vTaskSuspend(nullptr);
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

#if SERIAL_CONSOLE_LOGS
    // Small delay to let the boot message be visible to the user.
    delay(500);
#endif

    BOOT_LOG_LN("[main] Initializing SX1280 radio transceiver...");
    int16_t radioErr = radio.begin(modemConfig);
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
    txQueue      = xQueueCreate(TX_QUEUE_DEPTH, sizeof(PayloadFrame));
    rxQueue      = xQueueCreate(RX_QUEUE_DEPTH, sizeof(PayloadFrame));
    ackQueue     = xQueueCreate(8, sizeof(AckFrame));
    controlQueue = xQueueCreate(4, sizeof(ControlFrame));
    if (txQueue == nullptr || rxQueue == nullptr || ackQueue == nullptr || controlQueue == nullptr) {
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
