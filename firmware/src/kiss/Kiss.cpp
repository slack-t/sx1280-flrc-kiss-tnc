#include "Kiss.h"
#include <string.h>

size_t Kiss::encode(const Packet& pkt, uint8_t* outBuf, size_t outBufLen) {
    size_t i = 0;

    auto write = [&](uint8_t b) {
        if (i < outBufLen) outBuf[i++] = b;
    };

    write(KISS_FEND);
    write(KISS_DATA_FRAME);   // port 0, data frame

    for (uint8_t n = 0; n < pkt.len; n++) {
        uint8_t b = pkt.data[n];
        if (b == KISS_FEND) {
            write(KISS_FESC);
            write(KISS_TFEND);
        } else if (b == KISS_FESC) {
            write(KISS_FESC);
            write(KISS_TFESC);
        } else {
            write(b);
        }
    }

    write(KISS_FEND);
    return i;
}

bool Kiss::decode(uint8_t byte, Packet& pkt) {
    switch (_state) {
        case State::IDLE:
            if (byte == KISS_FEND) {
                _len      = 0;
                _overflow = false;
                _state    = State::IN_FRAME;
            }
            break;

        case State::IN_FRAME:
            if (byte == KISS_FEND) {
                if (_overflow || _len == 0) {
                    // Empty or overflowed frame — discard, stay ready for next
                    _len      = 0;
                    _overflow = false;
                    break;
                }
                // First byte after FEND is port/command; only accept port 0 data (0x00)
                if (_buf[0] != KISS_DATA_FRAME) {
                    _len   = 0;
                    _state = State::IN_FRAME;
                    break;
                }
                // Copy payload (skip the port byte)
                uint8_t payloadLen = _len - 1;
                memcpy(pkt.data, _buf + 1, payloadLen);
                pkt.len = payloadLen;
                _len    = 0;
                // Transition to IN_FRAME, not IDLE: the trailing FEND of this
                // frame acts as the opening FEND of the next in single-FEND streams.
                _state  = State::IN_FRAME;
                return true;
            } else if (byte == KISS_FESC) {
                _state = State::ESCAPE;
            } else {
                if (_len < PACKET_MAX_LEN) {
                    _buf[_len++] = byte;
                } else {
                    _overflow = true;
                }
            }
            break;

        case State::ESCAPE:
            _state = State::IN_FRAME;
            if (byte == KISS_TFEND) {
                byte = KISS_FEND;
            } else if (byte == KISS_TFESC) {
                byte = KISS_FESC;
            }
            // Invalid escape sequences: pass the byte through unchanged
            if (_len < PACKET_MAX_LEN) {
                _buf[_len++] = byte;
            } else {
                _overflow = true;
            }
            break;
    }
    return false;
}

void Kiss::reset() {
    _state    = State::IDLE;
    _len      = 0;
    _overflow = false;
}
