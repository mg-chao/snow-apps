use std::io;

use thiserror::Error;

#[derive(Debug, Error)]
pub enum ScreenRecorderError {
    #[error("invalid config: {0}")]
    InvalidConfig(String),

    #[error("unsupported feature: {0}")]
    UnsupportedFeature(String),

    #[error("capture error: {0}")]
    Capture(#[from] snow_capture::error::CaptureError),

    #[error("audio error: {0}")]
    Audio(#[from] snow_audio_recorder::error::AudioError),

    #[error("io error: {0}")]
    Io(#[from] io::Error),

    #[error("encode error: {0}")]
    Encode(String),

    #[error("decode error: {0}")]
    Decode(String),

    #[error("export error: {0}")]
    Export(String),

    #[error("export canceled")]
    ExportCanceled,
}

pub type Result<T> = std::result::Result<T, ScreenRecorderError>;

impl From<snow_recording_model::RecordingModelError> for ScreenRecorderError {
    fn from(value: snow_recording_model::RecordingModelError) -> Self {
        match value {
            snow_recording_model::RecordingModelError::Io(err) => Self::Io(err),
            snow_recording_model::RecordingModelError::Decode(err) => Self::Decode(err),
        }
    }
}

impl From<snow_recording_export::RecordingExportError> for ScreenRecorderError {
    fn from(value: snow_recording_export::RecordingExportError) -> Self {
        match value {
            snow_recording_export::RecordingExportError::InvalidConfig(error) => {
                Self::InvalidConfig(error)
            }
            snow_recording_export::RecordingExportError::Encode(error) => Self::Encode(error),
            snow_recording_export::RecordingExportError::Io(error) => Self::Io(error),
            snow_recording_export::RecordingExportError::Decode(error) => Self::Decode(error),
            snow_recording_export::RecordingExportError::Export(error) => Self::Export(error),
            snow_recording_export::RecordingExportError::ExportCanceled => Self::ExportCanceled,
        }
    }
}
