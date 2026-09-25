use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::io;
#[cfg(test)]
use std::io::{Read, Write};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt};

pub const PROTOCOL: &str = "snow-shot-mcp/1";
pub const MAX_FRAME_BYTES: usize = 64 * 1024 * 1024;

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct AppRequest {
    pub protocol: String,
    pub request_id: String,
    pub method: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub expected_revision: Option<u64>,
    pub idempotency_key: String,
    #[serde(default)]
    pub params: Value,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct AppError {
    pub code: String,
    pub message: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub details: Option<Value>,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct AppResponse {
    pub protocol: String,
    pub request_id: String,
    pub ok: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub session_id: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub revision: Option<u64>,
    #[serde(default)]
    pub result: Value,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<AppError>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub attachment_mime: Option<String>,
    #[serde(default)]
    pub attachment_length: usize,
}

#[derive(Debug, Serialize, Deserialize, Clone)]
pub struct Descriptor {
    pub protocol: String,
    pub socket: String,
    pub token: String,
    pub pid: u32,
    pub generation: String,
    pub max_frame_bytes: usize,
}

#[derive(Debug)]
pub struct Frame {
    pub json: Vec<u8>,
    pub attachment: Vec<u8>,
}

#[cfg(test)]
pub fn encode_frame<T: Serialize>(value: &T, attachment: &[u8]) -> io::Result<Vec<u8>> {
    let json = serde_json::to_vec(value).map_err(io::Error::other)?;
    let json_len = u32::try_from(json.len())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "JSON header is too large"))?;
    let payload_len = 4usize
        .checked_add(json.len())
        .and_then(|n| n.checked_add(attachment.len()))
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "frame length overflow"))?;
    if payload_len > MAX_FRAME_BYTES {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "frame exceeds the maximum size",
        ));
    }
    let payload_len_u32 = u32::try_from(payload_len).map_err(|_| {
        io::Error::new(
            io::ErrorKind::InvalidInput,
            "frame is too large for protocol",
        )
    })?;
    let mut frame = Vec::with_capacity(4 + payload_len);
    frame.extend_from_slice(&payload_len_u32.to_be_bytes());
    frame.extend_from_slice(&json_len.to_be_bytes());
    frame.extend_from_slice(&json);
    frame.extend_from_slice(attachment);
    Ok(frame)
}

#[cfg(test)]
pub fn write_frame<W: Write, T: Serialize>(
    writer: &mut W,
    value: &T,
    attachment: &[u8],
) -> io::Result<()> {
    let json = serde_json::to_vec(value).map_err(io::Error::other)?;
    let size = 4usize
        .checked_add(json.len())
        .and_then(|n| n.checked_add(attachment.len()))
        .filter(|n| *n <= MAX_FRAME_BYTES)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "frame too large"))?;
    writer.write_all(&(size as u32).to_be_bytes())?;
    writer.write_all(&(json.len() as u32).to_be_bytes())?;
    writer.write_all(&json)?;
    writer.write_all(attachment)?;
    writer.flush()
}

#[cfg(test)]
pub fn read_frame<R: Read>(reader: &mut R) -> io::Result<Option<Frame>> {
    let mut length = [0u8; 4];
    // EOF is clean only before the first byte, never halfway through the length prefix.
    loop {
        match reader.read(&mut length[..1]) {
            Ok(0) => return Ok(None),
            Ok(_) => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    reader.read_exact(&mut length[1..])?;
    let size = u32::from_be_bytes(length) as usize;
    if !(6..=MAX_FRAME_BYTES).contains(&size) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid frame length",
        ));
    }
    reader.read_exact(&mut length)?;
    let json_size = u32::from_be_bytes(length) as usize;
    if !(2..=1024 * 1024 + 4096).contains(&json_size) || json_size > size - 4 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid JSON length",
        ));
    }
    let mut json = vec![0; json_size];
    reader.read_exact(&mut json)?;
    // Reject malformed headers before allocating the potentially large attachment.
    let value: Value = serde_json::from_slice(&json)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid JSON envelope"))?;
    if !value.is_object() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "expected JSON object",
        ));
    }
    let attachment_size = size - 4 - json_size;
    if value
        .get("attachment_length")
        .and_then(Value::as_u64)
        .unwrap_or(0)
        != attachment_size as u64
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "attachment length mismatch",
        ));
    }
    let mut attachment = vec![0; attachment_size];
    reader.read_exact(&mut attachment)?;
    Ok(Some(Frame { json, attachment }))
}

pub async fn write_frame_async<T: Serialize>(
    writer: &mut (impl AsyncWrite + Unpin),
    value: &T,
    attachment: &[u8],
) -> io::Result<()> {
    let json = serde_json::to_vec(value).map_err(io::Error::other)?;
    let size = 4usize
        .checked_add(json.len())
        .and_then(|n| n.checked_add(attachment.len()))
        .filter(|n| *n <= MAX_FRAME_BYTES)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidInput, "frame too large"))?;
    writer.write_all(&(size as u32).to_be_bytes()).await?;
    writer.write_all(&(json.len() as u32).to_be_bytes()).await?;
    writer.write_all(&json).await?;
    writer.write_all(attachment).await?;
    writer.flush().await
}

