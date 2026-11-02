// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Ant Design switches. Use a stable ID in each render.
//!
//! `checked` is controlled; `default_checked` initializes local state on mount.
//! Pointer, Space, Enter, arrow keys and accessible actions share one change path.
//! Loading is disabled, including removal from keyboard traversal, as in Ant Design.
//! Labels and custom content are decorative; supply a stable accessible label.
//!
//! Switch opacity is composited over the theme's uniform container backdrop.
//! Use [`Switch::opacity_backdrop`] on a different opaque, uniform surface. The
//! pinned GPUI backend does not provide isolated subtree layers: a nonuniform
//! backdrop, ancestor opacity, or custom painting outside the layout box and
//! declared shadow extents cannot reproduce CSS group opacity exactly.

mod geometry;
mod opacity;
mod style;
pub use geometry::{SwitchGeometry, SwitchLayout, SwitchOffset};
pub use style::{SwitchStyle, SwitchTheme, SwitchTokenPatch, SwitchTokens};

use std::{rc::Rc, time::Duration};

use gpui::{
    AnyElement, App, Context, ElementId, FocusHandle, Hsla, IntoElement, MouseButton, Refineable,
    RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, Toggled, Window, canvas,
    div, point, prelude::*, px, radians, relative, size,
};

use crate::{
    ConfigScope, Size,
    behavior::{
        Accessibility, Activation, ActivationGuard, Availability, FocusRing, MotionPolicy,
        ScalarTransition, WaveMotion, WaveState, focus_handle, wave_color,
    },
    icons::{IconName, IconPalette, IconRenderOptions, IconRenderer},
    theme::{Color, CubicBezier},
};

/// A proposed checked value and the input which requested it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SwitchChange {
    pub checked: bool,
    pub activation: Activation,
}

type Handler = Box<dyn Fn(&SwitchChange, &mut Window, &mut App)>;
const EASE: CubicBezier = CubicBezier::new(0.25, 0.1, 0.25, 1.);
const EASE_IN_OUT: CubicBezier = CubicBezier::new(0.42, 0., 0.58, 1.);

#[derive(IntoElement)]
pub struct Switch {
    id: ElementId,
    scope: Option<ConfigScope>,
    checked: Option<bool>,
    default_checked: bool,
    disabled: Option<bool>,
    loading: bool,
    size: Option<Size>,
    checked_label: SharedString,
    unchecked_label: SharedString,
    checked_content: Option<AnyElement>,
    unchecked_content: Option<AnyElement>,
    focus: Option<FocusHandle>,
    accessibility: Accessibility,
    on_change: Option<Handler>,
    on_click: Option<Handler>,
    wave: bool,
    style: StyleRefinement,
    content_style: Option<Box<StyleRefinement>>,
    indicator_style: Option<Box<StyleRefinement>>,
    opacity_backdrop: Option<Color>,
    form_registration: Option<(crate::form::FieldHandle, crate::form::ControlToken)>,
}

impl crate::form::FormControl<bool> for Switch {
    fn into_form_element(
        mut self,
        field: crate::form::FormField<bool>,
        context: crate::form::FormItemContext,
        window: &mut Window,
        cx: &mut App,
    ) -> gpui::AnyElement {
        use crate::form::{FormControlBinding, FormPresence, ValidationTrigger};
        let (value, focus, token) = match FormControlBinding::controlled(
            self.id.clone(),
            &field,
            vec![(self.id.clone(), self.focus.clone())],
            window,
            cx,
        ) {
            Ok(value) => value,
            Err(error) => return crate::form::binding_error(context.id, &error),
        };
        self.checked = Some(value.value().unwrap_or(false));
        self.focus = focus.into_iter().next();
        self.form_registration = Some((field.handle(), token));
        self.scope = self.scope.or_else(|| Some(context.scope.clone()));
        self.disabled = self.disabled.or(Some(context.disabled));
        self.size = self.size.or(Some(context.size));
        self.accessibility.in_form(&context);
        let handler = self.on_change.take();
        self.on_change = Some(Box::new(move |change: &SwitchChange, window, cx| {
            let _ = field.change(
                FormPresence::Value(change.checked),
                ValidationTrigger::Change,
                window,
                cx,
            );
            if let Some(handler) = &handler {
                handler(change, window, cx);
            }
        }));
        self.into_any_element()
    }
}

