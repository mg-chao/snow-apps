// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    CarouselChange, CarouselEffect, CarouselEvent, CarouselItems, CarouselOptions, CarouselState,
    CarouselStyleContext, CarouselStyles, CarouselTheme, DotPlacement,
    element::{SlideFrame, Viewport, ViewportBounds},
    state::EventHandler,
};
use crate::{
    ConfigScope, Direction,
    behavior::{
        Accessibility, ScalarTransition,
        notice::{FocusBoundary, LiveSurface},
    },
    theme::{CubicBezier, ResolvedTheme},
};
use gpui::{
    AnyElement, App, ElementId, Entity, FocusHandle, IntoElement, Refineable, RenderOnce, Role,
    SharedString, StyleRefinement, Styled, Subscription, WeakEntity, Window, div, prelude::*, px,
};
use std::{
    cell::{Cell, RefCell},
    collections::{HashMap, HashSet},
    rc::Rc,
    sync::Arc,
    time::Duration,
};

pub(super) const EASE: CubicBezier = CubicBezier::new(0.25, 0.1, 0.25, 1.);

#[derive(Clone, Copy, Debug)]
pub struct CarouselArrowContext {
    pub previous: bool,
    pub current: Option<usize>,
    pub count: usize,
    pub vertical: bool,
    pub direction: Direction,
}
#[derive(Clone, Copy, Debug)]
pub struct CarouselDotContext {
    pub index: usize,
    pub active: bool,
}
pub(super) type ArrowRender =
    Rc<dyn Fn(&CarouselArrowContext, &mut Window, &mut App) -> AnyElement>;
pub(super) type DotRender = Rc<dyn Fn(&CarouselDotContext, &mut Window, &mut App) -> AnyElement>;
type ChangeHandler = Rc<dyn Fn(&CarouselChange, &mut Window, &mut App)>;
type StylesFn = Rc<dyn Fn(&CarouselStyleContext, &mut Window, &mut App) -> CarouselStyles>;

