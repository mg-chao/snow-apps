use std::path::PathBuf;

use ndarray::ArrayView4;
use rayon::prelude::*;
use serde::{Deserialize, Serialize};

use crate::{
    config::{LangCls, ModelType, OcrVersion, RecImage, RuntimeConfig, VisionBackend},
    error::{RapidOcrError, Result},
    model_registry::ModelRegistry,
    model_source::ModelSource,
    model_store::{default_model_store_dir, ensure_downloaded, verify_existing_file},
    runtime::provider::ProviderResolution,
    runtime::session::{OrtSession, SessionContract},
    vision::backend::resolve_backend_strict,
    vision::resize::LinearResizeScratch,
};

use super::{postprocess, preprocess};

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct ClassifierConfig {
    pub lang: LangCls,
    pub ocr_version: OcrVersion,
    pub model_type: ModelType,
    pub model_path: Option<PathBuf>,
    pub allow_download: bool,
    pub runtime: RuntimeConfig,
    pub cls_image_shape: [usize; 3],
    pub cls_batch_num: usize,
    pub cls_thresh: f32,
    pub label_list: Vec<String>,
    pub model_store_dir: Option<PathBuf>,
}

impl Default for ClassifierConfig {
    fn default() -> Self {
        Self {
            lang: LangCls::Ch,
            ocr_version: OcrVersion::PPocrV4,
            model_type: ModelType::Mobile,
            model_path: None,
            allow_download: true,
            runtime: RuntimeConfig::default(),
            cls_image_shape: [3, 48, 192],
            cls_batch_num: 6,
            cls_thresh: 0.9,
            label_list: vec!["0".to_string(), "180".to_string()],
            model_store_dir: None,
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct ClsInPlaceOutput {
    pub cls_res: Vec<(String, f32)>,
    /// Wall-clock classification duration; `None` unless stage timing was
    /// enabled via `set_stage_timing_enabled`.
    pub elapsed_ms: Option<f32>,
}

#[derive(Debug)]
pub struct Classifier {
    config: ClassifierConfig,
    vision_backend: VisionBackend,
    session: OrtSession,
    batch_scratch: Vec<f32>,
}

impl Classifier {
    pub fn new(config: ClassifierConfig) -> Result<Self> {
        if config.cls_batch_num == 0 {
            return Err(RapidOcrError::Config(
                "cls_batch_num must be greater than zero".to_string(),
            ));
        }

        let model_store_dir = config
            .model_store_dir
            .clone()
            .unwrap_or_else(default_model_store_dir);

        let model_path = if let Some(path) = &config.model_path {
            verify_existing_file(path)?
        } else if config.allow_download {
            let registry = ModelRegistry::from_default_yaml()?;
            let resolved =
                registry.resolve_cls(config.ocr_version, config.lang, config.model_type)?;
            ensure_downloaded(
                &resolved.model_url,
                resolved.sha256.as_deref(),
                model_store_dir,
            )?
        } else {
            return Err(RapidOcrError::Config(
                "classifier model_path is not set and allow_download=false".to_string(),
            ));
        };

        Self::new_with_source(config, ModelSource::File(&model_path))
    }

    pub fn new_with_source(config: ClassifierConfig, source: ModelSource<'_>) -> Result<Self> {
        if config.cls_batch_num == 0 {
            return Err(RapidOcrError::Config(
                "cls_batch_num must be greater than zero".to_string(),
            ));
        }

        let session = OrtSession::new_from_source(source, &config.runtime, SessionContract::Cls)?;
        let vision_backend = resolve_backend_strict(config.runtime.vision_backend)?;
        Ok(Self {
            vision_backend,
            config,
            session,
            batch_scratch: Vec::new(),
        })
    }

    pub fn classify_in_place(&mut self, images: &mut [RecImage]) -> Result<ClsInPlaceOutput> {
        let timing = crate::diagnostics::timing_start();
        if images.is_empty() {
            return Ok(ClsInPlaceOutput::default());
        }

        let mut cls_res = vec![("".to_string(), 0.0_f32); images.len()];

        let mut indices: Vec<usize> = (0..images.len()).collect();
        indices.sort_by(|a, b| {
            images[*a]
                .wh_ratio()
                .partial_cmp(&images[*b].wh_ratio())
                .unwrap_or(std::cmp::Ordering::Equal)
        });

        for beg in (0..images.len()).step_by(self.config.cls_batch_num) {
            let end = (beg + self.config.cls_batch_num).min(images.len());
            let batch_size = end - beg;
            let img_c = self.config.cls_image_shape[0];
            let img_h = self.config.cls_image_shape[1];
            let img_w = self.config.cls_image_shape[2];
            let sample_len = img_c
                .checked_mul(img_h)
                .and_then(|v| v.checked_mul(img_w))
                .ok_or_else(|| {
                    RapidOcrError::InvalidInput("cls batch sample size overflow".to_string())
                })?;
            let total_len = sample_len.checked_mul(batch_size).ok_or_else(|| {
                RapidOcrError::InvalidInput("cls batch size overflow".to_string())
            })?;
            self.batch_scratch.resize(total_len, 0.0);

            let batch_indices = &indices[beg..end];
            if batch_size > 1 {
                self.batch_scratch[..total_len]
                    .par_chunks_mut(sample_len)
                    .zip(batch_indices.par_iter().copied())
                    .try_for_each_init(
                        || (Vec::<u8>::new(), LinearResizeScratch::default()),
                        |(tmp_bgr, resize_scratch), (dst, sorted_idx)| {
                            preprocess::write_resize_norm_img_into_slice_with_scratch(
                                &images[sorted_idx],
                                self.config.cls_image_shape,
                                self.vision_backend,
                                dst,
                                tmp_bgr,
                                resize_scratch,
                            )
                        },
                    )?;
            } else {
                let sorted_idx = batch_indices[0];
                let mut tmp_bgr = Vec::new();
                let mut resize_scratch = LinearResizeScratch::default();
                preprocess::write_resize_norm_img_into_slice_with_scratch(
                    &images[sorted_idx],
                    self.config.cls_image_shape,
                    self.vision_backend,
                    &mut self.batch_scratch[..sample_len],
                    &mut tmp_bgr,
                    &mut resize_scratch,
                )?;
            }

            let batch_view =
                ArrayView4::from_shape((batch_size, img_c, img_h, img_w), &self.batch_scratch)
                    .map_err(|e| {
                        RapidOcrError::InvalidInput(format!("invalid cls batch tensor shape: {e}"))
                    })?;

            let label_list = &self.config.label_list;
            let decoded = self.session.run_array2_view_with(batch_view, |preds| {
                postprocess::decode_view(preds, label_list)
            })?;
            for (rno, (label, score)) in decoded.into_iter().enumerate() {
                let target = indices[beg + rno];
                if label.contains("180") && score > self.config.cls_thresh {
                    images[target] =
                        preprocess::rotate_180_with_backend(&images[target], self.vision_backend)?;
                }
                cls_res[target] = (label, score);
            }
        }

        Ok(ClsInPlaceOutput {
            cls_res,
            elapsed_ms: crate::diagnostics::timing_ms(timing),
        })
    }

    pub fn provider_resolution(&self) -> ProviderResolution {
        self.session.provider_resolution()
    }
}
