use std::{path::PathBuf, time::Instant};

use ndarray::ArrayView4;
use rayon::prelude::*;

use crate::{
    config::{LangRec, RecImage, RecognizeOptions, RecognizerConfig, VisionBackend},
    error::{RapidOcrError, Result},
    model_registry::{ModelRegistry, ResolvedRecModel},
    model_source::{DictionarySource, ModelSource},
    model_store::{default_model_store_dir, ensure_downloaded, verify_existing_file},
    rec::{
        bidi::reorder_bidi_for_display,
        decode::CtcLabelDecoder,
        preprocess::{batch_shape_for, write_resize_norm_img_into_slice_with_scratch},
    },
    runtime::provider::ProviderResolution,
    runtime::session::OrtSession,
    types::{LineResult, RecognizeOutput},
    vision::backend::resolve_backend_strict,
    vision::resize::LinearResizeScratch,
};

#[derive(Debug)]
pub struct Recognizer {
    config: RecognizerConfig,
    vision_backend: VisionBackend,
    session: OrtSession,
    decoder: CtcLabelDecoder,
    batch_scratch: Vec<f32>,
}

impl Recognizer {
    pub fn new(config: RecognizerConfig) -> Result<Self> {
        if config.rec_img_shape[0] != 3 {
            return Err(RapidOcrError::Config(format!(
                "rec_img_shape must start with channel=3, got {:?}",
                config.rec_img_shape
            )));
        }
        if config.rec_batch_num == 0 {
            return Err(RapidOcrError::Config(
                "rec_batch_num must be greater than zero".to_string(),
            ));
        }
        if config.rec_width_alignment == 0 {
            return Err(RapidOcrError::Config(
                "rec_width_alignment must be greater than zero".to_string(),
            ));
        }

        let model_store_dir = config
            .model_store_dir
            .clone()
            .unwrap_or_else(default_model_store_dir);
        let registry = ModelRegistry::from_default_yaml()?;
        let resolved = registry.resolve_rec(
            config.model.ocr_version,
            config.model.lang,
            config.model.model_type,
        )?;

        let model_path = resolve_model_path(&config, &resolved, &model_store_dir)?;
        let character_path = if config.model.rec_keys_path.is_some() {
            config.model.rec_keys_path.clone()
        } else {
            resolve_character_path(&config, &resolved, &model_store_dir)?
        };

        Self::new_with_sources(
            config,
            ModelSource::File(&model_path),
            character_path.as_deref().map(DictionarySource::File),
        )
    }

    pub fn new_with_sources(
        config: RecognizerConfig,
        model_source: ModelSource<'_>,
        dictionary_source: Option<DictionarySource<'_>>,
    ) -> Result<Self> {
        if config.rec_img_shape[0] != 3 {
            return Err(RapidOcrError::Config(format!(
                "rec_img_shape must start with channel=3, got {:?}",
                config.rec_img_shape
            )));
        }
        if config.rec_batch_num == 0 {
            return Err(RapidOcrError::Config(
                "rec_batch_num must be greater than zero".to_string(),
            ));
        }
        if config.rec_width_alignment == 0 {
            return Err(RapidOcrError::Config(
                "rec_width_alignment must be greater than zero".to_string(),
            ));
        }

        let vision_backend = resolve_backend_strict(config.runtime.vision_backend)?;
        let mut session = OrtSession::new_from_source(
            model_source,
            &config.runtime,
            crate::runtime::session::SessionContract::Rec,
        )?;
        let character = session.character_list.take();
        let decoder = if character.is_some() {
            CtcLabelDecoder::new(character, None)?
        } else {
            match dictionary_source {
                Some(DictionarySource::File(path)) => CtcLabelDecoder::new(None, Some(path))?,
                Some(DictionarySource::Memory { text, .. }) => {
                    CtcLabelDecoder::new_from_text(None, Some(text))?
                }
                None => CtcLabelDecoder::new(None, None)?,
            }
        };

        Ok(Self {
            config,
            vision_backend,
            session,
            decoder,
            batch_scratch: Vec::new(),
        })
    }

