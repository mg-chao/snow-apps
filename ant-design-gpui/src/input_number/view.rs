// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::motion::{HandleMotion, HandleTarget};
use super::{
    ExactDecimal, InputNumberState, InputNumberTheme, InputNumberTokens, StepDirection, StepSource,
};
use crate::{
    ConfigScope, Direction, Input, Size, ValidationStatus, Variant,
    behavior::Accessibility,
    icons::{IconName, IconRef, IconRenderOptions, icon},
    input::{InputFrame, actions},
};
use gpui::{
    AnyElement, App, Context, ElementId, Entity, Focusable, IntoElement, MouseButton, Refineable,
    RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, Window, accesskit, div,
    prelude::*, px, relative, size,
};
use std::sync::Arc;

/// A numeric editor using the same native text/IME implementation as Input.
/// Keep its InputNumberState entity outside the render method.
#[derive(IntoElement)]
pub struct InputNumber {
    id: ElementId,
    state: Entity<InputNumberState>,
    scope: Option<ConfigScope>,
    input: Input,
    variant: Option<Variant>,
    block: bool,
    controls: bool,
    controls_visible: Option<bool>,
    up_icon: IconRef,
    down_icon: IconRef,
    style: StyleRefinement,
    token_override: Option<Arc<super::InputNumberTokens>>,
}

impl InputNumber {
    pub fn new(id: impl Into<ElementId>, state: &Entity<InputNumberState>, cx: &App) -> Self {
        let id = id.into();
        Self {
            input: Input::new(id.clone(), &state.read(cx).editor).semantic_role(Role::SpinButton),
            id,
            state: state.clone(),
            scope: None,
            variant: None,
            block: false,
            controls: true,
            controls_visible: None,
            up_icon: IconName::UpOutlined.into(),
            down_icon: IconName::DownOutlined.into(),
            style: StyleRefinement::default(),
            token_override: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.input = self.input.scope(scope.clone());
        self.scope = Some(scope);
        self
    }
    pub fn size(mut self, size: Size) -> Self {
        self.input = self.input.size(size);
        self
    }
    pub fn variant(mut self, variant: Variant) -> Self {
        self.input = self.input.variant(variant);
        self.variant = Some(variant);
        self
    }
    pub fn status(mut self, status: ValidationStatus) -> Self {
        self.input = self.input.status(status);
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.input = self.input.disabled(disabled);
        self
    }
    pub fn read_only(mut self, read_only: bool) -> Self {
        self.input = self.input.read_only(read_only);
        self
    }
    pub fn placeholder(mut self, text: impl Into<SharedString>) -> Self {
        self.input = self.input.placeholder(text);
        self
    }
    pub fn prefix(mut self, prefix: impl IntoElement) -> Self {
        self.input = self.input.prefix(prefix);
        self
    }
    pub fn suffix(mut self, suffix: impl IntoElement) -> Self {
        self.input = self.input.suffix(suffix);
        self
    }
    pub fn allow_clear(mut self, allow: bool) -> Self {
        self.input = self.input.allow_clear(allow);
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.input = self.input.accessibility_label(label);
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.input = self.input.accessibility_description(description);
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.input = self.input.automation_id(id);
        self
    }
    pub fn block(mut self, block: bool) -> Self {
        self.block = block;
        self
    }
    pub fn controls(mut self, controls: bool) -> Self {
        self.controls = controls;
        self
    }
    pub fn controls_visible(mut self, visible: bool) -> Self {
        self.controls_visible = Some(visible);
        self
    }
    pub fn control_icons(mut self, up: impl Into<IconRef>, down: impl Into<IconRef>) -> Self {
        self.up_icon = up.into();
        self.down_icon = down.into();
        self
    }
    /// Composite controls retain their native editor while applying their own
    /// measured channel metrics without introducing a configuration subtree.
    pub(crate) fn with_tokens(mut self, tokens: Arc<super::InputNumberTokens>) -> Self {
        self.token_override = Some(tokens);
        self
    }
}
impl Styled for InputNumber {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}
impl crate::input::InputGroupItem for InputNumber {
    fn into_group_element(mut self, context: crate::input::InputGroupContext) -> AnyElement {
        self.scope = self.scope.or_else(|| Some(context.scope.clone()));
        self.input = self.input.in_group(context);
        self.into_any_element()
    }
}

struct MountedNumber {
    state: Entity<InputNumberState>,
    hovered: Option<StepDirection>,
    pressed: Option<StepDirection>,
    motion: HandleMotion,
    epoch: std::time::Instant,
    frame_pending: bool,
    controls_available: bool,
    _state: Subscription,
    _release: Subscription,
    _window: Vec<Subscription>,
}
impl MountedNumber {
    fn new(state: Entity<InputNumberState>, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let observe = Self::observe_state(&state, window, cx);
        let active = cx.observe_window_activation(window, |this, _, cx| {
            this.pressed = None;
            this.hovered = None;
            cx.notify();
        });
        let visible = cx.observe_window_visibility(window, |this, _, _, cx| {
            this.pressed = None;
            this.hovered = None;
            cx.notify();
        });
        let release =
            cx.on_release(|this, cx| this.state.update(cx, |state, _| state.cancel_repeat()));
        Self {
            state,
            hovered: None,
            pressed: None,
            motion: HandleMotion::default(),
            epoch: cx.background_executor().now(),
            frame_pending: false,
            controls_available: false,
            _state: observe,
            _release: release,
            _window: vec![active, visible],
        }
    }

    fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if !self.frame_pending {
            self.frame_pending = true;
            let mounted = cx.weak_entity();
            window.on_next_frame(move |_, cx| {
                let _ = mounted.update(cx, |mounted, cx| {
                    mounted.frame_pending = false;
                    cx.notify();
                });
            });
        }
    }

    fn observe_state(
        state: &Entity<InputNumberState>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        cx.observe_in(state, window, |this, _, window, cx| {
            let state = this.state.read(cx);
            if state.is_disabled(cx)
                || state.is_read_only(cx)
                || state.is_composing(cx)
                || !state.focus_handle(cx).is_focused(window)
            {
                this.pressed = None;
            }
            cx.notify();
        })
    }
}

impl RenderOnce for InputNumber {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let tokens = self
            .token_override
            .take()
            .unwrap_or_else(|| config.theme.component::<InputNumberTheme>());
        let mounted = window.use_keyed_state((self.id, "numeric"), cx, |window, cx| {
            MountedNumber::new(self.state.clone(), window, cx)
        });
        mounted.update(cx, |mounted, cx| {
            if mounted.state != self.state {
                mounted.state.update(cx, |state, _| state.cancel_repeat());
                mounted.state = self.state.clone();
                mounted._state = MountedNumber::observe_state(&self.state, window, cx);
                mounted.hovered = None;
                mounted.pressed = None;
            }
        });
        let state = self.state.read(cx);
        let exact_value = state.value().and_then(|value| value.decimal().ok());
        self.input.value_error = exact_value.as_ref().is_some_and(|value| {
            state
                .options()
                .minimum
                .as_ref()
                .is_some_and(|min| value < min)
                || state
                    .options()
                    .maximum
                    .as_ref()
                    .is_some_and(|max| value > max)
        });
        self.input.tokens = Some(Arc::new(tokens.input.clone()));
        self.input.numeric = true;
        self.input = if self.block {
            self.input.w_full()
        } else {
            self.input.w(px(tokens.control_width as f32))
        };
        self.input.style().refine(&self.style);
        let direction = config.direction;
        let variant = self.variant.unwrap_or(config.variant);
        let always = self.controls_visible.unwrap_or(tokens.handle_visible);
        let controls = self.controls;
        let up_icon = self.up_icon;
        let down_icon = self.down_icon;
        let target = self.state;
        self.input.frame_decorator =
            Some(Box::new(move |mut root, frame: InputFrame, window, cx| {
                let state = target.read(cx);
                // AccessKit numeric properties are f64. Unrepresentable exact values
                // remain available as text instead of reporting a rounded number.
                if let Some(value) = state
                    .value()
                    .and_then(|value| value.decimal().ok())
                    .as_ref()
                    .and_then(exact_number)
                {
                    root = root.aria_numeric_value(value);
                }
                if let Some(value) = state.options().minimum.as_ref().and_then(exact_number) {
                    root = root.aria_min_numeric_value(value);
                }
                if let Some(value) = state.options().maximum.as_ref().and_then(exact_number) {
                    root = root.aria_max_numeric_value(value);
                }
                if let Some(value) = exact_number(&state.options().step) {
                    root = root.aria_numeric_value_step(value);
                }
                for (action, direction) in [
                    (accesskit::Action::Increment, StepDirection::Up),
                    (accesskit::Action::Decrement, StepDirection::Down),
                ] {
                    if state.can_step(direction, cx) {
                        let target = target.clone();
                        root = root.on_a11y_action(action, move |_, window, cx| {
                            target.update(cx, |state, cx| {
                                state.step(direction, StepSource::Accessibility, false, window, cx)
                            })
                        });
                    }
                }
                if !frame.disabled && !frame.read_only {
                    let target = target.clone();
                    root = root.on_a11y_action(
                        accesskit::Action::SetValue,
                        move |data, window, cx| {
                            if let Some(accesskit::ActionData::NumericValue(value)) = data
                                && value.is_finite()
                            {
                                let editor = target.read(cx).editor.clone();
                                editor.update(cx, |editor, cx| {
                                    editor.replace_range(
                                        Some(0..editor.value().len()),
                                        &value.to_string(),
                                        crate::input::EditOrigin::Accessibility,
                                        window,
                                        cx,
                                    )
                                });
                            }
                        },
                    );
                }
                macro_rules! key {
                    ($action:ty, $direction:expr, $accelerated:expr) => {
                        let target = target.clone();
                        root = root.on_action(move |_: &$action, window, cx| {
                            if !target.read(cx).options().keyboard
                                || !target.read(cx).focus_handle(cx).is_focused(window)
                            {
                                cx.propagate();
                                return;
                            }
                            target.update(cx, |state, cx| {
                                state.step(
                                    $direction,
                                    StepSource::Keyboard,
                                    $accelerated,
                                    window,
                                    cx,
                                )
                            });
                            window.prevent_default();
                        });
                    };
                }
                key!(actions::Up, StepDirection::Up, false);
                key!(actions::Down, StepDirection::Down, false);
                key!(actions::SelectUp, StepDirection::Up, true);
                key!(actions::SelectDown, StepDirection::Down, true);
                let wheel_target = target.clone();
                root = root.on_scroll(move |event, window, cx| {
                    let state = wheel_target.read(cx);
                    if !state.options().change_on_wheel
                        || !state.focus_handle(cx).is_focused(window)
                        || state.is_composing(cx)
                        || state.is_disabled(cx)
                        || state.is_read_only(cx)
                    {
                        return;
                    }
                    let delta = event.delta.pixel_delta(px(16.)).y;
                    if delta == px(0.) {
                        return;
                    }
                    wheel_target.update(cx, |state, cx| {
                        state.step(
                            if delta > px(0.) {
                                StepDirection::Up
                            } else {
                                StepDirection::Down
                            },
                            StepSource::Wheel,
                            event.modifiers.shift,
                            window,
                            cx,
                        )
                    });
                    cx.stop_propagation();
                    window.prevent_default();
                });
                let available = controls && !frame.disabled && !frame.read_only;
                let enabled = [
                    state.can_step(StepDirection::Up, cx),
                    state.can_step(StepDirection::Down, cx),
                ];
                let background = if variant == Variant::Filled && !frame.focused {
                    tokens.filled_handle_bg
                } else if variant == Variant::Borderless {
                    crate::theme::Color::TRANSPARENT
                } else {
                    tokens.handle_bg
                };
                let visual = mounted.update(cx, |mounted, cx| {
                    if !available {
                        mounted.state.update(cx, |state, _| state.cancel_repeat());
                        mounted.hovered = None;
                        mounted.pressed = None;
                    }
                    let mut policy = crate::behavior::MotionPolicy::new(
                        config.motion && tokens.input.globals.motion,
                        cx.reduce_motion(),
                    );
                    policy.enabled &= available && window.is_visible() && window.is_window_active();
                    // Display-none removes the handle subtree; restoring it starts
                    // at current styles without reviving a cancelled press.
                    if mounted.controls_available != available {
                        policy.enabled = false;
                        mounted.controls_available = available;
                    }
                    let now = cx
                        .background_executor()
                        .now()
                        .saturating_duration_since(mounted.epoch);
                    let (visual, animating) = mounted.motion.resolve(
                        HandleTarget {
                            shown: available && (always || frame.hovered || frame.focused),
                            background: background.into(),
                            hovered: mounted.hovered,
                            pressed: mounted.pressed,
                            enabled,
                        },
                        &tokens,
                        now,
                        policy,
                    );
                    if animating {
                        mounted.request_frame(window, cx);
                    }
                    visual
                });
                if available && visual.width > 0. {
                    let padding = px(frame.metrics.padding_inline as f32 + visual.width);
                    root = match direction {
                        Direction::Ltr => root.pr(padding),
                        Direction::Rtl => root.pl(padding),
                    };
                    let border_width = tokens.input.globals.line_width;
                    let scale = f64::from(window.scale_factor());
                    let border_width = if border_width > 0. {
                        (border_width * scale).floor().max(1.) / scale
                    } else {
                        0.
                    };
                    root = root.child(
                        div()
                            .absolute()
                            .top_0()
                            .bottom_0()
                            .w(px(visual.width))
                            .opacity(visual.opacity)
                            .overflow_hidden()
                            .flex()
                            .flex_col()
                            .bg(visual.background)
                            .when(direction == Direction::Ltr, |root| {
                                root.right_0().border_l(px(border_width as f32))
                            })
                            .when(direction == Direction::Rtl, |root| {
                                root.left_0().border_r(px(border_width as f32))
                            })
                            .border_color(visual.border)
                            .child(step_button(
                                StepButton {
                                    direction: StepDirection::Up,
                                    label: config.locale.common.increase_value.clone(),
                                    icon: up_icon,
                                    tokens: tokens.clone(),
                                    state: target.clone(),
                                    mounted: mounted.clone(),
                                    separator: variant != Variant::Borderless,
                                    fraction: visual.fraction,
                                    color: visual.colors[0],
                                    background: visual.pressed[0],
                                    border: visual.border,
                                    border_width,
                                },
                                window,
                                cx,
                            ))
                            .child(step_button(
                                StepButton {
                                    direction: StepDirection::Down,
                                    label: config.locale.common.decrease_value.clone(),
                                    icon: down_icon,
                                    tokens: tokens.clone(),
                                    state: target.clone(),
                                    mounted: mounted.clone(),
                                    separator: variant != Variant::Borderless,
                                    fraction: 1. - visual.fraction,
                                    color: visual.colors[1],
                                    background: visual.pressed[1],
                                    border: visual.border,
                                    border_width,
                                },
                                window,
                                cx,
                            )),
                    );
                }
                root.overflow_hidden()
            }));
        self.input
    }
}

