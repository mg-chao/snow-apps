use std::sync::Arc;
use std::thread;
use std::time::Instant;

use crate::{
    cls::classifier::{Classifier, ClassifierConfig},
    config::RecognizeOptions,
    det::detector::{Detector, DetectorConfig},
    error::{RapidOcrError, Result},
    input::image_loader::{LoadImage, OcrInput},
    model_source::PipelineSources,
    pipeline::{
        config::EngineConfig,
        image_ops::{
            PreprocessRecord, apply_vertical_padding_in_place, crop_text_regions,
            map_boxes_to_original, map_img_to_original, resize_image_within_bounds,
        },
        types::{OcrCallOptions, OcrOutput, OcrResult, RunOptions},
    },
    rec::recognizer::Recognizer,
    runtime::provider::ProviderResolution,
    types::{LineResult, WordBox},
};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PipelineProviderResolutions {
    pub det: Option<ProviderResolution>,
    pub cls: Option<ProviderResolution>,
    pub rec: Option<ProviderResolution>,
}

#[derive(Debug, Clone, Copy)]
struct RunSwitches {
    use_det: bool,
    use_cls: bool,
    use_rec: bool,
    need_stage_images: bool,
    return_word_box: bool,
    return_single_char_box: bool,
    text_score: f32,
}

#[derive(Debug)]
struct PreparedImage {
    ori_h: usize,
    ori_w: usize,
    ratio_h: f32,
    ratio_w: f32,
    preprocess_record: PreprocessRecord,
    proc_img: crate::config::RecImage,
}

#[derive(Debug, Default)]
struct RunBuffers {
    det_boxes: Vec<crate::Quad>,
    det_scores: Vec<f32>,
    stage_images: Vec<crate::config::RecImage>,
    lines: Vec<LineResult>,
}

#[derive(Debug)]
pub struct RapidOcr {
    executor: Arc<rayon::ThreadPool>,
    config: EngineConfig,
    detector: Option<Detector>,
    classifier: Option<Classifier>,
    recognizer: Option<Recognizer>,
    loader: LoadImage,
}

impl RapidOcr {
    pub fn new(config: EngineConfig) -> Result<Self> {
        Self::new_with_sources(config, PipelineSources::default())
    }

    pub fn new_with_sources(config: EngineConfig, sources: PipelineSources<'_>) -> Result<Self> {
        config.validate()?;
        let executor = build_executor(&config)?;
        let det = if config.global.use_det {
            Some(match sources.det {
                Some(source) => {
                    Detector::new_with_source(detector_cfg_from_pipeline(&config), source)?
                }
                None => Detector::new(detector_cfg_from_pipeline(&config))?,
            })
        } else {
            None
        };
        let cls = if config.global.use_cls {
            Some(match sources.cls {
                Some(source) => {
                    Classifier::new_with_source(classifier_cfg_from_pipeline(&config), source)?
                }
                None => Classifier::new(classifier_cfg_from_pipeline(&config))?,
            })
        } else {
            None
        };
        let rec = if config.global.use_rec {
            Some(match sources.rec {
                Some(source) => Recognizer::new_with_sources(
                    config.rec.clone(),
                    source,
                    sources.rec_dictionary,
                )?,
                None => Recognizer::new(config.rec.clone())?,
            })
        } else {
            None
        };
        Ok(Self {
            executor,
            config,
            detector: det,
            classifier: cls,
            recognizer: rec,
            loader: LoadImage,
        })
    }

    pub fn run(&mut self, input: OcrInput, opts: OcrCallOptions) -> Result<OcrOutput> {
        let executor = Arc::clone(&self.executor);
        executor.install(|| self.run_inner(input, opts))
    }

