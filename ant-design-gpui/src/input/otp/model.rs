// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{collections::VecDeque, fmt, rc::Rc};

use gpui::SharedString;
use unicode_segmentation::UnicodeSegmentation;

use crate::input::HistoryLimit;

/// A segmented control is a form field, not an unbounded grid of native editors.
pub const MAX_OTP_LENGTH: usize = 1024;
pub type OtpFormatter = Rc<dyn Fn(&str) -> String>;
pub type OtpFilter = Rc<dyn Fn(&str) -> bool>;

#[derive(Clone)]
pub struct OtpOptions {
    pub length: usize,
    /// None displays the source; Some displays one grapheme per occupied cell.
    pub mask: Option<SharedString>,
    /// Transforms incoming text once, before filtering and the length limit.
    pub formatter: Option<OtpFormatter>,
    /// Receives complete Unicode graphemes, after formatting.
    pub filter: Option<OtpFilter>,
}
impl Default for OtpOptions {
    fn default() -> Self {
        Self {
            length: 6,
            mask: None,
            formatter: None,
            filter: None,
        }
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OtpError {
    InvalidLength,
    InvalidMask,
}
impl fmt::Display for OtpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::InvalidLength => "Verification code length must be between 1 and 1024",
            Self::InvalidMask => {
                "Verification code mask must be one grapheme without control characters"
            }
        })
    }
}
impl std::error::Error for OtpError {}
impl OtpOptions {
    pub fn validate(&self) -> Result<(), OtpError> {
        if !(1..=MAX_OTP_LENGTH).contains(&self.length) {
            return Err(OtpError::InvalidLength);
        }
        if self.mask.as_ref().is_some_and(|mask| {
            mask.graphemes(true).count() != 1 || mask.chars().any(char::is_control)
        }) {
            return Err(OtpError::InvalidMask);
        }
        Ok(())
    }
    fn characters(&self, input: &str) -> Vec<SharedString> {
        let input = input.replace(['\r', '\n'], "");
        let formatted = self
            .formatter
            .as_ref()
            .map_or(input.clone(), |format| format(&input));
        formatted
            .graphemes(true)
            .filter(|grapheme| !grapheme.chars().any(char::is_control))
            .filter(|grapheme| self.filter.as_ref().is_none_or(|filter| filter(grapheme)))
            .take(self.length)
            .map(|grapheme| SharedString::from(grapheme.to_owned()))
            .collect()
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(super) struct Selection {
    pub active: usize,
    pub all: bool,
}
#[derive(Clone)]
struct Snapshot {
    cells: Vec<SharedString>,
    selection: Selection,
}
struct Transaction {
    before: Snapshot,
    after: Snapshot,
}
impl Transaction {
    fn bytes(&self) -> usize {
        self.before
            .cells
            .iter()
            .chain(&self.after.cells)
            .map(|cell| cell.len())
            .sum()
    }
}
pub(super) struct OtpModel {
    pub options: OtpOptions,
    pub cells: Vec<SharedString>,
    pub selection: Selection,
    undo: VecDeque<Transaction>,
    redo: Vec<Transaction>,
    history_bytes: usize,
    history_limit: HistoryLimit,
}
impl OtpModel {
    pub fn new(options: OtpOptions) -> Result<Self, OtpError> {
        options.validate()?;
        Ok(Self {
            cells: vec![SharedString::default(); options.length],
            options,
            selection: Selection::default(),
            undo: VecDeque::new(),
            redo: Vec::new(),
            history_bytes: 0,
            history_limit: HistoryLimit::default(),
        })
    }
    pub fn value(&self) -> String {
        self.cells.iter().map(AsRef::as_ref).collect()
    }
    pub fn complete(&self) -> bool {
        self.cells.iter().all(|cell| !cell.is_empty())
    }
    pub fn focus_index(&self, requested: usize) -> usize {
        requested.min(self.cells.len() - 1).min(
            self.cells
                .iter()
                .position(|cell| cell.is_empty())
                .unwrap_or(self.cells.len() - 1),
        )
    }
    pub fn select(&mut self, index: usize) {
        self.selection = Selection {
            active: self.focus_index(index),
            all: false,
        };
    }
    pub fn can_undo(&self) -> bool {
        !self.undo.is_empty()
    }
    pub fn can_redo(&self) -> bool {
        !self.redo.is_empty()
    }
    pub fn set_history_limit(&mut self, limit: HistoryLimit) {
        self.history_limit = limit;
        self.trim_history();
    }
    pub fn set_value(&mut self, value: &str) -> bool {
        let mut cells = self.options.characters(value);
        cells.truncate(self.options.length);
        // Owner echoes must preserve interior empty cells and active composition.
        if cells.iter().map(AsRef::as_ref).collect::<String>() == self.value() {
            return false;
        }
        cells.resize(self.options.length, SharedString::default());
        self.cells = cells;
        self.selection.all = false;
        self.selection.active = self.focus_index(self.selection.active);
        self.clear_history();
        true
    }
    pub fn set_options(&mut self, options: OtpOptions) -> Result<bool, OtpError> {
        options.validate()?;
        let resized = self.options.length != options.length;
        self.options = options;
        if resized {
            self.cells
                .resize(self.options.length, SharedString::default());
            self.selection.active = self.focus_index(self.selection.active);
            self.selection.all = false;
            self.clear_history();
        }
        Ok(resized)
    }
    pub fn input(&mut self, index: usize, text: &str, advance: bool) -> bool {
        let chars = self.options.characters(text);
        if chars.is_empty() && !text.is_empty() {
            return false;
        }
        let before = self.snapshot();
        let replace_all = self.selection.all || chars.len() >= self.options.length;
        let start = if replace_all {
            0
        } else {
            index.min(self.options.length - 1)
        };
        if replace_all {
            self.cells.fill(SharedString::default());
        } else if chars.len() > 1 {
            self.cells[start..].fill(SharedString::default());
        }
        if chars.is_empty() {
            self.cells[start] = SharedString::default();
        } else {
            for (cell, character) in self.cells[start..].iter_mut().zip(&chars) {
                *cell = character.clone();
            }
        }
        self.selection.all = false;
        if advance {
            self.selection.active =
                self.focus_index((start + chars.len()).min(self.options.length - 1));
        }
        self.record(before)
    }
    pub fn clear(&mut self) -> bool {
        let before = self.snapshot();
        self.cells.fill(SharedString::default());
        self.selection = Selection::default();
        self.record(before)
    }
    pub fn undo(&mut self) -> bool {
        let Some(transaction) = self.undo.pop_back() else {
            return false;
        };
        self.restore(&transaction.before);
        self.redo.push(transaction);
        true
    }
    pub fn redo(&mut self) -> bool {
        let Some(transaction) = self.redo.pop() else {
            return false;
        };
        self.restore(&transaction.after);
        self.undo.push_back(transaction);
        true
    }
    fn snapshot(&self) -> Snapshot {
        Snapshot {
            cells: self.cells.clone(),
            selection: self.selection,
        }
    }
    fn restore(&mut self, snapshot: &Snapshot) {
        self.cells.clone_from(&snapshot.cells);
        self.selection = snapshot.selection;
    }
    fn record(&mut self, before: Snapshot) -> bool {
        if before.cells == self.cells {
            return false;
        }
        self.history_bytes -= self.redo.iter().map(Transaction::bytes).sum::<usize>();
        self.redo.clear();
        let transaction = Transaction {
            before,
            after: self.snapshot(),
        };
        self.history_bytes += transaction.bytes();
        self.undo.push_back(transaction);
        self.trim_history();
        true
    }
    fn clear_history(&mut self) {
        self.undo.clear();
        self.redo.clear();
        self.history_bytes = 0;
    }
    fn trim_history(&mut self) {
        while self.undo.len() + self.redo.len() > self.history_limit.transactions
            || self.history_bytes > self.history_limit.bytes
        {
            let removed = self.undo.pop_front().or_else(|| {
                if self.redo.is_empty() {
                    None
                } else {
                    Some(self.redo.remove(0))
                }
            });
            let Some(transaction) = removed else {
                break;
            };
            self.history_bytes -= transaction.bytes();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn model() -> OtpModel {
        OtpModel::new(OtpOptions::default()).unwrap()
    }
    #[test]
    fn full_paste_replaces_from_any_cell_and_partial_paste_starts_at_active_cell() {
        let mut model = model();
        model.set_value("12");
        model.select(2);
        assert!(model.input(2, "34", true));
        assert_eq!(model.value(), "1234");
        assert_eq!(model.selection.active, 4);
        assert!(model.input(4, "987654", true));
        assert_eq!(model.value(), "987654");
        assert_eq!(model.selection.active, 5);
        model.select(2);
        model.input(2, "AB", true);
        assert_eq!(model.value(), "98AB");
        assert_eq!(model.selection.active, 4);
    }
    #[test]
    fn formatter_and_filter_run_before_limit_and_preserve_graphemes() {
        let mut model = OtpModel::new(OtpOptions {
            length: 4,
            formatter: Some(Rc::new(str::to_uppercase)),
            filter: Some(Rc::new(|grapheme| grapheme != "-")),
            ..Default::default()
        })
        .unwrap();
        model.input(0, "a-中-e\u{301}-👩🏽‍💻-z", true);
        assert_eq!(model.value(), "A中E\u{301}👩🏽‍💻");
        assert!(model.complete());
        assert_eq!(model.cells[3].as_ref(), "👩🏽‍💻");
        model.select(0);
        assert!(!model.input(0, "-", true));
        assert_eq!(model.selection.active, 0);
    }
    #[test]
    fn aggregate_history_restores_cells_holes_and_focus_as_single_transactions() {
        let mut model = model();
        for text in ["1", "2", "3"] {
            model.input(model.selection.active, text, true);
        }
        assert_eq!(model.selection.active, 3);
        model.undo();
        assert_eq!(model.value(), "12");
        assert_eq!(model.selection.active, 2);
        model.redo();
        assert_eq!(model.value(), "123");
        model.select(1);
        model.input(1, "", true);
        assert_eq!(model.cells[1], "");
        assert_eq!(model.cells[2], "3");
        assert_eq!(model.focus_index(5), 1);
        assert!(!model.set_value("13"));
        assert_eq!(model.cells[1], "");
        model.undo();
        assert_eq!(model.value(), "123");
        model.selection.all = true;
        model.input(1, "9", true);
        assert_eq!(model.value(), "9");
        model.undo();
        assert_eq!(model.value(), "123");
    }
    #[test]
    fn external_replacement_and_length_changes_reset_bounded_history() {
        let mut model = model();
        model.set_history_limit(HistoryLimit {
            transactions: 2,
            bytes: 128,
        });
        for text in ["1", "2", "3", "4"] {
            model.input(model.selection.active, text, true);
        }
        assert!(model.undo());
        assert!(model.undo());
        assert!(!model.undo());
        model.set_value("1234567");
        assert_eq!(model.value(), "123456");
        assert!(!model.can_redo());
        model
            .set_options(OtpOptions {
                length: 2,
                ..Default::default()
            })
            .unwrap();
        assert_eq!(model.value(), "12");
        assert!(!model.can_undo());
        assert!(
            model
                .set_options(OtpOptions {
                    length: 0,
                    ..Default::default()
                })
                .is_err()
        );
        assert_eq!(model.cells.len(), 2);
    }
    #[test]
    fn invalid_masks_are_rejected_without_rejecting_combined_unicode_masks() {
        for mask in ["", "ab", "\n"] {
            assert_eq!(
                OtpOptions {
                    mask: Some(mask.into()),
                    ..Default::default()
                }
                .validate(),
                Err(OtpError::InvalidMask)
            );
        }
        assert!(
            OtpOptions {
                mask: Some("👩🏽‍💻".into()),
                ..Default::default()
            }
            .validate()
            .is_ok()
        );
    }
}
