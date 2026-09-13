//! Owned encoder input and the pristine-background generation of its overlay coverage.

#[derive(Default)]
pub(super) struct VideoBuffer {
    pub pixels: Vec<u8>,
    pub history: Option<super::damage::History>,
}

impl From<Vec<u8>> for VideoBuffer {
    fn from(pixels: Vec<u8>) -> Self {
        Self {
            pixels,
            history: None,
        }
    }
}
