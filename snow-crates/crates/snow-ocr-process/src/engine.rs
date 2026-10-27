use super::{Config, DetectorResizePolicy, TextDetectionProcessing, acceleration::Context};
use simd_paddle_ocr_rs::{
    cpu::{CpuContext, CpuOptions},
    detector::{DetResizePolicy, DetectorOptions, DetectorPlan},
    error::ErrorKind,
    image::{ImageError, ImageResult, ImageView, PixelFormat},
    lines::{LineOptions, OrientedLinePlan},
    model::Model,
    ocr::{OcrOptions, OcrPlan, OcrRequest, RunOptions},
    recognizer::{Dictionary, RecognitionScoreMode, RecognizerOptions, RecognizerPlan},
    runtime::{
        CompileOptions, CompiledModel, Compiler, CompilerOptions, ExecutionLimits, ExecutionTarget,
    },
};
use snow_memory::RasterBuffer;
use std::{path::Path, sync::Arc, thread};

pub(super) struct InputImage {
    pub width: usize,
    pub height: usize,
    pub pixels: RasterBuffer,
}

impl InputImage {
    pub fn view(&self) -> ImageResult<ImageView<'_>> {
        ImageView::new(
            self.pixels.as_slice(),
            self.width,
            self.height,
            0,
            PixelFormat::Bgr24,
        )
    }
}

#[derive(Debug, PartialEq)]
pub(super) struct OcrLine {
    pub text: String,
    pub score: f32,
    pub points: [[f32; 2]; 4],
}

fn accepted_line(text: &str, score: f32, points: [[f32; 2]; 4]) -> Option<OcrLine> {
    (!text.trim().is_empty() && score.is_finite() && score >= 0.5).then(|| OcrLine {
        text: text.to_owned(),
        score,
        points,
    })
}

fn source_quad(mut points: [[f32; 2]; 4], width: usize, height: usize) -> [[f32; 2]; 4] {
    // The detector's enclosing rotated rectangle can extend beyond a clipped
    // contour. Preserve Snow Shot's source-image bounds at the IPC boundary.
    for point in &mut points {
        point[0] = point[0].clamp(0., width as f32);
        point[1] = point[1].clamp(0., height as f32);
    }
    points
}

fn detector_limit(policy: DetectorResizePolicy, width: usize, height: usize) -> usize {
    match policy {
        DetectorResizePolicy::Min => 736,
        DetectorResizePolicy::Max if width.max(height) < 960 => 960,
        DetectorResizePolicy::Max if width.max(height) < 1500 => 1500,
        DetectorResizePolicy::Max => 2000,
    }
}

pub(super) struct Engine {
    request: OcrRequest,
    policy: DetectorResizePolicy,
}

impl Engine {
    pub fn recreate(self) -> ImageResult<Self> {
        let plan = self.request.plan().clone();
        // Drop all mutable storage before creating its replacement.
        drop(self.request);
        Ok(Self {
            request: plan.request()?,
            policy: self.policy,
        })
    }

    pub fn uses_gpu(&self) -> bool {
        self.request.uses_gpu()
    }

    pub fn backend(&self) -> &'static str {
        if self.uses_gpu() { "gpu" } else { "cpu" }
    }

    pub fn run(&mut self, input: &InputImage) -> ImageResult<Vec<OcrLine>> {
        self.request.set_detector_resize(
            match self.policy {
                DetectorResizePolicy::Max => DetResizePolicy::Max,
                DetectorResizePolicy::Min => DetResizePolicy::Min,
            },
            detector_limit(self.policy, input.width, input.height),
        )?;
        // The request owns reusable scratch. Copy only the accepted output before
        // it crosses the completion channel; never copy the input raster again.
        Ok(self
            .request
            .run(input.view()?, RunOptions::default())?
            .iter()
            .filter_map(|line| {
                accepted_line(
                    line.text,
                    line.recognition_score,
                    source_quad(line.detection.points, input.width, input.height),
                )
            })
            .collect())
    }
}

pub(super) fn make_engine(
    config: &Config,
    thread_budget: usize,
    gpu: Option<Arc<Context>>,
) -> ImageResult<Engine> {
    make_engine_for_models(
        &config.detector_model,
        &config.recognizer_model,
        &config.dictionary,
        thread_budget,
        config.detector_resize_policy,
        config.text_detection_processing,
        gpu,
    )
}

pub(super) fn make_engine_for_models(
    detector: &Path,
    recognizer: &Path,
    dictionary: &Path,
    thread_budget: usize,
    policy: DetectorResizePolicy,
    processing: TextDetectionProcessing,
    gpu: Option<Arc<Context>>,
) -> ImageResult<Engine> {
    let (detection, recognition) = prepare_pair(
        thread_budget > 1,
        || load_compiled(detector, "detection", gpu.clone()),
        || load_compiled(recognizer, "recognition", gpu.clone()),
    )?;
    let bytes = std::fs::read(dictionary)
        .map_err(|error| ImageError::caused_by(ErrorKind::Io, "dictionary", error))?;
    let dictionary = Arc::new(Dictionary::from_utf8(&bytes)?);
    let cpu = Arc::new(CpuContext::new(thread_budget.max(1))?);
    let detector = DetectorPlan::new(
        detection,
        DetectorOptions::default(),
        ExecutionLimits::default(),
    )?;
    let extractor = OrientedLinePlan::new(detector, None, LineOptions::default())?;
    let recognizer = RecognizerPlan::new(
        recognition,
        dictionary,
        RecognizerOptions {
            min_width: processing.recognition_width(),
            score_mode: RecognitionScoreMode::Probability,
            ..Default::default()
        },
        ExecutionLimits::default(),
    )?;
    let plan = OcrPlan::new(
        extractor,
        recognizer,
        OcrOptions::default(),
        cpu,
        CpuOptions::default(),
        1,
    )?;
    Ok(Engine {
        request: plan.request()?,
        policy,
    })
}

