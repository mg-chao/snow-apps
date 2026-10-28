// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::{Cell, RefCell},
    collections::{HashMap, HashSet},
    rc::Rc,
    time::{Duration, Instant},
};

use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Context, Edges, Element, ElementId, Entity, EntityId,
    FocusHandle, Global, GlobalElementId, InspectorElementId, IntoElement, LayoutId, Length,
    MouseMoveEvent, Pixels, Point, Refineable, RenderOnce, ScrollEvent, SharedString, Style,
    StyleRefinement, Subscription, WeakEntity, WeakFocusHandle, Window, WindowId, div, point,
    prelude::*, px, radians, relative, size,
};

use super::{
    MessageConfig, MessageContent, MessageLayout, MessageStack, MessageState, MessageStyle,
    MessageStyleContext, MessageTheme, MessageTokens, MessageType,
    element::{ContentProbe, HostElement, LiveSurface, SurfaceGeometry},
    state::RenderNotice,
};
use crate::{
    Direction,
    behavior::{MotionPolicy, ScalarTransition, evaluate_cubic_bezier},
    icons::{IconName, IconPalette, IconRenderOptions, icon},
};

#[derive(Default)]
pub(super) struct VisualState {
    epoch: Option<Instant>,
    positions: HashMap<u64, ScalarTransition>,
    measurements: HashMap<u64, CardMeasurement>,
    boundaries: HashMap<u64, FocusBoundary>,
    expansion: Option<ScalarTransition>,
    placeholder_opacity: Option<ScalarTransition>,
    placeholder_width: Option<ScalarTransition>,
    height: Option<HeightMotion>,
    stacked: Option<bool>,
    scroll: f32,
    max_scroll: f32,
    active_mount: Option<EntityId>,
}

#[derive(Clone, Copy)]
struct PlaceholderSample {
    opacity: f32,
    width: f32,
    animating: bool,
}

#[derive(Clone, Default)]
struct CardMeasurement {
    layout: Rc<Cell<Option<LayoutId>>>,
    geometry: Rc<Cell<Option<SurfaceGeometry>>>,
    offset: Rc<Cell<Point<Pixels>>>,
    presented: Rc<Cell<bool>>,
}

#[derive(Clone, Copy)]
struct CardPresentation {
    opacity: f32,
    stacked: bool,
    now: Instant,
}

struct PreparedCard {
    element: AnyElement,
    measurement: CardMeasurement,
    padding: Edges<Pixels>,
    border: Edges<Pixels>,
    style: StyleRefinement,
    wrapper_style: Style,
    max_width: Pixels,
    displayed: bool,
    participates: bool,
    opacity: f32,
    animate_root: bool,
    animate_indicator: bool,
}

impl PreparedCard {
    fn measure(mut self, window: &mut Window, cx: &mut App) -> (AnyElement, gpui::Size<Pixels>) {
        let native = self.element.layout_as_root(
            size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
            window,
            cx,
        );
        let content = if self.participates {
            self.measurement
                .layout
                .get()
                .map(|id| window.layout_bounds(id).size)
                .unwrap_or_default()
        } else {
            gpui::Size::default()
        };
        let resolve = |value: Option<Length>, relative: Pixels| match value {
            Some(Length::Definite(value)) => Some(
                value
                    .to_pixels(relative.into(), window.rem_size())
                    .max(px(0.)),
            ),
            _ => None,
        };
        let dimension = |intrinsic: Pixels,
                         explicit: Option<Length>,
                         min: Option<Length>,
                         max: Option<Length>,
                         relative: Pixels,
                         fallback_max: Option<Pixels>| {
            let min = resolve(min, relative).unwrap_or(px(0.));
            let max = resolve(max, relative)
                .or(fallback_max)
                .unwrap_or(px(f32::MAX))
                .max(min);
            resolve(explicit, relative)
                .unwrap_or(intrinsic)
                .clamp(min, max)
        };
        let logical = if !self.participates {
            gpui::Size::default()
        } else if self.style.aspect_ratio.is_some() {
            native
        } else {
            size(
                dimension(
                    content.width
                        + self.padding.left
                        + self.padding.right
                        + self.border.left
                        + self.border.right,
                    self.style.size.width,
                    self.style.min_size.width,
                    self.style.max_size.width,
                    self.max_width,
                    self.style
                        .max_size
                        .width
                        .is_none()
                        .then_some(self.max_width),
                ),
                dimension(
                    content.height
                        + self.padding.top
                        + self.padding.bottom
                        + self.border.top
                        + self.border.bottom,
                    self.style.size.height,
                    self.style.min_size.height,
                    self.style.max_size.height,
                    window.viewport_size().height,
                    None,
                ),
            )
        };
        self.measurement.geometry.set(Some(SurfaceGeometry {
            logical,
            scale: window.scale_factor(),
        }));
        let mut root_style = Style::default();
        root_style.refine(&self.style);
        let horizontal = inset_anchor(&root_style, &self.wrapper_style, true);
        let vertical = inset_anchor(&root_style, &self.wrapper_style, false);
        let offset = |start: Pixels, end: Pixels, size_delta: Pixels, anchor: InsetAnchor| {
            let start = start - window.pixel_snap(start);
            let end = end - window.pixel_snap(end);
            match anchor {
                InsetAnchor::Start => start,
                InsetAnchor::Center => (size_delta + start - end) / 2.,
                InsetAnchor::End => size_delta - end,
            }
        };
        self.measurement.offset.set(point(
            offset(
                self.padding.left,
                self.padding.right,
                logical.width - native.width,
                horizontal,
            ),
            offset(
                self.padding.top,
                self.padding.bottom,
                logical.height - native.height,
                vertical,
            ),
        ));
        (self.element, logical)
    }
}

