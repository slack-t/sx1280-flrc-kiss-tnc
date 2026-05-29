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
    uint32_t arqFragmentMetadataDrops = 0;
    uint32_t arqReassemblyIntegrityDrops = 0;
    uint32_t arqFrameCrcErrors   = 0;
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
    uint32_t serialRxIntegrityDrops = 0;
    uint32_t rxSpuriousIrqCount  = 0;
    uint32_t rxInvalidLengthCount = 0;
    uint32_t rxCrcErrorCount     = 0;
    uint32_t rxHeaderErrorCount  = 0;
    uint32_t rxSyncWordErrorCount = 0;
    uint32_t rxTimeoutCount      = 0;
    uint32_t rxReadDataErrorCount = 0;
    uint32_t rxMalformedAckCount = 0;
    uint32_t rxMalformedDataCount = 0;
    uint32_t kissMalformedFrameCount = 0;
    uint32_t kissOversizeDropCount = 0;
    uint32_t nativeTxCount           = 0;
    uint32_t nativeRxCount           = 0;
    uint32_t nativeOversizeDropCount = 0;
    int16_t  lastRadioErr        = 0;
    uint16_t lastIrqStatus       = 0;
    uint16_t lastPacketLength    = 0;
    RadioState radioState = RadioState::IDLE;
    uint8_t  transportMode       = 0;
    float    freqMHz     = 0.0f;
    uint32_t bitrateKbps = 0;
    uint8_t  codingRate  = 0;
    int8_t   txPowerDbm  = 0;
    uint8_t  preambleBits = 0;
    uint8_t  btShaping   = 0;
    uint32_t syncWord    = 0;
    int16_t  lbtRssiThresholdDbm = 0;
    uint16_t configCrc16 = 0;
    uint8_t  configVersion = 0;
    uint8_t  configSource = 0;
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
