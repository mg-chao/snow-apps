// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    CarouselOptions, CarouselSource, CarouselState, CarouselStyles, CarouselTokens, DotPlacement,
    element::{ControlGeometry, record_control},
    view::{
        ArrowRender, CarouselArrowContext, CarouselDotContext, ControlState, DotRender,
        Presentation,
    },
};
use crate::{
    Direction,
    behavior::{Accessibility, evaluate_cubic_bezier},
    theme::CubicBezier,
};
use gpui::{
    AnyElement, App, Entity, IntoElement, MouseButton, PathBuilder, Refineable, Role, Window,
    accesskit, canvas, div, point, prelude::*, px, relative,
};
use std::{cell::RefCell, rc::Rc, time::Duration};

pub(super) struct Controls {
    pub elements: Vec<AnyElement>,
    pub animating: bool,
}

#[allow(clippy::too_many_arguments)]
pub(super) fn render_controls(
    state: &Entity<CarouselState>,
    presentation: &Rc<RefCell<Presentation>>,
    options: &CarouselOptions,
    tokens: &CarouselTokens,
    styles: &CarouselStyles,
    previous_arrow: Option<ArrowRender>,
    next_arrow: Option<ArrowRender>,
    dot_render: Option<DotRender>,
    current: Option<usize>,
    now: Duration,
    progress: f64,
    window: &mut Window,
    cx: &mut App,
) -> Controls {
    let scope = state
        .read(cx)
        .scope
        .clone()
        .expect("mounted Carousel scope");
    let config = scope.snapshot(cx);
    let count = state.read(cx).items().len();
    let vertical = options.is_vertical();
    let rtl = !vertical && config.direction == Direction::Rtl;
    let policy = config.motion_policy(cx);
    let duration = tokens.globals().motion_duration_slow;
    let mut output = Controls {
        elements: Vec::new(),
        animating: false,
    };
    state
        .read(cx)
        .focus
        .clone()
        .tab_stop(count > 1 && !options.dots && !options.arrows && !config.disabled);
    let focused_control_removed = {
        let p = presentation.borrow();
        let unavailable_dots = count <= 1 || !options.dots || config.disabled;
        let unavailable_arrows = count <= 1 || !options.arrows || config.disabled;
        (unavailable_dots && p.dots.values().any(|dot| dot.focus.is_focused(window)))
            || (unavailable_arrows && p.arrows.iter().any(|arrow| arrow.focus.is_focused(window)))
    };
    if focused_control_removed {
        state.read(cx).focus.clone().focus(window, cx);
    }
    if count <= 1 {
        return output;
    }

    if options.arrows {
        for (which, custom) in [previous_arrow, next_arrow].into_iter().enumerate() {
            let previous = which == 0;
            let unavailable = config.disabled
                || (!options.infinite
                    && if previous {
                        current == Some(0)
                    } else {
                        current == Some(count - 1)
                    });
            let mut p = presentation.borrow_mut();
            let control = &mut p.arrows[which];
            control.focus.clone().tab_stop(!unavailable);
            if unavailable {
                if control.focus.is_focused(window) {
                    let root_focus = state.read(cx).focus.clone();
                    root_focus.focus(window, cx);
                }
                continue;
            }
            let focus = control.focus.clone();
            let target_alpha = if control.hovered || focus.is_focused(window) {
                1.
            } else {
                0.4
            };
            control
                .opacity
                .retarget(target_alpha, now, duration, policy);
            let alpha = control.opacity.sample(now);
            output.animating |= !alpha.finished;
            drop(p);
            let context = CarouselArrowContext {
                previous,
                current,
                count,
                vertical,
                direction: config.direction,
            };
            let size = tokens.arrow_size as f32;
            let offset = tokens.arrow_offset as f32;
            let mut button = div()
                .id(if previous {
                    "carousel-previous"
                } else {
                    "carousel-next"
                })
                .absolute()
                .w(px(size))
                .h(px(size))
                .cursor_pointer()
                .track_focus(&focus);
            if vertical {
                button = button.left(relative(0.5)).ml(px(-size / 2.));
                button = if previous {
                    button.top(px(offset))
                } else {
                    button.bottom(px(offset))
                };
            } else {
                button = button.top(relative(0.5)).mt(px(-size / 2.));
                button = if previous != rtl {
                    button.left(px(offset))
                } else {
                    button.right(px(offset))
                };
            }
            let target = state.clone();
            let keyboard = state.clone();
            let a11y = state.clone();
            let hover = presentation.clone();
            let hover_state = state.clone();
            let down_focus = focus.clone();
            button = button
                .on_mouse_down(MouseButton::Left, move |_, window, cx| {
                    down_focus.focus(window, cx);
                    cx.stop_propagation();
                })
                .on_click(move |_, window, cx| {
                    target.update(cx, |s, cx| {
                        s.navigate(
                            if previous { -1 } else { 1 },
                            CarouselSource::Arrow,
                            window,
                            cx,
                        )
                    });
                    cx.stop_propagation();
                })
                .on_key_down(move |event, window, cx| {
                    if event.keystroke.key == "enter" || event.keystroke.key == "space" {
                        keyboard.update(cx, |s, cx| {
                            s.navigate(
                                if previous { -1 } else { 1 },
                                CarouselSource::Keyboard,
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
                        s.navigate(
                            if previous { -1 } else { 1 },
                            CarouselSource::Accessibility,
                            window,
                            cx,
                        )
                    })
                })
                .on_hover(move |hovered, _, cx| {
                    hover.borrow_mut().arrows[which].hovered = *hovered;
                    hover_state.update(cx, |_, cx| cx.notify());
                });
            if let Some(custom) = custom {
                button = button.child(custom(&context, window, cx));
            } else {
                // The reference uses a rotated 2px border on a size/sqrt(2) square.
                let color = tokens
                    .globals()
                    .color_white
                    .to_hsla()
                    .opacity(alpha.value as f32);
                button = button.child(
                    canvas(
                        |_, _, _| (),
                        move |bounds, (), window, _| {
                            let center = bounds.center();
                            let side = size / std::f32::consts::SQRT_2;
                            if side <= 0. {
                                return;
                            }
                            let degrees: f32 = if vertical {
                                if previous { 45. } else { -135. }
                            } else if previous != rtl {
                                -45.
                            } else {
                                135.
                            };
                            let (sin, cos) = degrees.to_radians().sin_cos();
                            let position = |x: f32, y: f32| {
                                let (x, y) = (x - side / 2., y - side / 2.);
                                point(
                                    center.x + px(x * cos - y * sin),
                                    center.y + px(x * sin + y * cos),
                                )
                            };
                            let radius = 1f32.min(side / 2.);
                            let border = 2f32.min(side);
                            let mut path = PathBuilder::fill();
                            path.move_to(position(side - radius, 0.));
                            path.curve_to(position(side, radius), position(side, 0.));
                            path.line_to(position(side, border));
                            path.line_to(position(border, border));
                            path.line_to(position(border, side));
                            path.line_to(position(radius, side));
                            path.curve_to(position(0., side - radius), position(0., side));
                            path.line_to(position(0., radius));
                            path.curve_to(position(radius, 0.), position(0., 0.));
                            path.close();
                            if let Ok(path) = path.build() {
                                window.paint_path(path, color);
                            }
                        },
                    )
                    .size_full(),
                );
            }
            button.style().refine(&styles.arrow);
            let label = if previous {
                config.locale.carousel.prev_slide.clone()
            } else {
                config.locale.carousel.next_slide.clone()
            };
            let button = config.focus_ring().radius(px(1.)).apply(
                Accessibility::new(Role::Button).label(label).apply(button),
                &focus,
            );
            output.elements.push(
                record_control(
                    button,
                    state.clone(),
                    if previous {
                        ControlGeometry::Previous
                    } else {
                        ControlGeometry::Next
                    },
                )
                .into_any_element(),
            );
        }
    }
    if options.dots {
        let mut dots = div().absolute().flex().justify_center();
        if vertical {
            dots = dots.flex_col().top(relative(0.5)).w(px((tokens.dot_height
                + 2. * tokens.globals().margin_xxs)
                as f32));
            let start = options.dot_placement != DotPlacement::End;
            // Vertical reference layout intentionally does not mirror with RTL.
            let offset = (if options.dot_placement.vertical() {
                tokens.dot_offset
            } else {
                0.
            } - tokens.globals().margin_xxs) as f32;
            dots = if start {
                dots.left(px(offset))
            } else {
                dots.right(px(offset))
            };
        } else {
            dots = dots.left_0().right_0();
            let offset = (if options.dot_placement.vertical() {
                0.
            } else {
                tokens.dot_offset
            } - tokens.dot_gap) as f32;
            dots = if options.dot_placement == DotPlacement::Top {
                dots.top(px(offset))
            } else {
                dots.bottom(px(offset))
            };
            if rtl {
                dots = dots.flex_row_reverse();
            }
        }
        let mut total = 0.;
        for index in 0..count {
            let active = current == Some(index);
            let mut p = presentation.borrow_mut();
            let control = p.dots.entry(index).or_insert_with(|| {
                ControlState::new(
                    if active {
                        tokens.dot_active_width
                    } else {
                        tokens.dot_width
                    },
                    0.2,
                    cx,
                )
            });
            let focus = control.focus.clone().tab_stop(active && !config.disabled);
            let target = if active {
                tokens.dot_active_width
            } else {
                tokens.dot_width
            };
            control.length.retarget(target, now, duration, policy);
            let alpha_target = if control.hovered { 0.75 } else { 0.2 };
            control
                .opacity
                .retarget(alpha_target, now, duration, policy);
            let length = control.length.sample(now);
            let alpha = control.opacity.sample(now);
            output.animating |= !length.finished || !alpha.finished;
            drop(p);
            let gap = if vertical {
                tokens.globals().margin_xxs
            } else {
                tokens.dot_gap
            } as f32;
            let length = length.value as f32;
            total += length + gap * 2.;
            let mut button = div()
                .id(("carousel-dot", index))
                .relative()
                .cursor_pointer()
                .track_focus(&focus)
                .p(px(gap));
            button = if vertical {
                button
                    .w(px(tokens.dot_height as f32 + 2. * gap))
                    .h(px(length + 2. * gap))
            } else {
                button
                    .w(px(length + 2. * gap))
                    .h(px(tokens.dot_height as f32 + 2. * gap))
                    .min_w_0()
            };
            let background = tokens.globals().color_bg_container.to_hsla();
            let mut bar = div()
                .absolute()
                .top(px(gap))
                .right(px(gap))
                .bottom(px(gap))
                .left(px(gap))
                .rounded(px(tokens.dot_height as f32))
                .overflow_hidden()
                .bg(background.opacity(alpha.value as f32));
            if active {
                let fraction = if options.autoplay && options.dot_duration && policy.allows_motion()
                {
                    evaluate_cubic_bezier(CubicBezier::new(0., 0., 0.58, 1.), progress) as f32
                } else {
                    1.
                };
                let mut fill = div()
                    .absolute()
                    .left_0()
                    .top_0()
                    .bg(background)
                    .rounded(px(tokens.dot_height as f32));
                fill = if vertical {
                    fill.w_full().h(relative(fraction))
                } else {
                    fill.h_full().w(relative(fraction))
                };
                bar = bar.child(fill);
            }
            if let Some(custom) = &dot_render {
                button = button.child(custom(&CarouselDotContext { index, active }, window, cx));
            } else {
                button = button.child(bar);
            }
            let disabled = config.disabled;
            let target = state.clone();
            let key = state.clone();
            let a11y = state.clone();
            let hovered = presentation.clone();
            let hover_state = state.clone();
            let down_focus = focus.clone();
            button = button
                .on_mouse_down(MouseButton::Left, move |_, window, cx| {
                    if !disabled {
                        down_focus.focus(window, cx);
                    }
                    cx.stop_propagation();
                })
                .on_click(move |_, window, cx| {
                    target.update(cx, |s, cx| {
                        s.activate(index, CarouselSource::Dot, false, window, cx)
                    });
                    cx.stop_propagation();
                })
                .on_key_down(move |event, window, cx| {
                    if event.keystroke.key == "enter" || event.keystroke.key == "space" {
                        key.update(cx, |s, cx| {
                            s.activate(index, CarouselSource::Keyboard, false, window, cx)
                        });
                        window.prevent_default();
                        cx.stop_propagation();
                    }
                })
                .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
                    a11y.update(cx, |s, cx| {
                        s.activate(index, CarouselSource::Accessibility, false, window, cx)
                    })
                })
                .on_hover(move |value, _, cx| {
                    if let Some(dot) = hovered.borrow_mut().dots.get_mut(&index) {
                        dot.hovered = *value;
                    }
                    hover_state.update(cx, |_, cx| cx.notify());
                });
            button.style().refine(&styles.dot);
            let mut accessibility = Accessibility::new(Role::Button).label(
                config
                    .locale
                    .carousel
                    .go_to_slide
                    .replace("{index}", &(index + 1).to_string()),
            );
            accessibility.disabled = config.disabled;
            if active {
                accessibility.current = Some(accesskit::AriaCurrent::True);
            }
            dots = dots.child(record_control(
                config
                    .focus_ring()
                    .radius(px(tokens.dot_height as f32))
                    .apply(accessibility.apply(button), &focus),
                state.clone(),
                ControlGeometry::Dot(index, gap),
            ));
        }
        if vertical {
            dots = dots.mt(px(-total / 2.));
        }
        dots.style().refine(&styles.dots);
        output.elements.push(dots.into_any_element());
    }
    output
}
