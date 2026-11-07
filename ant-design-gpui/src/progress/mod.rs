// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Controlled native progress indicators. Keep the element ID stable between renders.
//! Values belong to the application; transitions and geometry caches belong to the element.

mod config;
mod element;
mod geometry;
mod paint;
mod state;
mod style;
mod view;

pub use config::{ProgressConfig, ProgressConfigPatch, ProgressStyleResolver};
pub use style::{ProgressTheme, ProgressTokenPatch, ProgressTokens};

use crate::{ConfigScope, Direction, Size, behavior::Accessibility};
use gpui::{
    AnyElement, App, ElementId, Hsla, IntoElement, Length, Refineable, SharedString,
    StyleRefinement, Styled, Window,
};
use std::{rc::Rc, sync::Arc};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ProgressKind {
    #[default]
    Line,
    Circle,
    Dashboard,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ProgressStatus {
    #[default]
    Normal,
    Active,
    Exception,
    Success,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ProgressLinecap {
    #[default]
    Round,
    Butt,
    Square,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ProgressGapPlacement {
    Top,
    #[default]
    Bottom,
    Start,
    End,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ProgressAlign {
    Start,
    Center,
    #[default]
    End,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ProgressInfoPosition {
    Inner,
    #[default]
    Outer,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ProgressPercentPosition {
    pub align: ProgressAlign,
    pub position: ProgressInfoPosition,
}

/// A line's dimensions describe its body (including the outer label). For steps,
/// width describes one step. Circular indicators use a single diameter.
#[derive(Clone, Debug, Default, PartialEq)]
pub enum ProgressSize {
    Small,
    #[default]
    Medium,
    Pixels(f32),
    Dimensions {
        width: Option<Length>,
        height: f32,
    },
}
impl From<Size> for ProgressSize {
    fn from(size: Size) -> Self {
        match size {
            Size::Small => Self::Small,
            _ => Self::Medium,
        }
    }
}
impl From<f32> for ProgressSize {
    fn from(size: f32) -> Self {
        Self::Pixels(size)
    }
}
impl From<i32> for ProgressSize {
    fn from(size: i32) -> Self {
        Self::Pixels(size as f32)
    }
}
impl From<[f32; 2]> for ProgressSize {
    fn from([width, height]: [f32; 2]) -> Self {
        Self::Dimensions {
            width: Some(gpui::px(width).into()),
            height,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ProgressSteps {
    pub count: usize,
    pub gap: f32,
}
impl From<usize> for ProgressSteps {
    fn from(count: usize) -> Self {
        Self { count, gap: 2. }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ProgressSuccess {
    pub percent: f64,
    pub color: Option<Hsla>,
}
impl ProgressSuccess {
    pub fn new(percent: f64) -> Self {
        Self {
            percent,
            color: None,
        }
    }
    pub fn color(mut self, color: impl Into<Hsla>) -> Self {
        self.color = Some(color.into());
        self
    }
}
impl From<f64> for ProgressSuccess {
    fn from(percent: f64) -> Self {
        Self::new(percent)
    }
}

/// Immutable, shared sRGB stops. Positions are fractions of the gradient, not percentages.
/// Nonfinite positions are discarded; the remaining positions are clamped and sorted.
#[derive(Clone, Debug, PartialEq)]
pub struct ProgressGradient {
    stops: Arc<[(f32, Hsla)]>,
    angle: Option<f32>,
}
impl ProgressGradient {
    pub fn new<C: Into<Hsla>>(stops: impl IntoIterator<Item = (f32, C)>) -> Self {
        let mut stops: Vec<_> = stops
            .into_iter()
            .filter_map(|(position, color)| {
                position
                    .is_finite()
                    .then(|| (position.clamp(0., 1.), color.into()))
            })
            .collect();
        stops.sort_by(|a, b| a.0.total_cmp(&b.0));
        Self {
            stops: stops.into(),
            angle: None,
        }
    }
    pub fn between(from: impl Into<Hsla>, to: impl Into<Hsla>) -> Self {
        Self::new([(0., from.into()), (1., to.into())])
    }
    /// CSS-compatible degrees: 0 points upward and 90 points right. Only lines use this.
    pub fn angle(mut self, degrees: f32) -> Self {
        self.angle = degrees.is_finite().then(|| degrees.rem_euclid(360.));
        self
    }
    pub fn stops(&self) -> &[(f32, Hsla)] {
        &self.stops
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum ProgressStroke {
    Solid(Hsla),
    Gradient(ProgressGradient),
    Steps(Arc<[Hsla]>),
}
impl From<Hsla> for ProgressStroke {
    fn from(color: Hsla) -> Self {
        Self::Solid(color)
    }
}
impl From<crate::theme::Color> for ProgressStroke {
    fn from(color: crate::theme::Color) -> Self {
        Self::Solid(color.into())
    }
}
impl From<ProgressGradient> for ProgressStroke {
    fn from(gradient: ProgressGradient) -> Self {
        Self::Gradient(gradient)
    }
}
impl ProgressStroke {
    pub fn steps<C: Into<Hsla>>(colors: impl IntoIterator<Item = C>) -> Self {
        Self::Steps(colors.into_iter().map(Into::into).collect())
    }
}

/// Native semantic styles. Rail/track backgrounds also style painted circular paths.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ProgressStyles {
    pub root: StyleRefinement,
    pub body: StyleRefinement,
    pub rail: StyleRefinement,
    pub track: StyleRefinement,
    pub indicator: StyleRefinement,
}
impl ProgressStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.body.refine(&other.body);
        self.rail.refine(&other.rail);
        self.track.refine(&other.track);
        self.indicator.refine(&other.indicator);
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct ProgressStyleContext {
    pub kind: ProgressKind,
    pub status: ProgressStatus,
    pub percent: f64,
    pub success_percent: Option<f64>,
    pub size: ProgressSize,
    pub steps: Option<ProgressSteps>,
    pub show_info: bool,
    pub percent_position: ProgressPercentPosition,
    pub direction: Direction,
    pub motion: bool,
}

type FormatFn = Rc<dyn Fn(f64, f64, &mut Window, &mut App) -> AnyElement>;
type TextFormatFn = Rc<dyn Fn(f64, f64) -> SharedString>;
type StylesFn = Box<dyn Fn(&ProgressStyleContext, &mut Window, &mut App) -> ProgressStyles>;

#[derive(IntoElement)]
pub struct Progress {
    id: ElementId,
    scope: Option<ConfigScope>,
    kind: ProgressKind,
    percent: f64,
    status: Option<ProgressStatus>,
    size: ProgressSize,
    stroke: Option<ProgressStroke>,
    rail_color: Option<Hsla>,
    stroke_width: Option<f32>,
    linecap: ProgressLinecap,
    success: Option<ProgressSuccess>,
    steps: Option<ProgressSteps>,
    rounding: Option<Box<dyn Fn(f64) -> f64>>,
    gap_degree: Option<f32>,
    gap_placement: ProgressGapPlacement,
    show_info: bool,
    percent_position: ProgressPercentPosition,
    format: Option<TextFormatFn>,
    format_content: Option<FormatFn>,
    // GPUI refinements are large. Allocate overrides only when supplied so
    // ordinary builders can safely compose in arrays on native GUI stacks.
    styles: Option<Box<ProgressStyles>>,
    styles_fn: Option<StylesFn>,
    style: Option<Box<StyleRefinement>>,
    accessibility: Accessibility,
}
impl Progress {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            kind: ProgressKind::Line,
            percent: 0.,
            status: None,
            size: ProgressSize::Medium,
            stroke: None,
            rail_color: None,
            stroke_width: None,
            linecap: ProgressLinecap::Round,
            success: None,
            steps: None,
            rounding: None,
            gap_degree: None,
            gap_placement: ProgressGapPlacement::Bottom,
            show_info: true,
            percent_position: ProgressPercentPosition::default(),
            format: None,
            format_content: None,
            styles: None,
            styles_fn: None,
            style: None,
            accessibility: Accessibility::default(),
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn kind(mut self, kind: ProgressKind) -> Self {
        self.kind = kind;
        self
    }
    pub fn percent(mut self, percent: f64) -> Self {
        self.percent = percent;
        self
    }
    pub fn status(mut self, status: ProgressStatus) -> Self {
        self.status = Some(status);
        self
    }
    pub fn size(mut self, size: impl Into<ProgressSize>) -> Self {
        self.size = size.into();
        self
    }
    pub fn stroke_color(mut self, stroke: impl Into<ProgressStroke>) -> Self {
        self.stroke = Some(stroke.into());
        self
    }
    pub fn rail_color(mut self, color: impl Into<Hsla>) -> Self {
        self.rail_color = Some(color.into());
        self
    }
    /// Circular stroke width as a percentage of the diameter. Use size for line thickness.
    pub fn stroke_width(mut self, width: f32) -> Self {
        self.stroke_width = Some(width);
        self
    }
    pub fn stroke_linecap(mut self, cap: ProgressLinecap) -> Self {
        self.linecap = cap;
        self
    }
    pub fn success(mut self, success: impl Into<ProgressSuccess>) -> Self {
        self.success = Some(success.into());
        self
    }
    pub fn steps(mut self, steps: impl Into<ProgressSteps>) -> Self {
        self.steps = Some(steps.into()).filter(|steps| steps.count > 0);
        self
    }
    /// Custom rounding applies to line steps, matching the reference.
    pub fn rounding(mut self, rounding: impl Fn(f64) -> f64 + 'static) -> Self {
        self.rounding = Some(Box::new(rounding));
        self
    }
    pub fn gap_degree(mut self, degrees: f32) -> Self {
        self.gap_degree = Some(degrees);
        self
    }
    pub fn gap_placement(mut self, placement: ProgressGapPlacement) -> Self {
        self.gap_placement = placement;
        self
    }
    pub fn show_info(mut self, show: bool) -> Self {
        self.show_info = show;
        self
    }
    pub fn percent_position(mut self, position: ProgressPercentPosition) -> Self {
        self.percent_position = position;
        self
    }
    pub fn format(mut self, format: impl Fn(f64, f64) -> SharedString + 'static) -> Self {
        self.format = Some(Rc::new(format));
        self.format_content = None;
        self
    }
    pub fn format_content<E: IntoElement>(
        mut self,
        format: impl Fn(f64, f64, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.format_content = Some(Rc::new(move |p, s, w, cx| {
            format(p, s, w, cx).into_any_element()
        }));
        self.format = None;
        self
    }
    pub fn styles(mut self, styles: ProgressStyles) -> Self {
        self.styles = Some(Box::new(styles));
        self
    }
    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&ProgressStyleContext, &mut Window, &mut App) -> ProgressStyles + 'static,
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
impl Styled for Progress {
    fn style(&mut self) -> &mut StyleRefinement {
        self.style.get_or_insert_default()
    }
}

#[cfg(test)]
mod tests;
