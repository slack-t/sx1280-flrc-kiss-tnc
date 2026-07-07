#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../config.h"
#include "../framing/Framing.h"
#include "SerialIntegrity.h"

// KISS framing constants (RFC 1055 / KISS spec)
static constexpr uint8_t KISS_FEND  = 0xC0;
static constexpr uint8_t KISS_FESC  = 0xDB;
static constexpr uint8_t KISS_TFEND = 0xDC;
static constexpr uint8_t KISS_TFESC = 0xDD;

// KISS port/command byte for data frames on port 0
static constexpr uint8_t KISS_DATA_FRAME = 0x00;
static constexpr uint8_t KISS_CONTROL_FRAME = 0x0F;
static constexpr uint16_t KISS_FRAME_PAYLOAD_MAX_LEN =
    TNC_PAYLOAD_MAX_LEN + SERIAL_INTEGRITY_HDR_LEN;

struct KissFrame {
    uint8_t command = 0;
    uint8_t data[KISS_FRAME_PAYLOAD_MAX_LEN];
    uint16_t len = 0;
};

enum class KissDecodeResult : uint8_t {
    NONE = 0,
    FRAME = 1,
    OVERSIZE_DROP = 2,
    INVALID_ESCAPE_DROP = 3,
};

class Kiss {
public:
    // Encode an opaque payload frame into a KISS data frame.
    // outBuf must be at least (2 * KISS_FRAME_PAYLOAD_MAX_LEN + 3) bytes.
    // Returns number of bytes written to outBuf, or 0 if outBuf is too small.
    static size_t encode(const PayloadFrame& frame, uint8_t* outBuf, size_t outBufLen);

    // Feed one byte at a time into the decoder.
    // Returns true when a complete frame has been decoded into frame.
    bool decode(uint8_t byte, PayloadFrame& frame);
    bool decodeFrame(uint8_t byte, KissFrame& frame);
    KissDecodeResult decodeFrameEx(uint8_t byte, KissFrame& frame);

    // Reset decoder state (e.g. on sync loss)
    void reset();

    static size_t encodeFrame(uint8_t command, const uint8_t* payload, uint16_t payloadLen,
                              uint8_t* outBuf, size_t outBufLen);

private:
    enum class State : uint8_t { IDLE, IN_FRAME, ESCAPE };

    State    _state    = State::IDLE;
    uint8_t  _buf[KISS_FRAME_PAYLOAD_MAX_LEN + 1];
    uint16_t _len      = 0;
    bool     _overflow = false;
};
