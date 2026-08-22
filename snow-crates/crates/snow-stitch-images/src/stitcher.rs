use std::path::{Path, PathBuf};

use crate::compositor::{
    synthesize_append_axis, synthesize_append_in_place, synthesize_prepend_axis,
    synthesize_prepend_in_place,
};
use crate::{
    Frame, MotionEstimate, MotionOutcome, ReferenceMode, StitchAxis, StitchBranch, StitchError,
    StitchOptions, StitchTraceEvent, TiledCanvas, TraceState, VerticalMotionEstimator,
    ViewportState, band_height, estimator::validate_estimator_options, synthesize_append,
    synthesize_prepend,
};

#[derive(Debug, Clone, PartialEq)]
pub struct StitchResult {
    pub image: Frame,
    pub trace: Vec<StitchTraceEvent>,
}

pub struct Stitcher {
    options: StitchOptions,
    accumulator: Option<StitchAccumulator>,
    estimator: Option<VerticalMotionEstimator>,
    next_index: usize,
}

impl std::fmt::Debug for Stitcher {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("Stitcher")
            .field("options", &self.options)
            .field("initialized", &self.accumulator.is_some())
            .field("next_index", &self.next_index)
            .finish()
    }
}

impl Stitcher {
    pub fn new(options: StitchOptions) -> Result<Self, StitchError> {
        validate_options(options)?;
        Ok(Self {
            options,
            accumulator: None,
            estimator: None,
            next_index: 0,
        })
    }

    pub fn push(&mut self, frame: Frame) -> Result<Option<StitchTraceEvent>, StitchError> {
        if self.accumulator.is_none() {
            validate_first(&frame, self.options)?;
            self.estimator = Some(VerticalMotionEstimator::new_for_axis(
                frame.geometry(),
                self.options.axis,
                self.options.estimator,
            )?);
            self.accumulator = Some(StitchAccumulator::new(frame, self.options)?);
            self.next_index = 1;
            return Ok(None);
        }

        let accumulator = self
            .accumulator
            .as_mut()
            .expect("initialized stitcher has an accumulator");
        let estimator = self
            .estimator
            .as_mut()
            .expect("initialized stitcher has an estimator");
        let trace_start = accumulator.trace.len();
        accumulator.process(
            self.next_index,
            frame,
            &mut |reference, previous_raw, incoming| {
                estimator.estimate(reference, previous_raw, incoming)
            },
        )?;
        self.next_index = self
            .next_index
            .checked_add(1)
            .ok_or(StitchError::Arithmetic {
                operation: "incrementing input-frame index",
            })?;
        Ok(accumulator.trace.get(trace_start).cloned())
    }

    pub fn image(&self) -> Option<Frame> {
        self.accumulator
            .as_ref()
            .and_then(|accumulator| accumulator.canvas.materialize().ok())
    }

    pub fn image_dimensions(&self) -> Option<(u32, u32)> {
        self.accumulator
            .as_ref()
            .map(|accumulator| (accumulator.canvas.width(), accumulator.canvas.height()))
    }

    pub fn materialize_rows(&self, top: u32, bottom: u32) -> Result<Frame, StitchError> {
        if self.options.axis != StitchAxis::Vertical {
            return Err(StitchError::InvalidOptions {
                message: "row materialization requires vertical stitching".to_owned(),
            });
        }
        self.materialize_axis(top, bottom)
    }

    pub fn materialize_axis(&self, start: u32, end: u32) -> Result<Frame, StitchError> {
        self.accumulator
            .as_ref()
            .ok_or(StitchError::EmptyInput)?
            .canvas
            .materialize_axis(start, end)
    }

    pub fn copy_rows(
        &self,
        top: u32,
        rows: u32,
        destination: &mut [u8],
    ) -> Result<(), StitchError> {
        self.accumulator
            .as_ref()
            .ok_or(StitchError::EmptyInput)?
            .canvas
            .copy_rows(top, rows, destination)
    }

    pub fn render_scaled_rows(
        &self,
        top: u32,
        rows: u32,
        width: u32,
        height: u32,
    ) -> Result<Frame, StitchError> {
        if self.options.axis != StitchAxis::Vertical {
            return Err(StitchError::InvalidOptions {
                message: "scaled row rendering requires vertical stitching".to_owned(),
            });
        }
        self.render_scaled_axis(top, rows, width, height)
    }

