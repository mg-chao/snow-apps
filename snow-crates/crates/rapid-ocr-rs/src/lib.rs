mod cls;
mod config;
mod det;
mod error;
mod input;
mod model_registry;
mod model_source;
mod model_store;
mod output;
mod pipeline;
mod rec;
mod runtime;
mod service;
mod types;
mod vision;

pub use config::{
    ColorOrder, LangCls, LangDet, LangRec, ModelConfig, ModelType, OcrVersion, ProviderPreference,
    RecImage, RecognizeOptions, RecognizerConfig, RuntimeBackend, RuntimeConfig, VisionBackend,
};
pub use error::{RapidOcrError, Result};
pub use input::image_loader::{LoadImage, OcrInput};
pub use model_source::{DictionarySource, ModelSource, PipelineSources};
pub use output::json::OcrJsonItem;
pub use pipeline::{
    config::{EngineConfig, GlobalConfig},
    rapid_ocr::{PipelineProviderResolutions, RapidOcr, RapidOcrEngine},
    types::{
        ClsResult, DetResult, FullResult, OcrCallOptions, OcrOutput, OcrResult, RecResult,
        RunOptions, StageTimings,
    },
};
pub use runtime::provider::{ProviderResolution, ResolvedExecutionProvider};
pub use service::{OcrService, RequestToken, ServiceStats};
pub use types::{LineResult, RecognizeOutput, WordBox, WordInfo, WordType};

pub type Quad = [[f32; 2]; 4];

pub fn initialize_onnx_runtime() -> Result<()> {
    // Keep managed ONNX diagnostics on stderr; the host protocol reader also
    // resynchronizes on the frame magic for native runtime builds that emit
    // unavoidable cpuinfo diagnostics on stdout.
    let _ = ort::init()
        .with_telemetry(false)
        .with_logger(std::sync::Arc::new(
            |_level: ort::logging::LogLevel,
             _category: &str,
             _id: &str,
             _location: &str,
             message: &str| {
                eprintln!("ONNX Runtime: {message}");
            },
        ))
        .commit();
    Ok(())
}

pub fn directml_is_available() -> bool {
    #[cfg(feature = "directml-provider")]
    {
        use ort::ep::{DirectML, ExecutionProvider};

        DirectML::default()
            .with_device_id(0)
            .is_available()
            .unwrap_or(false)
    }
    #[cfg(not(feature = "directml-provider"))]
    {
        false
    }
}
