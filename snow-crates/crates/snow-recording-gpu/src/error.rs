//! Error taxonomy for GPU video encoding. Initialization failures map to
//! the caller's "fall back to the CPU encode path" behavior.

use windows::Win32::Graphics::Dxgi::Common::DXGI_FORMAT;

pub type Result<T> = std::result::Result<T, GpuEncoderError>;

#[derive(Debug, thiserror::Error)]
pub enum GpuEncoderError {
    #[error("the D3D11 device cannot host video processing: {0}")]
    NoVideoProcessor(String),
    #[error("media foundation encoder initialization failed: {0}")]
    EncoderInit(String),
    #[error("the media foundation H.264 encoder is not D3D11-aware")]
    EncoderNotD3D11Aware,
    #[error("unsupported input texture format {0:?}")]
    UnsupportedInputFormat(DXGI_FORMAT),
    #[error("video processor conversion failed: {0}")]
    Convert(String),
    #[error("encoder submission failed: {0}")]
    Submit(String),
    #[error("encoder output retrieval failed: {0}")]
    Output(String),
    #[error("encoder drain did not complete in time")]
    DrainTimeout,
}
