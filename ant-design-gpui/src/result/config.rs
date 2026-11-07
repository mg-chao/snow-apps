// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{fmt, sync::Arc};

use gpui::{App, Window};

use super::{ResultStyleContext, ResultStyles};

type StylesFn = dyn Fn(&ResultStyleContext, &mut Window, &mut App) -> ResultStyles + Send + Sync;

/// Retain or clone this wrapper to preserve no-op configuration updates.
#[derive(Clone)]
pub struct ResultStyleResolver(Arc<StylesFn>);

impl ResultStyleResolver {
    pub fn new(
        resolver: impl Fn(&ResultStyleContext, &mut Window, &mut App) -> ResultStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }

    pub fn resolve(
        &self,
        context: &ResultStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> ResultStyles {
        (self.0)(context, window, cx)
    }
}

impl PartialEq for ResultStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

impl Eq for ResultStyleResolver {}

impl fmt::Debug for ResultStyleResolver {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("ResultStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

/// Scoped semantic styling. Content and status remain owned by each Result.
#[derive(Clone, Default, PartialEq)]
pub struct ResultConfig {
    pub styles: ResultStyles,
    pub styles_fn: Option<ResultStyleResolver>,
}

#[derive(Clone, Default, PartialEq)]
pub struct ResultConfigPatch {
    pub styles: ResultStyles,
    pub styles_fn: Option<ResultStyleResolver>,
}

impl ResultConfigPatch {
    pub fn resolve(&self, parent: &ResultConfig) -> ResultConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        ResultConfig {
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
