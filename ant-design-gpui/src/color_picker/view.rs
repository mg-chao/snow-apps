// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    ColorPickerChange, ColorPickerFormat, ColorPickerMode, ColorPickerState,
    ColorPickerStyleContext, ColorPickerStyles, ColorPickerTheme, ColorPickerTokens,
    ColorPickerValue, paint,
    state::{ChangeHandler, ClearHandler, Control, FormatHandler, OpenHandler, Surface},
};
use crate::{
    ConfigScope, Direction, Input, InputNumber, Select, Size, ValidationStatus, Variant,
    behavior::{Accessibility, ActivationGuard, Availability, FocusRing},
    icons::{IconName, icon},
    popup::{Popup, PopupArrow, PopupPlacement, PopupTrigger, PopupTriggers},
    segmented::{Segmented, SegmentedOption},
};
use gpui::{
    AnyElement, App, ElementId, Entity, IntoElement, MouseButton, MouseMoveEvent, MouseUpEvent,
    Refineable, RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, WeakEntity,
    Window, accesskit, canvas, div, point, prelude::*, px, relative,
};
use std::{rc::Rc, sync::Arc};

type TextRender = Rc<dyn Fn(&ColorPickerValue, &mut Window, &mut App) -> AnyElement>;
type PanelRender = Rc<dyn Fn(&ColorPickerPanelParts, &mut Window, &mut App) -> AnyElement>;
type LabelRender = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
type ActionHandler = Rc<dyn Fn(&mut Window, &mut App)>;
type StyleRender = Rc<dyn Fn(&ColorPickerStyleContext, &mut Window, &mut App) -> ColorPickerStyles>;

#[derive(Clone)]
pub struct ColorPickerPreset {
    pub key: SharedString,
    pub label: SharedString,
    pub colors: Arc<[ColorPickerValue]>,
    pub default_open: bool,
    label_render: Option<LabelRender>,
}
impl ColorPickerPreset {
    pub fn new(
        key: impl Into<SharedString>,
        label: impl Into<SharedString>,
        colors: impl IntoIterator<Item = ColorPickerValue>,
    ) -> Self {
        Self::try_new(key, label, colors).expect("valid ColorPicker preset colors")
    }
    pub fn try_new(
        key: impl Into<SharedString>,
        label: impl Into<SharedString>,
        colors: impl IntoIterator<Item = ColorPickerValue>,
    ) -> Result<Self, super::ColorPickerError> {
        Ok(Self {
            key: key.into(),
            label: label.into(),
            colors: colors
                .into_iter()
                .map(|value| value.canonicalized())
                .collect::<Result<_, _>>()?,
            default_open: true,
            label_render: None,
        })
    }
    fn canonicalized(&self) -> Result<Self, super::ColorPickerError> {
        let mut changed = None;
        for (index, value) in self.colors.iter().enumerate() {
            let canonical = value.canonicalized()?;
            if canonical != *value {
                changed.get_or_insert_with(|| self.colors.to_vec())[index] = canonical;
            }
        }
        let mut preset = self.clone();
        if let Some(colors) = changed {
            preset.colors = colors.into();
        }
        Ok(preset)
    }
    pub fn default_open(mut self, open: bool) -> Self {
        self.default_open = open;
        self
    }
    pub fn label_content<E: IntoElement>(
        mut self,
        render: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.label_render = Some(Rc::new(move |w, c| render(w, c).into_any_element()));
        self
    }
}

