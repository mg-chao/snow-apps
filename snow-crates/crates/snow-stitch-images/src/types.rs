use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum StitchAxis {
    #[default]
    Vertical,
    Horizontal,
}

impl StitchAxis {
    pub const fn primary_delta(self, dx: i32, dy: i32) -> i32 {
        match self {
            Self::Vertical => dy,
            Self::Horizontal => dx,
        }
    }

    pub const fn cross_delta(self, dx: i32, dy: i32) -> i32 {
        match self {
            Self::Vertical => dx,
            Self::Horizontal => dy,
        }
    }

    pub const fn primary_extent(self, width: u32, height: u32) -> u32 {
        match self {
            Self::Vertical => height,
            Self::Horizontal => width,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum StitchBranch {
    Append,
    Prepend,
    Contained,
    Skip,
    NoMovement,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum ReferenceMode {
    Synthetic,
    CanvasWindow,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "kebab-case")]
pub enum MotionOutcome {
    Motion { offset: i32 },
    NoMotion,
    Indeterminate,
}

impl MotionOutcome {
    pub const fn offset(self) -> Option<i32> {
        match self {
            Self::Motion { offset } => Some(offset),
            Self::NoMotion | Self::Indeterminate => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum MotionStage {
    InputTooSmall,
    IdenticalInterior,
    EmptyDescriptors,
    NoMatches,
    NoCandidates,
    SelectedNoMotion,
    LowConfidence,
    SceneCut,
    Selected,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct CandidateDiagnostics {
    pub offset: i32,
    pub raw_inliers: u32,
    pub inlier_tiles: u32,
    pub weighted_support: f32,
    pub weighted_inlier_share: f32,
    pub spatial_coverage: f32,
    pub alignment_error: f32,
    pub precise_alignment_error: Option<f32>,
    pub residual_gain: f32,
    pub score: f32,
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
pub struct RegionDiagnostics {
    pub fixed_tiles: u32,
    pub scrolling_tiles: u32,
    pub dynamic_tiles: u32,
    pub neutral_tiles: u32,
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MotionDiagnostics {
    pub stage: MotionStage,
    pub reference_keypoints: u32,
    pub incoming_keypoints: u32,
    pub mutual_matches: u32,
    pub direct_similarity: f32,
    pub selected_offset: Option<i32>,
    pub candidates: Vec<CandidateDiagnostics>,
    pub regions: RegionDiagnostics,
}

impl MotionDiagnostics {
    pub(crate) fn at(stage: MotionStage) -> Self {
        Self {
            stage,
            reference_keypoints: 0,
            incoming_keypoints: 0,
            mutual_matches: 0,
            direct_similarity: 0.0,
            selected_offset: None,
            candidates: Vec::new(),
            regions: RegionDiagnostics::default(),
        }
    }
}

#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct MotionEstimate {
    pub outcome: MotionOutcome,
    pub confidence: f32,
    pub diagnostics: MotionDiagnostics,
}

impl MotionEstimate {
    pub const fn offset(&self) -> Option<i32> {
        self.outcome.offset()
    }

    pub(crate) fn motion(offset: i32, confidence: f32, diagnostics: MotionDiagnostics) -> Self {
        debug_assert_ne!(offset, 0);
        Self {
            outcome: MotionOutcome::Motion { offset },
            confidence,
            diagnostics,
        }
    }

    pub(crate) fn no_motion(confidence: f32, diagnostics: MotionDiagnostics) -> Self {
        Self {
            outcome: MotionOutcome::NoMotion,
            confidence,
            diagnostics,
        }
    }

    pub(crate) fn indeterminate(confidence: f32, diagnostics: MotionDiagnostics) -> Self {
        Self {
            outcome: MotionOutcome::Indeterminate,
            confidence,
            diagnostics,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Serialize, Deserialize)]
pub struct MotionEstimatorOptions {
    pub tile_size: u32,
    pub max_features: u32,
    pub max_motion_ratio: f32,
    pub min_confidence: f32,
    pub temporal_learning_rate: f32,
}

impl Default for MotionEstimatorOptions {
    fn default() -> Self {
        Self {
            tile_size: 32,
            max_features: 2_500,
            max_motion_ratio: 0.6,
            min_confidence: 0.65,
            temporal_learning_rate: 0.2,
        }
    }
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Serialize, Deserialize)]
pub struct StitchOptions {
    pub axis: StitchAxis,
    pub estimator: MotionEstimatorOptions,
    pub record_decisions: bool,
}
