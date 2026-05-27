pub const KISS_DATA_PORT: u8 = 0x00;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum State {
    Idle,
    InFrame,
    Escape,
}

pub struct KissDecoder {
    state: State,
    buf: Vec<u8>,
    max_len: usize,
    overflow: bool,
}

impl KissDecoder {
    pub fn new(max_len: usize) -> Self {
        Self {
            state: State::Idle,
            buf: Vec::with_capacity(max_len + 1),
            max_len,
            overflow: false,
        }
    }

    pub fn reset(&mut self) {
        self.state = State::Idle;
        self.buf.clear();
        self.overflow = false;
    }

    pub fn feed(&mut self, byte: u8, out: &mut Vec<u8>) -> bool {
        match self.state {
            State::Idle => {
                if byte == 0xC0 {
                    self.buf.clear();
                    self.overflow = false;
                    self.state = State::InFrame;
                }
            }
            State::InFrame => {
                if byte == 0xC0 {
                    if self.buf.is_empty() {
                        // Empty delimiter: fresh start, no frame emitted.
                        self.overflow = false;
                        return false;
                    }
                    if self.overflow {
                        // Oversized frame: discard and start fresh candidate.
                        self.buf.clear();
                        self.overflow = false;
                        return false;
                    }
                    let port = self.buf[0];
                    if port != KISS_DATA_PORT {
                        // Non-data port: discard silently, including overflow state.
                        self.buf.clear();
                        self.overflow = false;
                        return false;
                    }
                    // Valid data frame: emit payload (strip port byte).
                    out.clear();
                    out.extend_from_slice(&self.buf[1..]);
                    self.buf.clear();
                    // Trailing FEND closes this frame and opens the next candidate.
                    return true;
                } else if byte == 0xDB {
                    self.state = State::Escape;
                } else {
                    if self.buf.len() < self.max_len + 1 {
                        self.buf.push(byte);
                    } else {
                        self.overflow = true;
                    }
                }
            }
            State::Escape => {
                if byte == 0xC0 {
                    // FEND while in escape: invalid sequence, discard partial frame.
                    self.buf.clear();
                    self.overflow = false;
                    self.state = State::InFrame;
                    return false;
                }
                self.state = State::InFrame;
                let decoded_byte = if byte == 0xDC {
                    0xC0
                } else if byte == 0xDD {
                    0xDB
                } else {
                    byte
                };
                if self.buf.len() < self.max_len + 1 {
                    self.buf.push(decoded_byte);
                } else {
                    self.overflow = true;
                }
            }
        }
        false
    }
}