#[derive(IntoElement)]
pub struct ColorPicker {
    id: ElementId,
    state: Entity<ColorPickerState>,
    scope: Option<ConfigScope>,
    value: Option<ColorPickerValue>,
    default_value: Option<ColorPickerValue>,
    format: Option<ColorPickerFormat>,
    default_format: ColorPickerFormat,
    open: Option<bool>,
    default_open: bool,
    disabled: Option<bool>,
    disabled_alpha: bool,
    disabled_format: bool,
    modes: Vec<ColorPickerMode>,
    allow_clear: bool,
    show_text: bool,
    size: Option<Size>,
    status: ValidationStatus,
    trigger: PopupTriggers,
    placement: PopupPlacement,
    arrow: Option<PopupArrow>,
    auto_adjust: bool,
    destroy: bool,
    child: Option<AnyElement>,
    text_render: Option<TextRender>,
    panel_render: Option<PanelRender>,
    presets: Option<Arc<[ColorPickerPreset]>>,
    styles: ColorPickerStyles,
    styles_fn: Option<StyleRender>,
    on_change: Option<ChangeHandler>,
    on_complete: Option<ChangeHandler>,
    on_format: Option<FormatHandler>,
    on_open: Option<OpenHandler>,
    on_clear: Option<ClearHandler>,
    label: Option<SharedString>,
    automation_id: Option<SharedString>,
}
impl ColorPicker {
    pub fn new(id: impl Into<ElementId>, state: &Entity<ColorPickerState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            value: None,
            default_value: None,
            format: None,
            default_format: ColorPickerFormat::Hex,
            open: None,
            default_open: false,
            disabled: None,
            disabled_alpha: false,
            disabled_format: false,
            modes: vec![ColorPickerMode::Single],
            allow_clear: false,
            show_text: false,
            size: None,
            status: ValidationStatus::None,
            trigger: PopupTriggers::CLICK,
            placement: PopupPlacement::BottomLeft,
            arrow: None,
            auto_adjust: true,
            destroy: false,
            child: None,
            text_render: None,
            panel_render: None,
            presets: None,
            styles: ColorPickerStyles::default(),
            styles_fn: None,
            on_change: None,
            on_complete: None,
            on_format: None,
            on_open: None,
            on_clear: None,
            label: None,
            automation_id: None,
        }
    }
    pub fn scope(mut self, v: ConfigScope) -> Self {
        self.scope = Some(v);
        self
    }
    pub fn value(mut self, v: impl Into<ColorPickerValue>) -> Self {
        self.value = Some(v.into());
        self
    }
    pub fn default_value(mut self, v: impl Into<ColorPickerValue>) -> Self {
        self.default_value = Some(v.into());
        self
    }
    pub fn format(mut self, v: ColorPickerFormat) -> Self {
        self.format = Some(v);
        self
    }
    pub fn default_format(mut self, v: ColorPickerFormat) -> Self {
        self.default_format = v;
        self
    }
    pub fn open(mut self, v: bool) -> Self {
        self.open = Some(v);
        self
    }
    pub fn default_open(mut self, v: bool) -> Self {
        self.default_open = v;
        self
    }
    pub fn disabled(mut self, v: bool) -> Self {
        self.disabled = Some(v);
        self
    }
    pub fn disabled_alpha(mut self, v: bool) -> Self {
        self.disabled_alpha = v;
        self
    }
    pub fn disabled_format(mut self, v: bool) -> Self {
        self.disabled_format = v;
        self
    }
    pub fn modes(mut self, v: impl IntoIterator<Item = ColorPickerMode>) -> Self {
        self.modes = v.into_iter().collect();
        self
    }
    pub fn mode(self, v: ColorPickerMode) -> Self {
        self.modes([v])
    }
    pub fn allow_clear(mut self, v: bool) -> Self {
        self.allow_clear = v;
        self
    }
    pub fn show_text(mut self, v: bool) -> Self {
        self.show_text = v;
        self
    }
    pub fn size(mut self, v: Size) -> Self {
        self.size = Some(v);
        self
    }
    pub fn status(mut self, v: ValidationStatus) -> Self {
        self.status = v;
        self
    }
    pub fn trigger(mut self, v: PopupTriggers) -> Self {
        self.trigger = v;
        self
    }
    pub fn placement(mut self, v: PopupPlacement) -> Self {
        self.placement = v;
        self
    }
    pub fn arrow(mut self, v: impl Into<PopupArrow>) -> Self {
        self.arrow = Some(v.into());
        self
    }
    pub fn auto_adjust_overflow(mut self, v: bool) -> Self {
        self.auto_adjust = v;
        self
    }
    pub fn destroy_on_hidden(mut self, v: bool) -> Self {
        self.destroy = v;
        self
    }
    pub fn child(mut self, v: impl IntoElement) -> Self {
        self.child = Some(v.into_any_element());
        self
    }
    pub fn presets(mut self, v: impl IntoIterator<Item = ColorPickerPreset>) -> Self {
        self.presets = Some(v.into_iter().collect());
        self
    }
    pub fn styles(mut self, v: ColorPickerStyles) -> Self {
        self.styles = v;
        self
    }
    pub fn styles_fn(
        mut self,
        v: impl Fn(&ColorPickerStyleContext, &mut Window, &mut App) -> ColorPickerStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(v));
        self
    }
    pub fn text_render<E: IntoElement>(
        mut self,
        f: impl Fn(&ColorPickerValue, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.text_render = Some(Rc::new(move |v, w, c| f(v, w, c).into_any_element()));
        self
    }
    pub fn panel_render<E: IntoElement>(
        mut self,
        f: impl Fn(&ColorPickerPanelParts, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.panel_render = Some(Rc::new(move |v, w, c| f(v, w, c).into_any_element()));
        self
    }
    pub fn on_change(
        mut self,
        f: impl Fn(&ColorPickerChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Rc::new(f));
        self
    }
    pub fn on_change_complete(
        mut self,
        f: impl Fn(&ColorPickerChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_complete = Some(Rc::new(f));
        self
    }
    pub fn on_format_change(
        mut self,
        f: impl Fn(ColorPickerFormat, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_format = Some(Rc::new(f));
        self
    }
    pub fn on_open_change(mut self, f: impl Fn(bool, &mut Window, &mut App) + 'static) -> Self {
        self.on_open = Some(Rc::new(f));
        self
    }
    pub fn on_clear(mut self, f: impl Fn(&mut Window, &mut App) + 'static) -> Self {
        self.on_clear = Some(Rc::new(f));
        self
    }
    pub fn accessibility_label(mut self, v: impl Into<SharedString>) -> Self {
        self.label = Some(v.into());
        self
    }
    pub fn automation_id(mut self, v: impl Into<SharedString>) -> Self {
        self.automation_id = Some(v.into());
        self
    }

    fn configure(&mut self, window: &mut Window, cx: &mut App) -> ColorPickerPanelParts {
        if let Some(value) = &self.value {
            self.value = Some(
                value
                    .canonicalized()
                    .expect("valid ColorPicker controlled value"),
            );
        }
        if let Some(value) = &self.default_value {
            self.default_value = Some(
                value
                    .canonicalized()
                    .expect("valid ColorPicker default value"),
            );
        }
        if let Some(presets) = &mut self.presets {
            let mut changed = None;
            for (index, preset) in presets.iter().enumerate() {
                let canonical = preset
                    .canonicalized()
                    .expect("valid ColorPicker preset colors");
                if !Arc::ptr_eq(&canonical.colors, &preset.colors) {
                    changed.get_or_insert_with(|| presets.to_vec())[index] = canonical;
                }
            }
            if let Some(canonical) = changed {
                *presets = canonical.into();
            }
        }
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let state = self.state.clone();
        let observed = state.clone();
        window.use_keyed_state(self.id.clone(), cx, move |window, cx| {
            Mount::new(&observed, window, cx)
        });
        state.update(cx, |s, cx| {
            s.configure_scope(scope.clone(), window, cx);
            s.on_change = self.on_change.clone();
            s.on_complete = self.on_complete.clone();
            s.on_format = self.on_format.clone();
            s.on_open = self.on_open.clone();
            s.on_clear = self.on_clear.clone();
            s.controlled = self.value.is_some();
            s.format_controlled = self.format.is_some();
            s.open_controlled = self.open.is_some();
            let mut sync = s.restore_completion_proposal(self.value.as_ref());
            let disabled = self.disabled.unwrap_or(config.disabled);
            if disabled && !s.disabled {
                s.cancel(window, cx);
            }
            if self.disabled_alpha
                && !s.disabled_alpha
                && s.drag
                    .as_ref()
                    .is_some_and(|drag| drag.surface == Surface::Alpha)
            {
                s.cancel(window, cx);
            }
            s.disabled = disabled;
            s.disabled_alpha = self.disabled_alpha;
            s.disabled_format = self.disabled_format;
            if let Some(value) = &self.value {
                value
                    .validate()
                    .expect("valid ColorPicker controlled value");
                if (s.drag.is_some() || s.key_active)
                    && value != &s.accepted
                    && value != s.model.value()
                {
                    s.cancel(window, cx);
                    s.model.set_value(value.clone());
                    sync = true;
                }
                s.accepted = value.clone();
                if s.drag.is_none() && !s.key_active && s.model.value() != value {
                    s.model.set_value(value.clone());
                    sync = true;
                }
            } else if !s.initialized
                && let Some(value) = &self.default_value
            {
                value.validate().expect("valid ColorPicker default value");
                s.accepted = value.clone();
                s.model.set_value(value.clone());
                sync = true;
            }
            s.initialized = true;
            let format = self.format.unwrap_or(if s.format_initialized {
                s.format
            } else {
                self.default_format
            });
            if format != s.format {
                s.format = format;
                sync = true;
            }
            s.format_initialized = true;
            let was_open = s.is_open();
            if let Some(open) = self.open {
                s.open = open;
            } else if !s.open_initialized {
                s.open = self.default_open;
            }
            s.open_initialized = true;
            if was_open && !s.is_open() {
                s.cancel(window, cx);
                s.clear_panel_bounds();
            }
            s.modes = [ColorPickerMode::Single, ColorPickerMode::Gradient]
                .into_iter()
                .filter(|mode| self.modes.contains(mode))
                .collect();
            if s.modes.is_empty() {
                s.modes.push(ColorPickerMode::Single);
            }
            if !s.modes.contains(&s.model.mode()) {
                s.cancel(window, cx);
                s.model.prepare_mode(s.modes[0]);
                sync = true;
            } else {
                s.model.prepare_mode(s.model.mode());
            }
            let groups: Vec<_> = self.presets.as_ref().map_or_else(Vec::new, |presets| {
                presets
                    .iter()
                    .map(|group| (group.key.clone(), group.colors.len()))
                    .collect()
            });
            s.retain_controls(&groups, window, cx);
            if sync {
                s.sync_editors(None, window, cx);
            }
            if s.disabled {
                s.open = false;
                s.clear_panel_bounds();
            }
        });
        let context = {
            let s = state.read(cx);
            ColorPickerStyleContext {
                value: s.accepted.clone(),
                format: s.format,
                mode: s.model.mode(),
                size: self.size.unwrap_or(config.size),
                disabled: s.disabled,
                open: s.is_open(),
                dragging: s.drag.is_some(),
                hovered: s.hovered,
                focused: s.trigger_focus.is_focused(window),
            }
        };
        let mut styles = config.color_picker.styles.clone();
        if let Some(resolve) = &config.color_picker.styles_fn {
            styles.refine(&resolve.resolve(&context, window, cx));
        }
        styles.refine(&self.styles);
        if let Some(resolve) = &self.styles_fn {
            styles.refine(&resolve(&context, window, cx));
        }
        ColorPickerPanelParts {
            state,
            scope,
            tokens: config.theme.component::<ColorPickerTheme>(),
            styles,
            allow_clear: self.allow_clear,
            presets: self.presets.clone(),
        }
    }
}
impl ColorPicker {
    fn trigger_width_hint(
        &self,
        parts: &ColorPickerPanelParts,
        metrics: &super::ColorPickerTriggerMetrics,
        value: &ColorPickerValue,
        format: ColorPickerFormat,
        config: &crate::ResolvedConfig,
        window: &Window,
    ) -> (Option<gpui::Pixels>, gpui::Pixels) {
        let root = &parts.styles.root;
        if self.child.is_some()
            || root.size.width.is_some()
            || root.max_size.width.is_some()
            || root.min_size.width.is_some()
            || root.padding != Default::default()
            || root.border_widths != Default::default()
            || root.margin != Default::default()
            || root.gap != Default::default()
            || root.flex_direction.is_some()
            || root.flex_wrap.is_some()
            || root.display.is_some()
            || parts.styles.description.margin != Default::default()
            || parts.styles.description.padding != Default::default()
            || parts.styles.description.border_widths != Default::default()
            || parts.styles.description.gap != Default::default()
            || parts.styles.description.flex_direction.is_some()
        {
            return (None, px(0.));
        }
        let g = parts.tokens.globals();
        let body = match parts.styles.body.size.width {
            Some(gpui::Length::Definite(length)) => f64::from(
                length.to_pixels(px(metrics.swatch_size as f32).into(), window.rem_size()),
            ),
            _ => metrics.swatch_size,
        };
        let decoration = body + metrics.padding * 2. + g.line_width * 2.;
        if !(self.show_text || self.text_render.is_some())
            || parts.styles.description.display == Some(gpui::Display::None)
        {
            return (Some(px(decoration.max(metrics.height) as f32)), px(0.));
        }
        let margins = metrics.text_gap + metrics.text_end_margin;
        if self.text_render.is_some() {
            let snapped = |value: f64| f64::from(window.pixel_snap(px(value as f32)));
            let scale = f64::from(window.scale_factor());
            let border = if g.line_width > 0. {
                snapped(g.line_width).max(1. / scale)
            } else {
                0.
            };
            let native = snapped(body)
                + snapped(metrics.padding) * 2.
                + border * 2.
                + snapped(metrics.text_gap)
                + snapped(metrics.text_end_margin);
            return (None, px((decoration + margins - native) as f32));
        }
        let mut authored = gpui::Style::default();
        authored.refine(
            div()
                .font(super::text::picker_font(&config.font))
                .text_size(px(metrics.font_size as f32))
                .style(),
        );
        authored.refine(root);
        authored.refine(&parts.styles.description);
        let mut style = window.text_style();
        style.refine(&authored.text);
        let font_size = style.font_size.to_pixels(window.rem_size());
        let advance = |text: SharedString| {
            f64::from(
                window
                    .text_system()
                    .shape_line(text.clone(), font_size, &[style.to_run(text.len())], None)
                    .width,
            )
        };
        let label =
            if let Some(gpui::Length::Definite(length)) = parts.styles.description.size.width {
                f64::from(length.to_pixels(px(0.).into(), window.rem_size()))
            } else if let ColorPickerValue::Gradient(stops) = value {
                stops
                    .iter()
                    .enumerate()
                    .map(|(index, stop)| {
                        advance(
                            format!(
                                "{} {}%{}",
                                stop.color.to_rgb_string(),
                                stop.percent,
                                if index + 1 < stops.len() { ", " } else { "" }
                            )
                            .into(),
                        )
                    })
                    .sum()
            } else {
                advance(trigger_text(
                    value,
                    format,
                    &config.locale.color_picker.transparent,
                ))
            };
        (
            Some(px((decoration + margins + label).max(metrics.height) as f32)),
            px(0.),
        )
    }
}

impl Styled for ColorPicker {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.styles.root
    }
}

struct Mount {
    state: WeakEntity<ColorPickerState>,
    _observer: Subscription,
    _release: Subscription,
    motion: super::motion::TriggerMotion,
    epoch: std::time::Instant,
    frame_pending: bool,
    frame_generation: u64,
    _window: [Subscription; 2],
}
impl Mount {
    fn new(
        state: &Entity<ColorPickerState>,
        window: &mut Window,
        cx: &mut gpui::Context<Self>,
    ) -> Self {
        let active = cx.observe_window_activation(window, |this, _, cx| {
            this.cancel_motion();
            cx.notify();
        });
        let visible = cx.observe_window_visibility(window, |this, _, _, cx| {
            this.cancel_motion();
            cx.notify();
        });
        Self {
            state: state.downgrade(),
            _observer: cx.observe(state, |_, _, cx| cx.notify()),
            _release: cx.on_release(|this: &mut Self, cx| {
                let _ = this.state.update(cx, |state, _| state.release_mount());
            }),
            motion: Default::default(),
            epoch: cx.background_executor().now(),
            frame_pending: false,
            frame_generation: 0,
            _window: [active, visible],
        }
    }
    fn cancel_motion(&mut self) {
        self.motion.cancel();
        self.frame_pending = false;
        self.frame_generation = self.frame_generation.wrapping_add(1);
    }
    fn colors(
        &mut self,
        target: super::motion::TriggerColors,
        duration: std::time::Duration,
        policy: crate::behavior::MotionPolicy,
        window: &Window,
        cx: &mut gpui::Context<Self>,
    ) -> super::motion::TriggerColors {
        if !policy.allows_motion() {
            self.frame_generation = self.frame_generation.wrapping_add(1);
            self.frame_pending = false;
        }
        let now = cx
            .background_executor()
            .now()
            .saturating_duration_since(self.epoch);
        let (colors, active) = self.motion.resolve(target, now, duration, policy);
        if active && !self.frame_pending {
            self.frame_pending = true;
            let generation = self.frame_generation;
            let mounted = cx.weak_entity();
            window.on_next_frame(move |window, cx| {
                let _ = mounted.update(cx, |this, cx| {
                    if this.frame_generation != generation {
                        return;
                    }
                    this.frame_pending = false;
                    if window.is_visible() && window.is_window_active() && !cx.reduce_motion() {
                        cx.notify();
                    } else {
                        this.cancel_motion();
                    }
                });
            });
        }
        colors
    }
}

/// Render inside Popup's trigger context so its real focus owner advertises
/// native Expand/Collapse/Invoke actions. Custom children claim that context
/// themselves, preserving their own accessible identity and activation policy.
#[derive(IntoElement)]
struct NativePickerTrigger {
    id: ElementId,
    state: Entity<ColorPickerState>,
    focus: gpui::FocusHandle,
    child: AnyElement,
    disabled: bool,
    open: bool,
    custom: bool,
    accessibility: Accessibility,
    ring: FocusRing,
    preferred_width: Option<gpui::Pixels>,
    width_correction: gpui::Pixels,
    widths: [Option<gpui::Length>; 3],
}
struct TriggerActivation {
    guard: ActivationGuard,
    _blur: Subscription,
}
impl RenderOnce for NativePickerTrigger {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let binding = if self.custom {
            PopupTrigger::default()
        } else {
            PopupTrigger::current(cx)
        };
        let hovered = self.state.clone();
        let mut element = div()
            .id(self.id.clone())
            .relative()
            .on_hover(move |hovered_value, _, cx| {
                hovered.update(cx, |s, cx| {
                    if s.hovered != *hovered_value {
                        s.hovered = *hovered_value;
                        cx.notify();
                    }
                })
            })
            .child(self.child);
        if self.custom {
            element = element.role(Role::Group);
        } else {
            element = element
                .track_focus(&self.focus)
                .role(Role::Button)
                .aria_expanded(self.open);
        }
        if self.custom {
            let measure = self.state.clone();
            element = element.child(
                canvas(
                    move |bounds, _, cx| {
                        measure.update(cx, |s, _| {
                            s.bounds.trigger = Some(bounds);
                            s.bounds.trigger_swatch = None;
                        })
                    },
                    |_, (), _, _| (),
                )
                .absolute()
                .inset_0(),
            );
        }
        element = if self.disabled {
            element.cursor_not_allowed()
        } else {
            element.cursor_pointer()
        };
        if self.custom {
            return super::anchor::IntrinsicAnchor::new(
                self.accessibility
                    .apply(self.ring.apply(element, &self.focus)),
                None,
                px(0.),
            )
            .into_any_element();
        }
        let focus = self.focus.clone();
        let disabled = self.disabled;
        let activation =
            window.use_keyed_state((self.id, "native-picker-trigger"), cx, move |window, cx| {
                let guard = ActivationGuard::new(Availability {
                    disabled,
                    busy: false,
                });
                TriggerActivation {
                    _blur: guard.observe_blur(&focus, window, cx),
                    guard,
                }
            });
        let guard = activation.read(cx).guard.clone();
        guard.update(
            Availability {
                disabled,
                busy: false,
            },
            &self.focus,
            window,
            cx,
        );
        let element = guard.install(element, |_, _, _| {});
        let element = binding.install(element, &guard, &self.focus);
        super::anchor::IntrinsicAnchor::new(
            binding.apply(
                self.accessibility
                    .apply(self.ring.apply(element, &self.focus)),
            ),
            self.preferred_width,
            self.width_correction,
        )
        .with_widths(self.widths[0], self.widths[1], self.widths[2])
        .into_any_element()
    }
}

impl RenderOnce for ColorPicker {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let parts = self.configure(window, cx);
        let config = parts.scope.snapshot(cx);
        let g = parts.tokens.globals();
        let s = self.state.read(cx);
        let disabled = s.disabled;
        let open = s.is_open();
        let focus = s.trigger_focus.clone();
        let value = s.accepted.clone();
        let format = s.format;
        let metrics = parts
            .tokens
            .trigger_metrics(self.size.unwrap_or(config.size));
        let border = match self.status {
            ValidationStatus::Error => {
                if s.hovered && !disabled {
                    g.color_error_hover
                } else {
                    g.color_error
                }
            }
            ValidationStatus::Warning => {
                if s.hovered && !disabled {
                    g.color_warning_hover
                } else {
                    g.color_warning
                }
            }
            _ => {
                if disabled {
                    g.color_border
                } else if s.hovered {
                    g.color_primary_hover
                } else if open {
                    g.color_primary
                } else {
                    g.color_border
                }
            }
        };
        let body_dimension = |length: Option<gpui::Length>| match length {
            Some(gpui::Length::Definite(length)) => {
                let value = f64::from(
                    length.to_pixels(px(metrics.swatch_size as f32).into(), window.rem_size()),
                );
                if value.is_finite() {
                    value.max(0.)
                } else {
                    metrics.swatch_size
                }
            }
            _ => metrics.swatch_size,
        };
        let trigger_min_width = metrics.height.max(
            body_dimension(parts.styles.body.size.width) + metrics.padding * 2. + g.line_width * 2.,
        );
        let trigger_min_height = metrics.height.max(
            body_dimension(parts.styles.body.size.height)
                + metrics.padding * 2.
                + g.line_width * 2.,
        );
        let custom_trigger = self.child.is_some();
        let (preferred_width, width_correction) =
            self.trigger_width_hint(&parts, &metrics, &value, format, &config, window);
        let observed = self.state.clone();
        let mounted = window.use_keyed_state(self.id.clone(), cx, move |window, cx| {
            Mount::new(&observed, window, cx)
        });
        focus.clone().tab_stop(!disabled && !custom_trigger);
        let trigger = if let Some(child) = self.child.take() {
            mounted.update(cx, |mount, _| mount.cancel_motion());
            child
        } else {
            let mut trigger = div()
                .id("trigger")
                .relative()
                .flex()
                .items_start()
                .justify_center()
                .flex_none()
                .min_w(px(trigger_min_width as f32))
                .min_h(px(trigger_min_height as f32))
                .p(px(metrics.padding as f32))
                .border(px(g.line_width as f32))
                .border_color(border)
                .rounded(px(metrics.radius as f32))
                .bg(if disabled {
                    g.color_bg_container_disabled
                } else {
                    g.color_bg_elevated
                })
                .font(super::text::picker_font(&config.font))
                .text_size(px(metrics.font_size as f32))
                .line_height(relative(g.line_height as f32));
            if config.direction == Direction::Rtl {
                trigger = trigger.flex_row_reverse();
            }
            let swatch_measure = self.state.clone();
            trigger = trigger.child(
                div()
                    .relative()
                    .flex_none()
                    .child(trigger_swatch(
                        &self.state,
                        value.clone(),
                        metrics.swatch_size,
                        metrics.swatch_radius,
                        &parts.tokens,
                        &parts.styles.body,
                        &parts.styles.content,
                        config.direction,
                        window,
                    ))
                    .child(
                        canvas(
                            move |bounds, _, cx| {
                                swatch_measure
                                    .update(cx, |s, _| s.bounds.trigger_swatch = Some(bounds))
                            },
                            |_, (), _, _| (),
                        )
                        .absolute()
                        .inset_0(),
                    ),
            );
            if self.show_text || self.text_render.is_some() {
                let text = if let Some(render) = &self.text_render {
                    render(&value, window, cx)
                } else if let ColorPickerValue::Gradient(stops) = &value {
                    let active = self.state.read(cx).model.active_index();
                    let mut cells = div().flex().items_center();
                    for (index, stop) in stops.iter().enumerate() {
                        cells = cells.child(
                            div()
                                .flex_none()
                                .whitespace_nowrap()
                                .text_color(if disabled || (open && index != active) {
                                    g.color_text_disabled
                                } else {
                                    g.color_text
                                })
                                .child(intrinsic_text(
                                    format!(
                                        "{} {}%{}",
                                        stop.color.to_rgb_string(),
                                        stop.percent,
                                        if index + 1 < stops.len() { ", " } else { "" }
                                    )
                                    .into(),
                                )),
                        );
                    }
                    cells.into_any_element()
                } else {
                    intrinsic_text(trigger_text(
                        &value,
                        format,
                        &config.locale.color_picker.transparent,
                    ))
                };
                let mut label = div()
                    .ml(px(metrics.text_gap as f32))
                    .mr(px(metrics.text_end_margin as f32))
                    .flex_none()
                    .self_center()
                    .whitespace_nowrap()
                    .text_color(if disabled {
                        g.color_text_disabled
                    } else {
                        g.color_text
                    })
                    .child(text);
                if config.direction == Direction::Rtl {
                    label = label
                        .ml(px(metrics.text_end_margin as f32))
                        .mr(px(metrics.text_gap as f32));
                }
                label.style().refine(&parts.styles.description);
                trigger = trigger.child(label);
            }
            trigger.style().refine(&parts.styles.root);
            let mut effective = gpui::Style::default();
            effective.refine(trigger.style());
            let mut halo_target = match self.status {
                ValidationStatus::Error => g.color_error_outline,
                ValidationStatus::Warning => g.color_warning_outline,
                _ => g.control_outline,
            }
            .to_hsla();
            if !open || disabled || parts.styles.root.box_shadow.is_some() {
                halo_target = gpui::transparent_black();
            }
            let target = super::motion::TriggerColors {
                border: effective.border_color.unwrap_or_else(|| border.to_hsla()),
                background: effective
                    .background
                    .as_ref()
                    .and_then(gpui::Fill::color)
                    .and_then(|background| background.as_solid()),
                halo: halo_target,
            };
            let policy = crate::behavior::MotionPolicy::new(
                config.motion
                    && g.motion
                    && window.is_visible()
                    && window.is_window_active()
                    && effective.display != gpui::Display::None
                    && effective.visibility != gpui::Visibility::Hidden
                    && effective.opacity.unwrap_or(1.) > 0.,
                cx.reduce_motion(),
            );
            let colors = mounted.update(cx, |mount, cx| {
                mount.colors(target, g.motion_duration_mid, policy, window, cx)
            });
            trigger = trigger.border_color(colors.border);
            if let Some(background) = colors.background {
                trigger = trigger.bg(background);
            }
            let observe = self.state.clone();
            let scale = f64::from(window.scale_factor());
            let borders = effective
                .border_widths
                .to_pixels(window.rem_size())
                .map(|width| {
                    if *width > px(0.) {
                        window.pixel_snap(*width).max(px((1. / scale) as f32))
                    } else {
                        px(0.)
                    }
                });
            let halo_color = colors.halo;
            let halo_radii = effective.corner_radii.to_pixels(window.rem_size());
            let halo_width = px(g.control_outline_width as f32);
            trigger = trigger.child(
                canvas(
                    move |bounds, _, cx| {
                        observe.update(cx, |s, _| {
                            s.bounds.trigger = Some(gpui::Bounds::new(
                                point(bounds.left() - borders.left, bounds.top() - borders.top),
                                gpui::size(
                                    bounds.size.width + (borders.left + borders.right),
                                    bounds.size.height + (borders.top + borders.bottom),
                                ),
                            ))
                        })
                    },
                    move |bounds, (), window, _| {
                        if halo_color.a > 0. {
                            window.paint_quad(
                                gpui::outline(
                                    bounds.dilate(halo_width),
                                    halo_color,
                                    Default::default(),
                                )
                                .border_widths(halo_width)
                                .corner_radii(halo_radii.map(|radius| *radius + halo_width)),
                            );
                        }
                    },
                )
                .absolute()
                .inset_0(),
            );
            trigger.into_any_element()
        };
        let trigger = NativePickerTrigger {
            id: self.id.clone(),
            state: self.state.clone(),
            focus,
            child: trigger,
            disabled,
            open,
            custom: custom_trigger,
            accessibility: Accessibility {
                label: self
                    .label
                    .or_else(|| Some(config.locale.color_picker.root_label.clone())),
                automation_id: self.automation_id,
                disabled,
                ..Accessibility::new(if custom_trigger {
                    Role::Group
                } else {
                    Role::Button
                })
            },
            ring: FocusRing::new(g.color_primary_border, g.line_width_focus as f32)
                .radius(px(metrics.radius as f32)),
            preferred_width,
            width_correction,
            widths: [
                parts.styles.root.size.width,
                parts.styles.root.min_size.width,
                parts.styles.root.max_size.width,
            ],
        };
        let state = self.state.clone();
        let popup_handle = self.state.read(cx).popup.clone();
        let render = self.panel_render;
        let content_parts = parts.clone();
        let initial_focus = self
            .state
            .update(cx, |s, cx| s.control_focus(Control::Palette, window, cx));
        let popup_padding = config
            .theme
            .component::<crate::popover::PopoverTheme>()
            .inner_padding;
        let mut surface = div()
            .p(px(popup_padding as f32))
            .font(super::text::picker_font(&config.font))
            .text_color(g.color_text)
            .text_size(px(g.font_size as f32))
            .line_height(relative(g.line_height as f32));
        surface.style().refine(&parts.styles.popup);
        Popup::new((self.id, "popup"))
            .defaults(
                config.popover,
                config
                    .theme
                    .component::<crate::popover::PopoverTheme>()
                    .z_index_popup,
            )
            .scope(parts.scope)
            .handle(&popup_handle)
            .initial_focus(&initial_focus)
            .open(open)
            .disabled(disabled)
            .triggers(self.trigger)
            .placement(self.placement)
            .arrow(self.arrow.unwrap_or(config.color_picker.arrow))
            .auto_adjust_overflow(self.auto_adjust)
            .destroy_on_hidden(self.destroy)
            .accessibility_role(Role::Dialog)
            .accessibility_label(config.locale.color_picker.panel_label.clone())
            .surface_style(surface.style().clone())
            .background(g.color_bg_elevated)
            .radius(px(g.border_radius_lg as f32))
            .shadows(g.box_shadow_secondary.clone())
            .on_open_change(move |change, window, cx| {
                state.update(cx, |s, cx| {
                    s.request_open(change.open, window, cx);
                    if !s.is_open()
                        && matches!(
                            change.reason,
                            crate::popup::PopupOpenReason::Escape
                                | crate::popup::PopupOpenReason::Accessibility
                        )
                    {
                        s.restore_trigger_focus(window, cx);
                    }
                })
            })
            .child(trigger)
            .content(move |_, window, cx| {
                let element = if let Some(render) = &render {
                    render(&content_parts, window, cx)
                } else {
                    content_parts.panel(window, cx)
                };
                let measure = content_parts.state.clone();
                let padding = px(popup_padding as f32);
                div().relative().child(element).child(
                    canvas(
                        move |bounds, _, cx| {
                            measure.update(cx, |s, _| {
                                s.bounds.panel = Some(gpui::Bounds::new(
                                    bounds.origin - point(padding, padding),
                                    bounds.size + gpui::size(padding * 2., padding * 2.),
                                ));
                            })
                        },
                        |_, (), _, _| (),
                    )
                    .absolute()
                    .inset_0(),
                )
            })
    }
}

