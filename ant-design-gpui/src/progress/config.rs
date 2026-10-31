// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{ProgressStyleContext, ProgressStyles};
use gpui::{App, Window};
use std::{fmt, sync::Arc};

type Resolver =
    dyn Fn(&ProgressStyleContext, &mut Window, &mut App) -> ProgressStyles + Send + Sync;
#[derive(Clone)]
pub struct ProgressStyleResolver(Arc<Resolver>);
impl ProgressStyleResolver {
    pub fn new(
        resolver: impl Fn(&ProgressStyleContext, &mut Window, &mut App) -> ProgressStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }
    pub fn resolve(
        &self,
        context: &ProgressStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> ProgressStyles {
        (self.0)(context, window, cx)
    }
}
impl PartialEq for ProgressStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for ProgressStyleResolver {}
impl fmt::Debug for ProgressStyleResolver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("ProgressStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ProgressConfig {
    pub styles: ProgressStyles,
    pub styles_fn: Option<ProgressStyleResolver>,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ProgressConfigPatch {
    pub styles: ProgressStyles,
    pub styles_fn: Option<ProgressStyleResolver>,
}
impl ProgressConfigPatch {
    pub fn resolve(&self, parent: &ProgressConfig) -> ProgressConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        ProgressConfig {
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
