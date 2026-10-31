// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT).

use std::collections::BTreeMap;

use ant_design_gpui::{
    Checkbox, ConfigScope, InputNumber, InputNumberEvent, InputNumberState, NumberOptions, Slider,
    SliderEvent, SliderOptions, SliderState, SliderValue,
    input_number::NumericValue,
    slider::{SliderMark, SliderOrientation, SliderRange, SliderStyles, SliderTooltip},
    theme::Color,
};
use gpui::{
    AnyElement, AppContext, BoxShadow, Context, Entity, IntoElement, Render, ScrollAnchor,
    Subscription, Window, div, linear_color_stop, linear_gradient, point, prelude::*, px,
};

use super::input_examples::{self as examples, Coverage, Demo, column, row};

const fn demo(id: &'static str, title: &'static str, description: &'static str) -> Demo {
    Demo {
        id,
        title,
        description,
        coverage: Coverage::Complete,
    }
}
pub(super) const DEMOS: &[Demo] = &[
    demo(
        "basic",
        "Basic",
        "Single and range sliders, with a shared disabled toggle.",
    ),
    demo(
        "input-number",
        "Slider with InputNumber",
        "Integer and fractional values stay synchronized in both directions.",
    ),
    demo(
        "icon-slider",
        "Slider with icon",
        "The value changes the emphasis of the surrounding icons.",
    ),
    demo(
        "tip-formatter",
        "Customize tooltip",
        "Format a value as a percentage, or disable its tooltip.",
    ),
    demo(
        "event",
        "Event",
        "Change events follow each edit; completion occurs on pointer or key release.",
    ),
    demo(
        "mark",
        "Graduated slider",
        "Marks, independent points, dots, and a marks-only scale.",
    ),
    demo(
        "vertical",
        "Vertical",
        "Single, range, and marked vertical sliders.",
    ),
    demo(
        "show-tooltip",
        "Control visibility of Tooltip",
        "Keep the tooltip visible even when the slider is idle.",
    ),
    demo("reverse", "Reverse", "Reverse single and range scales."),
    demo(
        "draggableTrack",
        "Draggable track",
        "Drag the selected track to move both handles together.",
    ),
    demo(
        "multiple",
        "Multiple handles",
        "Any number of ordered handles can share the same scale.",
    ),
    demo(
        "editable",
        "Dynamic edit nodes",
        "Click the rail to add a node; drag a handle 130px away to delete it. Keep two to five nodes.",
    ),
    demo(
        "disabled-handle",
        "Disabled per handle",
        "The first and third handles are disabled; the middle handle remains editable.",
    ),
    demo(
        "style-class",
        "Custom semantic styling",
        "Gradient tracks and handle shadows, including a vertical style callback.",
    ),
];

pub(super) struct SliderPage {
    scope: ConfigScope,
    pub states: BTreeMap<&'static str, Entity<SliderState>>,
    pub numbers: BTreeMap<&'static str, Entity<InputNumberState>>,
    pub changes: usize,
    pub completions: usize,
    pub disabled: bool,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
    _events: Vec<Subscription>,
}

