#!/usr/bin/env python3
import pathlib
import sys
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
PI_DAEMON_DIR = REPO_ROOT / "pi-daemon"
sys.path.insert(0, str(PI_DAEMON_DIR))

from kiss_tun import (
    FIRMWARE_PAYLOAD_CAP,
    FEND,
    FESC,
    TFEND,
    TFESC,
    KISS_DATA_PORT,
    KissDecoder,
    kiss_encode,
)


def hx(hex_bytes: str) -> bytes:
    return bytes.fromhex(hex_bytes.replace(" ", ""))


class KissConformanceTests(unittest.TestCase):
    def decode_stream(self, stream: bytes, mtu: int, chunk_sizes=None):
        decoder = KissDecoder(mtu)
        outputs = []

        if not chunk_sizes:
            chunk_sizes = [len(stream)]

        offset = 0
        chunk_idx = 0
        while offset < len(stream):
            chunk = chunk_sizes[chunk_idx % len(chunk_sizes)]
            chunk = min(chunk, len(stream) - offset)
            for port, payload in decoder.feed(stream[offset:offset + chunk]):
                outputs.append((port, payload))
            offset += chunk
            chunk_idx += 1
        return outputs

    def test_encode_escapes_special_bytes(self):
        payload = bytes([0x11, FEND, 0x22, FESC, 0x33])
        encoded = kiss_encode(payload)
        self.assertEqual(
            encoded,
            bytes([FEND, 0x00, 0x11, FESC, TFEND, 0x22, FESC, TFESC, 0x33, FEND]),
        )

    def test_roundtrip_all_byte_values(self):
        payload = bytes(range(256))
        encoded = kiss_encode(payload)
        outputs = self.decode_stream(encoded, mtu=300, chunk_sizes=[1, 7, 13, 31])
        self.assertEqual(outputs, [(KISS_DATA_PORT, payload)])

    def test_back_to_back_frames_require_explicit_openers(self):
        stream = hx("c0 00 11 22 c0 c0 00 33 44 c0")
        outputs = self.decode_stream(stream, mtu=10, chunk_sizes=[2, 3, 1, 4])
        self.assertEqual(
            outputs,
            [
                (KISS_DATA_PORT, hx("11 22")),
                (KISS_DATA_PORT, hx("33 44")),
            ],
        )

    def test_noise_prefix_is_ignored(self):
        payload = bytes((i & 0xFF) for i in range(369))
        noise_prefix = hx("23 24 25 26 27 28 29 2a 2b 2c 2d 2e 2f 30 31 32")
        encoded = kiss_encode(payload)
        outputs = self.decode_stream(
            noise_prefix + encoded,
            mtu=FIRMWARE_PAYLOAD_CAP,
            chunk_sizes=[1, 7, 19, 3, 64, 11, 5, 128, 17, 256],
        )
        self.assertEqual(outputs, [(KISS_DATA_PORT, payload)])

    def test_invalid_escape_discards_partial_frame_and_requires_fresh_fend(self):
        stream = hx("c0 00 aa db c0 c0 00 bb c0")
        outputs = self.decode_stream(stream, mtu=100, chunk_sizes=[1])
        self.assertEqual(outputs, [(KISS_DATA_PORT, hx("bb"))])

    def test_non_zero_port_is_discarded(self):
        stream = hx("c0 10 de ad c0 c0 00 42 c0")
        outputs = self.decode_stream(stream, mtu=100, chunk_sizes=[4, 5])
        self.assertEqual(outputs, [(KISS_DATA_PORT, hx("42"))])

    def test_oversized_frame_is_discarded_and_resyncs_cleanly(self):
        oversized = bytes([FEND, 0x00]) + (b"\xAA" * 12) + bytes([FEND])
        valid = bytes([FEND, 0x00, 0x77, FEND])
        outputs = self.decode_stream(oversized + valid, mtu=10, chunk_sizes=[5, 9, 4])
        self.assertEqual(outputs, [(KISS_DATA_PORT, hx("77"))])

    def test_runtime_mtu_is_enforced_by_decoder(self):
        too_big_payload = bytes(range(247))
        valid_payload = hx("12 34")
        stream = kiss_encode(too_big_payload) + kiss_encode(valid_payload)
        outputs = self.decode_stream(stream, mtu=246, chunk_sizes=[64, 64, 64, 64, 64])
        self.assertEqual(outputs, [(KISS_DATA_PORT, valid_payload)])

    def test_empty_frames_are_ignored(self):
        outputs = self.decode_stream(bytes([FEND, FEND, FEND]), mtu=10, chunk_sizes=[1, 1, 1])
        self.assertEqual(outputs, [])


if __name__ == "__main__":
    unittest.main()
