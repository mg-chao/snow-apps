//! Shared D3D11 ownership for capture, video processing and hardware encoding.
//! No operation in this crate downloads a texture to host memory.

#[cfg(windows)]
mod device;
#[cfg(windows)]
pub use device::*;
#[cfg(windows)]
mod video;
#[cfg(windows)]
pub use video::*;

/// Select only the encoder belonging to the capture adapter. A different
/// adapter would require an explicit transfer and is not a zero-copy candidate.
pub fn h264_encoder(vendor: u32) -> Option<&'static str> {
    match vendor {
        0x10de => Some("h264_nvenc"),
        0x1002 => Some("h264_amf"),
        0x8086 => Some("h264_qsv"),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn encoder_is_selected_by_capture_adapter() {
        assert_eq!(super::h264_encoder(0x10de), Some("h264_nvenc"));
        assert_eq!(super::h264_encoder(0x1002), Some("h264_amf"));
        assert_eq!(super::h264_encoder(0x8086), Some("h264_qsv"));
        assert_eq!(super::h264_encoder(0x1414), None);
        assert_eq!(super::h264_encoder(0), None);
    }
}
