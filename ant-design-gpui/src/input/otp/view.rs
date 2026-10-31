// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Visual defaults adapted from Ant Design 6.6.7 (MIT).

use std::sync::Arc;

use gpui::{
    AnyElement, App, Context, ElementId, Entity, Focusable, IntoElement, MouseButton, Refineable,
    RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, Window, accesskit, div,
    prelude::*, px,
};

use super::OtpState;
use crate::{
    ConfigScope, Direction, Input, Size, ValidationStatus, Variant,
    behavior::Accessibility,
    input::{CommitReason, InputFrame, InputInteraction, InputStyle, InputTheme, actions},
    theme::{AliasTokens, ComponentTheme},
};

pub struct OtpTheme;
#[derive(Clone, Debug, PartialEq)]
pub struct OtpTokens {
    pub gap: f64,
    /// Zero selects the font's intrinsic one-character input width.
    pub cell_width: f64,
    pub cell_width_sm: f64,
    pub cell_width_lg: f64,
    pub padding_inline: f64,
    pub padding_inline_sm: f64,
    pub padding_inline_lg: f64,
    pub(crate) globals: Arc<AliasTokens>,
}
#[derive(Clone, Debug, Default, PartialEq)]
pub struct OtpTokenPatch {
    pub gap: Option<f64>,
    pub cell_width: Option<f64>,
    pub cell_width_sm: Option<f64>,
    pub cell_width_lg: Option<f64>,
    pub padding_inline: Option<f64>,
    pub padding_inline_sm: Option<f64>,
    pub padding_inline_lg: Option<f64>,
}
impl ComponentTheme for OtpTheme {
    type Tokens = OtpTokens;
    type Patch = OtpTokenPatch;
    fn default_tokens(g: &AliasTokens) -> Self::Tokens {
        OtpTokens {
            gap: g.padding_xs,
            cell_width: 0.,
            cell_width_sm: 0.,
            cell_width_lg: 0.,
            padding_inline: g.padding_xxs,
            padding_inline_sm: g.padding_xxs / 2.,
            padding_inline_lg: g.padding_xs,
            globals: Arc::new(g.clone()),
        }
    }
    fn merge(parent: &Self::Patch, local: &Self::Patch) -> Self::Patch {
        macro_rules! fields {($($name:ident),*) => {OtpTokenPatch {$($name: local.$name.or(parent.$name)),*}};}
        fields!(
            gap,
            cell_width,
            cell_width_sm,
            cell_width_lg,
            padding_inline,
            padding_inline_sm,
            padding_inline_lg
        )
    }
    fn apply(tokens: &mut Self::Tokens, patch: &Self::Patch) {
        macro_rules! fields {($($name:ident),*) => {$(if let Some(value) = patch.$name && (value as f32).is_finite() && value >= 0. {tokens.$name = value;})*};}
        fields!(
            gap,
            cell_width,
            cell_width_sm,
            cell_width_lg,
            padding_inline,
            padding_inline_sm,
            padding_inline_lg
        );
    }
}

