// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use std::{collections::BTreeMap, rc::Rc, sync::Arc};

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, CountConfig, Input, InputGroup, InputState, OtpEvent,
    OtpInput, OtpOptions, OtpState, PasswordInput, SearchInput, Size, Switch, TextArea,
    ValidationStatus, Variant,
    icons::{IconName, IconPalette, icon},
    input::{CountStrategy, InputEvent, InputTheme, InputTokenPatch, TextSelection},
    theme::{Color, ComponentThemeConfig, ThemeConfig, TokenPatch},
};
use gpui::{
    AnyElement, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, ScrollHandle,
    Subscription, Window, div, prelude::*, px,
};

use super::input_examples::{self as examples, Coverage, Demo, column, control, decoration, row};

// Public <code> entries from components/input/index.en-US.md, in source order.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "basic",
        title: "Basic usage",
        description: "Basic usage example.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "size",
        title: "Three sizes of Input",
        description: "There are three sizes of an Input box: large (40px), medium (32px) and small (24px).",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "variant",
        title: "Variants",
        description: "Variants of Input: outlined, filled, borderless and underlined.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "compact-style",
        title: "Compact Style",
        description: "Use compact groups to place adjacent controls together.",
        coverage: Coverage::Partial(
            "The addon/search, Select/input and large icon-addon rows require addon and Select components.",
        ),
    },
    Demo {
        id: "search-input",
        title: "Search box",
        description: "Create a search box by grouping an input with a search button.",
        coverage: Coverage::Partial("The https:// addon/search row requires an addon component."),
    },
    Demo {
        id: "search-input-loading",
        title: "Search box with loading",
        description: "Search loading when onSearch.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "textarea",
        title: "TextArea",
        description: "For multi-line input.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "autosize-textarea",
        title: "Autosizing the height to fit the content",
        description: "Automatically adjust TextArea height to its content, optionally between minimum and maximum row counts.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "otp",
        title: "OTP",
        description: "One time password input.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "tooltip",
        title: "Format Tooltip Input",
        description: "Combine Input with a tooltip to display formatted numeric content.",
        coverage: Coverage::Planned(
            "The focus-triggered numeric tooltip composition requires a Tooltip component.",
        ),
    },
    Demo {
        id: "presuffix",
        title: "prefix and suffix",
        description: "Add prefix or suffix icons inside an input, including a suffix on Password.",
        coverage: Coverage::Partial(
            "The information icon is shown; its extra-information tooltip requires a Tooltip component.",
        ),
    },
    Demo {
        id: "password-input",
        title: "Password box",
        description: "Input type of password.",
        coverage: Coverage::Partial(
            "The custom visibility-icon row requires a password icon-rendering API.",
        ),
    },
    Demo {
        id: "allowClear",
        title: "With clear icon",
        description: "Click the remove icon to delete everything.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "show-count",
        title: "With character counting",
        description: "Show character counting.",
        coverage: Coverage::Partial(
            "Manual drag resizing of TextArea is pending; counted and fixed-height fields are shown.",
        ),
    },
    Demo {
        id: "advance-count",
        title: "Custom count logic",
        description: "Customize counting for scenarios such as emoji. An advisory maximum can be exceeded; an editing limit prevents additional characters.",
        coverage: Coverage::Complete,
    },
    Demo {
        id: "status",
        title: "Status",
        description: "Add error or warning status to Input.",
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
            "Independent styles for internal parts and styles callbacks are pending. These previews use existing root refinements and scoped tokens.",
        ),
    },
];

pub(super) struct InputPage {
    pub scope: ConfigScope,
    pub texts: BTreeMap<&'static str, Entity<InputState>>,
    pub codes: BTreeMap<&'static str, Entity<OtpState>>,
    pub changes: usize,
    pub commits: usize,
    pub searches: usize,
    pub last_search: String,
    pub otp_feedback: String,
    pub submitted: Option<String>,
    pub focus_textarea: bool,
    pub feedback: BTreeMap<&'static str, String>,
    style_scope: ConfigScope,
    scroll: ScrollHandle,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
    _events: Vec<Subscription>,
}

