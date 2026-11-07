// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native input verification surface. `--capture path.png` renders in a hidden
//! window and exits. The interactive mode also exercises the platform IME.

#[path = "input/navigation.rs"]
mod navigation;

use std::{path::PathBuf, rc::Rc, time::Duration};

use ant_design_gpui::{
    Button, ConfigScope, CountConfig, Direction, ExactDecimal, Input, InputGroup, InputNumber,
    InputNumberState, InputState, NumberOptions, OtpInput, OtpOptions, OtpState, PasswordInput,
    SearchInput, Size, TextArea, UiRoot, ValidationStatus, Variant,
    input::InputEvent,
    theme::{AlgorithmChain, ThemeConfig},
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, Subscription, Window,
    WindowBounds, WindowOptions, div, prelude::*, px, size,
};

struct Probe {
    scope: ConfigScope,
    states: Vec<Entity<InputState>>,
    numbers: Vec<Entity<InputNumberState>>,
    codes: Vec<Entity<OtpState>>,
    _scope: Subscription,
    _input_events: Subscription,
}

impl Probe {
    fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let states: Vec<_> = (0..22).map(|index| {
            cx.new(|cx| {
                let mut state = InputState::new(window, cx);
                if index >= 12 { state.set_multiline(true, window, cx); }
                state.set_value(match index {
                    0 => "Native text input",
                    1 => "中文输入 · e\u{301} · 👩🏽‍💻",
                    2 => "Mixed עברית العربية text",
                    12 => "First line\n第二行 · Unicode\nThird line",
                    13 => "A long note wraps naturally within its available width. 中文输入支持多行。 Select text and use the keyboard to edit this note.\nScroll to read the remaining lines.\nDrag the scrollbar without losing your selection.\nThe caret stays visible while editing.\nThe counter remains below the editor.\nLast line.",
                    14 => "Read-only value",
                    15 => "Disabled value",
                    16 => "密码 · secret",
                    17 => "Search Ant Design",
                    18 => "1200",
                    19 => "+86",
                    20 => "138 0013 8000",
                    21 => "Joined search",
                    _ => "",
                }, window, cx);
                state
            })
        }).collect();
        Self {
            _input_events: cx.subscribe_in(&states[0], window, |_, _, event, window, _| {
                if let InputEvent::CompositionChanged(active) = event {
                    window.set_window_title(if *active {
                        "Ant Design GPUI · Input verification · composing"
                    } else {
                        "Ant Design GPUI · Input verification"
                    });
                }
            }),
            _scope: scope.observe(cx),
            scope,
            states,
            codes: (0..2)
                .map(|index| {
                    cx.new(|cx| {
                        let mut state = OtpState::with_options(
                            OtpOptions {
                                mask: (index == 1).then(|| "●".into()),
                                ..Default::default()
                            },
                            window,
                            cx,
                        )
                        .unwrap();
                        state.set_value(
                            if index == 0 {
                                "123456"
                            } else {
                                "密码😀987"
                            },
                            window,
                            cx,
                        );
                        state
                    })
                })
                .collect(),
            numbers: (0..4)
                .map(|index| {
                    cx.new(|cx| {
                        let options = match index {
                            1 => NumberOptions {
                                step: "0.001".parse().unwrap(),
                                ..NumberOptions::exact()
                            },
                            2 => NumberOptions {
                                precision: Some(2),
                                parser: Some(Rc::new(|input| input.replace(['$', ','], ""))),
                                formatter: Some(Rc::new(|context| {
                                    if context.user_typing {
                                        context.input.to_owned()
                                    } else if context.value.is_some() {
                                        format!("$ {}", context.canonical)
                                    } else {
                                        String::new()
                                    }
                                })),
                                ..NumberOptions::exact()
                            },
                            _ => NumberOptions::default(),
                        };
                        let mut state =
                            InputNumberState::with_options(options, window, cx).unwrap();
                        state
                            .set_value(
                                Some(
                                    match index {
                                        0 => "3",
                                        1 => "9007199254740993.001",
                                        _ => "1234.5",
                                    }
                                    .parse::<ExactDecimal>()
                                    .unwrap()
                                    .into(),
                                ),
                                window,
                                cx,
                            )
                            .unwrap();
                        state
                    })
                })
                .collect(),
        }
    }
}

