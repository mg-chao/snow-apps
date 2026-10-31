// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Ant Design radios. Build with a stable ID in each render.
//!
//! `checked` is controlled; `default_checked` initializes retained local state once.
//! Space, pointer clicks (including the label), and accessible selection share one
//! guarded callback. A selected radio never toggles off. Use `RadioGroup` for typed
//! single selection, roving keyboard focus, and arrow navigation.

mod group;
mod paint;
mod style;

use group::RadioNavigation;
pub use group::{RadioButtonStyle, RadioGroup, RadioOption, RadioOptionType};
use paint::{ButtonPaint, ButtonPaintQueue, paint_dot};
pub use style::{
    RadioButtonVisualStyle, RadioStateStyle, RadioStyle, RadioTheme, RadioTokenPatch, RadioTokens,
};

use std::time::Duration;

use gpui::{
    App, Context, ElementId, FocusHandle, Hsla, IntoElement, KeyboardButton, MouseButton,
    Refineable, RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, Toggled,
    Window, canvas, div, prelude::*, px,
};

use crate::{
    ConfigScope, Direction, Size,
    behavior::{
        Accessibility, Activation, ActivationGuard, FocusRing, MotionPolicy, ScalarTransition,
        WaveMotion, WaveSample, WaveState, focus_handle, wave_color,
    },
    theme::CubicBezier,
};

/// A proposed selection. `checked` is always true; radios do not toggle off.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RadioChange {
    pub checked: bool,
    pub activation: Activation,
}

type ChangeHandler = Box<dyn Fn(&RadioChange, &mut Window, &mut App)>;
const CSS_EASE: CubicBezier = CubicBezier::new(0.25, 0.1, 0.25, 1.);

/// An immediate builder backed by a small retained state for each mounted ID.
#[derive(IntoElement)]
pub struct Radio {
    id: ElementId,
    scope: Option<ConfigScope>,
    label: SharedString,
    checked: Option<bool>,
    default_checked: bool,
    disabled: Option<bool>,
    focus: Option<FocusHandle>,
    accessibility: Accessibility,
    on_change: Option<ChangeHandler>,
    wave: bool,
    style: StyleRefinement,
    icon_style: Option<Box<StyleRefinement>>,
    label_style: Option<Box<StyleRefinement>>,
    size: Option<Size>,
    option_type: RadioOptionType,
    button_style: RadioButtonStyle,
    group_tab_stop: Option<bool>,
    group_block: bool,
    group_vertical: bool,
    position: (usize, usize),
    button_paint: Option<(ButtonPaintQueue, usize)>,
    on_navigate: Option<NavigationHandler>,
}

type NavigationHandler = Box<dyn Fn(RadioNavigation, &mut Window, &mut App)>;

