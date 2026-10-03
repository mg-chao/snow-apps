//! Owned encoder input and the pristine-background generation of its overlay coverage.
use snow_memory::RasterBuffer;

#[derive(Default)]
pub(super) struct VideoBuffer {
    pub pixels: RasterBuffer,
    pub history: Option<super::damage::History>,
}

impl From<Vec<u8>> for VideoBuffer {
    fn from(pixels: Vec<u8>) -> Self {
        Self {
            pixels: pixels.into(),
            history: None,
        }
    }
}

impl From<RasterBuffer> for VideoBuffer {
    fn from(pixels: RasterBuffer) -> Self {
        Self {
            pixels,
            history: None,
        }
    }
}