    pub fn render_scaled_axis(
        &self,
        start: u32,
        span: u32,
        width: u32,
        height: u32,
    ) -> Result<Frame, StitchError> {
        let end = start.checked_add(span).ok_or(StitchError::Arithmetic {
            operation: "calculating scaled axis range",
        })?;
        self.accumulator
            .as_ref()
            .ok_or(StitchError::EmptyInput)?
            .canvas
            .snapshot_axis(start, end)?
            .render_scaled(width, height)
    }

    pub fn snapshot_rows(
        &self,
        top: u32,
        bottom: u32,
    ) -> Result<crate::TiledCanvasSnapshot, StitchError> {
        if self.options.axis != StitchAxis::Vertical {
            return Err(StitchError::InvalidOptions {
                message: "row snapshot requires vertical stitching".to_owned(),
            });
        }
        self.snapshot_axis(top, bottom)
    }

    pub fn snapshot_axis(
        &self,
        start: u32,
        end: u32,
    ) -> Result<crate::TiledCanvasSnapshot, StitchError> {
        self.accumulator
            .as_ref()
            .ok_or(StitchError::EmptyInput)?
            .canvas
            .snapshot_axis(start, end)
    }

    pub fn trace(&self) -> &[StitchTraceEvent] {
        self.accumulator
            .as_ref()
            .map_or(&[], |accumulator| accumulator.trace.as_slice())
    }

    pub fn clear_trace(&mut self) {
        if let Some(accumulator) = self.accumulator.as_mut() {
            accumulator.trace.clear();
        }
    }

    pub fn input_count(&self) -> usize {
        self.next_index
    }

    pub fn reset(&mut self) {
        self.accumulator = None;
        self.estimator = None;
        self.next_index = 0;
    }

    pub fn finish(self) -> Result<StitchResult, StitchError> {
        self.accumulator
            .map(StitchAccumulator::finish)
            .ok_or(StitchError::EmptyInput)
    }
}

struct StitchAccumulator {
    viewport_extent: u32,
    expected_geometry: crate::Geometry,
    canvas: TiledCanvas,
    previous_raw: Frame,
    previous_raw_index: usize,
    synthetic_reference: Frame,
    reference_mode: ReferenceMode,
    state: ViewportState,
    processed_count: usize,
    accepted_count: usize,
    trace: Vec<StitchTraceEvent>,
    options: StitchOptions,
}

fn validate_options(options: StitchOptions) -> Result<(), StitchError> {
    validate_estimator_options(options.estimator)
}

fn validate_first(frame: &Frame, options: StitchOptions) -> Result<(), StitchError> {
    validate_options(options)?;
    if frame.width() <= 4 || frame.height() <= 4 {
        return Err(StitchError::InvalidFirstGeometry {
            width: frame.width(),
            height: frame.height(),
        });
    }
    Ok(())
}

impl StitchAccumulator {
    fn new(first: Frame, options: StitchOptions) -> Result<Self, StitchError> {
        validate_first(&first, options)?;
        Ok(Self {
            viewport_extent: options.axis.primary_extent(first.width(), first.height()),
            expected_geometry: first.geometry(),
            canvas: TiledCanvas::new_for_axis(first.clone(), options.axis)?,
            previous_raw: first.clone(),
            previous_raw_index: 0,
            synthetic_reference: first,
            reference_mode: ReferenceMode::Synthetic,
            state: ViewportState::default(),
            processed_count: 0,
            accepted_count: 0,
            trace: Vec::new(),
            options,
        })
    }

    fn snapshot(&self) -> Result<TraceState, StitchError> {
        Ok(TraceState {
            viewport_position: self.state.position,
            max_viewport_position: self.state.max_position,
            canvas_height: self.state.canvas_height(self.viewport_extent)?,
            processed_count: self.processed_count,
            accepted_count: self.accepted_count,
        })
    }

    fn validate_incoming(&self, index: usize, incoming: &Frame) -> Result<(), StitchError> {
        let actual = incoming.geometry();
        if actual != self.expected_geometry {
            return Err(StitchError::ViewportMismatch {
                index,
                expected: self.expected_geometry,
                actual,
            });
        }
        Ok(())
    }

