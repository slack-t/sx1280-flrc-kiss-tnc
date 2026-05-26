pub const KISS_DATA_PORT: u8 = 0x00;
pub const KISS_STATS_PORT: u8 = 0x10;

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
    log_buf: Vec<u8>,  // out-of-frame bytes (ESP32 debug output)
}

impl KissDecoder {
    pub fn new(max_len: usize) -> Self {
        Self {
            state: State::Idle,
            buf: Vec::with_capacity(max_len + 1),
            max_len,
            overflow: false,
            log_buf: Vec::new(),
        }
    }

    fn flush_log(&mut self) {
        if !self.log_buf.is_empty() {
            if let Ok(text) = std::str::from_utf8(&self.log_buf) {
                for line in text.lines() {
                    let line = line.trim();
                    if !line.is_empty() {
                        eprintln!("[ESP32] {}", line);
                    }
                }
            }
            self.log_buf.clear();
        }
    }

    pub fn reset(&mut self) {
        self.state = State::Idle;
        self.buf.clear();
        self.overflow = false;
    }

    pub fn feed(&mut self, byte: u8, out: &mut Vec<u8>) -> Option<u8> {
        match self.state {
            State::Idle => {
                if byte == 0xC0 {
                    self.flush_log();
                    self.buf.clear();
                    self.overflow = false;
                    self.state = State::InFrame;
                } else {
                    self.log_buf.push(byte);
                    if byte == b'\n' || self.log_buf.len() >= 256 {
                        self.flush_log();
                    }
                }
            }
            State::InFrame => {
                if byte == 0xC0 {
                    if self.overflow || self.buf.len() <= 1 {
                        self.buf.clear();
                        self.overflow = false;
                        return None;
                    }
                    let port = self.buf[0];
                    if port != KISS_DATA_PORT && port != KISS_STATS_PORT {
                        // Non-zero port: bytes accumulated between KISS frames.
                        // The ESP32 debug output lands here as plain ASCII text.
                        if let Ok(text) = std::str::from_utf8(&self.buf) {
                            for line in text.lines() {
                                let line = line.trim();
                                if !line.is_empty() {
                                    eprintln!("[ESP32] {}", line);
                                }
                            }
                        }
                        self.buf.clear();
                        return None;
                    }
                    
                    // Copy payload (excluding port byte) to output
                    out.clear();
                    out.extend_from_slice(&self.buf[1..]);
                    self.buf.clear();
                    
                    // Keep InFrame state for back-to-back single-FEND streaming
                    return Some(port);
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
        None
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
            if decoder.feed(b, &mut decoded).is_some() {
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
    fn test_back_to_back_single_fend() {
        let payload1 = vec![0x11, 0x22];
        let payload2 = vec![0x33, 0x44];
        let mut encoded1 = Vec::new();
        let mut encoded2 = Vec::new();
        kiss_encode(&payload1, &mut encoded1);
        kiss_encode(&payload2, &mut encoded2);

        // Combined as 0xC0 0x00 0x11 0x22 0xC0 0x00 0x33 0x44 0xC0
        let mut stream = Vec::new();
        stream.extend_from_slice(&encoded1);
        stream.extend_from_slice(&encoded2[1..]); // omit leading FEND of 2nd frame

        let mut decoder = KissDecoder::new(10);
        let mut decoded = Vec::new();
        let mut results = Vec::new();

        for &b in &stream {
            if decoder.feed(b, &mut decoded).is_some() {
                results.push(decoded.clone());
            }
        }

        assert_eq!(results.len(), 2);
        assert_eq!(results[0], payload1);
        assert_eq!(results[1], payload2);
    }
}
