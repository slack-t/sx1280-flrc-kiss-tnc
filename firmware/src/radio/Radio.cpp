#include "Radio.h"
#include <SPI.h>

static Radio* _radioInstance = nullptr;

int16_t Radio::begin() {
    _radioInstance = this;

    // Counting semaphore: multiple DIO1 edges (fragments arriving while radioRxTask
    // is still processing the previous one) must not be silently discarded.
    rxSemaphore = xSemaphoreCreateCounting(16, 0);
    _spiMutex   = xSemaphoreCreateMutex();

    SPI.begin(RADIO_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);
    Serial.printf("[radio] SPI started (SCK=%d MISO=%d MOSI=%d NSS=%d)\n",
                  RADIO_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);

    // RadioLib 6.x beginFLRC: (freq_MHz, bitrate_kbps, cr, power_dBm, preamble_bits, BT)
    // Sync word is NOT a parameter — set separately via setSyncWord() below.
    int16_t state = _radio.beginFLRC(
        RADIO_FREQ_MHZ,
        RADIO_BITRATE_KBPS,
        RADIO_CODING_RATE,
        TX_POWER_DBM,
        RADIO_PREAMBLE_BITS,
        RADIO_BT
    );
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[radio] beginFLRC failed! Error code: %d\n", state);
        return state;
    }
    Serial.println("[radio] beginFLRC -> OK");

    uint8_t syncWord[] = RADIO_SYNC_WORD_BYTES;
    state = _radio.setSyncWord(syncWord, RADIO_SYNC_WORD_LEN);
    if (state != RADIOLIB_ERR_NONE) {
        Serial.printf("[radio] setSyncWord failed! Error code: %d\n", state);
        return state;
    }
    Serial.println("[radio] setSyncWord -> OK");

    // DIO1 fires on both RX done and TX done. The ISR guards against the
    // TX-done pulse with _txActive so only genuine RX events wake radioRxTask.
    _radio.setDio1Action(_dio1Isr);

    Serial.println("[radio] init OK");
    return RADIOLIB_ERR_NONE;
}

void Radio::_startReceiveNoLock(bool forceReset) {
    if (forceReset) {
        // After TX or error: SX1280 packet params may have been altered.
        // Reset preamble length before re-entering RX.
        _radio.setPreambleLength(RADIO_PREAMBLE_BITS);
    }
    _radio.startReceive();
}

void Radio::startReceive() {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);
    _startReceiveNoLock(true);
    xSemaphoreGive(_spiMutex);
}

int16_t Radio::transmit(const Packet& pkt, bool isLastFragment) {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);

    _txActive = true;
    int16_t state = _radio.transmit(const_cast<uint8_t*>(pkt.data), pkt.len);
    _txActive = false;

    // Only return to RX on the final fragment (or if TX failed, as a safety fallback)
    if (isLastFragment || state != RADIOLIB_ERR_NONE) {
        _startReceiveNoLock(true);
    }

    xSemaphoreGive(_spiMutex);
    return state;
}

int16_t Radio::readPacket(Packet& pkt) {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);

    size_t len = _radio.getPacketLength();
    if (len == 0) {
        // Pre-RX_DONE sub-event (preamble/sync word detected, or CRC_ERROR with no
        // buffered payload). Do NOT call startReceive — that would abort an ongoing
        // reception and create a cascade of spurious DIO1 pulses. The radio is
        // already in RX mode; just release the mutex and go back to sleep.
        xSemaphoreGive(_spiMutex);
        return RADIOLIB_ERR_PACKET_TOO_LONG;
    }
    if (len > PACKET_MAX_LEN) {
        // Oversized packet: it is fully buffered but we can't read it.
        // Restart RX to flush the FIFO.
        _startReceiveNoLock(true);
        xSemaphoreGive(_spiMutex);
        return RADIOLIB_ERR_PACKET_TOO_LONG;
    }

    int16_t state = _radio.readData(pkt.data, len);
    pkt.len = (state == RADIOLIB_ERR_NONE) ? static_cast<uint8_t>(len) : 0;

    // Skip RSSI/SNR SPI reads for intermediate fragments — saves ~100µs of
    // turnaround time between consecutive fragments of the same IP frame.
    bool shouldQueryRssi = true;
    if (state == RADIOLIB_ERR_NONE && pkt.len > 0) {
        const uint8_t header   = pkt.data[0];
        const bool    is_split = (header & FRAMING_FLAG_SPLIT) != 0;
        const bool    is_last  = (header & FRAMING_FLAG_LAST)  != 0;
        if (is_split && !is_last) {
            shouldQueryRssi = false;
        }
    }

    if (shouldQueryRssi) {
        _lastRssi = static_cast<int8_t>(_radio.getRSSI());
        _lastSnr  = _radio.getSNR();
    }
    pkt.rssi = _lastRssi;
    pkt.snr  = _lastSnr;

    // RX-to-RX: skip setPreambleLength since packet params haven't changed.
    _startReceiveNoLock(false);
    xSemaphoreGive(_spiMutex);
    return state;
}

bool Radio::isChannelBusy() {
    // SX1280 scanChannel() is LoRa CAD only — it returns RADIOLIB_ERR_WRONG_MODEM in
    // FLRC mode, which would make every call report the channel as permanently busy.
    // This is a dedicated P2P link; collisions cannot occur, so LBT is not needed.
    return false;
}

void IRAM_ATTR Radio::_dio1Isr() {
    // Suppress the TX-done DIO1 pulse — only signal on genuine RX events.
    if (_radioInstance && _radioInstance->rxSemaphore && !_radioInstance->_txActive) {
        BaseType_t higher = pdFALSE;
        xSemaphoreGiveFromISR(_radioInstance->rxSemaphore, &higher);
        portYIELD_FROM_ISR(higher);
    }
}
