#include "Kiss.h"
#include <string.h>

size_t Kiss::encode(const IpFrame& frame, uint8_t* outBuf, size_t outBufLen) {
    size_t i = 0;

    auto write = [&](uint8_t b) {
        if (i < outBufLen) outBuf[i++] = b;
    };

    write(KISS_FEND);
    write(KISS_DATA_FRAME);

    for (uint16_t n = 0; n < frame.len; n++) {
        uint8_t b = frame.data[n];
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

bool Kiss::decode(uint8_t byte, IpFrame& frame) {
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
                if (_len == 0) {
                    // Empty delimiter while in-frame: fresh start, no frame emitted.
                    _overflow = false;
                    break;
                }
                if (_overflow) {
                    // Oversized frame: discard and start fresh candidate.
                    _len      = 0;
                    _overflow = false;
                    break;
                }
                if (_buf[0] != KISS_DATA_FRAME) {
                    // Non-data port: discard silently, including any overflow state.
                    _len      = 0;
                    _overflow = false;
                    break;
                }
                // Valid data frame: emit payload (strip port byte).
                uint16_t payloadLen = _len - 1;
                memcpy(frame.data, _buf + 1, payloadLen);
                frame.len = payloadLen;
                _len      = 0;
                // Trailing FEND closes this frame and opens the next candidate.
                _state    = State::IN_FRAME;
                return true;
            } else if (byte == KISS_FESC) {
                _state = State::ESCAPE;
            } else {
                if (_len < IP_MTU + 1) {
                    _buf[_len++] = byte;
                } else {
                    _overflow = true;
                }
            }
            break;

        case State::ESCAPE:
            if (byte == KISS_FEND) {
                // FEND while in escape: invalid sequence, discard partial frame.
                _len      = 0;
                _overflow = false;
                _state    = State::IN_FRAME;
                break;
            }
            _state = State::IN_FRAME;
            if (byte == KISS_TFEND) {
                byte = KISS_FEND;
            } else if (byte == KISS_TFESC) {
                byte = KISS_FESC;
            }
            if (_len < IP_MTU + 1) {
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