fn trigger_text(
    value: &ColorPickerValue,
    format: ColorPickerFormat,
    transparent: &SharedString,
) -> SharedString {
    if value.is_cleared() {
        return transparent.clone();
    }
    if value.is_gradient() {
        return value.to_css_string().into();
    }
    let color = value.active_color(0);
    match format {
        ColorPickerFormat::Hex => {
            let hex = color
                .to_hex_string()
                .chars()
                .take(7)
                .collect::<String>()
                .to_ascii_uppercase();
            let a = (color.to_hsb().3 * 100.).round();
            if a < 100. {
                format!("{hex},{a}%").into()
            } else {
                hex.into()
            }
        }
        ColorPickerFormat::Rgb => color.to_rgb_string().into(),
        ColorPickerFormat::Hsb => color.to_hsb_string().into(),
    }
}

// A normal layout node retains the shaped advance. GPUI rounds measured text
// leaves up to physical pixels; allowing that rounded width to size a trigger
// accumulates visible offsets in rows of color pickers at fractional DPI.
fn intrinsic_text(text: SharedString) -> AnyElement {
    super::text::IntrinsicText::new(text).into_any_element()
}

fn surface_radius(default: f32, style: &StyleRefinement, window: &Window) -> f32 {
    let mut native = gpui::Style::default();
    native.refine(div().rounded(px(default)).style());
    native.refine(style);
    f32::from(native.corner_radii.to_pixels(window.rem_size()).top_left)
}