impl Switch {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            checked: None,
            default_checked: false,
            disabled: None,
            loading: false,
            size: None,
            checked_label: SharedString::default(),
            unchecked_label: SharedString::default(),
            checked_content: None,
            unchecked_content: None,
            focus: None,
            accessibility: Accessibility::new(Role::Switch),
            on_change: None,
            on_click: None,
            wave: true,
            style: StyleRefinement::default(),
            content_style: None,
            indicator_style: None,
            opacity_backdrop: None,
            form_registration: None,
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    /// Propose changes through `on_change`; the owner supplies the new checked value.
    pub fn checked(mut self, checked: bool) -> Self {
        self.checked = Some(checked);
        self
    }
    /// Initialize local state once, when this ID mounts.
    pub fn default_checked(mut self, checked: bool) -> Self {
        self.default_checked = checked;
        self
    }
    /// Explicit false overrides the provider. Loading still disables the switch.
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    pub fn loading(mut self, loading: bool) -> Self {
        self.loading = loading;
        self
    }
    /// Small uses the small track; Medium and Large use the normal track.
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }
    pub fn checked_label(mut self, label: impl Into<SharedString>) -> Self {
        self.checked_label = label.into();
        self.checked_content = None;
        self
    }
    pub fn unchecked_label(mut self, label: impl Into<SharedString>) -> Self {
        self.unchecked_label = label.into();
        self.unchecked_content = None;
        self
    }
    /// Decorative content, measured alongside the unchecked slot to keep width stable.
    pub fn checked_content(mut self, content: impl IntoElement) -> Self {
        self.checked_content = Some(content.into_any_element());
        self.checked_label = SharedString::default();
        self
    }
    pub fn unchecked_content(mut self, content: impl IntoElement) -> Self {
        self.unchecked_content = Some(content.into_any_element());
        self.unchecked_label = SharedString::default();
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
        handler: impl Fn(&SwitchChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Box::new(handler));
        self
    }
    /// Runs after `on_change` for pointer, Space, Enter and accessible invocation.
    /// Arrow keys only emit changes, matching the source control.
    pub fn on_click(
        mut self,
        handler: impl Fn(&SwitchChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_click = Some(Box::new(handler));
        self
    }
    pub fn wave(mut self, enabled: bool) -> Self {
        self.wave = enabled;
        self
    }
    pub fn content_style(mut self, style: StyleRefinement) -> Self {
        self.content_style = Some(Box::new(style));
        self
    }
    pub fn indicator_style(mut self, style: StyleRefinement) -> Self {
        self.indicator_style = Some(Box::new(style));
        self
    }
    /// Opaque, uniform surface behind this switch, used for group opacity.
    ///
    /// The default is the current theme's `color_bg_container`. The RGB channels
    /// identify an opaque surface; the supplied color's alpha is ignored. This
    /// also covers decorative content inside the layout box and declared shadow
    /// extents. Keep arbitrary custom painting inside those bounds.
    pub fn opacity_backdrop(mut self, color: Color) -> Self {
        self.opacity_backdrop = Some(color.with_alpha(1.));
        self
    }
}

