//! Errors shared by the portable detector and its optional adapters.

use std::fmt;

/// Result type used by the detector's backend-independent API.
pub type Result<T> = std::result::Result<T, Error>;

/// Errors that can occur while preparing, processing, or exporting an image.
#[derive(Debug)]
pub enum Error {
    /// The input does not satisfy the detector's image contract.
    InvalidImage(String),
    /// A selected backend could not complete an operation.
    Backend(String),
    /// A filesystem operation failed.
    Io(std::io::Error),
    /// Image decoding or encoding failed.
    Image(image::ImageError),
    /// JSON serialization failed while writing an index or payload.
    Json(serde_json::Error),
    /// An optional OpenCV adapter reported an error.
    #[cfg(feature = "opencv")]
    OpenCv(opencv::Error),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidImage(message) => write!(f, "invalid image: {message}"),
            Self::Backend(message) => write!(f, "backend error: {message}"),
            Self::Io(error) => error.fmt(f),
            Self::Image(error) => error.fmt(f),
            Self::Json(error) => error.fmt(f),
            #[cfg(feature = "opencv")]
            Self::OpenCv(error) => error.fmt(f),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(error) => Some(error),
            Self::Image(error) => Some(error),
            Self::Json(error) => Some(error),
            #[cfg(feature = "opencv")]
            Self::OpenCv(error) => Some(error),
            Self::InvalidImage(_) | Self::Backend(_) => None,
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(error: std::io::Error) -> Self {
        Self::Io(error)
    }
}

impl From<image::ImageError> for Error {
    fn from(error: image::ImageError) -> Self {
        Self::Image(error)
    }
}

impl From<serde_json::Error> for Error {
    fn from(error: serde_json::Error) -> Self {
        Self::Json(error)
    }
}

#[cfg(feature = "opencv")]
impl From<opencv::Error> for Error {
    fn from(error: opencv::Error) -> Self {
        Self::OpenCv(error)
    }
}