    fn comparison_reference(&self) -> Result<Frame, StitchError> {
        match self.reference_mode {
            ReferenceMode::Synthetic => Ok(self.synthetic_reference.clone()),
            ReferenceMode::CanvasWindow => {
                let start =
                    u32::try_from(self.state.position).map_err(|_| StitchError::Arithmetic {
                        operation: "converting canvas-window position",
                    })?;
                let end =
                    start
                        .checked_add(self.viewport_extent)
                        .ok_or(StitchError::Arithmetic {
                            operation: "calculating canvas-window end",
                        })?;
                self.canvas.materialize_axis(start, end)
            }
        }
    }

    fn process(
        &mut self,
        index: usize,
        incoming: Frame,
        estimator: &mut impl FnMut(&Frame, &Frame, &Frame) -> Result<MotionEstimate, StitchError>,
    ) -> Result<(), StitchError> {
        self.validate_incoming(index, &incoming)?;
        let before = self.snapshot()?;
        let previous_raw_index = self.previous_raw_index;
        let comparison_mode = self.reference_mode;
        if self.previous_raw.visible_pixels_equal(&incoming) {
            if self.options.collect_trace {
                self.trace.push(StitchTraceEvent {
                    input_index: index,
                    previous_raw_index,
                    exact_duplicate: true,
                    reference_mode: comparison_mode,
                    motion: None,
                    confidence: None,
                    accepted_offset: None,
                    branch: StitchBranch::Skip,
                    before,
                    after: before,
                    growth: 0,
                    canvas_band_height: None,
                    synthetic_reference_band_height: None,
                    motion_diagnostics: None,
                });
            }
            return Ok(());
        }

        self.processed_count =
            self.processed_count
                .checked_add(1)
                .ok_or(StitchError::Arithmetic {
                    operation: "incrementing processed-frame count",
                })?;
        let canvas_window_reference = (comparison_mode == ReferenceMode::CanvasWindow)
            .then(|| self.comparison_reference())
            .transpose()?;
        let reference = canvas_window_reference
            .as_ref()
            .unwrap_or(&self.synthetic_reference);
        let estimate = estimator(reference, &self.previous_raw, &incoming)?;

        let maximum_shift = self.viewport_extent as f32 * self.options.estimator.max_motion_ratio;
        let accepted_offset = match estimate.outcome {
            MotionOutcome::Motion { offset }
                if offset != 0 && offset.unsigned_abs() as f32 <= maximum_shift =>
            {
                Some(offset)
            }
            MotionOutcome::Motion { .. }
            | MotionOutcome::NoMotion
            | MotionOutcome::Indeterminate => None,
        };
        let Some(offset) = accepted_offset else {
            self.previous_raw = incoming;
            self.previous_raw_index = index;
            let after = self.snapshot()?;
            if self.options.collect_trace {
                self.trace.push(StitchTraceEvent {
                    input_index: index,
                    previous_raw_index,
                    exact_duplicate: false,
                    reference_mode: comparison_mode,
                    motion: Some(estimate.outcome),
                    confidence: Some(estimate.confidence),
                    accepted_offset: None,
                    branch: StitchBranch::NoMovement,
                    before,
                    after,
                    growth: 0,
                    canvas_band_height: None,
                    synthetic_reference_band_height: None,
                    motion_diagnostics: Some(estimate.diagnostics),
                });
            }
            return Ok(());
        };

        self.accepted_count =
            self.accepted_count
                .checked_add(1)
                .ok_or(StitchError::Arithmetic {
                    operation: "incrementing accepted-motion count",
                })?;
        self.previous_raw = incoming;
        self.previous_raw_index = index;
        let transition = self.state.transition(offset)?;
        let shift = offset.unsigned_abs();
        let mut canvas_band = None;
        let mut synthetic_band = None;
        match transition.branch {
            StitchBranch::Append => {
                canvas_band = Some(band_height(self.viewport_extent, transition.growth)?);
                synthetic_band = Some(band_height(self.viewport_extent, shift)?);
                let band = band_height(self.viewport_extent, transition.growth)?;
                let overlap = band - transition.growth;
                let old_end = self.canvas.extent() - overlap;
                self.canvas.truncate_end(old_end)?;
                self.canvas.append_axis(
                    &self.previous_raw,
                    self.viewport_extent - band,
                    self.viewport_extent,
                )?;
                if self.options.axis == StitchAxis::Vertical
                    && comparison_mode == ReferenceMode::Synthetic
                {
                    synthesize_append_in_place(
                        &mut self.synthetic_reference,
                        &self.previous_raw,
                        shift,
                    )?;
                } else if self.options.axis == StitchAxis::Vertical {
                    self.synthetic_reference =
                        synthesize_append(reference, &self.previous_raw, shift)?;
                } else {
                    self.synthetic_reference = synthesize_append_axis(
                        reference,
                        &self.previous_raw,
                        self.options.axis,
                        shift,
                    )?;
                }
                self.reference_mode = ReferenceMode::Synthetic;
            }
            StitchBranch::Prepend => {
                canvas_band = Some(band_height(self.viewport_extent, transition.growth)?);
                synthetic_band = Some(band_height(self.viewport_extent, shift)?);
                let band = band_height(self.viewport_extent, transition.growth)?;
                let old_start = band - transition.growth;
                self.canvas.truncate_start(old_start)?;
                self.canvas.prepend_axis(&self.previous_raw, 0, band)?;
                if self.options.axis == StitchAxis::Vertical
                    && comparison_mode == ReferenceMode::Synthetic
                {
                    synthesize_prepend_in_place(
                        &mut self.synthetic_reference,
                        &self.previous_raw,
                        shift,
                    )?;
                } else if self.options.axis == StitchAxis::Vertical {
                    self.synthetic_reference =
                        synthesize_prepend(reference, &self.previous_raw, shift)?;
                } else {
                    self.synthetic_reference = synthesize_prepend_axis(
                        reference,
                        &self.previous_raw,
                        self.options.axis,
                        shift,
                    )?;
                }
                self.reference_mode = ReferenceMode::Synthetic;
            }
            StitchBranch::Contained => {
                self.reference_mode = ReferenceMode::CanvasWindow;
            }
            StitchBranch::Skip | StitchBranch::NoMovement => unreachable!("tracker branch"),
        }
        self.state = transition.next;
        self.assert_invariants()?;
        let after = self.snapshot()?;
        if self.options.collect_trace {
            self.trace.push(StitchTraceEvent {
                input_index: index,
                previous_raw_index,
                exact_duplicate: false,
                reference_mode: comparison_mode,
                motion: Some(estimate.outcome),
                confidence: Some(estimate.confidence),
                accepted_offset: Some(offset),
                branch: transition.branch,
                before,
                after,
                growth: transition.growth,
                canvas_band_height: canvas_band,
                synthetic_reference_band_height: synthetic_band,
                motion_diagnostics: Some(estimate.diagnostics),
            });
        }
        Ok(())
    }