    fn run_inner(&mut self, input: OcrInput, opts: OcrCallOptions) -> Result<OcrOutput> {
        let e2e_start = Instant::now();
        let mut output = OcrOutput::default();
        let switches = self.resolve_run_switches(&opts);
        let mut prepared = self.prepare_image(input, switches.use_det)?;
        let mut buffers = RunBuffers::default();

        if !self.run_detection_stage(&opts, switches, &mut prepared, &mut buffers, &mut output)? {
            output.e2e_ms = Some(e2e_start.elapsed().as_secs_f32() * 1000.0);
            return Ok(output);
        }

        self.run_classification_stage(switches, &mut buffers, &mut output)?;
        self.run_recognition_stage(switches, &mut buffers, &mut output)?;
        self.finalize_detection_outputs(switches, &prepared, &mut buffers, &mut output)?;
        self.finalize_recognition_outputs(switches, &buffers.lines, &mut output);

        output.e2e_ms = Some(e2e_start.elapsed().as_secs_f32() * 1000.0);
        Ok(output)
    }

    fn resolve_run_switches(&self, opts: &OcrCallOptions) -> RunSwitches {
        let use_det = opts.use_det.unwrap_or(self.config.global.use_det);
        let use_cls = opts.use_cls.unwrap_or(self.config.global.use_cls);
        let use_rec = opts.use_rec.unwrap_or(self.config.global.use_rec);
        RunSwitches {
            use_det,
            use_cls,
            use_rec,
            need_stage_images: use_cls || use_rec,
            return_word_box: opts
                .return_word_box
                .unwrap_or(self.config.global.return_word_box),
            return_single_char_box: opts
                .return_single_char_box
                .unwrap_or(self.config.global.return_single_char_box),
            text_score: opts.text_score.unwrap_or(self.config.global.text_score),
        }
    }

    fn prepare_image(&mut self, input: OcrInput, use_det: bool) -> Result<PreparedImage> {
        let ori_img = self.loader.load(input)?;
        let ori_h = ori_img.height();
        let ori_w = ori_img.width();
        let preprocessing_backend = if use_det {
            self.config.det.runtime.vision_backend
        } else {
            self.config.rec.runtime.vision_backend
        };
        let (proc_img, ratio_h, ratio_w) = resize_image_within_bounds(
            ori_img,
            self.config.global.min_side_len,
            self.config.global.max_side_len,
            preprocessing_backend,
        )?;

        Ok(PreparedImage {
            ori_h,
            ori_w,
            ratio_h,
            ratio_w,
            preprocess_record: PreprocessRecord {
                ratio_h,
                ratio_w,
                ..PreprocessRecord::default()
            },
            proc_img,
        })
    }

    fn run_detection_stage(
        &mut self,
        opts: &OcrCallOptions,
        switches: RunSwitches,
        prepared: &mut PreparedImage,
        buffers: &mut RunBuffers,
        output: &mut OcrOutput,
    ) -> Result<bool> {
        if switches.use_det {
            let pad_top = apply_vertical_padding_in_place(
                &mut prepared.proc_img,
                self.config.global.width_height_ratio,
                self.config.global.min_height,
            )?;
            prepared.preprocess_record.pad_top = pad_top;

            let detector = self.detector.as_mut().ok_or_else(|| {
                RapidOcrError::Config(
                    "detection requested but detector is not initialized".to_string(),
                )
            })?;
            detector.update_postprocess(opts.box_thresh, opts.unclip_ratio);
            let det_out = detector.detect(&prepared.proc_img)?;
            if det_out.boxes.is_empty() {
                return Ok(false);
            }

            output.elapsed_ms[0] = Some(det_out.elapsed_ms);
            output.det_breakdown_ms = det_out.breakdown;
            buffers.det_boxes = det_out.boxes;
            buffers.det_scores = det_out.scores;

            if switches.need_stage_images {
                buffers.stage_images = crop_text_regions(
                    &prepared.proc_img,
                    &buffers.det_boxes,
                    self.config.det.runtime.vision_backend,
                )?;
            }
        } else if switches.need_stage_images {
            buffers.stage_images.push(prepared.proc_img.clone());
        }

        Ok(true)
    }

    fn run_classification_stage(
        &mut self,
        switches: RunSwitches,
        buffers: &mut RunBuffers,
        output: &mut OcrOutput,
    ) -> Result<()> {
        if switches.use_cls {
            let classifier = self.classifier.as_mut().ok_or_else(|| {
                RapidOcrError::Config(
                    "classification requested but classifier is not initialized".to_string(),
                )
            })?;
            let cls_result = classifier.classify_in_place(&mut buffers.stage_images)?;
            output.cls_res = Some(cls_result.cls_res);
            output.elapsed_ms[1] = Some(cls_result.elapsed_ms);
        }
        Ok(())
    }

