// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Ant Design checkboxes. Build with a stable ID in each render.
//!
//! `checked` is controlled; `default_checked` initializes retained local state once.
//! Mixed state is independent of the checked value, as in Ant Design. Space, pointer
//! clicks (including the label), and accessible invocation share one guarded change
//! callback. Applications localize their own labels.

mod group;
mod paint;
mod style;

pub use group::{CheckboxGroup, CheckboxOption};
pub use style::{
    CheckboxStateStyle, CheckboxStyle, CheckboxTheme, CheckboxTokenPatch, CheckboxTokens,
};

use std::time::Duration;

use gpui::{
    App, Context, ElementId, FocusHandle, Hsla, IntoElement, KeyboardButton, Refineable,
    RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, Toggled, Window, canvas,
    div, prelude::*, px,
};

use crate::{
    ConfigScope, Direction,
    behavior::{
        Accessibility, Activation, ActivationGuard, FocusRing, MotionPolicy, ScalarTransition,
        WaveMotion, WaveState, focus_handle, wave_color,
    },
    theme::{AliasTokens, CubicBezier},
};
use paint::CheckmarkCache;

/// A proposed checked value, independent of the mixed-state presentation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CheckboxChange {
    pub checked: bool,
    pub activation: Activation,
}

type ChangeHandler = Box<dyn Fn(&CheckboxChange, &mut Window, &mut App)>;
const CSS_EASE: CubicBezier = CubicBezier::new(0.25, 0.1, 0.25, 1.);

/// An immediate builder backed by a small retained state for each mounted ID.
#[derive(IntoElement)]
pub struct Checkbox {
    id: ElementId,
    scope: Option<ConfigScope>,
    label: SharedString,
    checked: Option<bool>,
    default_checked: bool,
    indeterminate: bool,
    disabled: Option<bool>,
    focus: Option<FocusHandle>,
    accessibility: Accessibility,
    on_change: Option<ChangeHandler>,
    wave: bool,
    style: StyleRefinement,
    icon_style: Option<Box<StyleRefinement>>,
    label_style: Option<Box<StyleRefinement>>,
}

impl Checkbox {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            label: SharedString::default(),
            checked: None,
            default_checked: false,
            indeterminate: false,
            disabled: None,
            focus: None,
            accessibility: Accessibility::new(Role::CheckBox),
            on_change: None,
            wave: true,
            style: StyleRefinement::default(),
            icon_style: None,
            label_style: None,
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

    /// Display a mixed value. Activation still proposes the inverse of `checked`.
    pub fn indeterminate(mut self, indeterminate: bool) -> Self {
        self.indeterminate = indeterminate;
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
        handler: impl Fn(&CheckboxChange, &mut Window, &mut App) + 'static,
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
}

impl Styled for Checkbox {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

#[derive(Default)]
struct VisualTransition {
    from: Option<CheckboxStateStyle>,
    target: Option<CheckboxStateStyle>,
    progress: Option<ScalarTransition>,
}

impl VisualTransition {
    fn sample(&self, now: Duration) -> CheckboxStateStyle {
        let target = self.target.expect("initialized checkbox visual state");
        let progress = self.progress.map_or(1., |p| p.sample(now).value) as f32;
        CheckboxStateStyle {
            // Only the box and its pseudo-element transition; the wrapper's
            // label switches immediately when its inherited color changes.
            label: target.label,
            ..interpolate_style(self.from.unwrap_or(target), target, progress)
        }
    }

