// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native separators with optional text or custom GPUI title content.
//!
//! Build on each render with a stable ID. Titles are application-localized;
//! vertical dividers ignore titles. The control observes its configuration scope,
//! has no focus or input state, and schedules no animation frames.

mod paint;
mod style;

pub use crate::theme::LineType as DividerVariant;
pub use style::{DividerStyle, DividerTheme, DividerTokenPatch, DividerTokens};

use gpui::{
    AnyElement, App, DefiniteLength, ElementId, Entity, FontWeight, IntoElement, ParentElement,
    Refineable, RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, Window,
    accesskit, canvas, div, prelude::*, px, relative,
};

use crate::behavior::Accessibility;
use crate::{ConfigScope, Direction, Size};
use paint::RailCache;
use style::finite_length;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DividerOrientation {
    #[default]
    Horizontal,
    Vertical,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum DividerTitlePlacement {
    #[default]
    Center,
    Start,
    End,
    /// Physical left, independent of the configuration's direction.
    Left,
    /// Physical right, independent of the configuration's direction.
    Right,
}

impl DividerTitlePlacement {
    fn physical(self, direction: Direction) -> Self {
        match (self, direction) {
            (Self::Start, Direction::Ltr) | (Self::End, Direction::Rtl) => Self::Left,
            (Self::End, Direction::Ltr) | (Self::Start, Direction::Rtl) => Self::Right,
            _ => self,
        }
    }
}

#[derive(IntoElement)]
pub struct Divider {
    id: ElementId,
    scope: Option<ConfigScope>,
    orientation: DividerOrientation,
    placement: DividerTitlePlacement,
    variant: DividerVariant,
    size: Option<Size>,
    plain: bool,
    label: SharedString,
    content: Option<AnyElement>,
    title_margin: Option<DefiniteLength>,
    accessibility_label: Option<SharedString>,
    accessibility_description: Option<SharedString>,
    automation_id: Option<SharedString>,
    style: StyleRefinement,
    rail_style: Option<Box<StyleRefinement>>,
    content_style: Option<Box<StyleRefinement>>,
}

impl Divider {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            orientation: DividerOrientation::Horizontal,
            placement: DividerTitlePlacement::Center,
            variant: DividerVariant::Solid,
            size: None,
            plain: false,
            label: SharedString::default(),
            content: None,
            title_margin: None,
            accessibility_label: None,
            accessibility_description: None,
            automation_id: None,
            style: StyleRefinement::default(),
            rail_style: None,
            content_style: None,
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    /// Set a text title. An empty string restores an untitled divider.
    pub fn label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = label.into();
        self.content = None;
        self
    }

    /// Set a custom title, such as an icon and text. Supply an accessible label
    /// when the content does not have its own accessible text.
    pub fn child(mut self, content: impl IntoElement) -> Self {
        self.content = Some(content.into_any_element());
        self.label = SharedString::default();
        self
    }

    pub fn orientation(mut self, orientation: DividerOrientation) -> Self {
        self.orientation = orientation;
        self
    }

    pub fn vertical(self, vertical: bool) -> Self {
        self.orientation(if vertical {
            DividerOrientation::Vertical
        } else {
            DividerOrientation::Horizontal
        })
    }

    pub fn title_placement(mut self, placement: DividerTitlePlacement) -> Self {
        self.placement = placement;
        self
    }

    pub fn variant(mut self, variant: DividerVariant) -> Self {
        self.variant = variant;
        self
    }

    pub fn dashed(self, dashed: bool) -> Self {
        self.variant(if dashed {
            DividerVariant::Dashed
        } else {
            DividerVariant::Solid
        })
    }

    /// Only affects horizontal spacing; otherwise inherits an explicitly set scope size.
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }

    pub fn plain(mut self, plain: bool) -> Self {
        self.plain = plain;
        self
    }

    /// Distance from an edge to its title. Replaces the short rail and outer
    /// title padding; zero is meaningful. Ignored for centered titles.
    pub fn title_margin(mut self, margin: impl Into<DefiniteLength>) -> Self {
        self.title_margin = Some(margin.into());
        self
    }

    /// Refine both rails. Border color/width customize the stroke; remaining
    /// native styles (including opacity, margins and flex sizing) apply to the rail.
    pub fn rail_style(mut self, style: StyleRefinement) -> Self {
        self.rail_style = Some(Box::new(style));
        self
    }

    pub fn content_style(mut self, style: StyleRefinement) -> Self {
        self.content_style = Some(Box::new(style));
        self
    }

    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility_label = Some(label.into());
        self
    }

    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.accessibility_description = Some(description.into());
        self
    }

    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.automation_id = Some(id.into());
        self
    }
}

