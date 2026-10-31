// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native single-selection controls with stable option identity and a sliding surface.
//!
//! Build with stable, locally unique IDs during render. Values can be any application
//! type implementing `Clone + PartialEq`. `value` is controlled; `default_value`
//! initializes retained selection once. Applications localize their own labels.

mod measure;
mod style;

pub use style::{
    SegmentedMetrics, SegmentedShape, SegmentedStateStyle, SegmentedStyle, SegmentedStyleOptions,
    SegmentedTheme, SegmentedTokenPatch, SegmentedTokens,
};

use std::{cell::Cell, collections::HashMap, rc::Rc, sync::Arc, time::Duration};

use gpui::{
    AnyElement, App, Bounds, Context, ElementId, FocusHandle, IntoElement, KeyboardButton,
    MouseButton, Pixels, Refineable, Render, RenderOnce, Role, SharedString, StyleRefinement,
    Styled, Subscription, Toggled, Window, accesskit, canvas, div, prelude::*, px,
};

use crate::{
    ConfigScope, Direction, Size,
    behavior::{
        Accessibility, Activation, ActivationGuard, FocusRing, MotionPolicy, ScalarTransition,
    },
    icons::{IconPalette, IconRef, IconRenderOptions, IconRenderer},
    theme::{Color, CubicBezier},
};

type ChangeHandler<T> = Rc<dyn Fn(&T, &mut Window, &mut App)>;
type MeasuredBounds = Rc<Cell<Option<Bounds<Pixels>>>>;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SegmentedOrientation {
    #[default]
    Horizontal,
    Vertical,
}

/// An option's ID is independent of its value, label, and current list position.
pub struct SegmentedOption<T> {
    id: ElementId,
    value: T,
    label: SharedString,
    content: Option<AnyElement>,
    icon: Option<IconRef>,
    disabled: bool,
    focus: Option<FocusHandle>,
    accessibility: Accessibility,
    tooltip: Option<SharedString>,
    style: Option<Box<StyleRefinement>>,
    label_style: Option<Box<StyleRefinement>>,
    icon_style: Option<Box<StyleRefinement>>,
}

impl<T> SegmentedOption<T> {
    pub fn new(id: impl Into<ElementId>, value: T, label: impl Into<SharedString>) -> Self {
        Self {
            id: id.into(),
            value,
            label: label.into(),
            content: None,
            icon: None,
            disabled: false,
            focus: None,
            accessibility: Accessibility::new(Role::RadioButton),
            tooltip: None,
            style: None,
            label_style: None,
            icon_style: None,
        }
    }

    /// Replace the visual label while retaining its text as the accessible fallback.
    pub fn child(mut self, content: impl IntoElement) -> Self {
        self.content = Some(content.into_any_element());
        self
    }

    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self
    }

    /// Group/provider disabling always takes precedence over an option.
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = disabled;
        self
    }

    /// Handles participate in the group's roving tab order, including external handles.
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

    pub fn tooltip(mut self, tooltip: impl Into<SharedString>) -> Self {
        self.tooltip = Some(tooltip.into());
        self
    }

    pub fn style(mut self, style: StyleRefinement) -> Self {
        self.style = Some(Box::new(style));
        self
    }
    pub fn label_style(mut self, style: StyleRefinement) -> Self {
        self.label_style = Some(Box::new(style));
        self
    }
    pub fn icon_style(mut self, style: StyleRefinement) -> Self {
        self.icon_style = Some(Box::new(style));
        self
    }
}

/// A radio group with controlled or retained selection and one keyboard tab stop.
#[derive(IntoElement)]
pub struct Segmented<T: Clone + PartialEq + 'static> {
    id: ElementId,
    scope: Option<ConfigScope>,
    options: Vec<SegmentedOption<T>>,
    value: Option<T>,
    default_value: Option<T>,
    disabled: Option<bool>,
    size: Option<Size>,
    shape: SegmentedShape,
    orientation: Option<SegmentedOrientation>,
    vertical: bool,
    block: bool,
    accessibility: Accessibility,
    on_change: Option<ChangeHandler<T>>,
    style: StyleRefinement,
    item_style: Option<Box<StyleRefinement>>,
    label_style: Option<Box<StyleRefinement>>,
    icon_style: Option<Box<StyleRefinement>>,
}

