// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    NotificationConfig, NotificationLayout, NotificationPlacement, NotificationState,
    NotificationTheme, NotificationTokens,
    card::{CardContext, NoticeVisual, PreparedCard},
    geometry,
    state::RenderNotice,
};
use crate::{
    ConfigScope, Direction,
    behavior::{MotionPolicy, ScalarTransition, evaluate_cubic_bezier, notice::HostElement},
};
use gpui::{
    AnyElement, App, AvailableSpace, Bounds, ContentMask, Context, Element, ElementId, Entity,
    EntityId, GlobalElementId, InspectorElementId, IntoElement, LayoutId, MouseMoveEvent, Pixels,
    Point, Refineable, RenderOnce, ScrollEvent, SharedString, Style, Subscription, WeakEntity,
    Window, div, point, prelude::*, px, size,
};
use std::{
    cell::RefCell,
    collections::HashMap,
    rc::Rc,
    sync::Arc,
    time::{Duration, Instant},
};

#[derive(Default)]
pub(super) struct GroupVisual {
    positions: HashMap<u64, ScalarTransition>,
    expansion: Option<ScalarTransition>,
    height: Option<HeightMotion>,
    collapsed: Option<bool>,
    scroll: f32,
    max_scroll: f32,
}
struct HeightMotion {
    from: f64,
    target: f64,
    started: Duration,
    delay: Duration,
    duration: Duration,
    curve: crate::theme::CubicBezier,
}

impl HeightMotion {
    fn sample(&self, now: Duration) -> (f32, bool) {
        if self.duration.is_zero() {
            return (self.target as f32, false);
        }
        let elapsed = now.saturating_sub(self.started);
        if elapsed < self.delay {
            return (self.from as f32, true);
        }
        let elapsed = elapsed.saturating_sub(self.delay);
        if elapsed >= self.duration {
            return (self.target as f32, false);
        }
        let progress = evaluate_cubic_bezier(
            self.curve,
            elapsed.as_secs_f64() / self.duration.as_secs_f64(),
        );
        (
            (self.from + (self.target - self.from) * progress) as f32,
            true,
        )
    }
}

#[derive(Default)]
pub(super) struct VisualState {
    pub cards: HashMap<u64, NoticeVisual>,
    groups: [GroupVisual; 6],
    epoch: Option<Instant>,
    active_mount: Option<EntityId>,
}
impl GroupVisual {
    fn sample_height(
        &mut self,
        target: f32,
        snap: bool,
        elapsed: Duration,
        tokens: &NotificationTokens,
        motion: MotionPolicy,
    ) -> (f32, bool) {
        let g = tokens.globals();
        let height = self.height.get_or_insert_with(|| HeightMotion {
            from: target.into(),
            target: target.into(),
            started: elapsed,
            delay: Duration::ZERO,
            duration: Duration::ZERO,
            curve: g.motion_ease_in_out,
        });
        if height.target != f64::from(target) || snap || !motion.allows_motion() {
            let from = height.sample(elapsed).0.into();
            let shrinking = f64::from(target) < height.target && !snap && motion.allows_motion();
            *height = HeightMotion {
                from,
                target: target.into(),
                started: elapsed,
                delay: if shrinking {
                    g.motion_duration_mid
                } else {
                    Duration::ZERO
                },
                duration: if shrinking {
                    g.motion_duration_slow.saturating_mul(2)
                } else {
                    Duration::ZERO
                },
                curve: g.motion_ease_in_out,
            };
        }
        height.sample(elapsed)
    }
}

impl VisualState {
    pub fn invalidate(&mut self, id: u64) {
        if let Some(card) = self.cards.get_mut(&id) {
            card.dirty = true;
        }
    }
    pub fn invalidate_all(&mut self) {
        for card in self.cards.values_mut() {
            card.dirty = true;
        }
    }
    pub fn clear(&mut self) {
        let active_mount = self.active_mount;
        *self = Self {
            active_mount,
            ..Default::default()
        };
    }
}