impl Styled for Divider {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct DividerState {
    scope: ConfigScope,
    _config: Subscription,
    rails: [RailCache; 2],
}

impl RenderOnce for Divider {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .take()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| DividerState {
            _config: scope.observe_in(window, cx, |_, _, cx| cx.notify()),
            scope: scope.clone(),
            rails: Default::default(),
        });
        state.update(cx, |state, cx| {
            if state.scope != scope {
                state._config = scope.observe_in(window, cx, |_, _, cx| cx.notify());
                state.scope = scope.clone();
            }
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<DividerTheme>();
        let vertical = self.orientation == DividerOrientation::Vertical;
        let titled = !vertical && (self.content.is_some() || !self.label.is_empty());
        let metrics = DividerStyle::from_tokens(
            &tokens,
            self.size.or(config.configured_size()),
            titled,
            self.plain,
        );
        let placement = self.placement.physical(config.direction);
        let rem = window.rem_size();
        let root_font_size = self
            .style
            .text
            .font_size
            .map_or(metrics.font_size, |length| f32::from(length.to_pixels(rem)));
        let content_font_size = self
            .content_style
            .as_ref()
            .and_then(|style| style.text.font_size)
            .map_or(root_font_size, |length| f32::from(length.to_pixels(rem)));
        let padding = metrics
            .text_padding_inline
            .to_pixels(px(content_font_size).into(), rem);
        let root_width = if vertical {
            self.style
                .border_widths
                .left
                .or(self.style.border_widths.right)
        } else {
            self.style.border_widths.top
        };
        let mut rail_style = self
            .rail_style
            .take()
            .map(|style| *style)
            .unwrap_or_default();
        let rail_width = if vertical {
            rail_style
                .border_widths
                .left
                .or(rail_style.border_widths.right)
        } else {
            rail_style.border_widths.top
        };
        let width = finite_length(
            rail_width
                .or(root_width)
                .map_or(metrics.line_width, |length| {
                    f32::from(length.to_pixels(rem))
                }),
        );
        let color = rail_style
            .border_color
            .or(self.style.border_color)
            .unwrap_or(metrics.line_color.to_hsla());
        let variant =
            rail_style
                .border_style
                .or(self.style.border_style)
                .map_or(self.variant, |style| match style {
                    gpui::BorderStyle::Solid => DividerVariant::Solid,
                    gpui::BorderStyle::Dashed => DividerVariant::Dashed,
                });
        // Strokes are painted once by the native rail painter; native background,
        // opacity and layout refinements still use GPUI's normal style pipeline.
        rail_style.border_widths = Default::default();
        if vertical {
            self.style.border_widths.left = None;
            self.style.border_widths.right = None;
        } else {
            self.style.border_widths.top = None;
        }
        let mut root = div()
            .id(self.id)
            .role(Role::Splitter)
            .aria_orientation(if vertical {
                accesskit::Orientation::Vertical
            } else {
                accesskit::Orientation::Horizontal
            })
            .relative()
            .flex_shrink_0()
            .font(tokens.font())
            .text_size(px(metrics.font_size))
            .font_weight(FontWeight(metrics.font_weight))
            .line_height(relative(metrics.line_height))
            .text_color(metrics.text_color);
        if vertical {
            root = root
                .w(px(width))
                .h(px(finite_length(root_font_size * 0.9)))
                .mx(px(metrics.vertical_margin_inline))
                .top(px(-finite_length(root_font_size * 0.06)));
        } else {
            root = root.w_full().min_w_full().my(px(metrics.margin_block));
            if titled {
                root = root.flex().items_center().whitespace_nowrap().text_center();
            } else {
                root = root.h(px(width));
            }
        }
        root.style().refine(&self.style);
        if let Some(label) = self.accessibility_label.or_else(|| {
            titled
                .then_some(self.label.clone())
                .filter(|label| !label.is_empty())
        }) {
            root = root.aria_label(label);
        }
        if let Some(description) = self.accessibility_description {
            root = root.aria_description(description);
        }
        if titled {
            let edge_margin = self
                .title_margin
                .filter(|_| placement != DividerTitlePlacement::Center);
            let ratio = metrics.orientation_margin;
            let (left, right) = match placement {
                DividerTitlePlacement::Left if edge_margin.is_some() => (0., 1.),
                DividerTitlePlacement::Right if edge_margin.is_some() => (1., 0.),
                DividerTitlePlacement::Left => (ratio, 1. - ratio),
                DividerTitlePlacement::Right => (1. - ratio, ratio),
                _ => (0.5, 0.5),
            };
            let mut content = div()
                .id("content")
                .flex_shrink_0()
                .whitespace_nowrap()
                .px(px(finite_length(f32::from(padding))));
            if let Some(margin) = edge_margin {
                content = if placement == DividerTitlePlacement::Left {
                    content.pl(px(0.)).ml(margin)
                } else {
                    content.pr(px(0.)).mr(margin)
                };
            }
            if let Some(style) = self.content_style {
                content.style().refine(&style);
            }
            content = match self.content {
                Some(child) => content.child(child),
                None => content.child(self.label),
            };
            let appearance = RailAppearance {
                vertical: false,
                variant,
                width,
                color,
            };
            root = root
                .child(rail(state.clone(), 0, appearance, &rail_style, Some(left)))
                .child(content)
                .child(rail(state, 1, appearance, &rail_style, Some(right)));
        } else {
            root = root.child(rail(
                state,
                0,
                RailAppearance {
                    vertical,
                    variant,
                    width,
                    color,
                },
                &rail_style,
                None,
            ));
        }
        Accessibility {
            automation_id: self.automation_id,
            ..Default::default()
        }
        .apply(root)
    }
}

#[derive(Clone, Copy)]
struct RailAppearance {
    vertical: bool,
    variant: DividerVariant,
    width: f32,
    color: gpui::Hsla,
}

fn rail(
    state: Entity<DividerState>,
    index: usize,
    appearance: RailAppearance,
    style: &StyleRefinement,
    weight: Option<f32>,
) -> gpui::Canvas<()> {
    let mut rail = canvas(
        |_, _, _| (),
        move |bounds, (), window, cx| {
            state.update(cx, |state, _| {
                state.rails[index].paint(
                    bounds,
                    appearance.vertical,
                    appearance.variant,
                    px(appearance.width),
                    appearance.color,
                    window,
                )
            });
        },
    );
    rail = if let Some(weight) = weight {
        rail.h(px(appearance.width))
            .flex_basis(px(0.))
            .flex_grow(weight)
            .min_w(px(0.))
    } else {
        rail.absolute().inset_0()
    };
    rail.style().refine(style);
    rail
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod reference_tests;

#[cfg(test)]
mod style_tests;
