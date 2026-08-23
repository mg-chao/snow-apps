use std::{
    borrow::Cow,
    path::{Path, PathBuf},
};

use image::ImageReader;
use serde::{Deserialize, Serialize};

use crate::error::{RapidOcrError, Result};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum ColorOrder {
    Bgr,
    Rgb,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum VisionBackend {
    #[cfg_attr(not(feature = "opencv-backend"), default)]
    PureRust,
    #[cfg_attr(feature = "opencv-backend", default)]
    OpenCv,
}

impl VisionBackend {
    pub fn is_supported(self) -> bool {
        match self {
            Self::PureRust => true,
            Self::OpenCv => cfg!(feature = "opencv-backend"),
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum ModelType {
    #[default]
    Mobile,
    Server,
    Tiny,
    Small,
    Medium,
}

impl ModelType {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Mobile => "mobile",
            Self::Server => "server",
            Self::Tiny => "tiny",
            Self::Small => "small",
            Self::Medium => "medium",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
pub enum OcrVersion {
    #[default]
    #[serde(rename = "PP-OCRv4")]
    PPocrV4,
    #[serde(rename = "PP-OCRv5")]
    PPocrV5,
    #[serde(rename = "PP-OCRv6")]
    PPocrV6,
}

impl OcrVersion {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::PPocrV4 => "PP-OCRv4",
            Self::PPocrV5 => "PP-OCRv5",
            Self::PPocrV6 => "PP-OCRv6",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum LangDet {
    #[default]
    Ch,
    En,
    Multi,
}

impl LangDet {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Ch => "ch",
            Self::En => "en",
            Self::Multi => "multi",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum LangCls {
    #[default]
    Ch,
}

impl LangCls {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Ch => "ch",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum LangRec {
    #[default]
    Ch,
    ChDoc,
    En,
    Arabic,
    ChineseCht,
    Cyrillic,
    Devanagari,
    Japan,
    Korean,
    Ka,
    Latin,
    Ta,
    Te,
    Eslav,
    Th,
    El,
}

impl LangRec {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Ch => "ch",
            Self::ChDoc => "ch_doc",
            Self::En => "en",
            Self::Arabic => "arabic",
            Self::ChineseCht => "chinese_cht",
            Self::Cyrillic => "cyrillic",
            Self::Devanagari => "devanagari",
            Self::Japan => "japan",
            Self::Korean => "korean",
            Self::Ka => "ka",
            Self::Latin => "latin",
            Self::Ta => "ta",
            Self::Te => "te",
            Self::Eslav => "eslav",
            Self::Th => "th",
            Self::El => "el",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum ProviderPreference {
    #[default]
    Cpu,
    Auto {
        device_id: usize,
    },
    Cuda {
        device_id: usize,
    },
    DirectMl {
        device_id: usize,
    },
    Cann {
        device_id: usize,
    },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Serialize, Deserialize, Default)]
#[serde(rename_all = "snake_case")]
pub enum RuntimeBackend {
    #[default]
    OnnxCpu,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ModelConfig {
    pub lang: LangRec,
    pub ocr_version: OcrVersion,
    pub model_type: ModelType,
    pub model_path: Option<PathBuf>,
    pub rec_keys_path: Option<PathBuf>,
    pub allow_download: bool,
}

impl Default for ModelConfig {
    fn default() -> Self {
        Self {
            lang: LangRec::default(),
            ocr_version: OcrVersion::default(),
            model_type: ModelType::default(),
            model_path: None,
            rec_keys_path: None,
            allow_download: true,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct RuntimeConfig {
    pub backend: RuntimeBackend,
    /// Maximum host-thread budget for one engine. Services set this automatically per worker.
    pub thread_budget: Option<usize>,
    pub intra_threads: Option<usize>,
    pub inter_threads: Option<usize>,
    pub auto_tune_threads: bool,
    pub rayon_threads: Option<usize>,
    pub enable_cpu_mem_arena: bool,
    pub fail_if_provider_unavailable: bool,
    pub provider_preference: ProviderPreference,
    pub vision_backend: VisionBackend,
}

impl Default for RuntimeConfig {
    fn default() -> Self {
        Self {
            backend: RuntimeBackend::default(),
            thread_budget: None,
            intra_threads: None,
            inter_threads: None,
            auto_tune_threads: true,
            rayon_threads: None,
            enable_cpu_mem_arena: true,
            fail_if_provider_unavailable: false,
            provider_preference: ProviderPreference::default(),
            vision_backend: VisionBackend::default(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct RecognizerConfig {
    pub model: ModelConfig,
    pub runtime: RuntimeConfig,
    pub rec_batch_num: usize,
    pub rec_img_shape: [usize; 3],
    /// Rounds dynamic recognition tensor widths up to this multiple. Use 1 to disable.
    pub rec_width_alignment: usize,
    pub model_store_dir: Option<PathBuf>,
}

impl Default for RecognizerConfig {
    fn default() -> Self {
        Self {
            model: ModelConfig::default(),
            runtime: RuntimeConfig::default(),
            rec_batch_num: 6,
            rec_img_shape: [3, 48, 320],
            rec_width_alignment: 32,
            model_store_dir: None,
        }
    }
}

#[derive(Debug, Clone, Copy, Default, Serialize, Deserialize)]
pub struct RecognizeOptions {
    pub return_word_box: bool,
    pub return_single_char_box: bool,
}

#[derive(Debug, Clone)]
pub struct RecImage {
    width: usize,
    height: usize,
    data: Vec<u8>,
    color_order: ColorOrder,
}

impl RecImage {
    pub fn from_bgr_u8(width: usize, height: usize, data: Vec<u8>) -> Result<Self> {
        Self::new(width, height, data, ColorOrder::Bgr)
    }

    pub fn from_rgb_u8(width: usize, height: usize, data: Vec<u8>) -> Result<Self> {
        Self::new(width, height, data, ColorOrder::Rgb)
    }

    pub fn from_path(path: impl AsRef<Path>) -> Result<Self> {
        let image = ImageReader::open(path.as_ref())
            .map_err(|e| RapidOcrError::InvalidImage(e.to_string()))?
            .decode()
            .map_err(|e| RapidOcrError::InvalidImage(e.to_string()))?
            .to_rgb8();

        let (width, height) = image.dimensions();
        let raw = image.into_raw();

        let mut bgr = vec![0_u8; raw.len()];
        for (src, dst) in raw.chunks_exact(3).zip(bgr.chunks_exact_mut(3)) {
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
        }

        Self::new(width as usize, height as usize, bgr, ColorOrder::Bgr)
    }

    pub fn new(
        width: usize,
        height: usize,
        data: Vec<u8>,
        color_order: ColorOrder,
    ) -> Result<Self> {
        if width == 0 || height == 0 {
            return Err(RapidOcrError::InvalidImage(
                "image width and height must be greater than zero".to_string(),
            ));
        }

        let expected = width
            .checked_mul(height)
            .and_then(|v| v.checked_mul(3))
            .ok_or_else(|| RapidOcrError::InvalidImage("image dimensions overflow".to_string()))?;

        if data.len() != expected {
            return Err(RapidOcrError::InvalidImage(format!(
                "image data size mismatch: expected {expected}, got {}",
                data.len()
            )));
        }

        Ok(Self {
            width,
            height,
            data,
            color_order,
        })
    }

    pub fn width(&self) -> usize {
        self.width
    }

    pub fn height(&self) -> usize {
        self.height
    }

    pub fn color_order(&self) -> ColorOrder {
        self.color_order
    }

    pub fn as_bytes(&self) -> &[u8] {
        &self.data
    }

    pub fn into_bytes(self) -> Vec<u8> {
        self.data
    }
    pub fn as_bgr_cow(&self) -> Cow<'_, [u8]> {
        match self.color_order {
            ColorOrder::Bgr => Cow::Borrowed(&self.data),
            ColorOrder::Rgb => {
                let mut out = vec![0_u8; self.data.len()];
                for (src, dst) in self.data.chunks_exact(3).zip(out.chunks_exact_mut(3)) {
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                }
                Cow::Owned(out)
            }
        }
    }

    pub fn as_bgr_bytes(&self) -> Vec<u8> {
        self.as_bgr_cow().into_owned()
    }

    pub fn wh_ratio(&self) -> f32 {
        self.width() as f32 / self.height() as f32
    }
}

#[cfg(test)]
mod tests {
    use std::borrow::Cow;

    use super::{ColorOrder, RecImage, RuntimeBackend, RuntimeConfig, VisionBackend};

    #[test]
    fn rec_image_rejects_zero_dimension() {
        let err = RecImage::from_bgr_u8(0, 10, vec![]).expect_err("must reject zero width");
        assert!(
            err.to_string()
                .contains("image width and height must be greater than zero")
        );
    }

    #[test]
    fn runtime_config_default_backend_matches_feature() {
        let cfg = RuntimeConfig::default();
        assert_eq!(cfg.backend, RuntimeBackend::OnnxCpu);
        assert_eq!(cfg.thread_budget, None);
        assert!(cfg.auto_tune_threads);
        assert_eq!(cfg.rayon_threads, None);
        assert!(cfg.enable_cpu_mem_arena);
        assert!(!cfg.fail_if_provider_unavailable);
        #[cfg(feature = "opencv-backend")]
        assert_eq!(cfg.vision_backend, VisionBackend::OpenCv);
        #[cfg(not(feature = "opencv-backend"))]
        assert_eq!(cfg.vision_backend, VisionBackend::PureRust);
    }

    #[test]
    fn as_bgr_cow_borrows_for_bgr_images() {
        let image =
            RecImage::new(2, 1, vec![1, 2, 3, 4, 5, 6], ColorOrder::Bgr).expect("valid image");
        let bgr = image.as_bgr_cow();
        assert!(matches!(bgr, Cow::Borrowed(_)));
    }

    #[test]
    fn as_bgr_cow_allocates_for_rgb_images() {
        let image = RecImage::new(1, 1, vec![10, 20, 30], ColorOrder::Rgb).expect("valid image");
        let bgr = image.as_bgr_cow();
        assert!(matches!(bgr, Cow::Owned(_)));
        assert_eq!(bgr.as_ref(), &[30, 20, 10]);
    }
}
