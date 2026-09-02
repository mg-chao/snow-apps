use std::path::PathBuf;

use crate::Geometry;

#[derive(Debug, thiserror::Error)]
pub enum StitchError {
    #[error("at least one input frame is required")]
    EmptyInput,

    #[error("first frame must be larger than 4x4 pixels, got {width}x{height}")]
    InvalidFirstGeometry { width: u32, height: u32 },

    #[error("frame {index} has geometry {actual}; expected {expected}")]
    ViewportMismatch {
        index: usize,
        expected: Geometry,
        actual: Geometry,
    },

    #[error("invalid stitching options: {message}")]
    InvalidOptions { message: String },

    #[error("invalid frame buffer: {message}")]
    InvalidFrame { message: String },

    #[error(
        "crop ({x}, {y}, {width}, {height}) is outside image bounds {image_width}x{image_height}"
    )]
    CheckedCrop {
        x: u32,
        y: u32,
        width: u32,
        height: u32,
        image_width: u32,
        image_height: u32,
    },

    #[error("checked arithmetic failed while {operation}")]
    Arithmetic { operation: &'static str },

    #[error("could not decode image {path}: {source}")]
    Decode {
        path: PathBuf,
        #[source]
        source: image::ImageError,
    },

    #[error("could not encode image {path}: {source}")]
    Encode {
        path: PathBuf,
        #[source]
        source: image::ImageError,
    },

    #[error("could not serialize decision-log JSON: {0}")]
    DecisionSerialization(#[from] serde_json::Error),

    #[error("I/O failure for {path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
}
