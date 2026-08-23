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
pub use pipeline::compat_rapidocr::{from_rapidocr_yaml_file, from_rapidocr_yaml_str};
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
    ort::init().commit();
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