    pub fn recognize(
        &mut self,
        images: &[RecImage],
        options: RecognizeOptions,
    ) -> Result<RecognizeOutput> {
        let start = Instant::now();

        if images.is_empty() {
            return Ok(RecognizeOutput::default());
        }

        let width_list: Vec<f64> = images
            .iter()
            .map(|img| img.width() as f64 / img.height() as f64)
            .collect();
        let mut indices: Vec<usize> = (0..images.len()).collect();
        indices.sort_by(|a, b| {
            width_list[*a]
                .partial_cmp(&width_list[*b])
                .unwrap_or(std::cmp::Ordering::Equal)
        });

        let mut rec_res: Vec<Option<LineResult>> = vec![None; images.len()];

        for beg in (0..images.len()).step_by(self.config.rec_batch_num) {
            let end = (beg + self.config.rec_batch_num).min(images.len());
            let batch_indices = &indices[beg..end];

            let img_h = self.config.rec_img_shape[1] as f64;
            let img_w = self.config.rec_img_shape[2] as f64;
            let mut max_wh_ratio = img_w / img_h;

            let mut wh_ratio_list = Vec::with_capacity(end - beg);
            for sorted_idx in batch_indices {
                let img = &images[*sorted_idx];
                let wh_ratio = img.width() as f64 / img.height() as f64;
                max_wh_ratio = max_wh_ratio.max(wh_ratio);
                wh_ratio_list.push(wh_ratio as f32);
            }

            let (img_channel, img_height, natural_width) =
                batch_shape_for(max_wh_ratio, self.config.rec_img_shape)?;
            let dst_width = align_width(natural_width, self.config.rec_width_alignment)?;
            let tensor_wh_ratio = dst_width as f64 / img_height as f64;
            let sample_len = img_channel
                .checked_mul(img_height)
                .and_then(|v| v.checked_mul(dst_width))
                .ok_or_else(|| {
                    RapidOcrError::InvalidInput("rec batch sample size overflow".to_string())
                })?;
            let batch_size = batch_indices.len();
            let total_len = sample_len.checked_mul(batch_size).ok_or_else(|| {
                RapidOcrError::InvalidInput("rec batch size overflow".to_string())
            })?;
            self.batch_scratch.resize(total_len, 0.0);

            if batch_size > 1 {
                self.batch_scratch[..total_len]
                    .par_chunks_mut(sample_len)
                    .zip(batch_indices.par_iter().copied())
                    .try_for_each_init(
                        || (Vec::<u8>::new(), LinearResizeScratch::default()),
                        |(tmp_bgr, resize_scratch), (dst, image_idx)| {
                            let image = images.get(image_idx).ok_or_else(|| {
                                RapidOcrError::InvalidInput(format!(
                                    "batch index {image_idx} out of bounds for image count {}",
                                    images.len()
                                ))
                            })?;
                            write_resize_norm_img_into_slice_with_scratch(
                                image,
                                tensor_wh_ratio,
                                self.config.rec_img_shape,
                                self.vision_backend,
                                dst,
                                tmp_bgr,
                                resize_scratch,
                            )
                        },
                    )?;
            } else {
                let image_idx = batch_indices[0];
                let image = images.get(image_idx).ok_or_else(|| {
                    RapidOcrError::InvalidInput(format!(
                        "batch index {image_idx} out of bounds for image count {}",
                        images.len()
                    ))
                })?;
                let mut tmp_bgr = Vec::new();
                let mut resize_scratch = LinearResizeScratch::default();
                write_resize_norm_img_into_slice_with_scratch(
                    image,
                    tensor_wh_ratio,
                    self.config.rec_img_shape,
                    self.vision_backend,
                    &mut self.batch_scratch[..sample_len],
                    &mut tmp_bgr,
                    &mut resize_scratch,
                )?;
            }

            let batch_view = ArrayView4::from_shape(
                (batch_size, img_channel, img_height, dst_width),
                &self.batch_scratch[..total_len],
            )
            .map_err(|e| {
                RapidOcrError::InvalidInput(format!("invalid rec batch tensor shape: {e}"))
            })?;
            let decoder = &self.decoder;
            let (line_results, word_results) =
                self.session.run_array3_view_with(batch_view, |preds| {
                    decoder.decode_view(
                        preds,
                        options.return_word_box,
                        &wh_ratio_list,
                        tensor_wh_ratio as f32,
                    )
                })?;

            for (rno, (text, score)) in line_results.into_iter().enumerate() {
                let word_info = if options.return_word_box {
                    word_results.get(rno).cloned()
                } else {
                    None
                };

                let target_idx = indices[beg + rno];
                rec_res[target_idx] = Some(LineResult {
                    text,
                    score,
                    word_info,
                });
            }
        }

        let mut lines = Vec::with_capacity(images.len());
        for line in rec_res.into_iter().flatten() {
            lines.push(line);
        }

        if self.config.model.lang == LangRec::Arabic {
            for line in &mut lines {
                line.text = reorder_bidi_for_display(&line.text);
            }
        }

        Ok(RecognizeOutput {
            lines,
            elapsed: start.elapsed(),
        })
    }