/// Standalone native panel using the same retained controller as ColorPicker.
#[derive(IntoElement)]
pub struct ColorPickerPanel {
    picker: ColorPicker,
    style: StyleRefinement,
}
impl ColorPickerPanel {
    pub fn new(id: impl Into<ElementId>, state: &Entity<ColorPickerState>) -> Self {
        Self {
            picker: ColorPicker::new(id, state),
            style: StyleRefinement::default(),
        }
    }
    pub fn scope(mut self, v: ConfigScope) -> Self {
        self.picker = self.picker.scope(v);
        self
    }
    pub fn value(mut self, v: impl Into<ColorPickerValue>) -> Self {
        self.picker = self.picker.value(v);
        self
    }
    pub fn default_value(mut self, v: impl Into<ColorPickerValue>) -> Self {
        self.picker = self.picker.default_value(v);
        self
    }
    pub fn format(mut self, v: ColorPickerFormat) -> Self {
        self.picker = self.picker.format(v);
        self
    }
    pub fn default_format(mut self, v: ColorPickerFormat) -> Self {
        self.picker = self.picker.default_format(v);
        self
    }
    pub fn modes(mut self, v: impl IntoIterator<Item = ColorPickerMode>) -> Self {
        self.picker = self.picker.modes(v);
        self
    }
    pub fn disabled(mut self, v: bool) -> Self {
        self.picker = self.picker.disabled(v);
        self
    }
    pub fn disabled_alpha(mut self, v: bool) -> Self {
        self.picker = self.picker.disabled_alpha(v);
        self
    }
    pub fn disabled_format(mut self, v: bool) -> Self {
        self.picker = self.picker.disabled_format(v);
        self
    }
    pub fn allow_clear(mut self, v: bool) -> Self {
        self.picker = self.picker.allow_clear(v);
        self
    }
    pub fn presets(mut self, v: impl IntoIterator<Item = ColorPickerPreset>) -> Self {
        self.picker = self.picker.presets(v);
        self
    }
    pub fn styles(mut self, v: ColorPickerStyles) -> Self {
        self.picker = self.picker.styles(v);
        self
    }
    pub fn panel_render<E: IntoElement>(
        mut self,
        f: impl Fn(&ColorPickerPanelParts, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.picker = self.picker.panel_render(f);
        self
    }
    pub fn on_change(
        mut self,
        f: impl Fn(&ColorPickerChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.picker = self.picker.on_change(f);
        self
    }
    pub fn on_change_complete(
        mut self,
        f: impl Fn(&ColorPickerChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.picker = self.picker.on_change_complete(f);
        self
    }
}
impl Styled for ColorPickerPanel {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}
impl RenderOnce for ColorPickerPanel {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let parts = self.picker.configure(window, cx);
        let element = if let Some(render) = self.picker.panel_render {
            render(&parts, window, cx)
        } else {
            parts.panel(window, cx)
        };
        let config = parts.scope.snapshot(cx);
        let g = parts.tokens.globals();
        let state = parts.state.clone();
        let mut panel = div()
            .id(self.picker.id)
            .relative()
            .p(px(g.padding_sm as f32))
            .bg(g.color_bg_elevated)
            .rounded(px(g.border_radius_lg as f32))
            .font(super::text::picker_font(&config.font))
            .text_color(g.color_text)
            .text_size(px(g.font_size as f32))
            .line_height(relative(g.line_height as f32))
            .child(element)
            .child(
                canvas(
                    move |bounds, _, cx| state.update(cx, |s, _| s.bounds.panel = Some(bounds)),
                    |_, (), _, _| (),
                )
                .absolute()
                .inset_0(),
            );
        panel.style().refine(&self.style);
        panel
    }
}

struct RangeDescription {
    control: Control,
    value: f64,
    max: f64,
    label: SharedString,
}

/// Fresh native element factories for rearranging the configured panel parts.
#[derive(Clone)]
pub struct ColorPickerPanelParts {
    state: Entity<ColorPickerState>,
    scope: ConfigScope,
    tokens: Arc<ColorPickerTokens>,
    styles: ColorPickerStyles,
    allow_clear: bool,
    presets: Option<Arc<[ColorPickerPreset]>>,
}
impl ColorPickerPanelParts {
    pub fn panel(&self, window: &mut Window, cx: &mut App) -> AnyElement {
        let g = self.tokens.globals();
        let mut body = div()
            .flex()
            .flex_col()
            .w(px(self.tokens.width as f32))
            .child(self.picker(window, cx));
        if self.presets.is_some() {
            body = body
                .child(
                    div()
                        .h(px(g.line_width as f32))
                        .bg(g.color_split)
                        .mt(px(g.margin_sm as f32))
                        .mb(px(g.margin_xs as f32)),
                )
                .child(self.presets(window, cx));
        }
        div().child(body).into_any_element()
    }
    pub fn picker(&self, window: &mut Window, cx: &mut App) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let g = self.tokens.globals();
        let messages = &config.locale.color_picker;
        self.state.update(cx, |s, _| {
            s.bounds.editors.clear();
            s.bounds.presets.clear();
            s.bounds.handles.clear();
            s.bounds.clear = None;
        });
        let mut root = div().flex().flex_col().w(px(self.tokens.width as f32));
        if self.allow_clear || self.state.read(cx).modes.len() > 1 {
            let mut operation = div()
                .flex()
                .items_center()
                .justify_between()
                .mb(px(g.margin_xs as f32));
            if config.direction == Direction::Rtl {
                operation = operation.flex_row_reverse();
            }
            if self.state.read(cx).modes.len() > 1 {
                let state = self.state.clone();
                let modes = self.state.read(cx).modes.clone();
                operation = operation.child(
                    Segmented::new("mode")
                        .scope(self.scope.clone())
                        .size(Size::Small)
                        .disabled(self.state.read(cx).disabled)
                        .options(modes.into_iter().map(|mode| {
                            SegmentedOption::new(
                                if mode == ColorPickerMode::Single {
                                    "single"
                                } else {
                                    "gradient"
                                },
                                mode,
                                if mode == ColorPickerMode::Single {
                                    messages.single.clone()
                                } else {
                                    messages.gradient.clone()
                                },
                            )
                        }))
                        .value(self.state.read(cx).model.mode())
                        .on_change(move |mode, window, cx| {
                            state.update(cx, |s, cx| s.choose_mode(*mode, window, cx))
                        }),
                );
            }
            {
                let state = self.state.clone();
                let action = Rc::new(move |window: &mut Window, cx: &mut App| {
                    state.update(cx, |s, cx| s.clear(window, cx))
                });
                let mut control = button(
                    self,
                    Control::Clear,
                    messages.clear.clone(),
                    action,
                    window,
                    cx,
                )
                .relative()
                .w(px(self.tokens.preset_color_size as f32))
                .h(px(self.tokens.preset_color_size as f32))
                .ml_auto()
                .child(swatch(
                    &self.state,
                    ColorPickerValue::Cleared,
                    self.tokens.preset_color_size,
                    g.border_radius_sm,
                    &self.tokens,
                    false,
                ));
                if config.direction == Direction::Rtl {
                    control = control.ml_0().mr_auto();
                }
                let measure = self.state.clone();
                control = control.child(
                    canvas(
                        move |bounds, _, cx| {
                            measure.update(cx, |s, _| s.bounds.clear = Some(bounds))
                        },
                        |_, (), _, _| (),
                    )
                    .absolute()
                    .inset_0(),
                );
                operation = operation.child(
                    Accessibility {
                        disabled: self.state.read(cx).disabled,
                        ..Accessibility::new(Role::Button)
                    }
                    .apply(control),
                );
            }
            root = root.child(operation);
        }
        let mode = self.state.read(cx).model.mode();
        let show_gradient = mode == ColorPickerMode::Gradient
            || (self.state.read(cx).modes.len() == 1
                && self.state.read(cx).modes[0] == ColorPickerMode::Gradient);
        if show_gradient {
            root = root.child(
                self.rail(Surface::Gradient, window, cx)
                    .mb(px(g.margin_xs as f32)),
            );
        }
        root = root.child(self.palette(window, cx).mb(px(g.margin_sm as f32)));
        let disabled_alpha = self.state.read(cx).disabled_alpha;
        let mut sliders = div()
            .flex()
            .flex_col()
            .flex_1()
            .min_w(px(0.))
            .h(px(self.tokens.preview_size as f32));
        sliders = if disabled_alpha {
            sliders.justify_center()
        } else {
            sliders.justify_between()
        };
        sliders = sliders.child(self.rail(Surface::Hue, window, cx));
        if !disabled_alpha {
            sliders = sliders.child(self.rail(Surface::Alpha, window, cx));
        }
        let preview = swatch(
            &self.state,
            ColorPickerValue::Single(
                self.state
                    .read(cx)
                    .model
                    .value()
                    .active_color(self.state.read(cx).model.active_index()),
            ),
            self.tokens.preview_size,
            g.border_radius_sm,
            &self.tokens,
            true,
        );
        let mut row = div()
            .flex()
            .gap(px(g.margin_sm as f32))
            .mb(px(g.margin_sm as f32));
        row = if config.direction == Direction::Rtl {
            row.flex_row_reverse().child(sliders).child(preview)
        } else {
            row.child(sliders).child(preview)
        };
        root = root.child(row).child(self.editors(window, cx));
        root.into_any_element()
    }

    fn palette(&self, window: &mut Window, cx: &mut App) -> gpui::Stateful<gpui::Div> {
        let color = self.state.read(cx).model.active_color();
        let g = self.tokens.globals();
        let measure = self.state.clone();
        let drag = self.state.downgrade();
        let end = drag.clone();
        let focus = self
            .state
            .update(cx, |s, cx| s.control_focus(Control::Palette, window, cx));
        let mut palette = div()
            .id("palette")
            .relative()
            .w_full()
            .h(px(self.tokens.palette_height() as f32))
            .track_focus(&focus)
            .role(Role::Group)
            .cursor_crosshair();
        palette = keyboard(palette, self.state.clone(), Control::Palette);
        let diameter = self.tokens.handler_size as f32;
        let radius = surface_radius(g.border_radius_sm as f32, &self.styles.palette, window);
        let custom_background = self.styles.palette.background.is_some();
        let rim = g.color_bg_elevated;
        let focused = focus.is_focused(window)
            || [Control::Saturation, Control::Brightness]
                .iter()
                .any(|control| {
                    self.state
                        .read(cx)
                        .focuses
                        .get(control)
                        .is_some_and(|focus| focus.is_focused(window))
                });
        let (_, s, b, _) = color.to_hsb();
        let focus_color = g.color_primary_active;
        let appearance = paint::SurfaceAppearance {
            radius,
            checker: g.color_fill_secondary,
            inset: g.color_text_quaternary,
        };
        let thumb_appearance = paint::ThumbAppearance {
            rim,
            outline: g.color_fill_secondary,
            active_outline: focus_color,
            active: focused,
            scale: 1.,
        };
        palette = palette.child(
            canvas(
                move |bounds, _, cx| {
                    measure.update(cx, |state, _| {
                        state.bounds.palette = Some(bounds);
                        let center = point(
                            bounds.left() + bounds.size.width * s as f32,
                            bounds.top() + bounds.size.height * (1. - b) as f32,
                        );
                        record_handle(
                            state,
                            "palette",
                            0,
                            gpui::Bounds::new(
                                center - point(px(diameter / 2.), px(diameter / 2.)),
                                gpui::size(px(diameter), px(diameter)),
                            ),
                        );
                    })
                },
                move |bounds, (), window, _| {
                    if !custom_background {
                        paint::paint_palette(bounds, color, appearance, window);
                    }
                    window.with_content_mask(Some(gpui::ContentMask { bounds }), |window| {
                        paint::paint_thumb(
                            point(
                                bounds.left() + bounds.size.width * s as f32,
                                bounds.top() + bounds.size.height * (1. - b) as f32,
                            ),
                            diameter,
                            color,
                            thumb_appearance,
                            window,
                        );
                    });
                    let target = drag.clone();
                    window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
                        if phase.capture() {
                            let _ = target
                                .update(cx, |s, cx| s.pointer_move(event.position, window, cx));
                        }
                    });
                    let target = end.clone();
                    window.on_mouse_event(move |event: &MouseUpEvent, phase, window, cx| {
                        if phase.capture() && event.button == MouseButton::Left {
                            let _ = target.update(cx, |s, cx| s.pointer_end(window, cx));
                        }
                    });
                },
            )
            .absolute()
            .inset_0(),
        );
        let state = self.state.clone();
        palette = palette.on_mouse_down(MouseButton::Left, move |event, window, cx| {
            state.update(cx, |s, cx| {
                s.pointer_start(Surface::Palette, None, event.position, window, cx)
            });
            window.prevent_default();
            cx.stop_propagation();
        });
        // Both dimensions have real native range actions and focus handles. Their
        // transparent hit regions share the visible thumb and contain no duplicate
        // editor state or substitute accessibility tree.
        let config = self.scope.snapshot(cx);
        for (control, value, label) in [
            (
                Control::Saturation,
                s * 100.,
                config.locale.color_picker.saturation.clone(),
            ),
            (
                Control::Brightness,
                b * 100.,
                config.locale.color_picker.brightness.clone(),
            ),
        ] {
            let handle = self.range(
                div()
                    .id(if control == Control::Saturation {
                        "saturation"
                    } else {
                        "brightness"
                    })
                    .absolute()
                    .left(relative(s as f32))
                    .top(relative((1. - b) as f32))
                    .w(px(1.))
                    .h(px(1.)),
                RangeDescription {
                    control,
                    value,
                    max: 100.,
                    label,
                },
                window,
                cx,
            );
            palette = palette.child(handle);
        }
        palette.style().refine(&self.styles.palette);
        palette
    }

    fn rail(
        &self,
        surface: Surface,
        window: &mut Window,
        cx: &mut App,
    ) -> gpui::Stateful<gpui::Div> {
        let g = self.tokens.globals();
        let state = self.state.read(cx);
        let color = state.model.active_color();
        let stops = state.model.stops().to_vec();
        let value = state.model.value().clone();
        let measure = self.state.clone();
        let drag = self.state.downgrade();
        let end = drag.clone();
        let height = self.tokens.slider_height as f32;
        let checker = g.color_fill_secondary;
        let radius = surface_radius(height / 2., &self.styles.slider, window);
        let custom_background = self.styles.slider.background.is_some();
        let appearance = paint::SurfaceAppearance {
            radius,
            checker,
            inset: g.color_text_quaternary,
        };
        let mut rail = div()
            .id(match surface {
                Surface::Hue => "hue",
                Surface::Alpha => "alpha",
                _ => "gradient",
            })
            .relative()
            .h(px(height))
            .w_full()
            .cursor_pointer();
        rail = rail.child(
            canvas(
                move |bounds, _, cx| {
                    measure.update(cx, |s, _| match surface {
                        Surface::Hue => s.bounds.hue = Some(bounds),
                        Surface::Alpha => s.bounds.alpha = Some(bounds),
                        _ => s.bounds.gradient = Some(bounds),
                    })
                },
                move |bounds, (), window, _| {
                    if !custom_background {
                        match surface {
                            Surface::Hue => paint::paint_hue(bounds, appearance, window),
                            Surface::Alpha => paint::paint_alpha(bounds, color, appearance, window),
                            _ => paint::paint_gradient_rail(bounds, &value, appearance, window),
                        }
                    }
                    let target = drag.clone();
                    window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
                        if phase.capture() {
                            let _ = target
                                .update(cx, |s, cx| s.pointer_move(event.position, window, cx));
                        }
                    });
                    let target = end.clone();
                    window.on_mouse_event(move |event: &MouseUpEvent, phase, window, cx| {
                        if phase.capture() && event.button == MouseButton::Left {
                            let _ = target.update(cx, |s, cx| s.pointer_end(window, cx));
                        }
                    });
                },
            )
            .absolute()
            .inset_0(),
        );
        let state = self.state.clone();
        rail = rail.on_mouse_down(MouseButton::Left, move |event, window, cx| {
            state.update(cx, |s, cx| {
                s.pointer_start(surface, None, event.position, window, cx)
            });
            window.prevent_default();
            cx.stop_propagation();
        });
        let config = self.scope.snapshot(cx);
        let messages = &config.locale.color_picker;
        let (h, _, _, a) = color.to_hsb();
        let handles = match surface {
            Surface::Hue => vec![(
                Control::Hue,
                h / 360.,
                h,
                360.,
                color.with_saturation(1.).with_brightness(1.).with_alpha(1.),
                messages.hue.clone(),
            )],
            Surface::Alpha => vec![(
                Control::Alpha,
                a,
                a * 100.,
                100.,
                color,
                messages.alpha.clone(),
            )],
            _ => stops
                .iter()
                .enumerate()
                .map(|(index, s)| {
                    (
                        Control::Stop(s.id),
                        s.percent / 100.,
                        s.percent,
                        100.,
                        s.color,
                        messages
                            .gradient_stop
                            .replace("{index}", &(index + 1).to_string())
                            .into(),
                    )
                })
                .collect(),
        };
        for (index, (control, fraction, numeric, max, fill, label)) in
            handles.into_iter().enumerate()
        {
            let control_copy = control.clone();
            let diameter = self.tokens.handler_size_sm as f32;
            let rim = g.color_bg_elevated;
            let focus_color = g.color_primary_active;
            let focus = self
                .state
                .update(cx, |s, cx| s.control_focus(control.clone(), window, cx));
            let focused = focus.is_focused(window)
                || (surface == Surface::Gradient
                    && index == self.state.read(cx).model.active_index());
            let scale = if surface == Surface::Gradient && !focused {
                0.8
            } else {
                1.
            };
            let thumb_appearance = paint::ThumbAppearance {
                rim,
                outline: g.color_fill_secondary,
                active_outline: focus_color,
                active: focused,
                scale,
            };
            let kind = match surface {
                Surface::Hue => "hue",
                Surface::Alpha => "alpha",
                _ => "gradient",
            };
            let measure = self.state.clone();
            let mut handle = div()
                .id(match control {
                    Control::Stop(id) => ElementId::from(("stop", id)),
                    _ => ElementId::from("thumb"),
                })
                .absolute()
                .left(relative(fraction as f32))
                .top(px((height - diameter) / 2.))
                .ml(px(-diameter / 2.))
                .size(px(diameter));
            handle = handle.child(
                canvas(
                    move |bounds, _, cx| {
                        measure.update(cx, |state, _| {
                            let size = px(diameter * scale);
                            record_handle(
                                state,
                                kind,
                                index,
                                gpui::Bounds::new(
                                    bounds.center() - point(size / 2., size / 2.),
                                    gpui::size(size, size),
                                ),
                            );
                        })
                    },
                    move |bounds, (), window, _| {
                        paint::paint_thumb(
                            bounds.center(),
                            diameter,
                            fill,
                            thumb_appearance,
                            window,
                        )
                    },
                )
                .absolute()
                .inset_0(),
            );
            let target = self.state.clone();
            handle = handle.on_mouse_down(MouseButton::Left, move |event, window, cx| {
                let stop = if let Control::Stop(id) = control_copy {
                    Some(id)
                } else {
                    None
                };
                target.update(cx, |s, cx| {
                    s.pointer_start(surface, stop, event.position, window, cx)
                });
                window.prevent_default();
                cx.stop_propagation();
            });
            rail = rail.child(self.range(
                handle,
                RangeDescription {
                    control,
                    value: numeric,
                    max,
                    label,
                },
                window,
                cx,
            ));
        }
        rail.style().refine(&self.styles.slider);
        rail
    }

    fn range(
        &self,
        element: gpui::Stateful<gpui::Div>,
        description: RangeDescription,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let RangeDescription {
            control,
            value,
            max,
            label,
        } = description;
        let focus = self
            .state
            .update(cx, |s, cx| s.control_focus(control.clone(), window, cx));
        let mut element = keyboard(
            element
                .track_focus(&focus)
                .role(Role::Slider)
                .aria_numeric_value(value)
                .aria_min_numeric_value(0.)
                .aria_max_numeric_value(max)
                .aria_numeric_value_step(1.),
            self.state.clone(),
            control.clone(),
        );
        let target = self.state.clone();
        let c = control.clone();
        element = element.on_a11y_action(accesskit::Action::SetValue, move |data, window, cx| {
            if let Some(accesskit::ActionData::NumericValue(value)) = data {
                target.update(cx, |s, cx| {
                    s.accessible_value(c.clone(), *value, window, cx)
                });
            }
        });
        for (action, delta) in [
            (accesskit::Action::Increment, 1.),
            (accesskit::Action::Decrement, -1.),
        ] {
            let target = self.state.clone();
            let control = control.clone();
            element = element.on_a11y_action(action, move |_, window, cx| {
                target.update(cx, |s, cx| {
                    s.accessible_value(control.clone(), (value + delta).clamp(0., max), window, cx)
                })
            });
        }
        Accessibility {
            disabled: self.state.read(cx).disabled,
            label: Some(label),
            ..Accessibility::new(Role::Slider)
        }
        .apply(element)
        .into_any_element()
    }

    fn editors(&self, window: &mut Window, cx: &mut App) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let g = self.tokens.globals();
        let messages = &config.locale.color_picker;
        let cache = window.use_keyed_state(
            ("color-picker-editor-metrics", self.state.entity_id()),
            cx,
            |_, _| EditorMetrics::new(&config, self.tokens.clone()),
        );
        let (hex_tokens, number_tokens) = cache.update(cx, |cache, _| {
            if !Arc::ptr_eq(&cache.theme, &config.theme)
                || !Arc::ptr_eq(&cache.tokens, &self.tokens)
            {
                *cache = EditorMetrics::new(&config, self.tokens.clone());
            }
            (cache.hex.clone(), cache.numeric.clone())
        });
        let state = self.state.read(cx);
        let format = state.format;
        let editor_height = self.tokens.editor_height(format) as f32;
        let disabled = state.disabled;
        let format_text = match format {
            ColorPickerFormat::Hex => "HEX",
            ColorPickerFormat::Rgb => "RGB",
            ColorPickerFormat::Hsb => "HSB",
        };
        let mut row = div().flex().items_center().w_full().h(px(editor_height));
        if config.direction == Direction::Rtl {
            row = row.flex_row_reverse();
        }
        let font_size = g.font_size_sm as f32;
        let format_text_width = window
            .text_system()
            .shape_line(
                format_text.into(),
                px(font_size),
                &[gpui::TextRun {
                    len: format_text.len(),
                    font: config.font.clone(),
                    color: g.color_text.to_hsla(),
                    background_color: None,
                    underline: None,
                    strikethrough: None,
                }],
                None,
            )
            .width;
        let format_width = format_text_width + px((g.margin_xxs + g.font_size_icon) as f32);
        let selector = Select::new("format", &state.format_select)
            .scope(self.scope.clone())
            .size(Size::Small)
            .variant(Variant::Borderless)
            .disabled(disabled || state.disabled_format)
            .accessibility_label(messages.format.clone())
            .popup_width(crate::popup::PopupWidth::Fixed(px(70.)))
            .label_render(move |_, _, _| {
                div()
                    .text_size(px(font_size))
                    .text_left()
                    .child(intrinsic_text(format_text.into()))
            })
            .styles(crate::select::SelectStyles {
                root: div()
                    .w(format_width)
                    .h(px(editor_height))
                    .border_0()
                    .p_0()
                    .text_size(px(g.font_size_sm as f32))
                    .line_height(px(editor_height))
                    .style()
                    .clone(),
                content: div()
                    .p_0()
                    .mx_0()
                    .flex_none()
                    .w(format_text_width)
                    .h(px(editor_height))
                    .style()
                    .clone(),
                suffix: div()
                    .mx_0()
                    .when(config.direction == Direction::Ltr, |d| {
                        d.ml(px(g.margin_xxs as f32))
                    })
                    .when(config.direction == Direction::Rtl, |d| {
                        d.mr(px(g.margin_xxs as f32))
                    })
                    .size(px(g.font_size_icon as f32))
                    .style()
                    .clone(),
                option_content: div().text_size(px(g.font_size_sm as f32)).style().clone(),
                ..Default::default()
            });
        if !state.disabled_format {
            row = row.child(
                div()
                    .flex_none()
                    .when(config.direction == Direction::Ltr, |d| {
                        d.mr(px(g.margin_xs as f32))
                    })
                    .when(config.direction == Direction::Rtl, |d| {
                        d.ml(px(g.margin_xs as f32))
                    })
                    .child(selector),
            );
        }
        let mut inputs = div()
            .flex()
            .flex_1()
            .min_w(px(0.))
            .gap(px(g.margin_xxs as f32));
        if config.direction == Direction::Rtl {
            inputs = inputs.flex_row_reverse();
        }
        if format == ColorPickerFormat::Hex {
            let mut hex = Input::new("hex", &state.hex)
                .scope(self.scope.clone())
                .size(Size::Small)
                .disabled(disabled)
                .prefix(div().text_color(g.color_text_placeholder).child("#"))
                .accessibility_label(messages.hex.clone())
                .text_size(px(g.font_size as f32))
                .h(px(editor_height))
                .flex_1()
                .min_w(px(0.));
            hex.tokens = Some(hex_tokens);
            hex.style().refine(&self.styles.input);
            inputs = inputs.child(self.editor_measure(4, hex));
        } else {
            for index in 0..3 {
                let label = if format == ColorPickerFormat::Hsb {
                    [
                        messages.hue.clone(),
                        messages.saturation.clone(),
                        messages.brightness.clone(),
                    ][index]
                        .clone()
                } else {
                    [
                        messages.red.clone(),
                        messages.green.clone(),
                        messages.blue.clone(),
                    ][index]
                        .clone()
                };
                let mut channel = InputNumber::new(("channel", index), &state.channels[index], cx)
                    .with_tokens(number_tokens.clone())
                    .scope(self.scope.clone())
                    .size(Size::Small)
                    .disabled(disabled)
                    .accessibility_label(label)
                    .text_size(px(g.font_size_sm as f32))
                    .h(px(editor_height))
                    .flex_1()
                    .min_w(px(0.));
                channel.style().refine(&self.styles.input);
                inputs = inputs.child(self.editor_measure(index, channel));
            }
        }
        row = row.child(inputs);
        if !state.disabled_alpha {
            let mut alpha = InputNumber::new("alpha-editor", &state.channels[3], cx)
                .with_tokens(number_tokens)
                .scope(self.scope.clone())
                .size(Size::Small)
                .disabled(disabled)
                .accessibility_label(messages.alpha.clone())
                .text_size(px(g.font_size_sm as f32))
                .w(px(self.tokens.alpha_input_width as f32))
                .h(px(editor_height));
            alpha.style().refine(&self.styles.input);
            row = row.child(
                div()
                    .flex_none()
                    .when(config.direction == Direction::Ltr, |d| {
                        d.ml(px(g.margin_xxs as f32))
                    })
                    .when(config.direction == Direction::Rtl, |d| {
                        d.mr(px(g.margin_xxs as f32))
                    })
                    .child(self.editor_measure(3, alpha)),
            );
        }
        let _ = window;
        row.into_any_element()
    }
    fn editor_measure(&self, index: usize, element: impl IntoElement) -> AnyElement {
        let state = self.state.clone();
        div()
            .relative()
            .flex()
            .when(index != 3, |d| d.flex_1().min_w(px(0.)))
            .child(element)
            .child(
                canvas(
                    move |bounds, _, cx| {
                        state.update(cx, |s, _| {
                            let slot = match index {
                                4 => 0,
                                3 if s.format == ColorPickerFormat::Hex => 1,
                                _ => index,
                            };
                            if s.bounds.editors.len() <= slot {
                                s.bounds.editors.resize(slot + 1, bounds);
                            }
                            s.bounds.editors[slot] = bounds;
                        })
                    },
                    |_, (), _, _| (),
                )
                .absolute()
                .inset_0(),
            )
            .into_any_element()
    }

    pub fn presets(&self, window: &mut Window, cx: &mut App) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let g = self.tokens.globals();
        let messages = &config.locale.color_picker;
        let mut root = div().flex().flex_col().gap(px(g.margin_xxs as f32));
        let mut preset_slot = 0;
        if let Some(groups) = &self.presets {
            for group in groups.iter() {
                let open = self.state.update(cx, |s, _| {
                    *s.groups
                        .entry(group.key.clone())
                        .or_insert(group.default_open)
                });
                let target = self.state.clone();
                let key = group.key.clone();
                let action = Rc::new(move |_: &mut Window, cx: &mut App| {
                    target.update(cx, |s, cx| {
                        let value = s.groups.entry(key.clone()).or_insert(true);
                        *value = !*value;
                        cx.notify();
                    })
                });
                let mut label = button(
                    self,
                    Control::Group(group.key.clone()),
                    group.label.clone(),
                    action,
                    window,
                    cx,
                )
                .flex()
                .items_center()
                .text_size(px(g.font_size_sm as f32))
                .line_height(relative(g.line_height_sm as f32))
                .aria_expanded(open);
                if config.direction == Direction::Rtl {
                    label = label.flex_row_reverse();
                }
                let mut expand_icon = div()
                    .w(px(g.font_size_icon as f32))
                    .flex_none()
                    .text_color(g.color_text_quaternary)
                    .child(
                        icon(if open {
                            IconName::DownOutlined
                        } else {
                            IconName::RightOutlined
                        })
                        .inherit_color()
                        .mirror(config.direction == Direction::Rtl && !open)
                        .size(px(g.font_size_icon as f32)),
                    );
                expand_icon = if config.direction == Direction::Rtl {
                    expand_icon.ml(px((g.padding_xxs + g.margin_sm) as f32))
                } else {
                    expand_icon.mr(px((g.padding_xxs + g.margin_sm) as f32))
                };
                label = label.child(expand_icon);
                label = label.child(if let Some(render) = &group.label_render {
                    render(window, cx)
                } else {
                    div().child(group.label.clone()).into_any_element()
                });
                let mut section = div().child(
                    Accessibility {
                        disabled: self.state.read(cx).disabled,
                        ..Accessibility::new(Role::Button)
                    }
                    .apply(label),
                );
                if open {
                    let mut cells = div()
                        .flex()
                        .flex_wrap()
                        .gap(px((g.margin_xxs * 1.5) as f32))
                        .py(px(g.padding_xs as f32));
                    if config.direction == Direction::Rtl {
                        cells = cells.flex_row_reverse();
                    }
                    if group.colors.is_empty() {
                        cells = cells.child(
                            div()
                                .text_size(px(g.font_size_sm as f32))
                                .text_color(g.color_text_quaternary)
                                .child(messages.preset_empty.clone()),
                        );
                    }
                    for (index, value) in group.colors.iter().enumerate() {
                        let target = self.state.clone();
                        let next = value.clone();
                        let action = Rc::new(move |window: &mut Window, cx: &mut App| {
                            target.update(cx, |s, cx| s.choose_preset(next.clone(), window, cx))
                        });
                        let selected = self.state.read(cx).accepted.visual_eq(value);
                        let mut cell = button(
                            self,
                            Control::Preset(group.key.clone(), index),
                            messages
                                .preset_color
                                .replace("{color}", &value.to_css_string())
                                .into(),
                            action,
                            window,
                            cx,
                        )
                        .relative()
                        .size(px(self.tokens.preset_color_size as f32))
                        .rounded(px(g.border_radius_sm as f32))
                        .child(swatch(
                            &self.state,
                            value.clone(),
                            self.tokens.preset_color_size,
                            g.border_radius_sm,
                            &self.tokens,
                            false,
                        ));
                        if selected {
                            let check_color =
                                if value.active_color(0).is_bright_on(g.color_bg_elevated) {
                                    crate::theme::Color::BLACK.with_alpha(0.45)
                                } else {
                                    crate::theme::Color::WHITE
                                };
                            let line = g.line_width_bold as f32;
                            cell = cell.child(
                                canvas(
                                    |_, _, _| (),
                                    move |bounds, (), window, _| {
                                        paint_preset_check(bounds, line, check_color, window)
                                    },
                                )
                                .absolute()
                                .inset_0(),
                            );
                        }
                        let state = self.state.clone();
                        let slot = preset_slot;
                        preset_slot += 1;
                        cell = cell.child(
                            canvas(
                                move |bounds, _, cx| {
                                    state.update(cx, |s, _| {
                                        if s.bounds.presets.len() <= slot {
                                            s.bounds.presets.resize(slot + 1, bounds);
                                        }
                                        s.bounds.presets[slot] = bounds;
                                    })
                                },
                                |_, (), _, _| (),
                            )
                            .absolute()
                            .inset_0(),
                        );
                        cells = cells.child(
                            Accessibility {
                                disabled: self.state.read(cx).disabled,
                                ..Accessibility::new(Role::Button)
                            }
                            .apply(cell),
                        );
                    }
                    section = section.child(cells);
                }
                root = root.child(section);
            }
        }
        root.style().refine(&self.styles.presets);
        root.into_any_element()
    }
}

