#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../config.h"

// KISS framing constants (RFC 1055 / KISS spec)
static constexpr uint8_t KISS_FEND  = 0xC0;
static constexpr uint8_t KISS_FESC  = 0xDB;
static constexpr uint8_t KISS_TFEND = 0xDC;
static constexpr uint8_t KISS_TFESC = 0xDD;

// KISS port/command byte for data frames on port 0
static constexpr uint8_t KISS_DATA_FRAME = 0x00;

struct Packet {
    uint8_t  data[PACKET_MAX_LEN];
    uint8_t  len  = 0;
    int8_t   rssi = 0;
    float    snr  = 0.0f;
};

class Kiss {
public:
    // Encode a Packet into a KISS frame.
    // outBuf must be at least (2 * PACKET_MAX_LEN + 4) bytes.
    // Returns number of bytes written to outBuf.
    static size_t encode(const Packet& pkt, uint8_t* outBuf, size_t outBufLen);

    // Feed one byte at a time into the decoder.
    // Returns true when a complete frame has been decoded into pkt.
    // Resets internal state after a successful decode or after receiving FEND mid-frame.
    bool decode(uint8_t byte, Packet& pkt);

    // Reset decoder state (e.g. on sync loss)
    void reset();

private:
    enum class State : uint8_t { IDLE, IN_FRAME, ESCAPE };

    State   _state  = State::IDLE;
    uint8_t _buf[PACKET_MAX_LEN];
    uint8_t _len    = 0;
    bool    _overflow = false;
};