    pub fn provider_resolution(&self) -> ProviderResolution {
        self.session.provider_resolution()
    }
}

fn align_width(width: usize, alignment: usize) -> Result<usize> {
    if alignment == 0 {
        return Err(RapidOcrError::Config(
            "rec_width_alignment must be greater than zero".to_string(),
        ));
    }
    let remainder = width % alignment;
    if remainder == 0 {
        return Ok(width);
    }
    width
        .checked_add(alignment - remainder)
        .ok_or_else(|| RapidOcrError::InvalidInput("recognition width overflow".to_string()))
}

#[cfg(test)]
mod width_alignment_tests {
    use super::align_width;

    #[test]
    fn dynamic_width_rounds_up_to_configured_multiple() {
        assert_eq!(align_width(320, 32).expect("aligned width"), 320);
        assert_eq!(align_width(321, 32).expect("rounded width"), 352);
        assert_eq!(align_width(321, 1).expect("disabled alignment"), 321);
    }

    #[test]
    fn zero_width_alignment_is_rejected() {
        assert!(align_width(320, 0).is_err());
    }
}

fn resolve_model_path(
    config: &RecognizerConfig,
    resolved: &ResolvedRecModel,
    model_store_dir: &PathBuf,
) -> Result<PathBuf> {
    if let Some(model_path) = &config.model.model_path {
        return verify_existing_file(model_path);
    }

    if !config.model.allow_download {
        return Err(RapidOcrError::Config(
            "model_path is not set and allow_download=false".to_string(),
        ));
    }

    ensure_downloaded(
        &resolved.model_url,
        resolved.sha256.as_deref(),
        model_store_dir,
    )
}

fn resolve_character_path(
    config: &RecognizerConfig,
    resolved: &ResolvedRecModel,
    model_store_dir: &PathBuf,
) -> Result<Option<PathBuf>> {
    if let Some(path) = &config.model.rec_keys_path {
        return Ok(Some(verify_existing_file(path)?));
    }

    let Some(dict_url) = &resolved.dict_url else {
        return Ok(None);
    };

    if !config.model.allow_download {
        return Err(RapidOcrError::Config(
            "character metadata missing and dict download disabled".to_string(),
        ));
    }

    let path = ensure_downloaded(dict_url, None, model_store_dir)?;
    Ok(Some(path))
}

#[cfg(all(test, feature = "opencv-backend"))]
mod tests {
    use std::{fs, path::PathBuf};

