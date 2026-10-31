// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{fmt, sync::Arc};

use gpui::{App, Window};

use super::{DescriptionsStyleContext, DescriptionsStyles};

type StylesFn =
    dyn Fn(&DescriptionsStyleContext, &mut Window, &mut App) -> DescriptionsStyles + Send + Sync;

/// Clone this wrapper when retaining a resolver across scope updates.
#[derive(Clone)]
pub struct DescriptionsStyleResolver(Arc<StylesFn>);

impl DescriptionsStyleResolver {
    pub fn new(
        resolver: impl Fn(&DescriptionsStyleContext, &mut Window, &mut App) -> DescriptionsStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }

    pub fn resolve(
        &self,
        context: &DescriptionsStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> DescriptionsStyles {
        (self.0)(context, window, cx)
    }
}

impl PartialEq for DescriptionsStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}

impl Eq for DescriptionsStyleResolver {}

impl fmt::Debug for DescriptionsStyleResolver {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_tuple("DescriptionsStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

/// Semantic style defaults; items and layout remain owned by each component.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DescriptionsConfig {
    pub styles: DescriptionsStyles,
    pub styles_fn: Option<DescriptionsStyleResolver>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct DescriptionsConfigPatch {
    pub styles: DescriptionsStyles,
    pub styles_fn: Option<DescriptionsStyleResolver>,
}

impl DescriptionsConfigPatch {
    pub fn resolve(&self, parent: &DescriptionsConfig) -> DescriptionsConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        DescriptionsConfig {
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
