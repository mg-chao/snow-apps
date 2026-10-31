// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use std::{collections::BTreeMap, rc::Rc};

use ant_design_gpui::{
    Button, Checkbox, ConfigPatch, ConfigScope, ExactDecimal, InputNumber, InputNumberEvent,
    InputNumberState, NumberOptions, Size, ValidationStatus, Variant,
    icons::IconName,
    input::{InputTokenPatch, TextSelection},
    input_number::{InputNumberTheme, InputNumberTokenPatch},
    theme::{Color, ComponentThemeConfig, ThemeConfig},
};
use gpui::{
    AnyElement, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, ScrollHandle,
    Subscription, Window, div, prelude::*, px,
};

use super::input_examples::{self as examples, Coverage, Demo, column, control, decoration, row};

// Public <code> entries from components/input-number/index.en-US.md.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "basic",
        title: "Basic",
        description: "Numeric-only input box.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "size",
        title: "Sizes",
        description: "The default height is 32px. Large and small sizes are 40px and 24px, respectively.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "disabled",
        title: "Disabled",
        description: "Click the button to toggle between available and disabled states.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "digit",
        title: "High precision decimals",
        description: "Use exact decimal mode to preserve high precision values and changes.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "formatter",
        title: "Formatter",
        description: "Display a formatted value and use a parser to extract the numeric value.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "keyboard",
        title: "Keyboard",
        description: "Control keyboard behavior.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "change-on-wheel",
        title: "Wheel",
        description: "Control with mouse wheel.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "variant",
        title: "Variants",
        description: "Variants of InputNumber: outlined, filled, borderless and underlined.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "spinner",
        title: "Spinner",
        description: "Digit spinner.",
        coverage: Coverage::Planned("Spinner presentation mode requires an InputNumber mode API."),
    },
    Demo {
        id: "out-of-range",
        title: "Out of range",
        description: "Show warning styling when an externally controlled value is out of range.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "presuffix",
        title: "Prefix / Suffix",
        description: "Add a prefix or suffix inside input.",
        coverage: Coverage::Partial("The user-icon addon row requires an addon component."),
    },
    Demo {
        id: "status",
        title: "Status",
        description: "Add error or warning status to InputNumber.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "focus",
        title: "Focus",
        description: "Focus with additional options.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "style-class",
        title: "Custom semantic dom styling",
        description: "Customize native root styles and component tokens in a local scope.",
        coverage: Coverage::Partial(
            "Independent styles for internal parts and styles callbacks are pending. Root refinements and scoped font tokens are shown.",
        ),
    },
];

pub(super) struct InputNumberPage {
    pub scope: ConfigScope,
    pub numbers: BTreeMap<&'static str, Entity<InputNumberState>>,
    pub disabled: bool,
    pub keyboard: bool,
    pub changes: usize,
    pub steps: usize,
    pub feedback: BTreeMap<&'static str, String>,
    style_scope: ConfigScope,
    scroll: ScrollHandle,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
    _events: Vec<Subscription>,
}

fn decimal(value: &str) -> ExactDecimal {
    value.parse().expect("valid reference decimal")
}

pub(super) fn currency(value: &str) -> String {
    let (integer, fraction) = value
        .split_once('.')
        .map_or((value, None), |(a, b)| (a, Some(b)));
    let (sign, digits) = integer
        .strip_prefix('-')
        .map_or(("", integer), |digits| ("-", digits));
    let mut grouped = String::new();
    for (index, digit) in digits.chars().enumerate() {
        if index > 0 && (digits.len() - index).is_multiple_of(3) {
            grouped.push(',');
        }
        grouped.push(digit);
    }
    match fraction {
        Some(fraction) => format!("$ {sign}{grouped}.{fraction}"),
        None => format!("$ {sign}{grouped}"),
    }
}

