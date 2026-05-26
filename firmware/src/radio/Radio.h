#pragma once
#include <RadioLib.h>
#include "../config.h"
#include "../kiss/Kiss.h"

#define ERR_SPURIOUS_IRQ -1000

class Radio {
public:
    // Initialise SX1280 with FLRC parameters from config.h.
    // Returns RADIOLIB_ERR_NONE (0) on success, or a RadioLib error code on failure.
    int16_t begin();

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

    // Last received signal strength (dBm)
    int8_t lastRssi() const { return _lastRssi; }

    // FreeRTOS semaphore given from the DIO1 ISR — radio task waits on this
    SemaphoreHandle_t rxSemaphore = nullptr;

    // Set true while a blocking transmit() is in progress so the ISR does not
    // spuriously signal rxSemaphore on the TX-done DIO1 pulse.
    volatile bool _txActive = false;

private:
    SX1280 _radio = new Module(RADIO_NSS, RADIO_DIO1, RADIO_RST, RADIO_BUSY);

    SemaphoreHandle_t _spiMutex = nullptr;

    int8_t _lastRssi = 0;

    // SPI call without taking _spiMutex — use only when the mutex is already held.
    // forceReset=true (after TX or error): clears IRQ state fully.
    // forceReset=false (RX-to-RX): skips redundant re-init, faster turnaround.
    void _startReceiveNoLock(bool forceReset = false);

    static void IRAM_ATTR _dio1Isr();
};