impl SliderPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let extras = [
            "basic.range",
            "input-number.fraction",
            "tip-formatter.hidden",
            "mark.independent",
            "mark.dots",
            "mark.only",
            "vertical.range",
            "vertical.marks",
            "reverse.range",
            "style.vertical",
        ];
        let states: BTreeMap<_, _> = DEMOS
            .iter()
            .map(|d| d.id)
            .chain(extras)
            .map(|key| {
                let range = key.ends_with(".range")
                    || matches!(
                        key,
                        "draggableTrack" | "multiple" | "editable" | "disabled-handle"
                    );
                let vertical = key.starts_with("vertical") || key == "style.vertical";
                let marked = key.starts_with("mark") || key == "vertical.marks";
                let options = SliderOptions {
                    min: if key == "input-number" { 1. } else { 0. },
                    max: if key == "input-number" {
                        20.
                    } else if key == "input-number.fraction" {
                        1.
                    } else {
                        100.
                    },
                    step: if key == "mark.only" {
                        None
                    } else if key == "input-number.fraction" {
                        Some(0.01)
                    } else if key == "mark.dots" {
                        Some(10.)
                    } else {
                        Some(1.)
                    },
                    range: range.then(|| SliderRange {
                        draggable_track: key == "draggableTrack",
                        editable: key == "editable",
                        min_count: if key == "editable" { 2 } else { 0 },
                        max_count: (key == "editable").then_some(5),
                    }),
                    orientation: if vertical {
                        SliderOrientation::Vertical
                    } else {
                        SliderOrientation::Horizontal
                    },
                    marks: if marked {
                        vec![0., 26., 37., 100.]
                    } else {
                        vec![]
                    },
                    reverse: key.starts_with("reverse") || key == "style.vertical",
                    included: key != "mark.independent",
                    dots: key == "mark.dots",
                    disabled: if key == "disabled-handle" {
                        [true, false, true].into()
                    } else {
                        false.into()
                    },
                    ..Default::default()
                };
                let initial: SliderValue =
                    if matches!(key, "multiple" | "editable" | "disabled-handle") {
                        [20., 50., 80.].into()
                    } else if range {
                        [20., 50.].into()
                    } else if key == "input-number" {
                        1.0.into()
                    } else if key == "input-number.fraction" {
                        0.0.into()
                    } else {
                        30.0.into()
                    };
                (
                    key,
                    cx.new(|cx| {
                        let mut state = SliderState::with_options(options, window, cx).unwrap();
                        state.set_value(initial, window, cx).unwrap();
                        state
                    }),
                )
            })
            .collect();
        let numbers: BTreeMap<_, _> = [
            ("input-number", "1", "20", "1"),
            ("input-number.fraction", "0", "1", "0.01"),
        ]
        .into_iter()
        .map(|(key, min, max, step)| {
            let state = cx.new(|cx| {
                let options = NumberOptions {
                    minimum: Some(min.parse().unwrap()),
                    maximum: Some(max.parse().unwrap()),
                    step: step.parse().unwrap(),
                    ..Default::default()
                };
                let mut state = InputNumberState::with_options(options, window, cx).unwrap();
                state
                    .set_value(Some(NumericValue::Number(min.parse().unwrap())), window, cx)
                    .unwrap();
                state
            });
            (key, state)
        })
        .collect();
        let mut events: Vec<_> = states
            .iter()
            .map(|(&key, state)| {
                cx.subscribe_in(
                    state,
                    window,
                    move |page: &mut Self, _, event, window, cx| match event {
                        SliderEvent::Change(change) => {
                            if key == "event" {
                                page.changes += 1;
                            }
                            if let Some(number) = page.numbers.get(key) {
                                let value = change.value.as_slice()[0];
                                number.update(cx, |state, cx| {
                                    state
                                        .set_value(Some(NumericValue::Number(value)), window, cx)
                                        .unwrap()
                                });
                            }
                            cx.notify();
                        }
                        SliderEvent::ChangeComplete(_) => {
                            if key == "event" {
                                page.completions += 1;
                                cx.notify();
                            }
                        }
                        SliderEvent::Focus { .. } | SliderEvent::Blur { .. } => {}
                    },
                )
            })
            .collect();
        events.extend(numbers.iter().map(|(&key, state)| {
            cx.subscribe_in(
                state,
                window,
                move |page: &mut Self, _, event, window, cx| {
                    if let InputNumberEvent::Change(change) = event
                        && let Some(value) = change
                            .value
                            .as_ref()
                            .and_then(|value| value.to_number().ok())
                    {
                        page.states[key]
                            .update(cx, |state, cx| state.set_value(value, window, cx).unwrap());
                    }
                },
            )
        }));
        Self {
            _config: scope.observe(cx),
            scope,
            states,
            numbers,
            changes: 0,
            completions: 0,
            disabled: false,
            anchors,
            _events: events,
        }
    }

    fn slider(&self, key: &'static str) -> Slider {
        Slider::new(key, &self.states[key])
            .scope(self.scope.clone())
            .accessibility_label(key)
            .automation_id(format!("gallery.slider.{key}"))
    }
    fn marks(&self, key: &'static str) -> Slider {
        let red = div().text_color(Color::rgb(0xff5500)).style().clone();
        self.slider(key).marks([
            SliderMark::new(0., "0°C"),
            SliderMark::new(26., "26°C"),
            SliderMark::new(37., "37°C"),
            SliderMark::new(100., "100°C")
                .style(red)
                .content(|_, _| div().font_weight(gpui::FontWeight::SEMIBOLD).child("100°C")),
        ])
    }
    fn preview(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        match id {
            "basic" => column()
                .child(self.slider(id).disabled(self.disabled))
                .child(self.slider("basic.range").disabled(self.disabled))
                .child(
                    Checkbox::new("disable-sliders")
                        .label("Disabled")
                        .scope(self.scope.clone())
                        .checked(self.disabled)
                        .on_change(cx.listener(
                            |page, change: &ant_design_gpui::CheckboxChange, _, cx| {
                                page.disabled = change.checked;
                                cx.notify();
                            },
                        )),
                )
                .into_any_element(),
            "input-number" => column()
                .children(["input-number", "input-number.fraction"].map(|key| {
                    row(&self.scope, cx).child(self.slider(key)).child(
                        InputNumber::new(key, &self.numbers[key], cx)
                            .scope(self.scope.clone())
                            .accessibility_label(key)
                            .w(px(90.)),
                    )
                }))
                .into_any_element(),
            "icon-slider" => {
                let value = self.states[id].read(cx).value().as_slice()[0];
                let tokens = &self.scope.snapshot(cx).theme.tokens;
                row(&self.scope, cx)
                    .child(
                        div()
                            .text_color(if value < 50. {
                                tokens.color_primary
                            } else {
                                tokens.color_text_quaternary
                            })
                            .child("☹"),
                    )
                    .child(self.slider(id))
                    .child(
                        div()
                            .text_color(if value >= 50. {
                                tokens.color_primary
                            } else {
                                tokens.color_text_quaternary
                            })
                            .child("☺"),
                    )
                    .into_any_element()
            }
            "tip-formatter" => column()
                .child(self.slider(id).tooltip(
                    SliderTooltip::default().formatter(|value, _, _| Some(format!("{value}%"))),
                ))
                .child(self.slider("tip-formatter.hidden").tooltip(SliderTooltip {
                    enabled: false,
                    ..Default::default()
                }))
                .into_any_element(),
            "event" => column()
                .child(self.slider(id))
                .child(format!(
                    "Changes: {} · Completions: {}",
                    self.changes, self.completions
                ))
                .into_any_element(),
            "mark" => column()
                .gap(px(32.))
                .pb(px(24.))
                .children(
                    ["mark", "mark.independent", "mark.dots", "mark.only"]
                        .map(|key| self.marks(key)),
                )
                .into_any_element(),
            "vertical" => row(&self.scope, cx)
                .h(px(300.))
                .gap(px(48.))
                .child(self.slider(id))
                .child(self.slider("vertical.range"))
                .child(self.marks("vertical.marks"))
                .into_any_element(),
            "show-tooltip" => div()
                .pt(px(32.))
                .child(self.slider(id).tooltip(SliderTooltip {
                    open: Some(true),
                    ..Default::default()
                }))
                .into_any_element(),
            "reverse" => column()
                .child(self.slider(id))
                .child(self.slider("reverse.range"))
                .into_any_element(),
            "style-class" => {
                let style = custom_style(Color::rgb(0x91caff), Color::rgb(0x1677ff));
                column()
                    .child(self.slider(id).styles(style))
                    .child(
                        div()
                            .h(px(300.))
                            .child(self.slider("style.vertical").styles_fn(|context, _, _| {
                                if context.options.orientation == SliderOrientation::Vertical {
                                    custom_style(Color::rgb(0x722cc0), Color::rgb(0x722ed1))
                                } else {
                                    SliderStyles::default()
                                }
                            })),
                    )
                    .into_any_element()
            }
            _ => self.slider(id).into_any_element(),
        }
    }
}

fn custom_style(from: Color, to: Color) -> SliderStyles {
    let track = div()
        .bg(linear_gradient(
            180.,
            linear_color_stop(from, 0.),
            linear_color_stop(to, 1.),
        ))
        .style()
        .clone();
    let handle = div()
        .border_color(to)
        .shadow(vec![BoxShadow {
            color: to.into(),
            offset: point(px(0.), px(2.)),
            blur_radius: px(8.),
            spread_radius: px(0.),
            inset: false,
        }])
        .style()
        .clone();
    SliderStyles {
        track,
        handle,
        ..Default::default()
    }
}

impl Render for SliderPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.slider.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "slider",
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
