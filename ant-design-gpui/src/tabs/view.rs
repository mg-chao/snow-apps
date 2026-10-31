// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    TabItem, TabPlacement, TabsAnimation, TabsChange, TabsEdit, TabsEvent, TabsSource, TabsState,
    TabsTheme, TabsTokens, TabsType, measure::measure, model::ContentRenderer, state::EventHandler,
    style::TabsSpacing,
};
use crate::{
    ConfigScope, Direction, Size,
    behavior::{Accessibility, FocusRing, MotionPolicy},
    icons::{IconName, icon},
    input::motion::CSS_EASE,
    popup::{Popup, PopupHandle, PopupPlacement, PopupTriggers},
};
use gpui::{
    AnyElement, App, Bounds, ElementId, Entity, IntoElement, MouseButton, Refineable, RenderOnce,
    Role, SharedString, StyleRefinement, Styled, Subscription, WeakEntity, Window, accesskit,
    canvas, div, fill, point, prelude::*, px, size as gpui_size,
};
use std::{rc::Rc, sync::Arc};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TabsIndicatorAlign {
    Start,
    #[default]
    Center,
    End,
}

#[derive(Clone, Default)]
pub struct TabsIndicator {
    pub align: TabsIndicatorAlign,
    /// Receives the natural tab extent on the current axis, in logical pixels.
    pub size: Option<Rc<dyn Fn(f32) -> f32>>,
}
impl TabsIndicator {
    pub fn fixed(size: f32) -> Self {
        Self {
            size: Some(Rc::new(move |_| size)),
            ..Self::default()
        }
    }
    pub fn with_size(size: impl Fn(f32) -> f32 + 'static) -> Self {
        Self {
            size: Some(Rc::new(size)),
            ..Self::default()
        }
    }
    pub fn align(mut self, align: TabsIndicatorAlign) -> Self {
        self.align = align;
        self
    }
}

#[derive(Clone, Default)]
pub struct TabsStyles {
    pub root: StyleRefinement,
    pub header: StyleRefinement,
    pub item: StyleRefinement,
    pub remove: StyleRefinement,
    pub indicator: StyleRefinement,
    pub body: StyleRefinement,
    pub content: StyleRefinement,
    pub popup: StyleRefinement,
}
impl TabsStyles {
    fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.header.refine(&other.header);
        self.item.refine(&other.item);
        self.remove.refine(&other.remove);
        self.indicator.refine(&other.indicator);
        self.body.refine(&other.body);
        self.content.refine(&other.content);
        self.popup.refine(&other.popup);
    }
}
#[derive(Clone, Copy, Debug)]
pub struct TabsStyleContext {
    pub kind: TabsType,
    pub placement: TabPlacement,
    pub size: Size,
    pub direction: Direction,
    pub disabled: bool,
}

#[derive(Clone)]
pub struct TabsTabRender {
    pub item: TabItem,
    pub active: bool,
    state: WeakEntity<TabsState>,
}
impl TabsTabRender {
    pub fn activate(&self, window: &mut Window, cx: &mut App) {
        let _ = self.state.update(cx, |s, cx| {
            s.activate(&self.item.key, TabsSource::Pointer, window, cx)
        });
    }
    pub fn close(&self, window: &mut Window, cx: &mut App) {
        let _ = self.state.update(cx, |s, cx| {
            s.edit(
                TabsEdit::Remove(self.item.key.clone()),
                TabsSource::Pointer,
                window,
                cx,
            )
        });
    }
}
#[derive(Clone)]
pub struct TabsPopupRender {
    pub rest_tabs: Vec<TabItem>,
    pub active_key: Option<SharedString>,
    pub handle: PopupHandle,
    state: WeakEntity<TabsState>,
}
impl TabsPopupRender {
    pub fn select(&self, key: &str, window: &mut Window, cx: &mut App) {
        let enabled = self
            .state
            .update(cx, |s, cx| {
                if !s.enabled(key) {
                    return false;
                }
                s.activate(key, TabsSource::Overflow, window, cx);
                s.focus_tab(key, window, cx);
                true
            })
            .unwrap_or(false);
        if enabled {
            self.handle.close(window, cx);
        }
    }
    pub fn close(&self, window: &mut Window, cx: &mut App) {
        self.handle.close(window, cx);
    }
}

type StyleResolver = Rc<dyn Fn(&TabsStyleContext, &mut Window, &mut App) -> TabsStyles>;
type HeaderRenderer =
    Rc<dyn Fn(AnyElement, &Entity<TabsState>, &mut Window, &mut App) -> AnyElement>;
type TabRenderer = Rc<dyn Fn(AnyElement, &TabsTabRender, &mut Window, &mut App) -> AnyElement>;
type PopupRenderer = Rc<dyn Fn(AnyElement, &TabsPopupRender, &mut Window, &mut App) -> AnyElement>;
type ChangeHandler = Rc<dyn Fn(&TabsChange, &mut Window, &mut App)>;
type EditHandler = Rc<dyn Fn(&TabsEdit, &mut Window, &mut App)>;

