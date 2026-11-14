// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Ant Design tags. Build during render with stable, locally unique IDs.
//!
//! Closing is retained locally and can be cancelled with [`TagClose::prevent_default`].
//! [`CheckableTag`] is controlled: its change callback proposes a value to its owner.

mod group;
mod style;

pub use group::{CheckableTagGroup, TagOption};
pub use style::{TagStateStyle, TagStyle, TagTheme, TagTokenPatch, TagTokens};

use std::{cell::Cell, time::Duration};

use gpui::{
    AnyElement, App, Context, ElementId, FocusHandle, Hsla, IntoElement, KeyboardButton,
    MouseButton, Refineable, RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription,
    Toggled, Window, canvas, div, prelude::*, px,
};

use crate::{
    ConfigScope, Direction,
    behavior::{
        Accessibility, Activation, ActivationGuard, Availability, FocusRing, MotionPolicy,
        ScalarTransition, WaveMotion, WaveState, focus_handle, wave_color,
    },
    icons::{IconName, IconPalette, IconRef, IconRenderOptions, IconRenderer},
    theme::{Color, CubicBezier, LineType, PresetColor},
};

/// Tag colors are typed; custom colors retain their alpha and require no CSS parser.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum TagColor {
    #[default]
    Default,
    Preset(PresetColor),
    Success,
    Processing,
    Error,
    Warning,
    Custom(Color),
}

impl From<PresetColor> for TagColor {
    fn from(color: PresetColor) -> Self {
        Self::Preset(color)
    }
}

