use crc32fast::Hasher;

const SERIAL_INTEGRITY_MAGIC: u16 = 0x8AC1;
pub const SERIAL_INTEGRITY_HDR_LEN: usize = 8;

pub fn wrap_payload(payload: &[u8], out: &mut Vec<u8>) {
    let mut hasher = Hasher::new();
    hasher.update(payload);
    let crc = hasher.finalize();

    out.clear();
    out.extend_from_slice(&SERIAL_INTEGRITY_MAGIC.to_be_bytes());
    out.extend_from_slice(&(payload.len() as u16).to_be_bytes());
    out.extend_from_slice(&crc.to_be_bytes());
    out.extend_from_slice(payload);
}

pub fn unwrap_payload(frame: &[u8]) -> Result<&[u8], String> {
    if frame.len() < SERIAL_INTEGRITY_HDR_LEN {
        return Err("Frame too short for integrity header".into());
    }

    let magic = u16::from_be_bytes([frame[0], frame[1]]);
    if magic != SERIAL_INTEGRITY_MAGIC {
        return Err(format!("Invalid magic: {:#06x}", magic));
    }

    let length = u16::from_be_bytes([frame[2], frame[3]]) as usize;
    if length != frame.len() - SERIAL_INTEGRITY_HDR_LEN {
        return Err(format!("Length mismatch: expected {}, got {}", length, frame.len() - SERIAL_INTEGRITY_HDR_LEN));
    }

    let crc = u32::from_be_bytes([frame[4], frame[5], frame[6], frame[7]]);
    let payload = &frame[SERIAL_INTEGRITY_HDR_LEN..];

    let mut hasher = Hasher::new();
    hasher.update(payload);
    let computed_crc = hasher.finalize();

    if computed_crc != crc {
        return Err(format!("CRC mismatch: expected {:#010x}, got {:#010x}", crc, computed_crc));
    }

    Ok(payload)
}
