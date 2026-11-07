// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native loading indicators with retained delay and progress, keyed by a stable ID.

mod config;
mod element;
mod geometry;
mod paint;
mod state;
mod style;
mod view;

pub use config::{SpinConfig, SpinConfigPatch, SpinIndicator, SpinStyleResolver};
pub use style::{SpinStyle, SpinTheme, SpinTokenPatch, SpinTokens};

use crate::{ConfigScope, Direction, Size, behavior::Accessibility};
use gpui::{
    AnyElement, App, ElementId, IntoElement, ParentElement, Refineable, SharedString,
    StyleRefinement, Styled, Window,
};
use std::time::Duration;

/// A supplied percentage, or reference-compatible estimated progress.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum SpinPercent {
    Value(f64),
    Auto,
}

impl From<f64> for SpinPercent {
    fn from(value: f64) -> Self {
        Self::Value(value)
    }
}
impl From<f32> for SpinPercent {
    fn from(value: f32) -> Self {
        Self::Value(value.into())
    }
}
impl From<i32> for SpinPercent {
    fn from(value: i32) -> Self {
        Self::Value(value.into())
    }
}

/// Semantic native styles; missing properties inherit the preceding layer.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SpinStyles {
    pub root: StyleRefinement,
    pub section: StyleRefinement,
    pub indicator: StyleRefinement,
    pub description: StyleRefinement,
    pub container: StyleRefinement,
}

impl SpinStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.section.refine(&other.section);
        self.indicator.refine(&other.indicator);
        self.description.refine(&other.description);
        self.container.refine(&other.container);
    }
}

/// Effective inputs supplied to styles and custom indicator factories.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SpinStyleContext {
    pub size: Size,
    pub spinning: bool,
    pub fullscreen: bool,
    pub has_children: bool,
    pub direction: Direction,
    /// Unclamped finite effective progress. `None` is indeterminate.
    pub percent: Option<f64>,
    pub motion: bool,
    /// Clockwise rotation in radians for an explicitly animated custom indicator.
    pub rotation: f32,
}

enum Content {
    Text(SharedString),
    Element(AnyElement),
}

impl Content {
    fn into_element(self, direction: Direction) -> AnyElement {
        match self {
            Self::Text(text) => crate::behavior::text::normal_paragraph_text(text, direction),
            Self::Element(element) => element,
        }
    }
}

type StylesFn = Box<dyn Fn(&SpinStyleContext, &mut Window, &mut App) -> SpinStyles>;

#[derive(IntoElement)]
pub struct Spin {
    id: ElementId,
    scope: Option<ConfigScope>,
    spinning: bool,
    delay: Duration,
    size: Option<Size>,
    percent: Option<SpinPercent>,
    fullscreen: bool,
    description: Option<Content>,
    children: Vec<AnyElement>,
    has_children: bool,
    indicator: Option<SpinIndicator>,
    indicator_content: Option<AnyElement>,
    indicator_spin: Option<bool>,
    styles: SpinStyles,
    styles_fn: Option<StylesFn>,
    style: StyleRefinement,
    accessibility: Accessibility,
}

impl Spin {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            spinning: true,
            delay: Duration::ZERO,
            size: None,
            percent: None,
            fullscreen: false,
            description: None,
            children: Vec::new(),
            has_children: false,
            indicator: None,
            indicator_content: None,
            indicator_spin: None,
            styles: SpinStyles::default(),
            styles_fn: None,
            style: StyleRefinement::default(),
            accessibility: Accessibility::default(),
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn spinning(mut self, spinning: bool) -> Self {
        self.spinning = spinning;
        self
    }
    pub fn delay(mut self, delay: Duration) -> Self {
        self.delay = delay;
        self
    }
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }
    pub fn percent(mut self, percent: impl Into<SpinPercent>) -> Self {
        self.percent = Some(percent.into());
        self
    }
    pub fn fullscreen(mut self, fullscreen: bool) -> Self {
        self.fullscreen = fullscreen;
        self
    }
    pub fn description(mut self, text: impl Into<SharedString>) -> Self {
        let text = text.into();
        self.description = (!text.is_empty()).then_some(Content::Text(text));
        self
    }
    pub fn description_content(mut self, content: impl IntoElement) -> Self {
        self.description = Some(Content::Element(content.into_any_element()));
        self
    }
    pub fn indicator(mut self, indicator: impl Into<SpinIndicator>) -> Self {
        self.indicator = Some(indicator.into());
        self.indicator_content = None;
        self
    }
    pub fn indicator_content(mut self, content: impl IntoElement) -> Self {
        self.indicator_content = Some(content.into_any_element());
        self.indicator = None;
        self
    }
    /// Rotate icon references, or request frames for content using the factory's rotation context.
    pub fn indicator_spin(mut self, spin: bool) -> Self {
        self.indicator_spin = Some(spin);
        self
    }
    pub fn styles(mut self, styles: SpinStyles) -> Self {
        self.styles = styles;
        self
    }
    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&SpinStyleContext, &mut Window, &mut App) -> SpinStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Box::new(resolver));
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(label.into());
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(description.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(id.into());
        self
    }
}

impl ParentElement for Spin {
    fn extend(&mut self, elements: impl IntoIterator<Item = AnyElement>) {
        self.has_children = true;
        self.children.extend(elements);
    }
}
impl Styled for Spin {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

#[cfg(test)]
mod reference_tests;
#[cfg(test)]
mod tests;