impl From<Color> for TagColor {
    fn from(color: Color) -> Self {
        Self::Custom(color)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TagVariant {
    #[default]
    Filled,
    Solid,
    Outlined,
}

/// A close request shared by pointer, keyboard and accessibility activation.
#[derive(Debug)]
pub struct TagClose {
    pub activation: Activation,
    prevented: Cell<bool>,
}

impl TagClose {
    /// Keep the Tag mounted and visible, for example while asking its owner to confirm.
    pub fn prevent_default(&self) {
        self.prevented.set(true);
    }
    pub fn default_prevented(&self) -> bool {
        self.prevented.get()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TagChange {
    pub checked: bool,
    pub activation: Activation,
}

type ClickHandler = Box<dyn Fn(&Activation, &mut Window, &mut App)>;
type CloseHandler = Box<dyn Fn(&TagClose, &mut Window, &mut App)>;
type ChangeHandler = Box<dyn Fn(&TagChange, &mut Window, &mut App)>;
const CSS_EASE: CubicBezier = CubicBezier::new(0.25, 0.1, 0.25, 1.);

#[derive(IntoElement)]
pub struct Tag {
    id: ElementId,
    scope: Option<ConfigScope>,
    label: SharedString,
    content: Option<AnyElement>,
    icon: Option<IconRef>,
    color: TagColor,
    variant: TagVariant,
    disabled: Option<bool>,
    closable: Option<bool>,
    close_icon: Option<IconRef>,
    close_disabled: bool,
    href: Option<SharedString>,
    checked: Option<bool>,
    focus: Option<FocusHandle>,
    close_focus: Option<FocusHandle>,
    accessibility: Accessibility,
    close_label: Option<SharedString>,
    close_automation_id: Option<SharedString>,
    on_click: Option<ClickHandler>,
    on_close: Option<CloseHandler>,
    on_change: Option<ChangeHandler>,
    wave: bool,
    style: StyleRefinement,
    icon_style: Option<Box<StyleRefinement>>,
    content_style: Option<Box<StyleRefinement>>,
    close_style: Option<Box<StyleRefinement>>,
}

impl Tag {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            label: SharedString::default(),
            content: None,
            icon: None,
            color: TagColor::Default,
            variant: TagVariant::Filled,
            disabled: None,
            closable: None,
            close_icon: None,
            close_disabled: false,
            href: None,
            checked: None,
            focus: None,
            close_focus: None,
            accessibility: Accessibility::default(),
            close_label: None,
            close_automation_id: None,
            on_click: None,
            on_close: None,
            on_change: None,
            wave: true,
            style: StyleRefinement::default(),
            icon_style: None,
            content_style: None,
            close_style: None,
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = label.into();
        self.content = None;
        self
    }
    /// Custom content participates in native layout. Supply an accessible label when needed.
    pub fn child(mut self, content: impl IntoElement) -> Self {
        self.content = Some(content.into_any_element());
        self.label = SharedString::default();
        self
    }
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self
    }
    pub fn color(mut self, color: impl Into<TagColor>) -> Self {
        self.color = color.into();
        self
    }
    pub fn variant(mut self, variant: TagVariant) -> Self {
        self.variant = variant;
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    pub fn closable(mut self, closable: bool) -> Self {
        self.closable = Some(closable);
        self
    }
    /// A custom close icon enables closing unless `.closable(false)` is explicit.
    pub fn close_icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.close_icon = Some(icon.into());
        self
    }
    pub fn close_disabled(mut self, disabled: bool) -> Self {
        self.close_disabled = disabled;
        self
    }
    pub fn href(mut self, href: impl Into<SharedString>) -> Self {
        self.href = Some(href.into());
        self
    }
    pub fn focus_handle(mut self, focus: &FocusHandle) -> Self {
        self.focus = Some(focus.clone());
        self
    }
    pub fn close_focus_handle(mut self, focus: &FocusHandle) -> Self {
        self.close_focus = Some(focus.clone());
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
    pub fn close_accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.close_label = Some(label.into());
        self
    }
    pub fn close_automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.close_automation_id = Some(id.into());
        self
    }
    pub fn on_click(
        mut self,
        handler: impl Fn(&Activation, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_click = Some(Box::new(handler));
        self
    }
    pub fn on_close(
        mut self,
        handler: impl Fn(&TagClose, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_close = Some(Box::new(handler));
        self
    }
    pub fn wave(mut self, enabled: bool) -> Self {
        self.wave = enabled;
        self
    }
    pub fn icon_style(mut self, style: StyleRefinement) -> Self {
        self.icon_style = Some(Box::new(style));
        self
    }
    pub fn content_style(mut self, style: StyleRefinement) -> Self {
        self.content_style = Some(Box::new(style));
        self
    }
    pub fn close_style(mut self, style: StyleRefinement) -> Self {
        self.close_style = Some(Box::new(style));
        self
    }

    fn actionable(&self) -> bool {
        self.checked.is_some() || self.on_click.is_some() || self.href.is_some()
    }
    fn can_close(&self) -> bool {
        self.closable.unwrap_or(self.close_icon.is_some())
    }
}

impl Styled for Tag {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

/// A fully controlled tag with checkbox semantics and Space activation.
#[derive(IntoElement)]
pub struct CheckableTag(Tag);

impl CheckableTag {
    pub fn new(id: impl Into<ElementId>) -> Self {
        let mut tag = Tag::new(id);
        tag.checked = Some(false);
        tag.wave = false;
        Self(tag)
    }
    pub fn checked(mut self, checked: bool) -> Self {
        self.0.checked = Some(checked);
        self
    }
    pub fn on_change(
        mut self,
        handler: impl Fn(&TagChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.0.on_change = Some(Box::new(handler));
        self
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.0 = self.0.scope(scope);
        self
    }
    pub fn label(mut self, label: impl Into<SharedString>) -> Self {
        self.0 = self.0.label(label);
        self
    }
    pub fn child(mut self, child: impl IntoElement) -> Self {
        self.0 = self.0.child(child);
        self
    }
    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.0 = self.0.icon(icon);
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.0 = self.0.disabled(disabled);
        self
    }
    pub fn focus_handle(mut self, focus: &FocusHandle) -> Self {
        self.0 = self.0.focus_handle(focus);
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.0 = self.0.accessibility_label(label);
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.0 = self.0.accessibility_description(description);
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.0 = self.0.automation_id(id);
        self
    }
    pub fn on_click(
        mut self,
        handler: impl Fn(&Activation, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.0 = self.0.on_click(handler);
        self
    }
    pub fn icon_style(mut self, style: StyleRefinement) -> Self {
        self.0 = self.0.icon_style(style);
        self
    }
    pub fn content_style(mut self, style: StyleRefinement) -> Self {
        self.0 = self.0.content_style(style);
        self
    }
}

impl Styled for CheckableTag {
    fn style(&mut self) -> &mut StyleRefinement {
        self.0.style()
    }
}

impl RenderOnce for CheckableTag {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        self.0.render(window, cx)
    }
}

#[derive(Default)]
struct VisualTransition {
    from: Option<TagStateStyle>,
    target: Option<TagStateStyle>,
    progress: Option<ScalarTransition>,
}

impl VisualTransition {
    fn sample(&self, now: Duration) -> TagStateStyle {
        let to = self.target.expect("initialized Tag transition");
        let from = self.from.unwrap_or(to);
        let p = self.progress.map_or(1., |p| p.sample(now).value) as f32;
        TagStateStyle {
            background: mix(from.background, to.background, p),
            border: mix(from.border, to.border, p),
            foreground: mix(from.foreground, to.foreground, p),
            close: mix(from.close, to.close, p),
        }
    }
    fn resolve(
        &mut self,
        target: TagStateStyle,
        now: Duration,
        duration: Duration,
        motion: MotionPolicy,
    ) -> (TagStateStyle, bool) {
        if self.target != Some(target) {
            self.from = Some(self.target.map_or(target, |_| self.sample(now)));
            self.target = Some(target);
            let mut progress = ScalarTransition::new(0., CSS_EASE);
            progress.retarget(
                1.,
                now,
                if self.from == self.target {
                    Duration::ZERO
                } else {
                    duration
                },
                motion,
            );
            self.progress = Some(progress);
        }
        let progress = self.progress.as_mut().expect("initialized Tag progress");
        if !motion.allows_motion() {
            progress.retarget(1., now, Duration::ZERO, motion);
        }
        (
            self.sample(now),
            !self.progress.unwrap().sample(now).finished,
        )
    }
}

struct TagState {
    scope: ConfigScope,
    input: Option<TagInput>,
    close_input: Option<TagInput>,
    disabled: Option<bool>,
    close_disabled: bool,
    actionable: bool,
    closable: bool,
    checked: Option<bool>,
    closed: bool,
    hovered: bool,
    pressed: bool,
    close_hovered: bool,
    visual: VisualTransition,
    wave: WaveState,
    frame_pending: bool,
    epoch: std::time::Instant,
    _config: Subscription,
}

/// Passive tags own no native focus entries or input subscriptions.
struct TagInput {
    focus: FocusHandle,
    guard: ActivationGuard,
    _blur: Subscription,
}

impl TagInput {
    fn new(focus: Option<&FocusHandle>, window: &mut Window, cx: &mut App) -> Self {
        let focus = focus.cloned().unwrap_or_else(|| focus_handle(cx));
        let guard = ActivationGuard::default();
        let blur = guard.observe_blur(&focus, window, cx);
        Self {
            focus,
            guard,
            _blur: blur,
        }
    }

    fn set_focus(&mut self, focus: Option<&FocusHandle>, window: &mut Window, cx: &mut App) {
        if let Some(focus) = focus
            && self.focus != *focus
        {
            self.guard.cancel();
            self._blur = self.guard.observe_blur(focus, window, cx);
            self.focus = focus.clone();
        }
    }
}

impl TagState {
    fn new(tag: &Tag, scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        Self {
            _config: Self::observe_scope(&scope, window, cx),
            scope,
            input: None,
            close_input: None,
            disabled: tag.disabled,
            close_disabled: tag.close_disabled,
            actionable: tag.actionable(),
            closable: tag.can_close(),
            checked: tag.checked,
            closed: false,
            hovered: false,
            pressed: false,
            close_hovered: false,
            visual: VisualTransition::default(),
            wave: WaveState::default(),
            frame_pending: false,
            epoch: cx.background_executor().now(),
        }
    }
    fn observe_scope(
        scope: &ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        scope.observe_in(window, cx, |state, window, cx| {
            state.sync_availability(window, cx);
            cx.notify();
        })
    }
    fn sync_availability(&mut self, window: &mut Window, cx: &mut App) {
        let disabled = self
            .scope
            .snapshot(cx)
            .availability(self.disabled, false)
            .disabled;
        if let Some(input) = &self.input {
            input.guard.update(
                Availability {
                    disabled: disabled || self.closed || !self.actionable,
                    busy: false,
                },
                &input.focus,
                window,
                cx,
            );
        }
        if let Some(input) = &self.close_input {
            input.guard.update(
                Availability {
                    disabled: disabled || self.close_disabled || self.closed || !self.closable,
                    busy: false,
                },
                &input.focus,
                window,
                cx,
            );
        }
        if disabled || self.closed || !self.actionable {
            self.pressed = false;
            self.wave.clear();
        }
    }
    fn update(
        &mut self,
        tag: &Tag,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope != scope {
            if let Some(input) = &self.input {
                input.guard.cancel();
            }
            if let Some(input) = &self.close_input {
                input.guard.cancel();
            }
            self._config = Self::observe_scope(&scope, window, cx);
            self.scope = scope;
        }
        if tag.actionable() {
            self.input
                .get_or_insert_with(|| TagInput::new(tag.focus.as_ref(), window, cx))
                .set_focus(tag.focus.as_ref(), window, cx);
        }
        if tag.can_close() {
            self.close_input
                .get_or_insert_with(|| TagInput::new(tag.close_focus.as_ref(), window, cx))
                .set_focus(tag.close_focus.as_ref(), window, cx);
        }
        self.disabled = tag.disabled;
        self.close_disabled = tag.close_disabled;
        self.actionable = tag.actionable();
        self.closable = tag.can_close();
        self.checked = tag.checked;
        self.sync_availability(window, cx);
        if !self.actionable {
            self.input = None;
        }
        if !self.closable {
            self.close_input = None;
        }
    }
    fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if self.frame_pending {
            return;
        }
        self.frame_pending = true;
        let weak = cx.weak_entity();
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |state, cx| {
                state.frame_pending = false;
                cx.notify();
            });
        });
    }
    fn now(&self, cx: &App) -> Duration {
        cx.background_executor().now().duration_since(self.epoch)
    }
}

