#include "Radio.h"
#include <SPI.h>

static Radio* _radioInstance = nullptr;

int16_t Radio::begin() {
    _radioInstance = this;

    rxSemaphore = xSemaphoreCreateBinary();
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

void Radio::_startReceiveNoLock() {
    _radio.startReceive();
}

void Radio::startReceive() {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);
    _startReceiveNoLock();
    xSemaphoreGive(_spiMutex);
}

int16_t Radio::transmit(const Packet& pkt) {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);

    _txActive = true;
    int16_t state = _radio.transmit(const_cast<uint8_t*>(pkt.data), pkt.len);
    _txActive = false;

    // Always return to RX — even on TX failure the node must not stay deaf.
    _startReceiveNoLock();

    xSemaphoreGive(_spiMutex);
    return state;
}

int16_t Radio::readPacket(Packet& pkt) {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);

    // Verify that this is a genuine RX_DONE event on the SX1280 hardware.
    // This prevents spurious interrupts (like a late/delayed TX_DONE edge)
    // from triggering false reads, which leads to packet loops or duplicate (DUP) pings.
    uint16_t irq = _radio.getIrqStatus();
    if (!(irq & RADIOLIB_SX128X_IRQ_RX_DONE)) {
        _radio.clearIrqStatus();
        _startReceiveNoLock();
        xSemaphoreGive(_spiMutex);
        return RADIOLIB_ERR_SPI_CMD_INVALID;
    }

    size_t len = _radio.getPacketLength();
    if (len == 0 || len > PACKET_MAX_LEN) {
        _startReceiveNoLock();
        xSemaphoreGive(_spiMutex);
        return RADIOLIB_ERR_PACKET_TOO_LONG;
    }

    int16_t state = _radio.readData(pkt.data, len);
    pkt.len = (state == RADIOLIB_ERR_NONE) ? static_cast<uint8_t>(len) : 0;

    _lastRssi = static_cast<int8_t>(_radio.getRSSI());
    _lastSnr  = _radio.getSNR();

    pkt.rssi = _lastRssi;
    pkt.snr  = _lastSnr;

    _startReceiveNoLock();
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
