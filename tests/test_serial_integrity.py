#!/usr/bin/env python3
import pathlib
import sys
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_DAEMON_DIR = REPO_ROOT / "pi-daemon"
sys.path.insert(0, str(PI_DAEMON_DIR))

import serial_integrity


class SerialIntegrityTests(unittest.TestCase):
    def test_wrap_unwrap_accepts_max_payload(self):
        payload = bytes((i & 0xFF) for i in range(serial_integrity.SERIAL_INTEGRITY_MAX_PAYLOAD_LEN))
        wrapped = serial_integrity.wrap_payload(payload)
        self.assertEqual(
            len(wrapped),
            serial_integrity.SERIAL_INTEGRITY_HDR_LEN + serial_integrity.SERIAL_INTEGRITY_MAX_PAYLOAD_LEN,
        )
        self.assertEqual(serial_integrity.unwrap_payload(wrapped), payload)

    def test_wrap_rejects_payload_above_max(self):
        payload = b"x" * (serial_integrity.SERIAL_INTEGRITY_MAX_PAYLOAD_LEN + 1)
        with self.assertRaises(ValueError):
            serial_integrity.wrap_payload(payload)

    def test_unwrap_rejects_payload_above_max(self):
        payload = b"x" * (serial_integrity.SERIAL_INTEGRITY_MAX_PAYLOAD_LEN + 1)
        crc = serial_integrity.zlib.crc32(payload) & 0xFFFFFFFF
        header = serial_integrity._HEADER_STRUCT.pack(
            serial_integrity.SERIAL_INTEGRITY_MAGIC,
            len(payload),
            crc,
        )
        with self.assertRaises(ValueError):
            serial_integrity.unwrap_payload(header + payload)


if __name__ == "__main__":
    unittest.main()