#[derive(IntoElement)]
pub struct Tabs {
    id: ElementId,
    state: Entity<TabsState>,
    scope: Option<ConfigScope>,
    kind: TabsType,
    placement: TabPlacement,
    size: Option<Size>,
    disabled: Option<bool>,
    active: Option<Option<SharedString>>,
    default_active: Option<SharedString>,
    centered: bool,
    gutter: Option<f32>,
    hide_add: bool,
    destroy_on_hidden: bool,
    animation: TabsAnimation,
    indicator: TabsIndicator,
    styles: TabsStyles,
    styles_fn: Option<StyleResolver>,
    extra_start: Option<AnyElement>,
    extra_end: Option<AnyElement>,
    add_icon: Option<ContentRenderer>,
    remove_icon: Option<ContentRenderer>,
    more_icon: Option<ContentRenderer>,
    more_triggers: PopupTriggers,
    more_placement: Option<PopupPlacement>,
    popup_focus: Option<gpui::FocusHandle>,
    header_render: Option<HeaderRenderer>,
    tab_render: Option<TabRenderer>,
    popup_render: Option<PopupRenderer>,
    accessibility: Accessibility,
    on_event: Option<EventHandler>,
    on_change: Option<ChangeHandler>,
    on_edit: Option<EditHandler>,
}
impl Tabs {
    pub fn new(id: impl Into<ElementId>, state: &Entity<TabsState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            kind: TabsType::Line,
            placement: TabPlacement::Top,
            size: None,
            disabled: None,
            active: None,
            default_active: None,
            centered: false,
            gutter: None,
            hide_add: false,
            destroy_on_hidden: false,
            animation: TabsAnimation::default(),
            indicator: TabsIndicator::default(),
            styles: TabsStyles::default(),
            styles_fn: None,
            extra_start: None,
            extra_end: None,
            add_icon: None,
            remove_icon: None,
            more_icon: None,
            more_triggers: PopupTriggers::HOVER,
            more_placement: None,
            popup_focus: None,
            header_render: None,
            tab_render: None,
            popup_render: None,
            accessibility: Accessibility::default(),
            on_event: None,
            on_change: None,
            on_edit: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn kind(mut self, kind: TabsType) -> Self {
        self.kind = kind;
        self
    }
    pub fn placement(mut self, placement: TabPlacement) -> Self {
        self.placement = placement;
        self
    }
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }
    pub fn disabled(mut self, value: bool) -> Self {
        self.disabled = Some(value);
        self
    }
    pub fn active_key(mut self, key: impl Into<SharedString>) -> Self {
        self.active = Some(Some(key.into()));
        self
    }
    pub fn selected(mut self, key: Option<SharedString>) -> Self {
        self.active = Some(key);
        self
    }
    pub fn default_active_key(mut self, key: impl Into<SharedString>) -> Self {
        self.default_active = Some(key.into());
        self
    }
    pub fn centered(mut self, value: bool) -> Self {
        self.centered = value;
        self
    }
    pub fn gutter(mut self, value: f32) -> Self {
        self.gutter = Some(if value.is_finite() { value.max(0.) } else { 0. });
        self
    }
    pub fn hide_add(mut self, value: bool) -> Self {
        self.hide_add = value;
        self
    }
    pub fn destroy_on_hidden(mut self, value: bool) -> Self {
        self.destroy_on_hidden = value;
        self
    }
    pub fn animated(mut self, value: impl Into<TabsAnimation>) -> Self {
        self.animation = value.into();
        self
    }
    pub fn indicator(mut self, value: TabsIndicator) -> Self {
        self.indicator = value;
        self
    }
    pub fn styles(mut self, value: TabsStyles) -> Self {
        self.styles = value;
        self
    }
    pub fn styles_fn(
        mut self,
        value: impl Fn(&TabsStyleContext, &mut Window, &mut App) -> TabsStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(value));
        self
    }
    pub fn extra_start(mut self, value: impl IntoElement) -> Self {
        self.extra_start = Some(value.into_any_element());
        self
    }
    pub fn extra_end(mut self, value: impl IntoElement) -> Self {
        self.extra_end = Some(value.into_any_element());
        self
    }
    pub fn add_icon(
        mut self,
        value: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.add_icon = Some(Rc::new(value));
        self
    }
    pub fn remove_icon(
        mut self,
        value: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.remove_icon = Some(Rc::new(value));
        self
    }
    pub fn more_icon(
        mut self,
        value: impl Fn(&mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.more_icon = Some(Rc::new(value));
        self
    }
    pub fn more_triggers(mut self, value: PopupTriggers) -> Self {
        self.more_triggers = value;
        self
    }
    pub fn more_placement(mut self, value: PopupPlacement) -> Self {
        self.more_placement = Some(value);
        self
    }
    /// Keyboard opening focuses this handle in custom popup content.
    pub fn popup_focus(mut self, value: &gpui::FocusHandle) -> Self {
        self.popup_focus = Some(value.clone());
        self
    }
    pub fn header_render(
        mut self,
        value: impl Fn(AnyElement, &Entity<TabsState>, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.header_render = Some(Rc::new(value));
        self
    }
    pub fn tab_render(
        mut self,
        value: impl Fn(AnyElement, &TabsTabRender, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.tab_render = Some(Rc::new(value));
        self
    }
    pub fn popup_render(
        mut self,
        value: impl Fn(AnyElement, &TabsPopupRender, &mut Window, &mut App) -> AnyElement + 'static,
    ) -> Self {
        self.popup_render = Some(Rc::new(value));
        self
    }
    pub fn accessibility_label(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(value.into());
        self
    }
    pub fn automation_id(mut self, value: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(value.into());
        self
    }
    pub fn on_event(mut self, value: impl Fn(&TabsEvent, &mut Window, &mut App) + 'static) -> Self {
        self.on_event = Some(Rc::new(value));
        self
    }
    pub fn on_change(
        mut self,
        value: impl Fn(&TabsChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Rc::new(value));
        self
    }
    pub fn on_edit(mut self, value: impl Fn(&TabsEdit, &mut Window, &mut App) + 'static) -> Self {
        self.on_edit = Some(Rc::new(value));
        self
    }
}
impl Styled for Tabs {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.styles.root
    }
}

struct Mount {
    state: WeakEntity<TabsState>,
    generation: u64,
    _observer: Subscription,
    _release: Subscription,
}

fn padding<E: Styled>(element: E, p: TabsSpacing) -> E {
    element
        .pt(px(p.top as f32))
        .pr(px(p.right as f32))
        .pb(px(p.bottom as f32))
        .pl(px(p.left as f32))
}

impl RenderOnce for Tabs {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<TabsTheme>();
        let g = &tokens.globals;
        let vertical = self.placement.vertical();
        let trailing = self.placement.trailing(config.direction);
        let card = self.kind != TabsType::Line;
        let size = self.size.unwrap_or(config.size);
        let disabled = config.availability(self.disabled, false).disabled;
        let style_context = TabsStyleContext {
            kind: self.kind,
            placement: self.placement,
            size,
            direction: config.direction,
            disabled,
        };
        if let Some(resolver) = &self.styles_fn {
            self.styles.refine(&resolver(&style_context, window, cx));
        }
        let state = self.state.clone();
        let observed = state.clone();
        let _mount = window.with_element_namespace(self.id.clone(), |window| {
            window.use_keyed_state(("tabs-state", state.entity_id()), cx, move |_, cx| {
                let generation = observed.update(cx, |s, _| {
                    s.mounted = true;
                    s.generation = s.generation.wrapping_add(1);
                    s.frame_pending = false;
                    s.generation
                });
                Mount {
                    state: observed.downgrade(),
                    generation,
                    _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                    _release: cx.on_release(|m: &mut Mount, cx| {
                        let _ = m.state.update(cx, |s, _| {
                            if s.generation == m.generation {
                                s.mounted = false;
                                s.generation = s.generation.wrapping_add(1);
                                s.frame_pending = false;
                                for item in s.items.values_mut() {
                                    item.pressed = false;
                                    item.hovered = false;
                                }
                            }
                        });
                    }),
                }
            })
        });
        let on_event = self.on_event.clone();
        let on_change = self.on_change.clone();
        let on_edit = self.on_edit.clone();
        state.update(cx, |s, cx| {
            s.configure_scope(scope.clone(), window, cx);
            if s.placement != self.placement || s.direction != config.direction {
                s.offset = 0.;
                s.reveal_key = s.model.active.clone();
                s.indicator_initialized = false;
            }
            s.placement = self.placement;
            s.direction = config.direction;
            s.kind = self.kind;
            s.disabled = disabled;
            s.hide_add = self.hide_add;
            s.animation = self.animation;
            s.destroy_on_hidden = self.destroy_on_hidden;
            if !s.model.initialized {
                if let Some(key) = &self.default_active {
                    s.model.active = Some(key.clone());
                    s.reveal_key = Some(key.clone());
                }
                s.model.initialized = true;
            }
            s.model.controlled = self.active.is_some();
            if let Some(active) = &self.active
                && s.model.active != *active
            {
                s.model.active = active.clone();
                s.reveal_key = active.clone();
            }
            s.on_event = Some(Rc::new(move |event, window, cx| {
                if let Some(handler) = &on_event {
                    handler(event, window, cx);
                }
                if let TabsEvent::Change(change) = event
                    && let Some(handler) = &on_change
                {
                    handler(change, window, cx);
                }
                if let TabsEvent::Edit { action, .. } = event
                    && let Some(handler) = &on_edit
                {
                    handler(action, window, cx);
                }
            }));
            s.sync_tab_stops();
            s.sync_panes(g.motion_duration_slow, config.motion_policy(cx), window, cx);
        });
        let items = state.read(cx).model.items.clone();
        let active = state.read(cx).model.active.clone();
        let offset = state.read(cx).offset;
        let overflow = state.read(cx).overflow;
        let mut list = div()
            .relative()
            .flex()
            .flex_none()
            .items_stretch()
            .when(vertical, |d| d.flex_col().items_stretch())
            .when(!vertical && config.direction == Direction::Rtl, |d| {
                d.flex_row_reverse()
            });
        list = if vertical {
            list.top(px(-offset))
        } else {
            list.left(px(-offset * config.direction.forward_sign()))
        };
        let gap = self.gutter.unwrap_or(if card {
            tokens.card_gutter as f32
        } else if vertical {
            tokens.vertical_item_margin.top as f32
        } else {
            tokens.horizontal_item_gutter as f32
        });
        list = list.gap(px(gap));
        // The reference RTL line header includes a trailing item margin in
        // its measured strip. Preserve that space when centering and scrolling.
        if !vertical && !card && config.direction == Direction::Rtl && self.gutter.is_none() {
            list = list.pl(px(tokens.horizontal_item_gutter as f32));
        }
        for item in items.entries() {
            let node = self.tab(item, &tokens, size, &config, window, cx);
            list = list.child(node);
        }
        if self.kind == TabsType::EditableCard && !self.hide_add && !overflow {
            list = list.child(self.add(&tokens, &config, window, cx));
        }
        let target = state.clone();
        let mut viewport = div()
            .id("nav-viewport")
            .relative()
            .flex()
            .flex_1()
            .min_w_0()
            .min_h_0()
            .overflow_hidden()
            .items_stretch()
            .when(vertical, |d| d.flex_col())
            .when(!vertical && config.direction == Direction::Rtl, |d| {
                d.justify_end()
            })
            .when(self.centered && !overflow, |d| d.justify_center())
            .on_scroll(move |event, window, cx| {
                let delta = event.delta.pixel_delta(px(20.));
                let amount = if vertical {
                    -f32::from(delta.y)
                } else {
                    -f32::from(if delta.x != px(0.) { delta.x } else { delta.y })
                };
                let moved = target.update(cx, |s, cx| s.scroll(amount, window, cx));
                if moved {
                    cx.stop_propagation();
                    window.prevent_default();
                }
            })
            .child(measure(list, state.read(cx).list_bounds.clone()));
        let measured = state.clone();
        let indicator = self.indicator.clone();
        let paint_tokens = tokens.clone();
        let indicator_style = self.styles.indicator.clone();
        viewport = viewport.child(
            canvas(
                move |bounds, window, cx| {
                    measured.update(cx, |s, cx| {
                        s.measure(window, cx);
                        if card {
                            return None;
                        }
                        let span = s
                            .model
                            .active
                            .as_ref()
                            .and_then(|key| s.spans.get(key))
                            .copied()?;
                        let requested = indicator
                            .size
                            .as_ref()
                            .map_or(span.length, |size| size(span.length));
                        let length = if requested.is_finite() {
                            requested.max(0.)
                        } else {
                            span.length
                        };
                        let align = match indicator.align {
                            TabsIndicatorAlign::Start => 0.,
                            TabsIndicatorAlign::Center => 0.5,
                            TabsIndicatorAlign::End => 1.,
                        };
                        let start = span.start + (span.length - length) * align;
                        let now = s.now(cx);
                        let motion = s.scope.as_ref().unwrap().snapshot(cx).motion_policy(cx);
                        let policy = MotionPolicy::new(
                            motion.allows_motion()
                                && s.animation.indicator
                                && s.indicator_initialized,
                            false,
                        );
                        s.indicator_start.retarget(
                            start as f64,
                            now,
                            paint_tokens.globals.motion_duration_slow,
                            policy,
                        );
                        s.indicator_length.retarget(
                            length as f64,
                            now,
                            paint_tokens.globals.motion_duration_slow,
                            policy,
                        );
                        s.indicator_initialized = true;
                        let position = s.indicator_start.sample(now);
                        let length = s.indicator_length.sample(now);
                        if !position.finished || !length.finished {
                            s.request_frame(window, cx);
                        }
                        // Spans belong to the measured list. Its origin includes
                        // centering, RTL alignment and the offset actually laid
                        // out this frame, which can precede a pending reveal.
                        let list = s.list_bounds.get()?;
                        let origin = if vertical {
                            list.top() - bounds.top()
                        } else if s.direction == Direction::Rtl {
                            bounds.right() - list.right()
                        } else {
                            list.left() - bounds.left()
                        };
                        let coordinate = f32::from(origin) + position.value as f32;
                        let extent = length.value as f32;
                        let mut style = gpui::Style {
                            background: Some(paint_tokens.ink_bar_color.to_hsla().into()),
                            ..Default::default()
                        };
                        style.refine(&indicator_style);
                        let thickness = match if vertical {
                            style.size.width
                        } else {
                            style.size.height
                        } {
                            gpui::Length::Definite(gpui::DefiniteLength::Absolute(value)) => {
                                f32::from(value.to_pixels(window.rem_size()))
                            }
                            _ => paint_tokens.globals.line_width_bold as f32,
                        }
                        .max(0.);
                        let rect = if vertical {
                            Bounds::new(
                                point(
                                    if trailing {
                                        bounds.left()
                                    } else {
                                        bounds.right() - px(thickness)
                                    },
                                    bounds.top() + px(coordinate),
                                ),
                                gpui_size(px(thickness), px(extent)),
                            )
                        } else {
                            Bounds::new(
                                point(
                                    if s.direction == Direction::Rtl {
                                        bounds.right() - px(coordinate + extent)
                                    } else {
                                        bounds.left() + px(coordinate)
                                    },
                                    if trailing {
                                        bounds.top()
                                    } else {
                                        bounds.bottom() - px(thickness)
                                    },
                                ),
                                gpui_size(px(extent), px(thickness)),
                            )
                        };
                        Some((rect, style))
                    })
                },
                |_, data, window, cx| {
                    if let Some((bounds, style)) = data {
                        style.paint(bounds, window, cx, |_, _| {});
                    }
                },
            )
            .absolute()
            .size_full(),
        );
        let shadow_state = state.clone();
        let shadow_tokens = tokens.clone();
        viewport = viewport.child(
            canvas(
                |bounds, _, _| bounds,
                move |_, bounds, window, cx| {
                    let s = shadow_state.read(cx);
                    let g = &shadow_tokens.globals;
                    let remaining = (s.total - s.viewport - s.offset).max(0.);
                    let (before, after) = if !vertical && s.direction == Direction::Rtl {
                        (remaining, s.offset)
                    } else {
                        (s.offset, remaining)
                    };
                    for (at_start, distance) in [(true, before), (false, after)] {
                        if distance <= 0.5 {
                            continue;
                        }
                        let extent = px(g.control_height as f32);
                        let rect = if vertical {
                            Bounds::new(
                                point(
                                    bounds.left(),
                                    if at_start {
                                        bounds.top()
                                    } else {
                                        bounds.bottom() - extent
                                    },
                                ),
                                gpui_size(bounds.size.width, extent),
                            )
                        } else {
                            Bounds::new(
                                point(
                                    if at_start {
                                        bounds.left()
                                    } else {
                                        bounds.right() - extent
                                    },
                                    bounds.top(),
                                ),
                                gpui_size(extent, bounds.size.height),
                            )
                        };
                        let shadows = match (vertical, at_start) {
                            (true, true) => &g.box_shadow_tabs_overflow_top,
                            (true, false) => &g.box_shadow_tabs_overflow_bottom,
                            (false, true) => &g.box_shadow_tabs_overflow_left,
                            (false, false) => &g.box_shadow_tabs_overflow_right,
                        }
                        .iter()
                        .map(|shadow| gpui::BoxShadow {
                            inset: shadow.inset,
                            color: shadow.color.to_hsla(),
                            offset: point(px(shadow.x as f32), px(shadow.y as f32)),
                            blur_radius: px(shadow.blur as f32),
                            spread_radius: px(shadow.spread as f32),
                        })
                        .collect::<Vec<_>>();
                        window.paint_inset_shadows(rect, Default::default(), &shadows);
                    }
                },
            )
            .absolute()
            .size_full(),
        );
        let mut header = div()
            .id("header")
            .relative()
            .flex()
            .flex_none()
            .min_w_0()
            .min_h_0()
            .items_stretch()
            .role(Role::TabList)
            .aria_orientation(if vertical {
                accesskit::Orientation::Vertical
            } else {
                accesskit::Orientation::Horizontal
            })
            .when(vertical, |d| d.flex_col().max_h_full())
            .when(!vertical && config.direction == Direction::Rtl, |d| {
                d.flex_row_reverse()
            });
        let border = px(g.line_width as f32);
        let border_color = if vertical {
            g.color_border
        } else {
            g.color_border_secondary
        }
        .to_hsla();
        header = header.child(
            canvas(
                |bounds, _, _| bounds,
                move |_, bounds, window, _| {
                    let line = if vertical {
                        Bounds::new(
                            point(
                                if trailing {
                                    bounds.left()
                                } else {
                                    bounds.right() - border
                                },
                                bounds.top(),
                            ),
                            gpui_size(border, bounds.size.height),
                        )
                    } else {
                        Bounds::new(
                            point(
                                bounds.left(),
                                if trailing {
                                    bounds.top()
                                } else {
                                    bounds.bottom() - border
                                },
                            ),
                            gpui_size(bounds.size.width, border),
                        )
                    };
                    window.paint_quad(fill(line, border_color));
                },
            )
            .absolute()
            .size_full(),
        );
        if let Some(extra) = self.extra_start.take() {
            header = header.child(div().flex_none().child(extra));
        }
        header = header.child(measure(viewport, state.read(cx).viewport_bounds.clone()));
        let more_extent = if vertical {
            tokens.card_padding.top + tokens.card_padding.bottom + g.font_height
        } else {
            tokens.card_padding.left + tokens.card_padding.right + g.font_size
        } as f32;
        state.update(cx, |s, _| {
            s.reserved_more = if overflow { more_extent } else { 0. }
        });
        if overflow {
            header = header.child(self.more(&scope, &tokens, more_extent, window, cx));
        }
        if self.kind == TabsType::EditableCard && !self.hide_add && overflow {
            header = header.child(self.add(&tokens, &config, window, cx));
        }
        if let Some(extra) = self.extra_end.take() {
            header = header.child(div().flex_none().child(extra));
        }
        header = if vertical {
            header.min_w(px((g.control_height * 1.25) as f32))
        } else {
            header
                .when(!trailing, |d| {
                    d.mb(px(tokens.horizontal_margin.bottom as f32))
                })
                .when(trailing, |d| d.mt(px(g.margin as f32)))
        };
        header.style().refine(&self.styles.header);
        let header = if let Some(render) = &self.header_render {
            render(header.into_any_element(), &state, window, cx)
        } else {
            header.into_any_element()
        };
        let mut body = div().id("body").relative().min_w_0().min_h_0().flex_1();
        body.style().refine(&self.styles.body);
        let now = state.read(cx).now(cx);
        for item in items
            .entries()
            .iter()
            .filter(|item| active.as_ref() != Some(&item.key))
            .chain(
                items
                    .entries()
                    .iter()
                    .filter(|item| active.as_ref() == Some(&item.key)),
            )
        {
            // Paint the active pane last so exiting content cannot cover it.
            let selected = active.as_ref() == Some(&item.key);
            let Some(pane) = state.read(cx).panes.get(&item.key) else {
                continue;
            };
            let alpha = pane.opacity.sample(now);
            if !selected && alpha.finished {
                continue;
            }
            let mut content = div()
                .id(item.key.clone())
                .relative()
                .w_full()
                .opacity(alpha.value as f32)
                .track_focus(&pane.focus.clone().tab_stop(selected))
                .when(!selected, |d| {
                    d.absolute()
                        .top_0()
                        .left_0()
                        .capture_any_mouse_down(|_, _, cx| cx.stop_propagation())
                        .capture_any_mouse_up(|_, _, cx| cx.stop_propagation())
                        .capture_key_down(|_, _, cx| cx.stop_propagation())
                })
                .when(selected, |d| d.occlude())
                .when(vertical && !trailing, |d| d.pl(px(g.padding_lg as f32)))
                .when(vertical && trailing, |d| d.pr(px(g.padding_lg as f32)))
                .child(pane.view.clone());
            content.style().refine(&self.styles.content);
            let mut semantics = Accessibility::new(Role::TabPanel).label(item.label.clone());
            semantics.hidden = !selected;
            body = body.child(semantics.apply(content));
        }
        let mut root = div()
            .id(self.id.clone())
            .relative()
            .flex()
            .min_w_0()
            .min_h_0()
            .font(g.font_family.to_gpui_font())
            .text_size(px(g.font_size as f32))
            .line_height(gpui::relative(g.line_height as f32))
            .text_color(g.color_text.to_hsla())
            .when(!vertical, |d| d.flex_col())
            .when(config.direction == Direction::Rtl, |d| d.text_right());
        if trailing {
            root = root.child(body).child(header);
        } else {
            root = root.child(header).child(body);
        }
        root.style().refine(&self.styles.root);
        self.accessibility.apply(root)
    }
}

