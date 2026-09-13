use crate::sampler::CursorProbe;
use crate::{CursorCaptureError, CursorCompositionMode, CursorShape, CursorShapeCapture};

pub(crate) struct MacOsCursorSampler {
    pixels: Vec<u8>,
}

impl MacOsCursorSampler {
    pub(crate) fn new() -> Result<Self, CursorCaptureError> {
        Ok(Self {
            pixels: vec![0; 256 * 256 * 4],
        })
    }

    pub(crate) fn sample_cursor(&mut self) -> Result<CursorProbe, CursorCaptureError> {
        let cursor = snow_macos::cursor(&mut self.pixels).map_err(CursorCaptureError::platform)?;
        let length = cursor.width as usize * cursor.height as usize * 4;
        Ok(CursorProbe {
            x: cursor.x,
            y: cursor.y,
            visible: true,
            shape: CursorShapeCapture::Captured(CursorShape::from_rgba(
                cursor.hotspot_x,
                cursor.hotspot_y,
                cursor.width,
                cursor.height,
                CursorCompositionMode::AlphaBlend,
                self.pixels[..length].to_vec(),
            )),
        })
    }
}
