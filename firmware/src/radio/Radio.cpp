#include "Radio.h"
#include <SPI.h>
#include <string.h>

#if SERIAL_CONSOLE_LOGS
#define SERIAL_LOG(...) Serial.printf(__VA_ARGS__)
#define SERIAL_LOG_LN(msg) Serial.println(msg)
#else
#define SERIAL_LOG(...) ((void)0)
#define SERIAL_LOG_LN(msg) ((void)0)
#endif

static Radio* _radioInstance = nullptr;

int16_t Radio::begin(const ModemConfig& config) {
    _radioInstance = this;

    // Counting semaphore: multiple DIO1 edges (fragments arriving while radioRxTask
    // is still processing the previous one) must not be silently discarded.
    rxSemaphore = xSemaphoreCreateCounting(16, 0);
    _spiMutex   = xSemaphoreCreateMutex();

    SPI.begin(RADIO_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);
    SERIAL_LOG("[radio] SPI started (SCK=%d MISO=%d MOSI=%d NSS=%d)\n",
               RADIO_SCK, RADIO_MISO, RADIO_MOSI, RADIO_NSS);

    int16_t state = _applyConfigNoLock(config);
    if (state != RADIOLIB_ERR_NONE) {
        SERIAL_LOG("[radio] beginFLRC failed! Error code: %d\n", state);
        return state;
    }
    SERIAL_LOG_LN("[radio] beginFLRC -> OK");

    // DIO1 fires on both RX done and TX done. The ISR guards against the
    // TX-done pulse with _txActive so only genuine RX events wake radioRxTask.
    _radio.setDio1Action(_dio1Isr);

    SERIAL_LOG_LN("[radio] init OK");
    return RADIOLIB_ERR_NONE;
}

int16_t Radio::_applyConfigNoLock(const ModemConfig& config) {
    // RadioLib 6.x beginFLRC: (freq_MHz, bitrate_kbps, cr, power_dBm, preamble_bits, BT)
    int16_t state = _radio.beginFLRC(
        config.freqMHz,
        config.bitrateKbps,
        config.codingRate,
        config.txPowerDbm,
        config.preambleBits,
        config.shaping
    );
    if (state != RADIOLIB_ERR_NONE) {
        return state;
    }
    uint8_t syncWord[RADIO_SYNC_WORD_LEN];
    memcpy(syncWord, config.syncWord, sizeof(syncWord));
    state = _radio.setSyncWord(syncWord, RADIO_SYNC_WORD_LEN);
    if (state != RADIOLIB_ERR_NONE) {
        return state;
    }
    _config = config;
    return RADIOLIB_ERR_NONE;
}

int16_t Radio::applyConfig(const ModemConfig& config) {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);
    _txActive = true;
    int16_t state = _applyConfigNoLock(config);
    if (state == RADIOLIB_ERR_NONE) {
        _startReceiveNoLock(true);
    }
    _txActive = false;
    xSemaphoreGive(_spiMutex);
    return state;
}

void Radio::_startReceiveNoLock(bool forceReset) {
    if (forceReset) {
        // After TX or error: SX1280 packet params may have been altered.
        // Reset preamble length before re-entering RX.
        _radio.setPreambleLength(_config.preambleBits);
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

    // Keep _txActive set to true until we've returned the radio to RX mode
    // and pulled DIO1 low, so that the ISR completely ignores any late TX_DONE edges.
    if (isLastFragment || state != RADIOLIB_ERR_NONE) {
        _startReceiveNoLock(true);
        _txActive = false;
    }

    xSemaphoreGive(_spiMutex);
    return state;
}

int16_t Radio::readPacket(Packet& pkt) {
    xSemaphoreTake(_spiMutex, portMAX_DELAY);

    // Verify that this is a genuine RX_DONE event on the SX1280 hardware.
    // This prevents spurious interrupts (like a late/delayed TX_DONE edge)
    // from leaving the radio deaf or triggering false reads.
    uint16_t irq = _radio.getIrqStatus();
    _lastIrqStatus = irq;
    if (!(irq & RADIOLIB_SX128X_IRQ_RX_DONE)) {
        // Force the radio back into receive mode so it doesn't stay deaf.
        // _startReceiveNoLock(true) internally clears the chip's interrupt registers.
        _startReceiveNoLock(true);
        xSemaphoreGive(_spiMutex);
        _lastRadioErr = ERR_SPURIOUS_IRQ;
        return ERR_SPURIOUS_IRQ;
    }

    size_t len = _radio.getPacketLength();
    _lastPacketLength = static_cast<uint16_t>(len);
    if (len == 0 || len > PACKET_MAX_LEN) {
        // Clear interrupts and force-reset RX mode to flush the FIFO.
        // _startReceiveNoLock(true) internally clears the chip's interrupt registers.
        _startReceiveNoLock(true);
        xSemaphoreGive(_spiMutex);
        _lastRadioErr = ERR_INVALID_PACKET_LEN;
        return ERR_INVALID_PACKET_LEN;
    }

    int16_t state = _radio.readData(pkt.data, len);
    _lastRadioErr = state;
    pkt.len = (state == RADIOLIB_ERR_NONE) ? static_cast<uint8_t>(len) : 0;

    // Skip RSSI SPI reads for intermediate fragments — saves ~100µs of
    // turnaround time between consecutive fragments of the same payload frame.
    bool shouldQueryRssi = true;
    if (state == RADIOLIB_ERR_NONE && pkt.len > 0) {
        DataFrameHeader header;
        if (framingParseDataHeader(pkt, header) && !header.round_end) {
            shouldQueryRssi = false;
        }
    }

    if (shouldQueryRssi) {
        _lastRssi = static_cast<int8_t>(_radio.getRSSI());
    }
    pkt.rssi = _lastRssi;

    // RX-to-RX: force full reset to prevent pay load lockout states
    _startReceiveNoLock(true);
    xSemaphoreGive(_spiMutex);
    return state;
}

bool Radio::isChannelBusy() {
    // Radio is guaranteed to be in RX mode here: transmit() calls _startReceiveNoLock()
    // after the last fragment, and setup() calls startReceive() at boot.
    //
    // Do NOT call startReceive() here. The SX1280 startReceive() resets the RX state
    // machine and overwrites the FIFO. If radioRxTask has received a packet but hasn't
    // called readPacket() yet, that packet is silently destroyed.
    //
    // Instead, dwell in the already-active RX mode and read instantaneous RSSI directly
    // via SX1280 command 0x15 (GetInstantaneousRssi), which is non-destructive.
    xSemaphoreTake(_spiMutex, portMAX_DELAY);
    delayMicroseconds(RADIO_LBT_SENSE_US);
    float rssi = _radio.getRSSI();
    xSemaphoreGive(_spiMutex);
    return _config.lbtRssiThresholdDbm != 0 && rssi > (float)_config.lbtRssiThresholdDbm;
}

void IRAM_ATTR Radio::_dio1Isr() {
    // Suppress the TX-done DIO1 pulse — only signal on genuine RX events.
    if (_radioInstance && _radioInstance->rxSemaphore && !_radioInstance->_txActive) {
        // Fast hardware check: DIO1 must be physically HIGH for a genuine RX_DONE interrupt.
        // If the interrupt is serviced late (after TX is done and DIO1 is pulled low),
        // digitalRead will return LOW, allowing us to safely ignore the spurious event.
        if (digitalRead(RADIO_DIO1) == HIGH) {
            BaseType_t higher = pdFALSE;
            xSemaphoreGiveFromISR(_radioInstance->rxSemaphore, &higher);
            portYIELD_FROM_ISR(higher);
        }
    }
}