    fn assert_invariants(&self) -> Result<(), StitchError> {
        let expected_extent = self.state.canvas_height(self.viewport_extent)?;
        let actual_extent = self
            .options
            .axis
            .primary_extent(self.canvas.width(), self.canvas.height());
        let expected_cross = match self.options.axis {
            StitchAxis::Vertical => self.expected_geometry.width,
            StitchAxis::Horizontal => self.expected_geometry.height,
        };
        let actual_cross = match self.options.axis {
            StitchAxis::Vertical => self.canvas.width(),
            StitchAxis::Horizontal => self.canvas.height(),
        };
        if actual_cross != expected_cross
            || actual_extent != expected_extent
            || self.state.position < 0
            || self.state.position > self.state.max_position
            || self.synthetic_reference.geometry() != self.expected_geometry
        {
            return Err(StitchError::InvalidFrame {
                message: "internal canvas, reference, or viewport invariant failed".to_owned(),
            });
        }
        Ok(())
    }

    fn finish(self) -> StitchResult {
        StitchResult {
            image: self
                .canvas
                .materialize()
                .expect("valid tiled canvas materializes"),
            trace: self.trace,
        }
    }
}

fn stitch_owned_with_estimator<I, F>(
    mut frames: I,
    options: StitchOptions,
    mut estimator: F,
) -> Result<StitchResult, StitchError>
where
    I: Iterator<Item = Result<Frame, StitchError>>,
    F: FnMut(&Frame, &Frame, &Frame) -> Result<MotionEstimate, StitchError>,
{
    let first = frames.next().ok_or(StitchError::EmptyInput)??;
    let mut accumulator = StitchAccumulator::new(first, options)?;
    for (index, incoming) in frames.enumerate() {
        accumulator.process(index + 1, incoming?, &mut estimator)?;
    }
    Ok(accumulator.finish())
}

