// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    ScrollAreaEvent, ScrollAreaState, ScrollAreaTheme, ScrollAreaTokens, ScrollAxis,
    ScrollbarVisibility,
    geometry::{BarGeometry, clamp_offset, wheel_delta},
};
use crate::{
    ConfigScope, Direction,
    behavior::{Accessibility, FocusRing, MotionPolicy},
    locale::MessageKey,
};
use gpui::{
    AnyElement, App, AvailableSpace, Bounds, ContentMask, DispatchPhase, Element, ElementId,
    Entity, GlobalElementId, InspectorElementId, IntoElement, LayoutId, MouseButton,
    MouseMoveEvent, MouseUpEvent, ParentElement, Pixels, Refineable, RenderOnce, Role, ScrollEvent,
    SharedString, StyleRefinement, Styled, Subscription, WeakEntity, Window, accesskit, div, point,
    prelude::*, px, size,
};
use std::rc::Rc;

type ScrollHandler = Rc<dyn Fn(&ScrollAreaEvent, &mut Window, &mut App)>;

/// A bounded scroll viewport. Give the area a height (or a flex constraint); its
/// content keeps its natural height. Native wheel/trackpad offsets are immediate,
/// while scrollbar decoration animates independently and respects reduced motion.
#[derive(IntoElement)]
pub struct ScrollArea {
    id: ElementId,
    state: Entity<ScrollAreaState>,
    scope: Option<ConfigScope>,
    axis: ScrollAxis,
    visibility: ScrollbarVisibility,
    label: Option<SharedString>,
    description: Option<SharedString>,
    automation_id: Option<SharedString>,
    on_scroll: Option<ScrollHandler>,
    style: StyleRefinement,
    children: Vec<AnyElement>,
    embedded: bool,
}

