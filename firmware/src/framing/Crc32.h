#pragma once

#include <stdint.h>
#include <stddef.h>

namespace framing {

// Computes the standard IEEE 802.3 CRC32 (zlib/Ethernet) over a buffer.
// Polynomial: 0xEDB88320 (reversed representation of 0x04C11DB7)
inline uint32_t computeCrc32(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

} // namespace framing