    fn resolve(
        &mut self,
        target: CheckboxStateStyle,
        now: Duration,
        duration: Duration,
        curve: CubicBezier,
        motion: MotionPolicy,
    ) -> (CheckboxStateStyle, bool) {
        if self.target != Some(target) {
            self.from = Some(CheckboxStateStyle {
                label: target.label,
                ..self.target.map_or(target, |_| self.sample(now))
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

struct MarkTransition {
    scale: ScalarTransition,
    opacity: ScalarTransition,
}

impl MarkTransition {
    fn new(value: f64, global: &AliasTokens) -> Self {
        Self {
            scale: ScalarTransition::new(value, global.motion_ease_out_back),
            opacity: ScalarTransition::new(value, global.motion_ease_out_back),
        }
    }

    fn resolve(
        &mut self,
        target: f64,
        now: Duration,
        global: &AliasTokens,
        motion: MotionPolicy,
    ) -> (f32, f32, bool) {
        if self.scale.target() != target {
            let showing = target > 0.;
            let scale_curve = if showing {
                global.motion_ease_out_back
            } else {
                global.motion_ease_in_back
            };
            // Ant's hiding transition gives opacity its own default CSS ease,
            // so it fades while the back-eased scale initially overshoots.
            let opacity_curve = if showing { scale_curve } else { CSS_EASE };
            self.scale = ScalarTransition::new(self.scale.sample(now).value, scale_curve);
            self.opacity = ScalarTransition::new(self.opacity.sample(now).value, opacity_curve);
            let delay = if showing && motion.allows_motion() {
                global.motion_duration_fast
            } else {
                Duration::ZERO
            };
            let duration = if showing {
                global.motion_duration_mid
            } else {
                global.motion_duration_fast
            };
            self.scale.retarget(target, now + delay, duration, motion);
            self.opacity.retarget(target, now + delay, duration, motion);
        } else if !motion.allows_motion() {
            self.scale.retarget(target, now, Duration::ZERO, motion);
            self.opacity.retarget(target, now, Duration::ZERO, motion);
        }
        let scale = self.scale.sample(now);
        let opacity = self.opacity.sample(now);
        (
            scale.value.max(0.) as f32,
            opacity.value.clamp(0., 1.) as f32,
            !scale.finished || !opacity.finished,
        )
    }
}

struct CheckboxState {
    scope: ConfigScope,
    focus: FocusHandle,
    guard: ActivationGuard,
    checked: bool,
    controlled: bool,
    disabled: Option<bool>,
    hovered: bool,
    visual: VisualTransition,
    mark: Option<MarkTransition>,
    wave: WaveState,
    frame_pending: bool,
    clock: Box<dyn Fn(&App) -> Duration>,
    checkmark: CheckmarkCache,
    _config: Subscription,
    _blur: Subscription,
}

impl CheckboxState {
    fn new(
        checkbox: &Checkbox,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let focus = checkbox.focus.clone().unwrap_or_else(|| focus_handle(cx));
        let guard = ActivationGuard::default();
        let config = Self::observe_scope(&scope, window, cx);
        let blur = guard.observe_blur(&focus, window, cx);
        let epoch = cx.background_executor().now();
        Self {
            scope,
            focus,
            guard,
            checked: checkbox.checked.unwrap_or(checkbox.default_checked),
            controlled: checkbox.checked.is_some(),
            disabled: checkbox.disabled,
            hovered: false,
            visual: VisualTransition::default(),
            mark: None,
            wave: WaveState::default(),
            frame_pending: false,
            clock: Box::new(move |cx| cx.background_executor().now().duration_since(epoch)),
            checkmark: CheckmarkCache::default(),
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

    fn sync_availability(&mut self, window: &mut Window, cx: &mut App) {
        let availability = self.scope.snapshot(cx).availability(self.disabled, false);
        self.guard.update(availability, &self.focus, window, cx);
        if !availability.can_activate() {
            self.wave.clear();
        }
    }

    fn update(
        &mut self,
        checkbox: &Checkbox,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope != scope {
            self.guard.cancel();
            self._config = Self::observe_scope(&scope, window, cx);
            self.scope = scope;
        }
        if let Some(focus) = &checkbox.focus
            && self.focus != *focus
        {
            self.guard.cancel();
            self._blur = self.guard.observe_blur(focus, window, cx);
            self.focus = focus.clone();
        }
        if let Some(checked) = checkbox.checked {
            self.checked = checked;
        }
        self.controlled = checkbox.checked.is_some();
        self.disabled = checkbox.disabled;
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

impl RenderOnce for Checkbox {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let popup_trigger = crate::popup::PopupTrigger::current(cx);
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            CheckboxState::new(&self, scope.clone(), window, cx)
        });
        state.update(cx, |state, cx| {
            state.update(&self, scope.clone(), window, cx)
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<CheckboxTheme>();
        let global = tokens.globals();
        let motion = MotionPolicy::new(config.motion && global.motion, cx.reduce_motion());
        let retained = state.read(cx);
        let checked = retained.checked;
        let focus = retained.focus.clone();
        let guard = retained.guard.clone();
        let availability = guard.availability();
        let style =
            CheckboxStyle::from_tokens(&tokens, checked, self.indeterminate, availability.disabled);
        let target = if retained.hovered {
            style.hover
        } else {
            style.rest
        };
        let mark_target = f64::from(checked && !self.indeterminate);
        let (current, mark_progress, mark_opacity, has_active_wave) =
            state.update(cx, |state, cx| {
                let now = (state.clock)(cx);
                let (colors, color_animating) = state.visual.resolve(
                    target,
                    now,
                    global.motion_duration_slow,
                    CSS_EASE,
                    motion,
                );
                let mark = state
                    .mark
                    .get_or_insert_with(|| MarkTransition::new(mark_target, global));
                let (mark_scale, mark_opacity, mark_animating) =
                    mark.resolve(mark_target, now, global, motion);
                // Only Checkbox suppresses the wave when the owner leaves it unchecked.
                if !checked {
                    state.wave.discard_pending();
                }
                let wave = state
                    .wave
                    .sample(now, WaveMotion::Quick, global, motion, self.wave);
                if color_animating || mark_animating || wave {
                    state.request_frame(window, cx);
                }
                (colors, mark_scale, mark_opacity, wave)
            });

        let mut icon = div()
            .relative()
            .flex_none()
            .size(px(style.size as f32))
            .border(px(style.line_width as f32))
            .rounded(px(style.radius as f32))
            .bg(current.background)
            .border_color(current.border);
        if let Some(refinement) = &self.icon_style {
            icon.style().refine(refinement);
        }
        let effective = icon.style();
        let borders = gpui::Edges {
            top: effective.border_widths.top.unwrap_or_default(),
            bottom: effective.border_widths.bottom.unwrap_or_default(),
            left: effective.border_widths.left.unwrap_or_default(),
            right: effective.border_widths.right.unwrap_or_default(),
        }
        .to_pixels(window.rem_size());
        let radii = gpui::Corners {
            top_left: effective.corner_radii.top_left.unwrap_or_default(),
            top_right: effective.corner_radii.top_right.unwrap_or_default(),
            bottom_left: effective.corner_radii.bottom_left.unwrap_or_default(),
            bottom_right: effective.corner_radii.bottom_right.unwrap_or_default(),
        }
        .to_pixels(window.rem_size());
        // The root's opacity is already active while painting this child. The
        // icon's own opacity ends before the external focus ring is painted.
        let focus_opacity = effective.opacity.unwrap_or(1.);
        let wave_color = wave_color(effective, global.color_primary.to_hsla());
        let overlay = || {
            div()
                .absolute()
                .top(-borders.top)
                .bottom(-borders.bottom)
                .left(-borders.left)
                .right(-borders.right)
        };
        if self.indeterminate {
            icon = icon.child(
                overlay().flex().items_center().justify_center().child(
                    div()
                        .size(px(style.indeterminate_size as f32))
                        .bg(current.mark),
                ),
            );
        } else if mark_progress > 0.001 {
            let weak = state.downgrade();
            let mark_width = px(style.mark_width as f32);
            icon = icon.child(
                overlay().child(
                    canvas(
                        |_, _, _| (),
                        move |bounds, (), window, cx| {
                            let _ = weak.update(cx, |state, _| {
                                state.checkmark.paint(
                                    bounds,
                                    mark_width,
                                    mark_progress,
                                    current.mark.opacity(mark_opacity),
                                    window,
                                );
                            });
                        },
                    )
                    .size_full(),
                ),
            );
        }
        if has_active_wave {
            for (wave, wave_color) in
                state.update(cx, |state, _| state.wave.paint_samples(wave_color))
            {
                let spread = wave.spread;
                let color = wave_color.opacity(wave.opacity);
                icon = icon.child(
                    overlay().child(
                        canvas(
                            |_, _, _| (),
                            move |bounds, (), window, _| {
                                if spread > 0. && color.a > 0.001 {
                                    window.paint_quad(
                                        gpui::outline(
                                            bounds.dilate(px(spread)),
                                            color,
                                            Default::default(),
                                        )
                                        .border_widths(px(spread))
                                        .corner_radii(
                                            radii
                                                .clamp_radii_for_quad_size(bounds.size)
                                                .map(|r| *r + px(spread)),
                                        ),
                                    );
                                }
                            },
                        )
                        .size_full(),
                    ),
                );
            }
        }
        let icon = FocusRing::new(
            style.focus_color.opacity(focus_opacity),
            px(style.focus_width as f32),
        )
        .radius(px(style.radius as f32))
        .apply(icon, &focus)
        .corners(radii);
        let mut root = div()
            .id(self.id)
            .relative()
            .flex()
            .items_center()
            // Ant's zero-width trailing strut retains a line box even without a label.
            .min_h(px(style.line_height as f32))
            .font(tokens.font().clone())
            .text_size(px(style.font_size as f32))
            .line_height(gpui::relative((style.line_height / style.font_size) as f32))
            .text_color(current.label)
            .when(config.direction == Direction::Rtl, |root| {
                root.flex_row_reverse()
            })
            .when(availability.can_focus(), |root| root.track_focus(&focus))
            .aria_toggled(if self.indeterminate {
                Toggled::Mixed
            } else if checked {
                Toggled::True
            } else {
                Toggled::False
            })
            .child(icon);
        if !self.label.is_empty() {
            let mut label = div()
                .min_w_0()
                .px(px(style.label_padding as f32))
                .child(self.label.clone());
            if let Some(refinement) = &self.label_style {
                label.style().refine(refinement);
            }
            root = root.child(label);
        }
        root.style().refine(&self.style);
        if self.style.mouse_cursor.is_none() {
            root = if availability.disabled {
                root.cursor_not_allowed()
            } else {
                root.cursor_pointer()
            };
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
        let callback = self.on_change;
        let wave = self.wave;
        let weak = state.downgrade();
        let root = guard.install(root, move |activation, window, cx| {
            if activation == Activation::Keyboard(KeyboardButton::Enter) {
                return;
            }
            let change = weak.update(cx, |state, cx| {
                // Read live state, so consecutive activations before rendering are sound.
                let change = CheckboxChange {
                    checked: !state.checked,
                    activation,
                };
                if !state.controlled {
                    state.checked = change.checked;
                }
                if wave && motion.allows_motion() {
                    state.wave.start((state.clock)(cx));
                }
                cx.notify();
                change
            });
            if let Ok(change) = change
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
        popup_trigger.apply(self.accessibility.apply(root))
    }
}

fn interpolate_style(
    from: CheckboxStateStyle,
    to: CheckboxStateStyle,
    progress: f32,
) -> CheckboxStateStyle {
    if progress >= 1. || from == to {
        return to;
    }
    if progress <= 0. {
        return from;
    }
    fn color(from: Hsla, to: Hsla, progress: f32) -> Hsla {
        if from == to {
            return to;
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
    CheckboxStateStyle {
        background: color(from.background, to.background, progress),
        border: color(from.border, to.border, progress),
        mark: color(from.mark, to.mark, progress),
        label: color(from.label, to.label, progress),
    }
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod reference_tests;