    fn run_recognition_stage(
        &mut self,
        switches: RunSwitches,
        buffers: &mut RunBuffers,
        output: &mut OcrOutput,
    ) -> Result<()> {
        if !switches.use_rec {
            return Ok(());
        }

        let recognizer = self.recognizer.as_mut().ok_or_else(|| {
            RapidOcrError::Config(
                "recognition requested but recognizer is not initialized".to_string(),
            )
        })?;
        let rec = recognizer.recognize(
            &buffers.stage_images,
            RecognizeOptions {
                return_word_box: switches.return_word_box,
                return_single_char_box: switches.return_single_char_box,
            },
        )?;
        output.elapsed_ms[2] = Some(rec.elapsed.as_secs_f32() * 1000.0);
        buffers.lines = rec.lines;
        Ok(())
    }

    fn finalize_detection_outputs(
        &mut self,
        switches: RunSwitches,
        prepared: &PreparedImage,
        buffers: &mut RunBuffers,
        output: &mut OcrOutput,
    ) -> Result<()> {
        if !switches.use_det {
            return Ok(());
        }

        let mut mapped_boxes = std::mem::take(&mut buffers.det_boxes);
        map_boxes_to_original(
            &mut mapped_boxes,
            prepared.preprocess_record,
            prepared.ori_h,
            prepared.ori_w,
        );

        if !switches.use_rec {
            output.boxes = Some(mapped_boxes);
            output.det_scores = Some(std::mem::take(&mut buffers.det_scores));
            return Ok(());
        }

        let lines = std::mem::take(&mut buffers.lines);
        let det_scores = std::mem::take(&mut buffers.det_scores);
        let (filtered_boxes, filtered_scores, filtered_lines, kept_indices) =
            filter_empty_lines_boxes_and_scores(mapped_boxes, det_scores, lines);
        buffers.lines = filtered_lines;

        let mut computed_word_boxes = None;
        if switches.return_word_box && !filtered_boxes.is_empty() && !buffers.lines.is_empty() {
            let mapped_crops = map_img_to_original(
                &buffers.stage_images,
                prepared.ratio_h,
                prepared.ratio_w,
                self.config.det.runtime.vision_backend,
            )?;
            let filtered_crops = select_items_by_indices(mapped_crops, &kept_indices);
            let word_boxes = crate::rec::word_boxes::compute_word_boxes_with_backend(
                &filtered_crops,
                &filtered_boxes,
                &buffers.lines,
                switches.return_single_char_box,
                self.config.rec.runtime.vision_backend,
            )?;
            computed_word_boxes = Some(word_boxes);
        }

        let (
            score_filtered_boxes,
            score_filtered_scores,
            score_filtered_lines,
            score_filtered_words,
        ) = filter_by_text_score_for_full(
            filtered_boxes,
            filtered_scores,
            std::mem::take(&mut buffers.lines),
            computed_word_boxes,
            switches.text_score,
        );

        buffers.lines = score_filtered_lines;
        output.boxes = Some(score_filtered_boxes);
        output.det_scores = Some(score_filtered_scores);
        output.word_boxes = score_filtered_words;
        Ok(())
    }

    fn finalize_recognition_outputs(
        &self,
        switches: RunSwitches,
        lines: &[LineResult],
        output: &mut OcrOutput,
    ) {
        if !switches.use_rec || lines.is_empty() {
            return;
        }

        // Keep parity with Python: text_score only filters full outputs (det+rec),
        // rec-only mode returns raw recognition results.
        output.txts = Some(lines.iter().map(|v| v.text.clone()).collect());
        output.scores = Some(lines.iter().map(|v| v.score).collect());
        output.lines = Some(lines.to_vec());
    }

    pub fn provider_resolutions(&self) -> PipelineProviderResolutions {
        PipelineProviderResolutions {
            det: self.detector.as_ref().map(Detector::provider_resolution),
            cls: self
                .classifier
                .as_ref()
                .map(Classifier::provider_resolution),
            rec: self
                .recognizer
                .as_ref()
                .map(Recognizer::provider_resolution),
        }
    }
}

