use std::fmt;

use snow_core::error::{Classify, ErrorClass};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CursorCaptureError {
    UnsupportedPlatform,
    Platform(String),
}

impl CursorCaptureError {
    pub fn platform(message: impl Into<String>) -> Self {
        Self::Platform(message.into())
    }
}

impl fmt::Display for CursorCaptureError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::UnsupportedPlatform => {
                write!(f, "cursor capture is unsupported on this platform")
            }
            Self::Platform(message) => write!(f, "{message}"),
        }
    }
}

impl std::error::Error for CursorCaptureError {}

impl Classify for CursorCaptureError {
    fn class(&self) -> ErrorClass {
        match self {
            Self::UnsupportedPlatform => ErrorClass::InvalidConfig,
            Self::Platform(_) => ErrorClass::Transient,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use snow_core::error::{Classify, ErrorClass};

    #[test]
    fn unsupported_platform_classifies_as_invalid_config() {
        assert_eq!(
            CursorCaptureError::UnsupportedPlatform.class(),
            ErrorClass::InvalidConfig
        );
    }

    #[test]
    fn platform_error_classifies_as_transient() {
        assert_eq!(
            CursorCaptureError::platform("GetCursorInfo failed").class(),
            ErrorClass::Transient
        );
    }
}