/// Retain OtpState once; render new builders with a stable ID.
#[derive(IntoElement)]
pub struct OtpInput {
    id: ElementId,
    state: Entity<OtpState>,
    scope: Option<ConfigScope>,
    size: Option<Size>,
    variant: Option<Variant>,
    status: ValidationStatus,
    disabled: Option<bool>,
    read_only: Option<bool>,
    placeholder: SharedString,
    accessibility: Accessibility,
    separator: Option<Box<dyn Fn(usize) -> Option<AnyElement>>>,
    block: bool,
    style: StyleRefinement,
}
impl OtpInput {
    pub fn new(id: impl Into<ElementId>, state: &Entity<OtpState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            size: None,
            variant: None,
            status: ValidationStatus::None,
            disabled: None,
            read_only: None,
            placeholder: SharedString::default(),
            accessibility: Accessibility::new(Role::Group),
            separator: None,
            block: false,
            style: StyleRefinement::default(),
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }
    pub fn variant(mut self, variant: Variant) -> Self {
        self.variant = Some(variant);
        self
    }
    pub fn status(mut self, status: ValidationStatus) -> Self {
        self.status = status;
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    pub fn read_only(mut self, read_only: bool) -> Self {
        self.read_only = Some(read_only);
        self
    }
    pub fn placeholder(mut self, placeholder: impl Into<SharedString>) -> Self {
        self.placeholder = placeholder.into();
        self
    }
    pub fn block(mut self, block: bool) -> Self {
        self.block = block;
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
    /// Returns an optional separator after each zero-based cell index.
    pub fn separator(mut self, separator: impl Fn(usize) -> Option<AnyElement> + 'static) -> Self {
        self.separator = Some(Box::new(separator));
        self
    }
}
impl Styled for OtpInput {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct MountedOtp {
    state: Entity<OtpState>,
    scope: ConfigScope,
    disabled_override: Option<bool>,
    _state: Subscription,
    _scope: Subscription,
}
impl MountedOtp {
    fn new(
        state: Entity<OtpState>,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        Self {
            _state: cx.observe(&state, |_, _, cx| cx.notify()),
            _scope: Self::observe_scope(&scope, window, cx),
            state,
            scope,
            disabled_override: None,
        }
    }
    fn observe_scope(
        scope: &ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        scope.observe_in(window, cx, |this, window, cx| {
            let disabled = this.disabled_override.unwrap_or(
                this.scope.snapshot(cx).disabled || this.state.read(cx).requested_disabled,
            );
            this.state
                .update(cx, |state, cx| state.apply_disabled(disabled, window, cx));
            cx.notify();
        })
    }
}
impl RenderOnce for OtpInput {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let mounted = window.use_keyed_state((self.id.clone(), "otp"), cx, |window, cx| {
            MountedOtp::new(self.state.clone(), scope.clone(), window, cx)
        });
        mounted.update(cx, |mounted, cx| {
            if mounted.state != self.state {
                mounted.state = self.state.clone();
                mounted._state = cx.observe(&self.state, |_, _, cx| cx.notify());
            }
            if mounted.scope != scope {
                mounted.scope = scope.clone();
                mounted._scope = MountedOtp::observe_scope(&scope, window, cx);
            }
            mounted.disabled_override = self.disabled;
        });
        let config = scope.snapshot(cx);
        let disabled = self
            .disabled
            .unwrap_or(config.disabled || self.state.read(cx).requested_disabled);
        self.state.update(cx, |state, cx| {
            state.apply_disabled(disabled, window, cx);
            if let Some(read_only) = self.read_only {
                state.set_read_only(read_only, window, cx);
            }
        });
        let state = self.state.read(cx);
        let editors: Vec<_> = state
            .editors
            .iter()
            .map(|cell| cell.editor.clone())
            .collect();
        let mask = state.options().mask.clone();
        let read_only = state.is_read_only();
        let length = editors.len();
        let tokens = config.theme.component::<OtpTheme>();
        let mut input_tokens = (*config.theme.component::<InputTheme>()).clone();
        input_tokens.padding_inline = tokens.padding_inline;
        input_tokens.padding_inline_sm = tokens.padding_inline_sm;
        input_tokens.padding_inline_lg = tokens.padding_inline_lg;
        let input_tokens = Arc::new(input_tokens);
        let size = self.size.unwrap_or(config.size);
        let mut width = match size {
            Size::Small => tokens.cell_width_sm,
            Size::Medium => tokens.cell_width,
            Size::Large => tokens.cell_width_lg,
        };
        let variant = self.variant.unwrap_or(config.variant);
        if width == 0. {
            let metrics = InputStyle::from_tokens(
                &input_tokens,
                size,
                variant,
                self.status,
                InputInteraction::default(),
            )
            .at_scale(window.scale_factor());
            let text_system = window.text_system();
            let font = text_system.resolve_font(&input_tokens.font);
            // The reference's htmlSize=1 uses the font's maximum glyph box,
            // snapped to the device grid, plus its own padding and borders.
            let content = window.pixel_snap(
                text_system
                    .bounding_box(font, px(metrics.font_size as f32))
                    .size
                    .width,
            );
            let inline_border = if matches!(variant, Variant::Outlined | Variant::Filled) {
                metrics.border_width
            } else {
                0.
            };
            width = f64::from(f32::from(content).max(metrics.font_size as f32))
                + 2. * (metrics.padding_inline + inline_border);
        }
        let direction = config.direction;
        let label = self
            .accessibility
            .label
            .clone()
            .unwrap_or_else(|| config.locale.common.verification_code.clone());
        self.accessibility.label = Some(label.clone());
        self.accessibility.disabled = disabled;
        self.accessibility.read_only = read_only;
        self.accessibility.invalid = self.status == ValidationStatus::Error;
        let automation_id = self.accessibility.automation_id.clone();
        let mut root = div()
            .id(self.id)
            .flex()
            .items_center()
            .gap(px(tokens.gap as f32))
            .flex_shrink_0()
            .max_w_full()
            .when(direction == Direction::Rtl, |root| root.flex_row_reverse())
            .when(self.block, |root| root.w_full());
        root.style().refine(&self.style);
        for (index, editor) in editors.into_iter().enumerate() {
            let cell_label = config
                .locale
                .common
                .code_character
                .replace("{index}", &(index + 1).to_string())
                .replace("{length}", &length.to_string());
            let mut input = Input::new(("otp-cell", index), &editor)
                .scope(scope.clone())
                .size(size)
                .variant(variant)
                .status(self.status)
                .disabled(disabled)
                .read_only(read_only)
                .placeholder(self.placeholder.clone())
                .accessibility_label(format!("{label}, {cell_label}"))
                .w(px(width as f32))
                .flex_shrink_1()
                .min_w_0();
            if self.block {
                input = input.flex_1().min_w_0();
            }
            if let Some(id) = &automation_id {
                input = input.automation_id(format!("{id}.cell.{index}"));
            }
            input.centered = true;
            input.tokens = Some(input_tokens.clone());
            if let Some(mask) = &mask {
                input.mask_glyph = mask.clone();
                // Ant's mask is centered independently of the input's size and
                // validation colors, inheriting the surrounding OTP typography.
                input.mask_style = Some(crate::input::view::MaskStyle {
                    font: tokens.globals.font_family.to_gpui_font(),
                    font_size: tokens.globals.font_size,
                    color: tokens.globals.color_text.to_hsla(),
                });
            }
            let target = self.state.clone();
            input.frame_decorator = Some(Box::new(move |mut root, _: InputFrame, _, _| {
                let click = target.clone();
                root = root.capture_any_mouse_down(move |event, window, cx| {
                    if event.button == MouseButton::Left {
                        click.update(cx, |state, cx| state.focus_cell(index, window, cx));
                        window.prevent_default();
                        cx.stop_propagation();
                    }
                });
                let focus = target.clone();
                root = root.on_a11y_action(accesskit::Action::Focus, move |_, window, cx| {
                    focus.update(cx, |state, cx| state.focus_cell(index, window, cx));
                });
                macro_rules! action {
                    ($ty:ty, |$state:ident, $window:ident, $cx:ident| $body:expr) => {
                        let target = target.clone();
                        root = root.on_action(move |_: &$ty, window, cx| {
                            target.update(cx, |$state, cx| {
                                if $state.is_disabled()
                                    || $state.editors.get(index).is_none_or(|cell| {
                                        !cell.editor.read(cx).focus_handle(cx).is_focused(window)
                                    })
                                {
                                    cx.propagate();
                                    return;
                                }
                                if !$state.is_composing(cx) {
                                    let $window = &mut *window;
                                    let $cx = &mut *cx;
                                    $body;
                                }
                                window.prevent_default();
                            });
                        });
                    };
                }
                action!(actions::Left, |state, window, cx| state.focus_cell(
                    if direction == Direction::Ltr {
                        state.active_index().saturating_sub(1)
                    } else {
                        state.active_index() + 1
                    },
                    window,
                    cx
                ));
                action!(actions::Right, |state, window, cx| state.focus_cell(
                    if direction == Direction::Ltr {
                        state.active_index() + 1
                    } else {
                        state.active_index().saturating_sub(1)
                    },
                    window,
                    cx
                ));
                action!(actions::Home, |state, window, cx| state
                    .focus_cell(0, window, cx));
                action!(actions::SelectLeft, |state, window, cx| state.focus_cell(
                    if direction == Direction::Ltr {
                        state.active_index().saturating_sub(1)
                    } else {
                        state.active_index() + 1
                    },
                    window,
                    cx
                ));
                action!(actions::SelectRight, |state, window, cx| state.focus_cell(
                    if direction == Direction::Ltr {
                        state.active_index() + 1
                    } else {
                        state.active_index().saturating_sub(1)
                    },
                    window,
                    cx
                ));
                action!(actions::End, |state, window, cx| state.focus_cell(
                    length - 1,
                    window,
                    cx
                ));
                action!(actions::DocumentStart, |state, window, cx| state
                    .focus_cell(0, window, cx));
                action!(actions::DocumentEnd, |state, window, cx| state.focus_cell(
                    length - 1,
                    window,
                    cx
                ));
                action!(actions::WordLeft, |state, window, cx| state
                    .focus_cell(0, window, cx));
                action!(actions::WordRight, |state, window, cx| state.focus_cell(
                    length - 1,
                    window,
                    cx
                ));
                action!(actions::SelectAll, |state, window, cx| state
                    .select_all(window, cx));
                action!(actions::Copy, |state, _window, cx| state.copy(cx));
                action!(actions::Cut, |state, window, cx| state.cut(window, cx));
                action!(actions::Paste, |state, window, cx| state
                    .paste_clipboard(window, cx));
                action!(actions::Undo, |state, window, cx| state.undo(window, cx));
                action!(actions::Redo, |state, window, cx| state.redo(window, cx));
                action!(actions::Backspace, |state, window, cx| state
                    .backspace(window, cx));
                action!(actions::Delete, |state, window, cx| state
                    .delete(window, cx));
                action!(actions::DeleteWordLeft, |state, window, cx| state
                    .backspace(window, cx));
                action!(actions::DeleteWordRight, |state, window, cx| state
                    .delete(window, cx));
                action!(actions::Enter, |state, _window, cx| state
                    .commit(CommitReason::Enter, cx));
                action!(actions::Submit, |state, _window, cx| state
                    .commit(CommitReason::Enter, cx));
                root
            }));
            root = root.child(input);
            if index + 1 < length
                && let Some(separator) = self
                    .separator
                    .as_ref()
                    .and_then(|separator| separator(index))
            {
                let mut semantics = Accessibility::new(Role::GenericContainer);
                semantics.hidden = true;
                root = root.child(semantics.apply(div().flex_shrink_0().child(separator)));
            }
        }
        self.accessibility.apply(root)
    }
}
