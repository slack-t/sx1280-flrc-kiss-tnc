#include "Kiss.h"
#include <string.h>

size_t Kiss::encodeFrame(uint8_t command, const uint8_t* payload, uint16_t payloadLen,
                         uint8_t* outBuf, size_t outBufLen) {
    size_t requiredLen = 3; // FEND + port + trailing FEND
    for (uint16_t n = 0; n < payloadLen; n++) {
        uint8_t b = payload[n];
        requiredLen += (b == KISS_FEND || b == KISS_FESC) ? 2u : 1u;
    }
    if (requiredLen > outBufLen) {
        return 0;
    }

    size_t i = 0;
    auto write = [&](uint8_t b) { outBuf[i++] = b; };

    write(KISS_FEND);
    write(command);

    for (uint16_t n = 0; n < payloadLen; n++) {
        uint8_t b = payload[n];
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

size_t Kiss::encode(const PayloadFrame& frame, uint8_t* outBuf, size_t outBufLen) {
    return encodeFrame(KISS_DATA_FRAME, frame.data, frame.len, outBuf, outBufLen);
}

bool Kiss::decode(uint8_t byte, PayloadFrame& frame) {
    KissFrame kissFrame;
    if (decodeFrameEx(byte, kissFrame) != KissDecodeResult::FRAME) {
        return false;
    }
    if (kissFrame.command != KISS_DATA_FRAME) {
        return false;
    }
    if (kissFrame.len > TNC_PAYLOAD_MAX_LEN) {
        return false;
    }
    memcpy(frame.data, kissFrame.data, kissFrame.len);
    frame.len = kissFrame.len;
    return true;
}

bool Kiss::decodeFrame(uint8_t byte, KissFrame& frame) {
    return decodeFrameEx(byte, frame) == KissDecodeResult::FRAME;
}

KissDecodeResult Kiss::decodeFrameEx(uint8_t byte, KissFrame& frame) {
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
                    // Empty frame: ignore and require a fresh FEND to reopen.
                    _overflow = false;
                    _state    = State::IDLE;
                    break;
                }
                if (_overflow) {
                    // Oversized frame: discard and require a fresh FEND.
                    _len      = 0;
                    _overflow = false;
                    _state    = State::IDLE;
                    return KissDecodeResult::OVERSIZE_DROP;
                }
                // Valid KISS frame: emit command and payload (strip command byte).
                uint16_t payloadLen = _len - 1;
                frame.command = _buf[0];
                if (payloadLen > 0) {
                    memcpy(frame.data, _buf + 1, payloadLen);
                }
                frame.len = payloadLen;
                _len      = 0;
                _overflow = false;
                _state    = State::IDLE;
                return KissDecodeResult::FRAME;
            } else if (byte == KISS_FESC) {
                _state = State::ESCAPE;
            } else {
                if (_len < KISS_FRAME_PAYLOAD_MAX_LEN + 1) {
                    _buf[_len++] = byte;
                } else {
                    _overflow = true;
                }
            }
            break;

        case State::ESCAPE:
            if (byte == KISS_TFEND || byte == KISS_TFESC) {
                _state = State::IN_FRAME;
                byte = (byte == KISS_TFEND) ? KISS_FEND : KISS_FESC;
                if (_len < KISS_FRAME_PAYLOAD_MAX_LEN + 1) {
                    _buf[_len++] = byte;
                } else {
                    _overflow = true;
                }
            } else {
                // Invalid escape: discard partial frame and require a fresh FEND.
                _len      = 0;
                _overflow = false;
                _state    = State::IDLE;
                return KissDecodeResult::INVALID_ESCAPE_DROP;
            }
            break;
    }
    return KissDecodeResult::NONE;
}

void Kiss::reset() {
    _state    = State::IDLE;
    _len      = 0;
    _overflow = false;
}