impl<T: Clone + PartialEq + 'static> Segmented<T> {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            options: Vec::new(),
            value: None,
            default_value: None,
            disabled: None,
            size: None,
            shape: SegmentedShape::Default,
            orientation: None,
            vertical: false,
            block: false,
            accessibility: Accessibility::new(Role::RadioGroup),
            on_change: None,
            style: StyleRefinement::default(),
            item_style: None,
            label_style: None,
            icon_style: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn options(mut self, options: impl IntoIterator<Item = SegmentedOption<T>>) -> Self {
        self.options = options.into_iter().collect();
        self
    }
    pub fn value(mut self, value: T) -> Self {
        self.value = Some(value);
        self
    }
    pub fn default_value(mut self, value: T) -> Self {
        self.default_value = Some(value);
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }
    pub fn shape(mut self, shape: SegmentedShape) -> Self {
        self.shape = shape;
        self
    }
    /// Explicit orientation takes priority over `vertical`, independent of setter order.
    pub fn orientation(mut self, orientation: SegmentedOrientation) -> Self {
        self.orientation = Some(orientation);
        self
    }
    pub fn vertical(mut self, vertical: bool) -> Self {
        self.vertical = vertical;
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
    pub fn on_change(mut self, handler: impl Fn(&T, &mut Window, &mut App) + 'static) -> Self {
        self.on_change = Some(Rc::new(handler));
        self
    }
    pub fn item_style(mut self, style: StyleRefinement) -> Self {
        self.item_style = Some(Box::new(style));
        self
    }
    pub fn label_style(mut self, style: StyleRefinement) -> Self {
        self.label_style = Some(Box::new(style));
        self
    }
    pub fn icon_style(mut self, style: StyleRefinement) -> Self {
        self.icon_style = Some(Box::new(style));
        self
    }
}

impl<T: Clone + PartialEq + 'static> Styled for Segmented<T> {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct ItemState<T> {
    value: T,
    focus: FocusHandle,
    guard: ActivationGuard,
    disabled: bool,
    hovered: bool,
    pressed: bool,
    bounds: MeasuredBounds,
    colors: ColorTransition,
    surface: SurfaceStyle,
    generation: u64,
    _blur: Subscription,
}

struct SegmentedState<T> {
    scope: ConfigScope,
    selected: Option<T>,
    controlled: bool,
    disabled: Option<bool>,
    items: HashMap<ElementId, ItemState<T>>,
    order: Vec<ElementId>,
    generation: u64,
    thumb: ThumbTransition,
    frame_pending: bool,
    clock: Box<dyn Fn(&App) -> Duration>,
    _config: Subscription,
}

