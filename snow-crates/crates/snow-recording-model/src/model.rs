use serde::{Deserialize, Serialize};

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct StoredFrame {
    pub timestamp_ms: u64,
    pub duration_ms: u32,
    pub width: u32,
    pub height: u32,
    pub rgba: snow_memory::RasterBuffer,
}

#[cfg(test)]
mod tests {
    use super::*;
    #[derive(Serialize, Deserialize)]
    struct LegacyStoredFrame {
        timestamp_ms: u64,
        duration_ms: u32,
        width: u32,
        height: u32,
        rgba: Vec<u8>,
    }
    #[test]
    fn raster_storage_preserves_recording_wire_format_and_maps_deserialized_pixels() {
        let legacy = LegacyStoredFrame {
            timestamp_ms: 7,
            duration_ms: 33,
            width: 1024,
            height: 256,
            rgba: vec![0x5a; 1024 * 256 * 4],
        };
        let encoded = bincode::serialize(&legacy).unwrap();
        let stored: StoredFrame = bincode::deserialize(&encoded).unwrap();
        assert_eq!(stored.rgba, legacy.rgba);
        assert_eq!(bincode::serialize(&stored).unwrap(), encoded);
        #[cfg(any(windows, target_os = "macos", target_os = "linux"))]
        assert!(stored.rgba.is_page_backed());
    }
}
