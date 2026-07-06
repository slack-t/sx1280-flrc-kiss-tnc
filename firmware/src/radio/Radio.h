#pragma once
#include <RadioLib.h>
#include "../config.h"
#include "../kiss/Kiss.h"
#include "../config/ModemConfig.h"

// Thin wrapper to expose SX1280 command 0x1F (GetRssiInst).
// RadioLib defines RADIOLIB_SX128X_CMD_GET_RSSI_INST but never calls it;
// getRSSI() only reads the last-packet status and returns 0 between packets.
// A subclass can call protected getMod() from within its own scope.
class SX1280Ext : public SX1280 {
public:
    using SX1280::SX1280;
    int8_t getInstantRssi() {
        uint8_t raw = 0;
        getMod()->SPIreadStream(RADIOLIB_SX128X_CMD_GET_RSSI_INST, &raw, 1);
        return -(int8_t)(raw / 2);
    }
};

#define ERR_SPURIOUS_IRQ -1000
#define ERR_INVALID_PACKET_LEN -1001
#define ERR_RX_TIMEOUT -1002
#define ERR_SYNCWORD -1003
#define ERR_HEADER -1004

// Ownership: after boot, the MAC task (src/mac/Mac.cpp) is the sole caller of
// transmit(), startReceive(), readPacket(), isChannelBusy(), applyConfig() and
// scanBand(). Other tasks must go through the mac:: request functions.
class Radio {
public:
    // Initialise SX1280 with FLRC parameters from config.h.
    // Returns RADIOLIB_ERR_NONE (0) on success, or a RadioLib error code on failure.
    int16_t begin(const ModemConfig& config);
    int16_t applyConfig(const ModemConfig& config);
    const ModemConfig& config() const { return _config; }

    // Start continuous receive mode. DIO1 ISR will signal the rxSemaphore.
    void startReceive();

    // Transmit a packet. Blocks until TX complete.
    // If isLastFragment is true, it returns to RX mode. Otherwise, it stays in standby.
    // Returns RADIOLIB_ERR_NONE on success.
    int16_t transmit(const Packet& pkt, bool isLastFragment = true);

    // Read the last received packet from the SX1280 FIFO.
    // Call only after the rxSemaphore has been signalled.
    // Returns RADIOLIB_ERR_NONE on success.
    int16_t readPacket(Packet& pkt);

    // Perform a preamble-detect scan; returns true if another transmitter is
    // active on the channel (used for Listen-Before-Talk CSMA).
    bool isChannelBusy();

    // Sweep startMHz..stopMHz in stepMHz increments, dwelling dwellUs microseconds
    // per step, and store instantaneous RSSI (dBm) into rssiOut[0..n-1].
    // Holds the SPI mutex across the entire scan; restores original config on return.
    // Returns the number of samples written (≤ maxN).
    uint8_t scanBand(float startMHz, float stopMHz, float stepMHz,
                     uint32_t dwellUs, int8_t* rssiOut, uint8_t maxN);

    // Last received signal strength (dBm)
    int8_t lastRssi() const { return _lastRssi; }
    int16_t lastRadioErr() const { return _lastRadioErr; }
    uint16_t lastIrqStatus() const { return _lastIrqStatus; }
    uint16_t lastPacketLength() const { return _lastPacketLength; }

    // FreeRTOS semaphore given from the DIO1 ISR — the MAC task waits on this
    SemaphoreHandle_t rxSemaphore = nullptr;

    // Tick timestamp of the last DIO1 edge, captured in the ISR.
    volatile TickType_t lastDio1Tick = 0;

    // Set true while a blocking transmit() is in progress so the ISR does not
    // spuriously signal rxSemaphore on the TX-done DIO1 pulse.
    volatile bool _txActive = false;

private:
    SX1280Ext _radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);

    SemaphoreHandle_t _spiMutex = nullptr;

    int8_t _lastRssi = 0;
    int16_t _lastRadioErr = RADIOLIB_ERR_NONE;
    uint16_t _lastIrqStatus = 0;
    uint16_t _lastPacketLength = 0;
    ModemConfig _config;

    // SPI call without taking _spiMutex — use only when the mutex is already held.
    // forceReset=true (after TX or error): clears IRQ state fully.
    // forceReset=false (RX-to-RX): skips redundant re-init, faster turnaround.
    void _startReceiveNoLock(bool forceReset = false);
    int16_t _applyConfigNoLock(const ModemConfig& config);

    static void IRAM_ATTR _dio1Isr();
};