fn stitch_owned<I>(mut frames: I, options: StitchOptions) -> Result<StitchResult, StitchError>
where
    I: Iterator<Item = Result<Frame, StitchError>>,
{
    let first = frames.next().ok_or(StitchError::EmptyInput)??;
    validate_first(&first, options)?;
    let mut estimator =
        VerticalMotionEstimator::new_for_axis(first.geometry(), options.axis, options.estimator)?;
    let all_frames = std::iter::once(Ok(first)).chain(frames);
    stitch_owned_with_estimator(all_frames, options, |reference, previous_raw, incoming| {
        estimator.estimate(reference, previous_raw, incoming)
    })
}

pub fn stitch(frames: &[Frame], options: StitchOptions) -> Result<StitchResult, StitchError> {
    if frames.is_empty() {
        return Err(StitchError::EmptyInput);
    }
    validate_first(&frames[0], options)?;
    let expected = frames[0].geometry();
    for (index, frame) in frames.iter().enumerate().skip(1) {
        if frame.geometry() != expected {
            return Err(StitchError::ViewportMismatch {
                index,
                expected,
                actual: frame.geometry(),
            });
        }
    }
    stitch_owned(frames.iter().cloned().map(Ok), options)
}

pub fn stitch_files(
    paths: &[PathBuf],
    options: StitchOptions,
) -> Result<StitchResult, StitchError> {
    let decoded = paths.iter().map(|path| Frame::decode(Path::new(path)));
    stitch_owned(decoded, options)
}

pub fn stitch_iter<I>(frames: I, options: StitchOptions) -> Result<StitchResult, StitchError>
where
    I: IntoIterator<Item = Result<Frame, StitchError>>,
{
    stitch_owned(frames.into_iter(), options)
}

#[cfg(test)]
mod tests {
    use std::{cell::Cell, collections::VecDeque};

    use super::*;
    use crate::{MotionDiagnostics, MotionStage, PixelFormat};

    fn solid(value: u8, width: u32, height: u32) -> Frame {
        Frame::new(
            width,
            height,
            PixelFormat::Gray8,
            vec![value; (width * height) as usize],
        )
        .unwrap()
    }

    fn estimate(outcome: MotionOutcome) -> MotionEstimate {
        MotionEstimate {
            outcome,
            confidence: 1.0,
            diagnostics: MotionDiagnostics::at(MotionStage::Selected),
        }
    }

    fn run_outcomes(frames: &[Frame], outcomes: &[MotionOutcome]) -> StitchResult {
        run_outcomes_for_axis(frames, outcomes, StitchAxis::Vertical)
    }

    fn run_outcomes_for_axis(
        frames: &[Frame],
        outcomes: &[MotionOutcome],
        axis: StitchAxis,
    ) -> StitchResult {
        let mut outcomes = VecDeque::from(outcomes.to_vec());
        stitch_owned_with_estimator(
            frames.iter().cloned().map(Ok),
            StitchOptions {
                axis,
                collect_trace: true,
                ..StitchOptions::default()
            },
            |_, _, _| Ok(estimate(outcomes.pop_front().unwrap())),
        )
        .unwrap()
    }

    fn horizontal_row(values: &[u8], height: u32) -> Frame {
        let pixels = (0..height).flat_map(|_| values.iter().copied()).collect();
        Frame::new(values.len() as u32, height, PixelFormat::Gray8, pixels).unwrap()
    }

    #[test]
    fn one_frame_returns_clone_without_trace() {
        let first = solid(1, 5, 8);
        let result = stitch(std::slice::from_ref(&first), StitchOptions::default()).unwrap();
        assert_eq!(result.image, first);
        assert!(result.trace.is_empty());
    }

    #[test]
    fn iterator_stitches_frames_without_retaining_a_frame_slice() {
        let first = solid(1, 5, 8);
        let result =
            stitch_iter(std::iter::once(Ok(first.clone())), StitchOptions::default()).unwrap();
        assert_eq!(result.image, first);
    }

