use std::path::PathBuf;

use thiserror::Error;

#[derive(Debug, Error)]
pub enum RapidOcrError {
    #[error("invalid configuration: {0}")]
    Config(String),

    #[error("model resolution failed: {0}")]
    ModelResolve(String),

    #[error("download failed: {0}")]
    Download(String),

    #[error("file not found: {0}")]
    FileNotFound(PathBuf),

    #[error("invalid image: {0}")]
    InvalidImage(String),

    #[error("invalid input: {0}")]
    InvalidInput(String),

    #[error("decoding failed: {0}")]
    Decode(String),

    #[error("unsupported runtime backend: {0}")]
    UnsupportedBackend(String),

    #[error(transparent)]
    Io(#[from] std::io::Error),

    #[error(transparent)]
    Ort(#[from] ort::Error),

    #[error(transparent)]
    Yaml(#[from] serde_yaml::Error),

    #[error(transparent)]
    Reqwest(#[from] reqwest::Error),

    #[error("hash mismatch for {path:?}: expected {expected}, got {actual}")]
    HashMismatch {
        path: PathBuf,
        expected: String,
        actual: String,
    },
}

pub type Result<T> = std::result::Result<T, RapidOcrError>;