impl<T: Clone + PartialEq + 'static> SegmentedState<T> {
    fn new(
        control: &Segmented<T>,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let config = Self::observe_scope(&scope, window, cx);
        let epoch = cx.background_executor().now();
        Self {
            scope,
            selected: control
                .value
                .clone()
                .or_else(|| control.default_value.clone())
                .or_else(|| control.options.first().map(|o| o.value.clone())),
            controlled: control.value.is_some(),
            disabled: control.disabled,
            items: HashMap::new(),
            order: Vec::new(),
            generation: 0,
            thumb: ThumbTransition::default(),
            frame_pending: false,
            clock: Box::new(move |cx| cx.background_executor().now().duration_since(epoch)),
            _config: config,
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

    fn observe_item_blur(
        id: ElementId,
        focus: &FocusHandle,
        guard: &ActivationGuard,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        let weak = cx.weak_entity();
        let guard = guard.clone();
        window.on_focus_out(focus, cx, move |_, _, cx| {
            guard.cancel();
            let _ = weak.update(cx, |state, cx| {
                if let Some(item) = state.items.get_mut(&id)
                    && item.pressed
                {
                    item.pressed = false;
                    cx.notify();
                }
            });
        })
    }

    fn sync_availability(&mut self, window: &mut Window, cx: &mut App) {
        let disabled = self
            .scope
            .snapshot(cx)
            .availability(self.disabled, false)
            .disabled;
        for item in self.items.values_mut() {
            item.guard.update(
                crate::behavior::Availability {
                    disabled: disabled || item.disabled,
                    busy: false,
                },
                &item.focus,
                window,
                cx,
            );
            if !item.guard.availability().can_activate() {
                item.pressed = false;
            }
        }
        self.sync_tab_stops();
    }

    fn sync_tab_stops(&self) {
        let enabled = |id: &&ElementId| self.items[*id].guard.availability().can_focus();
        let selected = self
            .order
            .iter()
            .filter(enabled)
            .find(|id| self.selected.as_ref() == Some(&self.items[*id].value))
            .or_else(|| self.order.iter().find(enabled));
        for (id, item) in &self.items {
            item.focus.clone().tab_stop(selected == Some(id));
        }
    }

    fn update(
        &mut self,
        control: &Segmented<T>,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope != scope {
            for item in self.items.values() {
                item.guard.cancel();
            }
            self._config = Self::observe_scope(&scope, window, cx);
            self.scope = scope;
        }
        if let Some(value) = &control.value {
            self.selected = Some(value.clone());
        }
        self.controlled = control.value.is_some();
        self.disabled = control.disabled;
        self.order.clear();
        self.generation = self.generation.wrapping_add(1);
        for option in &control.options {
            self.order.push(option.id.clone());
            let item = self.items.entry(option.id.clone()).or_insert_with(|| {
                let focus = option
                    .focus
                    .clone()
                    .unwrap_or_else(|| cx.focus_handle().tab_index(0));
                let guard = ActivationGuard::default();
                let blur = Self::observe_item_blur(option.id.clone(), &focus, &guard, window, cx);
                ItemState {
                    value: option.value.clone(),
                    focus,
                    guard,
                    disabled: option.disabled,
                    hovered: false,
                    pressed: false,
                    bounds: Rc::default(),
                    colors: ColorTransition::default(),
                    surface: SurfaceStyle::default(),
                    generation: self.generation.wrapping_sub(1),
                    _blur: blur,
                }
            });
            // One bounded map entry per mounted ID; no quadratic list scans on rerender.
            assert_ne!(
                item.generation, self.generation,
                "Segmented option IDs must be unique"
            );
            item.generation = self.generation;
            if let Some(focus) = &option.focus
                && item.focus != *focus
            {
                item.guard.cancel();
                item._blur =
                    Self::observe_item_blur(option.id.clone(), focus, &item.guard, window, cx);
                item.focus = focus.clone();
            }
            if item.value != option.value {
                item.guard.cancel();
            }
            item.value = option.value.clone();
            item.disabled = option.disabled;
        }
        self.items
            .retain(|_, item| item.generation == self.generation);
        self.sync_availability(window, cx);
    }

    fn choose(&mut self, id: &ElementId, window: &mut Window, cx: &mut Context<Self>) -> Option<T> {
        let item = self.items.get(id)?;
        // Availability is read live even if the current element tree predates a config update.
        if !item.guard.availability().can_activate() {
            return None;
        }
        item.focus.focus(window, cx);
        if self.selected.as_ref() == Some(&item.value) {
            return None;
        }
        let value = item.value.clone();
        if !self.controlled {
            self.selected = Some(value.clone());
            self.sync_tab_stops();
        }
        cx.notify();
        Some(value)
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
}

impl<T: Clone + PartialEq + 'static> RenderOnce for Segmented<T> {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            SegmentedState::new(&self, scope.clone(), window, cx)
        });
        state.update(cx, |state, cx| {
            state.update(&self, scope.clone(), window, cx)
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<SegmentedTheme>();
        let global = tokens.globals();
        let disabled = config.availability(self.disabled, false).disabled;
        let style = SegmentedStyle::from_tokens(
            &tokens,
            SegmentedStyleOptions {
                size: self.size.unwrap_or(config.size),
                shape: self.shape,
                disabled,
            },
        );
        let metrics = style.metrics;
        let orientation = self.orientation.unwrap_or(if self.vertical {
            SegmentedOrientation::Vertical
        } else {
            SegmentedOrientation::Horizontal
        });
        let vertical = orientation == SegmentedOrientation::Vertical;
        let rtl = config.direction == Direction::Rtl;
        let motion = MotionPolicy::new(config.motion && global.motion, cx.reduce_motion());
        let duration = global.motion_duration_mid;
        let thumb_duration = global.motion_duration_slow;
        let curve = global.motion_ease_in_out;
        let thumb_moving = state.read_with(cx, |state, cx| {
            let selected_id = state
                .order
                .iter()
                .find(|id| state.selected.as_ref() == Some(&state.items[*id].value));
            motion.allows_motion()
                && !thumb_duration.is_zero()
                && selected_id.is_some()
                && state.thumb.id.is_some()
                && (selected_id != state.thumb.id.as_ref()
                    || state
                        .thumb
                        .progress
                        .is_some_and(|p| !p.sample((state.clock)(cx)).finished))
        });
        let mut root = div()
            .id(self.id)
            .relative()
            .flex()
            .flex_shrink_0()
            .items_stretch()
            .p(px(metrics.track_padding as f32))
            .rounded(px(metrics.track_radius as f32))
            .bg(style.track_background)
            .font(tokens.font().clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(gpui::relative(global.line_height as f32))
            .text_color(style.rest.text)
            .aria_size_of_set(self.options.len())
            .aria_orientation(if vertical {
                accesskit::Orientation::Vertical
            } else {
                accesskit::Orientation::Horizontal
            })
            .when(vertical, |root| root.flex_col())
            .when(!vertical && rtl, |root| root.flex_row_reverse())
            .when(self.block, |root| root.w_full());
        root.style().refine(&self.style);

        let weak = state.downgrade();
        root = root.child(
            div().absolute().top_0().left_0().size_full().child(
                canvas(
                    |_, _, _| (),
                    move |bounds, (), window, cx| {
                        let _ = weak.update(cx, |state, cx| {
                            let selected = state.order.iter().find_map(|id| {
                                let item = &state.items[id];
                                (state.selected.as_ref() == Some(&item.value))
                                    .then(|| {
                                        item.bounds.get().map(|b| {
                                            (
                                                id.clone(),
                                                Bounds::new(b.origin - bounds.origin, b.size),
                                                item.surface.clone(),
                                            )
                                        })
                                    })
                                    .flatten()
                            });
                            let target = selected
                                .as_ref()
                                .map(|(id, bounds, _)| (id.clone(), *bounds));
                            let now = (state.clock)(cx);
                            let (thumb, animating) =
                                state
                                    .thumb
                                    .resolve(target, now, thumb_duration, curve, motion);
                            if let Some(mut thumb) = thumb {
                                thumb.origin += bounds.origin;
                                let surface = &selected.as_ref().expect("selected surface").2;
                                let corners = surface.corners.clamp_radii_for_quad_size(thumb.size);
                                if !surface.native_shadow {
                                    window.paint_drop_shadows(thumb, corners, &surface.shadows);
                                }
                                if !surface.native_background {
                                    window.paint_quad(
                                        gpui::fill(
                                            thumb,
                                            style
                                                .selected
                                                .background
                                                .to_hsla()
                                                .opacity(surface.opacity),
                                        )
                                        .corner_radii(corners),
                                    );
                                }
                                if !surface.native_shadow {
                                    window.paint_inset_shadows(thumb, corners, &surface.shadows);
                                }
                            }
                            if animating {
                                state.request_frame(window, cx);
                            }
                        });
                        // GPUI suppresses ordinary hover during mouse gestures. Track the
                        // physical pointer only while pressed, without any idle frame loop.
                        let tracked: Vec<_> = weak
                            .read_with(cx, |state, _| {
                                state
                                    .items
                                    .iter()
                                    .filter(|(_, item)| item.pressed)
                                    .filter_map(|(id, item)| {
                                        item.bounds.get().map(|bounds| {
                                            (
                                                id.clone(),
                                                bounds.intersect(&window.content_mask().bounds),
                                            )
                                        })
                                    })
                                    .collect()
                            })
                            .unwrap_or_default();
                        if !tracked.is_empty() {
                            let weak = weak.clone();
                            window.on_mouse_event(
                                move |event: &gpui::MouseMoveEvent, phase, _, cx| {
                                    if phase != gpui::DispatchPhase::Capture {
                                        return;
                                    }
                                    let _ = weak.update(cx, |state, cx| {
                                        let mut changed = false;
                                        for (id, bounds) in &tracked {
                                            if let Some(item) = state.items.get_mut(id) {
                                                let hovered = bounds.contains(&event.position);
                                                if item.hovered != hovered {
                                                    item.hovered = hovered;
                                                    changed = true;
                                                }
                                            }
                                        }
                                        if changed {
                                            cx.notify();
                                        }
                                    });
                                },
                            );
                        }
                    },
                )
                .size_full(),
            ),
        );

        let count = self.options.len();
        for (index, mut option) in self.options.into_iter().enumerate() {
            let (focus, guard, bounds, selected, colors) = state.update(cx, |state, cx| {
                let now = (state.clock)(cx);
                let selected = state.selected.as_ref() == Some(&option.value);
                let item = state.items.get_mut(&option.id).expect("mounted option");
                let disabled = item.guard.availability().disabled;
                let mut target = if disabled {
                    SegmentedStateStyle {
                        text: style.disabled_text,
                        background: Color::TRANSPARENT,
                    }
                } else if selected {
                    SegmentedStateStyle {
                        text: style.selected.text,
                        background: Color::TRANSPARENT,
                    }
                } else if item.pressed && item.hovered {
                    style.active
                } else if item.hovered {
                    style.hover
                } else {
                    style.rest
                };
                if thumb_moving && !selected {
                    target.background = Color::TRANSPARENT;
                }
                let (colors, animating) = item.colors.resolve(target, now, duration, motion);
                let result = (
                    item.focus.clone(),
                    item.guard.clone(),
                    item.bounds.clone(),
                    selected,
                    colors,
                );
                if animating {
                    state.request_frame(window, cx);
                }
                result
            });
            let option_disabled = guard.availability().disabled;
            let mut label = div()
                .flex()
                .items_center()
                .justify_center()
                .min_w_0()
                .min_h(px(metrics.item_height as f32))
                .px(px(metrics.padding_inline as f32))
                .line_height(px(metrics.line_height as f32))
                .gap(px(metrics.icon_gap as f32))
                .when(rtl, |label| label.flex_row_reverse());
            if let Some(icon_ref) = option.icon {
                let font_size = [
                    option.icon_style.as_deref(),
                    self.icon_style.as_deref(),
                    option.label_style.as_deref(),
                    self.label_style.as_deref(),
                    option.style.as_deref(),
                    self.item_style.as_deref(),
                    Some(&self.style),
                ]
                .into_iter()
                .flatten()
                .find_map(|style| style.text.font_size)
                .map_or(px(metrics.icon_size as f32), |size| {
                    size.to_pixels(window.rem_size())
                });
                let mut graphic = foreground_icon(
                    icon_ref,
                    IconRenderOptions {
                        size: gpui::size(font_size, font_size),
                        scale_factor: window.scale_factor(),
                        colors: IconPalette {
                            text: colors.text.to_rgba(),
                            ..tokens.icon_palette()
                        },
                        ..Default::default()
                    },
                );
                if let Some(style) = &self.icon_style {
                    graphic.style().refine(style);
                }
                if let Some(style) = &option.icon_style {
                    graphic.style().refine(style);
                }
                label = label.child(graphic);
            }
            if let Some(content) = option.content {
                label = label.child(content);
            } else if !option.label.is_empty() {
                label = label.child(div().min_w_0().truncate().child(option.label.clone()));
            }
            if let Some(style) = &self.label_style {
                label.style().refine(style);
            }
            if let Some(style) = &option.label_style {
                label.style().refine(style);
            }
            let mut item = div()
                .id(option.id.clone())
                .relative()
                .min_w_0()
                .rounded(px(metrics.item_radius as f32))
                .bg(colors.background)
                .text_color(colors.text)
                .aria_toggled(if selected {
                    Toggled::True
                } else {
                    Toggled::False
                })
                .aria_position_in_set(index)
                .aria_selected(selected)
                .aria_size_of_set(count)
                .when(!option_disabled, |item| item.track_focus(&focus))
                .when(self.block, |item| item.flex_1())
                .child(label);
            if let Some(style) = &self.item_style {
                item.style().refine(style);
            }
            if let Some(style) = &option.style {
                item.style().refine(style);
            }
            let corners = gpui::Corners {
                top_left: item.style().corner_radii.top_left.unwrap_or_default(),
                top_right: item.style().corner_radii.top_right.unwrap_or_default(),
                bottom_left: item.style().corner_radii.bottom_left.unwrap_or_default(),
                bottom_right: item.style().corner_radii.bottom_right.unwrap_or_default(),
            }
            .to_pixels(window.rem_size());
            let focus_opacity = item.style().opacity.unwrap_or(1.);
            state.update(cx, |state, _| {
                let surface = &mut state
                    .items
                    .get_mut(&option.id)
                    .expect("mounted option")
                    .surface;
                surface.update(corners, focus_opacity, tokens.shadows());
                surface.native_shadow = item.style().box_shadow.is_some();
                surface.native_background = option
                    .style
                    .as_deref()
                    .is_some_and(|style| style.background.is_some())
                    || self
                        .item_style
                        .as_deref()
                        .is_some_and(|style| style.background.is_some());
            });
            if item.style().mouse_cursor.is_none() {
                item = if option_disabled {
                    item.cursor_not_allowed()
                } else {
                    item.cursor_pointer()
                };
            }
            if let Some(text) = option.tooltip {
                let scope = scope.clone();
                item = item.tooltip(move |_, cx| {
                    cx.new(|_| SegmentedTooltip {
                        text: text.clone(),
                        scope: scope.clone(),
                    })
                    .into()
                });
            }
            let weak = state.downgrade();
            let id = option.id.clone();
            item = item.on_hover(move |hovered, _, cx| {
                let _ = weak.update(cx, |state, cx| {
                    if let Some(item) = state.items.get_mut(&id)
                        && item.hovered != *hovered
                    {
                        item.hovered = *hovered;
                        cx.notify();
                    }
                });
            });
            let weak = state.downgrade();
            let id = option.id.clone();
            item = item.on_mouse_down(MouseButton::Left, move |_, _, cx| {
                let _ = weak.update(cx, |state, cx| {
                    if let Some(item) = state.items.get_mut(&id)
                        && item.guard.availability().can_activate()
                    {
                        item.pressed = true;
                        item.hovered = true;
                        cx.notify();
                    }
                });
            });
            for outside in [false, true] {
                let weak = state.downgrade();
                let id = option.id.clone();
                let reset = move |_: &gpui::MouseUpEvent, _: &mut Window, cx: &mut App| {
                    let _ = weak.update(cx, |state, cx| {
                        if let Some(item) = state.items.get_mut(&id)
                            && item.pressed
                        {
                            item.pressed = false;
                            cx.notify();
                        }
                    });
                };
                item = if outside {
                    item.on_mouse_up_out(MouseButton::Left, reset)
                } else {
                    item.on_mouse_up(MouseButton::Left, reset)
                };
            }
            let weak = state.downgrade();
            let id = option.id.clone();
            let callback = self.on_change.clone();
            let item = guard.install(item, move |activation, window, cx| {
                if activation == Activation::Keyboard(KeyboardButton::Enter) {
                    return;
                }
                let change = weak.update(cx, |state, cx| state.choose(&id, window, cx));
                if let Ok(Some(value)) = change
                    && let Some(callback) = &callback
                {
                    callback(&value, window, cx);
                }
            });
            option.accessibility.disabled = option_disabled;
            if option.accessibility.label.is_none() {
                option.accessibility.label = Some(option.label);
            }
            let ring = FocusRing::new(
                style.focus_color.to_hsla().opacity(focus_opacity),
                px(if option_disabled {
                    0.
                } else {
                    style.focus_width as f32
                }),
            )
            .with_offset(px(style.focus_offset as f32))
            .radius(px(metrics.item_radius as f32));
            root = root.child(measure::measure(
                option
                    .accessibility
                    .apply(ring.apply(item, &focus).corners(corners)),
                bounds,
            ));
        }
        let weak = state.downgrade();
        let callback = self.on_change;
        root = root.on_key_down(move |event, window, cx| {
            if event.keystroke.modifiers.modified() {
                return;
            }
            let key = event.keystroke.key.as_str();
            let next = weak
                .update(cx, |state, _| {
                    let current = state
                        .order
                        .iter()
                        .position(|id| state.items[id].focus.is_focused(window))?;
                    let backwards = match key {
                        "left" => !rtl,
                        "right" => rtl,
                        "up" => true,
                        "down" => false,
                        "home" => false,
                        "end" => true,
                        _ => return None,
                    };
                    let count = state.order.len();
                    (0..count).find_map(|step| {
                        let index = match key {
                            "home" => step,
                            "end" => count - 1 - step,
                            _ if backwards => (current + count - 1 - step) % count,
                            _ => (current + 1 + step) % count,
                        };
                        let id = &state.order[index];
                        state.items[id]
                            .guard
                            .availability()
                            .can_activate()
                            .then(|| id.clone())
                    })
                })
                .ok()
                .flatten();
            if let Some(id) = next {
                cx.stop_propagation();
                window.prevent_default();
                if let Ok(Some(value)) = weak.update(cx, |state, cx| state.choose(&id, window, cx))
                    && let Some(callback) = &callback
                {
                    callback(&value, window, cx);
                }
            }
        });
        self.accessibility.disabled = disabled;
        self.accessibility.apply(root)
    }
}

/// The shared moving surface follows native item refinements as well as its geometry.
#[derive(Clone, Default)]
struct SurfaceStyle {
    corners: gpui::Corners<Pixels>,
    opacity: f32,
    source_shadows: Arc<[gpui::BoxShadow]>,
    shadows: Arc<[gpui::BoxShadow]>,
    native_background: bool,
    native_shadow: bool,
}

impl SurfaceStyle {
    fn update(
        &mut self,
        corners: gpui::Corners<Pixels>,
        opacity: f32,
        shadows: &Arc<[gpui::BoxShadow]>,
    ) {
        self.corners = corners;
        if self.opacity == opacity && Arc::ptr_eq(&self.source_shadows, shadows) {
            return;
        }
        self.opacity = opacity;
        self.source_shadows = shadows.clone();
        self.shadows = if opacity == 1. {
            shadows.clone()
        } else {
            shadows
                .iter()
                .map(|shadow| gpui::BoxShadow {
                    color: shadow.color.opacity(opacity),
                    ..*shadow
                })
                .collect()
        };
    }
}

#[derive(Default)]
struct ThumbTransition {
    id: Option<ElementId>,
    from: Option<Bounds<Pixels>>,
    target: Option<Bounds<Pixels>>,
    progress: Option<ScalarTransition>,
}

impl ThumbTransition {
    fn sample(&self, now: Duration) -> Option<Bounds<Pixels>> {
        let target = self.target?;
        let from = self.from.unwrap_or(target);
        let progress = self.progress.map_or(1., |p| p.sample(now).value) as f32;
        Some(Bounds::new(
            from.origin + (target.origin - from.origin) * progress,
            gpui::size(
                from.size.width + (target.size.width - from.size.width) * progress,
                from.size.height + (target.size.height - from.size.height) * progress,
            ),
        ))
    }

    fn resolve(
        &mut self,
        target: Option<(ElementId, Bounds<Pixels>)>,
        now: Duration,
        duration: Duration,
        curve: CubicBezier,
        motion: MotionPolicy,
    ) -> (Option<Bounds<Pixels>>, bool) {
        let Some((id, target)) = target else {
            *self = Self::default();
            return (None, false);
        };
        if self.id.as_ref() != Some(&id) || self.target != Some(target) {
            let animate = self.id.is_some() && self.id.as_ref() != Some(&id);
            self.from = if animate {
                self.sample(now)
            } else {
                Some(target)
            };
            self.id = Some(id);
            self.target = Some(target);
            let mut progress = ScalarTransition::new(0., curve);
            progress.retarget(
                1.,
                now,
                if animate { duration } else { Duration::ZERO },
                motion,
            );
            self.progress = Some(progress);
        }
        if !motion.allows_motion()
            && let Some(progress) = &mut self.progress
        {
            progress.retarget(1., now, Duration::ZERO, motion);
        }
        (
            self.sample(now),
            self.progress.is_some_and(|p| !p.sample(now).finished),
        )
    }
}

#[derive(Default)]
struct ColorTransition {
    from: Option<SegmentedStateStyle>,
    target: Option<SegmentedStateStyle>,
    progress: Option<ScalarTransition>,
}

impl ColorTransition {
    fn sample(&self, now: Duration) -> SegmentedStateStyle {
        let target = self.target.expect("initialized colors");
        let from = self.from.unwrap_or(target);
        let p = self.progress.map_or(1., |p| p.sample(now).value) as f32;
        SegmentedStateStyle {
            background: interpolate(from.background, target.background, p),
            text: interpolate(from.text, target.text, p),
        }
    }
    fn resolve(
        &mut self,
        target: SegmentedStateStyle,
        now: Duration,
        duration: Duration,
        motion: MotionPolicy,
    ) -> (SegmentedStateStyle, bool) {
        if self.target != Some(target) {
            self.from = Some(if self.target.is_some() {
                self.sample(now)
            } else {
                target
            });
            self.target = Some(target);
            let mut progress = ScalarTransition::new(0., CubicBezier::new(0.25, 0.1, 0.25, 1.));
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
        if !motion.allows_motion()
            && let Some(progress) = &mut self.progress
        {
            progress.retarget(1., now, Duration::ZERO, motion);
        }
        (
            self.sample(now),
            self.progress.is_some_and(|p| !p.sample(now).finished),
        )
    }
}

fn interpolate(from: Color, to: Color, p: f32) -> Color {
    if p >= 1. || from == to {
        return to;
    }
    if p <= 0. {
        return from;
    }
    let from = from.to_rgba();
    let to = to.to_rgba();
    let alpha = from.a + (to.a - from.a) * p;
    let channel = |f: f32, t: f32| {
        if alpha > f32::EPSILON {
            (f * from.a * (1. - p) + t * to.a * p) / alpha
        } else {
            0.
        }
    };
    Color {
        r: (channel(from.r, to.r) * 255.).round() as u8,
        g: (channel(from.g, to.g) * 255.).round() as u8,
        b: (channel(from.b, to.b) * 255.).round() as u8,
        a: f64::from(alpha),
    }
}

struct SegmentedTooltip {
    text: SharedString,
    scope: ConfigScope,
}

// Resolve currentColor during painting, after native semantic-slot refinements.
fn foreground_icon(reference: IconRef, options: IconRenderOptions) -> gpui::Canvas<()> {
    canvas(
        |_, _, _| (),
        move |bounds, (), window, cx| {
            if cx.try_global::<IconRenderer>().is_none() {
                cx.set_global(IconRenderer::default());
            }
            let options = IconRenderOptions {
                colors: IconPalette {
                    text: window.text_style().color.into(),
                    ..options.colors
                },
                ..options
            };
            cx.update_global::<IconRenderer, _>(|renderer, cx| {
                let _ = renderer.paint(bounds, &reference, &options, window, cx);
            });
        },
    )
    .w(options.size.width)
    .h(options.size.height)
    .flex_shrink_0()
}
impl Render for SegmentedTooltip {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let g = &config.theme.tokens;
        div()
            .px(px(g.padding_xs as f32))
            .py(px(g.padding_xxs as f32))
            .rounded(px(g.border_radius as f32))
            .bg(g.color_bg_spotlight)
            .text_color(g.color_text_light_solid)
            .font(g.font_family.to_gpui_font())
            .text_size(px(g.font_size as f32))
            .line_height(gpui::relative(g.line_height as f32))
            .child(self.text.clone())
    }
}

#[cfg(test)]
mod tests;

#[cfg(test)]
mod reference_tests;