fn paint_preset_check(
    bounds: gpui::Bounds<gpui::Pixels>,
    line: f32,
    color: crate::theme::Color,
    window: &mut Window,
) {
    let dimension = f32::from(bounds.size.width);
    let w = dimension / 13. * 5.;
    let h = dimension / 13. * 8.;
    let origin = point(
        bounds.left() + px(dimension * 0.215 + w / 2.),
        bounds.top() + bounds.size.height / 2. + px(h / 2.),
    );
    let transform = |x: f32, y: f32| {
        origin
            + point(
                px(((x - w) - (y - h)) * std::f32::consts::FRAC_1_SQRT_2),
                px(((x - w) + (y - h)) * std::f32::consts::FRAC_1_SQRT_2),
            )
    };
    // CSS paints the right and bottom edges of a transformed border box.
    // Filling that L keeps its square corners and avoids stroke-cap/join rules.
    let mut path = gpui::PathBuilder::fill();
    path.move_to(transform(0., h - line));
    path.line_to(transform(w - line, h - line));
    path.line_to(transform(w - line, 0.));
    path.line_to(transform(w, 0.));
    path.line_to(transform(w, h));
    path.line_to(transform(0., h));
    path.close();
    if let Ok(path) = path.build() {
        window.paint_path(path, color.to_hsla());
    }
}

