// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{SpinStyleContext, SpinStyles};
use crate::icons::{IconName, IconRef};
use gpui::{AnyElement, App, IntoElement, Window};
use std::{fmt, sync::Arc};

type ContentFn = dyn Fn(&SpinStyleContext, &mut Window, &mut App) -> AnyElement + Send + Sync;
enum Source {
    Icon(IconRef),
    Content(Box<ContentFn>),
}

/// Shared icon or native factory. Cloning preserves identity for no-op scope updates.
#[derive(Clone)]
pub struct SpinIndicator {
    source: Arc<Source>,
    pub(crate) animated: bool,
}
impl SpinIndicator {
    pub fn new(icon: impl Into<IconRef>) -> Self {
        Self {
            source: Arc::new(Source::Icon(icon.into())),
            animated: false,
        }
    }
    pub fn from_content<E: IntoElement>(
        factory: impl Fn(&SpinStyleContext, &mut Window, &mut App) -> E + Send + Sync + 'static,
    ) -> Self {
        Self {
            source: Arc::new(Source::Content(Box::new(move |context, window, cx| {
                factory(context, window, cx).into_any_element()
            }))),
            animated: false,
        }
    }
    pub fn animated(mut self, animated: bool) -> Self {
        self.animated = animated;
        self
    }
    pub fn reference(&self) -> Option<&IconRef> {
        match self.source.as_ref() {
            Source::Icon(icon) => Some(icon),
            Source::Content(_) => None,
        }
    }
    pub fn render_content(
        &self,
        context: &SpinStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> Option<AnyElement> {
        match self.source.as_ref() {
            Source::Icon(_) => None,
            Source::Content(factory) => Some(factory(context, window, cx)),
        }
    }
}
impl PartialEq for SpinIndicator {
    fn eq(&self, other: &Self) -> bool {
        self.animated == other.animated && Arc::ptr_eq(&self.source, &other.source)
    }
}
impl Eq for SpinIndicator {}
impl fmt::Debug for SpinIndicator {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("SpinIndicator")
            .field("source", &Arc::as_ptr(&self.source))
            .field("animated", &self.animated)
            .finish()
    }
}
impl From<IconRef> for SpinIndicator {
    fn from(icon: IconRef) -> Self {
        Self::new(icon)
    }
}
impl From<IconName> for SpinIndicator {
    fn from(icon: IconName) -> Self {
        Self::new(icon)
    }
}

type StylesFn = dyn Fn(&SpinStyleContext, &mut Window, &mut App) -> SpinStyles + Send + Sync;
#[derive(Clone)]
pub struct SpinStyleResolver(Arc<StylesFn>);
impl SpinStyleResolver {
    pub fn new(
        resolver: impl Fn(&SpinStyleContext, &mut Window, &mut App) -> SpinStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }
    pub fn resolve(
        &self,
        context: &SpinStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> SpinStyles {
        (self.0)(context, window, cx)
    }
}
impl PartialEq for SpinStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for SpinStyleResolver {}
impl fmt::Debug for SpinStyleResolver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("SpinStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct SpinConfig {
    pub indicator: Option<SpinIndicator>,
    pub styles: SpinStyles,
    pub styles_fn: Option<SpinStyleResolver>,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SpinConfigPatch {
    pub indicator: Option<SpinIndicator>,
    pub styles: SpinStyles,
    pub styles_fn: Option<SpinStyleResolver>,
}
impl SpinConfigPatch {
    pub fn resolve(&self, parent: &SpinConfig) -> SpinConfig {
        let mut styles = parent.styles.clone();
        styles.refine(&self.styles);
        SpinConfig {
            indicator: self.indicator.clone().or_else(|| parent.indicator.clone()),
            styles,
            styles_fn: self.styles_fn.clone().or_else(|| parent.styles_fn.clone()),
        }
    }
}
