// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Ant Design buttons with retained GPUI focus and activation state.
//!
//! Construct a fresh builder on each render using a stable ID. The control owns its
//! internal focus, configuration subscription, and cancellable loading delay while
//! mounted. Labels are application-localized; icon-only controls need an explicit
//! accessible label. Use a scope to inherit a window's configuration.

mod label;
mod paint;
mod style;

pub(crate) use style::ButtonStyleOverride;

pub use style::{
    ButtonColor, ButtonMetrics, ButtonShape, ButtonStateStyle, ButtonStyle, ButtonStyleOptions,
    ButtonTheme, ButtonTokenPatch, ButtonTokens, ButtonType, ButtonVariant, IconPlacement,
};

use std::time::Duration;

use paint::DashedBorderCache;

use gpui::{
    App, BorrowAppContext, Context, ElementId, FocusHandle, FontWeight, IntoElement, MouseButton,
    Refineable, RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, Task,
    Window, canvas, div, point, prelude::*, px, radians, size,
};

use crate::{
    ConfigScope, Direction, Size,
    behavior::{
        Accessibility, Activation, ActivationGuard, FocusRing, MotionPolicy, ScalarTransition,
        WaveMotion, WaveState, focus_handle, wave_color,
    },
    icons::{IconName, IconPalette, IconRef, IconRenderOptions, IconRenderer},
    theme::{Color, CubicBezier, LineType},
};

type ClickHandler = Box<dyn Fn(&Activation, &mut Window, &mut App)>;

/// An immediate-mode builder backed by one small retained state per mounted ID.
#[derive(IntoElement)]
pub struct Button {
    id: ElementId,
    label: SharedString,
    scope: Option<ConfigScope>,
    button_type: ButtonType,
    color: Option<ButtonColor>,
    variant: Option<ButtonVariant>,
    size: Option<Size>,
    shape: ButtonShape,
    disabled: Option<bool>,
    loading: bool,
    loading_delay: Duration,
    icon: Option<IconRef>,
    loading_icon: Option<IconRef>,
    loading_icon_spin: Option<bool>,
    icon_placement: IconPlacement,
    ghost: bool,
    danger: bool,
    block: bool,
    wave: bool,
    auto_insert_space: bool,
    focus: Option<FocusHandle>,
    accessibility: Accessibility,
    href: Option<SharedString>,
    on_click: Option<ClickHandler>,
    style: StyleRefinement,
    icon_style: Option<Box<StyleRefinement>>,
    content_style: Option<Box<StyleRefinement>>,
    group: Option<crate::input::InputGroupContext>,
    visual_override: ButtonStyleOverride,
}

impl crate::input::InputGroupItem for Button {
    fn into_group_element(self, context: crate::input::InputGroupContext) -> gpui::AnyElement {
        self.in_group(context).into_any_element()
    }
}