fn record_handle(
    state: &mut ColorPickerState,
    kind: &'static str,
    index: usize,
    bounds: gpui::Bounds<gpui::Pixels>,
) {
    if let Some(existing) = state
        .bounds
        .handles
        .iter_mut()
        .find(|entry| entry.kind.as_ref() == kind && entry.index == index)
    {
        existing.bounds = bounds;
    } else {
        state.bounds.handles.push(super::ColorPickerHandleGeometry {
            kind: kind.into(),
            index,
            bounds,
        });
    }
}

struct EditorMetrics {
    theme: Arc<crate::theme::ResolvedTheme>,
    tokens: Arc<ColorPickerTokens>,
    hex: Arc<crate::input::InputTokens>,
    numeric: Arc<crate::input_number::InputNumberTokens>,
}
impl EditorMetrics {
    fn new(config: &crate::ResolvedConfig, tokens: Arc<ColorPickerTokens>) -> Self {
        let g = tokens.globals();
        let mut hex = (*config.theme.component::<crate::input::InputTheme>()).clone();
        hex.input_font_size_sm = g.font_size_sm;
        hex.padding_inline_sm = g.padding_xs;
        hex.padding_block_sm = 0.;
        let mut aliases = (*hex.globals).clone();
        aliases.line_height = (g.control_height_sm - 2. * g.line_width) / g.font_size_sm;
        hex.globals = Arc::new(aliases);
        Self {
            theme: config.theme.clone(),
            numeric: channel_tokens(config, &tokens),
            tokens,
            hex: Arc::new(hex),
        }
    }
}