pub fn kiss_encode(payload: &[u8], out: &mut Vec<u8>) {
    out.clear();
    out.push(0xC0);
    out.push(0x00); // port 0, data frame
    for &b in payload {
        if b == 0xC0 {
            out.push(0xDB);
            out.push(0xDC);
        } else if b == 0xDB {
            out.push(0xDB);
            out.push(0xDD);
        } else {
            out.push(b);
        }
    }
    out.push(0xC0);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_roundtrip_all_bytes() {
        let payload: Vec<u8> = (0..=255).collect();
        let mut encoded = Vec::new();
        kiss_encode(&payload, &mut encoded);

        let mut decoder = KissDecoder::new(300);
        let mut decoded = Vec::new();
        let mut success = false;

        for &b in &encoded {
            if decoder.feed(b, &mut decoded) {
                success = true;
                break;
            }
        }

        assert!(success);
        assert_eq!(decoded, payload);
    }

    #[test]
    fn test_escape_fend_in_payload() {
        let payload = vec![0x11, 0xC0, 0x22];
        let mut encoded = Vec::new();
        kiss_encode(&payload, &mut encoded);
        assert_eq!(encoded, vec![0xC0, 0x00, 0x11, 0xDB, 0xDC, 0x22, 0xC0]);
    }

    #[test]
    fn test_escape_fesc_in_payload() {
        let payload = vec![0x11, 0xDB, 0x22];
        let mut encoded = Vec::new();
        kiss_encode(&payload, &mut encoded);
        assert_eq!(encoded, vec![0xC0, 0x00, 0x11, 0xDB, 0xDD, 0x22, 0xC0]);
    }

    #[test]
    fn test_back_to_back_double_fend() {
        let payload1 = vec![0x11, 0x22];
        let payload2 = vec![0x33, 0x44];
        let mut encoded1 = Vec::new();
        let mut encoded2 = Vec::new();
        kiss_encode(&payload1, &mut encoded1);
        kiss_encode(&payload2, &mut encoded2);

        // Standard stream: FEND port1 data1 FEND FEND port2 data2 FEND
        let mut stream = Vec::new();
        stream.extend_from_slice(&encoded1);
        stream.extend_from_slice(&encoded2);

        let mut decoder = KissDecoder::new(10);
        let mut decoded = Vec::new();
        let mut results = Vec::new();

        for &b in &stream {
            if decoder.feed(b, &mut decoded) {
                results.push(decoded.clone());
            }
        }

        assert_eq!(results.len(), 2);
        assert_eq!(results[0], payload1);
        assert_eq!(results[1], payload2);
    }

    #[test]
    fn test_fend_inside_escape_discards_and_resyncs() {
        // FEND 0x00 0xAA FESC FEND  <- invalid escape, partial frame discarded
        // 0x00 0xBB FEND            <- valid frame (FEND above opened it)
        let stream = vec![0xC0, 0x00, 0xAA, 0xDB, 0xC0, 0x00, 0xBB, 0xC0];

        let mut decoder = KissDecoder::new(100);
        let mut decoded = Vec::new();
        let mut results = Vec::new();

        for &b in &stream {
            if decoder.feed(b, &mut decoded) {
                results.push(decoded.clone());
            }
        }

        assert_eq!(results.len(), 1);
        assert_eq!(results[0], vec![0xBB]);
    }

    #[test]
    fn test_non_zero_port_then_valid_resyncs() {
        // FEND 0x10 0xDE 0xAD FEND  <- port 1, discarded
        // 0x00 0x42 FEND            <- port 0, valid (FEND above opened it)
        let stream = vec![0xC0, 0x10, 0xDE, 0xAD, 0xC0, 0x00, 0x42, 0xC0];

        let mut decoder = KissDecoder::new(100);
        let mut decoded = Vec::new();
        let mut results = Vec::new();

        for &b in &stream {
            if decoder.feed(b, &mut decoded) {
                results.push(decoded.clone());
            }
        }

        assert_eq!(results.len(), 1);
        assert_eq!(results[0], vec![0x42]);
    }

    #[test]
    fn test_oversized_then_valid_resyncs() {
        let max_len = 10usize;
        let mut decoder = KissDecoder::new(max_len);
        let mut decoded = Vec::new();

        // Open oversized frame
        decoder.feed(0xC0, &mut decoded); // FEND
        decoder.feed(0x00, &mut decoded); // port
        for _ in 0..max_len + 5 {
            decoder.feed(0xAA, &mut decoded);
        }
        // Closing FEND discards oversized frame
        let r = decoder.feed(0xC0, &mut decoded);
        assert!(!r);

        // Next valid frame (FEND above opened it)
        decoder.feed(0x00, &mut decoded); // port
        decoder.feed(0x77, &mut decoded); // payload
        let r = decoder.feed(0xC0, &mut decoded);

        assert!(r);
        assert_eq!(decoded, vec![0x77]);
    }

    #[test]
    fn test_369_byte_regression_reproducer() {
        let payload: Vec<u8> = (0..369u16).map(|i| (i & 0xFF) as u8).collect();
        let mut encoded = Vec::new();
        kiss_encode(&payload, &mut encoded);

        let mut decoder = KissDecoder::new(512);
        let mut decoded = Vec::new();
        let mut complete = false;

        // Deliver in 64-byte chunks
        for chunk in encoded.chunks(64) {
            for &b in chunk {
                if decoder.feed(b, &mut decoded) {
                    complete = true;
                    break;
                }
            }
            if complete { break; }
        }

        assert!(complete, "369-byte frame not completed");
        assert_eq!(decoded.len(), 369, "369-byte frame truncated to {}", decoded.len());
        assert_eq!(decoded, payload);
    }
}