impl Button {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            label: SharedString::default(),
            scope: None,
            button_type: ButtonType::Default,
            color: None,
            variant: None,
            size: None,
            shape: ButtonShape::Default,
            disabled: None,
            loading: false,
            loading_delay: Duration::ZERO,
            icon: None,
            loading_icon: None,
            loading_icon_spin: None,
            icon_placement: IconPlacement::Start,
            ghost: false,
            danger: false,
            block: false,
            wave: true,
            auto_insert_space: true,
            focus: None,
            accessibility: Accessibility::new(Role::Button),
            href: None,
            on_click: None,
            style: StyleRefinement::default(),
            icon_style: None,
            content_style: None,
            group: None,
            visual_override: ButtonStyleOverride::default(),
        }
    }

    pub fn label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = label.into();
        self
    }

    pub(crate) fn refine_visuals(mut self, refinement: ButtonStyleOverride) -> Self {
        self.visual_override = refinement;
        self
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub(crate) fn in_group(mut self, context: crate::input::InputGroupContext) -> Self {
        self.scope = self.scope.or_else(|| Some(context.scope.clone()));
        self.size = self.size.or(Some(context.size));
        self.disabled = self.disabled.or(context.disabled);
        self.group = Some(context);
        self
    }

    pub fn button_type(mut self, button_type: ButtonType) -> Self {
        self.button_type = button_type;
        self
    }

    pub fn primary(self) -> Self {
        self.button_type(ButtonType::Primary)
    }

    pub fn color(mut self, color: ButtonColor) -> Self {
        self.color = Some(color);
        self
    }

    pub fn variant(mut self, variant: ButtonVariant) -> Self {
        self.variant = Some(variant);
        self
    }

    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }

    pub fn shape(mut self, shape: ButtonShape) -> Self {
        self.shape = shape;
        self
    }

    /// An explicit false overrides a disabled provider.
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }

    /// Loading blocks activation but retains keyboard focus and the tab stop.
    pub fn loading(mut self, loading: bool) -> Self {
        self.loading = loading;
        self
    }

    /// The button remains ready until this delay expires. Removing it or clearing
    /// loading cancels the timer; unrelated rerenders do not restart the delay.
    pub fn loading_delay(mut self, delay: Duration) -> Self {
        self.loading_delay = delay;
        self
    }

    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self
    }

    pub fn loading_icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.loading_icon = Some(icon.into());
        self
    }

    /// Opt a custom loading icon into rotation, or stop the default spinner.
    /// Rotation and frame scheduling respect the effective motion preference.
    /// Monochrome icons reuse a GPU atlas tile; colored rotations use raster variants.
    pub fn loading_icon_spin(mut self, spin: bool) -> Self {
        self.loading_icon_spin = Some(spin);
        self
    }

    /// Refine the icon slot independently of the control and its label.
    pub fn icon_style(mut self, style: StyleRefinement) -> Self {
        self.icon_style = Some(Box::new(style));
        self
    }

    /// Refine the label independently of the control and its icon.
    pub fn content_style(mut self, style: StyleRefinement) -> Self {
        self.content_style = Some(Box::new(style));
        self
    }

    pub fn icon_placement(mut self, placement: IconPlacement) -> Self {
        self.icon_placement = placement;
        self
    }

    pub fn ghost(mut self, ghost: bool) -> Self {
        self.ghost = ghost;
        self
    }

    pub fn danger(mut self, danger: bool) -> Self {
        self.danger = danger;
        self
    }

    pub fn block(mut self, block: bool) -> Self {
        self.block = block;
        self
    }

    /// Enable the standard click wave. Unbordered and link controls have no wave.
    pub fn wave(mut self, enabled: bool) -> Self {
        self.wave = enabled;
        self
    }

    pub fn auto_insert_space(mut self, insert: bool) -> Self {
        self.auto_insert_space = insert;
        self
    }

    /// Supply a retained handle for programmatic focus. Otherwise one is owned
    /// internally for the lifetime of this mounted ID.
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

    /// Expose link semantics and open the URL through GPUI after activation.
    /// The application remains responsible for validating application-supplied URLs.
    pub fn href(mut self, href: impl Into<SharedString>) -> Self {
        self.href = Some(href.into());
        self
    }

    /// Pointer, Enter, Space, and the native accessible action share this callback.
    pub fn on_click(
        mut self,
        handler: impl Fn(&Activation, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_click = Some(Box::new(handler));
        self
    }

    fn appearance(&self) -> (ButtonColor, ButtonVariant) {
        let (color, variant) = self.button_type.appearance();
        let variant = self.variant.unwrap_or(variant);
        let color = self.color.unwrap_or_else(|| {
            if self.danger {
                ButtonColor::Danger
            } else if self.variant == Some(ButtonVariant::Solid)
                && self.button_type == ButtonType::Default
            {
                ButtonColor::Primary
            } else {
                color
            }
        });
        (color, variant)
    }
}