impl InputNumberPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        scroll: ScrollHandle,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        assert_eq!(anchors.len(), DEMOS.len());
        let keys = [
            "basic",
            "size.large",
            "size.medium",
            "size.small",
            "disabled",
            "digit",
            "formatter.currency",
            "formatter.percent",
            "keyboard",
            "wheel",
            "variant.outlined",
            "variant.filled",
            "variant.borderless",
            "variant.underlined",
            "out-of-range",
            "prefix.currency",
            "prefix.disabled",
            "prefix.suffix",
            "status.error",
            "status.warning",
            "status.error-prefix",
            "status.warning-prefix",
            "focus",
            "style.object",
            "style.function",
        ];
        let numbers: BTreeMap<_, _> = keys
            .into_iter()
            .map(|key| {
                let bounded = matches!(
                    key,
                    "basic" | "disabled" | "keyboard" | "wheel" | "out-of-range"
                );
                let mut options = if key == "digit" {
                    NumberOptions::exact()
                } else {
                    NumberOptions::default()
                };
                if bounded || key.starts_with("size.") {
                    options.minimum = Some(decimal("1"));
                    options.maximum = Some(decimal(if key.starts_with("size.") {
                        "100000"
                    } else {
                        "10"
                    }));
                }
                if key == "digit" {
                    options.minimum = Some(decimal("0"));
                    options.maximum = Some(decimal("10"));
                    options.step = decimal("0.00000000000001");
                    // Keep the reference's initial "1" while preserving exact stepping.
                    options.formatter = Some(Rc::new(|context| {
                        if context.user_typing {
                            context.input.to_owned()
                        } else if context.canonical.contains('.') {
                            context
                                .canonical
                                .trim_end_matches('0')
                                .trim_end_matches('.')
                                .to_owned()
                        } else {
                            context.canonical.to_owned()
                        }
                    }));
                }
                options.change_on_wheel = key == "wheel";
                if key == "formatter.currency" {
                    options.parser = Some(Rc::new(|input| input.replace(['$', ',', ' '], "")));
                    options.formatter = Some(Rc::new(|context| {
                        if context.user_typing {
                            context.input.to_owned()
                        } else if context.value.is_some() {
                            currency(context.canonical)
                        } else {
                            String::new()
                        }
                    }));
                }
                if key == "formatter.percent" {
                    options.minimum = Some(decimal("0"));
                    options.maximum = Some(decimal("100"));
                    options.parser = Some(Rc::new(|input| input.replace('%', "")));
                    options.formatter = Some(Rc::new(|context| {
                        if context.user_typing {
                            context.input.to_owned()
                        } else if context.value.is_some() {
                            format!("{}%", context.canonical)
                        } else {
                            String::new()
                        }
                    }));
                }
                let initial = match key {
                    "digit" => Some("1"),
                    "formatter.currency" => Some("1000"),
                    "formatter.percent" => Some("100"),
                    "out-of-range" => Some("99"),
                    "focus" => Some("999"),
                    _ if bounded || key.starts_with("size.") => Some("3"),
                    _ => None,
                };
                let state = cx.new(|cx| {
                    let mut state = InputNumberState::with_options(options, window, cx)
                        .expect("valid reference number options");
                    if let Some(value) = initial {
                        state
                            .set_value(Some(decimal(value).into()), window, cx)
                            .unwrap();
                    }
                    state
                });
                (key, state)
            })
            .collect();
        let events = numbers
            .iter()
            .map(|(&key, state)| {
                cx.subscribe(state, move |this: &mut Self, _, event, cx| {
                    let message = match event {
                        InputNumberEvent::Change(change) => {
                            this.changes += 1;
                            format!(
                                "Value: {}",
                                change
                                    .value
                                    .as_ref()
                                    .map_or("empty".to_owned(), ToString::to_string)
                            )
                        }
                        InputNumberEvent::Step(step) => {
                            this.steps += 1;
                            format!(
                                "Step {:?} by {} ({:?})",
                                step.direction, step.offset, step.source
                            )
                        }
                        InputNumberEvent::Commit(commit) => format!(
                            "Committed: {}",
                            commit
                                .value
                                .as_ref()
                                .map_or("empty".to_owned(), ToString::to_string)
                        ),
                        _ => return,
                    };
                    this.feedback
                        .insert(key.split('.').next().unwrap(), message);
                    cx.notify();
                })
            })
            .collect();
        let mut theme = ThemeConfig::default();
        theme
            .components
            .set_theme(ComponentThemeConfig::<InputNumberTheme>::new(
                InputNumberTokenPatch {
                    input: InputTokenPatch {
                        input_font_size: Some(14.),
                        ..Default::default()
                    },
                    ..Default::default()
                },
            ));
        let style_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .expect("valid number example styles");
        Self {
            _config: scope.observe(cx),
            scope,
            numbers,
            disabled: true,
            keyboard: true,
            changes: 0,
            steps: 0,
            feedback: BTreeMap::new(),
            style_scope,
            scroll,
            anchors,
            _events: events,
        }
    }
    fn number(&self, key: &'static str, cx: &Context<Self>) -> InputNumber {
        InputNumber::new(key, &self.numbers[key], cx)
            .scope(self.scope.clone())
            .accessibility_label(key)
            .automation_id(format!("input-number.{key}"))
    }
    fn action(&self, id: &'static str, label: &'static str) -> Button {
        Button::new(id)
            .scope(self.scope.clone())
            .label(label)
            .automation_id(id)
    }
    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        let content = match id {
            "basic" => control("input-number.basic", self.number("basic", cx)),
            "size" => row(&self.scope, cx)
                .children(
                    [
                        ("size.large", Size::Large),
                        ("size.medium", Size::Medium),
                        ("size.small", Size::Small),
                    ]
                    .map(|(key, size)| {
                        control(
                            format!("input-number.{key}"),
                            self.number(key, cx).size(size),
                        )
                    }),
                )
                .into_any_element(),
            "disabled" => column()
                .child(self.number("disabled", cx).disabled(self.disabled))
                .child(
                    row(&self.scope, cx).child(control(
                        "input-number.disabled.toggle",
                        self.action("input-number.disabled.toggle", "Toggle disabled")
                            .primary()
                            .on_click(cx.listener(|this, _, window, cx| {
                                this.disabled = !this.disabled;
                                this.numbers["disabled"].update(cx, |state, cx| {
                                    state.set_disabled(this.disabled, window, cx)
                                });
                                cx.notify();
                            })),
                    )),
                )
                .into_any_element(),
            "digit" => self.number("digit", cx).w(px(200.)).into_any_element(),
            "formatter" => row(&self.scope, cx)
                .child(self.number("formatter.currency", cx).w(px(160.)))
                .child(self.number("formatter.percent", cx))
                .into_any_element(),
            "keyboard" => row(&self.scope, cx)
                .child(self.number("keyboard", cx))
                .child(control(
                    "input-number.keyboard.toggle",
                    Checkbox::new("input-number.keyboard.toggle")
                        .scope(self.scope.clone())
                        .checked(self.keyboard)
                        .label("Toggle keyboard")
                        .automation_id("input-number.keyboard.toggle")
                        .on_change(cx.listener(|this, _, window, cx| {
                            this.keyboard = !this.keyboard;
                            this.numbers["keyboard"].update(cx, |state, cx| {
                                let mut options = state.options().clone();
                                options.keyboard = this.keyboard;
                                state.set_options(options, window, cx).unwrap();
                            });
                            cx.notify();
                        })),
                ))
                .into_any_element(),
            "change-on-wheel" => control("input-number.wheel", self.number("wheel", cx)),
            "variant" => column()
                .children(
                    [
                        ("variant.outlined", "Outlined", Variant::Outlined),
                        ("variant.filled", "Filled", Variant::Filled),
                        ("variant.borderless", "Borderless", Variant::Borderless),
                        ("variant.underlined", "Underlined", Variant::Underlined),
                    ]
                    .map(|(key, label, variant)| {
                        self.number(key, cx)
                            .placeholder(label)
                            .variant(variant)
                            .w(px(200.))
                    }),
                )
                .into_any_element(),
            "out-of-range" => row(&self.scope, cx)
                .child(self.number("out-of-range", cx))
                .child(control(
                    "input-number.range.reset",
                    self.action("input-number.range.reset", "Reset")
                        .primary()
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.numbers["out-of-range"].update(cx, |state, cx| {
                                state
                                    .set_value(Some(decimal("99").into()), window, cx)
                                    .unwrap()
                            });
                            cx.notify();
                        })),
                ))
                .into_any_element(),
            "presuffix" => column()
                .child(self.number("prefix.currency", cx).prefix("￥").block(true))
                .child(
                    self.number("prefix.disabled", cx)
                        .prefix("￥")
                        .disabled(true)
                        .block(true),
                )
                .child(self.number("prefix.suffix", cx).suffix("RMB").block(true))
                .into_any_element(),
            "status" => column()
                .children(
                    [
                        ("status.error", ValidationStatus::Error, false),
                        ("status.warning", ValidationStatus::Warning, false),
                        ("status.error-prefix", ValidationStatus::Error, true),
                        ("status.warning-prefix", ValidationStatus::Warning, true),
                    ]
                    .map(|(key, status, prefix)| {
                        self.number(key, cx)
                            .status(status)
                            .block(true)
                            .when(prefix, |number| {
                                number.prefix(decoration(
                                    &self.scope,
                                    IconName::ClockCircleOutlined,
                                    cx,
                                ))
                            })
                    }),
                )
                .into_any_element(),
            "focus" => column()
                .child(
                    row(&self.scope, cx).children(
                        [
                            ("input-number.focus.start", "Focus at first", 0),
                            ("input-number.focus.end", "Focus at last", 1),
                            ("input-number.focus.all", "Focus to select all", 2),
                            (
                                "input-number.focus.prevent-scroll",
                                "Focus prevent scroll",
                                3,
                            ),
                        ]
                        .map(|(id, label, mode)| {
                            control(
                                id,
                                self.action(id, label).on_click(cx.listener(
                                    move |this, _, window, cx| {
                                        let offset = this.scroll.offset();
                                        this.numbers["focus"].update(cx, |state, cx| {
                                            state.focus(window, cx);
                                            match mode {
                                                0 => state.select(
                                                    TextSelection::collapsed(0),
                                                    window,
                                                    cx,
                                                ),
                                                1 => state.select(
                                                    TextSelection::collapsed(state.text(cx).len()),
                                                    window,
                                                    cx,
                                                ),
                                                2 => state.select_all(window, cx),
                                                _ => {}
                                            }
                                        });
                                        if mode == 3 {
                                            this.scroll.set_offset(offset);
                                        }
                                        cx.notify();
                                    },
                                )),
                            )
                        }),
                    ),
                )
                .child(self.number("focus", cx).block(true))
                .into_any_element(),
            "style-class" => column()
                .child(
                    self.number("style.object", cx)
                        .scope(self.style_scope.clone())
                        .placeholder("Object")
                        .w(px(200.))
                        .border_color(self.scope.snapshot(cx).theme.tokens.color_primary)
                        .rounded(px(8.)),
                )
                .child(
                    self.number("style.function", cx)
                        .scope(self.style_scope.clone())
                        .placeholder("Function")
                        .size(Size::Large)
                        .w(px(200.))
                        .border_color(Color::rgb(0x722ed1))
                        .bg(Color::rgb(0xfafafa).with_alpha(0.5))
                        .rounded(px(8.)),
                )
                .into_any_element(),
            "spinner" => div().into_any_element(),
            _ => unreachable!("registered number example"),
        };
        let feedback_key = match id {
            "change-on-wheel" => "wheel",
            "presuffix" => "prefix",
            _ => id,
        };
        column()
            .child(content)
            .when_some(self.feedback.get(feedback_key), |column, feedback| {
                column.child(feedback.clone())
            })
            .into_any_element()
    }
}

impl Render for InputNumberPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.input-number.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "input-number",
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
