#pragma once

#include <stdint.h>
#include <stdbool.h>

// 8-byte host-serial integrity envelope for GENERIC_FRAGMENTED mode
// [ Magic (2B) | PayloadLen (2B) | PayloadCRC32 (4B) | Payload... ]
// Magic: 0x8A 0xC1
// All fields big-endian. CRC32 covers ONLY the payload.

static constexpr uint16_t SERIAL_INTEGRITY_MAGIC = 0x8AC1;
static constexpr uint8_t SERIAL_INTEGRITY_HDR_LEN = 8;

struct SerialIntegrityHeader {
    uint16_t magic;
    uint16_t payload_len;
    uint32_t payload_crc32;
};

inline bool parseSerialIntegrityHeader(const uint8_t* data, size_t len, SerialIntegrityHeader& header) {
    if (len < SERIAL_INTEGRITY_HDR_LEN) {
        return false;
    }
    header.magic = static_cast<uint16_t>((data[0] << 8) | data[1]);
    header.payload_len = static_cast<uint16_t>((data[2] << 8) | data[3]);
    header.payload_crc32 = (static_cast<uint32_t>(data[4]) << 24) |
                           (static_cast<uint32_t>(data[5]) << 16) |
                           (static_cast<uint32_t>(data[6]) << 8) |
                            static_cast<uint32_t>(data[7]);
    return true;
}

inline void buildSerialIntegrityHeader(uint8_t* out, uint16_t payload_len, uint32_t payload_crc32) {
    out[0] = static_cast<uint8_t>(SERIAL_INTEGRITY_MAGIC >> 8);
    out[1] = static_cast<uint8_t>(SERIAL_INTEGRITY_MAGIC & 0xFF);
    out[2] = static_cast<uint8_t>(payload_len >> 8);
    out[3] = static_cast<uint8_t>(payload_len & 0xFF);
    out[4] = static_cast<uint8_t>(payload_crc32 >> 24);
    out[5] = static_cast<uint8_t>((payload_crc32 >> 16) & 0xFF);
    out[6] = static_cast<uint8_t>((payload_crc32 >> 8) & 0xFF);
    out[7] = static_cast<uint8_t>(payload_crc32 & 0xFF);
}
