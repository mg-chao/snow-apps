// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{ColorPickerStyleContext, ColorPickerStyles};
use crate::popup::PopupArrow;
use gpui::{App, Window};
use std::{fmt, sync::Arc};

type StylesFn =
    dyn Fn(&ColorPickerStyleContext, &mut Window, &mut App) -> ColorPickerStyles + Send + Sync;

#[derive(Clone)]
pub struct ColorPickerStyleResolver(Arc<StylesFn>);
impl ColorPickerStyleResolver {
    pub fn new(
        resolver: impl Fn(&ColorPickerStyleContext, &mut Window, &mut App) -> ColorPickerStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }
    pub fn resolve(
        &self,
        context: &ColorPickerStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> ColorPickerStyles {
        (self.0)(context, window, cx)
    }
}
impl PartialEq for ColorPickerStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for ColorPickerStyleResolver {}
impl fmt::Debug for ColorPickerStyleResolver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("ColorPickerStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct ColorPickerConfig {
    pub arrow: PopupArrow,
    pub styles: ColorPickerStyles,
    pub styles_fn: Option<ColorPickerStyleResolver>,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ColorPickerConfigPatch {
    pub arrow: Option<PopupArrow>,
    pub styles: ColorPickerStyles,
    pub styles_fn: Option<ColorPickerStyleResolver>,
}
impl ColorPickerConfigPatch {
    pub fn resolve(&self, parent: &ColorPickerConfig) -> ColorPickerConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        ColorPickerConfig {
            arrow: self.arrow.unwrap_or(parent.arrow),
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
