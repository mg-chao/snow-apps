// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    ScrollAxis,
    geometry::{BarGeometry, clamp_offset},
};
use crate::{
    ConfigScope,
    behavior::{ScalarTransition, focus_handle},
    theme::CubicBezier,
};
use gpui::{
    App, Context, EventEmitter, FocusHandle, Focusable, Pixels, Point, ScrollHandle, Size,
    Subscription, Window, point, px,
};

/// A settled native scroll position. Offsets follow GPUI's convention: scrolling
/// down/right produces negative values. Sizes and maxima are positive.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ScrollAreaEvent {
    pub offset: Point<Pixels>,
    pub max_offset: Point<Pixels>,
    pub viewport_size: Size<Pixels>,
    pub content_size: Size<Pixels>,
}

#[derive(Clone, Copy)]
pub(super) struct Drag {
    pub vertical: bool,
    pub grab: Pixels,
}

/// Construct once, then use `ScrollArea::new(id, &state)` during rendering.
pub struct ScrollAreaState {
    pub(super) handle: ScrollHandle,
    pub(super) focus: FocusHandle,
    pub(super) focus_on_pointer: bool,
    pub(super) axis: ScrollAxis,
    pub(super) hovered: bool,
    pub(super) bar_hovered: [bool; 2],
    pub(super) bars: [Option<BarGeometry>; 2],
    pub(super) drag: Option<Drag>,
    pub(super) epoch: std::time::Instant,
    pub(super) expansion: [ScalarTransition; 2],
    pub(super) visibility: ScalarTransition,
    pub(super) frame_pending: bool,
    pub(super) last_offset: Point<Pixels>,
    pub(super) wheel_start: Point<Pixels>,
    pub(super) line_height: Pixels,
    pub(super) mounted: bool,
    pub(super) mount_generation: u64,
    pub(super) pending_bottom: bool,
    pub(super) scope: Option<ConfigScope>,
    scope_subscription: Option<Subscription>,
    _subscriptions: Vec<Subscription>,
}

impl EventEmitter<ScrollAreaEvent> for ScrollAreaState {}

impl Focusable for ScrollAreaState {
    fn focus_handle(&self, _: &App) -> FocusHandle {
        self.focus.clone()
    }
}

