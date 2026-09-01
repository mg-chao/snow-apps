use std::io::{self, Read, Write};

pub const MAGIC: [u8; 4] = *b"SOCR";
pub const VERSION: u16 = 1;
pub const MAX_FRAME: usize = 1024 * 1024;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u16)]
pub enum Kind {
    Hello = 1,
    Ready = 2,
    Submit = 3,
    Cancel = 4,
    Complete = 5,
    Shutdown = 6,
    ShutdownAck = 7,
}

impl TryFrom<u16> for Kind {
    type Error = io::Error;
    fn try_from(value: u16) -> io::Result<Self> {
        match value {
            1 => Ok(Self::Hello),
            2 => Ok(Self::Ready),
            3 => Ok(Self::Submit),
            4 => Ok(Self::Cancel),
            5 => Ok(Self::Complete),
            6 => Ok(Self::Shutdown),
            7 => Ok(Self::ShutdownAck),
            _ => Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "unknown OCR protocol message",
            )),
        }
    }
}

#[derive(Debug)]
pub struct Frame {
    pub kind: Kind,
    pub request_id: u64,
    pub payload: Vec<u8>,
}

pub fn read_frame<R: Read>(reader: &mut R) -> io::Result<Frame> {
    let mut header = [0_u8; 20];
    reader.read_exact(&mut header)?;
    if header[..4] != MAGIC {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid OCR protocol magic",
        ));
    }
    let version = u16::from_le_bytes([header[4], header[5]]);
    if version != VERSION {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "unsupported OCR protocol version",
        ));
    }
    let kind = Kind::try_from(u16::from_le_bytes([header[6], header[7]]))?;
    let request_id = u64::from_le_bytes(header[8..16].try_into().unwrap());
    let length = u32::from_le_bytes(header[16..20].try_into().unwrap()) as usize;
    if length > MAX_FRAME {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "OCR protocol frame is too large",
        ));
    }
    let mut payload = vec![0_u8; length];
    reader.read_exact(&mut payload)?;
    Ok(Frame {
        kind,
        request_id,
        payload,
    })
}

pub fn write_frame<W: Write>(
    writer: &mut W,
    kind: Kind,
    request_id: u64,
    payload: &[u8],
) -> io::Result<()> {
    if payload.len() > MAX_FRAME {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "OCR protocol payload is too large",
        ));
    }
    let mut header = [0_u8; 20];
    header[..4].copy_from_slice(&MAGIC);
    header[4..6].copy_from_slice(&VERSION.to_le_bytes());
    header[6..8].copy_from_slice(&(kind as u16).to_le_bytes());
    header[8..16].copy_from_slice(&request_id.to_le_bytes());
    header[16..20].copy_from_slice(&(payload.len() as u32).to_le_bytes());
    writer.write_all(&header)?;
    writer.write_all(payload)?;
    writer.flush()
}

pub struct Decoder<'a> {
    data: &'a [u8],
    offset: usize,
}

impl<'a> Decoder<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data, offset: 0 }
    }
    fn take(&mut self, count: usize) -> io::Result<&'a [u8]> {
        let end = self
            .offset
            .checked_add(count)
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "payload overflow"))?;
        if end > self.data.len() {
            return Err(io::Error::new(
                io::ErrorKind::UnexpectedEof,
                "truncated OCR payload",
            ));
        }
        let value = &self.data[self.offset..end];
        self.offset = end;
        Ok(value)
    }
    pub fn u8(&mut self) -> io::Result<u8> {
        Ok(self.take(1)?[0])
    }
    pub fn u32(&mut self) -> io::Result<u32> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    pub fn u64(&mut self) -> io::Result<u64> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    pub fn string(&mut self) -> io::Result<String> {
        let length = self.u32()? as usize;
        String::from_utf8(self.take(length)?.to_vec()).map_err(|_| {
            io::Error::new(
                io::ErrorKind::InvalidData,
                "OCR payload string is not UTF-8",
            )
        })
    }
    pub fn done(&self) -> bool {
        self.offset == self.data.len()
    }
}

pub fn put_u8(out: &mut Vec<u8>, value: u8) {
    out.push(value);
}
pub fn put_u32(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}
pub fn put_f32(out: &mut Vec<u8>, value: f32) {
    out.extend_from_slice(&value.to_le_bytes());
}
pub fn put_string(out: &mut Vec<u8>, value: &str) {
    put_u32(out, value.len() as u32);
    out.extend_from_slice(value.as_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    #[test]
    fn frame_round_trip() {
        let mut encoded = Vec::new();
        write_frame(&mut encoded, Kind::Submit, 42, b"payload").unwrap();
        let decoded = read_frame(&mut Cursor::new(encoded)).unwrap();
        assert_eq!(decoded.kind, Kind::Submit);
        assert_eq!(decoded.request_id, 42);
        assert_eq!(decoded.payload, b"payload");
    }

    #[test]
    fn invalid_magic_and_version_are_rejected() {
        let mut header = [0_u8; 20];
        header[4..6].copy_from_slice(&VERSION.to_le_bytes());
        header[6..8].copy_from_slice(&(Kind::Ready as u16).to_le_bytes());
        assert!(read_frame(&mut Cursor::new(header)).is_err());

        header[..4].copy_from_slice(&MAGIC);
        header[4..6].copy_from_slice(&(VERSION + 1).to_le_bytes());
        assert!(read_frame(&mut Cursor::new(header)).is_err());
    }

    #[test]
    fn oversized_and_truncated_frames_are_rejected() {
        let oversized = vec![0_u8; MAX_FRAME + 1];
        assert!(write_frame(&mut Vec::new(), Kind::Submit, 0, &oversized).is_err());

        let mut truncated = Vec::new();
        truncated.extend_from_slice(&MAGIC);
        truncated.extend_from_slice(&VERSION.to_le_bytes());
        truncated.extend_from_slice(&(Kind::Ready as u16).to_le_bytes());
        truncated.extend_from_slice(&0_u64.to_le_bytes());
        truncated.extend_from_slice(&4_u32.to_le_bytes());
        truncated.extend_from_slice(&[1, 2]);
        assert!(read_frame(&mut Cursor::new(truncated)).is_err());
    }

    #[test]
    fn decoder_rejects_trailing_and_invalid_utf8() {
        let mut payload = Vec::new();
        put_u32(&mut payload, 1);
        payload.push(0xff);
        let mut decoder = Decoder::new(&payload);
        assert!(decoder.string().is_err());
        let mut decoder = Decoder::new(&[1_u8]);
        assert!(!decoder.done());
        assert!(decoder.u32().is_err());
    }
}
