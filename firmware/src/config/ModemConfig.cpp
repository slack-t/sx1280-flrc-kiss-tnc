#include "ModemConfig.h"
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static constexpr const char* NVS_NAMESPACE = "flrc";
static constexpr const char* NVS_KEY = "modem";

ModemConfig modemDefaultConfig() {
    return ModemConfig{};
}

ModemConfigSource modemLoadConfig(ModemConfig& cfg) {
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, true)) {
        cfg = modemDefaultConfig();
        return ModemConfigSource::DEFAULTS;
    }
    const size_t got = prefs.getBytes(NVS_KEY, &cfg, sizeof(cfg));
    prefs.end();
    char error[64];
    if (got != sizeof(cfg) || cfg.magic != MODEM_CONFIG_MAGIC || !modemValidateConfig(cfg, error, sizeof(error))) {
        cfg = modemDefaultConfig();
        return ModemConfigSource::DEFAULTS;
    }
    return ModemConfigSource::NVS;
}

bool modemSaveConfig(const ModemConfig& cfg) {
    char error[64];
    if (!modemValidateConfig(cfg, error, sizeof(error))) {
        return false;
    }
    Preferences prefs;
    if (!prefs.begin(NVS_NAMESPACE, false)) {
        return false;
    }
    const size_t written = prefs.putBytes(NVS_KEY, &cfg, sizeof(cfg));
    prefs.end();
    return written == sizeof(cfg);
}

void modemClearConfig() {
    Preferences prefs;
    if (prefs.begin(NVS_NAMESPACE, false)) {
        prefs.remove(NVS_KEY);
        prefs.end();
    }
}

static bool isAllowedBitrate(float kbps) {
    const int value = static_cast<int>(kbps + 0.5f);
    return value == 260 || value == 325 || value == 520 ||
           value == 650 || value == 1040 || value == 1300;
}

static bool isAllowedPreamble(uint8_t bits) {
    return bits == 4 || bits == 8 || bits == 12 || bits == 16 ||
           bits == 20 || bits == 24 || bits == 28 || bits == 32;
}

bool modemValidateConfig(const ModemConfig& cfg, char* error, size_t errorLen) {
    auto fail = [&](const char* msg) {
        if (error && errorLen > 0) {
            snprintf(error, errorLen, "%s", msg);
        }
        return false;
    };

    if (cfg.freqMHz < 2400.0f || cfg.freqMHz > 2500.0f) return fail("freq must be 2400..2500 MHz");
    if (!isAllowedBitrate(cfg.bitrateKbps)) return fail("bitrate must be 260,325,520,650,1040,1300");
    if (!(cfg.codingRate == 2 || cfg.codingRate == 3 || cfg.codingRate == 4)) return fail("cr must be 2,3,4");
    if (cfg.txPowerDbm < -18 || cfg.txPowerDbm > 13) return fail("power must be -18..13 dBm");
    if (!isAllowedPreamble(cfg.preambleBits)) return fail("preamble must be 4,8,12,16,20,24,28,32");
    if (!(cfg.shaping == RADIOLIB_SHAPING_0_5 || cfg.shaping == RADIOLIB_SHAPING_1_0)) return fail("bt must be 0 or 1");
    return true;
}

void modemFormatSyncWord(const uint8_t sync[RADIO_SYNC_WORD_LEN], char* out, size_t outLen) {
    if (!out || outLen == 0) return;
    snprintf(out, outLen, "%02x%02x%02x%02x", sync[0], sync[1], sync[2], sync[3]);
}

bool modemParseSyncWord(const char* text, uint8_t out[RADIO_SYNC_WORD_LEN]) {
    if (!text || strlen(text) != RADIO_SYNC_WORD_LEN * 2) {
        return false;
    }
    for (uint8_t i = 0; i < RADIO_SYNC_WORD_LEN; i++) {
        char byteText[3] = { text[i * 2], text[i * 2 + 1], '\0' };
        char* end = nullptr;
        long value = strtol(byteText, &end, 16);
        if (!end || *end != '\0' || value < 0 || value > 255) {
            return false;
        }
        out[i] = static_cast<uint8_t>(value);
    }
    return true;
}

void modemFormatConfig(const ModemConfig& cfg, char* out, size_t outLen) {
    char sync[16];
    modemFormatSyncWord(cfg.syncWord, sync, sizeof(sync));
    snprintf(out, outLen,
             "freq=%.3f bitrate=%.0f cr=%u power=%d preamble=%u bt=%u sync=%s lbt=%d",
             cfg.freqMHz,
             cfg.bitrateKbps,
             cfg.codingRate,
             cfg.txPowerDbm,
             cfg.preambleBits,
             cfg.shaping == RADIOLIB_SHAPING_0_5 ? 0u : 1u,
             sync,
             cfg.lbtRssiThresholdDbm);
}

uint16_t modemConfigChecksum(const ModemConfig& cfg) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&cfg);
    const size_t len = sizeof(cfg);
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(bytes[i]) << 8;
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000u) {
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

const char* modemConfigSourceName(ModemConfigSource source) {
    switch (source) {
        case ModemConfigSource::NVS:
            return "nvs";
        case ModemConfigSource::RESET_HELD:
            return "reset-held";
        case ModemConfigSource::DEFAULTS:
        default:
            return "defaults";
    }
}
