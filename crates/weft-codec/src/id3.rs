//! A minimal ID3v2.4 tag: UTF-8 text frames only.

fn syncsafe(n: u32) -> [u8; 4] {
    [
        ((n >> 21) & 0x7f) as u8,
        ((n >> 14) & 0x7f) as u8,
        ((n >> 7) & 0x7f) as u8,
        (n & 0x7f) as u8,
    ]
}

/// The tag for the given (frame id, text) pairs; empty texts are left out. With no
/// frames at all there is no tag.
pub fn tag(frames: &[(&[u8; 4], &str)]) -> Vec<u8> {
    let mut body = Vec::new();
    for (id, value) in frames {
        if value.is_empty() {
            continue;
        }
        let size = value.len() as u32 + 1;
        body.extend_from_slice(*id);
        body.extend_from_slice(&syncsafe(size));
        body.extend_from_slice(&[0, 0]); // flags
        body.push(3); // UTF-8
        body.extend_from_slice(value.as_bytes());
    }
    if body.is_empty() {
        return body;
    }
    let mut out = Vec::with_capacity(body.len() + 10);
    out.extend_from_slice(b"ID3");
    out.extend_from_slice(&[4, 0, 0]);
    out.extend_from_slice(&syncsafe(body.len() as u32));
    out.extend_from_slice(&body);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn writes_a_header_and_utf8_frames() {
        let t = tag(&[(b"TIT2", "Song"), (b"TPE1", ""), (b"TALB", "Ä")]);
        assert_eq!(&t[0..5], b"ID3\x04\x00");
        assert_eq!(t[9] as usize, t.len() - 10);
        assert_eq!(&t[10..14], b"TIT2");
        assert_eq!(&t[20..25], b"\x03Song");
        assert_eq!(&t[25..29], b"TALB");
        assert!(tag(&[(b"TIT2", "")]).is_empty());
    }
}
