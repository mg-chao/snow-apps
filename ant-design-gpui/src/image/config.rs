// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{ImageStyleContext, ImageStyles};
use crate::icons::{IconName, IconRef};
use gpui::{App, Window};
use std::{fmt, sync::Arc};

type Resolver = dyn Fn(&ImageStyleContext, &mut Window, &mut App) -> ImageStyles + Send + Sync;
#[derive(Clone)]
pub struct ImageStyleResolver(Arc<Resolver>);
impl ImageStyleResolver {
    pub fn new(
        f: impl Fn(&ImageStyleContext, &mut Window, &mut App) -> ImageStyles + Send + Sync + 'static,
    ) -> Self {
        Self(Arc::new(f))
    }
    pub fn resolve(
        &self,
        context: &ImageStyleContext,
        w: &mut Window,
        cx: &mut App,
    ) -> ImageStyles {
        (self.0)(context, w, cx)
    }
}
impl PartialEq for ImageStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl fmt::Debug for ImageStyleResolver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("ImageStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct ImageConfig {
    pub preview_enabled: bool,
    pub mask: bool,
    pub mask_closable: bool,
    pub focus_trap: bool,
    pub wheel: bool,
    pub movable: bool,
    pub close_icon: IconRef,
    pub styles: ImageStyles,
    pub styles_fn: Option<ImageStyleResolver>,
}
impl Default for ImageConfig {
    fn default() -> Self {
        Self {
            preview_enabled: true,
            mask: true,
            mask_closable: true,
            focus_trap: true,
            wheel: true,
            movable: true,
            close_icon: IconName::CloseOutlined.into(),
            styles: ImageStyles::default(),
            styles_fn: None,
        }
    }
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ImageConfigPatch {
    pub preview_enabled: Option<bool>,
    pub mask: Option<bool>,
    pub mask_closable: Option<bool>,
    pub focus_trap: Option<bool>,
    pub wheel: Option<bool>,
    pub movable: Option<bool>,
    pub close_icon: Option<IconRef>,
    pub styles: ImageStyles,
    pub styles_fn: Option<ImageStyleResolver>,
}
impl ImageConfigPatch {
    pub fn resolve(&self, parent: &ImageConfig) -> ImageConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        ImageConfig {
            preview_enabled: self.preview_enabled.unwrap_or(parent.preview_enabled),
            mask: self.mask.unwrap_or(parent.mask),
            mask_closable: self.mask_closable.unwrap_or(parent.mask_closable),
            focus_trap: self.focus_trap.unwrap_or(parent.focus_trap),
            wheel: self.wheel.unwrap_or(parent.wheel),
            movable: self.movable.unwrap_or(parent.movable),
            close_icon: self
                .close_icon
                .clone()
                .unwrap_or_else(|| parent.close_icon.clone()),
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
