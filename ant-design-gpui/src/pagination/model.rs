// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

/// The complete pager, an editable current/total pair, or its read-only form.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum PaginationMode {
    #[default]
    Full,
    Simple,
    SimpleReadOnly,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum QuickJumper {
    #[default]
    Hidden,
    /// Enter or leaving the editor commits the requested page.
    Blur,
    /// Enter or the confirmation action commits; leaving the editor does not.
    Confirm,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaginationSettings {
    pub mode: PaginationMode,
    pub quick_jumper: QuickJumper,
    pub show_less_items: bool,
    pub show_prev_next_jumpers: bool,
    pub hide_on_single_page: bool,
}
impl Default for PaginationSettings {
    fn default() -> Self {
        Self {
            mode: PaginationMode::Full,
            quick_jumper: QuickJumper::Hidden,
            show_less_items: false,
            show_prev_next_jumpers: true,
            hide_on_single_page: false,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PaginationItemKind {
    Page(u64),
    Previous,
    Next,
    JumpPrevious,
    JumpNext,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PaginationItem {
    pub kind: PaginationItemKind,
    pub target: u64,
    pub disabled: bool,
    /// Ant Design hides the inner neighbor of an ellipsis at <= 992 CSS pixels.
    pub hidden_at_narrow: bool,
}

/// All arithmetic and page generation are independent of GPUI. The visible
/// sequence has at most eleven entries, regardless of the size of the dataset.
pub(super) struct PaginationModel {
    pub total: u64,
    pub current: u64,
    pub page_size: u64,
    pub settings: PaginationSettings,
}
impl Default for PaginationModel {
    fn default() -> Self {
        Self {
            total: 0,
            current: 1,
            page_size: 10,
            settings: PaginationSettings::default(),
        }
    }
}
impl PaginationModel {
    pub fn current(&self) -> u64 {
        self.current
    }
    pub fn total(&self) -> u64 {
        self.total
    }
    pub fn page_size(&self) -> u64 {
        self.page_size
    }
    pub fn page_count(&self) -> u64 {
        page_count(self.total, self.page_size)
    }
    pub fn clamp(&self, page: u64) -> u64 {
        page.clamp(1, self.page_count().max(1))
    }
    pub fn normalize(&mut self) {
        self.page_size = self.page_size.max(1);
        self.current = self.clamp(self.current);
    }
    pub fn hidden(&self) -> bool {
        self.settings.hide_on_single_page && self.total <= self.page_size
    }
    pub fn range(&self) -> (u64, u64) {
        if self.total == 0 {
            (0, 0)
        } else {
            (
                self.current
                    .saturating_sub(1)
                    .saturating_mul(self.page_size)
                    .saturating_add(1),
                self.current.saturating_mul(self.page_size).min(self.total),
            )
        }
    }
    pub fn items(&self) -> Vec<PaginationItem> {
        let last = self.page_count();
        let mut items = Vec::with_capacity(11);
        items.push(PaginationItem {
            kind: PaginationItemKind::Previous,
            target: self.current.saturating_sub(1),
            disabled: self.current <= 1 || last == 0,
            hidden_at_narrow: false,
        });
        if self.settings.mode == PaginationMode::Full {
            let buffer = if self.settings.show_less_items { 1 } else { 2 };
            if last <= 3 + 2 * buffer {
                for page in 1..=last.max(1) {
                    items.push(PaginationItem {
                        kind: PaginationItemKind::Page(page),
                        target: page,
                        disabled: last == 0,
                        hidden_at_narrow: false,
                    });
                }
            } else {
                let mut left = self.current.saturating_sub(buffer).max(1);
                let mut right = self.current.saturating_add(buffer).min(last);
                if self.current - 1 <= buffer {
                    right = 1 + 2 * buffer;
                }
                if last - self.current <= buffer {
                    left = last - 2 * buffer;
                }
                let jump_previous = self.settings.show_prev_next_jumpers
                    && self.current > 2 * buffer
                    && self.current != 3;
                let jump_next = self.settings.show_prev_next_jumpers
                    && last - self.current >= 2 * buffer
                    && self.current != last - 2;
                if !self.settings.show_less_items && jump_previous && right != last {
                    left += 1;
                }
                if !self.settings.show_less_items && jump_next && left != 1 {
                    right -= 1;
                }
                let page = |page, hidden_at_narrow| PaginationItem {
                    kind: PaginationItemKind::Page(page),
                    target: page,
                    disabled: false,
                    hidden_at_narrow,
                };
                if left != 1 {
                    items.push(page(1, false));
                }
                if jump_previous {
                    items.push(PaginationItem {
                        kind: PaginationItemKind::JumpPrevious,
                        target: self.current.saturating_sub(2 * buffer + 1).max(1),
                        disabled: false,
                        hidden_at_narrow: false,
                    });
                }
                for index in left..=right {
                    items.push(page(
                        index,
                        (jump_previous && index == left) || (jump_next && index == right),
                    ));
                }
                if jump_next {
                    items.push(PaginationItem {
                        kind: PaginationItemKind::JumpNext,
                        target: self.current.saturating_add(2 * buffer + 1).min(last),
                        disabled: false,
                        hidden_at_narrow: false,
                    });
                }
                if right != last {
                    items.push(page(last, false));
                }
            }
        }
        items.push(PaginationItem {
            kind: PaginationItemKind::Next,
            target: self.current.saturating_add(1).min(last),
            disabled: self.current >= last,
            hidden_at_narrow: false,
        });
        items
    }
}

pub(super) fn page_count(total: u64, page_size: u64) -> u64 {
    if total == 0 {
        0
    } else {
        (total - 1) / page_size.max(1) + 1
    }
}

/// Parse arbitrarily large digit strings in linear time with bounded storage.
/// Saturating retains the intended clamp-to-last-page behavior for huge pastes.
pub(super) fn digit_page(text: &str) -> Option<u64> {
    if text.is_empty() || !text.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    Some(text.bytes().fold(0u64, |value, byte| {
        value
            .saturating_mul(10)
            .saturating_add(u64::from(byte - b'0'))
    }))
}

pub(super) fn simple_page(text: &str) -> Option<u64> {
    if let Some(value) = digit_page(text) {
        return Some(value);
    }
    let value = text.trim().parse::<f64>().ok()?;
    (value.is_finite() && value.fract() == 0.).then_some(value.max(0.) as u64)
}