#[derive(Clone, Copy)]
enum InsetAnchor {
    Start,
    Center,
    End,
}

fn inset_anchor(root: &Style, wrapper: &Style, horizontal: bool) -> InsetAnchor {
    use gpui::{AlignContent, AlignItems, Display, FlexDirection};
    let main = |style: &Style| match style.justify_content {
        Some(AlignContent::Center | AlignContent::SpaceAround | AlignContent::SpaceEvenly) => {
            InsetAnchor::Center
        }
        Some(AlignContent::End) => InsetAnchor::End,
        Some(AlignContent::Start) => InsetAnchor::Start,
        value => {
            let reverse = matches!(
                style.flex_direction,
                FlexDirection::RowReverse | FlexDirection::ColumnReverse
            );
            if matches!(value, Some(AlignContent::FlexEnd)) != reverse {
                InsetAnchor::End
            } else {
                InsetAnchor::Start
            }
        }
    };
    let cross = |style: &Style| match style.align_items {
        Some(AlignItems::Center) => InsetAnchor::Center,
        Some(AlignItems::End | AlignItems::FlexEnd) => InsetAnchor::End,
        _ => InsetAnchor::Start,
    };
    if root.display != Display::Flex {
        return InsetAnchor::Start;
    }
    let row = matches!(
        root.flex_direction,
        FlexDirection::Row | FlexDirection::RowReverse
    );
    if row == horizontal {
        return main(root);
    }
    if matches!(root.align_items, None | Some(AlignItems::Stretch)) {
        let wrapper_row = matches!(
            wrapper.flex_direction,
            FlexDirection::Row | FlexDirection::RowReverse
        );
        if wrapper_row == horizontal {
            main(wrapper)
        } else {
            cross(wrapper)
        }
    } else {
        cross(root)
    }
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

impl VisualState {
    pub(super) fn clear_content(&mut self) {
        let active_mount = self.active_mount;
        *self = Self::default();
        self.active_mount = active_mount;
    }

    fn sample_height(
        &mut self,
        target: f32,
        snap: bool,
        elapsed: Duration,
        tokens: &MessageTokens,
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

    fn sample_placeholder(
        &mut self,
        width: f32,
        shown: bool,
        elapsed: Duration,
        tokens: &MessageTokens,
        motion: MotionPolicy,
    ) -> PlaceholderSample {
        let g = tokens.globals();
        let opacity = self
            .placeholder_opacity
            .get_or_insert_with(|| ScalarTransition::new(0., g.motion_ease_in_out));
        opacity.retarget(
            if shown { 1. } else { 0. },
            elapsed,
            g.motion_duration_fast,
            motion,
        );
        let opacity = opacity.sample(elapsed);
        let width_motion = self
            .placeholder_width
            .get_or_insert_with(|| ScalarTransition::new(width.into(), g.motion_ease_in_out));
        width_motion.retarget(width.into(), elapsed, g.motion_duration_slow, motion);
        let width = width_motion.sample(elapsed);
        PlaceholderSample {
            opacity: opacity.value as f32,
            width: width.value as f32,
            animating: !opacity.finished || opacity.value > 0. && !width.finished,
        }
    }
}

struct BoundaryState {
    root: FocusHandle,
    window: WindowId,
    return_to: Option<WeakFocusHandle>,
    closed: Cell<bool>,
    _focus: Subscription,
}

#[derive(Default)]
struct FocusBoundaries(Vec<std::rc::Weak<BoundaryState>>);
impl Global for FocusBoundaries {}

/// An outgoing rich-content tree keeps its native paint while losing interaction.
#[derive(Clone)]
pub(super) struct FocusBoundary(Rc<BoundaryState>);

impl FocusBoundary {
    fn new(window: &mut Window, cx: &mut App) -> Self {
        let root = cx.focus_handle();
        let window_id = window.window_handle().window_id();
        let return_to = window.focused(cx).map(|focus| focus.downgrade());
        let state = Rc::new_cyclic(|weak: &std::rc::Weak<BoundaryState>| {
            let observed = weak.clone();
            let subscription = window.on_focus_in(&root, cx, move |window, cx| {
                if let Some(state) = observed.upgrade()
                    && state.closed.get()
                {
                    FocusBoundary(state).skip(window, cx);
                }
            });
            BoundaryState {
                root,
                window: window_id,
                return_to,
                closed: Cell::new(false),
                _focus: subscription,
            }
        });
        if !cx.has_global::<FocusBoundaries>() {
            cx.set_global(FocusBoundaries::default());
        }
        let registry = cx.global_mut::<FocusBoundaries>();
        registry.0.retain(|weak| weak.strong_count() != 0);
        registry.0.push(Rc::downgrade(&state));
        Self(state)
    }

    fn closed_roots(&self, cx: &App) -> Vec<FocusHandle> {
        cx.try_global::<FocusBoundaries>()
            .map(|registry| {
                registry
                    .0
                    .iter()
                    .filter_map(std::rc::Weak::upgrade)
                    .filter(|boundary| boundary.window == self.0.window && boundary.closed.get())
                    .map(|boundary| boundary.root.clone())
                    .collect()
            })
            .unwrap_or_else(|| vec![self.0.root.clone()])
    }

    fn restore(&self, roots: &[FocusHandle], window: &mut Window, cx: &mut App) {
        if let Some(focus) = self.0.return_to.as_ref().and_then(WeakFocusHandle::upgrade)
            && !roots.iter().any(|root| root.contains(&focus, window))
        {
            window.focus(&focus, cx);
        } else {
            window.blur(cx);
        }
    }

    pub(super) fn dismiss(&self, window: &mut Window, cx: &mut App) {
        if self.0.root.contains_focused(window, cx) {
            self.restore(&self.closed_roots(cx), window, cx);
        }
    }

    pub(super) fn is_closed(&self) -> bool {
        self.0.closed.get()
    }

    pub(super) fn skip(&self, window: &mut Window, cx: &mut App) {
        let mut visited = Vec::new();
        let roots = self.closed_roots(cx);
        while roots.iter().any(|root| root.contains_focused(window, cx)) {
            let current = window.focused(cx);
            if current.is_none() || visited.contains(&current) {
                self.restore(&roots, window, cx);
                break;
            }
            visited.push(current);
            if window.modifiers().shift {
                window.focus_prev(cx);
            } else {
                window.focus_next(cx);
            }
        }
    }
}

pub(super) fn closing_boundary(
    visual: &Rc<RefCell<VisualState>>,
    id: u64,
) -> Option<FocusBoundary> {
    let boundary = visual.borrow().boundaries.get(&id).cloned()?;
    boundary.0.closed.set(true);
    Some(boundary)
}

pub(super) fn was_presented(visual: &Rc<RefCell<VisualState>>, id: u64) -> bool {
    visual
        .borrow()
        .measurements
        .get(&id)
        .is_some_and(|measurement| measurement.presented.get())
}

pub(super) fn interrupted_exit_duration(
    created: Instant,
    now: Instant,
    base: Duration,
    curve: crate::theme::CubicBezier,
) -> Duration {
    if base.is_zero() {
        return Duration::ZERO;
    }
    let elapsed = now.saturating_duration_since(created);
    if elapsed >= base {
        return base;
    }
    let factor =
        evaluate_cubic_bezier(curve, elapsed.as_secs_f64() / base.as_secs_f64()).clamp(0., 1.);
    if factor <= 0. {
        return Duration::ZERO;
    }
    if factor >= 1. {
        return base;
    }
    base.mul_f64(factor)
}

/// Mount one controller around window contents. The host preserves the child's layout.
#[derive(IntoElement)]
pub struct MessageHost {
    id: ElementId,
    state: Entity<MessageState>,
    child: AnyElement,
}

impl MessageHost {
    pub fn new(
        id: impl Into<ElementId>,
        state: &Entity<MessageState>,
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
    state: WeakEntity<MessageState>,
    visual: Rc<RefCell<VisualState>>,
    _release: Subscription,
}

impl RenderOnce for MessageHost {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        if !self.state.read(cx).window_matches(window) {
            return self.child;
        }
        let visual = self.state.read(cx).visual.clone();
        let state = self.state.clone();
        let mount_id: ElementId = (
            self.id.clone(),
            SharedString::from(format!("message-controller-{}", state.entity_id())),
        )
            .into();
        let mounted_visual = visual.clone();
        let mount = window.use_keyed_state(mount_id, cx, move |_, cx| Mount {
            id: cx.entity_id(),
            state: state.downgrade(),
            visual: mounted_visual,
            _release: cx.on_release(|mount: &mut Mount, cx| {
                let owns = {
                    let mut visual = mount.visual.borrow_mut();
                    let owns = visual.active_mount == Some(mount.id);
                    if owns {
                        *visual = VisualState::default();
                    }
                    owns
                };
                if owns {
                    super::state::unmount(&mount.state, cx);
                }
            }),
        });
        let id = mount.entity_id();
        let active = visual.borrow().active_mount;
        if active.is_some_and(|active| active != id) {
            return self.child;
        }
        if active.is_none() {
            visual.borrow_mut().active_mount = Some(id);
            self.state.update(cx, |state, cx| state.mount(window, cx));
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
    state: &mut MessageState,
    _: &mut Window,
    cx: &mut Context<MessageState>,
) -> impl IntoElement {
    if state.notices.is_empty() {
        state.layouts.clear();
        state.visual.borrow_mut().clear_content();
        return gpui::Empty.into_any_element();
    }
    let config = state.effective_config(cx);
    let snapshot = state.scope.snapshot(cx);
    let tokens = snapshot.theme.component::<MessageTheme>();
    let motion = MotionPolicy::new(
        snapshot.motion && tokens.globals().motion,
        cx.reduce_motion(),
    );
    Viewport {
        state: cx.entity(),
        visual: state.visual.clone(),
        notices: state.render_notices(cx),
        config,
        tokens,
        direction: snapshot.direction,
        motion,
        list_hovered: state.list_hovered,
    }
    .into_any_element()
}

struct Viewport {
    state: Entity<MessageState>,
    visual: Rc<RefCell<VisualState>>,
    notices: Vec<RenderNotice>,
    config: MessageConfig,
    tokens: ArcTokens,
    direction: Direction,
    motion: MotionPolicy,
    list_hovered: bool,
}

type ArcTokens = std::sync::Arc<MessageTokens>;

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
        Some("message-viewport".into())
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
        if self.notices.is_empty() {
            self.state.update(cx, |state, _| state.layouts.clear());
            let mut visual = self.visual.borrow_mut();
            visual.positions.clear();
            visual.boundaries.clear();
            visual.epoch = None;
            visual.expansion = None;
            visual.placeholder_opacity = None;
            visual.placeholder_width = None;
            visual.height = None;
            visual.stacked = None;
            visual.scroll = 0.;
            visual.max_scroll = 0.;
            return;
        }
        let context = MessageStyleContext {
            kind: None,
            direction: self.direction,
            motion: self.motion.allows_motion(),
            closing: false,
            stacked: false,
        };
        let mut styles = self.config.styles.clone();
        if let Some(resolver) = &self.config.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        let metrics = MessageStyle::from_tokens(&self.tokens, None);
        let length = |value: Option<gpui::DefiniteLength>, fallback: f32| {
            value.map_or(fallback, |value| {
                f32::from(value.to_pixels(window.viewport_size().width.into(), window.rem_size()))
            })
        };
        let vertical_padding = length(styles.list.padding.top, metrics.edge_margin as f32)
            + length(styles.list.padding.bottom, metrics.edge_margin as f32)
            + length(styles.list_content.padding.top, 0.)
            + length(styles.list_content.padding.bottom, 0.);
        let gap = styles
            .list_content
            .gap
            .height
            .map_or(metrics.gap as f32, |length| {
                f32::from(length.to_pixels(window.viewport_size().width.into(), window.rem_size()))
            })
            .max(0.);
        let viewport = window.viewport_size();
        let mut content = div()
            .id("message-list-content")
            .debug_selector(|| "message-list-content".into())
            .relative()
            .w_full()
            .gap(px(metrics.gap as f32));
        content.style().refine(&styles.list_content);
        content = content.child(NoticeList {
            state: self.state.clone(),
            visual: self.visual.clone(),
            notices: self.notices.clone(),
            config: self.config.clone(),
            tokens: self.tokens.clone(),
            direction: self.direction,
            motion: self.motion,
            list_hovered: self.list_hovered,
            gap,
            vertical_padding,
            cards: Vec::new(),
            placeholders: Vec::new(),
            bounds: Bounds::default(),
            regions: Vec::new(),
            animating: false,
        });
        let mut layer = crate::behavior::traversal(div())
            .id("message-list")
            .w(viewport.width)
            .h(viewport.height)
            .p(px(metrics.edge_margin as f32))
            .overflow_hidden()
            .child(content);
        layer.style().refine(&styles.list);
        let mut layer = layer.into_any_element();
        layer.layout_as_root(
            size(
                AvailableSpace::Definite(viewport.width),
                AvailableSpace::Definite(viewport.height),
            ),
            window,
            cx,
        );
        window.defer_draw(
            layer,
            point(px(0.), px(self.config.top - metrics.edge_margin as f32)),
            self.tokens.z_index_popup.saturating_mul(1 << 20),
            None,
        );
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
    element: AnyElement,
    measured: gpui::Size<Pixels>,
    y: f32,
    visible: bool,
    participates: bool,
    opacity: f32,
    bounds: Bounds<Pixels>,
    presented: Rc<Cell<bool>>,
    animate_root: bool,
    animate_indicator: bool,
}
struct Placeholder {
    element: AnyElement,
    measured: gpui::Size<Pixels>,
    y: f32,
}
struct NoticeList {
    state: Entity<MessageState>,
    visual: Rc<RefCell<VisualState>>,
    notices: Vec<RenderNotice>,
    config: MessageConfig,
    tokens: ArcTokens,
    direction: Direction,
    motion: MotionPolicy,
    list_hovered: bool,
    gap: f32,
    vertical_padding: f32,
    cards: Vec<Card>,
    placeholders: Vec<Placeholder>,
    bounds: Bounds<Pixels>,
    regions: Vec<Bounds<Pixels>>,
    animating: bool,
}

fn progress(
    start: Instant,
    now: Instant,
    duration: Duration,
    policy: MotionPolicy,
    curve: crate::theme::CubicBezier,
) -> f32 {
    if !policy.allows_motion() || duration.is_zero() {
        return 1.;
    }
    evaluate_cubic_bezier(
        curve,
        now.saturating_duration_since(start).as_secs_f64() / duration.as_secs_f64(),
    ) as f32
}

#[derive(Clone, Copy)]
struct NoticeMotion {
    opacity: f32,
    offset: f32,
    animating: bool,
}

fn notice_motion(
    notice: &RenderNotice,
    now: Instant,
    metrics: &MessageStyle,
    motion: MotionPolicy,
) -> NoticeMotion {
    // Closing reverses from the sampled entry state. Continuing to advance entry
    // during exit can brighten an interrupted notice or flash an unseen notice.
    let entry_time = notice.closing.map_or(now, |closed| closed.min(now));
    let entering = progress(
        notice.created,
        entry_time,
        metrics.motion_duration,
        motion,
        metrics.motion_curve,
    );
    let leaving = notice.closing.map_or(0., |closed| {
        progress(
            closed,
            now,
            notice.exit_duration,
            motion,
            metrics.motion_curve,
        )
    });
    NoticeMotion {
        opacity: entering * (1. - leaving),
        offset: -(metrics.motion_offset as f32) * ((1. - entering) + entering * leaving),
        animating: if notice.closing.is_some() {
            entering > 0. && leaving < 1.
        } else {
            entering < 1.
        },
    }
}

impl NoticeList {
    fn card(
        &self,
        notice: &RenderNotice,
        max_width: Pixels,
        presentation: CardPresentation,
        window: &mut Window,
        cx: &mut App,
    ) -> PreparedCard {
        let CardPresentation {
            opacity,
            stacked,
            now,
        } = presentation;
        let metrics = MessageStyle::from_tokens(&self.tokens, notice.options.kind);
        let context = MessageStyleContext {
            kind: notice.options.kind,
            direction: self.direction,
            motion: self.motion.allows_motion(),
            closing: notice.closing.is_some(),
            stacked,
        };
        let mut styles = self.config.styles.clone();
        if let Some(resolver) = &self.config.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        styles.refine(&notice.options.styles);
        if let Some(resolver) = &notice.options.styles_fn {
            styles.refine(&resolver.resolve(&context, window, cx));
        }
        let padding = |value: Option<gpui::DefiniteLength>, fallback: f64| {
            value.map_or(px(fallback as f32), |value| {
                value.to_pixels(max_width.into(), window.rem_size())
            })
        };
        let padding = Edges {
            top: padding(styles.root.padding.top, metrics.padding.top),
            right: padding(styles.root.padding.right, metrics.padding.right),
            bottom: padding(styles.root.padding.bottom, metrics.padding.bottom),
            left: padding(styles.root.padding.left, metrics.padding.left),
        };
        let border = Edges {
            top: styles
                .root
                .border_widths
                .top
                .map_or(px(0.), |value| value.to_pixels(window.rem_size())),
            right: styles
                .root
                .border_widths
                .right
                .map_or(px(0.), |value| value.to_pixels(window.rem_size())),
            bottom: styles
                .root
                .border_widths
                .bottom
                .map_or(px(0.), |value| value.to_pixels(window.rem_size())),
            left: styles
                .root
                .border_widths
                .left
                .map_or(px(0.), |value| value.to_pixels(window.rem_size())),
        };
        let measurement = self
            .visual
            .borrow_mut()
            .measurements
            .entry(notice.id)
            .or_default()
            .clone();
        if styles.root.display == Some(gpui::Display::None) {
            measurement.layout.set(None);
            return PreparedCard {
                element: gpui::Empty.into_any_element(),
                measurement,
                padding,
                border,
                style: styles.root,
                wrapper_style: Style::default(),
                max_width,
                displayed: false,
                participates: false,
                opacity: 0.,
                animate_root: false,
                animate_indicator: false,
            };
        }

        let boundary = if matches!(notice.options.content, MessageContent::Factory(_)) {
            let existing = self.visual.borrow().boundaries.get(&notice.id).cloned();
            let boundary = existing.unwrap_or_else(|| {
                let boundary = FocusBoundary::new(window, cx);
                self.visual
                    .borrow_mut()
                    .boundaries
                    .insert(notice.id, boundary.clone());
                boundary
            });
            if notice.closing.is_some() {
                boundary.0.closed.set(true);
            }
            Some(boundary)
        } else {
            self.visual.borrow_mut().boundaries.remove(&notice.id);
            None
        };
        let content = match &notice.options.content {
            MessageContent::Text(text) => {
                crate::behavior::text::normal_paragraph_text(text.clone(), self.direction)
            }
            MessageContent::Factory(factory) => factory(&notice.handle, window, cx),
        };
        if matches!(notice.options.content, MessageContent::Factory(_))
            && notice.handle.is_dismissed()
            && !self
                .state
                .read(cx)
                .notices
                .iter()
                .any(|current| current.id == notice.id)
        {
            // A reentrant content factory may synchronously dismiss its notice.
            // Do not paint the stale render snapshot after its lifetime ended.
            return PreparedCard {
                element: gpui::Empty.into_any_element(),
                measurement,
                padding,
                border,
                style: styles.root,
                wrapper_style: Style::default(),
                max_width,
                displayed: false,
                participates: false,
                opacity: 0.,
                animate_root: false,
                animate_indicator: false,
            };
        }
        let mut title = div()
            .id("title")
            .flex_shrink(1.)
            .text_color(metrics.title_color)
            .text_size(px(metrics.font_size as f32))
            .line_height(relative(metrics.line_height as f32))
            .child(content);
        title.style().refine(&styles.title);
        if styles.title.text.line_height.is_none() {
            let font = title
                .style()
                .text
                .font_size
                .map_or(px(metrics.font_size as f32), |value| {
                    value.to_pixels(window.rem_size())
                });
            title =
                title.line_height(px((f64::from(f32::from(font)) * metrics.line_height) as f32));
        }
        let mut wrapper = div()
            .id("wrapper")
            .when_some(boundary.as_ref(), |wrapper, boundary| {
                wrapper.track_focus(&boundary.0.root)
            })
            .flex()
            .items_center()
            .min_w(px(0.))
            .gap(px(metrics.icon_gap as f32))
            .when(self.direction == Direction::Rtl, |d| {
                d.flex_row_reverse().text_right()
            });
        wrapper.style().refine(&styles.wrapper);
        let mut root_style = Style::default();
        root_style.refine(&styles.root);
        let animate_root = root_style.display != gpui::Display::None
            && root_style.visibility == gpui::Visibility::Visible
            && root_style.opacity.unwrap_or(1.) > 0.;
        let mut animate_indicator = false;
        let reference = notice.options.icon.clone().or_else(|| {
            notice.options.kind.map(|kind| {
                match kind {
                    MessageType::Info => IconName::InfoCircleFilled,
                    MessageType::Success => IconName::CheckCircleFilled,
                    MessageType::Error => IconName::CloseCircleFilled,
                    MessageType::Warning => IconName::ExclamationCircleFilled,
                    MessageType::Loading => IconName::LoadingOutlined,
                }
                .icon()
            })
        });
        if notice.options.show_icon
            && let Some(reference) = reference
        {
            let mut slot = div()
                .id("icon")
                .flex()
                .flex_shrink_0()
                .items_center()
                .line_height(px(0.))
                .text_size(px(metrics.icon_size as f32))
                .text_color(metrics.icon_color);
            slot.style().refine(&styles.icon);
            let icon_size = slot
                .style()
                .text
                .font_size
                .map_or(px(metrics.icon_size as f32), |value| {
                    value.to_pixels(window.rem_size())
                });
            let mut indicator_style = Style::default();
            indicator_style.refine(slot.style());
            animate_indicator = animate_root
                && self.motion.allows_motion()
                && reference.key() == IconName::LoadingOutlined.icon().key()
                && icon_size > px(0.)
                && indicator_style.display != gpui::Display::None
                && indicator_style.visibility == gpui::Visibility::Visible
                && indicator_style.opacity.unwrap_or(1.) > 0.;
            let rotation = if animate_indicator {
                (now.saturating_duration_since(notice.created)
                    .as_secs_f64()
                    .fract()
                    * std::f64::consts::TAU) as f32
            } else {
                0.
            };
            wrapper = wrapper.child(slot.child(icon(reference).inherit_color().render_options(
                IconRenderOptions {
                    size: size(icon_size, icon_size),
                    colors: IconPalette::from_theme(self.tokens.globals()),
                    rotation: radians(rotation),
                    ..Default::default()
                },
            )));
        }
        wrapper = wrapper.child(title);
        let mut wrapper_style = Style::default();
        wrapper_style.refine(wrapper.style());
        let mut root = div()
            .id(("notice", notice.id))
            .relative()
            .max_w(max_width)
            .flex_shrink_0()
            .font(self.tokens.font().clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(relative(metrics.line_height as f32))
            .text_color(metrics.color)
            .bg(metrics.background)
            .rounded(px(metrics.radius as f32))
            .pt(px(metrics.padding.top as f32))
            .pr(px(metrics.padding.right as f32))
            .pb(px(metrics.padding.bottom as f32))
            .pl(px(metrics.padding.left as f32))
            .shadow(self.tokens.shadows().to_vec())
            .child(ContentProbe {
                child: wrapper.into_any_element(),
                layout: measurement.layout.clone(),
                offset: measurement.offset.clone(),
            });
        root.style().refine(&styles.root);
        let normalize = |value: Option<Length>, relative: Pixels| match value {
            Some(Length::Definite(gpui::DefiniteLength::Fraction(fraction))) => {
                Some(px(f32::from(relative) * fraction).max(px(0.)).into())
            }
            other => other,
        };
        let viewport_height = window.viewport_size().height;
        {
            let style = root.style();
            style.size.width = normalize(style.size.width, max_width);
            style.min_size.width = normalize(style.min_size.width, max_width);
            style.max_size.width = normalize(style.max_size.width, max_width);
            style.size.height = normalize(style.size.height, viewport_height);
            style.min_size.height = normalize(style.min_size.height, viewport_height);
            style.max_size.height = normalize(style.max_size.height, viewport_height);
            style.padding.top = Some(padding.top.into());
            style.padding.right = Some(padding.right.into());
            style.padding.bottom = Some(padding.bottom.into());
            style.padding.left = Some(padding.left.into());
        }
        if styles.root.text.line_height.is_none() {
            let font = root
                .style()
                .text
                .font_size
                .map_or(px(metrics.font_size as f32), |value| {
                    value.to_pixels(window.rem_size())
                });
            root = root.line_height(px((f64::from(f32::from(font)) * metrics.line_height) as f32));
        }
        // Motion multiplies an explicitly styled opacity instead of replacing it.
        let styled_opacity = root.style().opacity.unwrap_or(1.);
        root = root.opacity((styled_opacity * opacity).clamp(0., 1.));
        if notice.closing.is_none() {
            root = root.occlude();
            let weak = self.state.downgrade();
            let id = notice.id;
            root = root.on_hover(move |hovered, window, cx| {
                let _ = weak.update(cx, |state, cx| state.hover(id, *hovered, window, cx));
            });
            if let Some(callback) = notice.options.on_click.clone() {
                let handle = notice.handle.clone();
                root = root.on_click(move |event, window, cx| {
                    if !handle.is_dismissed() {
                        callback(event, window, cx);
                    }
                });
            }
        }
        let mut semantics = notice.options.accessibility.clone();
        semantics.hidden |= notice.closing.is_some();
        let element = LiveSurface {
            element: semantics.apply(root).into_element(),
            closing: notice.closing.is_some(),
            boundary,
            geometry: Some(measurement.geometry.clone()),
        }
        .into_any_element();
        let mut effective_style = Style::default();
        effective_style.refine(&styles.root);
        let participates = effective_style.display != gpui::Display::None;
        PreparedCard {
            element,
            measurement,
            padding,
            border,
            style: styles.root,
            wrapper_style,
            max_width,
            displayed: participates && effective_style.visibility == gpui::Visibility::Visible,
            participates,
            opacity: (styled_opacity * opacity).clamp(0., 1.),
            animate_root,
            animate_indicator,
        }
    }
}

impl IntoElement for NoticeList {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for NoticeList {
    type RequestLayoutState = ();
    type PrepaintState = ();
    fn id(&self) -> Option<ElementId> {
        Some("message-notices".into())
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
        let metrics = MessageStyle::from_tokens(&self.tokens, None);
        let max_width =
            (window.viewport_size().width - px((metrics.edge_margin * 2.) as f32)).max(px(0.));
        let now = cx.background_executor().now();
        let count = self
            .notices
            .iter()
            .filter(|notice| notice.closing.is_none())
            .count();
        let stacked = match self.config.stack {
            MessageStack::Disabled => false,
            MessageStack::Enabled { threshold } => count > threshold.max(1),
        };
        let target = if stacked && !self.list_hovered {
            0.
        } else {
            1.
        };
        let (expansion, expanding, elapsed) = {
            let mut visual = self.visual.borrow_mut();
            let epoch = *visual.epoch.get_or_insert(now);
            let elapsed = now.saturating_duration_since(epoch);
            let transition = visual
                .expansion
                .get_or_insert_with(|| ScalarTransition::new(1., metrics.motion_curve));
            transition.retarget(target, elapsed, metrics.motion_duration, self.motion);
            let sample = transition.sample(elapsed);
            (sample.value as f32, !sample.finished, elapsed)
        };
        self.animating = expanding;
        let mut offset = 0.;
        let mut active_index = 0usize;
        self.cards.clear();
        for notice in &self.notices {
            let motion = notice_motion(notice, now, &metrics, self.motion);
            let visible = !stacked || active_index == 0 || expansion > 0.;
            let alpha = motion.opacity
                * if stacked && active_index > 0 {
                    expansion
                } else {
                    1.
                };
            let prepared = self.card(
                notice,
                max_width,
                CardPresentation {
                    opacity: alpha,
                    stacked,
                    now,
                },
                window,
                cx,
            );
            let visible = visible && prepared.displayed;
            let participates = prepared.participates;
            let opacity = prepared.opacity;
            let animate_root = prepared.animate_root;
            let animate_indicator = prepared.animate_indicator;
            let presented = prepared.measurement.presented.clone();
            let (element, measured) = prepared.measure(window, cx);
            let (y, moving) = {
                let mut visual = self.visual.borrow_mut();
                let transition = visual
                    .positions
                    .entry(notice.id)
                    .or_insert_with(|| ScalarTransition::new(offset as f64, metrics.motion_curve));
                if notice.closing.is_none() {
                    transition.retarget(
                        offset as f64,
                        elapsed,
                        metrics.motion_duration,
                        self.motion,
                    );
                }
                let sample = transition.sample(elapsed);
                (sample.value as f32 * expansion, !sample.finished)
            };
            self.animating |= animate_root && visible && (moving || motion.animating);
            self.cards.push(Card {
                notice: notice.clone(),
                element,
                measured,
                y: y + motion.offset,
                visible,
                participates,
                opacity,
                bounds: Bounds::default(),
                presented,
                animate_root,
                animate_indicator,
            });
            if notice.closing.is_none() && participates {
                offset += f32::from(measured.height).round() + self.gap;
                active_index += 1;
            }
        }
        self.placeholders.clear();
        if let Some(first) = self
            .cards
            .iter()
            .find(|card| card.notice.closing.is_none() && card.participates)
        {
            let placeholder = self.visual.borrow_mut().sample_placeholder(
                f32::from(first.measured.width).round(),
                stacked && !self.list_hovered,
                elapsed,
                &self.tokens,
                self.motion,
            );
            self.animating |= placeholder.animating;
            if placeholder.opacity > 0. {
                for index in (1..=2).rev() {
                    let width = (px(placeholder.width)
                        - px((self.tokens.globals().margin * index as f64) as f32))
                    .max(px(0.));
                    let height = px((self.tokens.globals().margin_xs * 2.) as f32);
                    let mut element = div()
                        .id(("stack-placeholder", index as usize))
                        .w(width)
                        .h(height)
                        .bg(metrics.background)
                        .rounded(px(metrics.radius as f32))
                        .shadow(self.tokens.placeholder_shadows().to_vec())
                        .opacity(placeholder.opacity)
                        .into_any_element();
                    let measured = element.layout_as_root(
                        size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
                        window,
                        cx,
                    );
                    self.placeholders.push(Placeholder {
                        element,
                        measured,
                        y: f32::from(first.measured.height).round()
                            - (2 - index) as f32 * self.tokens.globals().margin_xs as f32
                            + f32::from(height) * (1. - placeholder.opacity),
                    });
                }
            }
        }
        let expanded_height = (offset - self.gap).max(0.);
        let collapsed_height = self
            .cards
            .iter()
            .find(|card| card.notice.closing.is_none() && card.participates)
            .map_or(0., |card| f32::from(card.measured.height).round())
            + (self.tokens.globals().margin_xs * 2.) as f32;
        let target_height = if stacked && !self.list_hovered {
            collapsed_height
        } else {
            expanded_height
        };
        let mut visual = self.visual.borrow_mut();
        let collapsed = stacked && !self.list_hovered;
        let snap = visual.stacked.is_some_and(|previous| previous != collapsed);
        visual.stacked = Some(collapsed);
        let (height, changing_height) =
            visual.sample_height(target_height, snap, elapsed, &self.tokens, self.motion);
        self.animating |= changing_height;
        self.animating &= self.cards.iter().any(|card| card.animate_root);
        let retained: HashSet<_> = self.notices.iter().map(|notice| notice.id).collect();
        visual.positions.retain(|id, _| retained.contains(id));
        visual.measurements.retain(|id, _| retained.contains(id));
        visual.boundaries.retain(|id, _| retained.contains(id));
        visual.max_scroll =
            (height + self.vertical_padding - f32::from(window.viewport_size().height)).max(0.);
        visual.scroll = visual.scroll.clamp(0., visual.max_scroll);
        (
            window.request_layout(
                Style {
                    size: size(relative(1.).into(), px(height).into()),
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
        self.bounds = window
            .content_mask()
            .bounds
            .intersect(&Bounds::new(Point::default(), window.viewport_size()));
        self.regions.clear();
        let scroll = self.visual.borrow().scroll;
        let top = bounds.top() - px(scroll);
        for placeholder in &mut self.placeholders {
            let origin = point(
                bounds.center().x - placeholder.measured.width / 2.,
                top + px(placeholder.y),
            );
            placeholder.element.prepaint_at(origin, window, cx);
        }
        let mut layouts = Vec::new();
        for card in self.cards.iter_mut().rev() {
            if !card.visible {
                continue;
            }
            let origin = point(
                bounds.center().x - card.measured.width / 2.,
                top + px(card.y),
            );
            let card_bounds = Bounds::new(origin, card.measured);
            card.bounds = card_bounds;
            self.animating |= card.animate_indicator
                && card.notice.closing.is_none()
                && !card_bounds.intersect(&self.bounds).is_empty();
            card.element.prepaint_at(origin, window, cx);
            layouts.push(MessageLayout {
                id: card.notice.id,
                key: card.notice.key.clone(),
                bounds: card_bounds,
                closing: card.notice.closing.is_some(),
            });
            if card.notice.closing.is_none() {
                self.regions.push(Bounds::from_corners(
                    origin - point(px(0.), px(self.tokens.globals().margin as f32)),
                    card_bounds.bottom_right(),
                ));
            }
        }
        layouts.reverse();
        self.state.update(cx, |state, _| state.layouts = layouts);
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
        for placeholder in &mut self.placeholders {
            placeholder.element.paint(window, cx);
        }
        let shadow_outset = self
            .tokens
            .shadows()
            .iter()
            .filter(|shadow| !shadow.inset)
            .map(|shadow| {
                shadow.blur_radius * 3.
                    + shadow.spread_radius.max(px(0.))
                    + shadow.offset.x.abs().max(shadow.offset.y.abs())
            })
            .fold(px(0.), Pixels::max);
        for card in self.cards.iter_mut().rev().filter(|card| card.visible) {
            card.element.paint(window, cx);
            if card.opacity > 0.
                && !card
                    .bounds
                    .dilate(shadow_outset)
                    .intersect(&self.bounds)
                    .is_empty()
            {
                card.presented.set(true);
            }
        }
        let pointer = window.mouse_position();
        let hovered = self.bounds.contains(&pointer)
            && self.regions.iter().any(|region| region.contains(&pointer));
        if hovered != self.list_hovered {
            let weak = self.state.downgrade();
            window.defer(cx, move |window, cx| {
                let _ = weak.update(cx, |state, cx| state.list_hover(hovered, window, cx));
            });
        }
        let regions = self.regions.clone();
        let weak = self.state.downgrade();
        let bounds = self.bounds;
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase.capture() {
                let hovered = bounds.contains(&event.position)
                    && regions
                        .iter()
                        .any(|region| region.contains(&event.position));
                let _ = weak.update(cx, |state, cx| state.list_hover(hovered, window, cx));
            }
        });
        let regions = self.regions.clone();
        let weak = self.state.downgrade();
        let visual = self.visual.clone();
        let line_height =
            px((self.tokens.globals().font_size * self.tokens.globals().line_height) as f32);
        window.on_mouse_event(move |event: &ScrollEvent, phase, _, cx| {
            if phase.capture()
                && bounds.contains(&event.position)
                && regions
                    .iter()
                    .any(|region| region.contains(&event.position))
            {
                let (moved, contains_scroll) = {
                    let mut visual = visual.borrow_mut();
                    let old = visual.scroll;
                    visual.scroll = (old - f32::from(event.delta.pixel_delta(line_height).y))
                        .clamp(0., visual.max_scroll);
                    (visual.scroll != old, visual.max_scroll > 0.)
                };
                if moved {
                    let _ = weak.update(cx, |_, cx| cx.notify());
                }
                if contains_scroll {
                    cx.stop_propagation();
                }
            }
        });
        if self.animating
            && window.is_visible()
            && !self
                .bounds
                .intersect(&window.content_mask().bounds)
                .is_empty()
        {
            self.state
                .update(cx, |state, cx| state.schedule_frame(window, cx));
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn message_delayed_height_motion_matches_browser_fractional_samples() {
        let theme = crate::theme::ResolvedTheme::default();
        let tokens = theme.component::<MessageTheme>();
        let policy = MotionPolicy::new(true, false);
        let mut visual = VisualState::default();
        visual.sample_height(96., false, Duration::ZERO, &tokens, policy);
        for (millis, expected) in [
            (0, 96.),
            (200, 96.),
            (350, 91.109375),
            (500, 67.046875),
            (650, 43.84375),
            (800, 40.),
        ] {
            let (actual, animating) =
                visual.sample_height(40., false, Duration::from_millis(millis), &tokens, policy);
            assert!(
                (actual - expected).abs() < 0.02,
                "t={millis}: {actual} != {expected}"
            );
            assert_eq!(animating, millis < 800);
        }
    }

    #[test]
    fn message_placeholder_opacity_finishes_at_100ms_and_width_at_300ms() {
        let theme = crate::theme::ResolvedTheme::default();
        let tokens = theme.component::<MessageTheme>();
        let policy = MotionPolicy::new(true, false);
        let mut visual = VisualState::default();
        visual.sample_placeholder(100., false, Duration::ZERO, &tokens, policy);
        let start = visual.sample_placeholder(200., true, Duration::ZERO, &tokens, policy);
        assert_eq!(start.opacity, 0.);
        assert_eq!(start.width, 100.);
        let fast =
            visual.sample_placeholder(200., true, Duration::from_millis(100), &tokens, policy);
        assert_eq!(fast.opacity, 1.);
        assert!(fast.width > 100. && fast.width < 200.);
        assert!(fast.animating);
        let slow =
            visual.sample_placeholder(200., true, Duration::from_millis(300), &tokens, policy);
        assert_eq!(slow.opacity, 1.);
        assert_eq!(slow.width, 200.);
        assert!(!slow.animating);
        visual.sample_placeholder(200., false, Duration::from_millis(300), &tokens, policy);
        let half =
            visual.sample_placeholder(200., false, Duration::from_millis(350), &tokens, policy);
        let expected = 1. - evaluate_cubic_bezier(tokens.globals().motion_ease_in_out, 0.5) as f32;
        assert!((half.opacity - expected).abs() < 1e-6);
        let hidden =
            visual.sample_placeholder(200., false, Duration::from_millis(400), &tokens, policy);
        assert_eq!(hidden.opacity, 0.);
        assert!(!hidden.animating);
    }
}
