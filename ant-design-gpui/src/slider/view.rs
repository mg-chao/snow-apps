// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    SliderChange, SliderChangeSource, SliderDisabled, SliderOrientation, SliderState, SliderTheme,
    SliderValue, state::ChangeHandler,
};
use crate::{
    ConfigScope, Tooltip,
    behavior::{Accessibility, MotionPolicy},
    popup::{PopupPlacement, PopupTriggers},
    theme::Color,
};
use gpui::{
    AnyElement, App, AvailableSpace, Bounds, Element, ElementId, Entity, GlobalElementId,
    InspectorElementId, IntoElement, LayoutId, MouseButton, MouseMoveEvent, MouseUpEvent, Pixels,
    Refineable, RenderOnce, Role, SharedString, Style, StyleRefinement, Styled, Subscription,
    WeakEntity, Window, accesskit, canvas, div, fill, point, prelude::*, px, relative, size,
};
use std::{cell::RefCell, rc::Rc};

type MarkContent = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
type ValueText = Rc<dyn Fn(f64) -> SharedString>;
type TooltipContent = Rc<dyn Fn(f64, &mut Window, &mut App) -> Option<AnyElement>>;

#[derive(Clone)]
pub struct SliderMark {
    pub value: f64,
    pub label: SharedString,
    pub style: StyleRefinement,
    content: Option<MarkContent>,
}
impl SliderMark {
    pub fn new(value: f64, label: impl Into<SharedString>) -> Self {
        Self {
            value,
            label: label.into(),
            style: StyleRefinement::default(),
            content: None,
        }
    }
    pub fn content<E: IntoElement>(
        mut self,
        content: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.content = Some(Rc::new(move |window, cx| {
            content(window, cx).into_any_element()
        }));
        self
    }
    pub fn style(mut self, style: StyleRefinement) -> Self {
        self.style = style;
        self
    }
}

/// Native refinements for Ant Design's public slider semantic slots.
#[derive(Clone, Default)]
pub struct SliderStyles {
    pub root: StyleRefinement,
    pub rail: StyleRefinement,
    /// One surface spanning the selected interval, behind individual track segments.
    pub tracks: StyleRefinement,
    pub track: StyleRefinement,
    pub handle: StyleRefinement,
    pub dot: StyleRefinement,
    pub mark: StyleRefinement,
}
impl SliderStyles {
    fn refine(&mut self, other: &Self) {
        self.root.refine(&other.root);
        self.rail.refine(&other.rail);
        self.tracks.refine(&other.tracks);
        self.track.refine(&other.track);
        self.handle.refine(&other.handle);
        self.dot.refine(&other.dot);
        self.mark.refine(&other.mark);
    }
}

#[derive(Clone, Debug)]
pub struct SliderStyleContext {
    pub options: super::SliderOptions,
    pub value: SliderValue,
    pub disabled: bool,
    pub hovered: bool,
    pub focused: bool,
    pub dragging: bool,
}
type StyleResolver = Rc<dyn Fn(&SliderStyleContext, &mut Window, &mut App) -> SliderStyles>;

#[derive(Clone)]
pub struct SliderTooltip {
    pub enabled: bool,
    /// `None` follows hover, dragging, and focus until mouse release.
    pub open: Option<bool>,
    pub placement: Option<PopupPlacement>,
    pub auto_adjust_overflow: bool,
    pub formatter: Option<TooltipContent>,
}
impl Default for SliderTooltip {
    fn default() -> Self {
        Self {
            enabled: true,
            open: None,
            placement: None,
            auto_adjust_overflow: true,
            formatter: None,
        }
    }
}
impl SliderTooltip {
    /// Returning `None` suppresses this value's tooltip, including forced-open tooltips.
    pub fn formatter<E: IntoElement>(
        mut self,
        formatter: impl Fn(f64, &mut Window, &mut App) -> Option<E> + 'static,
    ) -> Self {
        self.formatter = Some(Rc::new(move |v, w, c| {
            formatter(v, w, c).map(IntoElement::into_any_element)
        }));
        self
    }
}