    use crate::{
        config::{
            LangRec, ModelType, OcrVersion, ProviderPreference, RecImage, RecognizeOptions,
            RecognizerConfig, RuntimeConfig, VisionBackend,
        },
        rec::recognizer::Recognizer,
        runtime::provider::ResolvedExecutionProvider,
    };

    fn test_images() -> Vec<RecImage> {
        let mut root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        root.push("test");
        root.push("test_files");

        let mut paths = fs::read_dir(&root)
            .expect("test fixture directory should exist")
            .map(|entry| entry.expect("fixture entry should be readable").path())
            .filter(|path| {
                path.extension()
                    .and_then(|v| v.to_str())
                    .is_some_and(|ext| {
                        matches!(ext.to_ascii_lowercase().as_str(), "png" | "jpg" | "jpeg")
                    })
            })
            .collect::<Vec<_>>();
        paths.sort();

        paths
            .into_iter()
            .map(|path| RecImage::from_path(&path).expect("fixture image should load"))
            .collect()
    }

    fn recognizer_config(
        version: OcrVersion,
        model_type: ModelType,
        vision_backend: VisionBackend,
    ) -> RecognizerConfig {
        let mut model_store_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        model_store_dir.push("target");
        model_store_dir.push("rec-parity-models");

        RecognizerConfig {
            model: crate::config::ModelConfig {
                lang: LangRec::Ch,
                ocr_version: version,
                model_type,
                model_path: None,
                rec_keys_path: None,
                allow_download: true,
            },
            runtime: RuntimeConfig {
                vision_backend,
                auto_tune_threads: false,
                intra_threads: Some(1),
                inter_threads: Some(1),
                rayon_threads: Some(1),
                provider_preference: ProviderPreference::Cpu,
                ..RuntimeConfig::default()
            },
            rec_batch_num: 6,
            rec_img_shape: [3, 48, 320],
            rec_width_alignment: 32,
            model_store_dir: Some(model_store_dir),
        }
    }

    #[test]
    #[ignore = "downloads v4/v5/v6 recognition models and runs ONNX inference on all image fixtures"]
    fn pure_and_opencv_recognition_match_ch_v4_v5_v6_on_test_images() {
        let images = test_images();
        let versions = [
            (OcrVersion::PPocrV4, ModelType::Mobile),
            (OcrVersion::PPocrV5, ModelType::Mobile),
            (OcrVersion::PPocrV6, ModelType::Small),
        ];

        for (version, model_type) in versions {
            let mut pure = Recognizer::new(recognizer_config(
                version,
                model_type,
                VisionBackend::PureRust,
            ))
            .expect("pure recognizer should initialize");
            let mut opencv = Recognizer::new(recognizer_config(
                version,
                model_type,
                VisionBackend::OpenCv,
            ))
            .expect("opencv recognizer should initialize");

            assert!(matches!(
                pure.provider_resolution().resolved,
                ResolvedExecutionProvider::Cpu
            ));
            assert!(matches!(
                opencv.provider_resolution().resolved,
                ResolvedExecutionProvider::Cpu
            ));

            let opts = RecognizeOptions {
                return_word_box: false,
                return_single_char_box: false,
            };
            let pure_out = pure
                .recognize(&images, opts)
                .expect("pure recognition should run");
            let opencv_out = opencv
                .recognize(&images, opts)
                .expect("opencv recognition should run");

            assert_eq!(
                pure_out.lines.len(),
                opencv_out.lines.len(),
                "line count mismatch for {version:?}"
            );
            for (idx, (pure_line, opencv_line)) in pure_out
                .lines
                .iter()
                .zip(opencv_out.lines.iter())
                .enumerate()
            {
                assert_eq!(
                    pure_line.text, opencv_line.text,
                    "text mismatch for {version:?} image index {idx}"
                );
                assert_eq!(
                    pure_line.score, opencv_line.score,
                    "score mismatch for {version:?} image index {idx}"
                );
            }
        }
    }
}