impl Radio {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            label: SharedString::default(),
            checked: None,
            default_checked: false,
            disabled: None,
            focus: None,
            accessibility: Accessibility::new(Role::RadioButton),
            on_change: None,
            wave: true,
            style: StyleRefinement::default(),
            icon_style: None,
            label_style: None,
            size: None,
            option_type: RadioOptionType::Default,
            button_style: RadioButtonStyle::Outline,
            group_tab_stop: None,
            group_block: false,
            group_vertical: false,
            position: (0, 1),
            button_paint: None,
            on_navigate: None,
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    pub fn label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = label.into();
        self
    }

    /// Supply the current value. Changes are proposals until the caller updates it.
    pub fn checked(mut self, checked: bool) -> Self {
        self.checked = Some(checked);
        self
    }

    /// Initialize local state on mount; subsequent renders preserve user changes.
    pub fn default_checked(mut self, checked: bool) -> Self {
        self.default_checked = checked;
        self
    }

    /// An explicit false overrides a disabled configuration provider.
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }

    pub fn focus_handle(mut self, focus: &FocusHandle) -> Self {
        self.focus = Some(focus.clone());
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

    pub fn on_change(
        mut self,
        handler: impl Fn(&RadioChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Box::new(handler));
        self
    }

    pub fn wave(mut self, enabled: bool) -> Self {
        self.wave = enabled;
        self
    }

    /// Refine the native box, independently of root layout and label typography.
    pub fn icon_style(mut self, style: StyleRefinement) -> Self {
        self.icon_style = Some(Box::new(style));
        self
    }

    pub fn label_style(mut self, style: StyleRefinement) -> Self {
        self.label_style = Some(Box::new(style));
        self
    }

    /// Button height and typography. Circular radios retain the token-defined size.
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }

    pub fn option_type(mut self, option_type: RadioOptionType) -> Self {
        self.option_type = option_type;
        self
    }

    pub fn button_style(mut self, style: RadioButtonStyle) -> Self {
        self.button_style = style;
        self
    }

    pub(crate) fn group_tab_stop(mut self, tab_stop: bool) -> Self {
        self.group_tab_stop = Some(tab_stop);
        self
    }

    pub(crate) fn group_block(mut self, block: bool) -> Self {
        self.group_block = block;
        self
    }

    pub(crate) fn group_vertical(mut self, vertical: bool) -> Self {
        self.group_vertical = vertical;
        self
    }

    pub(crate) fn button_position(mut self, index: usize, count: usize) -> Self {
        self.position = (index, count);
        self
    }

    pub(crate) fn button_paint(mut self, queue: ButtonPaintQueue, index: usize) -> Self {
        self.button_paint = Some((queue, index));
        self
    }

    pub(crate) fn on_navigate(
        mut self,
        handler: impl Fn(RadioNavigation, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_navigate = Some(Box::new(handler));
        self
    }
}