impl ScrollAreaState {
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let focus = focus_handle(cx);
        let subscriptions = vec![
            cx.on_focus(&focus, window, |_, _, cx| cx.notify()),
            cx.on_blur(&focus, window, |this, _, cx| {
                this.drag = None;
                cx.notify();
            }),
            cx.observe_window_activation(window, |this, window, cx| {
                if !window.is_window_active() {
                    this.cancel_pointer(cx);
                }
            }),
            cx.observe_window_visibility(window, |this, visibility, _, cx| {
                if !visibility.is_visible() {
                    this.cancel_pointer(cx);
                }
            }),
        ];
        let curve = CubicBezier {
            x1: 0.2,
            y1: 0.,
            x2: 0.,
            y2: 1.,
        };
        Self {
            handle: ScrollHandle::new(),
            focus,
            focus_on_pointer: true,
            axis: ScrollAxis::Vertical,
            hovered: false,
            bar_hovered: [false; 2],
            bars: [None; 2],
            drag: None,
            epoch: cx.background_executor().now(),
            expansion: [ScalarTransition::new(0., curve); 2],
            visibility: ScalarTransition::new(1., curve),
            frame_pending: false,
            last_offset: point(px(0.), px(0.)),
            wheel_start: point(px(0.), px(0.)),
            line_height: px(20.),
            mounted: false,
            mount_generation: 0,
            pending_bottom: false,
            scope: None,
            scope_subscription: None,
            _subscriptions: subscriptions,
        }
    }

    /// Use this handle with GPUI anchors or other native scroll consumers.
    /// After mutating it directly, notify the owning view to request a frame.
    pub fn handle(&self) -> &ScrollHandle {
        &self.handle
    }

    pub fn offset(&self) -> Point<Pixels> {
        self.handle.offset()
    }

    pub fn max_offset(&self) -> Point<Pixels> {
        self.handle.max_offset()
    }

    /// Apply a native offset, clamped to the measured content range. Before first
    /// layout the offset is retained and GPUI clamps it against the first layout.
    pub fn scroll_to(&mut self, offset: Point<Pixels>, cx: &mut Context<Self>) {
        self.pending_bottom = false;
        let offset = if self.handle.bounds().size.width > px(0.) {
            clamp_offset(offset, self.max_offset(), self.axis)
        } else {
            point(finite_negative(offset.x), finite_negative(offset.y))
        };
        if self.offset() != offset {
            self.handle.set_offset(offset);
            cx.notify();
        }
    }

    /// Add a native offset delta (negative values move down/right).
    pub fn scroll_by(&mut self, delta: Point<Pixels>, cx: &mut Context<Self>) {
        self.scroll_to(self.offset() + delta, cx);
    }

    pub fn scroll_to_top(&mut self, cx: &mut Context<Self>) {
        self.scroll_to(point(self.offset().x, px(0.)), cx);
    }

    /// Resolve the bottom against the next layout, including newly added content.
    pub fn scroll_to_bottom(&mut self, cx: &mut Context<Self>) {
        if self.axis.vertical() {
            self.pending_bottom = true;
            cx.notify();
        }
    }

    pub fn focus(&self, window: &mut Window, cx: &mut App) {
        self.focus.focus(window, cx);
    }

    pub(super) fn configure_scope(
        &mut self,
        scope: ConfigScope,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.scope.as_ref() != Some(&scope) {
            self.scope_subscription = Some(scope.observe_in(window, cx, |_, _, cx| cx.notify()));
            self.scope = Some(scope);
        }
    }

    pub(super) fn key(&mut self, key: &str, shift: bool, cx: &mut Context<Self>) -> bool {
        let viewport = self.handle.bounds().size;
        let maximum = self.max_offset();
        let offset = self.offset();
        let horizontal_page = self.axis == ScrollAxis::Horizontal
            || (shift && self.axis.horizontal() && maximum.x > px(0.));
        let page = if horizontal_page {
            viewport.width
        } else {
            viewport.height
        } * 0.9;
        let delta = match key {
            "up" if self.axis.vertical() => point(px(0.), px(40.)),
            "down" if self.axis.vertical() => point(px(0.), px(-40.)),
            "left" if self.axis.horizontal() => point(px(40.), px(0.)),
            "right" if self.axis.horizontal() => point(px(-40.), px(0.)),
            "pageup" | "pagedown" | "space" => {
                let backwards = key == "pageup" || (key == "space" && shift);
                let amount = if backwards { page } else { -page };
                if horizontal_page {
                    point(amount, px(0.))
                } else {
                    point(px(0.), amount)
                }
            }
            "home" => {
                self.scroll_to(
                    if horizontal_page {
                        point(px(0.), offset.y)
                    } else {
                        point(offset.x, px(0.))
                    },
                    cx,
                );
                return self.offset() != offset;
            }
            "end" => {
                self.scroll_to(
                    if horizontal_page {
                        point(-maximum.x, offset.y)
                    } else {
                        point(offset.x, -maximum.y)
                    },
                    cx,
                );
                return self.offset() != offset;
            }
            _ => return false,
        };
        self.scroll_by(delta, cx);
        self.offset() != offset
    }

    pub(super) fn pointer_down(
        &mut self,
        geometry: BarGeometry,
        position: Point<Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        if self.focus_on_pointer {
            self.focus.focus(window, cx);
        }
        if geometry.thumb.contains(&position) {
            self.drag = Some(Drag {
                vertical: geometry.vertical,
                grab: geometry.coordinate(position) - geometry.thumb_start(),
            });
            cx.notify();
        } else {
            let before = geometry.coordinate(position) < geometry.thumb_start();
            let viewport = self.handle.bounds().size;
            let amount = (if geometry.vertical {
                viewport.height
            } else {
                viewport.width
            }) * if before { 0.9 } else { -0.9 };
            self.scroll_by(
                if geometry.vertical {
                    point(px(0.), amount)
                } else {
                    point(amount, px(0.))
                },
                cx,
            );
        }
    }

    pub(super) fn pointer_move(&mut self, position: Point<Pixels>, cx: &mut Context<Self>) {
        if let Some(drag) = self.drag {
            if let Some(bar) = self.bars[usize::from(drag.vertical)] {
                let value = bar.offset_for_pointer(position, drag.grab);
                self.scroll_to(
                    if drag.vertical {
                        point(self.offset().x, value)
                    } else {
                        point(value, self.offset().y)
                    },
                    cx,
                );
            } else {
                self.drag = None;
                cx.notify();
            }
        }
    }

    pub(super) fn request_frame(&mut self, window: &Window, cx: &Context<Self>) {
        if self.frame_pending {
            return;
        }
        self.frame_pending = true;
        let weak = cx.weak_entity();
        let generation = self.mount_generation;
        window.on_next_frame(move |_, cx| {
            let _ = weak.update(cx, |this, cx| {
                if this.mounted && this.mount_generation == generation {
                    this.frame_pending = false;
                    cx.notify();
                }
            });
        });
    }

    fn cancel_pointer(&mut self, cx: &mut Context<Self>) {
        let changed = self.drag.take().is_some()
            || self.hovered
            || self.bar_hovered.into_iter().any(|value| value);
        self.hovered = false;
        self.bar_hovered = [false; 2];
        if changed {
            cx.notify();
        }
    }
}

fn finite_negative(value: Pixels) -> Pixels {
    if f32::from(value).is_finite() {
        value.min(px(0.))
    } else {
        px(0.)
    }
}
