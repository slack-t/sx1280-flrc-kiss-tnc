#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <RadioLib.h>
#include "../config.h"

static constexpr uint32_t MODEM_CONFIG_MAGIC = 0x464c5243u; // "FLRC"
static constexpr uint8_t MODEM_CONFIG_PROTOCOL_VERSION = 1;

enum class ModemConfigSource : uint8_t {
    DEFAULTS = 0,
    NVS = 1,
    RESET_HELD = 2,
};

struct ModemConfig {
    uint32_t magic = MODEM_CONFIG_MAGIC;
    float freqMHz = RADIO_FREQ_MHZ;
    float bitrateKbps = RADIO_BITRATE_KBPS;
    uint8_t codingRate = RADIO_CODING_RATE;
    int8_t txPowerDbm = TX_POWER_DBM;
    uint8_t preambleBits = RADIO_PREAMBLE_BITS;
    uint8_t shaping = RADIO_BT;
    uint8_t syncWord[RADIO_SYNC_WORD_LEN] = RADIO_SYNC_WORD_BYTES;
    int16_t lbtRssiThresholdDbm = RADIO_LBT_RSSI_THRESHOLD_DBM;
};

ModemConfig modemDefaultConfig();
ModemConfigSource modemLoadConfig(ModemConfig& cfg);
bool modemSaveConfig(const ModemConfig& cfg);
void modemClearConfig();
bool modemValidateConfig(const ModemConfig& cfg, char* error, size_t errorLen);
void modemFormatConfig(const ModemConfig& cfg, char* out, size_t outLen);
bool modemParseSyncWord(const char* text, uint8_t out[RADIO_SYNC_WORD_LEN]);
void modemFormatSyncWord(const uint8_t sync[RADIO_SYNC_WORD_LEN], char* out, size_t outLen);
uint16_t modemConfigChecksum(const ModemConfig& cfg);
const char* modemConfigSourceName(ModemConfigSource source);