/// Styled refinements apply to the native control, including its parent layout.
/// Focus, semantics and activation retain their stable identity across refinements.
impl Styled for Button {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

#[derive(Default)]
struct VisualTransition {
    from: Option<ButtonStateStyle>,
    target: Option<ButtonStateStyle>,
    progress: Option<ScalarTransition>,
}

impl VisualTransition {
    fn sample(&self, now: Duration) -> ButtonStateStyle {
        let target = self.target.expect("initialized visual state");
        let progress = self
            .progress
            .map_or(1., |progress| progress.sample(now).value);
        interpolate_style(self.from.unwrap_or(target), target, progress as f32)
    }

    fn resolve(
        &mut self,
        target: ButtonStateStyle,
        now: Duration,
        duration: Duration,
        curve: CubicBezier,
        motion: MotionPolicy,
    ) -> (ButtonStateStyle, bool) {
        if self.target != Some(target) {
            self.from = Some(if self.target.is_some() {
                self.sample(now)
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
        let finished = progress.sample(now).finished;
        (self.sample(now), !finished)
    }
}

struct ButtonState {
    scope: ConfigScope,
    focus: FocusHandle,
    guard: ActivationGuard,
    disabled: Option<bool>,
    loading_request: (bool, Duration),
    loading: bool,
    hovered: bool,
    pointer_armed: bool,
    pointer_in_window: bool,
    keyboard_pressed: bool,
    visual: VisualTransition,
    loader: Option<ScalarTransition>,
    wave: WaveState,
    clock: Box<dyn Fn(&App) -> Duration>,
    frame_pending: bool,
    dashed_border: DashedBorderCache,
    _config: Subscription,
    _blur: Subscription,
    delay_task: Option<Task<()>>,
}

impl ButtonState {
    fn new(
        scope: ConfigScope,
        focus: Option<FocusHandle>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let focus = focus.unwrap_or_else(|| focus_handle(cx));
        let guard = ActivationGuard::default();
        let subscription = Self::observe_scope(&scope, window, cx);
        let blur = Self::observe_blur(&focus, &guard, window, cx);
        let epoch = cx.background_executor().now();
        Self {
            scope,
            focus,
            guard,
            disabled: None,
            loading_request: (false, Duration::ZERO),
            loading: false,
            hovered: false,
            pointer_armed: false,
            pointer_in_window: true,
            keyboard_pressed: false,
            visual: VisualTransition::default(),
            loader: None,
            wave: WaveState::default(),
            // Use the executor's clock for deterministic transitions without a
            // dependency on the scheduler's otherwise opaque Instant type.
            clock: Box::new(move |cx| cx.background_executor().now().duration_since(epoch)),
            frame_pending: false,
            dashed_border: DashedBorderCache::default(),
            _config: subscription,
            _blur: blur,
            delay_task: None,
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
        let state = cx.weak_entity();
        window.on_focus_out(focus, cx, move |_, _, cx| {
            guard.cancel();
            let _ = state.update(cx, |state, cx| {
                if state.pointer_armed || state.keyboard_pressed {
                    state.pointer_armed = false;
                    state.keyboard_pressed = false;
                    cx.notify();
                }
            });
        })
    }

    fn sync_availability(&mut self, window: &mut Window, cx: &mut App) {
        let availability = self
            .scope
            .snapshot(cx)
            .availability(self.disabled, self.loading);
        self.guard.update(availability, &self.focus, window, cx);
        if !availability.can_activate() {
            self.pointer_armed = false;
            self.keyboard_pressed = false;
            self.wave.clear();
        }
    }

    fn update(
        &mut self,
        button: &Button,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope != scope {
            self.guard.cancel();
            self._config = Self::observe_scope(&scope, window, cx);
            self.scope = scope;
        }
        if let Some(focus) = &button.focus
            && focus != &self.focus
        {
            self.guard.cancel();
            self._blur = Self::observe_blur(focus, &self.guard, window, cx);
            self.focus = focus.clone();
        }
        self.disabled = button.disabled;
        let request = (button.loading, button.loading_delay);
        if self.loading_request != request {
            self.delay_task = None;
            self.loading_request = request;
            self.loading = button.loading && (self.loading || button.loading_delay.is_zero());
            if button.loading && !self.loading && !button.loading_delay.is_zero() {
                let timer = cx.background_executor().timer(button.loading_delay);
                self.delay_task = Some(cx.spawn_in(window, async move |state, cx| {
                    timer.await;
                    let _ = state.update_in(cx, |state, window, cx| {
                        state.loading = true;
                        state.sync_availability(window, cx);
                        cx.notify();
                    });
                }));
            }
        }
        self.sync_availability(window, cx);
    }

    fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if !self.frame_pending {
            self.frame_pending = true;
            let state = cx.weak_entity();
            window.on_next_frame(move |_, cx| {
                let _ = state.update(cx, |state, cx| {
                    state.frame_pending = false;
                    cx.notify();
                });
            });
        }
    }
}

impl RenderOnce for Button {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let popup_trigger = crate::popup::PopupTrigger::current(cx);
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            ButtonState::new(scope.clone(), self.focus.clone(), window, cx)
        });
        state.update(cx, |state, cx| {
            state.update(&self, scope.clone(), window, cx)
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<ButtonTheme>();
        let global = tokens.globals();
        let motion = MotionPolicy::new(config.motion && global.motion, cx.reduce_motion());
        let (color, variant) = self.appearance();
        let retained = state.read(cx);
        let loading = retained.loading;
        let availability = retained.guard.availability();
        let focus = retained.focus.clone();
        let guard = retained.guard.clone();
        let tracking_pointer = retained.pointer_armed;
        if let Some(group) = &self.group {
            group.set_interaction(
                availability.disabled,
                retained.hovered,
                focus.contains_focused(window, cx),
            );
        }
        let has_explicit_icon = self.icon.is_some();
        let icon_only = self.label.is_empty() && (has_explicit_icon || loading);
        let mut style = ButtonStyle::from_tokens(
            &tokens,
            ButtonStyleOptions {
                size: self.size.unwrap_or(config.size),
                color,
                variant,
                shape: self.shape,
                ghost: self.ghost,
                disabled: availability.disabled,
                icon_only,
            },
        );
        self.visual_override.apply(&mut style, icon_only);
        let target = if availability.disabled {
            style.rest
        } else if retained.keyboard_pressed || (retained.pointer_armed && retained.hovered) {
            style.active
        } else if retained.hovered {
            style.hover
        } else {
            style.rest
        };
        let spin_loader = self.loading_icon_spin.unwrap_or_else(|| {
            self.loading_icon.as_ref().is_none_or(|reference| {
                reference.key() == IconName::LoadingOutlined.descriptor().key
            })
        });
        let inserted_loader = !has_explicit_icon && !self.label.is_empty();
        let custom_loader = self.loading_icon.is_some();
        let (current, loader_progress, has_active_wave, spin_phase) =
            state.update(cx, |state, cx| {
                let now = (state.clock)(cx);
                let (current, color_animating) = state.visual.resolve(
                    target,
                    now,
                    global.motion_duration_mid,
                    global.motion_ease_in_out,
                    motion,
                );
                let loader_target = if loading { 1. } else { 0. };
                let loader = state.loader.get_or_insert_with(|| {
                    ScalarTransition::new(loader_target, global.motion_ease_in_out)
                });
                loader.retarget(loader_target, now, global.motion_duration_slow, motion);
                let loader = loader.sample(now);
                let show_loader =
                    loading || (inserted_loader && !custom_loader && loader.value > 0.);
                let wave = state.wave.sample(
                    now,
                    WaveMotion::Regular,
                    global,
                    motion,
                    self.wave && self.href.is_none(),
                );
                let loader_animating = !loader.finished && inserted_loader && !custom_loader;
                if color_animating
                    || loader_animating
                    || wave
                    || (show_loader && spin_loader && motion.allows_motion())
                {
                    state.request_frame(window, cx);
                }
                (
                    current,
                    loader.value as f32,
                    wave,
                    now.as_secs_f64().fract() as f32,
                )
            });
        let metrics = style.metrics;
        let mut content = div()
            .relative()
            .flex()
            .items_center()
            .justify_center()
            .flex_shrink_0()
            .h(px(metrics.height as f32))
            .min_w(px(metrics.min_width as f32))
            .px(px(metrics.padding_inline as f32))
            .border(px(style.line_width as f32))
            .rounded(px(metrics.radius as f32))
            .text_size(px(metrics.font_size as f32))
            .line_height(gpui::relative(
                (metrics.line_height / metrics.font_size) as f32,
            ))
            .font(tokens.font().clone())
            .font_weight(FontWeight(metrics.font_weight as f32))
            .when(self.block, |content| content.w_full())
            .when(icon_only, |content| content.w(px(metrics.height as f32)))
            .when(style.line_type == LineType::Dashed, |content| {
                content.border_dashed()
            })
            .when(loading, |content| {
                content.opacity(style.loading_opacity as f32)
            });
        if !style.shadow.is_empty() {
            content = content.shadow(
                style
                    .shadow
                    .iter()
                    .map(|shadow| gpui::BoxShadow {
                        inset: shadow.inset,
                        color: shadow.color.to_hsla(),
                        offset: point(px(shadow.x as f32), px(shadow.y as f32)),
                        blur_radius: px(shadow.blur as f32),
                        spread_radius: px(shadow.spread as f32),
                    })
                    .collect::<Vec<_>>(),
            );
        }
        let has_label = !self.label.is_empty();
        // A departing inserted loader remains decorative while the control is ready.
        let show_loader = loading || (inserted_loader && !custom_loader && loader_progress > 0.);
        let reference = if show_loader {
            Some(
                self.loading_icon
                    .take()
                    .unwrap_or_else(|| IconName::LoadingOutlined.into()),
            )
        } else {
            self.icon.take()
        };
        let reverse =
            (config.direction == Direction::Rtl) ^ (self.icon_placement == IconPlacement::End);
        content = content.when(reverse, |content| content.flex_row_reverse());
        let font_size = self
            .style
            .text
            .font_size
            .map_or(px(metrics.font_size as f32), |size| {
                size.to_pixels(window.rem_size())
            });
        if let Some(reference) = reference {
            let fixed_icon_size = icon_only
                .then(|| match self.size.unwrap_or(config.size) {
                    Size::Small => tokens.only_icon_size_sm,
                    Size::Medium => tokens.only_icon_size,
                    Size::Large => tokens.only_icon_size_lg,
                })
                .flatten();
            let icon_size = self
                .icon_style
                .as_ref()
                .and_then(|style| style.text.font_size)
                .map_or_else(
                    || {
                        fixed_icon_size.map_or_else(
                            || {
                                self.style
                                    .text
                                    .font_size
                                    .map_or(px(metrics.icon_size as f32), |_| font_size)
                            },
                            |size| px(size as f32),
                        )
                    },
                    |size| size.to_pixels(window.rem_size()),
                );
            let expanding = show_loader && inserted_loader && !custom_loader;
            let options = IconRenderOptions {
                size: size(icon_size, icon_size),
                colors: config.icon_palette,
                rotation: radians(if show_loader && spin_loader && motion.allows_motion() {
                    spin_phase * std::f32::consts::TAU
                } else {
                    0.
                }),
                ..Default::default()
            };
            let icon = foreground_icon(
                reference,
                options,
                if expanding { loader_progress } else { 1. },
            );
            let mut slot = div()
                .flex()
                .items_center()
                .justify_center()
                .flex_shrink_0()
                .child(icon);
            if let Some(style) = &self.icon_style {
                slot.style().refine(style);
            }
            if expanding {
                let gap = metrics.icon_gap as f32 * loader_progress;
                let (left, right) = config.direction.horizontal(
                    if self.icon_placement == IconPlacement::End {
                        gap
                    } else {
                        0.
                    },
                    if self.icon_placement == IconPlacement::Start {
                        gap
                    } else {
                        0.
                    },
                );
                slot = slot
                    .w(icon_size * loader_progress)
                    .opacity(loader_progress)
                    .overflow_hidden()
                    .ml(px(left))
                    .mr(px(right));
            } else if has_label {
                content = content.gap(px(metrics.icon_gap as f32));
            }
            content = content.child(slot);
        }
        if has_label {
            let label_font_size = self
                .content_style
                .as_ref()
                .and_then(|style| style.text.font_size)
                .map_or(font_size, |size| size.to_pixels(window.rem_size()));
            let mut label = label::button_label(
                self.label.clone(),
                self.auto_insert_space && !has_explicit_icon && !variant.is_unbordered(),
                loading,
                f32::from(label_font_size),
            );
            if let Some(style) = &self.content_style {
                label.style().refine(style);
            }
            content = content.child(label);
        }
        let mut surface = apply_colors(content, current);
        surface.style().refine(&self.style);
        if let Some(group) = &self.group {
            group.apply_style(surface.style());
        }
        let effective = surface.style();
        let radii = gpui::Corners {
            top_left: effective.corner_radii.top_left.unwrap_or_default(),
            top_right: effective.corner_radii.top_right.unwrap_or_default(),
            bottom_left: effective.corner_radii.bottom_left.unwrap_or_default(),
            bottom_right: effective.corner_radii.bottom_right.unwrap_or_default(),
        }
        .to_pixels(window.rem_size());
        let widths = gpui::Edges {
            top: effective.border_widths.top.unwrap_or_default(),
            bottom: effective.border_widths.bottom.unwrap_or_default(),
            left: effective.border_widths.left.unwrap_or_default(),
            right: effective.border_widths.right.unwrap_or_default(),
        }
        .to_pixels(window.rem_size());
        let opacity = effective.opacity.unwrap_or(1.);
        let wave_color = wave_color(effective, global.color_primary.to_hsla());
        // The fitted path covers uniform borders. GPUI handles deliberately
        // asymmetric refinements using the same geometry as its own surface.
        let dashed = effective.border_style == Some(gpui::BorderStyle::Dashed)
            && radii.top_left == radii.top_right
            && radii.top_left == radii.bottom_left
            && radii.top_left == radii.bottom_right
            && widths.top == widths.bottom
            && widths.top == widths.left
            && widths.top == widths.right;
        if dashed {
            // Keep the border's layout inset; paint the fitted dash geometry
            // over its transparent stroke without changing the control's bounds.
            surface.style().border_style = Some(gpui::BorderStyle::Solid);
            surface = surface.border_color(Color::TRANSPARENT);
        }
        let mut element = surface
            .id(self.id)
            .when(availability.can_focus(), |element| {
                element.track_focus(&focus)
            })
            .when(
                availability.disabled && self.style.mouse_cursor.is_none(),
                |element| element.cursor_not_allowed(),
            )
            .when(
                !availability.disabled && loading && self.style.mouse_cursor.is_none(),
                |element| element.cursor_default(),
            )
            .when(
                availability.can_activate() && self.style.mouse_cursor.is_none(),
                |element| element.cursor_pointer(),
            );
        // GPUI resolves absolute children against the padding box. These paint
        // and pointer overlays cover the same outer border box as the control.
        let overlay = || {
            div()
                .absolute()
                .top(-widths.top)
                .bottom(-widths.bottom)
                .left(-widths.left)
                .right(-widths.right)
        };
        let mut joined_border: Option<crate::input::group::BorderPainter> = None;
        if dashed {
            let color = self.style.border_color.unwrap_or(current.border.to_hsla());
            let radius = radii.top_left;
            let width = widths.top;
            let weak = state.downgrade();
            if self.group.is_some() {
                joined_border = Some(Box::new(move |bounds, opacity, window, cx| {
                    let _ = weak.update(cx, |state, _| {
                        state.dashed_border.paint(
                            bounds,
                            radius,
                            width,
                            color.opacity(opacity),
                            window,
                        );
                    });
                }));
            } else {
                element = element.child(
                    overlay().child(
                        canvas(
                            |_, _, _| (),
                            move |bounds, (), window, cx| {
                                let _ = weak.update(cx, |state, _| {
                                    state
                                        .dashed_border
                                        .paint(bounds, radius, width, color, window);
                                });
                            },
                        )
                        .size_full(),
                    ),
                );
            }
        }
        let weak = state.downgrade();
        element = element.on_hover(move |hovered, _, cx| {
            let _ = weak.update(cx, |state, cx| {
                if state.hovered != *hovered {
                    // GPUI suppresses hover during a held mouse gesture. The
                    // temporary physical tracker below owns that phase instead.
                    if state.pointer_armed {
                        return;
                    }
                    state.hovered = *hovered;
                    if *hovered {
                        state.pointer_in_window = true;
                    }
                    cx.notify();
                }
            });
        });
        let weak = state.downgrade();
        element = element.on_mouse_down(MouseButton::Left, move |_, _, cx| {
            set_pressed(&weak, true, false, cx)
        });
        let weak = state.downgrade();
        element = element.on_mouse_up(MouseButton::Left, move |_, _, cx| {
            set_pressed(&weak, false, false, cx)
        });
        let weak = state.downgrade();
        element = element.on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
            set_pressed(&weak, false, false, cx)
        });
        let weak = state.downgrade();
        let is_link = self.href.is_some();
        element = element.capture_key_down(move |event, _, cx| {
            if !event.is_held
                && !event.keystroke.modifiers.modified()
                && matches!(event.keystroke.key.as_str(), "enter" | "space")
                && !(is_link && event.keystroke.key == "space")
            {
                set_pressed(&weak, true, true, cx);
            }
        });
        let weak = state.downgrade();
        element = element.on_key_up(move |_, _, cx| set_pressed(&weak, false, true, cx));
        let weak = state.downgrade();
        element = element.on_mouse_exit(move |_, _, cx| {
            let _ = weak.update(cx, |state, cx| {
                state.pointer_in_window = false;
                if state.hovered {
                    state.hovered = false;
                    cx.notify();
                }
            });
        });
        if tracking_pointer {
            let weak = state.downgrade();
            element = element.child(
                overlay().child(
                    canvas(
                        |_, _, _| (),
                        move |bounds, (), window, cx| {
                            let visible = window.content_mask().bounds.intersect(&bounds);
                            let _ = weak.update(cx, |state, cx| {
                                let hovered = state.pointer_in_window
                                    && visible.contains(&window.mouse_position());
                                if state.pointer_armed && state.hovered != hovered {
                                    state.hovered = hovered;
                                    state.request_frame(window, cx);
                                }
                            });
                            let weak = weak.clone();
                            window.on_mouse_event(
                                move |event: &gpui::MouseMoveEvent, phase, _, cx| {
                                    if phase == gpui::DispatchPhase::Capture {
                                        let _ = weak.update(cx, |state, cx| {
                                            let hovered = visible.contains(&event.position);
                                            state.pointer_in_window = true;
                                            if state.pointer_armed && state.hovered != hovered {
                                                state.hovered = hovered;
                                                cx.notify();
                                            }
                                        });
                                    }
                                },
                            );
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
                element = element.child(
                    overlay().child(
                        canvas(
                            |_, _, _| (),
                            move |bounds, (), window, _| {
                                if color.a > 0.001 && spread > 0. {
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
                                                .map(|radius| *radius + px(spread)),
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
        let callback = self.on_click;
        let href = self.href;
        let role = if href.is_some() {
            Role::Link
        } else {
            Role::Button
        };
        let weak = state.downgrade();
        let element = guard.install(element, move |activation, window, cx| {
            if href.is_some() && activation == Activation::Keyboard(gpui::KeyboardButton::Space) {
                return;
            }
            if self.wave && href.is_none() && !variant.is_unbordered() && motion.allows_motion() {
                let _ = weak.update(cx, |state, cx| {
                    state.wave.start((state.clock)(cx));
                    cx.notify();
                });
            }
            if let Some(callback) = &callback {
                callback(&activation, window, cx);
            }
            if let Some(href) = &href {
                cx.open_url(href);
            }
        });
        self.accessibility.role = Some(role);
        let mut element = popup_trigger.install(element, &guard, &focus);
        self.accessibility.disabled = availability.disabled;
        self.accessibility.busy = loading;
        if self.accessibility.label.is_none() && !self.label.is_empty() {
            self.accessibility.label = Some(self.label);
        }
        if let Some(group) = &self.group {
            let ring = (focus.is_focused(window) && window.last_input_was_keyboard()).then(|| {
                FocusRing::new(style.focus_color.to_hsla(), px(style.focus_width as f32))
                    .with_offset(px(style.focus_offset as f32))
            });
            element = group.decorate_with_border(element, ring, window, joined_border);
        }
        popup_trigger.apply(
            self.accessibility.apply(
                FocusRing::new(
                    style
                        .focus_color
                        .with_alpha(style.focus_color.a * f64::from(opacity))
                        .to_hsla(),
                    px(if self.group.is_some() {
                        0.
                    } else {
                        style.focus_width as f32
                    }),
                )
                .with_offset(px(style.focus_offset as f32))
                .radius(radii.top_left)
                .apply(element, &focus)
                .corners(radii),
            ),
        )
    }
}

fn set_pressed(state: &gpui::WeakEntity<ButtonState>, pressed: bool, keyboard: bool, cx: &mut App) {
    let _ = state.update(cx, |state, cx| {
        let pressed = pressed && state.guard.availability().can_activate();
        if pressed && !keyboard {
            state.hovered = true;
            state.pointer_in_window = true;
        }
        let current = if keyboard {
            &mut state.keyboard_pressed
        } else {
            &mut state.pointer_armed
        };
        if *current != pressed {
            *current = pressed;
            cx.notify();
        }
    });
}

fn apply_colors<E: Styled>(element: E, state: ButtonStateStyle) -> E {
    element
        .bg(state.background)
        .border_color(state.border)
        .text_color(state.text)
}

fn interpolate_style(
    from: ButtonStateStyle,
    to: ButtonStateStyle,
    progress: f32,
) -> ButtonStateStyle {
    fn color(from: Color, to: Color, progress: f64) -> Color {
        let progress = progress.clamp(0., 1.);
        if progress == 0. || from == to {
            return from;
        }
        if progress == 1. {
            return to;
        }
        // CSS transitions use premultiplied sRGB. Palette mixing would darken
        // a colored surface as it fades to transparent black.
        let alpha = from.a + (to.a - from.a) * progress;
        let channel = |a: u8, b: u8| {
            if alpha > f64::EPSILON {
                ((f64::from(a) * from.a * (1. - progress) + f64::from(b) * to.a * progress) / alpha)
                    .round() as u8
            } else {
                0
            }
        };
        Color {
            r: channel(from.r, to.r),
            g: channel(from.g, to.g),
            b: channel(from.b, to.b),
            a: alpha,
        }
    }
    let progress = f64::from(progress);
    ButtonStateStyle {
        background: color(from.background, to.background, progress),
        border: color(from.border, to.border, progress),
        text: color(from.text, to.text, progress),
    }
}

fn foreground_icon(reference: IconRef, options: IconRenderOptions, scale: f32) -> gpui::Canvas<()> {
    canvas(
        |_, _, _| (),
        move |bounds, (), window, cx| {
            if cx.try_global::<IconRenderer>().is_none() {
                cx.set_global(IconRenderer::default());
            }
            let mut options = options;
            options.colors = IconPalette {
                text: window.text_style().color.into(),
                ..options.colors
            };
            cx.update_global::<IconRenderer, _>(|renderer, cx| {
                let _ = renderer.paint_with_scale(bounds, &reference, &options, scale, window, cx);
            });
        },
    )
    .w(options.size.width)
    .h(options.size.height)
    .flex_shrink_0()
}

#[cfg(test)]
mod tests;