fn build_executor(config: &EngineConfig) -> Result<Arc<rayon::ThreadPool>> {
    let threads = resolve_rayon_threads(config).unwrap_or(1).max(1);
    rayon::ThreadPoolBuilder::new()
        .num_threads(threads)
        .thread_name(|index| format!("rapid-ocr-{index}"))
        .build()
        .map(Arc::new)
        .map_err(|error| RapidOcrError::Config(format!("unable to create OCR executor: {error}")))
}

fn resolve_rayon_threads(config: &EngineConfig) -> Option<usize> {
    let runtimes = [
        &config.det.runtime,
        &config.cls.runtime,
        &config.rec.runtime,
    ];
    let explicit = runtimes
        .iter()
        .filter_map(|rt| rt.rayon_threads.filter(|v| *v > 0))
        .max();
    if explicit.is_some() {
        return explicit;
    }
    let budget = runtimes
        .iter()
        .filter_map(|rt| rt.thread_budget.filter(|v| *v > 0))
        .max();
    if budget.is_some() {
        return budget;
    }
    if runtimes.iter().any(|rt| rt.auto_tune_threads) {
        return available_parallelism();
    }
    None
}

fn available_parallelism() -> Option<usize> {
    let physical_cores = num_cpus::get_physical().max(1);
    thread::available_parallelism()
        .ok()
        .map(|v| v.get().clamp(1, physical_cores))
}

type FullFilterOutput = (
    Vec<crate::Quad>,
    Vec<f32>,
    Vec<LineResult>,
    Option<Vec<Vec<WordBox>>>,
);

fn filter_empty_lines_boxes_and_scores(
    boxes: Vec<crate::Quad>,
    det_scores: Vec<f32>,
    lines: Vec<LineResult>,
) -> (Vec<crate::Quad>, Vec<f32>, Vec<LineResult>, Vec<usize>) {
    if boxes.len() != lines.len() || det_scores.len() != lines.len() {
        return (boxes, det_scores, lines, Vec::new());
    }
    let mut out_boxes = Vec::new();
    let mut out_scores = Vec::new();
    let mut out_lines = Vec::new();
    let mut kept_indices = Vec::new();
    for (idx, ((b, s), l)) in boxes.into_iter().zip(det_scores).zip(lines).enumerate() {
        if l.text.trim().is_empty() {
            continue;
        }
        out_boxes.push(b);
        out_scores.push(s);
        out_lines.push(l);
        kept_indices.push(idx);
    }
    (out_boxes, out_scores, out_lines, kept_indices)
}

fn select_items_by_indices<T>(items: Vec<T>, indices: &[usize]) -> Vec<T> {
    if indices.is_empty() {
        return Vec::new();
    }

    let mut out = Vec::with_capacity(indices.len());
    let mut keep_iter = indices.iter().copied().peekable();
    for (idx, item) in items.into_iter().enumerate() {
        let Some(target_idx) = keep_iter.peek().copied() else {
            break;
        };
        if idx == target_idx {
            out.push(item);
            keep_iter.next();
        }
    }
    out
}

fn filter_by_text_score_for_full(
    boxes: Vec<crate::Quad>,
    det_scores: Vec<f32>,
    lines: Vec<LineResult>,
    word_boxes: Option<Vec<Vec<WordBox>>>,
    text_score: f32,
) -> FullFilterOutput {
    let mut out_boxes = Vec::new();
    let mut out_scores = Vec::new();
    let mut out_lines = Vec::new();
    let mut out_word_boxes = Vec::new();

    for (idx, line) in lines.into_iter().enumerate() {
        if idx >= boxes.len() || idx >= det_scores.len() {
            break;
        }
        if line.score < text_score {
            continue;
        }
        out_boxes.push(boxes[idx]);
        out_scores.push(det_scores[idx]);
        if let Some(word_line) = word_boxes.as_ref().and_then(|v| v.get(idx))
            && !word_line.is_empty()
        {
            out_word_boxes.push(word_line.clone());
        }
        out_lines.push(line);
    }

    let out_word_boxes = if word_boxes.is_some() {
        Some(out_word_boxes)
    } else {
        None
    };

    (out_boxes, out_scores, out_lines, out_word_boxes)
}