#[derive(IntoElement)]
pub struct Slider {
    id: ElementId,
    state: Entity<SliderState>,
    scope: Option<ConfigScope>,
    value: Option<SliderValue>,
    default_value: Option<SliderValue>,
    disabled: Option<SliderDisabled>,
    marks: Option<Vec<SliderMark>>,
    tooltip: SliderTooltip,
    label: Option<SharedString>,
    description: Option<SharedString>,
    automation_id: Option<SharedString>,
    handle_labels: Vec<SharedString>,
    value_text: Option<ValueText>,
    on_change: Option<ChangeHandler>,
    on_complete: Option<ChangeHandler>,
    styles: SliderStyles,
    styles_fn: Option<StyleResolver>,
}
impl Slider {
    pub fn new(id: impl Into<ElementId>, state: &Entity<SliderState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            value: None,
            default_value: None,
            disabled: None,
            marks: None,
            tooltip: SliderTooltip::default(),
            label: None,
            description: None,
            automation_id: None,
            handle_labels: Vec::new(),
            value_text: None,
            on_change: None,
            on_complete: None,
            styles: SliderStyles::default(),
            styles_fn: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn value(mut self, value: impl Into<SliderValue>) -> Self {
        self.value = Some(value.into());
        self
    }
    pub fn default_value(mut self, value: impl Into<SliderValue>) -> Self {
        self.default_value = Some(value.into());
        self
    }
    pub fn disabled(mut self, disabled: impl Into<SliderDisabled>) -> Self {
        self.disabled = Some(disabled.into());
        self
    }
    pub fn marks(mut self, marks: impl IntoIterator<Item = SliderMark>) -> Self {
        self.marks = Some(marks.into_iter().collect());
        self
    }
    pub fn tooltip(mut self, tooltip: SliderTooltip) -> Self {
        self.tooltip = tooltip;
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
    pub fn handle_labels(
        mut self,
        labels: impl IntoIterator<Item = impl Into<SharedString>>,
    ) -> Self {
        self.handle_labels = labels.into_iter().map(Into::into).collect();
        self
    }
    pub fn value_text(mut self, formatter: impl Fn(f64) -> SharedString + 'static) -> Self {
        self.value_text = Some(Rc::new(formatter));
        self
    }
    pub fn on_change(
        mut self,
        handler: impl Fn(&SliderChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Rc::new(handler));
        self
    }
    pub fn on_change_complete(
        mut self,
        handler: impl Fn(&SliderChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_complete = Some(Rc::new(handler));
        self
    }
    pub fn styles(mut self, styles: SliderStyles) -> Self {
        self.styles = styles;
        self
    }
    pub fn styles_fn(
        mut self,
        resolver: impl Fn(&SliderStyleContext, &mut Window, &mut App) -> SliderStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(resolver));
        self
    }
}
impl Styled for Slider {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.styles.root
    }
}

struct Mount {
    state: WeakEntity<SliderState>,
    _observer: Subscription,
    _release: Subscription,
}

impl RenderOnce for Slider {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<SliderTheme>();
        let state = self.state.clone();
        let observed = state.clone();
        let _mount = window.use_keyed_state(self.id.clone(), cx, move |_, cx| {
            observed.update(cx, |s, _| s.cancel_interaction());
            Mount {
                state: observed.downgrade(),
                _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                _release: cx.on_release(|this: &mut Mount, cx| {
                    let _ = this.state.update(cx, |s, _| {
                        s.cancel_interaction();
                        s.bounds = None;
                        s.hovered = false;
                    });
                }),
            }
        });
        state.update(cx, |s, cx| {
            s.configure_scope(scope.clone(), window, cx);
            if let Some(marks) = &self.marks {
                let values: Vec<_> = marks.iter().map(|mark| mark.value).collect();
                if s.options().marks != values {
                    let mut options = s.options().clone();
                    options.marks = values;
                    s.set_options(options, window, cx)
                        .expect("Slider marks must be finite");
                }
            }
            s.disabled_override = self.disabled.clone();
            s.on_change = self.on_change.clone();
            s.on_complete = self.on_complete.clone();
            s.controlled = self.value.is_some();
            if let Some(value) = &self.value {
                s.set_value(value.clone(), window, cx)
                    .expect("Slider value must match its validated options");
            } else if !s.initialized {
                if let Some(value) = &self.default_value {
                    s.set_value(value.clone(), window, cx)
                        .expect("valid Slider default value");
                }
                s.initialized = true;
            }
            s.sync_handles(window, cx);
            s.cancel_unavailable_interaction(cx);
        });
        let options = state.read(cx).options();
        let vertical = options.orientation == SliderOrientation::Vertical;
        let inverted = options.inverted(config.direction);
        let (min, max, range, included, step, show_dots, has_marks) = (
            options.min,
            options.max,
            options.range.is_some(),
            options.included,
            options.step,
            options.dots,
            !options.marks.is_empty(),
        );
        let disabled = state.read(cx).all_disabled(cx);
        if let Some(resolve) = &self.styles_fn {
            let s = state.read(cx);
            let context = SliderStyleContext {
                options: s.options().clone(),
                value: s.value(),
                disabled,
                hovered: s.hovered,
                focused: s.handles.values().any(|h| h.focus.is_focused(window)),
                dragging: s.drag.is_some(),
            };
            self.styles.refine(&resolve(&context, window, cx));
        }
        let values = state.read(cx).model.handles.clone();
        // A handle dragged out for deletion keeps its native focus/gesture identity
        // until mouseup. Moving it back restores the value without a new gesture.
        let deleting = state
            .read(cx)
            .drag
            .as_ref()
            .filter(|d| d.deleting)
            .and_then(|d| d.active);
        let visual_values = state
            .read(cx)
            .drag
            .as_ref()
            .filter(|d| d.deleting)
            .map_or_else(|| values.clone(), |d| d.working.clone());
        let policy =
            MotionPolicy::new(config.motion && tokens.globals().motion, cx.reduce_motion());
        let (hover, animations) = state.update(cx, |s, cx| {
            let now = cx
                .background_executor()
                .now()
                .saturating_duration_since(s.epoch);
            s.rail_motion.retarget(
                if s.hovered && !disabled { 1. } else { 0. },
                now,
                tokens.globals().motion_duration_mid,
                policy,
            );
            let rail = s.rail_motion.sample(now);
            let mut running = !rail.finished;
            let mut samples = Vec::with_capacity(visual_values.len());
            for (index, value) in visual_values.iter().enumerate() {
                let disabled = s.disabled(index, cx);
                let dragging = s.drag.as_ref().is_some_and(|d| d.active == Some(value.id));
                let handle = s.handles.get_mut(&value.id).expect("retained handle");
                let active =
                    !disabled && (handle.hovered || handle.focus.is_focused(window) || dragging);
                handle.motion.retarget(
                    if active { 1. } else { 0. },
                    now,
                    tokens.globals().motion_duration_mid,
                    policy,
                );
                let sample = handle.motion.sample(now);
                running |= !sample.finished;
                samples.push(sample.value as f32);
            }
            if running {
                s.request_frame(window, cx);
            }
            (rail.value as f32, samples)
        });
        let scale = state.read(cx).model.scale.clone();
        let fraction = |value| {
            let f = scale.fraction(value) as f32;
            if inverted { 1. - f } else { f }
        };
        let (start, end) = if range {
            (
                values.first().map_or(min, |h| h.value),
                values.last().map_or(min, |h| h.value),
            )
        } else {
            (min, values.first().map_or(min, |h| h.value))
        };
        let interval = (
            fraction(start).min(fraction(end)),
            fraction(start).max(fraction(end)),
        );
        let rail = tokens.rail_size as f32;
        let mut root = div()
            .id(self.id)
            .flex()
            .font(config.font.clone())
            .text_size(px(tokens.globals().font_size as f32))
            .line_height(relative(tokens.globals().line_height as f32));
        root = if vertical {
            root.flex_col().h_full()
        } else {
            root.flex_1()
                .when(self.styles.root.size.width.is_some(), |d| d.flex_none())
        };
        let target = state.clone();
        let mut body =
            root.relative()
                .min_w(px(0.))
                .min_h(px(0.))
                .on_hover(move |hovered, _, cx| {
                    target.update(cx, |s, cx| {
                        if s.hovered != *hovered {
                            s.hovered = *hovered;
                            cx.notify();
                        }
                    })
                });
        body = if vertical {
            body.w(px(rail * 3.))
                .mx(px(tokens.margin_cross() as f32))
                .my(px(tokens.margin_axis() as f32))
        } else {
            body.h(px(rail * 3.))
                .mx(px(tokens.margin_axis() as f32))
                .mt(px(tokens.margin_cross() as f32))
                .mb(px(if !has_marks {
                    tokens.margin_cross()
                } else {
                    tokens.margin_with_marks()
                } as f32))
        };
        body.style().refine(&self.styles.root);
        body = if disabled {
            body.cursor_not_allowed()
        } else {
            body.cursor_pointer()
        };
        let measure_state = state.clone();
        let drag_state = state.downgrade();
        let drag_end = drag_state.clone();
        body = body.child(
            canvas(
                move |bounds, _, cx| {
                    measure_state.update(cx, |s, _| s.bounds = Some(bounds));
                },
                move |_, (), window, _| {
                    let target = drag_state.clone();
                    window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
                        if phase.capture() {
                            let _ = target
                                .update(cx, |s, cx| s.pointer_move(event.position, window, cx));
                        }
                    });
                    let target = drag_end.clone();
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
        let target = state.clone();
        body = body.on_mouse_down(MouseButton::Left, move |event, window, cx| {
            let track = {
                let s = target.read(cx);
                s.model.track_draggable()
                    && !s.any_disabled(cx)
                    && s.bounds.is_some_and(|b| {
                        let p = if vertical {
                            (event.position.y - b.top()) / b.size.height
                        } else {
                            (event.position.x - b.left()) / b.size.width
                        };
                        p >= interval.0 && p <= interval.1
                    })
            };
            target.update(cx, |s, cx| {
                s.pointer_start(None, track, event.position, window, cx)
            });
            window.prevent_default();
            cx.stop_propagation();
        });
        let mut rail_element = div()
            .absolute()
            .rounded(px(tokens.globals().border_radius_xs as f32))
            .bg(mix(tokens.rail_bg, tokens.rail_hover_bg, hover));
        rail_element = if vertical {
            rail_element.top_0().bottom_0().left(px(rail)).w(px(rail))
        } else {
            rail_element.left_0().right_0().top(px(rail)).h(px(rail))
        };
        rail_element.style().refine(&self.styles.rail);
        body = body.child(rail_element);
        if included && !values.is_empty() {
            let positioned = |start: f32, end: f32| {
                if vertical {
                    div()
                        .absolute()
                        .left(px(rail))
                        .w(px(rail))
                        .top(relative(start))
                        .h(relative(end - start))
                } else {
                    div()
                        .absolute()
                        .top(px(rail))
                        .h(px(rail))
                        .left(relative(start))
                        .w(relative(end - start))
                }
            };
            let segments = if range {
                values.len().saturating_sub(1)
            } else {
                1
            };
            if segments > 0 {
                let mut aggregate = positioned(interval.0, interval.1);
                aggregate.style().refine(&self.styles.tracks);
                body = body.child(aggregate);
            }
            for index in 0..segments {
                let (from, to) = if range {
                    (values[index].value, values[index + 1].value)
                } else {
                    (min, values[0].value)
                };
                let (from, to) = (fraction(from), fraction(to));
                let mut track = positioned(from.min(to), from.max(to))
                    .rounded(px(tokens.globals().border_radius_xs as f32))
                    .bg(if disabled {
                        tokens.track_bg_disabled
                    } else {
                        mix(tokens.track_bg, tokens.track_hover_bg, hover)
                    });
                track.style().refine(&self.styles.track);
                body = body.child(track);
            }
        }
        let dot_scale = state.read(cx).model.scale.clone();
        let dot_tokens = tokens.clone();
        let dot_style = self.styles.dot.clone();
        let dot_opacity = dot_style.opacity.unwrap_or(1.);
        let dot_step = step.filter(|_| show_dots);
        let selected_values: Vec<_> = values.iter().map(|h| h.value).collect();
        body = body.child(
            div().absolute().inset_0().opacity(dot_opacity).child(
                canvas(
                    |_, _, _| (),
                    move |bounds, (), window, cx| {
                        let dpi = window.scale_factor();
                        let diameter = dot_tokens.dot_size as f32;
                        let mut style = Style::default();
                        style.refine(
                            &StyleRefinement::default()
                                .size(px(diameter))
                                .rounded(px(diameter / 2.))
                                .bg(dot_tokens.globals().color_bg_elevated)
                                .border(px(dot_tokens.handle_line_width as f32))
                                .border_color(dot_tokens.dot_border_color),
                        );
                        style.refine(&dot_style);
                        let dimension = |length, base| match length {
                            gpui::Length::Definite(length) => {
                                length.to_pixels(base, window.rem_size())
                            }
                            gpui::Length::Auto => px(diameter),
                        };
                        let width =
                            dimension(style.size.width, bounds.size.width.into()).max(px(0.));
                        let height =
                            dimension(style.size.height, bounds.size.height.into()).max(px(0.));
                        let mut paint_dot = |value: f64| {
                            let ratio = if max == min {
                                0.
                            } else {
                                ((value - min) / (max - min)) as f32
                            };
                            let ratio = if inverted { 1. - ratio } else { ratio };
                            let center = if vertical {
                                point(
                                    bounds.left() + px(rail * 1.5),
                                    bounds.top() + bounds.size.height * ratio,
                                )
                            } else {
                                point(
                                    bounds.left() + bounds.size.width * ratio,
                                    bounds.top() + px(rail * 1.5),
                                )
                            };
                            let active = if included {
                                value >= start && value <= end
                            } else {
                                selected_values
                                    .binary_search_by(|v| v.total_cmp(&value))
                                    .is_ok()
                            };
                            let border = if disabled {
                                dot_tokens.track_bg_disabled
                            } else if active {
                                dot_tokens.dot_active_border_color
                            } else {
                                mix(
                                    dot_tokens.dot_border_color,
                                    dot_tokens.globals().color_fill_content_hover,
                                    hover,
                                )
                            };
                            style.border_color =
                                Some(dot_style.border_color.unwrap_or(border.into()));
                            let dot_bounds = Bounds::new(
                                center - point(width / 2., height / 2.),
                                size(width, height),
                            );
                            style.paint(dot_bounds, window, cx, |_, _| {});
                        };
                        let dots = dot_scale.marks();
                        let extent = if vertical {
                            bounds.size.height
                        } else {
                            bounds.size.width
                        };
                        let budget = (extent.to_f64() * f64::from(dpi)).ceil().max(1.);
                        if dots.len() <= budget as usize || max == min {
                            for value in dots {
                                paint_dot(*value);
                            }
                        } else {
                            // Jump over marks occupying the same physical pixel.
                            // The sorted immutable scale avoids copying/scanning
                            // the entire mark set on each redraw.
                            let mut index = 0;
                            while index < dots.len() {
                                let value = dots[index];
                                paint_dot(value);
                                let next_pixel =
                                    (((value - min) / (max - min) * budget).floor() + 1.) / budget;
                                let threshold = min + next_pixel * (max - min);
                                index = dots
                                    .partition_point(|mark| *mark < threshold)
                                    .max(index + 1);
                            }
                            if let Some(last) = dots.last() {
                                paint_dot(*last);
                            }
                        }
                        if let Some(step) = dot_step {
                            let count = ((max - min) / step).floor();
                            // Subpixel dots are visually overlapping. Bound paint work by physical rail resolution.
                            let samples = count.min(budget) as usize;
                            for index in 0..=samples {
                                let value = if count <= budget {
                                    min + index as f64 * step
                                } else {
                                    min + (max - min) * index as f64 / samples.max(1) as f64
                                };
                                paint_dot(value);
                            }
                        }
                        if !included {
                            for value in &selected_values {
                                if dot_step.is_some()
                                    || dots.binary_search_by(|mark| mark.total_cmp(value)).is_ok()
                                {
                                    paint_dot(*value);
                                }
                            }
                        }
                    },
                )
                .size_full(),
            ),
        );
        for (index, mark) in self.marks.unwrap_or_default().into_iter().enumerate() {
            if !mark.value.is_finite() || mark.value < min || mark.value > max {
                continue;
            }
            let ratio = fraction(mark.value);
            let active = if included {
                mark.value >= start && mark.value <= end
            } else {
                values.iter().any(|v| v.value == mark.value)
            };
            let mut label = div()
                .id(("mark", index))
                .whitespace_nowrap()
                .text_color(if active {
                    tokens.globals().color_text
                } else {
                    tokens.globals().color_text_description
                });
            label.style().refine(&self.styles.mark);
            label.style().refine(&mark.style);
            label = if disabled {
                label.cursor_not_allowed()
            } else {
                label.cursor_pointer()
            };
            let content = mark
                .content
                .map_or_else(|| mark.label.into_any_element(), |f| f(window, cx));
            let target = state.clone();
            let value = mark.value;
            label = label
                .child(content)
                .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation())
                .on_click(move |_, window, cx| {
                    target.update(cx, |s, cx| {
                        s.activate_value(value, SliderChangeSource::Mark, window, cx)
                    });
                    cx.stop_propagation();
                });
            let anchor = if vertical {
                div()
                    .absolute()
                    .top(relative(ratio))
                    .left(px((tokens.rail_size * 3. + tokens.margin_axis()) as f32))
            } else {
                div().absolute().left(relative(ratio)).top(px(rail * 3.))
            };
            body = body.child(anchor.child(CenteredLabel {
                child: label.into_any_element(),
                vertical,
            }));
        }
        for (index, value) in visual_values.iter().enumerate() {
            let id = value.id;
            let numeric = value.value;
            let handle = state.read(cx).handles.get(&id).expect("handle state");
            let focus = handle.focus.clone();
            let tooltip_active = handle.hovered
                || handle.tooltip_focus_open
                || state
                    .read(cx)
                    .drag
                    .as_ref()
                    .is_some_and(|d| d.active == Some(id));
            let handle_disabled = state.read(cx).disabled(index, cx);
            let progress = animations[index];
            let handle_size = tokens.handle_size as f32;
            let ratio = fraction(numeric);
            let mut handle = div()
                .id(("handle", id))
                .absolute()
                .w(px(handle_size))
                .h(px(handle_size))
                .track_focus(&focus)
                .role(Role::Slider)
                .aria_numeric_value(numeric)
                .aria_min_numeric_value(min)
                .aria_max_numeric_value(max);
            if let Some(step) = step {
                handle = handle.aria_numeric_value_step(step);
            }
            if let Some(formatter) = &self.value_text {
                handle = handle.aria_value(formatter(numeric));
            }
            handle = if vertical {
                handle
                    .top(relative(ratio))
                    .mt(px(-handle_size / 2.))
                    .left(px(rail * 1.5 - handle_size / 2.))
            } else {
                handle
                    .left(relative(ratio))
                    .ml(px(-handle_size / 2.))
                    .top(px(rail * 1.5 - handle_size / 2.))
            };
            handle.style().refine(&self.styles.handle);
            let target = state.clone();
            handle = handle.on_mouse_down(MouseButton::Left, move |event, window, cx| {
                target.update(cx, |s, cx| {
                    s.pointer_start(Some(id), false, event.position, window, cx)
                });
                window.prevent_default();
                cx.stop_propagation();
            });
            let target = state.clone();
            handle = handle.on_key_down(move |event, window, cx| {
                if !event.keystroke.modifiers.modified()
                    && target.update(cx, |s, cx| s.key(id, &event.keystroke.key, window, cx))
                {
                    window.prevent_default();
                    cx.stop_propagation();
                }
            });
            let target = state.clone();
            handle = handle.on_key_up(move |event, window, cx| {
                target.update(cx, |s, cx| s.key_up(&event.keystroke.key, window, cx))
            });
            if !handle_disabled {
                for (action, units) in [
                    (accesskit::Action::Increment, 1),
                    (accesskit::Action::Decrement, -1),
                ] {
                    let target = state.clone();
                    handle = handle.on_a11y_action(action, move |_, window, cx| {
                        target.update(cx, |s, cx| {
                            if let Some(value) = s
                                .model
                                .handles
                                .iter()
                                .find(|h| h.id == id)
                                .and_then(|h| s.model.scale.offset(h.value, units))
                                && s.move_to(
                                    id,
                                    value,
                                    SliderChangeSource::Accessibility,
                                    window,
                                    cx,
                                )
                            {
                                s.complete(SliderChangeSource::Accessibility, window, cx);
                            }
                        })
                    });
                }
                let target = state.clone();
                handle =
                    handle.on_a11y_action(accesskit::Action::SetValue, move |data, window, cx| {
                        if let Some(accesskit::ActionData::NumericValue(value)) = data {
                            target.update(cx, |s, cx| {
                                if s.move_to(
                                    id,
                                    *value,
                                    SliderChangeSource::Accessibility,
                                    window,
                                    cx,
                                ) {
                                    s.complete(SliderChangeSource::Accessibility, window, cx);
                                }
                            });
                        }
                    });
            }
            let diameter = handle_size + (tokens.handle_size_hover as f32 - handle_size) * progress;
            let line = tokens.handle_line_width as f32
                + (tokens.handle_line_width_hover as f32 - tokens.handle_line_width as f32)
                    * progress;
            let hit = diameter + line * 2.;
            let target = state.clone();
            let mut hit_area = div()
                .id("hit-area")
                .absolute()
                .left(px((handle_size - hit) / 2.))
                .top(px((handle_size - hit) / 2.))
                .w(px(hit))
                .h(px(hit))
                .on_hover(move |hovered, _, cx| {
                    target.update(cx, |s, cx| {
                        if let Some(h) = s.handles.get_mut(&id)
                            && h.hovered != *hovered
                        {
                            h.hovered = *hovered;
                            cx.notify();
                        }
                    })
                });
            hit_area = if handle_disabled {
                hit_area.cursor_not_allowed()
            } else {
                hit_area.cursor_pointer()
            };
            handle = handle.child(hit_area);
            let colors = tokens.clone();
            let handle_style = self.styles.handle.clone();
            let paint_state = state.clone();
            handle = handle.child(
                canvas(
                    |_, _, _| (),
                    move |bounds, (), window, cx| {
                        // Derive the visual center once from the rail, just as
                        // pointer hit testing does. Rounding a fractional handle
                        // box and then its center accumulates a pixel of drift.
                        let center =
                            paint_state
                                .read(cx)
                                .bounds
                                .map_or(bounds.center(), |rail_bounds| {
                                    if vertical {
                                        point(
                                            rail_bounds.left() + px(rail * 1.5),
                                            rail_bounds.top() + rail_bounds.size.height * ratio,
                                        )
                                    } else {
                                        point(
                                            rail_bounds.left() + rail_bounds.size.width * ratio,
                                            rail_bounds.top() + px(rail * 1.5),
                                        )
                                    }
                                });
                        let circle = |size: f32| {
                            Bounds::new(
                                center - point(px(size / 2.), px(size / 2.)),
                                gpui::size(px(size), px(size)),
                            )
                        };
                        if progress > 0. && !handle_disabled {
                            window.paint_quad(
                                fill(
                                    circle(diameter + 12. * progress),
                                    colors.handle_active_outline_color.to_hsla(),
                                )
                                .corner_radii(px((diameter + 12. * progress) / 2.)),
                            );
                        }
                        let base = if handle_disabled {
                            colors.handle_color_disabled
                        } else {
                            mix(
                                colors.handle_color,
                                colors.globals().color_primary_border_hover,
                                hover,
                            )
                        };
                        let border = if handle_disabled {
                            base
                        } else {
                            mix(base, colors.handle_active_color, progress)
                        };
                        window.paint_quad(
                            fill(
                                circle(diameter + line * 2.),
                                handle_style.border_color.unwrap_or(border.into()),
                            )
                            .corner_radii(px(diameter / 2. + line)),
                        );
                        let background = handle_style
                            .background
                            .as_ref()
                            .and_then(gpui::Fill::color)
                            .unwrap_or(colors.globals().color_bg_elevated.to_hsla().into());
                        window.paint_quad(
                            fill(circle(diameter), background).corner_radii(px(diameter / 2.)),
                        );
                    },
                )
                .absolute()
                .inset_0(),
            );
            let mut semantics = Accessibility::new(Role::Slider);
            semantics.disabled = handle_disabled;
            semantics.label = self
                .handle_labels
                .get(index)
                .cloned()
                .or_else(|| self.label.clone());
            semantics.description = self.description.clone();
            semantics.automation_id = self
                .automation_id
                .as_ref()
                .map(|root| format!("{root}.handle.{index}").into());
            if deleting == Some(id) {
                handle = handle.opacity(0.);
            }
            let handle = semantics.apply(handle);
            let popup_open = self.tooltip.enabled
                && !handle_disabled
                && deleting != Some(id)
                && self.tooltip.open.unwrap_or(tooltip_active);
            let placement = self.tooltip.placement.unwrap_or(if vertical {
                if config.direction == crate::Direction::Rtl {
                    PopupPlacement::Left
                } else {
                    PopupPlacement::Right
                }
            } else {
                PopupPlacement::Top
            });
            let mut tooltip = Tooltip::new(("handle-tooltip", id))
                .scope(scope.clone())
                .child(handle)
                .triggers(PopupTriggers::NONE)
                .open(popup_open)
                .placement(placement)
                .auto_adjust_overflow(self.tooltip.auto_adjust_overflow)
                .surface_style(div().min_w(px(0.)).style().clone());
            if let Some(formatter) = self.tooltip.formatter.clone() {
                // Suppression and rendering share one element. Closed handles never
                // build tooltip content; each visible handle calls its factory once.
                let content = popup_open.then(|| formatter(numeric, window, cx)).flatten();
                let present = content.is_some();
                let content = RefCell::new(content);
                tooltip =
                    tooltip
                        .open(popup_open && present)
                        .title_content(move |_, window, cx| {
                            content
                                .borrow_mut()
                                .take()
                                .or_else(|| formatter(numeric, window, cx))
                                .unwrap_or_else(|| div().into_any_element())
                        });
            } else {
                tooltip = tooltip.title(numeric.to_string());
            }
            body = body.child(tooltip);
        }
        body
    }
}

