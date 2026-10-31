// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Text storage and editing transactions, independent of the window and renderer.
//!
//! Selection offsets are UTF-8 bytes. Only the native input bridge uses UTF-16.
//! A composition is one transaction, regardless of the number of preedit updates.

use std::{collections::VecDeque, ops::Range, sync::Arc, time::Duration};

use unicode_segmentation::UnicodeSegmentation;

/// A directional selection at UTF-8 byte boundaries.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct TextSelection {
    pub anchor: usize,
    pub caret: usize,
}

impl TextSelection {
    pub fn range(self) -> Range<usize> {
        self.anchor.min(self.caret)..self.anchor.max(self.caret)
    }

    pub fn is_empty(self) -> bool {
        self.anchor == self.caret
    }

    pub fn collapsed(offset: usize) -> Self {
        Self {
            anchor: offset,
            caret: offset,
        }
    }
}

/// The unit used for a count or hard limit. Custom strategies must be monotonic
/// when removing complete graphemes from the end of an insertion.
#[derive(Clone, Default)]
pub enum CountStrategy {
    #[default]
    Graphemes,
    Utf16,
    Custom(Arc<dyn Fn(&str) -> usize + Send + Sync>),
}

impl CountStrategy {
    pub fn count(&self, text: &str) -> usize {
        match self {
            Self::Graphemes => text.graphemes(true).count(),
            Self::Utf16 => text.encode_utf16().count(),
            Self::Custom(count) => count(text),
        }
    }
}

/// Input normalization and insertion constraints. External value replacement
/// intentionally does not enforce the hard limit, like controlled Ant inputs.
#[derive(Clone, Default)]
pub struct EditPolicy {
    pub multiline: bool,
    pub max_length: Option<usize>,
    pub count: CountStrategy,
}

