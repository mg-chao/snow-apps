// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native tooltips. The trigger keeps its layout, focus, and activation behavior.
mod style;
pub use style::{TooltipStyle, TooltipTheme, TooltipTokenPatch, TooltipTokens};

use crate::popup::{
    Popup, PopupColor, PopupHandle,
    component::{Slot, popup_methods},
};
use gpui::{
    App, ElementId, IntoElement, Refineable, RenderOnce, Role, SharedString, StyleRefinement,
    Window, div, prelude::*, px, relative,
};

#[derive(IntoElement)]
pub struct Tooltip {
    popup: Popup,
    title: Option<Slot>,
    color: Option<PopupColor>,
    unique: Option<bool>,
    content_style: StyleRefinement,
}
impl Tooltip {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            popup: Popup::new(id).accessibility_role(Role::Tooltip),
            title: None,
            color: None,
            unique: None,
            content_style: StyleRefinement::default(),
        }
    }
    popup_methods!();
    /// Share one surface with other unique tooltips in the same configuration scope.
    pub fn unique(mut self, unique: bool) -> Self {
        self.unique = Some(unique);
        self
    }
    /// Empty text suppresses the tooltip without changing its trigger.
    pub fn title(mut self, title: impl Into<SharedString>) -> Self {
        self.title = Slot::text(title);
        self
    }
    pub fn title_content<E: IntoElement>(
        mut self,
        content: impl Fn(&PopupHandle, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.title = Some(Slot::content(content));
        self
    }
    pub fn color(mut self, color: impl Into<PopupColor>) -> Self {
        self.color = Some(color.into());
        self
    }
    pub fn content_style(mut self, style: StyleRefinement) -> Self {
        self.content_style = style;
        self
    }
}
impl RenderOnce for Tooltip {
    fn render(self, _: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.popup.resolve_scope(cx);
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<TooltipTheme>();
        let style = TooltipStyle::from_tokens(&tokens, self.color, self.popup.placement_value());
        let g = tokens.globals();
        let unique_group = match self.unique {
            Some(false) => None,
            Some(true) => Some(
                scope
                    .tooltip_unique_group(cx)
                    .unwrap_or_else(|| scope.identity()),
            ),
            None => scope.tooltip_unique_group(cx),
        };
        let mut body = div()
            .min_w(px(style.min_width as f32))
            .max_w(px(style.max_width as f32))
            .min_h(px(style.min_height as f32))
            .py(px(style.padding_block as f32))
            .px(px(style.padding_inline as f32))
            .font(tokens.font().clone())
            .text_size(px(style.font_size as f32))
            .line_height(relative(style.line_height as f32))
            .text_color(style.foreground);
        let popup = self
            .popup
            .motion_duration(g.motion_duration_fast)
            .defaults(config.tooltip.popup, tokens.z_index_popup)
            .unique_group(unique_group)
            .appearance(
                body.style().clone(),
                style.background,
                px(style.radius as f32),
                g,
                g.border_radius_outer.min(4.),
            )
            .label_default(self.title.as_ref().and_then(|title| title.text.clone()))
            .disabled_when(self.title.is_none());
        let title = self.title;
        let content_style = self.content_style;
        popup.content(move |handle, window, cx| {
            let mut content =
                div().children(title.as_ref().map(|title| title.build(handle, window, cx)));
            content.style().refine(&content_style);
            content
        })
    }
}