impl ScrollArea {
    pub fn new(id: impl Into<ElementId>, state: &Entity<ScrollAreaState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            axis: ScrollAxis::Vertical,
            visibility: ScrollbarVisibility::Always,
            label: None,
            description: None,
            automation_id: None,
            on_scroll: None,
            style: StyleRefinement::default(),
            children: Vec::new(),
            embedded: false,
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn axis(mut self, axis: ScrollAxis) -> Self {
        self.axis = axis;
        self
    }
    pub fn scrollbar_visibility(mut self, visibility: ScrollbarVisibility) -> Self {
        self.visibility = visibility;
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = Some(label.into());
        self
    }
    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.description = Some(description.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.automation_id = Some(id.into());
        self
    }
    pub fn on_scroll(
        mut self,
        callback: impl Fn(&ScrollAreaEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_scroll = Some(Rc::new(callback));
        self
    }

    /// The containing control owns focus, traversal and keyboard navigation.
    pub(crate) fn embedded(mut self) -> Self {
        self.embedded = true;
        self
    }
}

impl Styled for ScrollArea {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

impl ParentElement for ScrollArea {
    fn extend(&mut self, elements: impl IntoIterator<Item = AnyElement>) {
        self.children.extend(elements);
    }
}

struct Mount {
    state: WeakEntity<ScrollAreaState>,
    generation: u64,
    _observer: Subscription,
    _release: Subscription,
}

impl RenderOnce for ScrollArea {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<ScrollAreaTheme>();
        let state = self.state;
        let observed = state.clone();
        let initial_visibility = if self.visibility == ScrollbarVisibility::Always {
            1.
        } else {
            0.
        };
        let mount_id = ("scroll-area-state", state.entity_id());
        let _mount = window.with_element_namespace(self.id.clone(), |window| {
            window.use_keyed_state(mount_id, cx, move |_, cx| {
                let generation = observed.update(cx, |s, _| {
                    s.mounted = true;
                    s.mount_generation = s.mount_generation.wrapping_add(1);
                    s.frame_pending = false;
                    s.visibility.retarget(
                        initial_visibility,
                        std::time::Duration::ZERO,
                        std::time::Duration::ZERO,
                        MotionPolicy::new(false, false),
                    );
                    s.mount_generation
                });
                Mount {
                    state: observed.downgrade(),
                    generation,
                    _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                    _release: cx.on_release(|this: &mut Mount, cx| {
                        let _ = this.state.update(cx, |s, _| {
                            if s.mount_generation != this.generation {
                                return;
                            }
                            s.mounted = false;
                            s.mount_generation = s.mount_generation.wrapping_add(1);
                            s.frame_pending = false;
                            s.drag = None;
                            s.hovered = false;
                            s.bar_hovered = [false; 2];
                            s.bars = [None; 2];
                        });
                    }),
                }
            })
        });
        state.update(cx, |s, cx| {
            s.configure_scope(scope, window, cx);
            s.focus_on_pointer = !self.embedded;
            if s.axis != self.axis {
                s.axis = self.axis;
                s.drag = None;
                if !self.axis.vertical() {
                    s.pending_bottom = false;
                }
                let offset = if s.handle.bounds().size.width > px(0.) {
                    clamp_offset(s.offset(), s.max_offset(), self.axis)
                } else {
                    point(
                        if self.axis.horizontal() {
                            s.offset().x
                        } else {
                            px(0.)
                        },
                        if self.axis.vertical() {
                            s.offset().y
                        } else {
                            px(0.)
                        },
                    )
                };
                s.handle.set_offset(offset);
            }
            if self.visibility == ScrollbarVisibility::Hidden {
                s.drag = None;
                s.bar_hovered = [false; 2];
            }
        });
        let handle = state.read(cx).handle.clone();
        let focus = state.read(cx).focus.clone();
        let target = state.clone();
        let measured = state.clone();
        let mut root = div()
            .on_children_prepainted(move |_, window, cx| {
                measured.update(cx, |s, _| s.line_height = window.line_height());
            })
            .id("viewport")
            .relative()
            .flex()
            .flex_col()
            .items_start()
            .w_full()
            .min_w(px(0.))
            .min_h(px(0.))
            .overflow_hidden()
            .track_scroll(&handle)
            .when(!self.embedded, |root| root.track_focus(&focus))
            .role(Role::ScrollView)
            .on_hover(move |hovered, _, cx| {
                target.update(cx, |s, cx| {
                    if s.hovered != *hovered {
                        s.hovered = *hovered;
                        cx.notify();
                    }
                });
            });
        root.style().refine(&self.style);
        // The axis and native listener configuration are part of the scroll
        // contract; visual refinements must not change them independently.
        root = root.overflow_hidden();
        if self.axis.horizontal() {
            root = root.overflow_x_scroll();
        }
        if self.axis.vertical() {
            root = root.overflow_y_scroll();
        }
        root.style().allow_concurrent_scroll = Some(true);
        root.style().restrict_scroll_to_axis = Some(false);
        // Scrollbar geometry is overlaid; styling cannot reserve a native gutter.
        root.style().scrollbar_width = Some(px(0.).into());
        let target = state.clone();
        let axis = self.axis;
        root = root.on_scroll(move |event, _, cx| {
            let moved = target.update(cx, |s, cx| {
                s.pending_bottom = false;
                let raw = event.delta.pixel_delta(s.line_height);
                // Capture the authoritative handle before native bubbling starts.
                // The native div listener runs immediately before this adapter;
                // normalize from that snapshot, without duplicating its arithmetic.
                let old = clamp_offset(s.wheel_start, s.max_offset(), axis);
                let next = clamp_offset(
                    old + wheel_delta(raw, event.modifiers.shift, axis, s.max_offset()),
                    s.max_offset(),
                    axis,
                );
                s.handle.set_offset(next);
                if next != old {
                    cx.notify();
                }
                next != old
            });
            if moved {
                cx.stop_propagation();
            }
        });
        let target = state.clone();
        root = root.on_key_down(move |event, window, cx| {
            let modifiers = event.keystroke.modifiers;
            // Descendant editors and controls retain their own key behavior.
            if target.read(cx).focus_on_pointer
                && target.read(cx).focus.is_focused(window)
                && !modifiers.control
                && !modifiers.platform
                && !modifiers.alt
                && target.update(cx, |s, cx| s.key(&event.keystroke.key, modifiers.shift, cx))
            {
                window.prevent_default();
                cx.stop_propagation();
            }
        });
        for (action, key) in [
            (accesskit::Action::ScrollUp, "up"),
            (accesskit::Action::ScrollDown, "down"),
            (accesskit::Action::ScrollLeft, "left"),
            (accesskit::Action::ScrollRight, "right"),
        ] {
            let target = state.clone();
            root = root.on_a11y_action(action, move |_, _, cx| {
                target.update(cx, |s, cx| {
                    s.key(key, false, cx);
                });
            });
        }
        let mut content = div()
            .id("content")
            .flex()
            .flex_col()
            .flex_none()
            .min_h_full()
            .children(self.children);
        content = if self.axis == ScrollAxis::Vertical {
            content.w_full()
        } else {
            content.min_w_full()
        };
        root = root.child(content);
        let accessibility = Accessibility {
            role: Some(Role::ScrollView),
            label: self.label,
            description: self.description,
            automation_id: self.automation_id,
            ..Default::default()
        };
        let ring = FocusRing::new(config.theme.tokens.color_primary.to_hsla(), px(2.))
            .with_offset(px(-2.))
            .radius(px(config.theme.tokens.border_radius as f32));
        let inner = accessibility.apply(root).into_any_element();
        let inner = if self.embedded {
            inner
        } else {
            ring.apply(inner, &focus).into_any_element()
        };
        ScrollAreaElement {
            id: self.id,
            inner,
            state,
            tokens,
            direction: config.direction,
            visibility: self.visibility,
            policy: MotionPolicy::new(
                config.motion && config.theme.tokens.motion,
                cx.reduce_motion(),
            ),
            duration: config.theme.tokens.motion_duration_mid,
            on_scroll: self.on_scroll,
        }
    }
}

struct ScrollAreaElement {
    id: ElementId,
    inner: AnyElement,
    state: Entity<ScrollAreaState>,
    tokens: std::sync::Arc<ScrollAreaTokens>,
    direction: Direction,
    visibility: ScrollbarVisibility,
    policy: MotionPolicy,
    duration: std::time::Duration,
    on_scroll: Option<ScrollHandler>,
}

impl IntoElement for ScrollAreaElement {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}

impl Element for ScrollAreaElement {
    type RequestLayoutState = ();
    type PrepaintState = Vec<AnyElement>;
    fn id(&self) -> Option<ElementId> {
        Some(self.id.clone())
    }
    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }
    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        (self.inner.request_layout(window, cx), ())
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> Vec<AnyElement> {
        self.state.update(cx, |s, _| {
            if std::mem::take(&mut s.pending_bottom) {
                s.handle.scroll_to_bottom();
            }
        });
        self.inner.prepaint(window, cx);
        let (bars, expansion, opacity, event) = self.state.update(cx, |s, cx| {
            let native = s.handle.bounds();
            let offset = clamp_offset(s.offset(), s.max_offset(), s.axis);
            s.handle.set_offset(offset);
            let maximum = s.max_offset();
            let width = px(self.tokens.thickness as f32).max(px(12.));
            let inset = px(self.tokens.inset as f32)
                .min(native.size.width / 2.)
                .min(native.size.height / 2.);
            let vertical_width = width.min((native.size.width - 2. * inset).max(px(0.)));
            let horizontal_height = width.min((native.size.height - 2. * inset).max(px(0.)));
            let horizontal = s.axis.horizontal()
                && maximum.x > px(0.)
                && self.visibility != ScrollbarVisibility::Hidden;
            let vertical = s.axis.vertical()
                && maximum.y > px(0.)
                && self.visibility != ScrollbarVisibility::Hidden;
            let vertical_track = Bounds::new(
                point(
                    if self.direction == Direction::Rtl {
                        native.left() + inset
                    } else {
                        native.right() - vertical_width - inset
                    },
                    native.top() + inset,
                ),
                size(
                    vertical_width,
                    (native.size.height
                        - 2. * inset
                        - if horizontal {
                            horizontal_height
                        } else {
                            px(0.)
                        })
                    .max(px(0.)),
                ),
            );
            let horizontal_track = Bounds::new(
                point(
                    native.left()
                        + inset
                        + if vertical && self.direction == Direction::Rtl {
                            vertical_width
                        } else {
                            px(0.)
                        },
                    native.bottom() - horizontal_height - inset,
                ),
                size(
                    (native.size.width
                        - 2. * inset
                        - if vertical { vertical_width } else { px(0.) })
                    .max(px(0.)),
                    horizontal_height,
                ),
            );
            let minimum = px(self.tokens.min_thumb_length as f32);
            s.bars = [
                horizontal
                    .then(|| {
                        BarGeometry::new(
                            horizontal_track,
                            false,
                            native.size.width,
                            maximum.x,
                            offset.x,
                            minimum,
                        )
                    })
                    .flatten(),
                vertical
                    .then(|| {
                        BarGeometry::new(
                            vertical_track,
                            true,
                            native.size.height,
                            maximum.y,
                            offset.y,
                            minimum,
                        )
                    })
                    .flatten(),
            ];
            for (bar, hovered) in s.bars.iter().zip(&mut s.bar_hovered) {
                if bar.is_none() {
                    *hovered = false;
                }
            }
            if !window.is_window_active()
                || !window.is_visible()
                || s.drag
                    .is_some_and(|d| s.bars[usize::from(d.vertical)].is_none())
            {
                s.drag = None;
            }
            let now = cx
                .background_executor()
                .now()
                .saturating_duration_since(s.epoch);
            let visible = self.visibility == ScrollbarVisibility::Always
                || (self.visibility == ScrollbarVisibility::Hover
                    && (s.hovered || s.focus.contains_focused(window, cx) || s.drag.is_some()));
            s.visibility.retarget(
                if visible { 1. } else { 0. },
                now,
                self.duration,
                self.policy,
            );
            let visibility = s.visibility.sample(now);
            let mut expansion = [0.; 2];
            let mut running = !visibility.finished;
            for (index, value) in expansion.iter_mut().enumerate() {
                let expanded = s.bar_hovered[index]
                    || s.drag.is_some_and(|d| usize::from(d.vertical) == index);
                s.expansion[index].retarget(
                    if expanded { 1. } else { 0. },
                    now,
                    self.duration,
                    self.policy,
                );
                let sample = s.expansion[index].sample(now);
                *value = sample.value as f32;
                running |= !sample.finished;
            }
            if running && window.is_visible() {
                s.request_frame(window, cx);
            }
            let event = (offset != s.last_offset).then(|| {
                s.last_offset = offset;
                let event = ScrollAreaEvent {
                    offset,
                    max_offset: maximum,
                    viewport_size: native.size,
                    content_size: size(
                        native.size.width + maximum.x,
                        native.size.height + maximum.y,
                    ),
                };
                cx.emit(event);
                event
            });
            (s.bars, expansion, visibility.value as f32, event)
        });
        if let Some(event) = event
            && let Some(callback) = &self.on_scroll
        {
            callback(&event, window, cx);
        }
        let mut overlays = Vec::with_capacity(2);
        if opacity <= 0. || self.visibility == ScrollbarVisibility::Hidden {
            return overlays;
        }
        for (index, geometry) in bars.into_iter().enumerate() {
            let Some(geometry) = geometry else {
                continue;
            };
            let mut overlay = scrollbar(
                &self.state,
                geometry,
                index,
                expansion[index],
                opacity,
                &self.tokens,
                cx,
            )
            .into_any_element();
            overlay.layout_as_root(
                size(
                    AvailableSpace::Definite(geometry.track.size.width),
                    AvailableSpace::Definite(geometry.track.size.height),
                ),
                window,
                cx,
            );
            window.with_content_mask(Some(ContentMask { bounds }), |window| {
                overlay.prepaint_at(geometry.track.origin, window, cx);
            });
            overlays.push(overlay);
        }
        overlays
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        overlays: &mut Vec<AnyElement>,
        window: &mut Window,
        cx: &mut App,
    ) {
        let target = self.state.downgrade();
        window.on_mouse_event(move |_: &ScrollEvent, phase, _, cx| {
            if phase == DispatchPhase::Capture {
                let _ = target.update(cx, |s, _| s.wheel_start = s.offset());
            }
        });
        self.inner.paint(window, cx);
        window.with_content_mask(Some(ContentMask { bounds }), |window| {
            for overlay in overlays {
                overlay.paint(window, cx);
            }
        });
        let target = self.state.downgrade();
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase != DispatchPhase::Capture {
                return;
            }
            let _ = target.update(cx, |s, cx| {
                if s.drag.is_some() {
                    if event.pressed_button == Some(MouseButton::Left) && window.is_window_active()
                    {
                        s.pointer_move(event.position, cx);
                    } else {
                        s.drag = None;
                        cx.notify();
                    }
                }
            });
        });
        let target = self.state.downgrade();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, _, cx| {
            if phase == DispatchPhase::Capture && event.button == MouseButton::Left {
                let _ = target.update(cx, |s, cx| {
                    if s.drag.take().is_some() {
                        cx.notify();
                    }
                });
            }
        });
    }
}