fn mix(from: Color, to: Color, t: f32) -> Color {
    let t = f64::from(t.clamp(0., 1.));
    let alpha = from.a + (to.a - from.a) * t;
    let channel = |a: u8, b: u8| {
        if alpha > 0. {
            ((f64::from(a) * from.a * (1. - t) + f64::from(b) * to.a * t) / alpha).round() as u8
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

/// Labels are measured natively, then centered at a value without altering rail layout.
struct CenteredLabel {
    child: AnyElement,
    vertical: bool,
}
impl IntoElement for CenteredLabel {
    type Element = Self;
    fn into_element(self) -> Self {
        self
    }
}
impl Element for CenteredLabel {
    type RequestLayoutState = ();
    type PrepaintState = ();
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
        cx: &mut App,
    ) -> (LayoutId, ()) {
        (window.request_layout(Style::default(), [], cx), ())
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
        let measured = self.child.layout_as_root(
            size(AvailableSpace::MaxContent, AvailableSpace::MaxContent),
            window,
            cx,
        );
        let offset = if self.vertical {
            point(px(0.), measured.height / 2.)
        } else {
            point(measured.width / 2., px(0.))
        };
        self.child.prepaint_at(bounds.origin - offset, window, cx);
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
        self.child.paint(window, cx);
    }
}