impl Render for Probe {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let mut content = div()
            .flex()
            .flex_col()
            .gap(px(18.))
            .p(px(28.))
            .w_full()
            .child(div().text_size(px(22.)).child("Native input verification"));
        for (row, variant) in [
            Variant::Outlined,
            Variant::Filled,
            Variant::Borderless,
            Variant::Underlined,
        ]
        .into_iter()
        .enumerate()
        {
            let mut controls = div().flex().items_center().gap(px(16.));
            for (col, size) in [Size::Small, Size::Medium, Size::Large]
                .into_iter()
                .enumerate()
            {
                let index = row * 3 + col;
                controls = controls.child(
                    div().flex_1().min_w_0().child(
                        Input::new(index, &self.states[index])
                            .scope(self.scope.clone())
                            .size(size)
                            .variant(variant)
                            .placeholder(format!("{size:?}"))
                            .accessibility_label(format!("{variant:?} {size:?}"))
                            .automation_id(format!("input.probe.{index}")),
                    ),
                );
            }
            content = content.child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(6.))
                    .child(format!("{variant:?}"))
                    .child(controls),
            );
        }
        content = content.child(
            div()
                .flex()
                .gap(px(16.))
                .child(
                    div().flex_1().min_w_0().child(
                        PasswordInput::new(16, &self.states[16])
                            .scope(self.scope.clone())
                            .allow_clear(true)
                            .accessibility_label("Password")
                            .automation_id("input.probe.password"),
                    ),
                )
                .child(
                    div().flex_1().min_w_0().child(
                        SearchInput::new(17, &self.states[17])
                            .scope(self.scope.clone())
                            .allow_clear(true)
                            .enter_button("Search")
                            .accessibility_label("Search")
                            .automation_id("input.probe.search"),
                    ),
                )
                .child(
                    div().flex_1().min_w_0().child(
                        Input::new(18, &self.states[18])
                            .scope(self.scope.clone())
                            .prefix("￥")
                            .suffix("元")
                            .allow_clear(true)
                            .count(CountConfig::new().maximum(8))
                            .accessibility_label("Amount")
                            .automation_id("input.probe.affixes"),
                    ),
                ),
        );
        content = content.child(div().flex().gap(px(16.)).children([12, 13].map(|index| {
            div().flex_1().min_w_0().child(
                TextArea::new(index, &self.states[index])
                    .scope(self.scope.clone())
                    .allow_clear(true)
                    .show_count(true)
                    .auto_size(2, 5)
                    .status(if index == 12 {
                        ValidationStatus::Warning
                    } else {
                        ValidationStatus::Error
                    })
                    .accessibility_label("Multiline note")
                    .automation_id(format!("input.probe.{index}")),
            )
        })));
        content = content.child(div().flex().gap(px(16.)).children((0..3).map(|index| {
            div().flex_1().min_w_0().child(
                InputNumber::new(("number", index), &self.numbers[index], cx)
                    .scope(self.scope.clone())
                    .block(true)
                    .controls_visible(true)
                    .size([Size::Small, Size::Medium, Size::Large][index])
                    .variant([Variant::Outlined, Variant::Filled, Variant::Underlined][index])
                    .accessibility_label(["Quantity", "Exact decimal", "Currency"][index])
                    .automation_id(format!("input.probe.number.{index}")),
            )
        })));
        content = content.child(div().flex().gap(px(36.)).children((0..2).map(|index| {
            OtpInput::new(("code", index), &self.codes[index])
                .scope(self.scope.clone())
                .size(if index == 0 {
                    Size::Medium
                } else {
                    Size::Large
                })
                .variant(if index == 0 {
                    Variant::Outlined
                } else {
                    Variant::Filled
                })
                .separator(|after| (after == 2).then(|| div().child("-").into_any_element()))
                .accessibility_label(if index == 0 {
                    "Verification code"
                } else {
                    "Masked verification code"
                })
                .automation_id(format!("input.probe.code.{index}"))
        })));
        content = content.child(
            div()
                .flex()
                .gap(px(16.))
                .child(
                    InputGroup::new("phone-group")
                        .scope(self.scope.clone())
                        .flex_1()
                        .accessibility_label("Phone number")
                        .child(
                            Input::new(19, &self.states[19])
                                .w(px(80.))
                                .flex_shrink_0()
                                .accessibility_label("Calling code"),
                        )
                        .child(
                            Input::new(20, &self.states[20])
                                .flex_1()
                                .accessibility_label("Phone number"),
                        )
                        .child(Button::new("phone-check").primary().label("Verify")),
                )
                .child(
                    InputGroup::new("mixed-group")
                        .scope(self.scope.clone())
                        .flex_1()
                        .accessibility_label("Search quantity")
                        .child(
                            InputNumber::new("group-number", &self.numbers[3], cx)
                                .w(px(100.))
                                .flex_shrink_0()
                                .accessibility_label("Quantity"),
                        )
                        .child(
                            SearchInput::new(21, &self.states[21])
                                .flex_1()
                                .accessibility_label("Joined search"),
                        ),
                ),
        );
        content.child(div().flex().gap(px(16.)).children([14, 15].map(|index| {
            div().flex_1().min_w_0().child(
                Input::new(index, &self.states[index])
                    .scope(self.scope.clone())
                    .read_only(index == 14)
                    .disabled(index == 15)
                    .accessibility_label(if index == 14 {
                        "Read-only value"
                    } else {
                        "Disabled value"
                    })
                    .automation_id(format!("input.probe.{index}")),
            )
        })))
    }
}

