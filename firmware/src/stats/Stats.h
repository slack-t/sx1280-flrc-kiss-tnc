#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

enum class RadioState : uint8_t {
    IDLE,
    TX,
    RX,
    ERROR,
};

struct Stats {
    int8_t   rssi        = 0;
    float    snr         = 0.0f;
    uint32_t txCount     = 0;
    uint32_t rxCount     = 0;
    uint32_t errorCount  = 0;
    uint32_t txBytes     = 0;
    uint32_t rxBytes     = 0;
    RadioState radioState = RadioState::IDLE;
    float    freqMHz     = 0.0f;
    uint32_t bitrateKbps = 0;
};

// Singleton wrapper with a FreeRTOS mutex.
// Always use lock()/unlock() around reads and writes.
class StatsManager {
public:
    static StatsManager& instance();

    void     lock();
    void     unlock();
    Stats&   get();          // call only while locked

private:
    StatsManager();
    Stats              _stats;
    SemaphoreHandle_t  _mutex;
};