    #[test]
    fn incremental_stitcher_matches_batch_stitching() {
        let frames = vec![solid(1, 5, 8), solid(1, 5, 8)];
        let options = StitchOptions {
            collect_trace: true,
            ..StitchOptions::default()
        };
        let batch = stitch(&frames, options).unwrap();
        let mut incremental = Stitcher::new(options).unwrap();
        assert_eq!(incremental.push(frames[0].clone()).unwrap(), None);
        assert_eq!(
            incremental.push(frames[1].clone()).unwrap(),
            batch.trace.first().cloned()
        );
        assert_eq!(incremental.finish().unwrap(), batch);
    }

    #[test]
    fn empty_and_too_small_inputs_have_structured_errors() {
        assert!(matches!(
            stitch(&[], StitchOptions::default()).unwrap_err(),
            StitchError::EmptyInput
        ));
        assert!(matches!(
            stitch(&[solid(1, 4, 8)], StitchOptions::default()).unwrap_err(),
            StitchError::InvalidFirstGeometry {
                width: 4,
                height: 8
            }
        ));
    }

    #[test]
    fn mismatched_viewport_is_rejected_before_estimation() {
        let frames = [solid(1, 5, 8), solid(2, 6, 8)];
        let error = stitch(&frames, StitchOptions::default()).unwrap_err();
        assert!(matches!(
            error,
            StitchError::ViewportMismatch { index: 1, .. }
        ));
    }

    #[test]
    fn duplicate_skip_retains_previous_raw_index_and_does_not_estimate() {
        let calls = Cell::new(0);
        let frames = vec![solid(1, 5, 8), solid(1, 5, 8), solid(1, 5, 8)];
        let result = stitch_owned_with_estimator(
            frames.into_iter().map(Ok),
            StitchOptions {
                collect_trace: true,
                ..StitchOptions::default()
            },
            |_, _, _| {
                calls.set(calls.get() + 1);
                Ok(estimate(MotionOutcome::NoMotion))
            },
        )
        .unwrap();
        assert_eq!(calls.get(), 0);
        assert_eq!(result.trace.len(), 2);
        assert!(
            result
                .trace
                .iter()
                .all(|event| event.branch == StitchBranch::Skip)
        );
        assert!(
            result
                .trace
                .iter()
                .all(|event| event.previous_raw_index == 0)
        );
    }

    #[test]
    fn non_skip_advances_previous_raw_even_without_motion() {
        let calls = Cell::new(0);
        let frames = vec![solid(1, 5, 8), solid(2, 5, 8), solid(2, 5, 8)];
        let result = stitch_owned_with_estimator(
            frames.into_iter().map(Ok),
            StitchOptions {
                collect_trace: true,
                ..StitchOptions::default()
            },
            |_, _, _| {
                calls.set(calls.get() + 1);
                Ok(estimate(MotionOutcome::NoMotion))
            },
        )
        .unwrap();
        assert_eq!(calls.get(), 1);
        assert_eq!(result.trace[0].branch, StitchBranch::NoMovement);
        assert_eq!(result.trace[1].branch, StitchBranch::Skip);
        assert_eq!(result.trace[1].previous_raw_index, 1);
        assert_eq!(result.trace[1].before.processed_count, 1);
        assert_eq!(result.trace[1].after.processed_count, 1);
    }

    #[test]
    fn append_contained_and_prepend_each_apply_once() {
        let frames = vec![
            solid(1, 5, 8),
            solid(2, 5, 8),
            solid(3, 5, 8),
            solid(4, 5, 8),
        ];
        let result = run_outcomes(
            &frames,
            &[
                MotionOutcome::Motion { offset: -3 },
                MotionOutcome::Motion { offset: 2 },
                MotionOutcome::Motion { offset: 3 },
            ],
        );
        assert_eq!(result.image.height(), 13);
        assert_eq!(
            result
                .trace
                .iter()
                .map(|event| event.branch)
                .collect::<Vec<_>>(),
            vec![
                StitchBranch::Append,
                StitchBranch::Contained,
                StitchBranch::Prepend
            ]
        );
        assert_eq!(
            result.trace[1].before.canvas_height,
            result.trace[1].after.canvas_height
        );
        assert_eq!(result.trace[2].reference_mode, ReferenceMode::CanvasWindow);
        assert_eq!(result.trace[2].growth, 2);
        assert_eq!(result.trace[2].canvas_band_height, Some(4));
        assert_eq!(result.trace[2].synthetic_reference_band_height, Some(5));
    }