fn scrollbar(
    state: &Entity<ScrollAreaState>,
    geometry: BarGeometry,
    index: usize,
    expansion: f32,
    opacity: f32,
    tokens: &ScrollAreaTokens,
    cx: &App,
) -> impl IntoElement {
    let active = state
        .read(cx)
        .drag
        .is_some_and(|d| d.vertical == geometry.vertical);
    let color = if active {
        tokens.thumb_active_color
    } else {
        tokens
            .thumb_color
            .mix(tokens.thumb_hover_color, f64::from(expansion))
    };
    let thickness = tokens.collapsed_thickness as f32
        + (tokens.thickness - tokens.collapsed_thickness) as f32 * expansion;
    let config = state
        .read(cx)
        .scope
        .as_ref()
        .expect("configured scroll area")
        .snapshot(cx);
    let track = geometry.track;
    let visual_bounds = geometry.visual_thumb(px(thickness), config.direction);
    let thickness = if geometry.vertical {
        visual_bounds.size.width
    } else {
        visual_bounds.size.height
    };
    let value = if geometry.vertical {
        -state.read(cx).offset().y
    } else {
        -state.read(cx).offset().x
    };
    let mut bar = div()
        .id(("scrollbar", index))
        .relative()
        .w(track.size.width)
        .h(track.size.height)
        .role(Role::ScrollBar)
        .aria_label(
            config
                .locale
                .text(if geometry.vertical {
                    MessageKey::VerticalScrollBar
                } else {
                    MessageKey::HorizontalScrollBar
                })
                .clone(),
        )
        .aria_orientation(if geometry.vertical {
            accesskit::Orientation::Vertical
        } else {
            accesskit::Orientation::Horizontal
        })
        .aria_numeric_value(f64::from(f32::from(value)))
        .aria_min_numeric_value(0.)
        .aria_max_numeric_value(f64::from(f32::from(geometry.maximum)))
        .rounded(px(tokens.thickness as f32 / 2.))
        .bg(tokens
            .track_color
            .with_alpha(tokens.track_color.a * f64::from(expansion * opacity))
            .to_hsla());
    let mut visual = div()
        .absolute()
        .rounded(thickness / 2.)
        .bg(color.with_alpha(color.a * f64::from(opacity)).to_hsla());
    visual = visual
        .top(visual_bounds.top() - track.top())
        .left(visual_bounds.left() - track.left())
        .w(visual_bounds.size.width)
        .h(visual_bounds.size.height);
    let target = state.clone();
    bar = bar.child(visual).on_hover(move |hovered, _, cx| {
        target.update(cx, |s, cx| {
            if s.bar_hovered[index] != *hovered {
                s.bar_hovered[index] = *hovered;
                cx.notify();
            }
        });
    });
    let target = state.clone();
    bar = bar.on_mouse_down(MouseButton::Left, move |event, window, cx| {
        target.update(cx, |s, cx| {
            s.pointer_down(geometry, event.position, window, cx)
        });
        window.prevent_default();
        cx.stop_propagation();
    });
    for (action, backwards) in [
        (accesskit::Action::Increment, false),
        (accesskit::Action::Decrement, true),
    ] {
        let target = state.clone();
        bar = bar.on_a11y_action(action, move |_, _, cx| {
            target.update(cx, |s, cx| {
                let key = match (geometry.vertical, backwards) {
                    (true, true) => "up",
                    (true, false) => "down",
                    (false, true) => "left",
                    (false, false) => "right",
                };
                s.key(key, false, cx);
            });
        });
    }
    let target = state.clone();
    bar.on_a11y_action(accesskit::Action::SetValue, move |data, _, cx| {
        if let Some(accesskit::ActionData::NumericValue(value)) = data
            && value.is_finite()
        {
            target.update(cx, |s, cx| {
                let offset = px(-value.clamp(0., f64::from(f32::from(geometry.maximum))) as f32);
                s.scroll_to(
                    if geometry.vertical {
                        point(s.offset().x, offset)
                    } else {
                        point(offset, s.offset().y)
                    },
                    cx,
                );
            });
        }
    })
}