fn detector_cfg_from_pipeline(config: &EngineConfig) -> DetectorConfig {
    config.det.clone()
}

fn classifier_cfg_from_pipeline(config: &EngineConfig) -> ClassifierConfig {
    config.cls.clone()
}

#[derive(Debug)]
pub struct RapidOcrEngine {
    inner: RapidOcr,
}

impl RapidOcrEngine {
    pub fn new(config: EngineConfig) -> Result<Self> {
        Ok(Self {
            inner: RapidOcr::new(config)?,
        })
    }

    pub fn new_with_sources(config: EngineConfig, sources: PipelineSources<'_>) -> Result<Self> {
        Ok(Self {
            inner: RapidOcr::new_with_sources(config, sources)?,
        })
    }

    pub fn run(&mut self, input: OcrInput, options: RunOptions) -> Result<OcrResult> {
        let out = self.inner.run(input, options)?;
        OcrResult::try_from(out)
    }

    pub fn provider_resolutions(&self) -> PipelineProviderResolutions {
        self.inner.provider_resolutions()
    }
}

#[cfg(test)]
mod tests {
    use crate::{
        pipeline::rapid_ocr::{filter_by_text_score_for_full, filter_empty_lines_boxes_and_scores},
        types::{LineResult, WordBox},
    };

    #[test]
    fn filter_empty_lines_keeps_boxes_scores_lines_aligned() {
        let boxes = vec![
            [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
            [[2.0, 2.0], [3.0, 2.0], [3.0, 3.0], [2.0, 3.0]],
        ];
        let det_scores = vec![0.9, 0.8];
        let lines = vec![
            LineResult {
                text: "ok".to_string(),
                score: 0.9,
                word_info: None,
            },
            LineResult {
                text: "   ".to_string(),
                score: 0.8,
                word_info: None,
            },
        ];

        let (out_boxes, out_scores, out_lines, kept_indices) =
            filter_empty_lines_boxes_and_scores(boxes, det_scores, lines);

        assert_eq!(out_boxes.len(), 1);
        assert_eq!(out_scores.len(), 1);
        assert_eq!(out_lines.len(), 1);
        assert_eq!(kept_indices, vec![0]);
        assert_eq!(out_lines[0].text, "ok");
    }

    #[test]
    fn filter_by_text_score_keeps_full_outputs_in_sync() {
        let boxes = vec![
            [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]],
            [[2.0, 2.0], [3.0, 2.0], [3.0, 3.0], [2.0, 3.0]],
            [[4.0, 4.0], [5.0, 4.0], [5.0, 5.0], [4.0, 5.0]],
        ];
        let det_scores = vec![0.1, 0.2, 0.3];
        let lines = vec![
            LineResult {
                text: "a".to_string(),
                score: 0.95,
                word_info: None,
            },
            LineResult {
                text: "b".to_string(),
                score: 0.50,
                word_info: None,
            },
            LineResult {
                text: "c".to_string(),
                score: 0.99,
                word_info: None,
            },
        ];
        let words = Some(vec![
            vec![WordBox {
                text: "a".to_string(),
                score: 0.95,
                bbox: boxes[0],
            }],
            vec![],
            vec![WordBox {
                text: "c".to_string(),
                score: 0.99,
                bbox: boxes[2],
            }],
        ]);

        let (out_boxes, out_scores, out_lines, out_words) =
            filter_by_text_score_for_full(boxes, det_scores, lines, words, 0.9);

        assert_eq!(out_boxes.len(), 2);
        assert_eq!(out_scores.len(), 2);
        assert_eq!(out_lines.len(), 2);
        assert_eq!(out_lines[0].text, "a");
        assert_eq!(out_lines[1].text, "c");

        let out_words = out_words.expect("word boxes should exist");
        assert_eq!(out_words.len(), 2);
        assert_eq!(out_words[0][0].text, "a");
        assert_eq!(out_words[1][0].text, "c");
    }
}
