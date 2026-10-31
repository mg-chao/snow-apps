// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{MessageStyleResolver, MessageStyles};
use std::time::Duration;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum MessageStack {
    #[default]
    Disabled,
    Enabled {
        threshold: usize,
    },
}
impl MessageStack {
    pub fn enabled() -> Self {
        Self::Enabled { threshold: 3 }
    }
    pub(crate) fn threshold(self) -> Option<usize> {
        match self {
            Self::Disabled => None,
            Self::Enabled { threshold } => Some(threshold.max(1)),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct MessageConfig {
    pub top: f32,
    pub duration: Duration,
    pub max_count: Option<usize>,
    pub stack: MessageStack,
    pub pause_on_hover: bool,
    pub styles: MessageStyles,
    pub styles_fn: Option<MessageStyleResolver>,
}
impl Default for MessageConfig {
    fn default() -> Self {
        Self {
            top: 8.,
            duration: Duration::from_secs(3),
            max_count: None,
            stack: MessageStack::Disabled,
            pause_on_hover: true,
            styles: MessageStyles::default(),
            styles_fn: None,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct MessageConfigPatch {
    pub top: Option<f32>,
    pub duration: Option<Duration>,
    /// `Some(None)` removes an inherited limit; a zero limit has the same meaning.
    pub max_count: Option<Option<usize>>,
    pub stack: Option<MessageStack>,
    pub pause_on_hover: Option<bool>,
    pub styles: MessageStyles,
    pub styles_fn: Option<MessageStyleResolver>,
}
impl MessageConfigPatch {
    pub fn resolve(&self, parent: &MessageConfig) -> MessageConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        MessageConfig {
            top: self.top.filter(|top| top.is_finite()).unwrap_or(parent.top),
            duration: self.duration.unwrap_or(parent.duration),
            max_count: self
                .max_count
                .unwrap_or(parent.max_count)
                .filter(|count| *count > 0),
            stack: self.stack.unwrap_or(parent.stack),
            pause_on_hover: self.pause_on_hover.unwrap_or(parent.pause_on_hover),
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
