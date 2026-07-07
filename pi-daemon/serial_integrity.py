import struct
import zlib

# [ Magic (2B) | PayloadLen (2B) | PayloadCRC32 (4B) | Payload... ]
# Magic: 0x8A 0xC1
SERIAL_INTEGRITY_MAGIC = 0x8AC1
SERIAL_INTEGRITY_HDR_LEN = 8
SERIAL_INTEGRITY_MAX_PAYLOAD_LEN = 1280
_HEADER_STRUCT = struct.Struct(">HHI")

def wrap_payload(payload: bytes) -> bytes:
    if len(payload) > SERIAL_INTEGRITY_MAX_PAYLOAD_LEN:
        raise ValueError(
            f"Payload too large: {len(payload)} > {SERIAL_INTEGRITY_MAX_PAYLOAD_LEN}"
        )
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    header = _HEADER_STRUCT.pack(SERIAL_INTEGRITY_MAGIC, len(payload), crc)
    return header + payload

def unwrap_payload(frame: bytes) -> bytes:
    if len(frame) < SERIAL_INTEGRITY_HDR_LEN:
        raise ValueError("Frame too short for integrity header")
    magic, length, crc = _HEADER_STRUCT.unpack(frame[:SERIAL_INTEGRITY_HDR_LEN])
    if magic != SERIAL_INTEGRITY_MAGIC:
        raise ValueError(f"Invalid magic: {hex(magic)}")
    if length != len(frame) - SERIAL_INTEGRITY_HDR_LEN:
        raise ValueError(f"Length mismatch: expected {length}, got {len(frame) - SERIAL_INTEGRITY_HDR_LEN}")
    if length > SERIAL_INTEGRITY_MAX_PAYLOAD_LEN:
        raise ValueError(
            f"Payload too large: {length} > {SERIAL_INTEGRITY_MAX_PAYLOAD_LEN}"
        )
    payload = frame[SERIAL_INTEGRITY_HDR_LEN:]
    computed_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if computed_crc != crc:
        raise ValueError(f"CRC mismatch: expected {hex(crc)}, got {hex(computed_crc)}")
    return payload