impl EditPolicy {
    fn normalize(&self, text: &str) -> String {
        if self.multiline {
            text.replace("\r\n", "\n").replace('\r', "\n")
        } else {
            text.replace(['\r', '\n'], "")
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum EditOrigin {
    Typing,
    Paste,
    Cut,
    Delete,
    Clear,
    Composition,
    CompositionCancel,
    Programmatic,
    Format,
    Undo,
    Redo,
    Accessibility,
    Step,
}

#[derive(Clone, Copy, Debug)]
pub struct HistoryLimit {
    pub transactions: usize,
    pub bytes: usize,
}

impl Default for HistoryLimit {
    fn default() -> Self {
        Self {
            transactions: 128,
            bytes: 8 * 1024 * 1024,
        }
    }
}

#[derive(Clone)]
struct Delta {
    start: usize,
    removed: String,
    inserted: String,
    before: TextSelection,
    after: TextSelection,
    origin: EditOrigin,
    time: Duration,
    group: u64,
}

impl Delta {
    fn bytes(&self) -> usize {
        self.removed.len() + self.inserted.len()
    }
}

struct Composition {
    text: String,
    selection: TextSelection,
}

/// A form-sized editing buffer. History stores changed spans, not entire values.
pub struct EditingBuffer {
    text: String,
    selection: TextSelection,
    marked: Option<Range<usize>>,
    composition: Option<Composition>,
    graphemes: Vec<usize>,
    scalars: Vec<(usize, usize)>,
    undo: VecDeque<Delta>,
    redo: Vec<Delta>,
    history_bytes: usize,
    history_limit: HistoryLimit,
    group: u64,
    revision: u64,
}

impl Default for EditingBuffer {
    fn default() -> Self {
        Self::new("")
    }
}

impl EditingBuffer {
    pub fn new(text: impl Into<String>) -> Self {
        let text = text.into();
        let selection = TextSelection::collapsed(text.len());
        let mut this = Self {
            text,
            selection,
            marked: None,
            composition: None,
            graphemes: Vec::new(),
            scalars: Vec::new(),
            undo: VecDeque::new(),
            redo: Vec::new(),
            history_bytes: 0,
            history_limit: HistoryLimit::default(),
            group: 0,
            revision: 0,
        };
        this.index();
        this
    }

    pub fn text(&self) -> &str {
        &self.text
    }
    pub fn selection(&self) -> TextSelection {
        self.selection
    }
    pub fn marked_range(&self) -> Option<Range<usize>> {
        self.marked.clone()
    }
    pub fn is_composing(&self) -> bool {
        self.composition.is_some()
    }
    pub fn revision(&self) -> u64 {
        self.revision
    }
    pub fn can_undo(&self) -> bool {
        !self.undo.is_empty()
    }
    pub fn can_redo(&self) -> bool {
        !self.redo.is_empty()
    }
    pub fn history_bytes(&self) -> usize {
        self.history_bytes
    }
    pub fn grapheme_boundaries(&self) -> &[usize] {
        &self.graphemes
    }
    pub fn utf16_len(&self) -> usize {
        self.scalars.last().map_or(0, |entry| entry.1)
    }

    pub fn count(&self, strategy: &CountStrategy) -> usize {
        match strategy {
            CountStrategy::Graphemes => self.graphemes.len().saturating_sub(1),
            CountStrategy::Utf16 => self.utf16_len(),
            CountStrategy::Custom(count) => count(&self.text),
        }
    }

    fn index(&mut self) {
        self.graphemes.clear();
        self.graphemes
            .extend(self.text.grapheme_indices(true).map(|(i, _)| i));
        self.graphemes.push(self.text.len());
        self.scalars.clear();
        let mut utf16 = 0;
        for (byte, c) in self.text.char_indices() {
            self.scalars.push((byte, utf16));
            utf16 += c.len_utf16();
        }
        self.scalars.push((self.text.len(), utf16));
        self.revision = self.revision.wrapping_add(1);
    }

    pub fn byte_to_utf16(&self, byte: usize) -> usize {
        let i = self
            .scalars
            .partition_point(|entry| entry.0 <= byte)
            .saturating_sub(1);
        self.scalars[i].1
    }

    pub fn utf16_to_byte(&self, offset: usize, round_up: bool) -> usize {
        let i = if round_up {
            self.scalars
                .partition_point(|entry| entry.1 < offset)
                .min(self.scalars.len() - 1)
        } else {
            self.scalars
                .partition_point(|entry| entry.1 <= offset)
                .saturating_sub(1)
        };
        self.scalars[i].0
    }

    pub fn utf16_range(&self, range: Range<usize>) -> Range<usize> {
        self.byte_to_utf16(range.start)..self.byte_to_utf16(range.end)
    }

    pub fn byte_range(&self, range: Range<usize>) -> Range<usize> {
        let start = self.utf16_to_byte(range.start.min(range.end), false);
        let end = self.utf16_to_byte(range.start.max(range.end), true);
        start..end
    }

    pub fn snap(&self, byte: usize, forward: bool) -> usize {
        let i = if forward {
            self.graphemes
                .partition_point(|offset| *offset < byte)
                .min(self.graphemes.len() - 1)
        } else {
            self.graphemes
                .partition_point(|offset| *offset <= byte)
                .saturating_sub(1)
        };
        self.graphemes[i]
    }

    pub fn select(&mut self, selection: TextSelection) {
        self.break_group();
        self.selection = TextSelection {
            anchor: self.snap(selection.anchor, false),
            caret: self.snap(selection.caret, false),
        };
    }

    pub fn select_all(&mut self) {
        self.select(TextSelection {
            anchor: 0,
            caret: self.text.len(),
        });
    }

    pub fn move_to(&mut self, offset: usize, extend: bool) {
        let caret = self.snap(offset, false);
        self.select(TextSelection {
            anchor: if extend { self.selection.anchor } else { caret },
            caret,
        });
    }

    pub fn adjacent(&self, offset: usize, forward: bool) -> usize {
        let i = if forward {
            self.graphemes
                .partition_point(|byte| *byte <= offset)
                .min(self.graphemes.len() - 1)
        } else {
            self.graphemes
                .partition_point(|byte| *byte < offset)
                .saturating_sub(1)
        };
        self.graphemes[i]
    }

    pub fn word_boundary(&self, offset: usize, forward: bool) -> usize {
        if forward {
            self.text
                .unicode_word_indices()
                .map(|(i, word)| i + word.len())
                .find(|end| *end > offset)
                .unwrap_or(self.text.len())
        } else {
            self.text
                .unicode_word_indices()
                .map(|(i, _)| i)
                .take_while(|start| *start < offset)
                .last()
                .unwrap_or(0)
        }
    }

    pub fn move_horizontal(&mut self, forward: bool, extend: bool, word: bool) {
        let range = self.selection.range();
        let target = if !extend && !range.is_empty() {
            if forward { range.end } else { range.start }
        } else if word {
            self.word_boundary(self.selection.caret, forward)
        } else {
            self.adjacent(self.selection.caret, forward)
        };
        self.move_to(target, extend);
    }

    pub fn select_word(&mut self, offset: usize) {
        let offset = self.snap(offset, false);
        let range = self
            .text
            .split_word_bound_indices()
            .find(|(start, word)| *start <= offset && offset < start + word.len())
            .map(|(start, word)| start..start + word.len())
            .unwrap_or(offset..offset);
        self.select(TextSelection {
            anchor: range.start,
            caret: range.end,
        });
    }

    pub fn line_range(&self, offset: usize) -> Range<usize> {
        let offset = self.snap(offset, false);
        let start = self.text[..offset].rfind('\n').map_or(0, |i| i + 1);
        let end = self.text[offset..]
            .find('\n')
            .map_or(self.text.len(), |i| offset + i);
        start..end
    }

    pub fn set_text(&mut self, text: &str, policy: &EditPolicy) -> bool {
        let text = policy.normalize(text);
        if text == self.text {
            return false;
        }
        self.text = text;
        self.composition = None;
        self.marked = None;
        self.index();
        self.select(self.selection);
        self.clear_history();
        true
    }

    /// Insert a user edit, applying the hard limit to the insertion only. Text
    /// supplied by an external owner can remain over the limit and be shortened.
    pub fn replace(
        &mut self,
        range: Option<Range<usize>>,
        inserted: &str,
        origin: EditOrigin,
        now: Duration,
        policy: &EditPolicy,
    ) -> bool {
        let before_composition = self.composition.take();
        let range = range
            .or_else(|| self.marked.clone())
            .unwrap_or(self.selection.range());
        let range = self.scalar_range(range);
        self.marked = None;
        let mut inserted = policy.normalize(inserted);
        self.limit_insertion(&range, &mut inserted, policy);
        let before_selection = self.selection;
        let removed = self.text[range.clone()].to_owned();
        let changed = removed != inserted;
        self.text.replace_range(range.clone(), &inserted);
        self.index();
        self.selection = TextSelection::collapsed(self.snap(range.start + inserted.len(), true));
        if let Some(before) = before_composition {
            self.record_composition(before, now);
        } else if changed {
            self.record(Delta {
                start: range.start,
                removed,
                inserted,
                before: before_selection,
                after: self.selection,
                origin,
                time: now,
                group: self.group,
            });
        }
        changed
    }

    /// Apply a formatter to the edit it immediately follows. The displayed text
    /// and its undo delta must agree; otherwise undoing a comma insertion corrupts
    /// subsequent offsets. Reformatting is never allowed during composition.
    pub fn rewrite_formatted(
        &mut self,
        text: &str,
        selection: TextSelection,
        now: Duration,
        policy: &EditPolicy,
        amend: bool,
    ) -> bool {
        if self.is_composing() {
            return false;
        }
        let text = policy.normalize(text);
        if text == self.text {
            return false;
        }
        let previous = if amend && self.redo.is_empty() {
            self.undo.pop_back()
        } else {
            None
        };
        let mut before_text = self.text.clone();
        let (before, origin, time, group) = if let Some(delta) = previous {
            self.history_bytes -= delta.bytes();
            before_text.replace_range(
                delta.start..delta.start + delta.inserted.len(),
                &delta.removed,
            );
            (delta.before, delta.origin, delta.time, delta.group)
        } else {
            (self.selection, EditOrigin::Format, now, self.group)
        };
        self.text = text;
        self.index();
        self.selection = TextSelection {
            anchor: self.snap(selection.anchor, false),
            caret: self.snap(selection.caret, false),
        };
        if before_text != self.text {
            let (start, removed, inserted) = difference(&before_text, &self.text);
            self.record(Delta {
                start,
                removed,
                inserted,
                before,
                after: self.selection,
                origin,
                time,
                group,
            });
        }
        true
    }

    fn scalar_range(&self, range: Range<usize>) -> Range<usize> {
        let start = range.start.min(range.end).min(self.text.len());
        let end = range.start.max(range.end).min(self.text.len());
        let start = self.scalars[self
            .scalars
            .partition_point(|p| p.0 <= start)
            .saturating_sub(1)]
        .0;
        let end = self.scalars[self
            .scalars
            .partition_point(|p| p.0 < end)
            .min(self.scalars.len() - 1)]
        .0;
        start..end
    }

    fn limit_insertion(&self, range: &Range<usize>, inserted: &mut String, policy: &EditPolicy) {
        let Some(max) = policy.max_length else {
            return;
        };
        if inserted.is_empty() {
            return;
        }
        let candidate = |text: &str| {
            let mut value = String::with_capacity(self.text.len() - range.len() + text.len());
            value.push_str(&self.text[..range.start]);
            value.push_str(text);
            value.push_str(&self.text[range.end..]);
            value
        };
        let effective_max = max.max(policy.count.count(&candidate("")));
        if policy.count.count(&candidate(inserted)) <= effective_max {
            return;
        }
        let boundaries: Vec<_> = inserted
            .grapheme_indices(true)
            .map(|(i, _)| i)
            .chain([inserted.len()])
            .collect();
        let mut low = 0;
        let mut high = boundaries.len() - 1;
        while low < high {
            let mid = (low + high).div_ceil(2);
            if policy.count.count(&candidate(&inserted[..boundaries[mid]])) <= effective_max {
                low = mid;
            } else {
                high = mid - 1;
            }
        }
        inserted.truncate(boundaries[low]);
    }

    pub fn delete(
        &mut self,
        forward: bool,
        word: bool,
        now: Duration,
        policy: &EditPolicy,
    ) -> bool {
        let mut range = self.selection.range();
        if range.is_empty() {
            let target = if word {
                self.word_boundary(range.start, forward)
            } else {
                self.adjacent(range.start, forward)
            };
            range = target.min(range.start)..target.max(range.end);
        }
        self.replace(Some(range), "", EditOrigin::Delete, now, policy)
    }

    /// UTF-16 ranges here follow the native IME contract, including a selection
    /// relative to the new marked text. Preedit is not length-limited.
    pub fn compose(
        &mut self,
        range: Option<Range<usize>>,
        text: &str,
        selected: Option<Range<usize>>,
        policy: &EditPolicy,
    ) -> bool {
        if self.composition.is_none() {
            self.break_group();
            self.composition = Some(Composition {
                text: self.text.clone(),
                selection: self.selection,
            });
        }
        let range = range
            .map(|r| self.byte_range(r))
            .or_else(|| self.marked.clone())
            .unwrap_or(self.selection.range());
        let text = policy.normalize(text);
        let changed = self.text[range.clone()] != text;
        self.text.replace_range(range.clone(), &text);
        self.index();
        let end = range.start + text.len();
        self.marked = Some(range.start..end);
        self.selection = if let Some(selected) = selected {
            let base = self.byte_to_utf16(range.start);
            let count = text.encode_utf16().count();
            TextSelection {
                anchor: self.utf16_to_byte(base + selected.start.min(count), false),
                caret: self.utf16_to_byte(base + selected.end.min(count), true),
            }
        } else {
            TextSelection::collapsed(end)
        };
        changed
    }

    pub fn finish_composition(&mut self, now: Duration, policy: &EditPolicy) -> bool {
        if self.composition.is_none() {
            return false;
        }
        let range = self.marked.clone().unwrap_or(self.selection.range());
        let text = self.text[range.clone()].to_owned();
        self.replace(Some(range), &text, EditOrigin::Composition, now, policy)
    }

    pub fn cancel_composition(&mut self) -> bool {
        let Some(before) = self.composition.take() else {
            return false;
        };
        let changed = self.text != before.text;
        self.text = before.text;
        self.selection = before.selection;
        self.marked = None;
        self.index();
        self.break_group();
        changed
    }

    fn record_composition(&mut self, before: Composition, now: Duration) {
        if self.text == before.text {
            return;
        }
        let (start, removed, inserted) = difference(&before.text, &self.text);
        self.record(Delta {
            start,
            removed,
            inserted,
            before: before.selection,
            after: self.selection,
            origin: EditOrigin::Composition,
            time: now,
            group: self.group,
        });
        self.break_group();
    }

    pub fn break_group(&mut self) {
        self.group = self.group.wrapping_add(1);
    }

    pub fn set_history_limit(&mut self, limit: HistoryLimit) {
        self.history_limit = limit;
        self.prune_history();
    }

    pub fn clear_history(&mut self) {
        self.undo.clear();
        self.redo.clear();
        self.history_bytes = 0;
        self.break_group();
    }

    fn record(&mut self, delta: Delta) {
        self.history_bytes -= self.redo.iter().map(Delta::bytes).sum::<usize>();
        self.redo.clear();
        self.history_bytes += delta.bytes();
        let merge = self.undo.back_mut().filter(|last| {
            last.origin == delta.origin
                && last.group == delta.group
                && last.after == delta.before
                && delta.time.saturating_sub(last.time) <= Duration::from_millis(750)
        });
        if let Some(last) = merge.filter(|last| {
            (delta.origin == EditOrigin::Typing
                && delta.removed.is_empty()
                && delta.start == last.start + last.inserted.len())
                || (delta.origin == EditOrigin::Delete
                    && last.inserted.is_empty()
                    && delta.inserted.is_empty()
                    && (delta.start == last.start
                        || delta.start + delta.removed.len() == last.start))
        }) {
            if delta.origin == EditOrigin::Typing {
                last.inserted.push_str(&delta.inserted);
            } else if delta.start == last.start {
                last.removed.push_str(&delta.removed);
            } else {
                last.removed.insert_str(0, &delta.removed);
                last.start = delta.start;
            }
            last.after = delta.after;
            last.time = delta.time;
        } else {
            self.undo.push_back(delta);
        }
        self.prune_history();
    }

    fn prune_history(&mut self) {
        while self.undo.len() + self.redo.len() > self.history_limit.transactions
            || self.history_bytes > self.history_limit.bytes
        {
            if let Some(delta) = self.undo.pop_front() {
                self.history_bytes -= delta.bytes();
            } else if !self.redo.is_empty() {
                self.history_bytes -= self.redo.remove(0).bytes();
            } else {
                break;
            }
        }
    }

    pub fn undo(&mut self, now: Duration, policy: &EditPolicy) -> bool {
        self.finish_composition(now, policy);
        let Some(delta) = self.undo.pop_back() else {
            return false;
        };
        self.text.replace_range(
            delta.start..delta.start + delta.inserted.len(),
            &delta.removed,
        );
        self.selection = delta.before;
        self.index();
        self.redo.push(delta);
        self.break_group();
        true
    }

    pub fn redo(&mut self) -> bool {
        let Some(delta) = self.redo.pop() else {
            return false;
        };
        self.text.replace_range(
            delta.start..delta.start + delta.removed.len(),
            &delta.inserted,
        );
        self.selection = delta.after;
        self.index();
        self.undo.push_back(delta);
        self.break_group();
        true
    }
}

fn difference(before: &str, after: &str) -> (usize, String, String) {
    let prefix = before
        .chars()
        .zip(after.chars())
        .take_while(|(a, b)| a == b)
        .map(|(c, _)| c.len_utf8())
        .sum::<usize>();
    let suffix = before[prefix..]
        .chars()
        .rev()
        .zip(after[prefix..].chars().rev())
        .take_while(|(a, b)| a == b)
        .map(|(c, _)| c.len_utf8())
        .sum::<usize>();
    (
        prefix,
        before[prefix..before.len() - suffix].to_owned(),
        after[prefix..after.len() - suffix].to_owned(),
    )
}

#[cfg(test)]
mod tests;