impl Tabs {
    fn tab(
        &self,
        item: &TabItem,
        tokens: &Arc<TabsTokens>,
        size: Size,
        config: &crate::ResolvedConfig,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let state = &self.state;
        let g = &tokens.globals;
        let selected = state.read(cx).active_key() == Some(&item.key);
        let disabled = !state.read(cx).enabled(&item.key);
        let vertical = self.placement.vertical();
        let card = self.kind != TabsType::Line;
        let key = item.key.clone();
        let focus = state.read(cx).items[&key].focus.clone();
        let bounds = state.read(cx).items[&key].bounds.clone();
        let surface_bounds = state.read(cx).items[&key].surface_bounds.clone();
        let target = state.clone();
        let click_key = key.clone();
        let keyboard = state.clone();
        let hover_state = state.clone();
        let hover_key = key.clone();
        let down_state = state.clone();
        let down_key = key.clone();
        let up_state = state.clone();
        let up_key = key.clone();
        let release_state = state.clone();
        let release_key = key.clone();
        let a11y_state = state.clone();
        let a11y_key = key.clone();
        let (color, background) = state.update(cx, |s, cx| {
            let now = s.now(cx);
            let motion = config.motion_policy(cx);
            let item = s.items.get_mut(&key).unwrap();
            let color = tokens.label_color(selected, disabled, item.hovered, item.pressed);
            let (color, moving) = item.color.resolve(
                color.to_hsla(),
                now,
                g.motion_duration_slow,
                CSS_EASE,
                motion,
            );
            let bg = if !card {
                crate::theme::Color::TRANSPARENT
            } else if selected {
                g.color_bg_container
            } else {
                tokens.card_bg
            };
            let (background, background_moving) = item.background.resolve(
                bg.to_hsla(),
                now,
                g.motion_duration_slow,
                g.motion_ease_in_out,
                motion,
            );
            if moving || background_moving {
                s.request_frame(window, cx);
            }
            (color, background)
        });
        let mut node = padding(
            div()
                .id(key.clone())
                .relative()
                .flex()
                .flex_none()
                .items_center()
                .whitespace_nowrap()
                // Upstream's RTL rule leaves the first vertical line tab's leading
                // margin in place; later siblings replace it with vertical spacing.
                .when(
                    vertical
                        && !card
                        && config.direction == Direction::Rtl
                        && state.read(cx).items().index(&key) == Some(0),
                    |d| d.ml(px(tokens.horizontal_item_gutter as f32)),
                )
                .track_focus(&focus)
                .role(Role::Tab)
                .aria_selected(selected)
                .text_size(px(tokens.font_size(size) as f32))
                .text_color(color)
                .bg(background)
                .line_height(px(tokens.line_height(size) as f32))
                .when(disabled, |d| d.cursor_not_allowed())
                .when(!disabled, |d| d.cursor_pointer())
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse())
                .on_hover(move |hovered, _, cx| {
                    hover_state.update(cx, |s, cx| {
                        if let Some(item) = s.items.get_mut(&hover_key) {
                            item.hovered = *hovered;
                            item.pressed &= *hovered;
                            cx.notify();
                        }
                    });
                })
                .on_mouse_down(MouseButton::Left, move |_, window, cx| {
                    down_state.update(cx, |s, cx| {
                        if s.enabled(&down_key) {
                            s.focus_tab(&down_key, window, cx);
                            if let Some(item) = s.items.get_mut(&down_key) {
                                item.pressed = true;
                            }
                            cx.notify();
                        }
                    });
                })
                .on_mouse_up(MouseButton::Left, move |_, _, cx| {
                    up_state.update(cx, |s, cx| {
                        if let Some(item) = s.items.get_mut(&up_key) {
                            item.pressed = false;
                            cx.notify();
                        }
                    });
                })
                .on_mouse_up_out(MouseButton::Left, move |_, _, cx| {
                    release_state.update(cx, |s, cx| {
                        if let Some(item) = s.items.get_mut(&release_key)
                            && item.pressed
                        {
                            item.pressed = false;
                            cx.notify();
                        }
                    });
                })
                .on_click(move |_, window, cx| {
                    target.update(cx, |s, cx| {
                        s.activate(&click_key, TabsSource::Pointer, window, cx)
                    });
                })
                .on_key_down(move |event, window, cx| {
                    if keyboard.update(cx, |s, cx| s.key(&event.keystroke.key, window, cx)) {
                        window.prevent_default();
                        cx.stop_propagation();
                    }
                })
                .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
                    a11y_state.update(cx, |s, cx| {
                        s.activate(&a11y_key, TabsSource::Accessibility, window, cx)
                    });
                }),
            tokens.padding(size, card, vertical),
        );
        if card {
            node = node
                .border(px(g.line_width as f32))
                .border_color(g.color_border_secondary.to_hsla());
            let radius = px(tokens.card_radius(size) as f32);
            node = match self.placement {
                TabPlacement::Top => node.rounded_t(radius),
                TabPlacement::Bottom => node.rounded_b(radius),
                _ if self.placement.trailing(config.direction) => node.rounded_r(radius),
                _ => node.rounded_l(radius),
            };
            if selected {
                let trailing = self.placement.trailing(config.direction);
                let width = px(g.line_width as f32);
                let color = g.color_bg_container.to_hsla();
                let seam_bounds = surface_bounds.clone();
                node = node.child(
                    canvas(
                        move |_, _, _| seam_bounds.get(),
                        move |_, bounds, window, _| {
                            let Some(bounds) = bounds else {
                                return;
                            };
                            let seam = if vertical {
                                Bounds::new(
                                    point(
                                        if trailing {
                                            bounds.left()
                                        } else {
                                            bounds.right() - width
                                        },
                                        bounds.top(),
                                    ),
                                    gpui_size(width, bounds.size.height),
                                )
                            } else {
                                Bounds::new(
                                    point(
                                        bounds.left(),
                                        if trailing {
                                            bounds.top()
                                        } else {
                                            bounds.bottom() - width
                                        },
                                    ),
                                    gpui_size(bounds.size.width, width),
                                )
                            };
                            window.paint_quad(fill(seam, color));
                        },
                    )
                    .absolute()
                    .size_full(),
                );
            }
        }
        if let Some(icon_ref) = &item.icon {
            node = node.child(
                div()
                    .flex()
                    .flex_none()
                    .when(config.direction == Direction::Ltr, |d| {
                        d.mr(px(g.margin_sm as f32))
                    })
                    .when(config.direction == Direction::Rtl, |d| {
                        d.ml(px(g.margin_sm as f32))
                    })
                    .child(
                        icon(icon_ref.clone())
                            .inherit_color()
                            .size(px(tokens.font_size(size) as f32)),
                    ),
            );
        }
        let label = div().child(if let Some(render) = &item.label_render {
            render(window, cx)
        } else {
            item.label.clone().into_any_element()
        });
        let ring = FocusRing::new(
            g.color_primary_border.to_hsla(),
            px(g.line_width_focus as f32),
        );
        node = if card {
            node.child(label)
        } else {
            node.child(ring.apply(label, &focus))
        };
        if self.kind == TabsType::EditableCard && item.closable && !disabled {
            let target = state.clone();
            let close_key = key.clone();
            let keyboard = state.clone();
            let keyboard_key = key.clone();
            let a11y = state.clone();
            let a11y_key = key.clone();
            let close_focus = state.read(cx).items[&key].close_focus.clone();
            let icon = item
                .close_render
                .as_ref()
                .or(self.remove_icon.as_ref())
                .map_or_else(
                    || {
                        icon(IconName::CloseOutlined)
                            .inherit_color()
                            .size(px(g.font_size_sm as f32))
                            .into_any_element()
                    },
                    |render| render(window, cx),
                );
            let mut close = div()
                .id("remove")
                .flex()
                .items_center()
                .justify_center()
                .flex_none()
                .py(px(1.))
                .px(px(6.))
                .when(config.direction == Direction::Ltr, |d| {
                    d.ml(px(g.margin_xs as f32)).mr(px(-(g.margin_xxs as f32)))
                })
                .when(config.direction == Direction::Rtl, |d| {
                    d.mr(px(g.margin_xs as f32)).ml(px(-(g.margin_xxs as f32)))
                })
                .text_color(g.color_icon.to_hsla())
                .cursor_pointer()
                .role(Role::Button)
                .track_focus(&close_focus)
                .hover(|d| d.text_color(g.color_text_heading.to_hsla()))
                .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation())
                .on_key_down(move |event, window, cx| {
                    if matches!(event.keystroke.key.as_str(), "enter" | "space") {
                        keyboard.update(cx, |s, cx| {
                            s.edit(
                                TabsEdit::Remove(keyboard_key.clone()),
                                TabsSource::Keyboard,
                                window,
                                cx,
                            )
                        });
                        window.prevent_default();
                        cx.stop_propagation();
                    }
                })
                .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
                    a11y.update(cx, |s, cx| {
                        s.edit(
                            TabsEdit::Remove(a11y_key.clone()),
                            TabsSource::Accessibility,
                            window,
                            cx,
                        )
                    });
                    cx.stop_propagation();
                })
                .on_click(move |_, window, cx| {
                    target.update(cx, |s, cx| {
                        s.edit(
                            TabsEdit::Remove(close_key.clone()),
                            TabsSource::Pointer,
                            window,
                            cx,
                        )
                    });
                    cx.stop_propagation();
                })
                .child(icon);
            close.style().refine(&self.styles.remove);
            node = node.child(
                Accessibility::new(Role::Button)
                    .label(config.locale.common.close.clone())
                    .apply(
                        FocusRing::new(
                            g.color_primary_border.to_hsla(),
                            px(g.line_width_focus as f32),
                        )
                        .radius(px(g.border_radius_sm as f32))
                        .apply(close, &close_focus),
                    ),
            );
        }
        node.style().refine(&self.styles.item);
        if card && focus.is_focused(window) && window.last_input_was_keyboard() {
            let color = g.color_primary_border.to_hsla();
            let width = px(g.line_width_focus as f32);
            let radius = px(tokens.card_radius(size) as f32);
            let outline_bounds = surface_bounds.clone();
            let mut corners = gpui::Corners::default();
            match self.placement {
                TabPlacement::Top => {
                    corners.top_left = radius;
                    corners.top_right = radius;
                }
                TabPlacement::Bottom => {
                    corners.bottom_left = radius;
                    corners.bottom_right = radius;
                }
                _ if self.placement.trailing(config.direction) => {
                    corners.top_right = radius;
                    corners.bottom_right = radius;
                }
                _ => {
                    corners.top_left = radius;
                    corners.bottom_left = radius;
                }
            }
            node = node.child(
                canvas(
                    move |_, _, _| outline_bounds.get(),
                    move |_, bounds, window, _| {
                        let Some(bounds) = bounds else {
                            return;
                        };
                        window.paint_quad(
                            gpui::outline(bounds.dilate(width - px(3.)), color, Default::default())
                                .border_widths(width)
                                .corner_radii(corners),
                        );
                    },
                )
                .absolute()
                .size_full(),
            );
        }
        let mut accessibility = Accessibility::new(Role::Tab).label(item.label.clone());
        accessibility.disabled = disabled;
        accessibility.automation_id = self
            .accessibility
            .automation_id
            .as_ref()
            .map(|id| format!("{id}.tab.{key}").into());
        let node = measure(accessibility.apply(node), surface_bounds).into_any_element();
        let node = if let Some(render) = &self.tab_render {
            render(
                node,
                &TabsTabRender {
                    item: item.clone(),
                    active: selected,
                    state: state.downgrade(),
                },
                window,
                cx,
            )
        } else {
            node
        };
        measure(node, bounds).into_any_element()
    }

    fn add(
        &self,
        tokens: &TabsTokens,
        config: &crate::ResolvedConfig,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let target = self.state.clone();
        let keyboard = self.state.clone();
        let a11y = self.state.clone();
        let disabled = target.read(cx).disabled;
        let focus = target.read(cx).add_focus.clone().tab_stop(!disabled);
        let content = self.add_icon.as_ref().map_or_else(
            || {
                icon(IconName::PlusOutlined)
                    .inherit_color()
                    .size(px(tokens.globals.font_size as f32))
                    .into_any_element()
            },
            |render| render(window, cx),
        );
        let button = div()
            .id("add")
            .flex()
            .flex_none()
            .items_center()
            .justify_center()
            .min_w(px(
                tokens.card_height_for(self.size.unwrap_or(config.size)) as f32
            ))
            .min_h(px(
                tokens.card_height_for(self.size.unwrap_or(config.size)) as f32
            ))
            .border(px(tokens.globals.line_width as f32))
            .border_color(tokens.globals.color_border_secondary.to_hsla())
            .rounded_t(px(tokens.globals.border_radius_lg as f32))
            .role(Role::Button)
            .track_focus(&focus)
            .when(!disabled, |d| d.cursor_pointer())
            .text_color(tokens.item_color.to_hsla())
            .when(disabled, |d| {
                d.cursor_not_allowed()
                    .text_color(tokens.globals.color_text_disabled.to_hsla())
            })
            .when(!disabled, |d| {
                d.hover(|d| d.text_color(tokens.item_hover_color.to_hsla()))
            })
            .on_key_down(move |event, window, cx| {
                if matches!(event.keystroke.key.as_str(), "enter" | "space") {
                    keyboard.update(cx, |s, cx| {
                        s.edit(TabsEdit::Add, TabsSource::Keyboard, window, cx)
                    });
                    window.prevent_default();
                    cx.stop_propagation();
                }
            })
            .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
                a11y.update(cx, |s, cx| {
                    s.edit(TabsEdit::Add, TabsSource::Accessibility, window, cx)
                });
            })
            .on_click(move |_, window, cx| {
                target.update(cx, |s, cx| {
                    s.edit(TabsEdit::Add, TabsSource::Pointer, window, cx)
                });
            })
            .child(content);
        let mut semantics =
            Accessibility::new(Role::Button).label(config.locale.common.add_tab.clone());
        semantics.disabled = disabled;
        semantics
            .apply(
                FocusRing::new(
                    tokens.globals.color_primary_border.to_hsla(),
                    px(tokens.globals.line_width_focus as f32),
                )
                .apply(button, &focus),
            )
            .into_any_element()
    }

    fn more(
        &self,
        scope: &ConfigScope,
        tokens: &Arc<TabsTokens>,
        extent: f32,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let state = self.state.clone();
        let handle = state.read(cx).popup.clone();
        let popup_focus = state.read(cx).popup_focus.clone();
        let config = scope.snapshot(cx);
        let content = self.more_icon.as_ref().map_or_else(
            || {
                icon(IconName::EllipsisOutlined)
                    .inherit_color()
                    .size(px(tokens.globals.font_size as f32))
                    .into_any_element()
            },
            |render| render(window, cx),
        );
        let more_focus = state
            .read(cx)
            .more_focus
            .clone()
            .tab_stop(!state.read(cx).disabled);
        let button = div()
            .id("more-trigger")
            .flex()
            .flex_none()
            .items_center()
            .justify_center()
            .cursor_pointer()
            .track_focus(&more_focus)
            .role(Role::Button)
            .when(self.placement.vertical(), |d| d.h(px(extent)))
            .when(!self.placement.vertical(), |d| d.w(px(extent)))
            .child(content);
        let popup_tokens = tokens.clone();
        let renderer = self.popup_render.clone();
        let styles = self.styles.popup.clone();
        let close_label = config.locale.common.close.clone();
        let mut popup_font = config.font.clone();
        popup_font.weight = gpui::FontWeight::NORMAL;
        Popup::new("more-popup")
            .scope(scope.clone())
            .handle(&handle)
            .initial_focus(self.popup_focus.as_ref().unwrap_or(&popup_focus))
            .disabled(state.read(cx).disabled)
            .child(
                Accessibility::new(Role::Button)
                    .label(config.locale.common.more_tabs.clone())
                    .apply(
                        FocusRing::new(
                            tokens.globals.color_primary_border.to_hsla(),
                            px(tokens.globals.line_width_focus as f32),
                        )
                        .apply(button, &more_focus),
                    ),
            )
            .triggers(self.more_triggers | PopupTriggers::CLICK)
            .arrow(false)
            .placement(
                self.more_placement
                    .unwrap_or(if config.direction == Direction::Rtl {
                        PopupPlacement::BottomLeft
                    } else {
                        PopupPlacement::BottomRight
                    }),
            )
            .z_index(tokens.z_index_popup)
            .gap(px(4.))
            .fresh(true)
            .destroy_on_hidden(true)
            .shadows(tokens.globals.box_shadow_secondary.clone())
            .surface_style(div().p_0().style().clone())
            .background(tokens.globals.color_bg_container.to_hsla())
            .radius(px(tokens.globals.border_radius_lg as f32))
            .accessibility_role(Role::ListBox)
            .accessibility_label(config.locale.common.more_tabs.clone())
            .content(move |handle, window, cx| {
                let s = state.read(cx);
                let context = Rc::new(TabsPopupRender {
                    rest_tabs: s
                        .hidden
                        .iter()
                        .filter_map(|key| s.model.items.get(key).cloned())
                        .collect(),
                    active_key: s.model.active.clone(),
                    handle: handle.clone(),
                    state: state.downgrade(),
                });
                let keyboard = context.clone();
                let popup_focus = s.popup_focus.clone();
                let popup_active = s.popup_active.clone();
                let scroll = s.popup_scroll.clone();
                let editable = s.kind == TabsType::EditableCard;
                let disabled = s.disabled;
                // The menu owns one intrinsic width, including offscreen options.
                // UniformList lays each row out as a root, so both the list and
                // its rows must explicitly fill that width.
                let width = popup_width(
                    &context.rest_tabs,
                    &popup_tokens,
                    &popup_font,
                    editable,
                    disabled,
                    window,
                    cx,
                );
                let mut menu = div()
                    .id("more-list")
                    .flex()
                    .flex_col()
                    .w(width)
                    .min_w(px(120.).min(window.viewport_size().width))
                    .max_w(window.viewport_size().width)
                    .py(px(popup_tokens.globals.padding_xxs as f32))
                    .font(popup_font.clone())
                    .text_size(px(popup_tokens.globals.font_size as f32))
                    .line_height(gpui::relative(popup_tokens.globals.line_height as f32))
                    .text_left()
                    .track_focus(&popup_focus)
                    .on_key_down(move |event, window, cx| {
                        let key = event.keystroke.key.as_str();
                        if !matches!(
                            key,
                            "up" | "down"
                                | "home"
                                | "end"
                                | "enter"
                                | "space"
                                | "delete"
                                | "backspace"
                        ) {
                            return;
                        }
                        let chosen = keyboard
                            .state
                            .update(cx, |s, cx| {
                                let enabled: Vec<_> = s
                                    .hidden
                                    .iter()
                                    .filter(|key| s.enabled(key))
                                    .cloned()
                                    .collect();
                                if enabled.is_empty() {
                                    return None;
                                }
                                let current = s
                                    .popup_active
                                    .as_ref()
                                    .and_then(|key| enabled.iter().position(|item| item == key));
                                if matches!(key, "delete" | "backspace") {
                                    if let Some(index) = current {
                                        s.edit(
                                            TabsEdit::Remove(enabled[index].clone()),
                                            TabsSource::Keyboard,
                                            window,
                                            cx,
                                        );
                                    }
                                    return None;
                                }
                                if matches!(key, "enter" | "space") {
                                    return Some(enabled[current.unwrap_or(0)].clone());
                                }
                                let index = match key {
                                    "home" => 0,
                                    "end" => enabled.len() - 1,
                                    "up" => current.map_or(enabled.len() - 1, |i| {
                                        (i + enabled.len() - 1) % enabled.len()
                                    }),
                                    _ => current.map_or(0, |i| (i + 1) % enabled.len()),
                                };
                                s.popup_active = Some(enabled[index].clone());
                                if let Some(row) =
                                    s.hidden.iter().position(|key| key == &enabled[index])
                                {
                                    s.popup_scroll
                                        .scroll_to_item(row, gpui::ScrollStrategy::Nearest);
                                }
                                cx.notify();
                                None
                            })
                            .ok()
                            .flatten();
                        if let Some(key) = chosen {
                            keyboard.select(&key, window, cx);
                        }
                        cx.stop_propagation();
                        window.prevent_default();
                    });
                menu.style().refine(&styles);
                let rows = context.clone();
                let row_tokens = popup_tokens.clone();
                let close_label = close_label.clone();
                let count = rows.rest_tabs.len();
                let row_height =
                    popup_tokens.globals.font_height + popup_tokens.globals.padding_xxs * 2.;
                state.update(cx, |s, _| s.rendered_popup_rows = 0);
                menu = menu.child(
                    gpui::uniform_list("overflow-options", count, move |range, window, cx| {
                        let _ = rows
                            .state
                            .update(cx, |s, _| s.rendered_popup_rows += range.len());
                        range
                            .map(|index| {
                                let item = &rows.rest_tabs[index];
                                let selection = rows.clone();
                                let key = item.key.clone();
                                let a11y_select = rows.clone();
                                let a11y_key = item.key.clone();
                                let g = &row_tokens.globals;
                                let hover_color = g.control_item_bg_hover.to_hsla();
                                let unavailable = disabled || item.disabled;
                                let mut row = div()
                                    .id(item.key.clone())
                                    .flex()
                                    .w_full()
                                    .min_w_0()
                                    .items_center()
                                    .h(px(row_height as f32))
                                    .px(px(g.padding_sm as f32))
                                    .py(px(g.padding_xxs as f32))
                                    .whitespace_nowrap()
                                    .text_color(
                                        if unavailable {
                                            g.color_text_disabled
                                        } else {
                                            g.color_text
                                        }
                                        .to_hsla(),
                                    )
                                    .when(!unavailable, |d| {
                                        d.cursor_pointer().hover(move |d| d.bg(hover_color))
                                    })
                                    .when(unavailable, |d| d.cursor_not_allowed())
                                    .role(Role::ListBoxOption)
                                    .aria_selected(rows.active_key.as_ref() == Some(&item.key))
                                    .when(
                                        !unavailable && popup_active.as_ref() == Some(&item.key),
                                        |d| d.bg(hover_color),
                                    )
                                    .on_click(move |_, window, cx| {
                                        selection.select(&key, window, cx)
                                    })
                                    .on_a11y_action(
                                        accesskit::Action::Click,
                                        move |_, window, cx| {
                                            a11y_select.select(&a11y_key, window, cx)
                                        },
                                    );
                                if let Some(icon_ref) = &item.icon {
                                    row = row.child(
                                        icon(icon_ref.clone())
                                            .inherit_color()
                                            .size(px(g.font_size as f32)),
                                    );
                                }
                                row = row.child(div().flex_1().min_w_0().truncate().child(
                                    if let Some(render) = &item.label_render {
                                        render(window, cx)
                                    } else {
                                        item.label.clone().into_any_element()
                                    },
                                ));
                                if editable && item.closable && !unavailable {
                                    let close_state = rows.state.clone();
                                    let close_key = item.key.clone();
                                    row = row.child(
                                        div()
                                            .id("remove")
                                            .flex_none()
                                            .ml(px(g.margin_sm as f32))
                                            .px(px(6.))
                                            .py(px(1.))
                                            .text_color(g.color_icon.to_hsla())
                                            .role(Role::Button)
                                            .aria_label(close_label.clone())
                                            .on_click(move |_, window, cx| {
                                                let _ = close_state.update(cx, |s, cx| {
                                                    s.edit(
                                                        TabsEdit::Remove(close_key.clone()),
                                                        TabsSource::Overflow,
                                                        window,
                                                        cx,
                                                    )
                                                });
                                                cx.stop_propagation();
                                            })
                                            .child(
                                                icon(IconName::CloseOutlined)
                                                    .inherit_color()
                                                    .size(px(g.font_size_sm as f32)),
                                            ),
                                    );
                                }
                                let mut semantics = Accessibility::new(Role::ListBoxOption)
                                    .label(item.label.clone());
                                semantics.disabled = unavailable;
                                semantics.apply(row).into_any_element()
                            })
                            .collect::<Vec<_>>()
                    })
                    .w_full()
                    .h(px((count as f64 * row_height)
                        .min((200. - popup_tokens.globals.padding_xxs * 2.).max(0.))
                        as f32))
                    .track_scroll(&scroll),
                );
                if let Some(render) = &renderer {
                    render(menu.into_any_element(), &context, window, cx)
                } else {
                    menu.into_any_element()
                }
            })
            .into_any_element()
    }
}