fn load_compiled(
    path: &Path,
    stage: &str,
    gpu: Option<Arc<Context>>,
) -> ImageResult<CompiledModel> {
    let model = Model::load_path(path)
        .map_err(|error| ImageError::caused_by(error.kind(), format!("{stage} model"), error))?;
    let target = if gpu.is_some() {
        ExecutionTarget::Gpu
    } else {
        ExecutionTarget::Cpu
    };
    #[cfg(feature = "gpu")]
    let options = CompilerOptions {
        gpu,
        ..Default::default()
    };
    #[cfg(not(feature = "gpu"))]
    let options = CompilerOptions::default();
    Compiler::new(options)
        .compile(Arc::new(model), CompileOptions::default(), target)
        .map_err(|error| ImageError::caused_by(error.kind(), format!("{stage} compilation"), error))
}

fn prepare_pair<A: Send, B>(
    parallel: bool,
    first: impl FnOnce() -> ImageResult<A> + Send,
    second: impl FnOnce() -> ImageResult<B>,
) -> ImageResult<(A, B)> {
    if !parallel {
        return Ok((first()?, second()?));
    }
    // One extra loader overlaps both parsing and compilation. Join even on
    // failure: no model allocation or background task escapes preparation.
    thread::scope(|scope| {
        let worker = thread::Builder::new()
            .name("snow-ocr-model".into())
            .spawn_scoped(scope, first)
            .map_err(|error| ImageError::caused_by(ErrorKind::Io, "model loader thread", error))?;
        let second = second();
        let first = worker.join().map_err(|_| {
            ImageError::with_kind(ErrorKind::Execution, "model loader thread panicked")
        })?;
        Ok((first?, second?))
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn model_preparation_overlaps_tasks_and_preserves_result_order() {
        let barrier = std::sync::Barrier::new(2);
        assert_eq!(
            prepare_pair(
                true,
                || {
                    barrier.wait();
                    Ok(1)
                },
                || {
                    barrier.wait();
                    Ok(2)
                }
            )
            .unwrap(),
            (1, 2)
        );
        assert_eq!(prepare_pair(false, || Ok(1), || Ok(2)).unwrap(), (1, 2));
    }

    #[test]
    fn model_preparation_joins_and_drops_successful_peer_on_failure() {
        struct ModelResource(Arc<std::sync::atomic::AtomicBool>);
        impl Drop for ModelResource {
            fn drop(&mut self) {
                self.0.store(true, std::sync::atomic::Ordering::Release);
            }
        }
        for fail_first in [false, true] {
            let dropped = Arc::new(std::sync::atomic::AtomicBool::new(false));
            let load = |fail| {
                if fail {
                    Err(ImageError::new("invalid model"))
                } else {
                    Ok(ModelResource(dropped.clone()))
                }
            };
            assert!(prepare_pair(true, || load(fail_first), || load(!fail_first)).is_err());
            assert!(dropped.load(std::sync::atomic::Ordering::Acquire));
        }
    }

    #[test]
    fn result_filter_keeps_geometry_and_owned_utf8_text() {
        let points = [[1., 2.], [3., 2.], [3., 4.], [1., 4.]];
        let mut text = String::from("文字 Snow");
        let line = accepted_line(&text, 0.5, points).unwrap();
        text.clear();
        assert_eq!(line.text, "文字 Snow");
        assert_eq!(line.points, points);
        assert!(accepted_line(" ", 1., points).is_none());
        assert!(accepted_line("text", 0.499, points).is_none());
        assert!(accepted_line("text", f32::NAN, points).is_none());
    }

    #[test]
    fn enclosing_quad_is_clipped_in_original_image_coordinates() {
        let quad = [[5., 4.], [133.72, 13.05], [130., 66.], [-1.28, 73.]];
        assert_eq!(
            source_quad(quad, 130, 70),
            [[5., 4.], [130., 13.05], [130., 66.], [0., 70.]]
        );
    }

    #[test]
    fn resize_limits_preserve_policy_boundaries() {
        for (width, expected) in [
            (959, 960),
            (960, 1500),
            (1499, 1500),
            (1500, 2000),
            (3840, 2000),
        ] {
            assert_eq!(
                detector_limit(DetectorResizePolicy::Max, width, 100),
                expected
            );
            assert_eq!(detector_limit(DetectorResizePolicy::Min, width, 100), 736);
        }
        assert_eq!(TextDetectionProcessing::default().recognition_width(), 320);
        assert_eq!(TextDetectionProcessing::SpeedFirst.recognition_width(), 48);
    }
}