/// Mount once around the owning view; placement remains relative to the native window.
#[derive(IntoElement)]
pub struct NotificationHost {
    id: ElementId,
    state: Entity<NotificationState>,
    child: AnyElement,
}
impl NotificationHost {
    pub fn new(
        id: impl Into<ElementId>,
        state: &Entity<NotificationState>,
        child: impl IntoElement,
    ) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            child: child.into_any_element(),
        }
    }
}
struct Mount {
    id: EntityId,
    state: WeakEntity<NotificationState>,
    visual: Rc<RefCell<VisualState>>,
    _release: Subscription,
}
impl RenderOnce for NotificationHost {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        if !self.state.read(cx).window_matches(window) {
            return self.child;
        }
        let visual = self.state.read(cx).visual.clone();
        let state = self.state.clone();
        let mounted_visual = visual.clone();
        let key: ElementId = (
            self.id.clone(),
            SharedString::from(format!("notification-controller-{}", state.entity_id())),
        )
            .into();
        let mount = window.use_keyed_state(key, cx, move |_, cx| Mount {
            id: cx.entity_id(),
            state: state.downgrade(),
            visual: mounted_visual,
            _release: cx.on_release(|mount: &mut Mount, cx| {
                let owns = mount.visual.borrow().active_mount == Some(mount.id);
                if owns {
                    mount.visual.borrow_mut().active_mount = None;
                    super::state::unmount(&mount.state, cx);
                }
            }),
        });
        let id = mount.entity_id();
        let active = visual.borrow().active_mount;
        if active.is_some_and(|owner| owner != id) {
            return self.child;
        }
        if active.is_none() {
            visual.borrow_mut().active_mount = Some(id);
            self.state.update(cx, |state, _| state.mount(window));
        }
        HostElement {
            id: self.id,
            child: self.child,
            layer: self.state.into_any_element(),
        }
        .into_any_element()
    }
}

