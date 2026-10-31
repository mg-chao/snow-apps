// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT).

use std::{collections::BTreeMap, rc::Rc, time::Duration};

use ant_design_gpui::{
    Button, ConfigScope, Input, InputState, Select, SelectEvent, SelectMode, SelectOption,
    SelectOptions, SelectSettings, SelectState, Size, ValidationStatus, Variant,
    popup::PopupPlacement,
    select::{SelectGroup, SelectSearch, SelectSearchField, SelectStyles},
    theme::Color,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, SharedString,
    Subscription, Task, Window, div, prelude::*, px,
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

// Public examples, in the same order as components/select/index.en-US.md.
pub(super) const DEMOS: &[Demo] = &[
    demo(
        "basic",
        "Basic Usage",
        "Single selection, disabled, loading, and clearable controls.",
    ),
    demo(
        "search",
        "Select with search field",
        "Type to search the available choices by label.",
    ),
    demo(
        "search-filter-option",
        "Custom Search",
        "A custom case-insensitive filter matches option labels.",
    ),
    demo(
        "search-multi-field",
        "Multi field search",
        "Search either the label or an additional application field. Try b11.",
    ),
    demo(
        "multiple",
        "Multiple selection",
        "Select several choices; remove them individually or clear the selection.",
    ),
    demo(
        "size",
        "Sizes",
        "Large, middle, and small sizes for single and multiple selection.",
    ),
    demo(
        "option-render",
        "Custom dropdown options",
        "Render rich option content while retaining searchable labels and stable values.",
    ),
    demo(
        "search-sort",
        "Search with sort",
        "Sort the filtered options alphabetically.",
    ),
    demo(
        "tags",
        "Tags",
        "Type a new value and press Enter to create a tag.",
    ),
    demo(
        "optgroup",
        "Option Group",
        "Group related options under a non-selectable heading.",
    ),
    demo(
        "coordinate",
        "Coordinate",
        "Changing the province replaces the city options and selects the first city.",
    ),
    demo(
        "label-in-value",
        "Get value of selected item",
        "Each change carries both the domain value and its display label.",
    ),
    demo(
        "automatic-tokenization",
        "Automatic tokenization",
        "Paste comma-separated values to create several tags at once.",
    ),
    demo(
        "custom-tokenization",
        "Custom tokenization",
        "Paste: \"San Francisco, CA\", New York. Quoted commas stay inside a tag.",
    ),
    demo(
        "select-users",
        "Search and Select Users",
        "Debounced asynchronous search with cancellation. This offline demo uses a local user directory.",
    ),
    demo(
        "suffix",
        "Prefix and Suffix",
        "Add custom content before the selected value or in the suffix slot.",
    ),
    demo(
        "custom-dropdown-menu",
        "Custom dropdown",
        "The popup contains an editable field and an Add item action.",
    ),
    demo(
        "hide-selected",
        "Hide Already Selected",
        "Selected values disappear from the available choices.",
    ),
    demo(
        "variant",
        "Variants",
        "Outlined, filled, borderless, and underlined controls.",
    ),
    demo(
        "custom-tag-render",
        "Custom Tag Render",
        "Colored tags keep the native removal behavior.",
    ),
    demo(
        "custom-label-render",
        "Custom Selected Label Render",
        "Render a fallback label when a selected value is absent from the options.",
    ),
    demo(
        "responsive",
        "Responsive maxTagCount",
        "Resize the window to fit tags into one line and summarize the remaining values.",
    ),
    demo(
        "big-data",
        "Big Data",
        "100,000 retained options with a virtualized popup. Search or jump with the keyboard.",
    ),
    demo("status", "Status", "Error and warning validation states."),
    demo(
        "placement",
        "Placement",
        "Top-start, top-end, bottom-start, and bottom-end popup placement.",
    ),
    demo(
        "maxCount",
        "Max Count",
        "At most three selected values; removing one enables another choice.",
    ),
    demo(
        "style-class",
        "Custom semantic styling",
        "Refine native semantic slots with an object or a state-dependent callback.",
    ),
];

pub(super) fn options(names: &[&str]) -> SelectOptions<String> {
    SelectOptions::new(names.iter().map(|name| {
        SelectOption::new(
            SharedString::from((*name).to_owned()),
            (*name).to_owned(),
            *name,
        )
    }))
    .unwrap()
}

fn standard_options() -> SelectOptions<String> {
    SelectOptions::new([
        SelectOption::new("jack", "jack".to_owned(), "Jack"),
        SelectOption::new("lucy", "lucy".to_owned(), "Lucy"),
        SelectOption::new("disabled", "disabled".to_owned(), "Disabled").disabled(true),
        SelectOption::new("yiminghe", "yiminghe".to_owned(), "Yiminghe"),
    ])
    .unwrap()
}

/// A small quoted-token parser: the example needs no regex or parser dependency.
pub(super) fn quoted_tokens(input: &str) -> Vec<SharedString> {
    let mut result = Vec::new();
    let mut current = String::new();
    let mut quoted = false;
    for ch in input.chars() {
        match ch {
            '"' => quoted = !quoted,
            ',' | '\n' if !quoted => {
                if !current.trim().is_empty() {
                    result.push(current.trim().to_owned().into());
                }
                current.clear();
            }
            _ => current.push(ch),
        }
    }
    if !current.trim().is_empty() {
        result.push(current.trim().to_owned().into());
    }
    result
}

pub(super) struct SelectPage {
    scope: ConfigScope,
    pub states: BTreeMap<&'static str, Entity<SelectState<String>>>,
    pub feedback: BTreeMap<&'static str, String>,
    popup_input: Entity<InputState>,
    remote_loading: bool,
    remote_task: Option<Task<()>>,
    big_data_ready: bool,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
    _events: Vec<Subscription>,
}

impl SelectPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let standard = standard_options();
        let extras = [
            "basic.disabled",
            "basic.loading",
            "basic.clear",
            "coordinate.city",
            "size.small",
            "size.large",
            "size.multi",
            "size.multi-small",
            "size.multi-large",
            "variant.filled",
            "variant.borderless",
            "variant.underlined",
            "status.warning",
            "placement.top-start",
            "placement.top-end",
            "placement.bottom-end",
            "style.function",
        ];
        let states: BTreeMap<_, _> = DEMOS
            .iter()
            .map(|d| d.id)
            .chain(extras)
            .map(|key| {
                let tags = matches!(
                    key,
                    "tags" | "automatic-tokenization" | "custom-tokenization"
                );
                let multiple =
                    tags || matches!(
                        key,
                        "multiple"
                            | "option-render"
                            | "hide-selected"
                            | "custom-tag-render"
                            | "responsive"
                            | "big-data"
                            | "maxCount"
                            | "select-users"
                    ) || key.starts_with("size.multi");
                let data = match key {
                    "coordinate" => options(&["Zhejiang", "Jiangsu"]),
                    "coordinate.city" => options(&["Hangzhou", "Ningbo", "Wenzhou"]),
                    "custom-tag-render" => options(&["gold", "lime", "green", "cyan"]),
                    "hide-selected" => options(&["Apples", "Nails", "Bananas", "Helicopters"]),
                    "option-render" => options(&["Happy", "Sad", "Angry", "Cool", "Sleepy"]),
                    "responsive" => options(&[
                        "Long Label: a10",
                        "Long Label: c12",
                        "Long Label: h17",
                        "Long Label: j19",
                        "Long Label: k20",
                    ]),
                    "search-multi-field" => SelectOptions::new(
                        [
                            ("a11", "c11"),
                            ("b22", "b11"),
                            ("c33", "b33"),
                            ("d44", "d44"),
                        ]
                        .map(|(label, field)| {
                            SelectOption::new(label, label.to_owned(), label)
                                .search_field("otherField", field)
                        }),
                    )
                    .unwrap(),
                    "optgroup" => SelectOptions::from_entries([
                        SelectGroup::new(
                            "manager",
                            "Manager",
                            [
                                SelectOption::new("jack", "jack".to_owned(), "Jack"),
                                SelectOption::new("lucy", "lucy".to_owned(), "Lucy"),
                            ],
                        )
                        .into(),
                        SelectGroup::new(
                            "engineer",
                            "Engineer",
                            [SelectOption::new(
                                "yiminghe",
                                "yiminghe".to_owned(),
                                "Yiminghe",
                            )],
                        )
                        .into(),
                    ])
                    .unwrap(),
                    "select-users" | "big-data" | "custom-tokenization" => options(&[]),
                    _ => standard.clone(),
                };
                let state = cx.new(|cx| {
                    let mut state = if tags {
                        SelectState::tags(data, window, cx)
                    } else {
                        SelectState::new(data, window, cx)
                    };
                    state.set_settings(
                        SelectSettings {
                            mode: if tags {
                                SelectMode::Tags
                            } else if multiple {
                                SelectMode::Multiple
                            } else {
                                SelectMode::Single
                            },
                            max_count: (key == "maxCount").then_some(3),
                            hide_selected: key == "hide-selected",
                            token_separators: if key == "automatic-tokenization" {
                                vec![',']
                            } else {
                                vec![]
                            },
                            ..Default::default()
                        },
                        cx,
                    );
                    let mut search = SelectSearch::label();
                    search.enabled = Some(multiple || key.starts_with("search"));
                    if key == "search-filter-option" {
                        search = search
                            .filter(|q, o| o.label.to_lowercase().contains(&q.to_lowercase()));
                    }
                    if key == "search-sort" {
                        search = search
                            .sort(|a, b, _| a.label.to_lowercase().cmp(&b.label.to_lowercase()));
                    }
                    if key == "search-multi-field" {
                        search.fields = vec![
                            SelectSearchField::Label,
                            SelectSearchField::Field("otherField".into()),
                        ];
                    }
                    if key == "select-users" {
                        search.filter_enabled = false;
                        search.auto_clear = false;
                    }
                    state.set_search(search, cx);
                    if key == "custom-tokenization" {
                        state.set_tokenizer(Some(Rc::new(quoted_tokens)), cx);
                    }
                    let initial: &[&str] = match key {
                        "coordinate" => &["Zhejiang"],
                        "coordinate.city" => &["Hangzhou"],
                        "custom-tag-render" => &["gold", "cyan"],
                        "option-render" => &["Happy"],
                        "responsive" => &[
                            "Long Label: a10",
                            "Long Label: c12",
                            "Long Label: h17",
                            "Long Label: j19",
                            "Long Label: k20",
                        ],
                        "custom-label-render" => &["missing"],
                        "multiple" | "size.multi" | "size.multi-small" | "size.multi-large" => {
                            &["lucy", "jack"]
                        }
                        "basic" | "basic.disabled" | "basic.loading" | "basic.clear"
                        | "label-in-value" | "size" | "size.small" | "size.large" => &["lucy"],
                        _ => &[],
                    };
                    state.set_values(initial.iter().map(|s| (*s).to_owned()), cx);
                    state
                });
                (key, state)
            })
            .collect();
        let events = states
            .iter()
            .map(|(&key, state)| {
                cx.subscribe(state, move |this: &mut Self, _, event, cx| {
                    match event {
                        SelectEvent::Change(change) => {
                            this.feedback.insert(
                                key,
                                format!(
                                    "Selected: {}",
                                    change
                                        .values
                                        .iter()
                                        .map(|v| format!("{} ({})", v.label, v.value))
                                        .collect::<Vec<_>>()
                                        .join(", ")
                                ),
                            );
                            if key == "coordinate" {
                                let cities = if change
                                    .values
                                    .first()
                                    .is_some_and(|v| v.value == "Jiangsu")
                                {
                                    ["Nanjing", "Suzhou", "Zhenjiang"]
                                } else {
                                    ["Hangzhou", "Ningbo", "Wenzhou"]
                                };
                                this.states["coordinate.city"].update(cx, |state, cx| {
                                    state.set_options(options(&cities), cx);
                                    state.set_values([cities[0].to_owned()], cx);
                                });
                            }
                            cx.notify();
                        }
                        SelectEvent::Search(query) if key == "select-users" => {
                            this.remote_loading = true;
                            let query = query.to_lowercase();
                            this.states[key]
                                .update(cx, |state, cx| state.set_options(options(&[]), cx));
                            // Dropping the previous task cancels both debounce and stale delivery.
                            this.remote_task = Some(cx.spawn(async move |page, cx| {
                                cx.background_executor()
                                    .timer(Duration::from_millis(300))
                                    .await;
                                let names = [
                                    "Jack Brown",
                                    "Lucy Green",
                                    "Yiming He",
                                    "Alice Smith",
                                    "Bob Williams",
                                ];
                                let data = options(
                                    &names
                                        .into_iter()
                                        .filter(|name| name.to_lowercase().contains(&query))
                                        .collect::<Vec<_>>(),
                                );
                                let _ = page.update(cx, |page, cx| {
                                    page.states["select-users"]
                                        .update(cx, |state, cx| state.set_options(data, cx));
                                    page.remote_loading = false;
                                    cx.notify();
                                });
                            }));
                            cx.notify();
                        }
                        _ => {}
                    }
                })
            })
            .collect();
        Self {
            _config: scope.observe(cx),
            scope,
            states,
            feedback: BTreeMap::new(),
            popup_input: cx.new(|cx| InputState::new(window, cx)),
            remote_loading: false,
            remote_task: None,
            big_data_ready: false,
            anchors,
            _events: events,
        }
    }

    fn select(&self, key: &'static str) -> Select<String> {
        Select::new(key, &self.states[key])
            .scope(self.scope.clone())
            .automation_id(format!("gallery.select.{key}"))
            .accessibility_label(key)
            .placeholder("Please select")
            .w_full()
    }

    // Keep demo builders in separate frames; one large match overflows the
    // native Windows UI thread's stack in debug builds.
    fn preview(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        match id {
            "basic" => self.preview_basic(id),
            "size" => self.preview_sizes(),
            "coordinate" => self.preview_coordinate(id, cx),
            "option-render" => self.preview_option_render(id),
            "select-users" => self.preview_users(id),
            "suffix" => self.preview_suffix(id),
            "custom-dropdown-menu" => self.preview_custom_dropdown(id),
            "variant" => self.preview_variants(),
            "custom-tag-render" => self.preview_custom_tag_render(id),
            "custom-label-render" => self.preview_custom_label_render(id),
            "responsive" => self.preview_responsive(id),
            "status" => self.preview_status(id),
            "placement" => self.preview_placement(),
            "style-class" => self.preview_style_class(id),
            _ => self.preview_default(id),
        }
    }

    fn preview_basic(&self, id: &'static str) -> AnyElement {
        column()
            .child(self.select(id))
            .child(self.select("basic.disabled").disabled(true))
            .child(self.select("basic.loading").loading(true))
            .child(self.select("basic.clear").allow_clear(true))
            .into_any_element()
    }

    fn preview_sizes(&self) -> AnyElement {
        column()
            .children(
                [
                    ("size.large", Size::Large),
                    ("size", Size::Medium),
                    ("size.small", Size::Small),
                    ("size.multi-large", Size::Large),
                    ("size.multi", Size::Medium),
                    ("size.multi-small", Size::Small),
                ]
                .into_iter()
                .map(|(key, size)| self.select(key).size(size)),
            )
            .into_any_element()
    }

    fn preview_coordinate(&self, id: &'static str, cx: &Context<Self>) -> AnyElement {
        row(&self.scope, cx)
            .child(self.select(id).w(px(140.)))
            .child(self.select("coordinate.city").w(px(140.)))
            .into_any_element()
    }

    fn preview_option_render(&self, id: &'static str) -> AnyElement {
        self.select(id)
            .option_render(|o, _, _| {
                let (emoji, description) = match o.option.value.as_str() {
                    "Happy" => ("😄", "Feeling Good"),
                    "Sad" => ("😢", "Feeling Blue"),
                    "Angry" => ("😡", "Furious"),
                    "Cool" => ("😎", "Chilling"),
                    _ => ("😴", "Need Sleep"),
                };
                div().child(format!("{emoji} {} ({description})", o.option.label))
            })
            .into_any_element()
    }

    fn preview_users(&self, id: &'static str) -> AnyElement {
        self.select(id)
            .loading(self.remote_loading)
            .placeholder("Select users")
            .empty_render({
                let loading = self.remote_loading;
                move |_, _| {
                    div().p(px(12.)).child(if loading {
                        "Searching…"
                    } else {
                        "No results found"
                    })
                }
            })
            .into_any_element()
    }

    fn preview_suffix(&self, id: &'static str) -> AnyElement {
        self.select(id)
            .prefix("User")
            .suffix("⌕")
            .into_any_element()
    }

    fn preview_custom_dropdown(&self, id: &'static str) -> AnyElement {
        let input = self.popup_input.clone();
        let scope = self.scope.clone();
        self.select(id)
            .popup_render(move |menu, state, _, cx| {
                let state = state.clone();
                let input_for_add = input.clone();
                column().gap(px(8.)).child(menu).child(
                    row(&scope, cx)
                        .px(px(8.))
                        .pb(px(4.))
                        .child(
                            Input::new("add-name", &input)
                                .scope(scope.clone())
                                .placeholder("Please enter item")
                                .flex_1()
                                .min_w_0(),
                        )
                        .child(
                            Button::new("add-option")
                                .label("Add item")
                                .scope(scope.clone())
                                .on_click(move |_, window, cx| {
                                    let text = input_for_add.read(cx).value().trim().to_owned();
                                    state.update(cx, |state, cx| {
                                        let mut values: Vec<_> = state
                                            .options()
                                            .rows()
                                            .iter()
                                            .filter_map(|row| row.option().cloned())
                                            .collect();
                                        let name = if text.is_empty() {
                                            format!("New item {}", values.len() + 1)
                                        } else {
                                            text.clone()
                                        };
                                        if !values.iter().any(|option| option.value == name) {
                                            values.push(SelectOption::new(
                                                SharedString::from(name.clone()),
                                                name.clone(),
                                                name,
                                            ));
                                            state.set_options(
                                                SelectOptions::new(values).unwrap(),
                                                cx,
                                            );
                                        }
                                    });
                                    input_for_add.update(cx, |input, cx| {
                                        input.set_value("", window, cx);
                                        input.focus(window, cx);
                                    });
                                }),
                        ),
                )
            })
            .into_any_element()
    }

    fn preview_variants(&self) -> AnyElement {
        column()
            .children(
                [
                    ("variant", Variant::Outlined),
                    ("variant.filled", Variant::Filled),
                    ("variant.borderless", Variant::Borderless),
                    ("variant.underlined", Variant::Underlined),
                ]
                .into_iter()
                .map(|(key, variant)| self.select(key).variant(variant)),
            )
            .into_any_element()
    }

    fn preview_custom_tag_render(&self, id: &'static str) -> AnyElement {
        self.select(id)
            .tag_render(|tag, _, _| {
                let color = match tag.value.value.as_str() {
                    "gold" => Color::rgb(0xd48806),
                    "lime" => Color::rgb(0x7cb305),
                    "green" => Color::rgb(0x389e0d),
                    _ => Color::rgb(0x08979c),
                };
                let close = tag.clone();
                div()
                    .id(tag.id.clone())
                    .flex()
                    .items_center()
                    .gap(px(4.))
                    .px(px(6.))
                    .rounded(px(4.))
                    .border_1()
                    .border_color(color)
                    .bg(color.with_alpha(0.1))
                    .text_color(color)
                    .child(tag.value.label.clone())
                    .when(tag.closable, |d| {
                        d.child(
                            div()
                                .id("remove")
                                .cursor_pointer()
                                .child("×")
                                .on_click(move |_, w, cx| close.close(w, cx)),
                        )
                    })
            })
            .into_any_element()
    }

    fn preview_custom_label_render(&self, id: &'static str) -> AnyElement {
        self.select(id)
            .label_render(|value, _, _| {
                if value.value == "missing" {
                    "No option match".to_owned()
                } else {
                    value.label.to_string()
                }
            })
            .into_any_element()
    }

    fn preview_responsive(&self, id: &'static str) -> AnyElement {
        self.select(id)
            .responsive_tags(true)
            .allow_clear(true)
            .into_any_element()
    }

    fn preview_status(&self, id: &'static str) -> AnyElement {
        column()
            .child(self.select(id).status(ValidationStatus::Error))
            .child(
                self.select("status.warning")
                    .status(ValidationStatus::Warning),
            )
            .into_any_element()
    }

    fn preview_placement(&self) -> AnyElement {
        column()
            .children(
                [
                    ("placement", PopupPlacement::BottomLeft),
                    ("placement.bottom-end", PopupPlacement::BottomRight),
                    ("placement.top-start", PopupPlacement::TopLeft),
                    ("placement.top-end", PopupPlacement::TopRight),
                ]
                .into_iter()
                .map(|(key, placement)| {
                    self.select(key)
                        .placement(placement)
                        .placeholder(format!("{placement:?}"))
                }),
            )
            .into_any_element()
    }

    fn preview_style_class(&self, id: &'static str) -> AnyElement {
        let mut styles = SelectStyles::default();
        styles.prefix = div().text_color(Color::rgb(0x1890ff)).style().clone();
        styles.suffix = styles.prefix.clone();
        column()
            .child(self.select(id).prefix("☺").styles(styles).rounded(px(8.)))
            .child(
                self.select("style.function")
                    .prefix("☺")
                    .variant(Variant::Filled)
                    .rounded(px(8.))
                    .styles_fn(|context, _, _| {
                        let mut styles = SelectStyles::default();
                        if context.variant == Variant::Filled {
                            let purple = Color::rgb(0x722ed1);
                            styles.prefix = div().text_color(purple).style().clone();
                            styles.suffix = styles.prefix.clone();
                            styles.popup = div().border_1().border_color(purple).style().clone();
                        }
                        styles
                    }),
            )
            .into_any_element()
    }

    fn preview_default(&self, id: &'static str) -> AnyElement {
        self.select(id).allow_clear(true).into_any_element()
    }
}

impl Render for SelectPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        // The large immutable index is constructed once, only when this page is visited.
        if !self.big_data_ready {
            self.states["big-data"].update(cx, |state, cx| {
                state.set_options(
                    SelectOptions::new(
                        (0..100_000)
                            .map(|n| SelectOption::new(n as u64, format!("a{n}"), format!("a{n}"))),
                    )
                    .unwrap(),
                    cx,
                )
            });
            self.big_data_ready = true;
        }
        examples::grid(&self.scope, window, cx)
            .id("gallery.select.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                let preview = column()
                    .child(self.preview(demo.id, cx))
                    .when_some(self.feedback.get(demo.id), |d, text| {
                        d.child(div().text_size(px(12.)).child(text.clone()))
                    });
                examples::card("select", demo, anchor.clone(), preview, &self.scope, cx)
            }))
    }
}
