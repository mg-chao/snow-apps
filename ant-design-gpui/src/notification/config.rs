// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{NotificationPlacement, NotificationStyleResolver, NotificationStyles};
use crate::icons::IconRef;
use std::{sync::Arc, time::Duration};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NotificationStack {
    Disabled,
    Enabled { threshold: usize },
}
impl Default for NotificationStack {
    fn default() -> Self {
        Self::Enabled { threshold: 3 }
    }
}
impl NotificationStack {
    pub(crate) fn threshold(self) -> Option<usize> {
        match self {
            Self::Disabled => None,
            Self::Enabled { threshold } => Some(threshold.max(1)),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct NotificationConfig {
    /// None follows the current theme's edge-margin token.
    pub top: Option<f32>,
    pub bottom: Option<f32>,
    pub placement: NotificationPlacement,
    pub duration: Duration,
    pub pause_on_hover: bool,
    pub show_progress: bool,
    pub closable: bool,
    pub close_icon: Option<NotificationIcon>,
    pub stack: NotificationStack,
    pub max_count: Option<usize>,
    pub styles: NotificationStyles,
    pub styles_fn: Option<NotificationStyleResolver>,
}
impl Default for NotificationConfig {
    fn default() -> Self {
        Self {
            top: None,
            bottom: None,
            placement: Default::default(),
            duration: Duration::from_millis(4500),
            pause_on_hover: true,
            show_progress: false,
            closable: true,
            close_icon: None,
            stack: Default::default(),
            max_count: None,
            styles: Default::default(),
            styles_fn: None,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct NotificationConfigPatch {
    pub top: Option<f32>,
    pub bottom: Option<f32>,
    pub placement: Option<NotificationPlacement>,
    pub duration: Option<Duration>,
    pub pause_on_hover: Option<bool>,
    pub show_progress: Option<bool>,
    pub closable: Option<bool>,
    pub close_icon: Option<NotificationIcon>,
    pub stack: Option<NotificationStack>,
    /// Some(None), or Some(Some(0)), removes an inherited count limit.
    pub max_count: Option<Option<usize>>,
    pub styles: NotificationStyles,
    pub styles_fn: Option<NotificationStyleResolver>,
}
impl NotificationConfigPatch {
    pub fn resolve(&self, parent: &NotificationConfig) -> NotificationConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        NotificationConfig {
            top: self.top.filter(|v| v.is_finite()).or(parent.top),
            bottom: self.bottom.filter(|v| v.is_finite()).or(parent.bottom),
            placement: self.placement.unwrap_or(parent.placement),
            duration: self.duration.unwrap_or(parent.duration),
            pause_on_hover: self.pause_on_hover.unwrap_or(parent.pause_on_hover),
            show_progress: self.show_progress.unwrap_or(parent.show_progress),
            closable: self.closable.unwrap_or(parent.closable),
            close_icon: self
                .close_icon
                .clone()
                .or_else(|| parent.close_icon.clone()),
            stack: self.stack.unwrap_or(parent.stack),
            max_count: self
                .max_count
                .unwrap_or(parent.max_count)
                .filter(|v| *v > 0),
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}

/// An immutable configured icon; cloning preserves configuration identity.
#[derive(Clone, Debug)]
pub struct NotificationIcon(Arc<IconRef>);
impl NotificationIcon {
    pub fn new(icon: impl Into<IconRef>) -> Self {
        Self(Arc::new(icon.into()))
    }
    pub fn reference(&self) -> &IconRef {
        &self.0
    }
}
impl PartialEq for NotificationIcon {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for NotificationIcon {}
impl From<IconRef> for NotificationIcon {
    fn from(icon: IconRef) -> Self {
        Self::new(icon)
    }
}
impl From<crate::icons::IconName> for NotificationIcon {
    fn from(icon: crate::icons::IconName) -> Self {
        Self::new(icon)
    }
}
