// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{PickerStyleContext, PickerStyles};
use crate::popup::{PopupArrow, PopupPlacement};
use gpui::{App, Window};
use std::{fmt, sync::Arc};

type StylesFn = dyn Fn(&PickerStyleContext, &mut Window, &mut App) -> PickerStyles + Send + Sync;

/// Keep the wrapper stable to preserve no-op scope updates.
#[derive(Clone)]
pub struct PickerStyleResolver(Arc<StylesFn>);
impl PickerStyleResolver {
    pub fn new(
        resolver: impl Fn(&PickerStyleContext, &mut Window, &mut App) -> PickerStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }

    pub fn resolve(
        &self,
        context: &PickerStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> PickerStyles {
        (self.0)(context, window, cx)
    }
}
impl PartialEq for PickerStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for PickerStyleResolver {}
impl fmt::Debug for PickerStyleResolver {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("PickerStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

/// Shared provider defaults; selection and constraints remain owned by the picker.
#[derive(Clone, Debug, PartialEq)]
pub struct PickerConfig {
    pub allow_clear: bool,
    pub input_read_only: bool,
    pub preview_value: bool,
    /// `None` uses the native default: time/date-time require confirmation.
    pub need_confirm: Option<bool>,
    /// `None` shows Today/Now for single date/time pickers only.
    pub show_now: Option<bool>,
    /// `None` aligns below the leading edge: left in LTR, right in RTL.
    /// Explicit placements retain their physical alignment in either direction.
    pub placement: Option<PopupPlacement>,
    /// Single pickers hide the arrow by default; the range provider enables it.
    pub arrow: PopupArrow,
    pub styles: PickerStyles,
    pub styles_fn: Option<PickerStyleResolver>,
}

impl Default for PickerConfig {
    fn default() -> Self {
        Self {
            allow_clear: true,
            input_read_only: false,
            preview_value: true,
            need_confirm: None,
            show_now: None,
            placement: None,
            arrow: false.into(),
            styles: Default::default(),
            styles_fn: None,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct PickerConfigPatch {
    pub allow_clear: Option<bool>,
    pub input_read_only: Option<bool>,
    pub preview_value: Option<bool>,
    pub need_confirm: Option<bool>,
    pub show_now: Option<bool>,
    pub placement: Option<PopupPlacement>,
    pub arrow: Option<PopupArrow>,
    pub styles: PickerStyles,
    pub styles_fn: Option<PickerStyleResolver>,
}

impl PickerConfigPatch {
    pub fn resolve(&self, parent: &PickerConfig) -> PickerConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        PickerConfig {
            allow_clear: self.allow_clear.unwrap_or(parent.allow_clear),
            input_read_only: self.input_read_only.unwrap_or(parent.input_read_only),
            preview_value: self.preview_value.unwrap_or(parent.preview_value),
            need_confirm: self.need_confirm.or(parent.need_confirm),
            show_now: self.show_now.or(parent.show_now),
            placement: self.placement.or(parent.placement),
            arrow: self.arrow.unwrap_or(parent.arrow),
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{StyleRefinement, Styled, px};

    #[test]
    fn picker_config_preserves_false_and_refines_semantic_slots() {
        let parent = PickerConfig {
            need_confirm: Some(true),
            show_now: Some(true),
            styles: PickerStyles {
                root: StyleRefinement::default().mt(px(4.)),
                footer: StyleRefinement::default().p(px(3.)),
                ..Default::default()
            },
            ..Default::default()
        };
        let child = PickerConfigPatch {
            allow_clear: Some(false),
            need_confirm: Some(false),
            show_now: Some(false),
            styles: PickerStyles {
                root: StyleRefinement::default().mt(px(8.)),
                ..Default::default()
            },
            ..Default::default()
        }
        .resolve(&parent);
        assert!(!child.allow_clear);
        assert_eq!(child.need_confirm, Some(false));
        assert_eq!(child.show_now, Some(false));
        assert_eq!(child.styles.root.margin.top, Some(px(8.).into()));
        assert_eq!(child.styles.footer, parent.styles.footer);
    }

    #[test]
    fn picker_config_inherits_resolver_identity_without_evaluating_it() {
        let calls = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let observed = calls.clone();
        let resolver = PickerStyleResolver::new(move |_, _, _| {
            observed.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            PickerStyles::default()
        });
        let parent = PickerConfig {
            styles_fn: Some(resolver.clone()),
            ..Default::default()
        };
        let child = PickerConfigPatch::default().resolve(&parent);
        assert_eq!(child.styles_fn, Some(resolver));
        assert_eq!(calls.load(std::sync::atomic::Ordering::Relaxed), 0);
    }

    #[test]
    fn picker_config_preserves_unset_and_explicit_physical_placements() {
        let unset = PickerConfig::default();
        assert_eq!(unset.placement, None);
        assert_eq!(PickerConfigPatch::default().resolve(&unset).placement, None);
        let parent = PickerConfigPatch {
            placement: Some(PopupPlacement::BottomLeft),
            ..Default::default()
        }
        .resolve(&unset);
        assert_eq!(parent.placement, Some(PopupPlacement::BottomLeft));
        assert_eq!(
            PickerConfigPatch::default().resolve(&parent).placement,
            parent.placement
        );
        assert_eq!(
            PickerConfigPatch {
                placement: Some(PopupPlacement::BottomRight),
                ..Default::default()
            }
            .resolve(&parent)
            .placement,
            Some(PopupPlacement::BottomRight)
        );
    }
}