impl InputPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        scroll: ScrollHandle,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        assert_eq!(anchors.len(), DEMOS.len());
        let definitions = [
            ("basic", "", false, None),
            ("size.large", "", false, None),
            ("size.medium", "", false, None),
            ("size.small", "", false, None),
            ("variant.outlined", "", false, None),
            ("variant.filled", "", false, None),
            ("variant.borderless", "", false, None),
            ("variant.underlined", "", false, None),
            ("variant.search", "", false, None),
            ("compact.single", "26888888", false, None),
            ("compact.area", "0571", false, None),
            ("compact.phone", "26888888", false, None),
            ("compact.submit", "Combine input and button", false, None),
            ("search.default", "", false, None),
            ("search.clear", "", false, None),
            ("search.primary", "", false, None),
            ("search.label", "", false, None),
            ("search.suffix", "", false, None),
            ("loading.default", "", false, None),
            ("loading.primary", "", false, None),
            ("loading.label", "", false, None),
            ("textarea.basic", "", true, None),
            ("textarea.limit", "", true, Some(6)),
            ("autosize.free", "", true, None),
            ("autosize.bounded", "", true, None),
            ("autosize.controlled", "", true, None),
            ("prefix.username", "", false, None),
            ("prefix.currency", "", false, None),
            ("prefix.disabled", "", false, None),
            ("prefix.password", "", false, None),
            ("password.default", "", false, None),
            ("password.controlled", "", false, None),
            ("password.disabled", "", false, None),
            ("clear.input", "", false, None),
            ("clear.textarea", "", true, None),
            ("count.input", "", false, Some(20)),
            ("count.textarea", "", true, Some(100)),
            ("count.fixed", "", true, Some(100)),
            ("advance.exceed", "Hello, antd!", false, None),
            ("advance.emoji", "🔥🔥🔥", false, None),
            ("advance.limit", "🔥 antd", false, Some(6)),
            ("status.error", "", false, None),
            ("status.warning", "", false, None),
            ("status.error-prefix", "", false, None),
            ("status.warning-prefix", "", false, None),
            ("focus", "Ant Design love you!", false, None),
            ("style.object", "", false, None),
            ("style.function", "", false, None),
            ("style.textarea", "TextArea", true, None),
            ("style.password", "Password", false, None),
            ("style.search", "", false, None),
        ];
        let texts: BTreeMap<_, _> = definitions
            .into_iter()
            .map(|(key, value, multiline, maximum)| {
                let state = cx.new(|cx| {
                    let mut state = InputState::new(window, cx);
                    state.set_multiline(multiline, window, cx);
                    state.set_count_strategy(
                        if matches!(key, "advance.emoji" | "advance.limit") {
                            CountStrategy::Custom(Arc::new(|text| text.chars().count()))
                        } else {
                            CountStrategy::Utf16
                        },
                        cx,
                    );
                    state.set_max_length(maximum, cx);
                    state.set_value(value, window, cx);
                    state
                });
                (key, state)
            })
            .collect();
        let mut events: Vec<_> = texts
            .iter()
            .map(|(&key, state)| {
                cx.subscribe(state, move |this: &mut Self, _, event, cx| {
                    let message = match event {
                        InputEvent::Change(change) => {
                            if key == "basic" {
                                this.changes += 1;
                            }
                            format!("Value changed: {} characters", change.value.chars().count())
                        }
                        InputEvent::Commit(_) if key == "basic" => {
                            this.commits += 1;
                            "Value committed".to_owned()
                        }
                        InputEvent::Search(search) => {
                            this.searches += 1;
                            this.last_search = format!("{:?}: {}", search.source, search.value);
                            format!("Search: {}", this.last_search)
                        }
                        InputEvent::PasswordVisibilityChanged(visible) => {
                            format!("Password {}", if *visible { "visible" } else { "hidden" })
                        }
                        _ => return,
                    };
                    this.feedback
                        .insert(key.split('.').next().unwrap(), message);
                    cx.notify();
                })
            })
            .collect();
        let codes: BTreeMap<_, _> = [
            "uppercase",
            "disabled",
            "length",
            "filled",
            "mask",
            "separator",
            "colors",
            "style",
        ]
        .into_iter()
        .map(|key| {
            let options = OtpOptions {
                length: if key == "length" { 8 } else { 6 },
                formatter: (key == "uppercase")
                    .then(|| Rc::new(|text: &str| text.to_uppercase()) as _),
                mask: (key == "mask").then(|| "🔒".into()),
                ..Default::default()
            };
            (
                key,
                cx.new(|cx| {
                    OtpState::with_options(options, window, cx).expect("valid reference OTP")
                }),
            )
        })
        .collect();
        for (&key, state) in &codes {
            events.push(cx.subscribe(state, move |this: &mut Self, _, event, cx| {
                match event {
                    OtpEvent::Change(change) => {
                        this.otp_feedback = format!(
                            "{key}: {} of {} cells filled",
                            change.cells.iter().filter(|cell| !cell.is_empty()).count(),
                            change.cells.len()
                        )
                    }
                    OtpEvent::Complete(_) => this.otp_feedback = format!("{key}: code complete"),
                    _ => return,
                }
                cx.notify();
            }));
        }
        let mut theme = ThemeConfig::default();
        theme.components.set_theme(
            ComponentThemeConfig::<InputTheme>::new(InputTokenPatch {
                active_border_color: Some(Color::rgb(0xaaaaaa)),
                hover_border_color: Some(Color::rgb(0xd9d9d9)),
                ..Default::default()
            })
            .with_tokens(TokenPatch {
                color_border: Some(Color::rgb(0xaaaaaa)),
                ..Default::default()
            }),
        );
        let style_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .expect("valid input example styles");
        Self {
            _config: scope.observe(cx),
            scope,
            texts,
            codes,
            changes: 0,
            commits: 0,
            searches: 0,
            last_search: String::new(),
            otp_feedback: String::new(),
            submitted: None,
            focus_textarea: false,
            feedback: BTreeMap::new(),
            style_scope,
            scroll,
            anchors,
            _events: events,
        }
    }

    fn input(&self, key: &'static str, placeholder: &'static str) -> Input {
        Input::new(key, &self.texts[key])
            .scope(self.scope.clone())
            .placeholder(placeholder)
            .accessibility_label(if placeholder.is_empty() {
                key
            } else {
                placeholder
            })
            .automation_id(format!("input.{key}"))
    }
    fn area(&self, key: &'static str, placeholder: &'static str) -> TextArea {
        TextArea::new(key, &self.texts[key])
            .scope(self.scope.clone())
            .placeholder(placeholder)
            .accessibility_label(if placeholder.is_empty() {
                key
            } else {
                placeholder
            })
            .automation_id(format!("input.{key}"))
    }
    fn password(&self, key: &'static str) -> PasswordInput {
        PasswordInput::new(key, &self.texts[key])
            .scope(self.scope.clone())
            .placeholder("input password")
            .accessibility_label(key)
            .automation_id(format!("input.{key}"))
    }
    fn search(&self, key: &'static str) -> SearchInput {
        SearchInput::new(key, &self.texts[key])
            .scope(self.scope.clone())
            .placeholder("input search text")
            .accessibility_label(key)
            .automation_id(format!("input.{key}"))
    }
    fn action(&self, id: &'static str, label: &'static str) -> Button {
        Button::new(id)
            .scope(self.scope.clone())
            .automation_id(id)
            .label(label)
    }
    fn focus_actions(&self, cx: &Context<Self>) -> AnyElement {
        row(&self.scope, cx)
            .children(
                [
                    ("input.focus.start", "Focus at first", 0),
                    ("input.focus.end", "Focus at last", 1),
                    ("input.focus.all", "Focus to select all", 2),
                    ("input.focus.prevent-scroll", "Focus prevent scroll", 3),
                ]
                .map(|(id, label, mode)| {
                    control(
                        id,
                        self.action(id, label)
                            .on_click(cx.listener(move |this, _, window, cx| {
                                let offset = this.scroll.offset();
                                this.texts["focus"].update(cx, |state, cx| {
                                    state.focus(window, cx);
                                    match mode {
                                        0 => state.select(TextSelection::collapsed(0), window, cx),
                                        1 => state.select(
                                            TextSelection::collapsed(state.value().len()),
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
                            })),
                    )
                }),
            )
            .child(control(
                "input.focus.toggle",
                Switch::new("input.focus.toggle")
                    .scope(self.scope.clone())
                    .automation_id("input.focus.toggle")
                    .accessibility_label("Switch Input and TextArea")
                    .checked(!self.focus_textarea)
                    .checked_label("Input")
                    .unchecked_label("TextArea")
                    .on_change(cx.listener(|this, _, window, cx| {
                        this.focus_textarea = !this.focus_textarea;
                        this.texts["focus"].update(cx, |state, cx| {
                            state.set_multiline(this.focus_textarea, window, cx)
                        });
                        cx.notify();
                    })),
            ))
            .into_any_element()
    }

    fn preview(&self, id: &str, cx: &Context<Self>) -> AnyElement {
        match id {
            "basic" => column()
                .child(control("input.basic", self.input("basic", "Basic usage")))
                .child(format!(
                    "{} changes · {} commits",
                    self.changes, self.commits
                ))
                .into_any_element(),
            "size" => column()
                .children(
                    [
                        ("size.large", "large size", Size::Large),
                        ("size.medium", "default size", Size::Medium),
                        ("size.small", "small size", Size::Small),
                    ]
                    .map(|(key, label, size)| {
                        control(
                            format!("input.{key}"),
                            self.input(key, label).size(size).prefix(decoration(
                                &self.scope,
                                IconName::UserOutlined,
                                cx,
                            )),
                        )
                    }),
                )
                .into_any_element(),
            "variant" => column()
                .children(
                    [
                        ("variant.outlined", "Outlined", Variant::Outlined),
                        ("variant.filled", "Filled", Variant::Filled),
                        ("variant.borderless", "Borderless", Variant::Borderless),
                        ("variant.underlined", "Underlined", Variant::Underlined),
                    ]
                    .map(|(key, label, variant)| self.input(key, label).variant(variant)),
                )
                .child(
                    self.search("variant.search")
                        .placeholder("Filled")
                        .variant(Variant::Filled),
                )
                .into_any_element(),
            "compact-style" => column()
                .child(
                    InputGroup::new("compact.single")
                        .scope(self.scope.clone())
                        .block(true)
                        .child(self.input("compact.single", "Phone")),
                )
                .child(
                    InputGroup::new("compact.phone")
                        .scope(self.scope.clone())
                        .block(true)
                        .child(
                            self.input("compact.area", "Area code")
                                .w(gpui::relative(0.2)),
                        )
                        .child(
                            self.input("compact.phone", "Phone number")
                                .flex_1()
                                .min_w_0(),
                        ),
                )
                .child(
                    InputGroup::new("compact.submit")
                        .scope(self.scope.clone())
                        .block(true)
                        .child(
                            self.input("compact.submit", "Combine input and button")
                                .flex_1()
                                .min_w_0(),
                        )
                        .child(
                            self.action("input.compact.submit", "Submit")
                                .primary()
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.submitted = Some(format!(
                                        "Submitted: {}",
                                        this.texts["compact.submit"].read(cx).value()
                                    ));
                                    cx.notify();
                                })),
                        ),
                )
                .when_some(self.submitted.as_ref(), |column, value| {
                    column.child(value.clone())
                })
                .into_any_element(),
            "search-input" => column()
                .child(self.search("search.default").w(px(200.)))
                .child(control(
                    "input.search.clear",
                    self.search("search.clear").allow_clear(true).w(px(200.)),
                ))
                .child(self.search("search.primary").primary_action(true))
                .child(
                    self.search("search.label")
                        .allow_clear(true)
                        .enter_button("Search")
                        .size(Size::Large),
                )
                .child(
                    self.search("search.suffix")
                        .enter_button("Search")
                        .size(Size::Large)
                        .suffix(
                            div().text_color(Color::rgb(0x1677ff)).child(
                                icon(IconName::AudioOutlined)
                                    .size(px(16.))
                                    .palette(IconPalette {
                                        text: Color::rgb(0x1677ff).to_rgba(),
                                        ..self.scope.snapshot(cx).icon_palette
                                    }),
                            ),
                        ),
                )
                .child(format!("{} searches · {}", self.searches, self.last_search))
                .into_any_element(),
            "search-input-loading" => column()
                .child(
                    self.search("loading.default")
                        .placeholder("input search loading default")
                        .loading(true),
                )
                .child(
                    self.search("loading.primary")
                        .placeholder("input search loading with enterButton")
                        .primary_action(true)
                        .loading(true),
                )
                .child(
                    self.search("loading.label")
                        .enter_button("Search")
                        .size(Size::Large)
                        .loading(true),
                )
                .into_any_element(),
            "textarea" => column()
                .child(self.area("textarea.basic", "").rows(4))
                .child(self.area("textarea.limit", "maxLength is 6").rows(4))
                .into_any_element(),
            "autosize-textarea" => column()
                .child(control(
                    "input.autosize.free",
                    self.area("autosize.free", "Autosize height based on content lines")
                        .auto_size(1, usize::MAX),
                ))
                .child(control(
                    "input.autosize.bounded",
                    self.area(
                        "autosize.bounded",
                        "Autosize height with minimum and maximum number of lines",
                    )
                    .auto_size(2, 6),
                ))
                .child(control(
                    "input.autosize.controlled",
                    self.area("autosize.controlled", "Controlled autosize")
                        .auto_size(3, 5),
                ))
                .into_any_element(),
            "otp" => column()
                .children(
                    [
                        ("uppercase", "With formatter (Upcase)"),
                        ("disabled", "With Disabled"),
                        ("length", "With Length (8)"),
                        ("filled", "With variant"),
                        ("mask", "With custom display character"),
                        ("separator", "With custom ReactNode separator"),
                        ("colors", "With custom function separator"),
                    ]
                    .map(|(key, title)| {
                        let mut otp = OtpInput::new(key, &self.codes[key])
                            .scope(self.scope.clone())
                            .accessibility_label(title)
                            .automation_id(format!("input.otp.{key}"))
                            .disabled(key == "disabled");
                        if key == "filled" {
                            otp = otp.variant(Variant::Filled);
                        }
                        if key == "separator" {
                            otp = otp.separator(|index| {
                                Some(
                                    div()
                                        .id(index)
                                        .debug_selector(move || {
                                            format!("input.otp.separator.{index}")
                                        })
                                        .child("/")
                                        .into_any_element(),
                                )
                            });
                        }
                        if key == "colors" {
                            otp = otp.separator(|index| {
                                Some(
                                    div()
                                        .id(index)
                                        .debug_selector(move || format!("input.otp.colors.{index}"))
                                        .text_color(Color::rgb(if index & 1 == 0 {
                                            0x0000ff
                                        } else {
                                            0xff0000
                                        }))
                                        .child("—")
                                        .into_any_element(),
                                )
                            });
                        }
                        column()
                            .gap(px(8.))
                            .child(title)
                            .child(control(format!("input.otp.{key}"), otp))
                            .into_any_element()
                    }),
                )
                .child(self.otp_feedback.clone())
                .into_any_element(),
            "presuffix" => column()
                .child(
                    self.input("prefix.username", "Enter your username")
                        .prefix(decoration(&self.scope, IconName::UserOutlined, cx))
                        .suffix(decoration(&self.scope, IconName::InfoCircleOutlined, cx)),
                )
                .child(self.input("prefix.currency", "").prefix("￥").suffix("RMB"))
                .child(
                    self.input("prefix.disabled", "")
                        .prefix("￥")
                        .suffix("RMB")
                        .disabled(true),
                )
                .child(
                    self.password("prefix.password")
                        .placeholder("input password support suffix")
                        .suffix(decoration(&self.scope, IconName::LockOutlined, cx)),
                )
                .into_any_element(),
            "password-input" => {
                let visible = self.texts["password.controlled"]
                    .read(cx)
                    .password_visible();
                column()
                    .child(self.password("password.default"))
                    .child(
                        row(&self.scope, cx)
                            .child(
                                div()
                                    .flex_1()
                                    .min_w_0()
                                    .child(self.password("password.controlled")),
                            )
                            .child(control(
                                "input.password.toggle",
                                self.action(
                                    "input.password.toggle",
                                    if visible { "Hide" } else { "Show" },
                                )
                                .w(px(80.))
                                .on_click(cx.listener(
                                    |this, _, window, cx| {
                                        this.texts["password.controlled"].update(
                                            cx,
                                            |state, cx| {
                                                state.set_password_visible(
                                                    !state.password_visible(),
                                                    window,
                                                    cx,
                                                )
                                            },
                                        );
                                        cx.notify();
                                    },
                                )),
                            )),
                    )
                    .child(
                        self.password("password.disabled")
                            .placeholder("disabled input password")
                            .disabled(true),
                    )
                    .when_some(self.feedback.get("password"), |column, value| {
                        column.child(value.clone())
                    })
                    .into_any_element()
            }
            "allowClear" => column()
                .child(control(
                    "input.clear.input",
                    self.input("clear.input", "input with clear icon")
                        .allow_clear(true),
                ))
                .child(control(
                    "input.clear.textarea",
                    self.area("clear.textarea", "textarea with clear icon")
                        .allow_clear(true),
                ))
                .when_some(self.feedback.get("clear"), |column, value| {
                    column.child(value.clone())
                })
                .into_any_element(),
            "show-count" => column()
                .gap(px(32.))
                .child(self.input("count.input", "").show_count(true))
                .child(self.area("count.textarea", "can resize").show_count(true))
                .child(
                    self.area("count.fixed", "disable resize")
                        .show_count(true)
                        .h(px(120.)),
                )
                .into_any_element(),
            "advance-count" => column()
                .child(
                    column().gap(px(8.)).child("Exceed Max").child(
                        self.input("advance.exceed", "")
                            .count(CountConfig::new().maximum(10)),
                    ),
                )
                .child(
                    column()
                        .gap(px(8.))
                        .child("Emoji count as length 1")
                        .child(self.input("advance.emoji", "").show_count(true)),
                )
                .child(
                    column().gap(px(8.)).child("Not exceed max").child(
                        self.input("advance.limit", "")
                            .count(CountConfig::new().maximum(6)),
                    ),
                )
                .into_any_element(),
            "status" => column()
                .children(
                    [
                        ("status.error", "Error", ValidationStatus::Error, false),
                        (
                            "status.warning",
                            "Warning",
                            ValidationStatus::Warning,
                            false,
                        ),
                        (
                            "status.error-prefix",
                            "Error with prefix",
                            ValidationStatus::Error,
                            true,
                        ),
                        (
                            "status.warning-prefix",
                            "Warning with prefix",
                            ValidationStatus::Warning,
                            true,
                        ),
                    ]
                    .map(|(key, label, status, prefix)| {
                        self.input(key, label).status(status).when(prefix, |input| {
                            input.prefix(decoration(&self.scope, IconName::ClockCircleOutlined, cx))
                        })
                    }),
                )
                .into_any_element(),
            "focus" => column()
                .child(self.focus_actions(cx))
                .child(control(
                    "input.focus.editor",
                    if self.focus_textarea {
                        self.area("focus", "").into_any_element()
                    } else {
                        self.input("focus", "").into_any_element()
                    },
                ))
                .into_any_element(),
            "style-class" => column()
                .child(
                    self.input("style.object", "Object")
                        .scope(self.style_scope.clone()),
                )
                .child(
                    self.input("style.function", "Function")
                        .scope(self.style_scope.clone())
                        .size(Size::Medium)
                        .border_color(Color::rgb(0x696fc7)),
                )
                .child(
                    self.area("style.textarea", "")
                        .scope(self.style_scope.clone())
                        .show_count(true)
                        .border_color(Color::rgb(0xbde3c3)),
                )
                .child(
                    self.password("style.password")
                        .scope(self.style_scope.clone())
                        .size(Size::Medium)
                        .border_color(Color::rgb(0xf5d3c4)),
                )
                .child(
                    OtpInput::new("style.otp", &self.codes["style"])
                        .scope(self.style_scope.clone())
                        .size(Size::Medium)
                        .accessibility_label("Styled verification code")
                        .automation_id("input.style.otp")
                        .separator(|_| Some(div().child("*").into_any_element())),
                )
                .child(
                    self.search("style.search")
                        .scope(self.style_scope.clone())
                        .size(Size::Large)
                        .placeholder("Search")
                        .border_color(Color::rgb(0x4da8da)),
                )
                .into_any_element(),
            "tooltip" => div().into_any_element(),
            _ => unreachable!("registered input example"),
        }
    }
}

impl Render for InputPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.input.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "input",
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
