// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use super::{NotificationPlacement, NotificationType};
use crate::{
    Direction,
    theme::{AliasTokens, ComponentTheme},
};
use gpui::{
    App, Background, BoxShadow, DefiniteLength, Font, Refineable, StyleRefinement, Window,
    linear_color_stop, linear_gradient, point, px,
};
use std::{fmt, sync::Arc};

#[derive(Clone, Debug, Default, PartialEq)]
pub struct NotificationStyles {
    pub list: StyleRefinement,
    pub list_content: StyleRefinement,
    pub root: StyleRefinement,
    pub wrapper: StyleRefinement,
    pub section: StyleRefinement,
    pub icon: StyleRefinement,
    pub title: StyleRefinement,
    pub description: StyleRefinement,
    pub actions: StyleRefinement,
    pub close: StyleRefinement,
    pub progress: StyleRefinement,
}
impl NotificationStyles {
    pub(crate) fn refine(&mut self, other: &Self) {
        self.list.refine(&other.list);
        self.list_content.refine(&other.list_content);
        self.refine_card(other);
    }
    pub(super) fn refine_card(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.wrapper.refine(&other.wrapper);
        self.section.refine(&other.section);
        self.icon.refine(&other.icon);
        self.title.refine(&other.title);
        self.description.refine(&other.description);
        self.actions.refine(&other.actions);
        self.close.refine(&other.close);
        self.progress.refine(&other.progress);
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NotificationStyleContext {
    pub kind: Option<NotificationType>,
    pub placement: NotificationPlacement,
    pub direction: Direction,
    pub motion: bool,
    pub closing: bool,
    pub stacked: bool,
    pub closable: bool,
    pub show_progress: bool,
}
type StylesFn =
    dyn Fn(&NotificationStyleContext, &mut Window, &mut App) -> NotificationStyles + Send + Sync;
#[derive(Clone)]
pub struct NotificationStyleResolver(Arc<StylesFn>);
impl NotificationStyleResolver {
    pub fn new(
        f: impl Fn(&NotificationStyleContext, &mut Window, &mut App) -> NotificationStyles
        + Send
        + Sync
        + 'static,
    ) -> Self {
        Self(Arc::new(f))
    }
    pub fn resolve(
        &self,
        context: &NotificationStyleContext,
        window: &mut Window,
        cx: &mut App,
    ) -> NotificationStyles {
        (self.0)(context, window, cx)
    }
}
impl PartialEq for NotificationStyleResolver {
    fn eq(&self, other: &Self) -> bool {
        Arc::ptr_eq(&self.0, &other.0)
    }
}
impl Eq for NotificationStyleResolver {}
impl fmt::Debug for NotificationStyleResolver {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("NotificationStyleResolver")
            .field(&Arc::as_ptr(&self.0))
            .finish()
    }
}

pub struct NotificationTheme;
#[derive(Clone, Debug, PartialEq)]
pub struct NotificationTokens {
    pub width: DefiniteLength,
    pub z_index_popup: usize,
    pub progress_bg: Background,
    pub color_success_bg: Option<Background>,
    pub color_error_bg: Option<Background>,
    pub color_info_bg: Option<Background>,
    pub color_warning_bg: Option<Background>,
    pub(super) globals: Arc<AliasTokens>,
    pub(super) font: Font,
    pub(super) shadows: Arc<[BoxShadow]>,
}
impl NotificationTokens {
    pub fn globals(&self) -> &AliasTokens {
        &self.globals
    }
    pub(super) fn background(&self, kind: Option<NotificationType>) -> Background {
        match kind {
            Some(NotificationType::Success) => self.color_success_bg,
            Some(NotificationType::Error) => self.color_error_bg,
            Some(NotificationType::Info) => self.color_info_bg,
            Some(NotificationType::Warning) => self.color_warning_bg,
            None => None,
        }
        .unwrap_or_else(|| self.globals.color_bg_elevated.to_hsla().into())
    }
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct NotificationTokenPatch {
    pub width: Option<DefiniteLength>,
    pub z_index_popup: Option<usize>,
    pub progress_bg: Option<Background>,
    pub color_success_bg: Option<Background>,
    pub color_error_bg: Option<Background>,
    pub color_info_bg: Option<Background>,
    pub color_warning_bg: Option<Background>,
}
impl ComponentTheme for NotificationTheme {
    type Tokens = NotificationTokens;
    type Patch = NotificationTokenPatch;
    fn default_tokens(g: &AliasTokens) -> NotificationTokens {
        NotificationTokens {
            width: px(384.).into(),
            z_index_popup: (g.z_index_popup_base.max(0.) as usize).saturating_add(1050),
            progress_bg: linear_gradient(
                90.,
                linear_color_stop(g.color_primary_border_hover.to_hsla(), 0.),
                linear_color_stop(g.color_primary.to_hsla(), 1.),
            ),
            color_success_bg: None,
            color_error_bg: None,
            color_info_bg: None,
            color_warning_bg: None,
            globals: Arc::new(g.clone()),
            font: g.font_family.to_gpui_font(),
            shadows: g
                .box_shadow
                .iter()
                .map(|s| BoxShadow {
                    color: s.color.to_hsla(),
                    offset: point(px(s.x as f32), px(s.y as f32)),
                    blur_radius: px((s.blur / 2.) as f32),
                    spread_radius: px(s.spread as f32),
                    inset: s.inset,
                })
                .collect(),
        }
    }
    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        NotificationTokenPatch {
            width: local.width.or(parent.width),
            z_index_popup: local.z_index_popup.or(parent.z_index_popup),
            progress_bg: local.progress_bg.or(parent.progress_bg),
            color_success_bg: local.color_success_bg.or(parent.color_success_bg),
            color_error_bg: local.color_error_bg.or(parent.color_error_bg),
            color_info_bg: local.color_info_bg.or(parent.color_info_bg),
            color_warning_bg: local.color_warning_bg.or(parent.color_warning_bg),
        }
    }
    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        if let Some(width) = patch.width {
            tokens.width = width;
        }
        if let Some(z) = patch.z_index_popup {
            tokens.z_index_popup = z;
        }
        if let Some(bg) = patch.progress_bg {
            tokens.progress_bg = bg;
        }
        if let Some(bg) = patch.color_success_bg {
            tokens.color_success_bg = Some(bg);
        }
        if let Some(bg) = patch.color_error_bg {
            tokens.color_error_bg = Some(bg);
        }
        if let Some(bg) = patch.color_info_bg {
            tokens.color_info_bg = Some(bg);
        }
        if let Some(bg) = patch.color_warning_bg {
            tokens.color_warning_bg = Some(bg);
        }
    }
}