fn exact_number(decimal: &ExactDecimal) -> Option<f64> {
    let number = decimal.to_number().ok()?;
    (ExactDecimal::from_number(number).ok().as_ref() == Some(decimal)).then_some(number)
}

struct StepButton {
    direction: StepDirection,
    label: SharedString,
    icon: IconRef,
    tokens: Arc<InputNumberTokens>,
    state: Entity<InputNumberState>,
    mounted: Entity<MountedNumber>,
    fraction: f32,
    separator: bool,
    color: gpui::Hsla,
    background: gpui::Hsla,
    border: gpui::Hsla,
    border_width: f64,
}

fn step_button(button: StepButton, window: &Window, cx: &App) -> AnyElement {
    let StepButton {
        direction,
        label,
        icon: icon_ref,
        tokens,
        state,
        mounted,
        fraction,
        separator,
        color,
        background,
        border,
        border_width,
    } = button;
    let disabled = !state.read(cx).can_step(direction, cx);
    let down = state.clone();
    let down_mount = mounted.clone();
    let up = state.clone();
    let up_mount = mounted.clone();
    let outside = state.clone();
    let outside_mount = mounted.clone();
    let hover_mount = mounted.clone();
    let exit = state.clone();
    let exit_mount = mounted.clone();
    let mut semantics = Accessibility::new(Role::Button).label(label);
    semantics.disabled = disabled;
    semantics
        .apply(
            div()
                .id(if direction == StepDirection::Up {
                    "step-up"
                } else {
                    "step-down"
                })
                .h(relative(fraction))
                .w_full()
                .flex()
                .items_center()
                .justify_center()
                .when(!disabled, |root| root.cursor_pointer())
                .when(disabled, |root| root.cursor_not_allowed())
                .bg(background)
                .when(separator && direction == StepDirection::Down, |root| {
                    root.border_t(px(border_width as f32)).border_color(border)
                })
                .on_mouse_down(MouseButton::Left, move |event, window, cx| {
                    if down.read(cx).can_step(direction, cx) {
                        down_mount.update(cx, |mount, cx| {
                            mount.pressed = Some(direction);
                            cx.notify();
                        });
                        down.update(cx, |state, cx| {
                            state.start_repeat(direction, event.modifiers.shift, window, cx)
                        });
                    }
                    window.prevent_default();
                    cx.stop_propagation();
                })
                .on_mouse_up(MouseButton::Left, move |_, window, cx| {
                    up.update(cx, |state, _| state.cancel_repeat());
                    up_mount.update(cx, |mount, cx| {
                        mount.pressed = None;
                        cx.notify();
                    });
                    window.prevent_default();
                    cx.stop_propagation();
                })
                .on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
                    outside.update(cx, |state, _| state.cancel_repeat());
                    outside_mount.update(cx, |mount, cx| {
                        mount.pressed = None;
                        cx.notify();
                    });
                })
                .on_hover(move |hovered, _, cx| {
                    hover_mount.update(cx, |mount, cx| {
                        if *hovered {
                            mount.hovered = Some(direction);
                        } else if mount.hovered == Some(direction) {
                            mount.hovered = None;
                        }
                        cx.notify();
                    })
                })
                .on_mouse_exit(move |_, _, cx| {
                    exit.update(cx, |state, _| state.cancel_repeat());
                    exit_mount.update(cx, |mount, cx| {
                        if mount.hovered == Some(direction) {
                            mount.hovered = None;
                        }
                        if mount.pressed == Some(direction) {
                            mount.pressed = None;
                        }
                        cx.notify();
                    });
                })
                .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
                    state.update(cx, |state, cx| {
                        state.step(direction, StepSource::Accessibility, false, window, cx)
                    })
                })
                .child(icon(icon_ref).render_options(IconRenderOptions {
                    size: size(
                        px(tokens.handle_font_size as f32),
                        px(tokens.handle_font_size as f32),
                    ),
                    scale_factor: window.scale_factor(),
                    colors: crate::icons::IconPalette {
                        text: color.into(),
                        ..Default::default()
                    },
                    ..Default::default()
                })),
        )
        .into_any_element()
}