impl Styled for Switch {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct SwitchState {
    scope: ConfigScope,
    focus: FocusHandle,
    guard: ActivationGuard,
    activate: Option<crate::form::NativeActivation>,
    checked: bool,
    controlled: bool,
    disabled: Option<bool>,
    loading: bool,
    hovered: bool,
    pointer_pressed: bool,
    keyboard_pressed: bool,
    position: ScalarTransition,
    press: ScalarTransition,
    color_from: Option<Hsla>,
    color_target: Option<Hsla>,
    color_progress: ScalarTransition,
    wave: WaveState,
    frame_pending: bool,
    clock: Box<dyn Fn(&App) -> Duration>,
    _config: Subscription,
    _blur: Subscription,
}

impl SwitchState {
    fn new(
        control: &Switch,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let focus = control.focus.clone().unwrap_or_else(|| focus_handle(cx));
        let guard = ActivationGuard::default();
        let config = Self::observe_scope(&scope, window, cx);
        let blur = Self::observe_blur(&focus, window, cx);
        let epoch = cx.background_executor().now();
        let checked = control.checked.unwrap_or(control.default_checked);
        Self {
            scope,
            focus,
            guard,
            activate: None,
            checked,
            controlled: control.checked.is_some(),
            disabled: control.disabled,
            loading: control.loading,
            hovered: false,
            pointer_pressed: false,
            keyboard_pressed: false,
            position: ScalarTransition::new(f64::from(checked), EASE_IN_OUT),
            press: ScalarTransition::new(0., EASE_IN_OUT),
            color_from: None,
            color_target: None,
            color_progress: ScalarTransition::new(1., EASE),
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
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        cx.on_focus_out(focus, window, |this, _, _, cx| {
            this.guard.cancel();
            if this.pointer_pressed || this.keyboard_pressed {
                this.pointer_pressed = false;
                this.keyboard_pressed = false;
                cx.notify();
            }
        })
    }

    fn sync_availability(&mut self, window: &mut Window, cx: &mut App) {
        let disabled = self.disabled.unwrap_or(self.scope.snapshot(cx).disabled) || self.loading;
        self.guard.update(
            Availability {
                disabled,
                busy: self.loading,
            },
            &self.focus,
            window,
            cx,
        );
        if disabled {
            self.pointer_pressed = false;
            self.keyboard_pressed = false;
            self.wave.clear();
        }
    }

    fn update(
        &mut self,
        control: &Switch,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope != scope {
            self.guard.cancel();
            self.pointer_pressed = false;
            self.keyboard_pressed = false;
            self._config = Self::observe_scope(&scope, window, cx);
            self.scope = scope;
        }
        if let Some(focus) = &control.focus
            && self.focus != *focus
        {
            self.guard.cancel();
            self.pointer_pressed = false;
            self.keyboard_pressed = false;
            self._blur = Self::observe_blur(focus, window, cx);
            self.focus = focus.clone();
        }
        if let Some(checked) = control.checked {
            self.checked = checked;
        }
        self.controlled = control.checked.is_some();
        self.disabled = control.disabled;
        self.loading = control.loading;
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

    fn color(&self, now: Duration) -> Hsla {
        let to = self.color_target.expect("initialized Switch color");
        mix_color(
            self.color_from.unwrap_or(to),
            to,
            self.color_progress.sample(now).value as f32,
        )
    }
}

impl RenderOnce for Switch {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let popup_trigger = crate::popup::PopupTrigger::current(cx);
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            SwitchState::new(&self, scope.clone(), window, cx)
        });
        state.update(cx, |state, cx| {
            state.update(&self, scope.clone(), window, cx)
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<SwitchTheme>();
        let global = tokens.globals();
        let motion = MotionPolicy::new(config.motion && global.motion, cx.reduce_motion());
        let retained = state.read(cx);
        let checked = retained.checked;
        let focus = retained.focus.clone();
        let guard = retained.guard.clone();
        let availability = guard.availability();
        let metrics = SwitchStyle::from_tokens(
            &tokens,
            self.size.unwrap_or(config.size),
            checked,
            availability.disabled,
            self.loading,
        );
        // Inline opacity overrides the disabled CSS declaration. Apply the
        // resolved value once after native content instead of multiplying each
        // overlapping track, shadow and handle primitive independently.
        let opacity = self.style.opacity.take().unwrap_or(metrics.opacity as f32);
        let opacity = if opacity.is_finite() {
            opacity.clamp(0., 1.)
        } else {
            1.
        };
        let opacity_backdrop = self.opacity_backdrop.unwrap_or(global.color_bg_container);
        let mut opacity_outset =
            opacity_shadow_outset(&metrics, &self.style, self.indicator_style.as_deref());
        opacity_outset = opacity_outset.max(metrics.focus_width as f32 + 1.);
        let target_color = if retained.hovered {
            metrics.hover_background
        } else {
            metrics.background
        };
        let duration = global.motion_duration_mid;
        let (position, press, background, has_active_wave, spin) = state.update(cx, |state, cx| {
            let now = (state.clock)(cx);
            state
                .position
                .retarget(f64::from(checked), now, duration, motion);
            state.press.retarget(
                f64::from(state.pointer_pressed || state.keyboard_pressed),
                now,
                duration,
                motion,
            );
            if state.color_target != Some(target_color) {
                state.color_from = Some(
                    state
                        .color_target
                        .map_or(target_color, |_| state.color(now)),
                );
                state.color_target = Some(target_color);
                state.color_progress = ScalarTransition::new(0., EASE);
                state.color_progress.retarget(
                    1.,
                    now,
                    if state.color_from == state.color_target {
                        Duration::ZERO
                    } else {
                        duration
                    },
                    motion,
                );
            } else if !motion.allows_motion() {
                state
                    .color_progress
                    .retarget(1., now, Duration::ZERO, motion);
            }
            let position = state.position.sample(now);
            let press = state.press.sample(now);
            let wave = state
                .wave
                .sample(now, WaveMotion::Regular, global, motion, self.wave);
            let spinning = self.loading && motion.allows_motion();
            if !position.finished
                || !press.finished
                || !state.color_progress.sample(now).finished
                || wave
                || spinning
            {
                state.request_frame(window, cx);
            }
            (
                position.value as f32,
                press.value as f32,
                state.color(now),
                wave,
                if spinning {
                    now.as_secs_f64().fract() as f32
                } else {
                    0.
                },
            )
        });

        let height = metrics.track_height as f32;
        let handle = metrics.handle_size as f32;
        let padding = metrics.track_padding as f32;
        let layout = metrics.layout(f64::from(position), f64::from(press), config.direction);
        let has_content = !self.checked_label.is_empty()
            || !self.unchecked_label.is_empty()
            || self.checked_content.is_some()
            || self.unchecked_content.is_some()
            || self.content_style.is_some();
        // An empty switch needs only its track and handle. Preserve the content
        // strut's intrinsic width without constructing two empty layout slots.
        let inner = if has_content {
            let mut inner = div()
                .grid()
                .grid_cols_min_content(1)
                .h_full()
                .rounded(px(100.))
                .overflow_hidden();
            for is_checked in [true, false] {
                let content = if is_checked {
                    self.checked_content.take()
                } else {
                    self.unchecked_content.take()
                };
                let label = if is_checked {
                    self.checked_label.clone()
                } else {
                    self.unchecked_label.clone()
                };
                let (left, right, offset) = if is_checked {
                    (
                        layout.checked_padding_left,
                        layout.checked_padding_right,
                        layout.checked_slot_left,
                    )
                } else {
                    (
                        layout.unchecked_padding_left,
                        layout.unchecked_padding_right,
                        layout.unchecked_slot_left,
                    )
                };
                let mut slot = div()
                    .id(if is_checked {
                        "checked-content"
                    } else {
                        "unchecked-content"
                    })
                    .relative()
                    .col_start(1)
                    .row_start(1)
                    .flex()
                    .items_center()
                    .justify_center()
                    .text_size(px(metrics.font_size as f32))
                    .text_color(metrics.content_color)
                    .h_full()
                    .pl(px(left as f32))
                    .pr(px(right as f32))
                    .whitespace_nowrap()
                    .left(relative(offset.fraction as f32))
                    .ml(px(offset.pixels as f32))
                    .mr(px(-offset.pixels as f32));
                if let Some(refinement) = &self.content_style {
                    slot.style().refine(refinement);
                }
                slot = if let Some(content) = content {
                    slot.child(content)
                } else {
                    slot.child(label)
                };
                inner = inner.child(
                    Accessibility {
                        role: Some(Role::Group),
                        hidden: true,
                        ..Default::default()
                    }
                    .apply(slot),
                );
            }
            Some(inner)
        } else {
            None
        };

        let mut indicator = div()
            .absolute()
            .top(px(padding))
            .size(px(handle))
            .left(relative(layout.handle_left.fraction as f32))
            .ml(px(layout.handle_left.pixels as f32));
        if let Some(refinement) = &self.indicator_style {
            indicator.style().refine(refinement);
        }
        let mut surface = div().rounded(px(handle / 2.)).bg(metrics.handle_background);
        if !metrics.handle_shadow.is_empty() {
            surface = surface.shadow(
                metrics
                    .handle_shadow
                    .iter()
                    .map(|s| gpui::BoxShadow {
                        inset: s.inset,
                        color: s.color.into(),
                        offset: point(px(s.x as f32), px(s.y as f32)),
                        // CSS box-shadow blur is twice the Gaussian sigma used
                        // by GPUI's shadow shader (css-backgrounds-3 §6.1.2).
                        blur_radius: px((s.blur * 0.5) as f32),
                        spread_radius: px(s.spread as f32),
                    })
                    .collect::<Vec<_>>(),
            );
        }
        if press == 0. && self.indicator_style.is_none() {
            // The resting handle can paint directly. A separate surface is only
            // needed for a stretching handle or independently styled indicator.
            indicator.style().refine(surface.style());
        } else {
            indicator = indicator.child(
                surface
                    .absolute()
                    .top_0()
                    .bottom_0()
                    .left(px(-layout.handle_extension_left as f32))
                    .right(px(-layout.handle_extension_right as f32)),
            );
        }
        if self.loading {
            let icon_size = metrics.loading_icon_size as f32;
            let icon_color = metrics.loading_icon_color;
            let options = IconRenderOptions {
                size: size(px(icon_size), px(icon_size)),
                rotation: radians(spin * std::f32::consts::TAU),
                ..Default::default()
            };
            indicator = indicator.child(
                div()
                    .absolute()
                    .inset_0()
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(
                        canvas(
                            |_, _, _| (),
                            move |bounds, (), window, cx| {
                                if cx.try_global::<IconRenderer>().is_none() {
                                    cx.set_global(IconRenderer::default());
                                }
                                let options = IconRenderOptions {
                                    colors: IconPalette {
                                        text: icon_color.into(),
                                        ..options.colors
                                    },
                                    ..options
                                };
                                cx.update_global::<IconRenderer, _>(|renderer, cx| {
                                    let _ = renderer.paint(
                                        bounds,
                                        &IconName::LoadingOutlined.into(),
                                        &options,
                                        window,
                                        cx,
                                    );
                                });
                            },
                        )
                        .size(px(icon_size)),
                    ),
            );
        }

        let mut root = div()
            .id(self.id)
            .relative()
            .grid()
            .grid_cols_min_content(1)
            .flex_none()
            .min_w(px(if has_content {
                metrics.track_min_width
            } else {
                metrics.track_min_width.max(layout.content_inset)
            } as f32))
            .h(px(height))
            .rounded(px(100.))
            .font(tokens.font().clone())
            .text_size(px(global.font_size as f32))
            .line_height(px(height))
            .text_color(global.color_text)
            .bg(background)
            .when(availability.can_focus(), |root| root.track_focus(&focus))
            .aria_toggled(if checked {
                Toggled::True
            } else {
                Toggled::False
            })
            .children(inner)
            .child(indicator);
        root.style().refine(&self.style);
        if self.style.mouse_cursor.is_none() {
            root = if availability.disabled {
                root.cursor_not_allowed()
            } else {
                root.cursor_pointer()
            };
        }
        let radii = gpui::Corners {
            top_left: root.style().corner_radii.top_left.unwrap_or_default(),
            top_right: root.style().corner_radii.top_right.unwrap_or_default(),
            bottom_left: root.style().corner_radii.bottom_left.unwrap_or_default(),
            bottom_right: root.style().corner_radii.bottom_right.unwrap_or_default(),
        }
        .to_pixels(window.rem_size());
        if has_active_wave {
            let color = wave_color(root.style(), global.color_primary.to_hsla());
            for (wave, color) in state.update(cx, |state, _| state.wave.paint_samples(color)) {
                let spread = wave.spread;
                opacity_outset = opacity_outset.max(spread);
                let color = color.opacity(wave.opacity);
                root = root.child(
                    div().absolute().inset_0().child(
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
        let weak = state.downgrade();
        root = root.on_hover(move |hovered, _, cx| {
            let _ = weak.update(cx, |state, cx| {
                if state.hovered != *hovered {
                    state.hovered = *hovered;
                    cx.notify();
                }
            });
        });
        for down in [true, false] {
            let weak = state.downgrade();
            if down {
                root = root.on_mouse_down(MouseButton::Left, move |_, _, cx| {
                    set_pressed(&weak, true, false, cx)
                });
            } else {
                root = root.on_mouse_up(MouseButton::Left, move |_, _, cx| {
                    set_pressed(&weak, false, false, cx)
                });
            }
        }
        let weak = state.downgrade();
        root = root.on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
            set_pressed(&weak, false, false, cx)
        });
        let weak = state.downgrade();
        root = root.capture_key_down(move |event, _, cx| {
            if !event.is_held
                && !event.keystroke.modifiers.modified()
                && matches!(event.keystroke.key.as_str(), "space" | "enter")
            {
                set_pressed(&weak, true, true, cx);
            }
        });
        let weak = state.downgrade();
        root = root.on_key_up(move |_, _, cx| set_pressed(&weak, false, true, cx));
        let callback = Rc::new(self.on_change);
        let arrows = callback.clone();
        let weak = state.downgrade();
        root = root.on_key_down(move |event, window, cx| {
            if event.keystroke.modifiers.modified()
                || !matches!(event.keystroke.key.as_str(), "left" | "right")
            {
                return;
            }
            let next = event.keystroke.key == "right";
            let changed = request_change(
                &weak,
                Some(next),
                Activation::KeyboardNavigation,
                false,
                motion,
                cx,
            );
            if let Some(change) = changed
                && let Some(callback) = &*arrows
            {
                callback(&change, window, cx);
            }
            cx.stop_propagation();
        });
        let weak = state.downgrade();
        let wave = self.wave;
        let click = self.on_click;
        let activate: crate::form::NativeActivation = Rc::new(move |activation, window, cx| {
            if let Some(change) = request_change(&weak, None, activation, wave, motion, cx) {
                if let Some(callback) = &*callback {
                    callback(&change, window, cx);
                }
                if let Some(click) = &click {
                    click(&change, window, cx);
                }
            }
        });
        state.update(cx, |state, _| state.activate = Some(activate.clone()));
        if let Some((field, token)) = &self.form_registration {
            crate::form::FormControlBinding::install_native_actions(
                field,
                *token,
                &state,
                |state, window, cx| {
                    state.sync_availability(window, cx);
                    if state.guard.availability().can_focus() {
                        state.focus.focus(window, cx);
                        true
                    } else {
                        false
                    }
                },
                Some(|state| state.activate.clone()),
                cx,
            );
        }
        root = guard.install(root, move |activation, window, cx| {
            activate(activation, window, cx)
        });
        self.accessibility.disabled = availability.disabled;
        self.accessibility.busy |= self.loading;
        if self.loading {
            self.accessibility.busy_allows_activation = false;
        }
        let root = popup_trigger.install(root, &guard, &focus);
        let root = popup_trigger.apply(self.accessibility.apply(root));
        opacity::OpacityGroup {
            element: FocusRing::new(metrics.focus_color, px(metrics.focus_width as f32))
                .radius(px(100.))
                .apply(root, &focus)
                .corners(radii),
            backdrop: opacity_backdrop,
            opacity,
            outset: opacity_outset,
        }
    }
}

fn opacity_shadow_outset(
    metrics: &SwitchStyle,
    root: &StyleRefinement,
    indicator: Option<&StyleRefinement>,
) -> f32 {
    // GPUI's shadow shader bounds its Gaussian support at three blur radii.
    // A conservative symmetric extent avoids clipping an offset custom shadow.
    let native = |style: &StyleRefinement| {
        style
            .box_shadow
            .as_ref()
            .into_iter()
            .flatten()
            .filter(|shadow| !shadow.inset)
            .map(|shadow| {
                3. * f32::from(shadow.blur_radius).max(0.)
                    + f32::from(shadow.spread_radius).max(0.)
                    + f32::from(shadow.offset.x)
                        .abs()
                        .max(f32::from(shadow.offset.y).abs())
            })
            .fold(0., f32::max)
    };
    let token = metrics
        .handle_shadow
        .iter()
        .filter(|shadow| !shadow.inset)
        .map(|shadow| {
            (1.5 * shadow.blur.max(0.) + shadow.spread.max(0.) + shadow.x.abs().max(shadow.y.abs()))
                as f32
        })
        .fold(0., f32::max);
    token.max(native(root)).max(indicator.map_or(0., native))
}

fn set_pressed(state: &gpui::WeakEntity<SwitchState>, pressed: bool, keyboard: bool, cx: &mut App) {
    let _ = state.update(cx, |state, cx| {
        let pressed = pressed && state.guard.availability().can_activate();
        let current = if keyboard {
            &mut state.keyboard_pressed
        } else {
            &mut state.pointer_pressed
        };
        if *current != pressed {
            *current = pressed;
            cx.notify();
        }
    });
}

fn request_change(
    state: &gpui::WeakEntity<SwitchState>,
    next: Option<bool>,
    activation: Activation,
    wave: bool,
    motion: MotionPolicy,
    cx: &mut App,
) -> Option<SwitchChange> {
    state
        .update(cx, |state, cx| {
            if !state.guard.availability().can_activate() {
                return None;
            }
            let checked = next.unwrap_or(!state.checked);
            if checked == state.checked {
                return None;
            }
            if !state.controlled {
                state.checked = checked;
            }
            if wave && motion.allows_motion() {
                state.wave.start((state.clock)(cx));
            }
            cx.notify();
            Some(SwitchChange {
                checked,
                activation,
            })
        })
        .ok()
        .flatten()
}

fn mix_color(from: Hsla, to: Hsla, progress: f32) -> Hsla {
    if from == to || progress >= 1. {
        return to;
    }
    if progress <= 0. {
        return from;
    }
    let from = from.to_rgb();
    let to = to.to_rgb();
    let alpha = from.a + (to.a - from.a) * progress;
    let channel = |a: f32, b: f32| {
        if alpha > f32::EPSILON {
            (a * from.a * (1. - progress) + b * to.a * progress) / alpha
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
mod reference_tests;
#[cfg(test)]
mod tests;
