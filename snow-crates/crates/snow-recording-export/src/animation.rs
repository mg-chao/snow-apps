//! Retain animation endpoints and the timing/loop metadata of one-frame clips.
use crate::ExportFormat;
use crate::error::{RecordingExportError, Result};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::Path;

fn png_chunk(output: &mut impl Write, kind: &[u8; 4], data: &[u8]) -> std::io::Result<()> {
    output.write_all(&(data.len() as u32).to_be_bytes())?;
    output.write_all(kind)?;
    output.write_all(data)?;
    let mut crc = !0u32;
    for byte in kind.iter().chain(data) {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb88320 & 0u32.wrapping_sub(crc & 1));
        }
    }
    output.write_all(&(!crc).to_be_bytes())
}

pub fn preserve_timing(
    path: &Path,
    format: ExportFormat,
    size: (u32, u32),
    duration_us: u64,
    loops: bool,
) -> Result<()> {
    if !matches!(format, ExportFormat::Apng | ExportFormat::Webp) {
        return Ok(());
    }
    let mut input = File::open(path)?;
    let mut temporary = tempfile::NamedTempFile::new_in(path.parent().unwrap_or(Path::new(".")))?;
    match format {
        ExportFormat::Apng => {
            let mut header = [0; 33]; // signature + IHDR
            input.read_exact(&mut header)?;
            if &header[..8] != b"\x89PNG\r\n\x1a\n" || &header[12..16] != b"IHDR" {
                return Err(RecordingExportError::Encode(
                    "invalid APNG staging header".into(),
                ));
            }
            loop {
                let mut chunk = [0; 8];
                input.read_exact(&mut chunk)?;
                if &chunk[4..] == b"acTL" {
                    return Ok(());
                }
                if &chunk[4..] == b"IDAT" {
                    break;
                }
                input.seek(SeekFrom::Current(
                    i64::from(u32::from_be_bytes(chunk[..4].try_into().unwrap())) + 4,
                ))?;
            }
            temporary.write_all(&header)?;
            let mut control = [0; 8];
            control[..4].copy_from_slice(&1u32.to_be_bytes());
            control[4..].copy_from_slice(&u32::from(!loops).to_be_bytes());
            png_chunk(&mut temporary, b"acTL", &control)?;
            let mut frame = [0; 26];
            frame[4..8].copy_from_slice(&size.0.to_be_bytes());
            frame[8..12].copy_from_slice(&size.1.to_be_bytes());
            let mut numerator = 0;
            let mut denominator = 0;
            unsafe {
                ffmpeg_next::ffi::av_reduce(
                    &mut numerator,
                    &mut denominator,
                    duration_us.min(i64::MAX as u64) as i64,
                    1_000_000,
                    65535,
                );
            }
            frame[20..22].copy_from_slice(&(numerator as u16).to_be_bytes());
            frame[22..24].copy_from_slice(&(denominator as u16).to_be_bytes());
            png_chunk(&mut temporary, b"fcTL", &frame)?;
            input.seek(SeekFrom::Start(33))?;
            std::io::copy(&mut input, &mut temporary)?;
        }
        ExportFormat::Webp => {
            let mut header = [0; 12];
            input.read_exact(&mut header)?;
            if &header[..4] != b"RIFF" || &header[8..] != b"WEBP" {
                return Err(RecordingExportError::Encode(
                    "invalid WebP staging header".into(),
                ));
            }
            let mut animated = false;
            let mut last_frame = None;
            let mut previous_duration_ms = 0u64;
            let mut last_duration_ms = 0u32;
            let mut payload = Vec::new();
            let mut payload_len = 0u32;
            loop {
                let mut chunk = [0; 8];
                if input.read(&mut chunk)? == 0 {
                    break;
                }
                let length = u32::from_le_bytes(chunk[4..].try_into().unwrap());
                if &chunk[..4] == b"ANIM" {
                    animated = true;
                }
                if &chunk[..4] == b"ANMF" {
                    let position = input.stream_position()?;
                    let mut frame = [0; 16];
                    input.read_exact(&mut frame)?;
                    previous_duration_ms += u64::from(last_duration_ms);
                    last_duration_ms = u32::from_le_bytes([frame[12], frame[13], frame[14], 0]);
                    last_frame = Some(position + 12);
                    input.seek(SeekFrom::Start(position))?;
                }
                let padded = length
                    .checked_add(length & 1)
                    .ok_or_else(|| RecordingExportError::Encode("WebP chunk overflow".into()))?;
                if matches!(&chunk[..4], b"VP8 " | b"VP8L" | b"ALPH") {
                    payload.push((input.stream_position()? - 8, padded + 8));
                    payload_len = payload_len
                        .checked_add(padded + 8)
                        .ok_or_else(|| RecordingExportError::Encode("WebP size overflow".into()))?;
                }
                input.seek(SeekFrom::Current(i64::from(padded)))?;
            }
            if animated {
                if let Some(offset) = last_frame {
                    drop(input);
                    let mut file = std::fs::OpenOptions::new().write(true).open(path)?;
                    file.seek(SeekFrom::Start(offset))?;
                    let delay = duration_us
                        .div_ceil(1000)
                        .saturating_sub(previous_duration_ms)
                        .clamp(1, 0xffffff) as u32;
                    file.write_all(&delay.to_le_bytes()[..3])?;
                }
                return Ok(());
            }
            if payload.is_empty() {
                return Err(RecordingExportError::Encode("WebP image is missing".into()));
            }
            let mut extended = [0; 10];
            extended[0] = 0x12; // animation + alpha
            extended[4..7].copy_from_slice(&(size.0 - 1).to_le_bytes()[..3]);
            extended[7..10].copy_from_slice(&(size.1 - 1).to_le_bytes()[..3]);
            temporary.write_all(b"RIFF")?;
            temporary.write_all(&(4 + 18 + 14 + 24 + payload_len).to_le_bytes())?;
            temporary.write_all(b"WEBPVP8X\x0a\0\0\0")?;
            temporary.write_all(&extended)?;
            temporary.write_all(b"ANIM\x06\0\0\0\0\0\0\0")?;
            temporary.write_all(&u16::from(!loops).to_le_bytes())?;
            temporary.write_all(b"ANMF")?;
            temporary.write_all(&(payload_len + 16).to_le_bytes())?;
            let mut frame = [0; 16];
            frame[6..9].copy_from_slice(&(size.0 - 1).to_le_bytes()[..3]);
            frame[9..12].copy_from_slice(&(size.1 - 1).to_le_bytes()[..3]);
            frame[12..15].copy_from_slice(
                &(duration_us.div_ceil(1000).clamp(1, 0xffffff) as u32).to_le_bytes()[..3],
            );
            frame[15] = 2; // replace, no alpha blending with the background
            temporary.write_all(&frame)?;
            for (offset, length) in payload {
                input.seek(SeekFrom::Start(offset))?;
                std::io::copy(&mut (&mut input).take(u64::from(length)), &mut temporary)?;
            }
        }
        _ => unreachable!(),
    }
    drop(input);
    temporary.flush()?;
    temporary.persist(path).map_err(|error| error.error)?;
    Ok(())
}
