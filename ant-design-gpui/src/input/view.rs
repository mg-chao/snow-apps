// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{cell::RefCell, rc::Rc, sync::Arc, time::Duration};

use gpui::{
    AnyElement, App, AvailableSpace, Bounds, ContentMask, Context, DefiniteLength, Edges, Element,
    ElementId, ElementInputHandler, Entity, GlobalElementId, InspectorElementId, IntoElement,
    LayoutId, MouseButton, Pixels, Point, Refineable, RenderOnce, Role, SharedString, Style,
    StyleRefinement, Styled, Subscription, Task, Window, canvas, div, fill, point, prelude::*, px,
    relative, size,
};

use crate::{
    Button, ConfigScope, Direction, ScrollArea, ScrollAreaState, Size, ValidationStatus, Variant,
    behavior::{Accessibility, FocusRing, MotionPolicy},
    icons::IconName,
};

use super::{
    InputInteraction, InputState, InputStyle, InputTheme, SearchSource,
    accessibility::{GeometryCell, SelectionMapCell, TextSemantics},
    action_view::{ActionTheme, InputAction, input_action},
    controls::{CountConfig, CountInfo, SearchButton},
    layout::{LayoutCache, LayoutStyle, NavigationLayout, TextAlignment, TextLayout},
    motion::{CSS_EASE, LINEAR, SurfaceMotion},
    state::InputMode,
};

/// A single-line native input. Keep its state entity outside the render method.
#[derive(IntoElement)]
pub struct Input {
    id: ElementId,
    state: Entity<InputState>,
    scope: Option<ConfigScope>,
    size: Option<Size>,
    variant: Option<Variant>,
    status: ValidationStatus,
    disabled: Option<bool>,
    read_only: Option<bool>,
    placeholder: SharedString,
    accessibility: Accessibility,
    style: StyleRefinement,
    multiline: bool,
    rows: usize,
    auto_size: Option<(usize, usize)>,
    prefix: Option<AnyElement>,
    suffix: Option<AnyElement>,
    allow_clear: bool,
    count: Option<CountConfig>,
    pub(crate) mode: InputMode,
    pub(crate) visibility_toggle: bool,
    pub(crate) search_loading: bool,
    pub(crate) search_button: SearchButton,
    pub(crate) tokens: Option<Arc<super::InputTokens>>,
    pub(crate) frame_decorator: Option<FrameDecorator>,
    pub(crate) numeric: bool,
    pub(crate) value_error: bool,
    pub(crate) centered: bool,
    // Reuse the native editor in composite controls without a second control
    // surface. Text layout, IME geometry, selection and editing stay shared.
    pub(crate) embedded: bool,
    /// Embedding controls may use a tighter editing line box than standalone Input.
    pub(crate) embedded_line_height: Option<f64>,
    pub(crate) mask_glyph: SharedString,
    pub(crate) mask_style: Option<MaskStyle>,
    group: Option<super::InputGroupContext>,
}

pub(crate) struct MaskStyle {
    pub font: gpui::Font,
    pub font_size: f64,
    pub color: gpui::Hsla,
}

pub(crate) struct InputFrame {
    pub metrics: InputStyle,
    pub hovered: bool,
    pub focused: bool,
    pub disabled: bool,
    pub read_only: bool,
}
pub(crate) type FrameDecorator = Box<
    dyn FnOnce(
        gpui::Stateful<gpui::Div>,
        InputFrame,
        &mut Window,
        &mut App,
    ) -> gpui::Stateful<gpui::Div>,
>;