fn channel_tokens(
    config: &crate::ResolvedConfig,
    tokens: &ColorPickerTokens,
) -> Arc<crate::input_number::InputNumberTokens> {
    let g = tokens.globals();
    let mut numeric = (*config
        .theme
        .component::<crate::input_number::InputNumberTheme>())
    .clone();
    numeric.input.input_font_size_sm = g.font_size_sm;
    numeric.input.padding_inline_sm = g.padding_xxs;
    numeric.input.padding_block_sm = 0.;
    numeric.handle_width = tokens.input_number_handle_width;
    let mut aliases = (*numeric.input.globals).clone();
    aliases.line_height = g.line_height_sm;
    numeric.input.globals = Arc::new(aliases);
    Arc::new(numeric)
}

fn keyboard(
    element: gpui::Stateful<gpui::Div>,
    state: Entity<ColorPickerState>,
    control: Control,
) -> gpui::Stateful<gpui::Div> {
    let end = state.clone();
    element
        .on_key_down(move |event, window, cx| {
            if !event.keystroke.modifiers.control
                && !event.keystroke.modifiers.alt
                && !event.keystroke.modifiers.platform
                && state.update(cx, |s, cx| {
                    s.key(
                        control.clone(),
                        &event.keystroke.key,
                        event.keystroke.modifiers.shift,
                        window,
                        cx,
                    )
                })
            {
                window.prevent_default();
                cx.stop_propagation();
            }
        })
        .on_key_up(move |event, window, cx| {
            end.update(cx, |s, cx| s.key_end(&event.keystroke.key, window, cx))
        })
}

