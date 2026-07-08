#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "../radio/Radio.h"
#include "../config/ModemConfig.h"

// Single-owner MAC task. After mac::start() the MAC task is the ONLY caller
// of radio transmit and receive-state transition APIs. Code on other tasks
// must use the request functions below instead of touching the Radio object.
namespace mac {

// Returned by requestApplyConfig() when the MAC layer is not running.
static constexpr int16_t ERR_MAC_NOT_STARTED = -1100;

struct Init {
    Radio*        radio;
    QueueHandle_t txQueue;   // PayloadFrame: host → radio
    QueueHandle_t rxQueue;   // PayloadFrame: radio → host
};

// Create MAC resources and spawn the task on core 1.
// Returns false on allocation or task-spawn failure (caller must treat as fatal).
bool start(const Init& init);

// Wake the MAC task after enqueueing to txQueue (call after xQueueSend).
void notifyTxWork();

// Execute radio.applyConfig() in MAC task context, between frames.
// Blocks until the MAC task has applied the config. Returns RadioLib status.
int16_t requestApplyConfig(const ModemConfig& cfg);

// Execute radio.scanBand() in MAC task context, between frames.
// Blocks until the scan completes. Returns the number of samples written.
uint8_t requestScanBand(float startMHz, float stopMHz, float stepMHz,
                        uint32_t dwellUs, int8_t* rssiOut, uint8_t maxN);

// Refresh link-health fields in Stats (linkReady/linkState/linkAgeMs).
// Safe to call from any task.
void refreshLinkStats();

enum class DiagnosticEgressMode : uint8_t {
    OPEN = 0,
    BLOCKED = 1,
    ONE_SHOT_ZERO_CREDIT = 2,
};

// Diagnostic receiver egress controls for hardware ARQ credit-stall tests.
// ONE_SHOT_ZERO_CREDIT allows one delivery, then reports zero receiver credits.
void setDiagnosticEgressMode(DiagnosticEgressMode mode);
DiagnosticEgressMode diagnosticEgressMode();

}  // namespace mac
