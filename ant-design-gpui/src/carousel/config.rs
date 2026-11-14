// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{CarouselStyleContext, CarouselStyles};
use gpui::{App, Window};
use std::{fmt, sync::Arc};

type StylesFn =
    dyn Fn(&CarouselStyleContext, &mut Window, &mut App) -> CarouselStyles + Send + Sync;

#[derive(Clone)]
pub struct CarouselStyleResolver(Arc<StylesFn>);
impl CarouselStyleResolver {
    pub fn new(
        resolver: impl Fn(&CarouselStyleContext, &mut Window, &mut App) -> CarouselStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }
    pub fn resolve(
        &self,
        context: &CarouselStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> CarouselStyles {
        (self.0)(context, window, cx)
    }
}
impl PartialEq for CarouselStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for CarouselStyleResolver {}
impl fmt::Debug for CarouselStyleResolver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("CarouselStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct CarouselConfig {
    pub styles: CarouselStyles,
    pub styles_fn: Option<CarouselStyleResolver>,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CarouselConfigPatch {
    pub styles: CarouselStyles,
    pub styles_fn: Option<CarouselStyleResolver>,
}
impl CarouselConfigPatch {
    pub fn resolve(&self, parent: &CarouselConfig) -> CarouselConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        CarouselConfig {
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