/// Match the upstream menu's max-content width with a 120px minimum. Text shaping
/// is cached by GPUI; custom labels use native layout instead of their accessible
/// names. This measures all options without mounting offscreen interactive rows.
fn popup_width(
    items: &[TabItem],
    tokens: &TabsTokens,
    font: &gpui::Font,
    editable: bool,
    disabled: bool,
    window: &mut Window,
    cx: &mut App,
) -> gpui::Pixels {
    let g = &tokens.globals;
    items
        .iter()
        .map(|item| {
            let label_width = if let Some(render) = &item.label_render {
                let label = render(window, cx);
                div()
                    .id((ElementId::Name("overflow-measure".into()), item.key.clone()))
                    .font(font.clone())
                    .text_size(px(g.font_size as f32))
                    .line_height(gpui::relative(g.line_height as f32))
                    .whitespace_nowrap()
                    .child(label)
                    .into_any_element()
                    .layout_as_root(
                        gpui_size(
                            gpui::AvailableSpace::MaxContent,
                            gpui::AvailableSpace::MaxContent,
                        ),
                        window,
                        cx,
                    )
                    .width
            } else {
                window
                    .text_system()
                    .shape_line(
                        item.label.clone(),
                        px(g.font_size as f32),
                        &[gpui::TextRun {
                            len: item.label.len(),
                            font: font.clone(),
                            color: g.color_text.to_hsla(),
                            background_color: None,
                            underline: None,
                            strikethrough: None,
                        }],
                        None,
                    )
                    .width
            };
            let icon_width = if item.icon.is_some() { g.font_size } else { 0. };
            let remove_width = if editable && item.closable && !disabled && !item.disabled {
                g.margin_sm + 12. + g.font_size_sm
            } else {
                0.
            };
            label_width + px((g.padding_sm * 2. + icon_width + remove_width) as f32)
        })
        .fold(px(120.), gpui::Pixels::max)
}