pub(super) fn render(
    state: &mut NotificationState,
    _: &mut Window,
    cx: &mut Context<NotificationState>,
) -> AnyElement {
    let notices = state.snapshot(cx);
    if notices.is_empty() {
        state.layouts.clear();
        state.visual.borrow_mut().clear();
        return gpui::Empty.into_any_element();
    }
    let snapshot = state.scope.snapshot(cx);
    let tokens = snapshot.theme.component::<NotificationTheme>();
    let motion = MotionPolicy::new(
        snapshot.motion && tokens.globals().motion,
        cx.reduce_motion(),
    );
    Viewport {
        state: cx.entity(),
        visual: state.visual.clone(),
        notices,
        scope: state.scope.clone(),
        config: state.effective_config(cx),
        tokens,
        direction: snapshot.direction,
        motion,
        hovered: state.hovered,
        focused: state.focused,
    }
    .into_any_element()
}
struct Viewport {
    state: Entity<NotificationState>,
    visual: Rc<RefCell<VisualState>>,
    notices: Vec<RenderNotice>,
    scope: ConfigScope,
    config: NotificationConfig,
    tokens: Arc<NotificationTokens>,
    direction: Direction,
    motion: MotionPolicy,
    hovered: [bool; 6],
    focused: [bool; 6],
}
impl IntoElement for Viewport {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for Viewport {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some("notification-viewport".into())
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
        (
            window.request_layout(
                Style {
                    size: size(px(0.).into(), px(0.).into()),
                    ..Default::default()
                },
                [],
                cx,
            ),
            (),
        )
    }
    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        self.state.update(cx, |state, _| state.layouts.clear());
        let viewport = window.viewport_size();
        // Native windows can have a nonpositive client area during creation,
        // minimization, or DPI changes. There is no drawable overlay in that frame.
        if viewport.width <= px(0.) || viewport.height <= px(0.) {
            return;
        }
        let priority = crate::modal::host::overlay_priority(self.tokens.z_index_popup, window, cx);
        for placement in NotificationPlacement::ALL {
            let notices: Vec<_> = self
                .notices
                .iter()
                .filter(|n| n.placement == placement)
                .cloned()
                .collect();
            if notices.is_empty() {
                continue;
            }
            let g = self.tokens.globals();
            let config = self.config.clone();
            let mut styles = config.styles.clone();
            if let Some(resolver) = &config.styles_fn {
                styles.refine(&resolver.resolve(
                    &super::NotificationStyleContext {
                        kind: None,
                        placement,
                        direction: self.direction,
                        motion: self.motion.allows_motion(),
                        closing: false,
                        stacked: false,
                        closable: config.closable,
                        show_progress: config.show_progress,
                    },
                    window,
                    cx,
                ));
            }
            let length = |value: Option<gpui::DefiniteLength>, fallback| {
                value.map_or(fallback, |v| {
                    f32::from(v.to_pixels(viewport.width.into(), window.rem_size()))
                })
            };
            let vertical_padding = length(styles.list.padding.top, g.margin_lg as f32)
                + length(styles.list.padding.bottom, g.margin_lg as f32)
                + length(styles.list_content.padding.top, 0.)
                + length(styles.list_content.padding.bottom, 0.);
            let gap = length(styles.list_content.gap.height, g.margin as f32).max(0.);
            let token_width = self
                .tokens
                .width
                .to_pixels(viewport.width.into(), window.rem_size());
            let holder_width = if f32::from(token_width).is_finite() {
                (token_width + px(g.margin_lg as f32 * 2.)).clamp(px(0.), viewport.width)
            } else {
                viewport.width.min(px(432.))
            };
            let inset = if placement.bottom() {
                config.bottom
            } else {
                config.top
            }
            .unwrap_or(g.margin_lg as f32);
            let group = Group {
                state: self.state.clone(),
                visual: self.visual.clone(),
                scope: self.scope.clone(),
                notices,
                placement,
                config,
                gap,
                vertical_padding,
                holder_visible: [&styles.list, &styles.list_content].iter().all(|style| {
                    style.opacity.unwrap_or(1.) > 0.
                        && style
                            .visibility
                            .is_none_or(|v| v == gpui::Visibility::Visible)
                        && style.display != Some(gpui::Display::None)
                }),
                tokens: self.tokens.clone(),
                direction: self.direction,
                motion: self.motion,
                hovered: self.hovered[placement.index()],
                focused: self.focused[placement.index()],
                cards: Vec::new(),
                placeholders: Vec::new(),
                regions: Vec::new(),
                clip: Bounds::default(),
                animating: false,
            };
            let mut content = div()
                .id(("notification-list-content", placement.index()))
                .relative()
                .flex_none()
                .w_full();
            content.style().refine(&styles.list_content);
            let mut layer = crate::behavior::traversal(div())
                .id(("notification-list", placement.index()))
                .relative()
                .flex()
                .flex_col()
                .w(holder_width)
                .h(viewport.height)
                .p(px(g.margin_lg as f32))
                .overflow_hidden()
                .when(placement.bottom(), |d| d.flex_col_reverse());
            layer.style().refine(&styles.list);
            let mut layer = layer.child(content.child(group)).into_any_element();
            let measured = layer.layout_as_root(
                size(
                    AvailableSpace::Definite(holder_width),
                    AvailableSpace::Definite(viewport.height),
                ),
                window,
                cx,
            );
            let x = if placement.center() {
                (viewport.width - measured.width) / 2.
            } else if placement.right() {
                viewport.width - measured.width
            } else {
                px(0.)
            };
            let y = px(if placement.bottom() {
                g.margin_lg as f32 - inset
            } else {
                inset - g.margin_lg as f32
            });
            window.defer_draw(layer, point(x, y), priority, None);
        }
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        _: &mut Window,
        _: &mut App,
    ) {
    }
}

