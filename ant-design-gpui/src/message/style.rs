// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use std::{fmt, sync::Arc, time::Duration};

use gpui::{App, BoxShadow, Edges, Font, Hsla, Refineable, StyleRefinement, Window, point, px};

use super::MessageType;
use crate::{
    Direction,
    theme::{AliasTokens, Color, ComponentTheme, CubicBezier, ResolvedTheme},
};

/// Native refinements for the holder, content list, notice card and its contents.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MessageStyles {
    pub list: StyleRefinement,
    pub list_content: StyleRefinement,
    pub root: StyleRefinement,
    pub wrapper: StyleRefinement,
    pub icon: StyleRefinement,
    pub title: StyleRefinement,
}

impl MessageStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.list.refine(&other.list);
        self.list_content.refine(&other.list_content);
        self.root.refine(&other.root);
        self.wrapper.refine(&other.wrapper);
        self.icon.refine(&other.icon);
        self.title.refine(&other.title);
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MessageStyleContext {
    pub kind: Option<MessageType>,
    pub direction: Direction,
    pub motion: bool,
    pub closing: bool,
    pub stacked: bool,
}

type StylesFn = dyn Fn(&MessageStyleContext, &mut Window, &mut App) -> MessageStyles + Send + Sync;

/// A shared semantic style factory; cloning preserves its configuration identity.
#[derive(Clone)]
pub struct MessageStyleResolver(Arc<StylesFn>);

impl MessageStyleResolver {
    pub fn new(
        resolver: impl Fn(&MessageStyleContext, &mut Window, &mut App) -> MessageStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(resolver))
    }

    pub fn resolve(
        &self,
        context: &MessageStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> MessageStyles {
        (self.0)(context, window, cx)
    }
}

impl PartialEq for MessageStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for MessageStyleResolver {}
impl fmt::Debug for MessageStyleResolver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("MessageStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

pub struct MessageTheme;

#[derive(Clone, Debug, PartialEq)]
pub struct MessageTokens {
    pub z_index_popup: usize,
    pub content_bg: Color,
    pub content_padding: Edges<f64>,
    globals: Arc<AliasTokens>,
    font: Font,
    shadows: Arc<[BoxShadow]>,
    placeholder_shadows: Arc<[BoxShadow]>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct MessageTokenPatch {
    pub z_index_popup: Option<usize>,
    pub content_bg: Option<Color>,
    pub content_padding: Option<Edges<f64>>,
}

impl ComponentTheme for MessageTheme {
    type Tokens = MessageTokens;
    type Patch = MessageTokenPatch;

    fn default_tokens(g: &AliasTokens) -> MessageTokens {
        let vertical = ((g.control_height_lg - g.font_size * g.line_height) / 2.).max(0.);
        let shadows = |values: &crate::theme::ShadowList| {
            values
                .iter()
                .map(|s| BoxShadow {
                    color: s.color.to_hsla(),
                    offset: point(px(s.x as f32), px(s.y as f32)),
                    // CSS box-shadow blur is twice the Gaussian standard deviation.
                    // GPUI's shadow shader takes that standard deviation directly.
                    blur_radius: px((s.blur / 2.) as f32),
                    spread_radius: px(s.spread as f32),
                    inset: s.inset,
                })
                .collect()
        };
        MessageTokens {
            z_index_popup: (g.z_index_popup_base.max(0.) as usize).saturating_add(1010),
            content_bg: g.color_bg_elevated,
            content_padding: Edges {
                top: vertical,
                right: g.padding_sm,
                bottom: vertical,
                left: g.padding_sm,
            },
            globals: Arc::new(g.clone()),
            font: g.font_family.to_gpui_font(),
            shadows: shadows(&g.box_shadow),
            placeholder_shadows: shadows(&g.box_shadow_tertiary),
        }
    }

    fn merge(parent: &MessageTokenPatch, local: &MessageTokenPatch) -> MessageTokenPatch {
        MessageTokenPatch {
            z_index_popup: local.z_index_popup.or(parent.z_index_popup),
            content_bg: local.content_bg.or(parent.content_bg),
            content_padding: local.content_padding.or(parent.content_padding),
        }
    }

    fn apply(tokens: &mut MessageTokens, patch: &MessageTokenPatch) {
        if let Some(value) = patch.z_index_popup {
            tokens.z_index_popup = value;
        }
        if let Some(value) = patch.content_bg
            && value.a.is_finite()
        {
            tokens.content_bg = value.with_alpha(value.a);
        }
        if let Some(value) = &patch.content_padding {
            let valid = |value: f64, fallback: f64| {
                if value.is_finite() && (value as f32).is_finite() {
                    value.max(0.)
                } else {
                    fallback
                }
            };
            tokens.content_padding = Edges {
                top: valid(value.top, tokens.content_padding.top),
                right: valid(value.right, tokens.content_padding.right),
                bottom: valid(value.bottom, tokens.content_padding.bottom),
                left: valid(value.left, tokens.content_padding.left),
            };
        }
    }
}

impl MessageTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub fn font(&self) -> &Font {
        &self.font
    }
    pub fn shadows(&self) -> &Arc<[BoxShadow]> {
        &self.shadows
    }
    pub fn placeholder_shadows(&self) -> &Arc<[BoxShadow]> {
        &self.placeholder_shadows
    }
}

#[derive(Clone, Debug, PartialEq)]
pub struct MessageStyle {
    pub background: Hsla,
    pub color: Hsla,
    pub title_color: Hsla,
    pub icon_color: Hsla,
    pub padding: Edges<f64>,
    pub radius: f64,
    pub font_size: f64,
    pub icon_size: f64,
    pub line_height: f64,
    pub icon_gap: f64,
    pub gap: f64,
    pub edge_margin: f64,
    pub z_index: usize,
    pub motion_duration: Duration,
    pub motion_curve: CubicBezier,
    pub motion_offset: f64,
}

impl MessageStyle {
    pub fn resolve(theme: &ResolvedTheme, kind: Option<MessageType>) -> Self {
        Self::from_tokens(&theme.component::<MessageTheme>(), kind)
    }

    pub fn from_tokens(tokens: &MessageTokens, kind: Option<MessageType>) -> Self {
        let g = tokens.globals();
        let icon_color = match kind {
            Some(MessageType::Success) => g.color_success,
            Some(MessageType::Warning) => g.color_warning,
            Some(MessageType::Error) => g.color_error,
            Some(MessageType::Info | MessageType::Loading) => g.color_info,
            None => g.color_text,
        };
        Self {
            background: tokens.content_bg.into(),
            color: g.color_text.into(),
            title_color: g.color_text_heading.into(),
            icon_color: icon_color.into(),
            padding: tokens.content_padding,
            radius: g.border_radius_lg,
            font_size: g.font_size,
            icon_size: g.font_size_lg,
            line_height: g.line_height,
            icon_gap: g.margin_xs,
            gap: g.margin,
            edge_margin: g.margin_lg,
            z_index: tokens.z_index_popup,
            motion_duration: g.motion_duration_mid,
            motion_curve: g.motion_ease_in_out,
            motion_offset: 64.,
        }
    }
}