pub async fn read_frame_async<R: AsyncRead + Unpin>(reader: &mut R) -> io::Result<Option<Frame>> {
    let mut length = [0u8; 4];
    // EOF is clean only before the first byte, never halfway through the length prefix.
    loop {
        match reader.read(&mut length[..1]).await {
            Ok(0) => return Ok(None),
            Ok(_) => break,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => return Err(e),
        }
    }
    reader.read_exact(&mut length[1..]).await?;
    let size = u32::from_be_bytes(length) as usize;
    if !(6..=MAX_FRAME_BYTES).contains(&size) {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid frame length",
        ));
    }
    reader.read_exact(&mut length).await?;
    let json_size = u32::from_be_bytes(length) as usize;
    if !(2..=1024 * 1024 + 4096).contains(&json_size) || json_size > size - 4 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "invalid JSON length",
        ));
    }
    let mut json = vec![0; json_size];
    reader.read_exact(&mut json).await?;
    // Reject malformed headers before allocating the potentially large attachment.
    let value: Value = serde_json::from_slice(&json)
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid JSON envelope"))?;
    if !value.is_object() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "expected JSON object",
        ));
    }
    let attachment_size = size - 4 - json_size;
    if value
        .get("attachment_length")
        .and_then(Value::as_u64)
        .unwrap_or(0)
        != attachment_size as u64
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "attachment length mismatch",
        ));
    }
    let mut attachment = vec![0; attachment_size];
    reader.read_exact(&mut attachment).await?;
    Ok(Some(Frame { json, attachment }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trips_partial_frame_reads() {
        let response = AppResponse {
            protocol: PROTOCOL.to_owned(),
            request_id: "r1".to_owned(),
            ok: true,
            session_id: Some("s1".to_owned()),
            revision: Some(4),
            result: serde_json::json!({"message": "日本語 ☕"}),
            error: None,
            attachment_mime: Some("image/png".to_owned()),
            attachment_length: 4,
        };
        let encoded = encode_frame(&response, b"png!").unwrap();
        let mut reader = ChunkedReader {
            bytes: encoded,
            offset: 0,
            chunk_size: 3,
        };
        let frame = read_frame(&mut reader).unwrap().unwrap();
        let decoded: AppResponse = serde_json::from_slice(&frame.json).unwrap();
        assert_eq!(decoded.request_id, "r1");
        assert_eq!(frame.attachment, b"png!");
    }

    #[test]
    fn rejects_oversized_and_truncated_frames() {
        let too_large = (MAX_FRAME_BYTES as u32 + 1).to_be_bytes();
        assert!(read_frame(&mut &too_large[..]).is_err());
        let truncated = [0, 0, 0, 4, 0, 0, 0];
        assert!(read_frame(&mut &truncated[..]).is_err());
    }

    #[test]
    fn eof_headers_json_and_attachment_lengths_are_strict() {
        assert!(read_frame(&mut &[][..]).unwrap().is_none());
        for prefix in [&[0][..], &[0, 0][..], &[0, 0, 0][..]] {
            assert!(read_frame(&mut &prefix[..]).is_err());
        }
        for bytes in [
            &[0, 0, 0, 6, 0, 0, 0, 2, b'[', b']'][..],
            &[0, 0, 0, 6, 0, 0, 0, 2, b'{', b'x'][..],
            &[6, 0, 0, 0, 0, 0, 0, 2, b'{', b'}'][..],
        ] {
            assert!(read_frame(&mut &bytes[..]).is_err());
        }
        let bad = encode_frame(&serde_json::json!({"attachment_length":4}), b"ab").unwrap();
        assert!(read_frame(&mut &bad[..]).is_err());
        let valid = encode_frame(&serde_json::json!({"attachment_length":4}), b"abcd").unwrap();
        assert!(read_frame(&mut &valid[..valid.len() - 1]).is_err());
    }
    #[test]
    fn partial_writes_preserve_header_and_binary() {
        struct Writer(Vec<u8>);
        impl Write for Writer {
            fn write(&mut self, data: &[u8]) -> io::Result<usize> {
                let n = data.len().min(3);
                self.0.extend_from_slice(&data[..n]);
                Ok(n)
            }
            fn flush(&mut self) -> io::Result<()> {
                Ok(())
            }
        }
        let mut writer = Writer(Vec::new());
        let json = serde_json::json!({"attachment_length":5,"text":"\u{4e16}\u{754c}"});
        write_frame(&mut writer, &json, b"abcde").unwrap();
        assert_eq!(writer.0, encode_frame(&json, b"abcde").unwrap());
    }

    struct ChunkedReader {
        bytes: Vec<u8>,
        offset: usize,
        chunk_size: usize,
    }

    impl Read for ChunkedReader {
        fn read(&mut self, output: &mut [u8]) -> io::Result<usize> {
            if self.offset == self.bytes.len() {
                return Ok(0);
            }
            let count = self
                .chunk_size
                .min(output.len())
                .min(self.bytes.len() - self.offset);
            output[..count].copy_from_slice(&self.bytes[self.offset..self.offset + count]);
            self.offset += count;
            Ok(count)
        }
    }
}