fn main() {
    let mut capture: Option<PathBuf> = None;
    let mut dark = false;
    let mut compact = false;
    let mut rtl = false;
    let mut variant = Variant::Outlined;
    let mut custom_typography = false;
    let mut verify_navigation = false;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--capture" => capture = Some(args.next().expect("capture path").into()),
            "--dark" => dark = true,
            "--compact" => compact = true,
            "--rtl" => rtl = true,
            "--custom-typography" => custom_typography = true,
            "--variant" => {
                variant = match args.next().as_deref() {
                    Some("outlined") => Variant::Outlined,
                    Some("filled") => Variant::Filled,
                    Some("borderless") => Variant::Borderless,
                    Some("underlined") => Variant::Underlined,
                    _ => panic!("expected outlined, filled, borderless or underlined"),
                }
            }
            "--verify-navigation" => verify_navigation = true,
            _ => panic!("unknown argument: {arg}"),
        }
    }
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| {
                patch.direction = Some(if rtl { Direction::Rtl } else { Direction::Ltr });
                patch.variant = Some(variant);
                let mut theme = ThemeConfig {
                    algorithms: Some(match (dark, compact) {
                        (false, false) => AlgorithmChain::default(),
                        (true, false) => AlgorithmChain::dark(),
                        (false, true) => AlgorithmChain::compact(),
                        (true, true) => AlgorithmChain::dark_compact(),
                    }),
                    ..Default::default()
                };
                if custom_typography {
                    theme.components.set::<ant_design_gpui::input::InputTheme>(
                        ant_design_gpui::input::InputTokenPatch {
                            input_font_size: Some(18.),
                            input_font_size_lg: Some(21.),
                            ..Default::default()
                        },
                    );
                }
                patch.theme = Some(theme);
            })
            .expect("valid theme");
        let hidden = capture.is_some() || verify_navigation;
        let viewport_size = size(px(1080.), px(if custom_typography { 1040. } else { 840. }));
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                    None,
                    viewport_size,
                    cx,
                ))),
                show: !hidden,
                focus: !hidden,
                ..Default::default()
            },
            |window, cx| {
                window.set_window_title("Ant Design GPUI · Input verification");
                // Hidden Win32 windows defer their initial placement; initialize
                // the client surface explicitly before the capture draw.
                window.resize(viewport_size);
                let probe = cx.new(|cx| Probe::new(scope.clone(), window, cx));
                let root = cx.new(|cx| UiRoot::new(scope, probe.clone(), window, cx));
                if verify_navigation {
                    window
                        .spawn(cx, async move |cx| {
                            cx.background_executor()
                                .timer(Duration::from_millis(300))
                                .await;
                            cx.update(|window, cx| {
                                window.draw(cx).clear(cx);
                                let states = probe.read(cx).states.clone();
                                navigation::verify(&states, window, cx);
                                cx.quit();
                            })
                            .expect("verification window exists");
                        })
                        .detach();
                } else if let Some(path) = capture {
                    window
                        .spawn(cx, async move |cx| {
                            cx.background_executor()
                                .timer(Duration::from_millis(300))
                                .await;
                            cx.update(|window, cx| {
                                window.draw(cx).clear(cx);
                                window
                                    .render_to_image()
                                    .expect("render input verification")
                                    .save(&path)
                                    .expect("save input verification");
                                cx.quit();
                            })
                            .expect("verification window exists");
                        })
                        .detach();
                } else {
                    window.activate_window();
                }
                root
            },
        )
        .expect("open input verification window");
    });
}