impl Styled for Radio {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

#[derive(Default)]
struct VisualTransition {
    from: Option<AnimatedStyle>,
    target: Option<AnimatedStyle>,
    progress: Option<ScalarTransition>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum VisualKind {
    Circle,
    Button,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct AnimatedStyle {
    kind: VisualKind,
    background: Hsla,
    // The circle animates its border; the button animates its text color.
    accent: Hsla,
}

impl AnimatedStyle {
    fn new(style: RadioStateStyle, button: bool) -> Self {
        Self {
            kind: if button {
                VisualKind::Button
            } else {
                VisualKind::Circle
            },
            background: style.background,
            accent: if button { style.label } else { style.border },
        }
    }

    fn apply(self, mut style: RadioStateStyle) -> RadioStateStyle {
        style.background = self.background;
        match self.kind {
            VisualKind::Circle => style.border = self.accent,
            VisualKind::Button => style.label = self.accent,
        }
        style
    }
}

impl VisualTransition {
    fn sample(&self, now: Duration) -> AnimatedStyle {
        let target = self.target.expect("initialized radio visual state");
        let progress = self.progress.map_or(1., |p| p.sample(now).value) as f32;
        let from = self.from.unwrap_or(target);
        if progress >= 1. || from == target {
            return target;
        }
        if progress <= 0. {
            return from;
        }
        AnimatedStyle {
            kind: target.kind,
            background: interpolate_color(from.background, target.background, progress),
            accent: interpolate_color(from.accent, target.accent, progress),
        }
    }

    fn resolve(
        &mut self,
        target: AnimatedStyle,
        now: Duration,
        duration: Duration,
        curve: CubicBezier,
        motion: MotionPolicy,
    ) -> (AnimatedStyle, bool) {
        if self.target != Some(target) {
            let from = self.target.map_or(target, |_| self.sample(now));
            self.from = Some(if from.kind == target.kind {
                from
            } else {
                target
            });
            self.target = Some(target);
            let mut progress = ScalarTransition::new(0., curve);
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
        let progress = self.progress.as_mut().expect("initialized transition");
        if !motion.allows_motion() {
            progress.retarget(1., now, Duration::ZERO, motion);
        }
        (
            self.sample(now),
            !self.progress.unwrap().sample(now).finished,
        )
    }
}

// Ant Design animates the dot's pseudo-element separately from the outer ring.
#[derive(Default)]
struct ColorTransition {
    from: Option<Hsla>,
    target: Option<Hsla>,
    progress: Option<ScalarTransition>,
}

impl ColorTransition {
    fn sample(&self, now: Duration) -> Hsla {
        let target = self.target.expect("initialized radio dot color");
        let progress = self.progress.map_or(1., |p| p.sample(now).value) as f32;
        interpolate_color(self.from.unwrap_or(target), target, progress)
    }

    fn resolve(
        &mut self,
        target: Hsla,
        now: Duration,
        duration: Duration,
        curve: CubicBezier,
        motion: MotionPolicy,
    ) -> (Hsla, bool) {
        if self.target != Some(target) {
            self.from = Some(self.target.map_or(target, |_| self.sample(now)));
            self.target = Some(target);
            let mut progress = ScalarTransition::new(0., curve);
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
        let progress = self.progress.as_mut().expect("initialized transition");
        if !motion.allows_motion() {
            progress.retarget(1., now, Duration::ZERO, motion);
        }
        (
            self.sample(now),
            !self.progress.unwrap().sample(now).finished,
        )
    }
}

struct RadioState {
    scope: ConfigScope,
    focus: FocusHandle,
    guard: ActivationGuard,
    checked: bool,
    controlled: bool,
    disabled: Option<bool>,
    hovered: bool,
    visual: VisualTransition,
    dot: Option<ScalarTransition>,
    dot_size: Option<ScalarTransition>,
    dot_color: ColorTransition,
    pressed: bool,
    wave: WaveState,
    frame_pending: bool,
    clock: Box<dyn Fn(&App) -> Duration>,
    _config: Subscription,
    _blur: Subscription,
}

impl RadioState {
    fn new(radio: &Radio, scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let focus = radio.focus.clone().unwrap_or_else(|| focus_handle(cx));
        let guard = ActivationGuard::default();
        let config = Self::observe_scope(&scope, window, cx);
        let blur = Self::observe_blur(&focus, &guard, window, cx);
        let epoch = cx.background_executor().now();
        Self {
            scope,
            focus,
            guard,
            checked: radio.checked.unwrap_or(radio.default_checked),
            controlled: radio.checked.is_some(),
            disabled: radio.disabled,
            hovered: false,
            visual: VisualTransition::default(),
            dot: None,
            dot_size: None,
            dot_color: ColorTransition::default(),
            pressed: false,
            wave: WaveState::default(),
            frame_pending: false,
            clock: Box::new(move |cx| cx.background_executor().now().duration_since(epoch)),
            _config: config,
            _blur: blur,
        }
    }

    fn observe_scope(
        scope: &ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        scope.observe_in(window, cx, |this, window, cx| {
            this.sync_availability(window, cx);
            cx.notify();
        })
    }

    fn observe_blur(
        focus: &FocusHandle,
        guard: &ActivationGuard,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        let guard = guard.clone();
        let weak = cx.weak_entity();
        window.on_focus_out(focus, cx, move |_, _, cx| {
            guard.cancel();
            let _ = weak.update(cx, |state, cx| {
                if state.pressed {
                    state.pressed = false;
                    cx.notify();
                }
            });
        })
    }

    fn sync_availability(&mut self, window: &mut Window, cx: &mut App) {
        let availability = self.scope.snapshot(cx).availability(self.disabled, false);
        self.guard.update(availability, &self.focus, window, cx);
        if !availability.can_activate() {
            self.wave.clear();
            self.pressed = false;
        }
    }

    fn update(
        &mut self,
        radio: &Radio,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope != scope {
            self.guard.cancel();
            self._config = Self::observe_scope(&scope, window, cx);
            self.scope = scope;
        }
        if let Some(focus) = &radio.focus
            && self.focus != *focus
        {
            self.guard.cancel();
            self._blur = Self::observe_blur(focus, &self.guard, window, cx);
            self.focus = focus.clone();
        }
        if let Some(checked) = radio.checked {
            self.checked = checked;
        }
        self.controlled = radio.checked.is_some();
        self.disabled = radio.disabled;
        if let Some(tab_stop) = radio.group_tab_stop {
            self.focus = self.focus.clone().tab_stop(tab_stop);
        }
        self.sync_availability(window, cx);
    }

    fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if !self.frame_pending {
            self.frame_pending = true;
            let weak = cx.weak_entity();
            window.on_next_frame(move |_, cx| {
                let _ = weak.update(cx, |state, cx| {
                    state.frame_pending = false;
                    cx.notify();
                });
            });
        }
    }
}

impl RenderOnce for Radio {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let popup_trigger = crate::popup::PopupTrigger::current(cx);
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            RadioState::new(&self, scope.clone(), window, cx)
        });
        state.update(cx, |state, cx| {
            state.update(&self, scope.clone(), window, cx)
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<RadioTheme>();
        let global = tokens.globals();
        let motion = MotionPolicy::new(config.motion && global.motion, cx.reduce_motion());
        let retained = state.read(cx);
        let checked = retained.checked;
        let focus = retained.focus.clone();
        let guard = retained.guard.clone();
        let availability = guard.availability();
        let button = self.option_type == RadioOptionType::Button;
        let circle = RadioStyle::from_tokens(&tokens, checked, availability.disabled);
        let button_style = RadioButtonVisualStyle::from_tokens(
            &tokens,
            self.size.unwrap_or(config.size),
            self.button_style,
            checked,
            availability.disabled,
        );
        let target = if button {
            if retained.pressed {
                button_style.active
            } else if retained.hovered {
                button_style.hover
            } else {
                button_style.rest
            }
        } else if retained.hovered {
            circle.hover
        } else {
            circle.rest
        };
        let (current, dot_progress, dot_size, has_active_wave) = state.update(cx, |state, cx| {
            let now = (state.clock)(cx);
            let (visual, color_animating) = state.visual.resolve(
                AnimatedStyle::new(target, button),
                now,
                global.motion_duration_mid,
                CSS_EASE,
                motion,
            );
            let mut current = visual.apply(target);
            let (mark, mark_animating) = state.dot_color.resolve(
                target.mark,
                now,
                global.motion_duration_slow,
                global.motion_ease_in_out_circ,
                motion,
            );
            current.mark = mark;
            let dot_target = f64::from(checked);
            let dot = state.dot.get_or_insert_with(|| {
                ScalarTransition::new(dot_target, global.motion_ease_in_out_circ)
            });
            dot.retarget(dot_target, now, global.motion_duration_slow, motion);
            let dot = dot.sample(now);
            let size = state.dot_size.get_or_insert_with(|| {
                ScalarTransition::new(circle.dot_size, global.motion_ease_in_out_circ)
            });
            size.retarget(circle.dot_size, now, global.motion_duration_slow, motion);
            let size = size.sample(now);
            let kind = if button {
                WaveMotion::Regular
            } else {
                WaveMotion::Quick
            };
            let wave = state.wave.sample(now, kind, global, motion, self.wave);
            if color_animating
                || (!button && (!dot.finished || !size.finished || mark_animating))
                || wave
            {
                state.request_frame(window, cx);
            }
            (
                current,
                dot.value.clamp(0., 1.) as f32,
                size.value.max(0.) as f32,
                wave,
            )
        });

        let rtl = config.direction == Direction::Rtl;
        let font_size = if button {
            button_style.font_size
        } else {
            circle.font_size
        };
        let line_height = if button {
            button_style.line_height
        } else {
            circle.line_height
        };
        let mut root = div()
            .id(self.id)
            .relative()
            .flex()
            .items_center()
            .font(tokens.font().clone())
            .text_size(px(font_size as f32))
            .line_height(gpui::relative((line_height / font_size) as f32))
            .text_color(current.label)
            .when(rtl, |root| root.flex_row_reverse())
            .when(availability.can_focus(), |root| root.track_focus(&focus))
            .aria_toggled(if checked {
                Toggled::True
            } else {
                Toggled::False
            })
            .when(self.group_block, |root| {
                root.flex_1().min_w_0().justify_center()
            });

        if button {
            let width = px(button_style.line_width as f32);
            let radius = px(button_style.radius as f32);
            let (index, count) = self.position;
            root = root
                .h(px(button_style.height as f32))
                .flex_none()
                .px(px(button_style.padding_inline as f32))
                .border(width)
                .border_color(current.border)
                .bg(current.background);
            if self.group_block {
                root = root.flex_1();
            }
            if self.group_vertical {
                if index > 0 {
                    root = root.mt(-width);
                }
                if index == 0 {
                    root = root.rounded_t(radius);
                }
                if index + 1 == count {
                    root = root.rounded_b(radius);
                }
            } else {
                if index > 0 {
                    root = if rtl {
                        root.mr(-width)
                    } else {
                        root.ml(-width)
                    };
                }
                if index == 0 {
                    root = if rtl {
                        root.rounded_r(radius)
                    } else {
                        root.rounded_l(radius)
                    };
                }
                if index + 1 == count {
                    root = if rtl {
                        root.rounded_l(radius)
                    } else {
                        root.rounded_r(radius)
                    };
                }
            }
        } else {
            root = root.min_h(px(circle.line_height as f32));
            if self.group_tab_stop.is_none() {
                root = if rtl {
                    root.ml(px(circle.group_gap as f32))
                } else {
                    root.mr(px(circle.group_gap as f32))
                };
            }
            let mut icon = div()
                .relative()
                .flex_none()
                .size(px(circle.size as f32))
                .border(px(circle.line_width as f32))
                .rounded_full()
                .bg(current.background)
                .border_color(current.border);
            if let Some(refinement) = &self.icon_style {
                icon.style().refine(refinement);
            }
            let borders = border_widths(icon.style(), window);
            let radii = corner_radii(icon.style(), window);
            let opacity = icon.style().opacity.unwrap_or(1.);
            let wave_color = wave_color(icon.style(), global.color_primary.to_hsla());
            if dot_progress > 0.001 && dot_size > 0. {
                let dot_size = px(dot_size * dot_progress);
                icon = icon.child(
                    outer_overlay(borders).child(
                        canvas(
                            |_, _, _| (),
                            move |bounds, (), window, _| {
                                paint_dot(
                                    bounds,
                                    dot_size,
                                    current.mark.opacity(dot_progress),
                                    window,
                                );
                            },
                        )
                        .size_full(),
                    ),
                );
            }
            if has_active_wave {
                for (sample, color) in
                    state.update(cx, |state, _| state.wave.paint_samples(wave_color))
                {
                    icon = icon.child(wave_overlay(borders, radii, color, sample));
                }
            }
            root = root.child(
                FocusRing::new(
                    circle.focus_color.opacity(opacity),
                    px(circle.focus_width as f32),
                )
                .apply(icon, &focus)
                .corners(radii),
            );
        }

        if !self.label.is_empty() {
            let mut label = div().min_w_0().child(self.label.clone());
            if button {
                label = label.whitespace_nowrap();
            } else {
                label = label.px(px(circle.label_padding as f32));
            }
            if let Some(refinement) = &self.label_style {
                label.style().refine(refinement);
            }
            root = root.child(label);
        }
        root.style().refine(&self.style);
        let button_radii = corner_radii(root.style(), window);
        let button_borders = border_widths(root.style(), window);
        let button_opacity = root.style().opacity.unwrap_or(1.);
        let button_wave_color = wave_color(root.style(), global.color_primary.to_hsla());
        let hidden = root.style().visibility == Some(gpui::Visibility::Hidden);
        if !hidden && let Some((queue, index)) = self.button_paint.take() {
            let focus = focus.clone();
            let color = root.style().border_color.unwrap_or(current.border);
            if checked {
                // The group paints this stroke after its neighbors, once. This
                // also avoids double-compositing semitransparent custom colors.
                root = root.border_color(crate::theme::Color::TRANSPARENT);
            }
            root = root.child(
                outer_overlay(button_borders).child(
                    canvas(
                        move |bounds, _, _| {
                            queue.record(
                                index,
                                ButtonPaint {
                                    bounds,
                                    borders: button_borders,
                                    radii: button_radii,
                                    color,
                                    checked,
                                    focus: focus.clone(),
                                    focus_color: button_style.focus_color,
                                    focus_width: px(button_style.focus_width as f32),
                                    opacity: button_opacity,
                                },
                            );
                        },
                        |_, (), _, _| {},
                    )
                    .size_full(),
                ),
            );
        }
        if self.style.mouse_cursor.is_none() {
            root = if availability.disabled {
                root.cursor_not_allowed()
            } else {
                root.cursor_pointer()
            };
        }
        if button && has_active_wave {
            let borders = border_widths(root.style(), window);
            let radii = corner_radii(root.style(), window);
            for (sample, color) in
                state.update(cx, |state, _| state.wave.paint_samples(button_wave_color))
            {
                root = root.child(wave_overlay(borders, radii, color, sample));
            }
        }
        let weak = state.downgrade();
        root = root.on_hover(move |hovered, _, cx| {
            let _ = weak.update(cx, |state, cx| {
                if state.hovered != *hovered {
                    state.hovered = *hovered;
                    cx.notify();
                }
            });
        });
        if button {
            let weak = state.downgrade();
            root = root.on_mouse_down(MouseButton::Left, move |_, _, cx| {
                set_pressed(&weak, true, cx)
            });
            let weak = state.downgrade();
            root = root.on_mouse_up(MouseButton::Left, move |_, _, cx| {
                set_pressed(&weak, false, cx)
            });
            let weak = state.downgrade();
            root = root.on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
                set_pressed(&weak, false, cx)
            });
        }
        if let Some(navigate) = self.on_navigate {
            let navigation_guard = guard.clone();
            root = root.on_key_down(move |event, window, cx| {
                if event.keystroke.modifiers.modified()
                    || !navigation_guard.availability().can_activate()
                {
                    return;
                }
                let navigation = match event.keystroke.key.as_str() {
                    "up" => Some(RadioNavigation::Previous),
                    "down" => Some(RadioNavigation::Next),
                    "left" => Some(if rtl {
                        RadioNavigation::Next
                    } else {
                        RadioNavigation::Previous
                    }),
                    "right" => Some(if rtl {
                        RadioNavigation::Previous
                    } else {
                        RadioNavigation::Next
                    }),
                    "home" => Some(RadioNavigation::First),
                    "end" => Some(RadioNavigation::Last),
                    _ => None,
                };
                if let Some(navigation) = navigation {
                    navigation_guard.cancel();
                    navigate(navigation, window, cx);
                    cx.stop_propagation();
                }
            });
        }
        let callback = self.on_change;
        let wave = self.wave;
        let in_group = self.group_tab_stop.is_some();
        let weak = state.downgrade();
        let root = guard.install(root, move |activation, window, cx| {
            if activation == Activation::Keyboard(KeyboardButton::Enter) {
                return;
            }
            let change = weak.update(cx, |state, cx| {
                if wave && motion.allows_motion() {
                    state.wave.start((state.clock)(cx));
                    cx.notify();
                }
                if state.checked && !in_group {
                    return None;
                }
                let change = RadioChange {
                    checked: true,
                    activation,
                };
                if !state.controlled {
                    state.checked = true;
                }
                cx.notify();
                Some(change)
            });
            if let Ok(Some(change)) = change
                && let Some(callback) = &callback
            {
                callback(&change, window, cx);
            }
        });
        self.accessibility.disabled = availability.disabled;
        if self.accessibility.label.is_none() && !self.label.is_empty() {
            self.accessibility.label = Some(self.label);
        }
        let root = popup_trigger.install(root, &guard, &focus);
        let root = popup_trigger.apply(self.accessibility.apply(root));
        // Keep a concrete semantic element through RenderOnce. Erasing it into
        // AnyElement here would hide its role from native accessibility adapters.
        FocusRing::new(
            button_style.focus_color.opacity(button_opacity),
            px(if button && !hidden && self.group_tab_stop.is_none() {
                button_style.focus_width as f32
            } else {
                0.
            }),
        )
        .apply(root, &focus)
        .corners(button_radii)
    }
}

fn set_pressed(state: &gpui::WeakEntity<RadioState>, pressed: bool, cx: &mut App) {
    let _ = state.update(cx, |state, cx| {
        let pressed = pressed && state.guard.availability().can_activate();
        if state.pressed != pressed {
            state.pressed = pressed;
            cx.notify();
        }
    });
}

fn border_widths(style: &StyleRefinement, window: &Window) -> gpui::Edges<gpui::Pixels> {
    gpui::Edges {
        top: style.border_widths.top.unwrap_or_default(),
        bottom: style.border_widths.bottom.unwrap_or_default(),
        left: style.border_widths.left.unwrap_or_default(),
        right: style.border_widths.right.unwrap_or_default(),
    }
    .to_pixels(window.rem_size())
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

fn outer_overlay(borders: gpui::Edges<gpui::Pixels>) -> gpui::Div {
    div()
        .absolute()
        .top(-borders.top)
        .bottom(-borders.bottom)
        .left(-borders.left)
        .right(-borders.right)
}

fn wave_overlay(
    borders: gpui::Edges<gpui::Pixels>,
    radii: gpui::Corners<gpui::Pixels>,
    color: Hsla,
    wave: WaveSample,
) -> gpui::Div {
    let spread = px(wave.spread);
    let color = color.opacity(wave.opacity);
    outer_overlay(borders).child(
        canvas(
            |_, _, _| (),
            move |bounds, (), window, _| {
                if spread > px(0.) && color.a > 0.001 {
                    window.paint_quad(
                        gpui::outline(bounds.dilate(spread), color, Default::default())
                            .border_widths(spread)
                            .corner_radii(
                                radii
                                    .clamp_radii_for_quad_size(bounds.size)
                                    .map(|r| *r + spread),
                            ),
                    );
                }
            },
        )
        .size_full(),
    )
}

fn interpolate_color(from: Hsla, to: Hsla, progress: f32) -> Hsla {
    if from == to || progress >= 1. {
        return to;
    }
    if progress <= 0. {
        return from;
    }
    let from = from.to_rgb();
    let to = to.to_rgb();
    let p = progress.clamp(0., 1.);
    // CSS interpolates colors in premultiplied sRGB. In particular, a blue
    // checked border fades to transparent without turning black on the way.
    let alpha = from.a + (to.a - from.a) * p;
    let channel = |from_channel: f32, to_channel: f32| {
        if alpha > f32::EPSILON {
            (from_channel * from.a * (1. - p) + to_channel * to.a * p) / alpha
        } else {
            0.
        }
    };
    gpui::Rgba {
        r: channel(from.r, to.r),
        g: channel(from.g, to.g),
        b: channel(from.b, to.b),
        a: alpha,
    }
    .into()
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod layout_tests;

#[cfg(test)]
mod reference_tests;