impl RenderOnce for Tag {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let popup_trigger = crate::popup::PopupTrigger::current(cx);
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            TagState::new(&self, scope.clone(), window, cx)
        });
        state.update(cx, |state, cx| {
            state.update(&self, scope.clone(), window, cx)
        });
        if state.read(cx).closed {
            return div().id(self.id).hidden().into_any_element();
        }
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<TagTheme>();
        let global = tokens.globals();
        let disabled = config.availability(self.disabled, false).disabled;
        let metrics =
            TagStyle::from_tokens(&tokens, self.color, self.variant, self.checked, disabled);
        let motion = MotionPolicy::new(config.motion && global.motion, cx.reduce_motion());
        let retained = state.read(cx);
        let focus = retained.input.as_ref().map(|input| input.focus.clone());
        let guard = retained.input.as_ref().map(|input| input.guard.clone());
        let close_input = retained
            .close_input
            .as_ref()
            .map(|input| (input.focus.clone(), input.guard.clone()));
        let mut target = if self.checked.is_some() && retained.pressed {
            metrics.active
        } else if self.checked.is_some() && retained.hovered {
            metrics.hover
        } else {
            metrics.rest
        };
        if retained.close_hovered
            && close_input
                .as_ref()
                .is_some_and(|(_, guard)| guard.availability().can_activate())
        {
            target.close = global.color_text_heading.into();
        }
        if self.close_disabled {
            target.close = global.color_text_disabled.into();
        }
        let (current, wave) = state.update(cx, |state, cx| {
            let now = state.now(cx);
            let (current, animating) =
                state
                    .visual
                    .resolve(target, now, global.motion_duration_mid, motion);
            let wave = state
                .wave
                .sample(now, WaveMotion::Regular, global, motion, self.wave);
            if animating || wave {
                state.request_frame(window, cx);
            }
            (current, wave)
        });
        let actionable = self.actionable();
        let closable = self.can_close();
        let has_content = !self.label.is_empty() || self.content.is_some();
        let mut root = div()
            .id(self.id.clone())
            .relative()
            .flex()
            .items_center()
            .flex_none()
            .px(px(metrics.padding_inline as f32))
            .border(px(metrics.line_width as f32))
            .rounded(px(metrics.radius as f32))
            .font(tokens.font().clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(px(metrics.line_height as f32))
            .whitespace_nowrap()
            .text_color(current.foreground)
            .bg(current.background)
            .border_color(current.border)
            .when(global.line_type == LineType::Dashed, |root| {
                root.border_dashed()
            })
            .when(config.direction == Direction::Rtl, |root| {
                root.flex_row_reverse()
            });
        if has_content || self.icon.is_some() || closable {
            root = root.min_h(px((metrics.line_height + 2. * metrics.line_width) as f32));
        }
        if let Some(focus) = &focus
            && !disabled
        {
            root = root.track_focus(focus);
        }
        if let Some(checked) = self.checked {
            root = root.role(Role::CheckBox).aria_toggled(if checked {
                Toggled::True
            } else {
                Toggled::False
            });
        } else if self.href.is_some() {
            root = root.role(Role::Link);
        } else if self.on_click.is_some() {
            root = root.role(Role::Button);
        }
        let has_icon = self.icon.is_some();
        if let Some(reference) = self.icon {
            let size = self
                .icon_style
                .as_ref()
                .and_then(|style| style.text.font_size)
                .or(self.style.text.font_size)
                .map_or(px(metrics.icon_size as f32), |size| {
                    size.to_pixels(window.rem_size())
                });
            let mut element = foreground_icon(reference, tokens.icon_palette()).size(size);
            if let Some(style) = &self.icon_style {
                element.style().refine(style);
            }
            root = root.child(element);
        }
        if has_content {
            let content = self
                .content
                .unwrap_or_else(|| self.label.clone().into_any_element());
            // Plain tags need only their native text node. Allocate a semantic
            // content slot when spacing or a caller's refinement requires it.
            root = if has_icon || self.content_style.is_some() {
                let mut slot = div().flex_none().whitespace_nowrap();
                if has_icon {
                    slot = if config.direction == Direction::Rtl {
                        slot.mr(px(metrics.padding_inline as f32))
                    } else {
                        slot.ml(px(metrics.padding_inline as f32))
                    };
                }
                if let Some(style) = &self.content_style {
                    slot.style().refine(style);
                }
                root.child(slot.child(content))
            } else {
                root.child(content)
            };
        }
        if closable {
            let (close_focus, close_guard) = close_input.expect("closable tag has retained input");
            let close_disabled = disabled || self.close_disabled;
            let gap = if has_icon && has_content {
                metrics.padding_inline
            } else {
                metrics.close_gap
            };
            let size = self
                .close_style
                .as_ref()
                .and_then(|style| style.text.font_size)
                .map_or(px(metrics.close_icon_size as f32), |size| {
                    size.to_pixels(window.rem_size())
                });
            let mut close = div()
                .id("close")
                .role(Role::Button)
                .relative()
                .flex()
                .items_center()
                .justify_center()
                .flex_none()
                .text_color(current.close)
                .size(size)
                .when(config.direction == Direction::Rtl, |close| {
                    close.mr(px(gap as f32))
                })
                .when(config.direction == Direction::Ltr, |close| {
                    close.ml(px(gap as f32))
                })
                .when(!close_disabled, |close| close.track_focus(&close_focus));
            if let Some(style) = &self.close_style {
                close.style().refine(style);
            }
            let close_radii = corner_radii(close.style(), window);
            close = close.child(
                foreground_icon(
                    self.close_icon
                        .unwrap_or_else(|| IconName::CloseOutlined.into()),
                    tokens.icon_palette(),
                )
                .size_full(),
            );
            close = if close_disabled {
                close.cursor_not_allowed()
            } else {
                close.cursor_pointer()
            };
            let weak = state.downgrade();
            close = close.on_hover(move |hovered, _, cx| {
                let _ = weak.update(cx, |state, cx| {
                    if state.close_hovered != *hovered {
                        state.close_hovered = *hovered;
                        cx.notify();
                    }
                });
            });
            let callback = self.on_close;
            let weak = state.downgrade();
            // Always stop close input at its own semantic node, including disabled
            // close buttons nested inside an otherwise actionable tag.
            close = close
                .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation())
                .on_click(|_, _, cx| cx.stop_propagation());
            let close = close_guard.install(close, move |activation, window, cx| {
                cx.stop_propagation();
                let event = TagClose {
                    activation,
                    prevented: Cell::new(false),
                };
                if let Some(callback) = &callback {
                    callback(&event, window, cx);
                }
                if event.default_prevented() {
                    return;
                }
                let _ = weak.update(cx, |state, cx| {
                    state.closed = true;
                    state.sync_availability(window, cx);
                    cx.notify();
                });
            });
            let close = Accessibility {
                label: Some(
                    self.close_label
                        .unwrap_or_else(|| config.locale.common.close.clone()),
                ),
                disabled: close_disabled,
                automation_id: self.close_automation_id,
                ..Default::default()
            }
            .apply(close);
            root = root.child(
                FocusRing::new(metrics.focus_color, px(metrics.focus_width as f32))
                    .radius(px(1.))
                    .apply(close, &close_focus)
                    .corners(close_radii),
            );
        }
        root.style().refine(&self.style);
        let radii = corner_radii(root.style(), window);
        if self.style.mouse_cursor.is_none() {
            root = if disabled {
                root.cursor_not_allowed()
            } else if actionable {
                root.cursor_pointer()
            } else {
                root.cursor_default()
            };
        }
        if actionable {
            let weak = state.downgrade();
            root = root.on_hover(move |hovered, _, cx| {
                let _ = weak.update(cx, |state, cx| {
                    if state.hovered != *hovered {
                        state.hovered = *hovered;
                        if !hovered {
                            state.pressed = false;
                        }
                        cx.notify();
                    }
                });
            });
            let weak = state.downgrade();
            root = root.on_mouse_down(MouseButton::Left, move |_, _, cx| {
                let _ = weak.update(cx, |state, cx| {
                    if state
                        .input
                        .as_ref()
                        .is_some_and(|input| input.guard.availability().can_activate())
                    {
                        state.pressed = true;
                        cx.notify();
                    }
                });
            });
            for outside in [false, true] {
                let weak = state.downgrade();
                let release = move |_: &gpui::MouseUpEvent, _: &mut Window, cx: &mut App| {
                    let _ = weak.update(cx, |state, cx| {
                        if state.pressed {
                            state.pressed = false;
                            cx.notify();
                        }
                    });
                };
                root = if outside {
                    root.on_mouse_up_out(MouseButton::Left, release)
                } else {
                    root.on_mouse_up(MouseButton::Left, release)
                };
            }
        }
        if wave {
            let effective = root.style();
            let color = wave_color(effective, global.color_primary.to_hsla());
            let borders = gpui::Edges {
                top: effective.border_widths.top.unwrap_or_default(),
                bottom: effective.border_widths.bottom.unwrap_or_default(),
                left: effective.border_widths.left.unwrap_or_default(),
                right: effective.border_widths.right.unwrap_or_default(),
            }
            .to_pixels(window.rem_size());
            for (wave, color) in state.update(cx, |state, _| state.wave.paint_samples(color)) {
                root = root.child(
                    canvas(
                        |_, _, _| (),
                        move |bounds, (), window, _| {
                            let spread = px(wave.spread);
                            if spread > px(0.) && wave.opacity > 0. {
                                window.paint_quad(
                                    gpui::outline(
                                        bounds.dilate(spread),
                                        color.opacity(wave.opacity),
                                        Default::default(),
                                    )
                                    .border_widths(spread)
                                    .corner_radii(
                                        radii
                                            .clamp_radii_for_quad_size(bounds.size)
                                            .map(|radius| *radius + spread),
                                    ),
                                );
                            }
                        },
                    )
                    .absolute()
                    .top(-borders.top)
                    .bottom(-borders.bottom)
                    .left(-borders.left)
                    .right(-borders.right),
                );
            }
        }
        let wave = self.wave && self.on_click.is_some() && self.checked.is_none();
        let callback = self.on_click;
        let change_callback = self.on_change;
        let href = self.href;
        let checkable = self.checked.is_some();
        let weak = state.downgrade();
        let root = if let Some(guard) = guard {
            let root = guard.install(root, move |activation, window, cx| {
                if (checkable && activation == Activation::Keyboard(KeyboardButton::Enter))
                    || (href.is_some() && activation == Activation::Keyboard(KeyboardButton::Space))
                {
                    return;
                }
                let change = weak
                    .update(cx, |state, cx| {
                        if wave && motion.allows_motion() {
                            state.wave.start(state.now(cx));
                        }
                        state.pressed = false;
                        cx.notify();
                        state.checked.map(|checked| TagChange {
                            checked: !checked,
                            activation,
                        })
                    })
                    .ok()
                    .flatten();
                if let Some(change) = change
                    && let Some(callback) = &change_callback
                {
                    callback(&change, window, cx);
                }
                if let Some(callback) = &callback {
                    callback(&activation, window, cx);
                }
                if let Some(href) = &href {
                    cx.open_url(href);
                }
            });
            popup_trigger.install(root, &guard, focus.as_ref().expect("actionable tag focus"))
        } else {
            root
        };
        self.accessibility.disabled = disabled;
        if self.accessibility.label.is_none() && !self.label.is_empty() {
            self.accessibility.label = Some(self.label);
        }
        let root = popup_trigger.apply(self.accessibility.apply(root));
        match focus {
            Some(focus) => FocusRing::new(metrics.focus_color, px(metrics.focus_width as f32))
                .radius(px(metrics.radius as f32))
                .apply(root, &focus)
                .corners(radii)
                .into_any_element(),
            None => root.into_any_element(),
        }
    }
}