#[derive(IntoElement)]
pub struct Carousel {
    id: ElementId,
    state: Entity<CarouselState>,
    scope: Option<ConfigScope>,
    options: CarouselOptions,
    styles: CarouselStyles,
    style: StyleRefinement,
    styles_fn: Option<StylesFn>,
    accessibility: Accessibility,
    on_event: Option<EventHandler>,
    before_change: Option<ChangeHandler>,
    after_change: Option<ChangeHandler>,
    previous_arrow: Option<ArrowRender>,
    next_arrow: Option<ArrowRender>,
    dot_render: Option<DotRender>,
}
impl Carousel {
    pub fn new(id: impl Into<ElementId>, state: &Entity<CarouselState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            options: CarouselOptions::default(),
            styles: CarouselStyles::default(),
            style: StyleRefinement::default(),
            styles_fn: None,
            accessibility: Accessibility::default(),
            on_event: None,
            before_change: None,
            after_change: None,
            previous_arrow: None,
            next_arrow: None,
            dot_render: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn options(mut self, options: CarouselOptions) -> Self {
        self.options = options;
        self
    }
    pub fn effect(mut self, effect: CarouselEffect) -> Self {
        self.options.effect = effect;
        self
    }
    pub fn fade(self, fade: bool) -> Self {
        self.effect(if fade {
            CarouselEffect::Fade
        } else {
            CarouselEffect::Scroll
        })
    }
    pub fn dot_placement(mut self, placement: DotPlacement) -> Self {
        self.options.dot_placement = placement;
        self
    }
    pub fn vertical(mut self, vertical: bool) -> Self {
        self.options.vertical = Some(vertical);
        self
    }
    pub fn dots(mut self, value: bool) -> Self {
        self.options.dots = value;
        self
    }
    pub fn arrows(mut self, value: bool) -> Self {
        self.options.arrows = value;
        self
    }
    pub fn draggable(mut self, value: bool) -> Self {
        self.options.draggable = value;
        self
    }
    pub fn infinite(mut self, value: bool) -> Self {
        self.options.infinite = value;
        self
    }
    pub fn adaptive_height(mut self, value: bool) -> Self {
        self.options.adaptive_height = value;
        self
    }
    pub fn initial_slide(mut self, value: usize) -> Self {
        self.options.initial_slide = value;
        self
    }
    pub fn autoplay(mut self, value: bool) -> Self {
        self.options.autoplay = value;
        self
    }
    pub fn autoplay_speed(mut self, value: Duration) -> Self {
        self.options.autoplay_speed = value;
        self
    }
    pub fn dot_duration(mut self, value: bool) -> Self {
        self.options.dot_duration = value;
        self
    }
    pub fn speed(mut self, value: Duration) -> Self {
        self.options.speed = value;
        self
    }
    pub fn easing(mut self, value: CubicBezier) -> Self {
        self.options.easing = value;
        self
    }
    pub fn wait_for_animate(mut self, value: bool) -> Self {
        self.options.wait_for_animate = value;
        self
    }
    pub fn styles(mut self, value: CarouselStyles) -> Self {
        self.styles = value;
        self
    }
    pub fn styles_with(
        mut self,
        resolver: impl Fn(&CarouselStyleContext, &mut Window, &mut App) -> CarouselStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(resolver));
        self
    }
    pub fn accessibility(mut self, value: Accessibility) -> Self {
        self.accessibility = value;
        self
    }
    pub fn on_event(
        mut self,
        callback: impl Fn(&CarouselEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_event = Some(Rc::new(callback));
        self
    }
    pub fn before_change(
        mut self,
        callback: impl Fn(&CarouselChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.before_change = Some(Rc::new(callback));
        self
    }
    pub fn after_change(
        mut self,
        callback: impl Fn(&CarouselChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.after_change = Some(Rc::new(callback));
        self
    }
    pub fn prev_arrow(
        mut self,
        render: impl Fn(&CarouselArrowContext, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.previous_arrow = Some(Rc::new(render));
        self
    }
    pub fn next_arrow(
        mut self,
        render: impl Fn(&CarouselArrowContext, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.next_arrow = Some(Rc::new(render));
        self
    }
    pub fn dot_render(
        mut self,
        render: impl Fn(&CarouselDotContext, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.dot_render = Some(Rc::new(render));
        self
    }
}
impl Styled for Carousel {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

pub(super) struct ControlState {
    pub focus: FocusHandle,
    pub hovered: bool,
    pub length: ScalarTransition,
    pub opacity: ScalarTransition,
}
impl ControlState {
    pub fn new(length: f64, opacity: f64, cx: &mut App) -> Self {
        Self {
            focus: cx.focus_handle().tab_index(0),
            hovered: false,
            length: ScalarTransition::new(length, EASE),
            opacity: ScalarTransition::new(opacity, EASE),
        }
    }
}
pub(super) struct Presentation {
    pub dots: HashMap<usize, ControlState>,
    pub arrows: [ControlState; 2],
    pub boundaries: HashMap<SharedString, FocusBoundary>,
    heights: HashMap<SharedString, HeightMeasurement>,
    pending_heights: Vec<SharedString>,
    maximum_height: Option<(SharedString, f32)>,
    all_heights_measured: bool,
    items: CarouselItems,
    active_boundary: Option<FocusBoundary>,
    revisions: HashMap<SharedString, u64>,
    revision_version: u64,
    generation: u64,
    theme: Option<Arc<ResolvedTheme>>,
    measurement_styles: Option<[StyleRefinement; 3]>,
    pub viewport: ViewportBounds,
}
struct HeightMeasurement {
    bounds: Rc<Cell<Option<f32>>>,
    value: Option<f32>,
}
impl Presentation {
    fn new(cx: &mut App) -> Self {
        Self {
            dots: HashMap::new(),
            arrows: [
                ControlState::new(0., 0.4, cx),
                ControlState::new(0., 0.4, cx),
            ],
            boundaries: HashMap::new(),
            heights: HashMap::new(),
            pending_heights: Vec::new(),
            maximum_height: None,
            all_heights_measured: false,
            items: CarouselItems::default(),
            active_boundary: None,
            revisions: HashMap::new(),
            revision_version: u64::MAX,
            generation: u64::MAX,
            theme: None,
            measurement_styles: None,
            viewport: Rc::default(),
        }
    }

    fn clear_heights(&mut self) {
        self.heights.clear();
        self.pending_heights.clear();
        self.maximum_height = None;
        self.all_heights_measured = false;
    }

    fn invalidate_height(&mut self, key: &SharedString) {
        self.heights.remove(key);
        self.pending_heights.retain(|pending| pending != key);
        self.all_heights_measured = false;
        if self
            .maximum_height
            .as_ref()
            .is_some_and(|(maximum, _)| maximum == key)
        {
            self.maximum_height = self
                .heights
                .iter()
                .filter_map(|(key, measurement)| {
                    measurement.value.map(|height| (key.clone(), height))
                })
                .max_by(|(_, left), (_, right)| left.total_cmp(right));
        }
    }

    // Only cells requested by the previous measuring layout are inspected. A
    // settled carousel does not walk its slide collection during animation.
    fn collect_heights(&mut self) {
        let pending = std::mem::take(&mut self.pending_heights);
        for key in pending {
            let Some(measurement) = self.heights.get_mut(&key) else {
                continue;
            };
            if let Some(height) = measurement.bounds.get() {
                let height = if height.is_finite() {
                    height.max(0.)
                } else {
                    0.
                };
                measurement.value = Some(height);
                if self
                    .maximum_height
                    .as_ref()
                    .is_none_or(|(_, maximum)| height >= *maximum)
                {
                    self.maximum_height = Some((key, height));
                }
            } else {
                self.pending_heights.push(key);
            }
        }
        self.all_heights_measured =
            self.heights.len() == self.items.len() && self.pending_heights.is_empty();
    }

    fn measured_height(&self, key: &SharedString) -> Option<f32> {
        self.heights
            .get(key)
            .and_then(|measurement| measurement.value)
    }

    fn measurement(&mut self, key: &SharedString) -> Rc<Cell<Option<f32>>> {
        if let Some(measurement) = self.heights.get(key) {
            return measurement.bounds.clone();
        }
        let bounds = Rc::new(Cell::new(None));
        self.heights.insert(
            key.clone(),
            HeightMeasurement {
                bounds: bounds.clone(),
                value: None,
            },
        );
        self.pending_heights.push(key.clone());
        bounds
    }
}
struct Mount {
    state: WeakEntity<CarouselState>,
    generation: u64,
    presentation: Rc<RefCell<Presentation>>,
    _observer: Subscription,
    _release: Subscription,
    _focus_in: Subscription,
    _focus_out: Subscription,
}

impl RenderOnce for Carousel {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<CarouselTheme>();
        let state = self.state.clone();
        let observed = state.clone();
        let mount = window.with_element_namespace(self.id.clone(), |window| {
            window.use_keyed_state(
                ("carousel-state", state.entity_id()),
                cx,
                move |window, cx| {
                    let generation = observed.update(cx, |s, _| s.mount());
                    let focus = observed.read(cx).focus.clone();
                    let target = observed.downgrade();
                    let blur = target.clone();
                    Mount {
                        state: observed.downgrade(),
                        generation,
                        presentation: Rc::new(RefCell::new(Presentation::new(cx))),
                        _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                        _release: cx.on_release(|m: &mut Mount, cx| {
                            let _ = m.state.update(cx, |s, _| s.unmount(m.generation));
                        }),
                        _focus_in: window.on_focus_in(&focus, cx, move |window, cx| {
                            let _ = target.update(cx, |s, cx| s.set_focused(true, window, cx));
                        }),
                        _focus_out: window.on_focus_out(&focus, cx, move |_, window, cx| {
                            let _ = blur.update(cx, |s, cx| s.set_focused(false, window, cx));
                        }),
                    }
                },
            )
        });
        let presentation = mount.read(cx).presentation.clone();
        let on_event = self.on_event.clone();
        let before_change = self.before_change.clone();
        let after_change = self.after_change.clone();
        let boundaries = presentation.clone();
        let handler: EventHandler = Rc::new(move |event, window, cx| {
            if let CarouselEvent::BeforeChange(_) = event {
                // Close previous-frame focus/pointer boundaries synchronously.
                // Repaint opens only the new active slide.
                if let Some(boundary) = boundaries.borrow().active_boundary.clone() {
                    boundary.0.closed.set(true);
                    boundary.dismiss(window, cx);
                }
            }
            if let Some(handler) = &on_event {
                handler(event, window, cx);
            }
            match event {
                CarouselEvent::BeforeChange(change) => {
                    if let Some(callback) = &before_change {
                        callback(change, window, cx);
                    }
                }
                CarouselEvent::AfterChange(change) => {
                    if let Some(callback) = &after_change {
                        callback(change, window, cx);
                    }
                }
            }
        });
        state.update(cx, |s, cx| {
            s.configure(
                self.options.clone(),
                scope.clone(),
                Some(handler),
                window,
                cx,
            )
        });
        let (sample, current, count, now, generation) = state.update(cx, |s, cx| {
            s.dot_bounds.clear();
            s.previous_arrow_bounds = None;
            s.next_arrow_bounds = None;
            (
                s.sample(cx),
                s.current_index(),
                s.items().len(),
                s.now(cx),
                s.measurement_generation,
            )
        });
        let vertical = self.options.is_vertical();
        let direction = if vertical {
            Direction::Ltr
        } else {
            config.direction
        };
        let style_context = CarouselStyleContext {
            current,
            count,
            vertical,
            direction,
            autoplay: self.options.autoplay,
            paused: !state.read(cx).autoplay_running(),
        };
        let mut styles = config.carousel.styles.clone();
        if let Some(resolver) = &config.carousel.styles_fn {
            styles.refine(&resolver.resolve(&style_context, window, cx));
        }
        styles.refine(&self.styles);
        if let Some(resolver) = &self.styles_fn {
            styles.refine(&resolver(&style_context, window, cx));
        }
        styles.root.refine(&self.style);

        let items = state.read(cx).items().clone();
        let entries = items.entries();
        let mut visible: HashMap<usize, (f64, f64, usize)> = HashMap::new();
        if self.options.effect == CarouselEffect::Fade {
            for (order, &(index, alpha)) in sample.fade_layers.iter().enumerate() {
                if alpha > 0. {
                    visible.insert(index, (0., alpha, order));
                }
            }
        } else if count > 0 {
            let position = sample.position;
            let start = position.floor() as i64;
            let end = position.ceil() as i64;
            for slot in start..=end {
                if self.options.infinite || (slot >= 0 && slot < count as i64) {
                    let index = slot.rem_euclid(count as i64) as usize;
                    visible.insert(
                        index,
                        (
                            (slot as f64 - position) * self.options.axis_direction(direction),
                            1.,
                            index,
                        ),
                    );
                }
            }
        }
        if visible.is_empty()
            && let Some(index) = current
        {
            visible.insert(index, (0., 1., 0));
        }

        let fixed_height = styles
            .root
            .size
            .height
            .is_some_and(|height| height != gpui::Length::Auto);
        let mut p = presentation.borrow_mut();
        if !p.items.same(&items) {
            let old_items = p.items.clone();
            p.dots.retain(|i, _| *i < count);
            p.boundaries.retain(|key, boundary| {
                let same_content = match (
                    old_items.get(key).and_then(|item| item.factory.as_ref()),
                    items.get(key).and_then(|item| item.factory.as_ref()),
                ) {
                    (Some(old), Some(new)) => Rc::ptr_eq(old, new),
                    (None, None) => items.index(key).is_some(),
                    _ => false,
                };
                if same_content {
                    true
                } else {
                    boundary.0.closed.set(true);
                    boundary.dismiss(window, cx);
                    false
                }
            });
            p.revisions.retain(|key, _| items.index(key).is_some());
            p.items = items.clone();
            p.clear_heights();
        }
        let invalidate_cached_views = p.generation != generation
            || p.theme
                .as_ref()
                .is_none_or(|old| !Arc::ptr_eq(old, &config.theme))
            || p.measurement_styles.as_ref().is_none_or(|old| {
                old[0] != styles.root || old[1] != styles.viewport || old[2] != styles.slide
            });
        if invalidate_cached_views {
            p.clear_heights();
            p.generation = generation;
            p.theme = Some(config.theme.clone());
            p.measurement_styles = Some([
                styles.root.clone(),
                styles.viewport.clone(),
                styles.slide.clone(),
            ]);
        }
        if p.revision_version != state.read(cx).measurement_revision_version {
            for (key, revision) in &state.read(cx).measurement_revisions {
                if p.revisions.get(key) != Some(revision) {
                    p.invalidate_height(key);
                    p.revisions.insert(key.clone(), *revision);
                }
            }
            p.revision_version = state.read(cx).measurement_revision_version;
        }
        p.collect_heights();
        let measure_indices: Vec<usize> = if fixed_height {
            Vec::new()
        } else if self.options.adaptive_height {
            current
                .filter(|i| p.measured_height(&entries[*i].key).is_none())
                .into_iter()
                .collect()
        } else if vertical {
            (count > 0 && p.measured_height(&entries[0].key).is_none())
                .then_some(0)
                .into_iter()
                .collect()
        } else if p.all_heights_measured {
            Vec::new()
        } else {
            entries
                .iter()
                .enumerate()
                .filter_map(|(index, entry)| {
                    p.measured_height(&entry.key).is_none().then_some(index)
                })
                .collect()
        };
        let needs_measure = !measure_indices.is_empty();
        let height = if self.options.adaptive_height {
            current
                .and_then(|i| p.measured_height(&entries[i].key))
                .unwrap_or(0.)
        } else if vertical {
            entries
                .first()
                .and_then(|entry| p.measured_height(&entry.key))
                .unwrap_or(0.)
        } else {
            p.maximum_height.as_ref().map_or(0., |(_, height)| *height)
        };
        let measuring: HashSet<usize> = measure_indices.iter().copied().collect();
        let viewport_bounds = p.viewport.clone();
        let mut viewport = div()
            .relative()
            .grid()
            .grid_cols(1)
            .items_start()
            .w_full()
            .min_w_0()
            .overflow_hidden();
        if fixed_height {
            viewport = viewport.h_full();
        } else if !needs_measure {
            viewport = viewport.h(px(height));
        } else {
            viewport = viewport.min_h(px(height));
        }
        viewport.style().refine(&styles.viewport);
        let mut indices: Vec<usize> = visible.keys().copied().collect();
        indices.extend(
            measure_indices
                .iter()
                .copied()
                .filter(|index| !visible.contains_key(index)),
        );
        // Preserve the sampled back-to-front composition across fade retargeting.
        indices.sort_by_key(|i| {
            visible
                .get(i)
                .map_or((false, 0), |(_, _, order)| (true, *order))
        });
        p.active_boundary = None;
        for index in indices {
            let key = &entries[index].key;
            let boundary = p
                .boundaries
                .entry(key.clone())
                .or_insert_with(|| FocusBoundary::new(window, cx))
                .clone();
            let active = current == Some(index);
            if active {
                p.active_boundary = Some(boundary.clone());
            }
            boundary.0.closed.set(!active);
            if !active {
                boundary.dismiss(window, cx);
            }
            drop(p);
            let content = CarouselState::ensure_view(&state, index, window, cx);
            p = presentation.borrow_mut();
            let Some(content) = content else {
                let measurement = p.measurement(key);
                measurement.set(Some(0.));
                continue;
            };
            let measured = measuring.contains(&index);
            // GPUI's subtree cache key excludes inherited paint opacity.
            // A live fade must rebuild its primitives at the sampled alpha.
            let content = if measured
                || invalidate_cached_views
                // Pinned GPUI's view cache replays visual/input state, but not
                // semantic nodes or accessibility action listeners. Rebuild
                // visible content while AT is active, including warm frames.
                || window.is_a11y_active()
                || styles.slide != StyleRefinement::default()
                || (self.options.effect == CarouselEffect::Fade && sample.animating)
            {
                content.into_any_element()
            } else if fixed_height {
                content
                    .cached(StyleRefinement::default().w_full().h_full())
                    .into_any_element()
            } else if let Some(height) = p.measured_height(key) {
                content
                    .cached(StyleRefinement::default().w_full().h(px(height)))
                    .into_any_element()
            } else {
                content.into_any_element()
            };
            let mut slide = div()
                .id((ElementId::Name("slide".into()), key.clone()))
                .track_focus(&boundary.0.root)
                .w_full()
                .min_w_0();
            if fixed_height {
                slide = slide.h_full();
            }
            slide.style().refine(&styles.slide);
            let (displacement, opacity, _) = visible.get(&index).copied().unwrap_or((0., 1., 0));
            slide = slide.opacity(opacity as f32).child(content);
            let label = entries[index].label.clone().unwrap_or_else(|| {
                config
                    .locale
                    .carousel
                    .slide_label
                    .replace("{index}", &(index + 1).to_string())
                    .replace("{count}", &count.to_string())
                    .into()
            });
            let element = LiveSurface {
                element: Accessibility {
                    role: Some(Role::Group),
                    label: Some(label),
                    hidden: !active,
                    ..Default::default()
                }
                .apply(slide),
                closing: !active,
                boundary: Some(boundary),
                geometry: None,
            };
            let measurement = measured.then(|| p.measurement(key));
            let frame = SlideFrame {
                child: element.into_any_element(),
                viewport: viewport_bounds.clone(),
                displacement,
                vertical,
                visible: visible.contains_key(&index),
                measured: measurement,
            };
            let mut wrapper = div().w_full().min_w_0().child(frame);
            if measured {
                wrapper = wrapper.col_start(1).row_start(1).self_start();
            } else {
                wrapper = wrapper.absolute().inset_0();
            }
            viewport = viewport.child(wrapper);
        }
        drop(p);
        let controls = super::controls::render_controls(
            &state,
            &presentation,
            &self.options,
            &tokens,
            &styles,
            self.previous_arrow,
            self.next_arrow,
            self.dot_render,
            current,
            now,
            sample.autoplay_progress,
            window,
            cx,
        );
        let mut root = div()
            .id(self.id)
            .relative()
            .w_full()
            .min_w_0()
            .track_focus(&state.read(cx).focus)
            .child(Viewport {
                child: viewport.into_element(),
                state: state.clone(),
                bounds: viewport_bounds,
                controls_animating: controls.animating,
                repair_layout: false,
            });
        root.style().refine(&styles.root);
        let hover_state = state.clone();
        root = root.on_hover(move |hovered, window, cx| {
            hover_state.update(cx, |s, cx| s.set_hovered(*hovered, window, cx))
        });
        let pointer_state = state.clone();
        root = root.on_mouse_down(gpui::MouseButton::Left, move |event, window, cx| {
            pointer_state.update(cx, |s, cx| {
                s.pointer_start(event.position, window, cx);
            });
        });
        root = root.children(controls.elements);
        let key_state = state.clone();
        let key_presentation = presentation.clone();
        root = root.on_key_down(move |event, window, cx| {
            let focused = window.focused(cx);
            let s = key_state.read(cx);
            let dot_focused = focused.as_ref().is_some_and(|f| {
                key_presentation
                    .borrow()
                    .dots
                    .values()
                    .any(|d| d.focus == *f)
            });
            let own = focused.as_ref().is_some_and(|f| {
                *f == s.focus
                    || key_presentation
                        .borrow()
                        .arrows
                        .iter()
                        .any(|a| a.focus == *f)
                    || key_presentation
                        .borrow()
                        .dots
                        .values()
                        .any(|d| d.focus == *f)
            });
            if !own
                || event.keystroke.modifiers.control
                || event.keystroke.modifiers.alt
                || event.keystroke.modifiers.platform
            {
                return;
            }
            let step = match event.keystroke.key.as_str() {
                "left" if !vertical => Some(if direction == Direction::Rtl { 1 } else { -1 }),
                "right" if !vertical => Some(if direction == Direction::Rtl { -1 } else { 1 }),
                "up" if vertical => Some(-1),
                "down" if vertical => Some(1),
                _ => None,
            };
            if let Some(step) = step {
                key_state.update(cx, |s, cx| {
                    s.navigate(step, super::CarouselSource::Keyboard, window, cx)
                });
                if dot_focused {
                    let next = key_state.read(cx).current_index().and_then(|index| {
                        key_presentation
                            .borrow()
                            .dots
                            .get(&index)
                            .map(|dot| dot.focus.clone())
                    });
                    if let Some(focus) = next {
                        focus.focus(window, cx);
                    }
                }
                cx.stop_propagation();
                window.prevent_default();
            }
        });
        let mut accessibility = self.accessibility;
        accessibility.disabled |= config.disabled;
        accessibility.role.get_or_insert(Role::Region);
        accessibility
            .label
            .get_or_insert_with(|| config.locale.carousel.root_label.clone());
        accessibility.apply(root)
    }
}
