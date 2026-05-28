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
    uint32_t txCount     = 0;
    uint32_t rxCount     = 0;
    uint32_t errorCount  = 0;
    uint32_t txBytes     = 0;
    uint32_t rxBytes     = 0;
    uint32_t arqFramesStarted      = 0;
    uint32_t arqFramesCompleted    = 0;
    uint32_t arqFramesFailed       = 0;
    uint32_t arqRetryCount       = 0;
    uint32_t arqAckTimeoutCount  = 0;
    uint32_t arqReassemblyDrops  = 0;
    uint32_t arqAckTxCount       = 0;
    uint32_t arqAckRxCount       = 0;
    uint32_t arqAckTxErrors      = 0;
    uint32_t arqDuplicateSuppressed = 0;
    uint32_t arqQueueDrops       = 0;
    uint32_t arqIdentityResets   = 0;
    uint32_t arqAckQueueDrops    = 0;
    uint32_t radioTxErrors       = 0;
    uint32_t radioRxErrors       = 0;
    uint32_t txQueueWaitCount    = 0;
    uint32_t rxQueueWaitCount    = 0;
    uint32_t serialTxZeroWrites  = 0;
    uint32_t serialTxTimeouts    = 0;
    uint32_t serialTxEncodeFails = 0;
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