fn button(
    parts: &ColorPickerPanelParts,
    control: Control,
    label: SharedString,
    action: ActionHandler,
    window: &mut Window,
    cx: &mut App,
) -> gpui::Stateful<gpui::Div> {
    let focus = parts
        .state
        .update(cx, |s, cx| s.control_focus(control.clone(), window, cx));
    let disabled = parts.state.read(cx).disabled;
    let click = action.clone();
    let down = action.clone();
    let invoke = action.clone();
    div()
        .id(format!("{control:?}"))
        .track_focus(&focus)
        .role(Role::Button)
        .aria_label(label)
        .cursor_pointer()
        .on_click(move |_, window, cx| {
            if !disabled {
                click(window, cx);
            }
        })
        .on_key_down(move |event, window, cx| {
            if !disabled && event.keystroke.key == "enter" {
                down(window, cx);
                window.prevent_default();
                cx.stop_propagation();
            }
        })
        .on_key_up(move |event, window, cx| {
            if !disabled && event.keystroke.key == "space" {
                action(window, cx);
                window.prevent_default();
                cx.stop_propagation();
            }
        })
        .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
            if !disabled {
                invoke(window, cx);
            }
        })
}

fn swatch(
    state: &Entity<ColorPickerState>,
    value: ColorPickerValue,
    dimension: f64,
    radius: f64,
    tokens: &ColorPickerTokens,
    preview: bool,
) -> impl IntoElement {
    let state = state.clone();
    let checker = tokens.globals().color_fill_secondary;
    let border = tokens.globals().color_split;
    let appearance = paint::SurfaceAppearance {
        radius: radius as f32,
        checker,
        inset: tokens.globals().color_text_quaternary,
    };
    let red = tokens
        .globals()
        .palettes
        .get(crate::theme::PresetColor::Red)
        .tones[5];
    div()
        .relative()
        .flex_none()
        .size(px(dimension as f32))
        .rounded(px(radius as f32))
        .child(
            canvas(
                move |bounds, _, cx| {
                    if preview {
                        state.update(cx, |s, _| s.bounds.preview = Some(bounds));
                    }
                },
                move |bounds, (), window, _| {
                    if value.is_cleared() {
                        paint::paint_clear(bounds, radius as f32, border, red, window);
                    } else {
                        paint::paint_color(bounds, &value, appearance, window);
                    }
                },
            )
            .absolute()
            .inset_0(),
        )
}

#[allow(clippy::too_many_arguments)]
fn trigger_swatch(
    state: &Entity<ColorPickerState>,
    value: ColorPickerValue,
    dimension: f64,
    radius: f64,
    tokens: &ColorPickerTokens,
    body_style: &StyleRefinement,
    content_style: &StyleRefinement,
    direction: Direction,
    window: &Window,
) -> AnyElement {
    if *body_style == StyleRefinement::default() && *content_style == StyleRefinement::default() {
        return swatch(state, value, dimension, radius, tokens, false).into_any_element();
    }
    let checker = tokens.globals().color_fill_secondary;
    let inset = tokens.globals().color_text_quaternary;
    let default_body_inset = body_style.box_shadow.is_none();
    let mut body = div()
        .relative()
        .flex_none()
        .size(px(dimension as f32))
        .rounded(px(radius as f32));
    if direction == Direction::Rtl {
        // A narrower block starts at the logical inline edge in CSS. GPUI's
        // block flow is physical, so align this single child explicitly.
        body = body.flex().flex_col().items_end();
    }
    body.style().refine(body_style);
    let mut body_native = gpui::Style::default();
    body_native.refine(body.style());
    let body_radii = body_native.corner_radii.to_pixels(window.rem_size());
    let borders = body_native.border_widths.to_pixels(window.rem_size());
    let inner_radius =
        |radius: gpui::Pixels, a: gpui::Pixels, b: gpui::Pixels| (radius - a.max(b)).max(px(0.));
    let body_inner_radii = gpui::Corners {
        top_left: inner_radius(body_radii.top_left, borders.top, borders.left),
        top_right: inner_radius(body_radii.top_right, borders.top, borders.right),
        bottom_left: inner_radius(body_radii.bottom_left, borders.bottom, borders.left),
        bottom_right: inner_radius(body_radii.bottom_right, borders.bottom, borders.right),
    };
    if value.is_cleared() {
        let border = tokens.globals().color_split;
        let red = tokens
            .globals()
            .palettes
            .get(crate::theme::PresetColor::Red)
            .tones[5];
        return body
            .child(
                canvas(
                    |_, _, _| (),
                    move |bounds, (), window, _| {
                        paint::paint_clear(
                            bounds,
                            f32::from(body_radii.top_left),
                            border,
                            red,
                            window,
                        );
                    },
                )
                .absolute()
                .inset_0(),
            )
            .into_any_element();
    }
    if body_native
        .background
        .as_ref()
        .and_then(gpui::Fill::color)
        .is_none_or(|background| background.as_solid().is_some())
    {
        body = body.child(
            canvas(
                |_, _, _| (),
                move |bounds, (), window, _| {
                    window.paint_quad(
                        gpui::fill(
                            bounds,
                            gpui::checkerboard(
                                checker,
                                f32::from(bounds.size.width) / 4. * window.scale_factor(),
                            ),
                        )
                        .corner_radii(body_inner_radii),
                    );
                    if default_body_inset {
                        paint::paint_surface_inset_corners(bounds, body_inner_radii, inset, window);
                    }
                },
            )
            .absolute()
            .inset_0(),
        );
    } else if default_body_inset {
        body = body.child(
            canvas(
                |_, _, _| (),
                move |bounds, (), window, _| {
                    paint::paint_surface_inset_corners(bounds, body_inner_radii, inset, window)
                },
            )
            .absolute()
            .inset_0(),
        );
    }
    let mut content = div().relative().w_full().h_full();
    content.style().corner_radii = body.style().corner_radii.clone();
    if let ColorPickerValue::Single(color) = &value {
        content = content.bg(color.to_rgba());
    }
    content.style().refine(content_style);
    let mut content_native = gpui::Style::default();
    content_native.refine(content.style());
    let content_radii = content_native.corner_radii.to_pixels(window.rem_size());
    if value.is_gradient() && content_style.background.is_none() {
        content = content.child(
            canvas(
                |_, _, _| (),
                move |bounds, (), window, _| {
                    paint::paint_color(
                        bounds,
                        &value,
                        paint::SurfaceAppearance {
                            radius: f32::from(content_radii.top_left),
                            checker: crate::theme::Color::TRANSPARENT,
                            inset: crate::theme::Color::TRANSPARENT,
                        },
                        window,
                    );
                },
            )
            .absolute()
            .inset_0(),
        );
    }
    if content_style.box_shadow.is_none() {
        content = content.child(
            canvas(
                |_, _, _| (),
                move |bounds, (), window, _| {
                    window.paint_quad(
                        gpui::outline(bounds, checker, Default::default())
                            .corner_radii(content_radii),
                    );
                },
            )
            .absolute()
            .inset_0(),
        );
    }
    body.child(content).into_any_element()
}

#[cfg(test)]
#[path = "semantic_tests.rs"]
mod semantic_tests;

#[cfg(test)]
#[path = "motion_tests.rs"]
mod motion_tests;