struct Card {
    notice: RenderNotice,
    prepared: PreparedCard,
    y: f32,
    opacity: f32,
    animating: bool,
    visible: bool,
    bounds: Bounds<Pixels>,
}
struct Placeholder {
    element: AnyElement,
    bounds: Bounds<Pixels>,
    clip: Bounds<Pixels>,
}
struct Group {
    state: Entity<NotificationState>,
    visual: Rc<RefCell<VisualState>>,
    scope: ConfigScope,
    notices: Vec<RenderNotice>,
    placement: NotificationPlacement,
    config: NotificationConfig,
    tokens: Arc<NotificationTokens>,
    direction: Direction,
    motion: MotionPolicy,
    gap: f32,
    vertical_padding: f32,
    holder_visible: bool,
    hovered: bool,
    focused: bool,
    cards: Vec<Card>,
    placeholders: Vec<Placeholder>,
    regions: Vec<Bounds<Pixels>>,
    clip: Bounds<Pixels>,
    animating: bool,
}
fn fraction(
    start: Instant,
    now: Instant,
    duration: Duration,
    curve: crate::theme::CubicBezier,
    motion: MotionPolicy,
) -> f32 {
    if !motion.allows_motion() || duration.is_zero() {
        1.
    } else {
        evaluate_cubic_bezier(
            curve,
            now.saturating_duration_since(start).as_secs_f64() / duration.as_secs_f64(),
        ) as f32
    }
}
fn notice_motion(
    n: &RenderNotice,
    now: Instant,
    tokens: &NotificationTokens,
    motion: MotionPolicy,
) -> (f32, f32, bool) {
    let g = tokens.globals();
    let entry = fraction(
        n.created,
        n.closing.unwrap_or(now),
        g.motion_duration_mid,
        g.motion_ease_in_out,
        motion,
    );
    let exit = n.closing.map_or(0., |closed| {
        fraction(closed, now, n.exit_duration, g.motion_ease_in_out, motion)
    });
    let alpha = entry * (1. - exit);
    (
        alpha,
        64. * (1. - alpha),
        if n.closing.is_some() {
            alpha > 0.
        } else {
            entry < 1.
        },
    )
}
impl IntoElement for Group {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for Group {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some(("notification-list-content", self.placement.index()).into())
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
        let now = cx.background_executor().now();
        let g = self.tokens.globals();
        let viewport = window.viewport_size();
        let max_width = (viewport.width - px((2. * g.margin_lg) as f32)).max(px(0.));
        let width = self
            .tokens
            .width
            .to_pixels(viewport.width.into(), window.rem_size());
        let width = if f32::from(width).is_finite() {
            width.max(px(0.)).min(max_width)
        } else {
            px(384.).min(max_width)
        };
        let count = self.notices.iter().filter(|n| n.closing.is_none()).count();
        let stacked = self.config.stack.threshold().is_some_and(|t| count > t);
        let collapsed = stacked && !self.hovered && !self.focused;
        let (elapsed, expansion, expanding) = {
            let mut visual = self.visual.borrow_mut();
            let epoch = *visual.epoch.get_or_insert(now);
            let elapsed = now.saturating_duration_since(epoch);
            let transition = visual.groups[self.placement.index()]
                .expansion
                .get_or_insert_with(|| ScalarTransition::new(1., g.motion_ease_in_out));
            transition.retarget(
                if collapsed { 0. } else { 1. },
                elapsed,
                g.motion_duration_mid,
                self.motion,
            );
            let sample = transition.sample(elapsed);
            (elapsed, sample.value as f32, !sample.finished)
        };
        self.animating = expanding;
        let context = CardContext {
            state: &self.state,
            visual: &self.visual,
            config: &self.config,
            tokens: &self.tokens,
            scope: &self.scope,
            direction: self.direction,
            motion: self.motion,
            now,
            elapsed,
        };
        self.cards.clear();
        let mut index = 0;
        for n in &self.notices {
            let (alpha, _, moving) = notice_motion(n, now, &self.tokens, self.motion);
            let opacity = alpha * if stacked && index > 0 { expansion } else { 1. };
            let prepared = context.prepare(n, width, opacity, stacked, window, cx);
            let visible = self.holder_visible
                && (!stacked || index == 0 || expansion > 0.)
                && prepared.paintable
                && prepared.measured.width > px(0.)
                && prepared.measured.height > px(0.);
            self.cards.push(Card {
                notice: n.clone(),
                animating: moving || prepared.animating,
                prepared,
                y: 0.,
                opacity,
                visible,
                bounds: Bounds::default(),
            });
            if n.closing.is_none() {
                index += 1;
            }
        }
        let heights: Vec<_> = self
            .cards
            .iter()
            .filter(|c| c.notice.closing.is_none())
            .map(|c| f32::from(c.prepared.measured.height))
            .collect();
        let gap = self.gap;
        let offsets = geometry::offsets(&heights, gap, false);
        let mut active = 0;
        let mut visual = self.visual.borrow_mut();
        let group = &mut visual.groups[self.placement.index()];
        for card in &mut self.cards {
            let target = offsets.get(active).copied().unwrap_or(0.);
            let position = group
                .positions
                .entry(card.notice.id)
                .or_insert_with(|| ScalarTransition::new(target.into(), g.motion_ease_in_out));
            if card.notice.closing.is_none() {
                position.retarget(target.into(), elapsed, g.motion_duration_mid, self.motion);
                active += 1;
            }
            let sample = position.sample(elapsed);
            card.y = sample.value as f32 * expansion;
            self.animating |= card.visible && !sample.finished;
        }
        let retained: std::collections::HashSet<_> = self.notices.iter().map(|n| n.id).collect();
        group.positions.retain(|id, _| retained.contains(id));
        let height = if collapsed {
            heights.first().copied().unwrap_or(0.)
                + 8. * self
                    .config
                    .stack
                    .threshold()
                    .unwrap_or(1)
                    .min(heights.len())
                    .saturating_sub(1) as f32
        } else {
            heights.iter().sum::<f32>() + gap * (heights.len().saturating_sub(1)) as f32
        };
        let snap = group
            .collapsed
            .is_some_and(|previous| previous != collapsed);
        group.collapsed = Some(collapsed);
        let (height, changing_height) =
            group.sample_height(height, snap, elapsed, &self.tokens, self.motion);
        self.animating |= changing_height;
        let inset = if self.placement.bottom() {
            self.config.bottom
        } else {
            self.config.top
        }
        .unwrap_or(g.margin_lg as f32);
        group.max_scroll = (height + self.vertical_padding + (inset - g.margin_lg as f32).abs()
            - f32::from(viewport.height))
        .max(0.);
        group.scroll = group.scroll.clamp(0., group.max_scroll);
        (
            window.request_layout(
                Style {
                    size: size(gpui::relative(1.).into(), px(height).into()),
                    ..Default::default()
                },
                [],
                cx,
            ),
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
    ) {
        let g = self.tokens.globals();
        let viewport = bounds.size;
        let now = cx.background_executor().now();
        let inset = 0.;
        let (scroll, expansion) = {
            let visual = self.visual.borrow();
            let group = &visual.groups[self.placement.index()];
            (
                group.scroll,
                group.expansion.as_ref().map_or(1., |t| {
                    t.sample(now.saturating_duration_since(visual.epoch.unwrap_or(now)))
                        .value as f32
                }),
            )
        };
        self.clip = Bounds::new(Point::default(), window.viewport_size())
            .intersect(&window.content_mask().bounds);
        self.regions.clear();
        self.placeholders.clear();
        let threshold = self.config.stack.threshold().unwrap_or(0);
        let active: Vec<_> = self
            .cards
            .iter()
            .filter(|c| c.notice.closing.is_none())
            .collect();
        let collapsed_offsets = geometry::offsets(
            &active
                .iter()
                .map(|c| f32::from(c.prepared.measured.height))
                .collect::<Vec<_>>(),
            0.,
            true,
        );
        let mut stack_bounds = HashMap::new();
        if self.holder_visible && expansion < 1. && active.len() > threshold {
            for depth in (1..active.len().min(threshold).min(3)).rev() {
                let card = active[depth];
                let scale = 1. - depth.min(2) as f32 * 0.06;
                let measured = card.prepared.measured;
                if !card.prepared.paintable || measured.width <= px(0.) || measured.height <= px(0.)
                {
                    continue;
                }
                let mut base = geometry::card_bounds(
                    self.placement,
                    viewport,
                    measured,
                    0.,
                    inset,
                    collapsed_offsets[depth],
                    scroll,
                );
                base.origin += bounds.origin;
                let origin = point(
                    base.left() + measured.width * (1. - scale) / 2.,
                    base.top()
                        + if self.placement.bottom() {
                            px(0.)
                        } else {
                            measured.height * (1. - scale)
                        },
                );
                let scaled = size(measured.width * scale, measured.height * scale);
                let surface_bounds = Bounds::new(origin, scaled);
                stack_bounds.insert(card.notice.id, surface_bounds);
                // Only the exposed padding and surface are painted. Hidden content is not
                // mounted in the native focus/accessibility tree while the group is folded.
                let mut surface = div();
                surface.style().refine(&card.prepared.surface);
                surface = surface
                    .w(scaled.width)
                    .h(scaled.height)
                    .p(px(0.))
                    .opacity(card.prepared.surface_opacity * (1. - expansion));
                let style = surface.style();
                let lengths = |v: Option<gpui::AbsoluteLength>| {
                    v.map(|v| (v.to_pixels(window.rem_size()) * scale).into())
                };
                style.corner_radii = gpui::CornersRefinement {
                    top_left: lengths(card.prepared.surface.corner_radii.top_left),
                    top_right: lengths(card.prepared.surface.corner_radii.top_right),
                    bottom_left: lengths(card.prepared.surface.corner_radii.bottom_left),
                    bottom_right: lengths(card.prepared.surface.corner_radii.bottom_right),
                };
                style.border_widths = gpui::EdgesRefinement {
                    top: lengths(card.prepared.surface.border_widths.top),
                    right: lengths(card.prepared.surface.border_widths.right),
                    bottom: lengths(card.prepared.surface.border_widths.bottom),
                    left: lengths(card.prepared.surface.border_widths.left),
                };
                style.box_shadow = card.prepared.surface.box_shadow.as_ref().map(|shadows| {
                    shadows
                        .iter()
                        .map(|shadow| gpui::BoxShadow {
                            offset: shadow.offset * scale,
                            blur_radius: shadow.blur_radius * scale,
                            spread_radius: shadow.spread_radius * scale,
                            color: shadow.color,
                            inset: shadow.inset,
                        })
                        .collect()
                });
                if card
                    .notice
                    .options
                    .show_progress
                    .unwrap_or(self.config.show_progress)
                    && card.notice.budget.timed()
                {
                    surface = surface.child(
                        div()
                            .absolute()
                            .bottom(px(0.))
                            .left(px(g.border_radius_lg as f32 * scale))
                            .right(px(g.border_radius_lg as f32 * scale))
                            .h(px(2. * scale))
                            .bg(gpui::rgba(0x0000000a))
                            .child(
                                div()
                                    .h_full()
                                    .w(gpui::relative(card.notice.budget.fraction(now)))
                                    .bg(self.tokens.progress_bg),
                            ),
                    );
                }
                let mut element = surface.into_any_element();
                element.layout_as_root(
                    size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
                    window,
                    cx,
                );
                let shadow = px(g.margin_xxl as f32 * scale);
                let clip_origin = point(
                    origin.x - shadow,
                    if self.placement.bottom() {
                        origin.y - shadow
                    } else {
                        origin.y + scaled.height / 2.
                    },
                );
                let clip = Bounds::new(
                    clip_origin,
                    size(scaled.width + shadow * 2., scaled.height / 2. + shadow),
                )
                .intersect(&self.clip);
                window.with_content_mask(Some(ContentMask { bounds: clip }), |window| {
                    element.prepaint_at(origin, window, cx)
                });
                self.placeholders.push(Placeholder {
                    element,
                    bounds: surface_bounds,
                    clip,
                });
            }
        }
        let mut layouts = Vec::new();
        let mut focused = false;
        for card in self.cards.iter_mut().rev() {
            let (_, offset, _) = notice_motion(&card.notice, now, &self.tokens, self.motion);
            let mut card_bounds = geometry::card_bounds(
                self.placement,
                viewport,
                card.prepared.measured,
                0.,
                inset,
                card.y,
                scroll,
            );
            card_bounds.origin += bounds.origin;
            if self.placement.center() {
                card_bounds.origin.y += px(if self.placement.bottom() {
                    offset
                } else {
                    -offset
                });
            } else {
                card_bounds.origin.x += px(if self.placement.right() {
                    offset
                } else {
                    -offset
                });
            }
            card.bounds = card_bounds;
            if card.visible {
                card.prepared
                    .element
                    .prepaint_at(card_bounds.origin, window, cx);
                if card.notice.closing.is_none() {
                    let mut region = card_bounds;
                    region.origin.y -= px(g.margin as f32);
                    region.size.height += px(g.margin as f32);
                    self.regions.push(region);
                }
            }
            if let Some(surface) = self.visual.borrow().cards.get(&card.notice.id) {
                focused |= card.notice.closing.is_none()
                    && surface.boundary.0.root.contains_focused(window, cx);
            }
            layouts.push(NotificationLayout {
                id: card.notice.id,
                key: card.notice.key.clone(),
                placement: self.placement,
                bounds: stack_bounds
                    .get(&card.notice.id)
                    .copied()
                    .unwrap_or(card_bounds),
                closing: card.notice.closing.is_some(),
                visible: card.visible && card.opacity > 0.
                    || stack_bounds.contains_key(&card.notice.id),
            });
        }
        for p in &self.placeholders {
            self.regions.push(p.bounds);
        }
        layouts.reverse();
        self.state
            .update(cx, |state, _| state.layouts.extend(layouts));
        let hovered = self.clip.contains(&window.mouse_position())
            && self
                .regions
                .iter()
                .any(|r| r.contains(&window.mouse_position()));
        if hovered != self.hovered || focused != self.focused {
            let weak = self.state.downgrade();
            let placement = self.placement;
            window.defer(cx, move |window, cx| {
                let _ = weak.update(cx, |state, cx| {
                    state.group_interaction(placement, hovered, focused, window, cx)
                });
            });
        }
    }
    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        _: Bounds<Pixels>,
        _: &mut (),
        _: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) {
        for p in &mut self.placeholders {
            window.with_content_mask(Some(ContentMask { bounds: p.clip }), |window| {
                p.element.paint(window, cx)
            });
        }
        for card in self.cards.iter_mut().rev() {
            if card.visible {
                card.prepared.element.paint(window, cx);
                if card.opacity > 0.
                    && !card.bounds.intersect(&self.clip).is_empty()
                    && let Some(surface) = self.visual.borrow_mut().cards.get_mut(&card.notice.id)
                {
                    surface.presented = true;
                }
            }
        }
        let weak = self.state.downgrade();
        let placement = self.placement;
        let regions = self.regions.clone();
        let clip = self.clip;
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase.capture() {
                let hovered = clip.contains(&event.position)
                    && regions.iter().any(|r| r.contains(&event.position));
                let _ = weak.update(cx, |state, cx| {
                    state.group_interaction(
                        placement,
                        hovered,
                        state.focused[placement.index()],
                        window,
                        cx,
                    )
                });
            }
        });
        let weak = self.state.downgrade();
        let visual = self.visual.clone();
        let regions = self.regions.clone();
        let line_height =
            px((self.tokens.globals().font_size * self.tokens.globals().line_height) as f32);
        window.on_mouse_event(move |event: &ScrollEvent, phase, _, cx| {
            if phase.capture()
                && clip.contains(&event.position)
                && regions.iter().any(|r| r.contains(&event.position))
            {
                let mut visual = visual.borrow_mut();
                let group = &mut visual.groups[placement.index()];
                let old = group.scroll;
                let delta = f32::from(event.delta.pixel_delta(line_height).y);
                group.scroll = (old + if placement.bottom() { delta } else { -delta })
                    .clamp(0., group.max_scroll);
                if old != group.scroll {
                    let _ = weak.update(cx, |_, cx| cx.notify());
                }
                if group.max_scroll > 0. {
                    cx.stop_propagation();
                }
            }
        });
        if self.cards.iter().any(|c| {
            c.visible
                && (self.animating || c.animating)
                && !c.bounds.intersect(&self.clip).is_empty()
        }) {
            self.state
                .update(cx, |state, cx| state.schedule_frame(window, cx));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn measured_group_height_delays_shrink_and_interrupts_from_current_height() {
        let tokens = crate::theme::ThemeConfig::default()
            .resolve()
            .component::<NotificationTheme>();
        let mut group = GroupVisual::default();
        let motion = MotionPolicy::new(true, false);
        assert_eq!(
            group.sample_height(200., false, Duration::ZERO, &tokens, motion),
            (200., false)
        );
        assert_eq!(
            group.sample_height(100., false, Duration::ZERO, &tokens, motion),
            (200., true)
        );
        let delay = tokens.globals().motion_duration_mid;
        let duration = tokens.globals().motion_duration_slow * 2;
        assert_eq!(
            group.sample_height(100., false, delay, &tokens, motion),
            (200., true)
        );
        let middle = group.sample_height(100., false, delay + duration / 2, &tokens, motion);
        assert!(middle.0 > 100. && middle.0 < 200. && middle.1);
        assert_eq!(
            group.sample_height(100., false, delay + duration, &tokens, motion),
            (100., false)
        );
        assert_eq!(
            group.sample_height(300., false, delay + duration, &tokens, motion),
            (300., false)
        );
        assert_eq!(
            group.sample_height(50., true, delay + duration, &tokens, motion),
            (50., false)
        );
    }
}
