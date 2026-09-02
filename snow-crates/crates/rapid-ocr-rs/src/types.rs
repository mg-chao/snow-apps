use std::time::Duration;

use serde::{Deserialize, Serialize};

use crate::Quad;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum WordType {
    Cn,
    EnNum,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct WordInfo {
    pub words: Vec<Vec<String>>,
    pub word_cols: Vec<Vec<usize>>,
    pub word_types: Vec<WordType>,
    pub line_txt_len: f32,
    pub confs: Vec<f32>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct LineResult {
    pub text: String,
    pub score: f32,
    pub word_info: Option<WordInfo>,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct RecognizeOutput {
    pub lines: Vec<LineResult>,
    /// Wall-clock recognition duration; `None` unless stage timing was
    /// enabled via `set_stage_timing_enabled`.
    #[serde(skip)]
    pub elapsed: Option<Duration>,
}

impl RecognizeOutput {
    pub fn texts(&self) -> Vec<&str> {
        self.lines.iter().map(|line| line.text.as_str()).collect()
    }

    pub fn scores(&self) -> Vec<f32> {
        self.lines.iter().map(|line| line.score).collect()
    }

    pub fn len(&self) -> usize {
        self.lines.len()
    }

    pub fn is_empty(&self) -> bool {
        self.lines.is_empty()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WordBox {
    pub text: String,
    pub score: f32,
    pub bbox: Quad,
}