impl Input {
    pub fn new(id: impl Into<ElementId>, state: &Entity<InputState>) -> Self {
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
            accessibility: Accessibility::new(Role::TextInput),
            style: StyleRefinement::default(),
            multiline: false,
            rows: 1,
            auto_size: None,
            prefix: None,
            suffix: None,
            allow_clear: false,
            count: None,
            mode: InputMode::Text,
            visibility_toggle: true,
            search_loading: false,
            search_button: SearchButton::default(),
            tokens: None,
            frame_decorator: None,
            numeric: false,
            value_error: false,
            centered: false,
            embedded: false,
            embedded_line_height: None,
            mask_glyph: "•".into(),
            mask_style: None,
            group: None,
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
    pub fn placeholder(mut self, value: impl Into<SharedString>) -> Self {
        self.placeholder = value.into();
        self
    }
    pub fn accessibility_label(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(value.into());
        self
    }
    pub fn accessibility_description(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(value.into());
        self
    }
    pub fn automation_id(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(value.into());
        self
    }
    pub fn multiline(mut self, multiline: bool) -> Self {
        self.multiline = multiline;
        self
    }
    pub fn rows(mut self, rows: usize) -> Self {
        self.rows = rows.max(1);
        self
    }
    pub fn auto_size(mut self, min_rows: usize, max_rows: usize) -> Self {
        self.auto_size = Some((min_rows.max(1), max_rows.max(min_rows).max(1)));
        self
    }
    pub fn prefix(mut self, prefix: impl IntoElement) -> Self {
        self.prefix = Some(prefix.into_any_element());
        self
    }
    pub fn suffix(mut self, suffix: impl IntoElement) -> Self {
        self.suffix = Some(suffix.into_any_element());
        self
    }
    pub fn allow_clear(mut self, allow: bool) -> Self {
        self.allow_clear = allow;
        self
    }
    pub fn show_count(mut self, show: bool) -> Self {
        self.count = show.then(CountConfig::default);
        self
    }
    pub fn count(mut self, count: CountConfig) -> Self {
        self.count = Some(count);
        self
    }
    pub(crate) fn semantic_role(mut self, role: Role) -> Self {
        self.accessibility.role = Some(role);
        self
    }
    pub(crate) fn in_group(mut self, context: super::InputGroupContext) -> Self {
        self.scope = self.scope.or_else(|| Some(context.scope.clone()));
        self.size = self.size.or(Some(context.size));
        self.disabled = self.disabled.or(context.disabled);
        self.group = Some(context);
        self
    }
}

impl super::InputGroupItem for Input {
    fn into_group_element(self, context: super::InputGroupContext) -> AnyElement {
        self.in_group(context).into_any_element()
    }
}

impl Styled for Input {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct GeometryCache {
    layout: Rc<TextLayout>,
    alignment: TextAlignment,
    placeholder: bool,
    geometry: Rc<super::geometry::InputGeometry>,
}

struct MountedInput {
    state: Entity<InputState>,
    scope: ConfigScope,
    disabled_override: Option<bool>,
    hovered: bool,
    selecting: bool,
    drag_position: Option<Point<Pixels>>,
    drag_task: Option<Task<()>>,
    scroll_max: Point<Pixels>,
    caret_visible: bool,
    caret_eligible: bool,
    caret_reset: bool,
    caret_task: Option<Task<()>>,
    single_line_scroll: Point<Pixels>,
    scroll_area: Option<Entity<ScrollAreaState>>,
    ensure_caret: bool,
    cache: Rc<RefCell<LayoutCache>>,
    geometry_cache: Option<GeometryCache>,
    surface: SurfaceMotion,
    epoch: std::time::Instant,
    frame_pending: bool,
    component_motion: bool,
    accessibility_mapping: SelectionMapCell,
    _state_subscription: Subscription,
    _scope_subscription: Subscription,
    _window_subscriptions: Vec<Subscription>,
}

impl MountedInput {
    fn new(
        state: Entity<InputState>,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let state_subscription = Self::observe_state(&state, window, cx);
        let scope_subscription = Self::observe_scope(&scope, window, cx);
        let active = cx.observe_window_activation(window, |this, window, cx| {
            this.stop_drag();
            this.caret_reset = true;
            this.sync_caret(window, cx);
            cx.notify();
        });
        let visible = cx.observe_window_visibility(window, |this, _, window, cx| {
            this.stop_drag();
            this.caret_reset = true;
            this.sync_caret(window, cx);
            cx.notify();
        });
        Self {
            state,
            scope,
            disabled_override: None,
            hovered: false,
            selecting: false,
            drag_position: None,
            drag_task: None,
            scroll_max: Point::default(),
            caret_visible: false,
            caret_eligible: false,
            caret_reset: true,
            caret_task: None,
            single_line_scroll: Point::default(),
            scroll_area: None,
            ensure_caret: true,
            cache: Rc::default(),
            geometry_cache: None,
            surface: SurfaceMotion::default(),
            epoch: cx.background_executor().now(),
            frame_pending: false,
            component_motion: true,
            accessibility_mapping: SelectionMapCell::default(),
            _state_subscription: state_subscription,
            _scope_subscription: scope_subscription,
            _window_subscriptions: vec![active, visible],
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
        state: &Entity<InputState>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Subscription {
        cx.observe_in(state, window, |this, _, window, cx| {
            let state = this.state.read(cx);
            if state.disabled || !state.focus.is_focused(window) {
                this.stop_drag();
            }
            this.ensure_caret = !this.selecting;
            this.caret_reset = true;
            this.sync_caret(window, cx);
            cx.notify();
        })
    }

    fn caret_eligible(&self, window: &Window, cx: &App) -> bool {
        let state = self.state.read(cx);
        !state.disabled
            && state.focus.is_focused(window)
            && state.selection().is_empty()
            && window.is_window_active()
            && window.is_visible()
    }

    fn sync_caret(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let eligible = self.caret_eligible(window, cx);
        if self.caret_reset || eligible != self.caret_eligible {
            self.caret_task = None;
            self.caret_visible = eligible;
            self.caret_reset = false;
            self.caret_eligible = eligible;
        }
        let blink = eligible
            && !self.selecting
            && !self.state.read(cx).is_composing()
            && MotionPolicy::new(
                self.scope.snapshot(cx).motion && self.component_motion,
                cx.reduce_motion(),
            )
            .allows_motion();
        if !blink {
            self.caret_task = None;
            self.caret_visible = eligible;
        } else if self.caret_task.is_none() {
            // One wake per half second. The next timer is armed by rendering,
            // so a hidden/unmounted or stalled view cannot sustain a frame loop.
            self.caret_task = Some(cx.spawn_in(window, async move |mounted, cx| {
                cx.background_executor()
                    .timer(Duration::from_millis(500))
                    .await;
                let _ = mounted.update_in(cx, |this, window, cx| {
                    this.caret_task = None;
                    this.caret_visible = this.caret_eligible(window, cx) && !this.caret_visible;
                    cx.notify();
                });
            }));
        }
    }

    fn scroll(&self, cx: &App) -> Point<Pixels> {
        self.scroll_area
            .as_ref()
            .map_or(self.single_line_scroll, |area| -area.read(cx).offset())
    }

    fn set_scroll(&mut self, scroll: Point<Pixels>, cx: &App) {
        if let Some(area) = &self.scroll_area {
            // Caret reveal and selection dragging update the same native handle
            // as wheel/thumb input. Its content uses this editor's shaped height.
            area.read(cx).handle().set_offset(point(px(0.), -scroll.y));
        } else {
            self.single_line_scroll = scroll;
        }
    }

    fn stop_drag(&mut self) {
        self.selecting = false;
        self.drag_position = None;
        self.drag_task = None;
    }

    fn drag_target(&self, cx: &App) -> Option<Point<Pixels>> {
        let position = self.drag_position?;
        let geometry = self.state.read(cx).geometry()?;
        let viewport = geometry.viewport;
        Some(
            point(
                position.x.clamp(
                    viewport.left(),
                    (viewport.right() - px(1.)).max(viewport.left()),
                ),
                position.y.clamp(
                    viewport.top(),
                    (viewport.bottom() - px(1.)).max(viewport.top()),
                ),
            ) + self.scroll(cx)
                - geometry.scroll,
        )
    }

    fn drag_velocity(&self, cx: &App) -> Point<Pixels> {
        let Some(position) = self.drag_position else {
            return Point::default();
        };
        let Some(geometry) = self.state.read(cx).geometry() else {
            return Point::default();
        };
        let axis = |position: Pixels, start: Pixels, end: Pixels| {
            let outside = if position < start {
                position - start
            } else if position > end {
                position - end
            } else {
                px(0.)
            };
            let distance = f32::from(outside);
            px(if distance == 0. {
                0.
            } else {
                distance.signum() * (2. + distance.abs() / 6.).min(32.)
            })
        };
        point(
            axis(
                position.x,
                geometry.viewport.left(),
                geometry.viewport.right(),
            ),
            axis(
                position.y,
                geometry.viewport.top(),
                geometry.viewport.bottom(),
            ),
        )
    }

    fn advance_drag(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        let next = self.scroll(cx) + self.drag_velocity(cx);
        self.set_scroll(
            point(
                next.x.clamp(px(0.), self.scroll_max.x),
                next.y.clamp(px(0.), self.scroll_max.y),
            ),
            cx,
        );
        if let Some(position) = self.drag_target(cx) {
            self.state.update(cx, |state, cx| {
                state.select_at(position, true, 1, window, cx)
            });
        }
        self.ensure_caret = false;
    }

    fn schedule_drag(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if !self.selecting
            || self.drag_task.is_some()
            || !window.is_window_active()
            || !window.is_visible()
        {
            return;
        }
        let scroll = self.scroll(cx);
        let next = scroll + self.drag_velocity(cx);
        if next.x.clamp(px(0.), self.scroll_max.x) == scroll.x
            && next.y.clamp(px(0.), self.scroll_max.y) == scroll.y
        {
            return;
        }
        self.drag_task = Some(cx.spawn_in(window, async move |mounted, cx| {
            cx.background_executor()
                .timer(Duration::from_millis(16))
                .await;
            let _ = mounted.update_in(cx, |this, window, cx| {
                this.drag_task = None;
                if this.selecting && window.is_window_active() && window.is_visible() {
                    this.advance_drag(window, cx);
                    cx.notify();
                }
            });
        }));
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

impl RenderOnce for Input {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let search = self.mode == InputMode::Search;
        let wrapped = search || (self.multiline && self.count.is_some());
        let mut outer_style = StyleRefinement::default();
        if wrapped {
            // Width, flex and positioning belong to the complete composite.
            // Visual refinements still apply to the native editor surface.
            macro_rules! take { ($($field:ident),*) => { $(outer_style.$field = std::mem::take(&mut self.style.$field);)* }; }
            take!(
                position,
                inset,
                size,
                min_size,
                max_size,
                aspect_ratio,
                margin,
                align_self,
                flex_basis,
                flex_grow,
                flex_shrink,
                grid_location,
                opacity,
                visibility
            );
        }
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let mounted = window.use_keyed_state(self.id.clone(), cx, |window, cx| {
            MountedInput::new(self.state.clone(), scope.clone(), window, cx)
        });
        mounted.update(cx, |mounted, cx| {
            mounted.disabled_override = self.disabled;
            if mounted.state != self.state {
                mounted.stop_drag();
                mounted.caret_reset = true;
                mounted._state_subscription = MountedInput::observe_state(&self.state, window, cx);
                mounted.state = self.state.clone();
                mounted.set_scroll(Point::default(), cx);
            }
            if mounted.scope != scope {
                mounted._scope_subscription = MountedInput::observe_scope(&scope, window, cx);
                mounted.scope = scope.clone();
            }
            if self.multiline && mounted.scroll_area.is_none() {
                mounted.scroll_area = Some(cx.new(|cx| ScrollAreaState::new(window, cx)));
            } else if !self.multiline && mounted.scroll_area.is_some() {
                mounted.scroll_area = None;
                mounted.single_line_scroll = Point::default();
            }
        });
        let config = scope.snapshot(cx);
        let disabled = self
            .disabled
            .unwrap_or(config.disabled || self.state.read(cx).requested_disabled);
        self.state.update(cx, |state, cx| {
            state.set_multiline(self.multiline, window, cx);
            state.apply_disabled(disabled, window, cx);
            state.configure_mode(self.mode, self.search_loading, window, cx);
            if let Some(read_only) = self.read_only {
                state.set_read_only(read_only, window, cx);
            }
        });
        let tokens = self
            .tokens
            .clone()
            .unwrap_or_else(|| config.theme.component::<InputTheme>());
        mounted.update(cx, |mounted, cx| {
            mounted.component_motion = tokens.globals.motion;
            mounted.sync_caret(window, cx);
        });
        let state = self.state.read(cx);
        let can_clear =
            self.allow_clear && !disabled && !state.read_only && !state.value().is_empty();
        let password_visible = state.password_visible();
        let count = self.count.as_ref().map(|count| {
            let info = CountInfo {
                value: state.value(),
                count: state.character_count(),
                maximum: count.maximum.or(state.max_length()),
            };
            (
                count.format(info),
                info.maximum.is_some_and(|maximum| info.count > maximum),
            )
        });
        let focus = state.focus.clone();
        let focused = focus.contains_focused(window, cx);
        let mut metrics = InputStyle::from_tokens(
            &tokens,
            self.size.unwrap_or(config.size),
            self.variant.unwrap_or(config.variant),
            self.status,
            InputInteraction {
                disabled,
                hovered: mounted.read(cx).hovered,
                focused,
                keyboard_focus: window.last_input_was_keyboard(),
            },
        )
        .at_scale(window.scale_factor());
        if !self.numeric
            && (self.prefix.is_some()
                || self.suffix.is_some()
                || self.count.is_some()
                || self.allow_clear
                || (self.mode == InputMode::Password && self.visibility_toggle))
        {
            metrics = metrics.affix(
                &tokens,
                self.size.unwrap_or(config.size),
                self.variant.unwrap_or(config.variant),
                self.multiline,
            );
        }
        if self.multiline {
            metrics = metrics.multiline(&tokens);
        }
        if self.numeric {
            metrics = metrics.numeric(
                &tokens,
                self.size.unwrap_or(config.size),
                self.variant.unwrap_or(config.variant),
                self.status,
                disabled,
            );
        }
        if self.value_error && !disabled {
            metrics.text = tokens.globals.color_error.to_hsla();
        }
        if self.embedded {
            if let Some(height) = self.embedded_line_height {
                metrics.line_height = height;
            }
            metrics.padding_inline = 0.;
            metrics.padding_block = 0.;
            metrics.border_width = 0.;
            metrics.radius = 0.;
            metrics.background = gpui::transparent_black();
            metrics.border = gpui::transparent_black();
            metrics.keyboard_outline = false;
            metrics.underline_only = false;
        }
        let read_only = state.read_only;
        let text: SharedString = if state.value().is_empty() {
            self.placeholder.clone()
        } else {
            state.value_snapshot()
        };
        let mask_style = self
            .mask_style
            .as_ref()
            .filter(|_| state.masked && !state.value().is_empty());
        let layout_style = LayoutStyle {
            font: tokens.font.clone(),
            font_size: px(metrics.font_size as f32),
            line_height: px(metrics.line_height as f32),
            wrap_width: None,
            masked: state.masked && !state.value().is_empty(),
            mask_glyph: self.mask_glyph,
            right_to_left: config.direction == Direction::Rtl,
        };
        let layout_style = if let Some(mask) = mask_style {
            LayoutStyle {
                font: mask.font.clone(),
                font_size: px(mask.font_size as f32),
                ..layout_style
            }
        } else {
            layout_style
        };
        self.accessibility.disabled = disabled;
        self.accessibility.read_only = state.read_only;
        self.accessibility.invalid = self.status == ValidationStatus::Error || self.value_error;
        if state.is_protected() {
            self.accessibility.role = Some(Role::PasswordInput);
        } else if self.multiline {
            self.accessibility.role = Some(Role::MultilineTextInput);
        }
        let value = if state.is_protected() {
            SharedString::default()
        } else {
            state.value_snapshot()
        };
        let revision = state.buffer.revision();
        let accessible_geometry = GeometryCell::default();
        let semantics = TextSemantics {
            source: if state.value().is_empty() {
                SharedString::default()
            } else {
                text.clone()
            },
            selection: state.selection(),
            anchor_affinity: state.anchor_location().affinity,
            caret_affinity: state.caret_location().affinity,
            revision,
            protected: state.is_protected(),
            mask_glyph: state.masked.then(|| layout_style.mask_glyph.clone()),
            disabled,
            read_only: state.read_only,
            scale: window.scale_factor(),
            geometry: accessible_geometry.clone(),
        };
        let mut viewport = TextViewport {
            state: self.state.clone(),
            mounted: mounted.clone(),
            text,
            layout_style,
            multiline: self.multiline,
            rows: self.rows,
            auto_size: self.auto_size,
            revision,
            placeholder: state.value().is_empty(),
            selection_color: tokens.globals.color_primary_bg.to_hsla(),
            caret_color: metrics.text,
            text_color: metrics.text,
            accessible_geometry,
            alignment: if self.centered {
                TextAlignment::Center
            } else if config.direction == Direction::Rtl {
                TextAlignment::Right
            } else {
                TextAlignment::Left
            },
            padding: Edges::default(),
        };
        let shadow = mounted.update(cx, |mounted, cx| {
            let mut policy =
                MotionPolicy::new(config.motion && tokens.globals.motion, cx.reduce_motion());
            policy.enabled &= !self.embedded && window.is_visible() && window.is_window_active();
            let now = cx
                .background_executor()
                .now()
                .saturating_duration_since(mounted.epoch);
            let duration = if self.variant.unwrap_or(config.variant) == Variant::Borderless {
                Duration::ZERO
            } else {
                tokens.globals.motion_duration_mid
            };
            let (shadow, animating) = mounted.surface.resolve(
                &mut metrics,
                now,
                duration,
                if self.numeric {
                    tokens.globals.motion_duration_mid
                } else {
                    duration
                },
                if self.numeric { LINEAR } else { CSS_EASE },
                policy,
            );
            if animating {
                mounted.request_frame(window, cx);
            }
            shadow
        });
        viewport.text_color = if let Some(mask) = mask_style {
            mask.color
        } else if viewport.placeholder {
            metrics.placeholder
        } else {
            metrics.text
        };
        viewport.caret_color = mask_style.map_or(metrics.text, |mask| mask.color);
        let mut root = div()
            .id(self.id.clone())
            .relative()
            .flex()
            .items_center()
            .when(self.multiline, |root| root.items_stretch())
            .when(self.multiline && self.count.is_some(), |root| {
                root.flex_auto().min_h_0()
            })
            .gap(px(tokens.globals.padding_xxs as f32))
            .when(config.direction == Direction::Rtl, |root| {
                root.flex_row_reverse()
            })
            .w_full()
            .min_w_0()
            .px(px(metrics.padding_inline as f32))
            .py(px(metrics.padding_block as f32))
            .font(tokens.font.clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(px(metrics.line_height as f32))
            .text_color(metrics.text)
            .rounded(px(metrics.radius as f32))
            .bg(metrics.background)
            .border(px(metrics.border_width as f32))
            .border_color(metrics.border)
            .aria_value(value)
            .aria_placeholder(self.placeholder)
            .when(!disabled, |root| root.track_focus(&focus).cursor_text())
            .when(disabled, |root| root.cursor_not_allowed());
        if metrics.underline_only {
            root = root
                .border_0()
                .border_b(px(metrics.border_width as f32))
                .pt(px((metrics.padding_block + metrics.border_width) as f32));
        }
        if !self.embedded && !shadow.is_empty() {
            root = root.shadow(shadow);
        }
        if self.multiline && self.allow_clear {
            // Compact/custom tokens can make paddingLG narrower than the clear
            // affordance. Reserve its full hit area in either inline direction.
            let clear_padding = px(tokens
                .globals
                .padding_lg
                .max(metrics.padding_inline + tokens.globals.font_size_icon + 1.)
                as f32);
            root = match config.direction {
                Direction::Ltr => root.pr(clear_padding),
                Direction::Rtl => root.pl(clear_padding),
            };
        }
        root.style().refine(&self.style);
        let visibility = self.mode == InputMode::Password && self.visibility_toggle;
        let has_suffix =
            visibility || self.suffix.is_some() || (!self.multiline && count.is_some());
        let mut affix_padding = Edges::<DefiniteLength>::default();
        if self.multiline {
            // The scroll viewport reaches the inner frame edge. Text padding
            // belongs inside it, so it neither displaces the scrollbar nor
            // combines with a second gutter to narrow the editing area.
            affix_padding.refine(&root.style().padding);
            viewport.padding = affix_padding;
            if self.prefix.is_some() {
                match config.direction {
                    Direction::Ltr => viewport.padding.left = px(0.).into(),
                    Direction::Rtl => viewport.padding.right = px(0.).into(),
                }
            }
            if has_suffix {
                match config.direction {
                    Direction::Ltr => viewport.padding.right = px(0.).into(),
                    Direction::Rtl => viewport.padding.left = px(0.).into(),
                }
            }
            root = root.p_0();
        }
        if let Some(group) = &self.group {
            group.set_interaction(disabled, mounted.read(cx).hovered, focused);
            if wrapped {
                group.part(true, !search).apply_style(root.style());
                group.apply_style(&mut outer_style);
            } else {
                group.apply_style(root.style());
            }
        }
        root = super::accessibility::decorate(
            root,
            &self.state,
            semantics,
            mounted.read(cx).accessibility_mapping.clone(),
        );
        let target = self.state.clone();
        let mount = mounted.clone();
        let embedded = self.embedded;
        root = root.on_mouse_down(MouseButton::Left, move |event, window, cx| {
            target.update(cx, |state, cx| {
                if state.disabled {
                    return;
                }
                state.focus(window, cx);
                state.select_at(
                    event.position,
                    event.modifiers.shift,
                    event.click_count,
                    window,
                    cx,
                );
                mount.update(cx, |mounted, _| {
                    mounted.selecting = true;
                    mounted.drag_position = Some(event.position);
                });
                if !embedded {
                    cx.stop_propagation();
                }
            });
        });
        let mount = mounted.clone();
        root = root.on_mouse_up(MouseButton::Left, move |_, window, cx| {
            mount.update(cx, |mounted, cx| {
                mounted.stop_drag();
                mounted.caret_reset = true;
                mounted.sync_caret(window, cx);
                cx.notify();
            });
        });
        let mount = mounted.clone();
        root = root.on_mouse_up_out(MouseButton::Left, move |_, window, cx| {
            if mount.read(cx).selecting {
                mount.update(cx, |mounted, cx| {
                    mounted.stop_drag();
                    mounted.caret_reset = true;
                    mounted.sync_caret(window, cx);
                    cx.notify();
                });
            }
        });
        let mount = mounted.clone();
        root = root.on_hover(move |hovered, _, cx| {
            mount.update(cx, |mounted, cx| {
                mounted.hovered = *hovered;
                cx.notify();
            });
        });
        let line_height = px(metrics.line_height as f32);
        let scroll_mount = mounted.clone();
        if !self.multiline {
            root = root.on_scroll(move |event, _, cx| {
                scroll_mount.update(cx, |mounted, cx| {
                    let delta = event.delta.pixel_delta(line_height);
                    let scroll = mounted.scroll(cx) - delta;
                    mounted.set_scroll(point(scroll.x.max(px(0.)), scroll.y.max(px(0.))), cx);
                    mounted.ensure_caret = false;
                    cx.notify();
                });
            });
        }
        if let Some(prefix) = self.prefix {
            root = root.child(
                div()
                    .flex()
                    .items_center()
                    .flex_shrink_0()
                    .text_color(metrics.affix)
                    .when(self.multiline, |slot| match config.direction {
                        Direction::Ltr => slot.pl(affix_padding.left),
                        Direction::Rtl => slot.pr(affix_padding.right),
                    })
                    .child(prefix),
            );
        }
        root = root.child(viewport);
        let mut suffix = div()
            .flex()
            .items_center()
            .flex_shrink_0()
            .when(config.direction == Direction::Rtl, |slot| {
                slot.flex_row_reverse()
            })
            .when(self.multiline, |slot| match config.direction {
                Direction::Ltr => slot.pr(affix_padding.right),
                Direction::Rtl => slot.pl(affix_padding.left),
            });
        if self.allow_clear {
            // A hidden clear affordance retains its space, matching Ant's
            // visibility:hidden behavior without an invisible action or Tab stop.
            let clear = if can_clear {
                input_action(
                    self.id.clone(),
                    InputAction::Clear,
                    &self.state,
                    ActionTheme {
                        config: &config,
                        tokens: &tokens.globals,
                        font_size: metrics.font_size,
                    },
                    window,
                    cx,
                )
            } else {
                div()
                    .size(px(tokens.globals.font_size_icon as f32))
                    .into_any_element()
            };
            if self.multiline {
                root = root.child(
                    div()
                        .absolute()
                        .top(px(tokens.globals.padding_xs as f32))
                        .when(config.direction == Direction::Ltr, |slot| {
                            slot.right(px(metrics.padding_inline as f32))
                        })
                        .when(config.direction == Direction::Rtl, |slot| {
                            slot.left(px(metrics.padding_inline as f32))
                        })
                        .child(clear),
                );
            } else {
                let (start, end) = if has_suffix {
                    (tokens.globals.padding_xxs, tokens.globals.padding_xs)
                } else {
                    (0., 0.)
                };
                let (left, right) = config.direction.horizontal(start, end);
                suffix = suffix.child(
                    div()
                        .flex()
                        .items_center()
                        .ml(px(left as f32))
                        .mr(px(right as f32))
                        .child(clear),
                );
            }
        }
        if !self.multiline
            && let Some((count, exceeded)) = &count
        {
            let end = if visibility || self.suffix.is_some() {
                tokens.globals.padding_xxs
            } else {
                0.
            };
            let (left, right) = config.direction.horizontal(0., end);
            suffix = suffix.child(
                div()
                    .flex_shrink_0()
                    .ml(px(left as f32))
                    .mr(px(right as f32))
                    .text_color(if *exceeded {
                        tokens.globals.color_error.to_hsla()
                    } else if disabled {
                        tokens.globals.color_text_disabled.to_hsla()
                    } else {
                        tokens.globals.color_text_description.to_hsla()
                    })
                    .child(count.clone()),
            );
        }
        if visibility {
            let end = if self.suffix.is_some() {
                tokens.globals.padding_xs
            } else {
                0.
            };
            let (left, right) = config.direction.horizontal(0., end);
            suffix = suffix.child(
                div()
                    .flex()
                    .items_center()
                    .ml(px(left as f32))
                    .mr(px(right as f32))
                    .child(input_action(
                        self.id.clone(),
                        InputAction::Visibility(password_visible),
                        &self.state,
                        ActionTheme {
                            config: &config,
                            tokens: &tokens.globals,
                            font_size: metrics.font_size,
                        },
                        window,
                        cx,
                    )),
            );
        }
        if let Some(content) = self.suffix {
            suffix = suffix.child(
                div()
                    .flex()
                    .items_center()
                    .flex_shrink_0()
                    .text_color(metrics.affix)
                    .child(content),
            );
        }
        if has_suffix || (self.allow_clear && !self.multiline) {
            root = root.child(suffix);
        }
        if search {
            root = match config.direction {
                Direction::Ltr => root.rounded_tr(px(0.)).rounded_br(px(0.)),
                Direction::Rtl => root.rounded_tl(px(0.)).rounded_bl(px(0.)),
            };
        }
        if let Some(decorate) = self.frame_decorator {
            root = decorate(
                root,
                InputFrame {
                    metrics: metrics.clone(),
                    hovered: mounted.read(cx).hovered,
                    focused,
                    disabled,
                    read_only,
                },
                window,
                cx,
            );
        }
        // GPUI dispatches same-node actions in registration order. Specialized
        // controls own their commands; the shared editor supplies fallbacks.
        root = super::actions::bind(root, &self.state, focus.is_focused(window));
        if let Some(group) = &self.group {
            let ring = (metrics.keyboard_outline
                && focus.is_focused(window)
                && window.last_input_was_keyboard())
            .then(|| FocusRing::new(metrics.border, px(tokens.globals.line_width_focus as f32)));
            root = group
                .with_opacity(outer_style.opacity.unwrap_or(1.))
                .decorate(root, ring, window);
        }
        let input = self.accessibility.apply(root);
        let mut content = if metrics.keyboard_outline && self.group.is_none() {
            FocusRing::new(metrics.border, px(tokens.globals.line_width_focus as f32))
                .radius(px(metrics.radius as f32))
                .apply(input, &focus)
                .into_any_element()
        } else {
            input.into_any_element()
        };
        if search {
            let target = self.state.clone();
            let (color, variant, refinement) = super::style::search_button_style(
                &tokens,
                self.size.unwrap_or(config.size),
                self.variant.unwrap_or(config.variant),
                !matches!(self.search_button, SearchButton::Icon),
                disabled,
            );
            let mut button = Button::new("search-action")
                .scope(scope.clone())
                .size(self.size.unwrap_or(config.size))
                .disabled(disabled)
                .loading(self.search_loading)
                .color(color)
                .variant(variant)
                .refine_visuals(refinement)
                .accessibility_label(config.locale.common.search.clone())
                .flex_shrink_0()
                .on_click(move |_, _, cx| {
                    target.update(cx, |state, cx| state.search(SearchSource::Action, cx))
                });
            button = match self.search_button {
                SearchButton::Icon => button.icon(IconName::SearchOutlined),
                SearchButton::PrimaryIcon => button.icon(IconName::SearchOutlined),
                SearchButton::Label(label) => button.label(label),
            };
            if let Some(group) = &self.group {
                button = button.in_group(
                    group
                        .part(false, true)
                        .with_opacity(outer_style.opacity.unwrap_or(1.)),
                );
            }
            button = match config.direction {
                Direction::Ltr => button
                    .rounded_tl(px(0.))
                    .rounded_bl(px(0.))
                    .ml(px(-tokens.globals.line_width as f32)),
                Direction::Rtl => button
                    .rounded_tr(px(0.))
                    .rounded_br(px(0.))
                    .mr(px(-tokens.globals.line_width as f32)),
            };
            let mut wrapper = div()
                .id((self.id.clone(), "search-group"))
                .flex()
                .items_stretch()
                .w_full()
                .min_w_0()
                .when(config.direction == Direction::Rtl, |root| {
                    root.flex_row_reverse()
                })
                .child(content)
                .child(
                    div()
                        .flex_shrink_0()
                        .on_mouse_down(MouseButton::Left, |_, window, _| window.prevent_default())
                        .child(button),
                );
            wrapper.style().refine(&outer_style);
            content = wrapper.into_any_element();
        }
        if self.multiline
            && let Some((count, exceeded)) = count
        {
            let mut wrapper = div()
                .flex()
                .flex_col()
                .w_full()
                .min_w_0()
                .child(content)
                .child(
                    div()
                        .flex()
                        .flex_shrink_0()
                        .when(config.direction == Direction::Ltr, |root| {
                            root.justify_end()
                        })
                        .text_size(px(metrics.font_size as f32))
                        .line_height(px(metrics.line_height as f32))
                        .text_color(if exceeded {
                            tokens.globals.color_error.to_hsla()
                        } else {
                            if disabled {
                                tokens.globals.color_text_disabled.to_hsla()
                            } else {
                                tokens.globals.color_text_description.to_hsla()
                            }
                        })
                        .child(count),
                );
            wrapper.style().refine(&outer_style);
            content = wrapper.into_any_element();
        }
        content
    }
}

#[derive(Clone)]
struct TextViewport {
    state: Entity<InputState>,
    mounted: Entity<MountedInput>,
    text: SharedString,
    layout_style: LayoutStyle,
    multiline: bool,
    rows: usize,
    auto_size: Option<(usize, usize)>,
    revision: u64,
    placeholder: bool,
    selection_color: gpui::Hsla,
    caret_color: gpui::Hsla,
    text_color: gpui::Hsla,
    accessible_geometry: GeometryCell,
    alignment: TextAlignment,
    padding: Edges<DefiniteLength>,
}

impl IntoElement for TextViewport {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for TextViewport {
    type RequestLayoutState = ();
    type PrepaintState = (Rc<TextLayout>, Option<AnyElement>);

    fn id(&self) -> Option<ElementId> {
        None
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        _: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style::default();
        style.size.width = relative(1.).into();
        style.flex_grow = 1.;
        style.flex_shrink = 1.;
        style.padding = self.padding;
        let mounted = self.mounted.clone();
        let text = self.text.clone();
        let layout_style = self.layout_style.clone();
        let multiline = self.multiline;
        let rows = self.rows;
        let auto_size = self.auto_size;
        (
            window.request_measured_layout(style, move |known, available, window, cx| {
                let width = known
                    .width
                    .unwrap_or(match available.width {
                        AvailableSpace::Definite(w) => w,
                        _ => px(160.),
                    })
                    .max(px(1.));
                let mut style = layout_style.clone();
                style.wrap_width = multiline.then_some(width);
                let height = if multiline {
                    if let Some((min_rows, max_rows)) = auto_size {
                        mounted
                            .update(cx, |mounted, _| {
                                mounted
                                    .cache
                                    .borrow_mut()
                                    .shape(&text, &style, window)
                                    .height
                            })
                            .max(style.line_height * min_rows)
                            .min(style.line_height * max_rows)
                    } else {
                        style.line_height * rows
                    }
                } else {
                    style.line_height
                };
                size(width, known.height.unwrap_or(height))
            }),
            (),
        )
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> Self::PrepaintState {
        let padding = self
            .padding
            .map(|value| value.to_pixels(bounds.size.width.into(), window.rem_size()));
        let text_bounds = Bounds::new(
            bounds.origin
                + point(
                    padding.left.min(bounds.size.width),
                    padding.top.min(bounds.size.height),
                ),
            size(
                (bounds.size.width - padding.left - padding.right).max(px(0.)),
                (bounds.size.height - padding.top - padding.bottom).max(px(0.)),
            ),
        );
        self.layout_style.wrap_width = self.multiline.then_some(text_bounds.size.width.max(px(1.)));
        let layout = self.mounted.update(cx, |mounted, _| {
            mounted
                .cache
                .borrow_mut()
                .shape(&self.text, &self.layout_style, window)
        });
        let scroll_area = self.mounted.read(cx).scroll_area.clone();
        if let Some(scroll_area) = scroll_area {
            let mut editor = self.clone();
            let mut painter = self.clone();
            let shaped = layout.clone();
            let mut area = ScrollArea::new("text-scroll", &scroll_area)
                .scope(self.mounted.read(cx).scope.clone())
                .embedded()
                .w(bounds.size.width)
                .h(bounds.size.height)
                .child(
                    canvas(
                        move |_, window, cx| {
                            // Native layout has measured/clamped the handle.
                            // Paint and IME use viewport coordinates so caret
                            // reveal during this prepaint takes effect at once.
                            editor.prepaint_text(text_bounds, &shaped, window, cx);
                            shaped
                        },
                        move |_, layout, window, cx| {
                            painter.paint_text(text_bounds, &layout, window, cx);
                        },
                    )
                    .w_full()
                    .h(layout.height + bounds.size.height - text_bounds.size.height)
                    .flex_none(),
                )
                .into_any_element();
            area.layout_as_root(
                size(
                    AvailableSpace::Definite(bounds.size.width),
                    AvailableSpace::Definite(bounds.size.height),
                ),
                window,
                cx,
            );
            area.prepaint_at(bounds.origin, window, cx);
            (layout, Some(area))
        } else {
            self.prepaint_text(text_bounds, &layout, window, cx);
            (layout, None)
        }
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        (layout, area): &mut Self::PrepaintState,
        window: &mut Window,
        cx: &mut App,
    ) {
        if let Some(area) = area {
            area.paint(window, cx);
        } else {
            self.paint_text(bounds, layout, window, cx);
        }
    }
}

impl TextViewport {
    fn prepaint_text(
        &mut self,
        bounds: Bounds<Pixels>,
        layout: &Rc<TextLayout>,
        window: &mut Window,
        cx: &mut App,
    ) {
        let line_height = self.layout_style.line_height;
        let caret = self.state.read(cx).caret_location();
        let queried_geometry = {
            let state = self.state.read(cx);
            state
                .navigation_layout
                .as_ref()
                .filter(|navigation| {
                    navigation.style == self.layout_style
                        && navigation.alignment == self.alignment
                        && navigation.viewport == bounds
                })
                .and_then(|_| state.geometry_snapshot())
        };
        let geometry = self.mounted.update(cx, |mounted, cx| {
            let mut scroll = mounted.scroll(cx);
            let cached = mounted.geometry_cache.as_ref().filter(|cache| {
                Rc::ptr_eq(&cache.layout, layout)
                    && cache.geometry.revision == self.revision
                    && cache.geometry.viewport == bounds
                    && cache.alignment == self.alignment
                    && cache.placeholder == self.placeholder
            });
            let mut geometry = if let Some(cache) = cached {
                cache.geometry.clone()
            } else if let Some(geometry) = queried_geometry {
                geometry
            } else {
                let mut geometry =
                    layout.geometry(self.revision, bounds, scroll, line_height, self.alignment);
                if self.placeholder {
                    geometry.clusters.clear();
                    geometry.carets = vec![super::geometry::CaretStop {
                        offset: 0,
                        position: point(
                            match self.alignment {
                                TextAlignment::Center => bounds.size.width / 2.,
                                TextAlignment::Right => bounds.size.width,
                                TextAlignment::Left => px(0.),
                            },
                            px(0.),
                        ),
                    }];
                }
                Rc::new(geometry)
            };
            if mounted.ensure_caret {
                scroll = geometry.scroll_to_caret(caret, scroll);
            }
            mounted.scroll_max = point(
                if self.multiline {
                    px(0.)
                } else {
                    (layout.width - bounds.size.width).max(px(0.))
                },
                (layout.height - bounds.size.height).max(px(0.)),
            );
            scroll.x = scroll.x.clamp(px(0.), mounted.scroll_max.x);
            scroll.y = scroll.y.clamp(px(0.), mounted.scroll_max.y);
            mounted.set_scroll(scroll, cx);
            if geometry.scroll != scroll {
                Rc::make_mut(&mut geometry).scroll = scroll;
            }
            mounted.ensure_caret = false;
            mounted.geometry_cache = Some(GeometryCache {
                layout: layout.clone(),
                alignment: self.alignment,
                placeholder: self.placeholder,
                geometry: geometry.clone(),
            });
            geometry
        });
        *self.accessible_geometry.borrow_mut() = Some(geometry.clone());
        let cache = Rc::downgrade(&self.mounted.read(cx).cache);
        let scroll = geometry.scroll;
        self.state.update(cx, |state, _| {
            state.set_geometry(geometry);
            state.navigation_layout = Some(NavigationLayout {
                cache,
                style: self.layout_style.clone(),
                alignment: self.alignment,
                viewport: bounds,
                scroll,
            });
        });
        self.mounted
            .update(cx, |mounted, cx| mounted.schedule_drag(window, cx));
    }

    fn paint_text(
        &mut self,
        bounds: Bounds<Pixels>,
        layout: &Rc<TextLayout>,
        window: &mut Window,
        cx: &mut App,
    ) {
        let state = self.state.read(cx);
        let focus = state.focus.clone();
        let focused = focus.is_focused(window);
        let disabled = state.disabled;
        let selection = state.selection();
        let caret = state.caret_location();
        let geometry = state.geometry_snapshot();
        let marked = state.buffer.marked_range();
        let scroll = self.mounted.read(cx).scroll(cx);
        let caret_visible = self.mounted.read(cx).caret_visible;
        let mounted = self.mounted.clone();
        window.on_mouse_event(move |event: &gpui::MouseMoveEvent, phase, window, cx| {
            if phase != gpui::DispatchPhase::Capture || !mounted.read(cx).selecting {
                return;
            }
            mounted.update(cx, |mounted, cx| {
                if !event.dragging() {
                    mounted.stop_drag();
                    mounted.caret_reset = true;
                    mounted.sync_caret(window, cx);
                    cx.notify();
                    return;
                }
                mounted.drag_position = Some(event.position);
                if let Some(position) = mounted.drag_target(cx) {
                    mounted.state.update(cx, |state, cx| {
                        state.select_at(position, true, 1, window, cx)
                    });
                }
                mounted.ensure_caret = false;
                cx.notify();
            });
        });
        if !disabled {
            window.handle_input(
                &focus,
                ElementInputHandler::new(bounds, self.state.clone()),
                cx,
            );
        }
        window.with_content_mask(Some(ContentMask { bounds }), |window| {
            if !self.placeholder
                && let Some(geometry) = &geometry
            {
                for rect in geometry
                    .selection_rects(selection.range())
                    .into_iter()
                    .filter(|_| !selection.is_empty())
                {
                    window.paint_quad(fill(rect, self.selection_color));
                }
            }
            for positioned in &layout.paragraphs {
                let origin = bounds.origin + point(px(0.), positioned.top) - scroll;
                let height = positioned.paragraph.height(self.layout_style.line_height);
                if origin.y + height < bounds.top() || origin.y > bounds.bottom() {
                    continue;
                }
                positioned.paragraph.paint(
                    origin,
                    self.layout_style.line_height,
                    self.text_color,
                    self.alignment,
                    bounds.size.width.max(layout.width),
                    window,
                );
            }
            if let Some(geometry) = &geometry {
                if let Some(marked) = marked {
                    for rect in geometry.selection_rects(marked) {
                        window.paint_quad(fill(
                            Bounds::new(
                                point(rect.left(), rect.bottom() - px(1.)),
                                size(rect.size.width, px(1.)),
                            ),
                            self.caret_color,
                        ));
                    }
                }
                if focused
                    && caret_visible
                    && selection.is_empty()
                    && let Some(mut caret) = geometry.caret_bounds(caret)
                    && caret.left() <= bounds.right()
                    && caret.right() >= bounds.left()
                {
                    // An aligned end caret lies exactly on the content edge.
                    // Keep its stroke inside the clip without changing the
                    // text/selection/IME boundary used for hit testing.
                    caret.origin.x = caret
                        .left()
                        .min(bounds.right() - caret.size.width)
                        .max(bounds.left());
                    window.paint_quad(fill(caret, self.caret_color));
                }
            }
        });
    }
}
