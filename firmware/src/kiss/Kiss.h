#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../config.h"
#include "../framing/Framing.h"

// KISS framing constants (RFC 1055 / KISS spec)
static constexpr uint8_t KISS_FEND  = 0xC0;
static constexpr uint8_t KISS_FESC  = 0xDB;
static constexpr uint8_t KISS_TFEND = 0xDC;
static constexpr uint8_t KISS_TFESC = 0xDD;

// KISS port/command byte for data frames on port 0
static constexpr uint8_t KISS_DATA_FRAME = 0x00;

class Kiss {
public:
    // Encode an IpFrame into a KISS frame.
    // outBuf must be at least (2 * IP_MTU + 4) bytes (1011 bytes worst-case).
    // Returns number of bytes written to outBuf, or 0 if outBuf is too small.
    static size_t encode(const IpFrame& frame, uint8_t* outBuf, size_t outBufLen);

    // Feed one byte at a time into the decoder.
    // Returns true when a complete frame has been decoded into frame.
    bool decode(uint8_t byte, IpFrame& frame);

    // Reset decoder state (e.g. on sync loss)
    void reset();

private:
    enum class State : uint8_t { IDLE, IN_FRAME, ESCAPE };

    State    _state    = State::IDLE;
    uint8_t  _buf[IP_MTU + 1];
    uint16_t _len      = 0;
    bool     _overflow = false;
};
