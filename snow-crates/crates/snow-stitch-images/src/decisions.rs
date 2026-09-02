use serde::{Deserialize, Serialize};

use crate::{MotionDiagnostics, MotionOutcome, ReferenceMode, StitchBranch};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub struct StitchProgressState {
    pub viewport_position: i64,
    pub max_viewport_position: i64,
    pub canvas_height: u32,
    pub processed_count: usize,
    pub accepted_count: usize,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct StitchDecision {
    pub input_index: usize,
    pub previous_raw_index: usize,
    pub exact_duplicate: bool,
    pub reference_mode: ReferenceMode,
    pub motion: Option<MotionOutcome>,
    pub confidence: Option<f32>,
    pub accepted_offset: Option<i32>,
    pub branch: StitchBranch,
    pub before: StitchProgressState,
    pub after: StitchProgressState,
    pub growth: u32,
    pub canvas_band_height: Option<u32>,
    pub synthetic_reference_band_height: Option<u32>,
    pub motion_diagnostics: Option<MotionDiagnostics>,
}