fn corner_radii(style: &StyleRefinement, window: &Window) -> gpui::Corners<gpui::Pixels> {
    gpui::Corners {
        top_left: style.corner_radii.top_left.unwrap_or_default(),
        top_right: style.corner_radii.top_right.unwrap_or_default(),
        bottom_left: style.corner_radii.bottom_left.unwrap_or_default(),
        bottom_right: style.corner_radii.bottom_right.unwrap_or_default(),
    }
    .to_pixels(window.rem_size())
}

/// CSS color transitions interpolate premultiplied sRGB, preserving translucent tints.
fn mix(from: Hsla, to: Hsla, progress: f32) -> Hsla {
    if progress >= 1. || from == to {
        return to;
    }
    if progress <= 0. {
        return from;
    }
    let from = from.to_rgb();
    let to = to.to_rgb();
    let p = progress.clamp(0., 1.);
    let a = from.a + (to.a - from.a) * p;
    let channel = |x: f32, y: f32| {
        if a > f32::EPSILON {
            (x * from.a * (1. - p) + y * to.a * p) / a
        } else {
            0.
        }
    };
    gpui::Rgba {
        r: channel(from.r, to.r),
        g: channel(from.g, to.g),
        b: channel(from.b, to.b),
        a,
    }
    .into()
}

fn foreground_icon(reference: IconRef, palette: IconPalette) -> gpui::Canvas<()> {
    canvas(
        |_, _, _| (),
        move |bounds, (), window, cx| {
            if cx.try_global::<IconRenderer>().is_none() {
                cx.set_global(IconRenderer::default());
            }
            let options = IconRenderOptions {
                size: bounds.size,
                scale_factor: window.scale_factor(),
                colors: IconPalette {
                    text: window.text_style().color.into(),
                    ..palette
                },
                ..Default::default()
            };
            cx.update_global::<IconRenderer, _>(|renderer, cx| {
                let _ = renderer.paint(bounds, &reference, &options, window, cx);
            });
        },
    )
    .flex_shrink_0()
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod reference_tests;