    #[test]
    fn horizontal_append_and_prepend_preserve_exact_seam_columns() {
        let frames = [
            horizontal_row(&[1, 2, 3, 4, 5, 6, 7, 8], 5),
            horizontal_row(&[11, 12, 13, 14, 15, 16, 17, 18], 5),
            horizontal_row(&[21, 22, 23, 24, 25, 26, 27, 28], 5),
        ];
        let result = run_outcomes_for_axis(
            &frames,
            &[
                MotionOutcome::Motion { offset: -3 },
                MotionOutcome::Motion { offset: 4 },
            ],
            StitchAxis::Horizontal,
        );
        assert_eq!((result.image.width(), result.image.height()), (12, 5));
        assert_eq!(
            result.image.row(0).unwrap(),
            &[21, 22, 23, 24, 4, 5, 6, 14, 15, 16, 17, 18]
        );
        assert_eq!(
            result
                .trace
                .iter()
                .map(|event| event.branch)
                .collect::<Vec<_>>(),
            vec![StitchBranch::Append, StitchBranch::Prepend]
        );
    }

    #[test]
    fn defensive_motion_limit_is_strict() {
        let height = 10;
        let accepted = run_outcomes(
            &[solid(1, 5, height), solid(2, 5, height)],
            &[MotionOutcome::Motion { offset: -6 }],
        );
        assert_eq!(accepted.trace[0].branch, StitchBranch::Append);
        let rejected = run_outcomes(
            &[solid(1, 5, height), solid(2, 5, height)],
            &[MotionOutcome::Motion { offset: -7 }],
        );
        assert_eq!(rejected.trace[0].branch, StitchBranch::NoMovement);
        assert_eq!(rejected.trace[0].accepted_offset, None);
    }

    #[test]
    fn no_motion_and_indeterminate_preserve_canvas_window_state() {
        let frames = vec![
            solid(1, 5, 8),
            solid(2, 5, 8),
            solid(3, 5, 8),
            solid(4, 5, 8),
            solid(5, 5, 8),
        ];
        let result = run_outcomes(
            &frames,
            &[
                MotionOutcome::Motion { offset: -3 },
                MotionOutcome::Motion { offset: 2 },
                MotionOutcome::NoMotion,
                MotionOutcome::Indeterminate,
            ],
        );
        assert_eq!(result.trace[1].branch, StitchBranch::Contained);
        assert_eq!(result.trace[2].branch, StitchBranch::NoMovement);
        assert_eq!(result.trace[3].branch, StitchBranch::NoMovement);
        assert_eq!(result.trace[2].reference_mode, ReferenceMode::CanvasWindow);
        assert_eq!(result.trace[3].reference_mode, ReferenceMode::CanvasWindow);
        for event in &result.trace[2..] {
            assert_eq!(
                event.after.viewport_position,
                result.trace[1].after.viewport_position
            );
            assert_eq!(
                event.after.max_viewport_position,
                result.trace[1].after.max_viewport_position
            );
            assert_eq!(
                event.after.canvas_height,
                result.trace[1].after.canvas_height
            );
            assert_eq!(
                event.after.accepted_count,
                result.trace[1].after.accepted_count
            );
        }
    }

    #[test]
    fn invalid_estimator_options_are_rejected() {
        let frame = solid(1, 5, 8);
        for estimator in [
            crate::MotionEstimatorOptions {
                max_motion_ratio: -0.1,
                ..crate::MotionEstimatorOptions::default()
            },
            crate::MotionEstimatorOptions {
                min_confidence: f32::NAN,
                ..crate::MotionEstimatorOptions::default()
            },
        ] {
            let error = stitch(
                std::slice::from_ref(&frame),
                StitchOptions {
                    axis: StitchAxis::Vertical,
                    estimator,
                    collect_trace: false,
                },
            )
            .unwrap_err();
            assert!(matches!(error, StitchError::InvalidOptions { .. }));
        }
    }
}
