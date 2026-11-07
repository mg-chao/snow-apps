// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Public examples adapted from Ant Design 6.6.7 (MIT).

use std::{cell::RefCell, collections::BTreeMap, rc::Rc};

use ant_design_gpui::{
    ConfigScope, Direction, Divider, InputNumber, InputNumberEvent, InputNumberState,
    NumberOptions, NumericValue, Pagination, PaginationState, Size,
    divider::DividerTitlePlacement,
    pagination::{
        PaginationAlign, PaginationItemKind, PaginationMode, PaginationSizeChangerRender,
        PaginationStyles, QuickJumper,
    },
    theme::Color,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, Subscription,
    Window, div, prelude::*, px,
};

use super::input_examples::{self as examples, Coverage, Demo, column};

const fn demo(id: &'static str, title: &'static str, description: &'static str) -> Demo {
    Demo {
        id,
        title,
        description,
        coverage: Coverage::Complete,
    }
}

// Public examples, in components/pagination/index.en-US.md order.
pub(super) const DEMOS: &[Demo] = &[
    demo(
        "basic",
        "Basic",
        "A basic pagination control with five pages.",
    ),
    demo(
        "align",
        "Align",
        "Align pagination to the start, center, or end.",
    ),
    demo(
        "more",
        "More",
        "Navigate a longer list and jump five pages with the ellipsis controls.",
    ),
    demo(
        "changer",
        "Changer",
        "Change the number of items per page. The second control is disabled.",
    ),
    demo(
        "jump",
        "Jumper",
        "Enter a page number to jump directly to it.",
    ),
    demo(
        "mini",
        "Size",
        "Small and large pagination, with optional totals, size changer, and quick jumper.",
    ),
    demo(
        "simple",
        "Simple mode",
        "An editable current page, a read-only pager, and a disabled pager.",
    ),
    demo(
        "controlled",
        "Controlled",
        "The application owns the current page and accepts pagination changes.",
    ),
    demo(
        "total",
        "Total number",
        "Display the total number of items or the inclusive range on this page.",
    ),
    demo(
        "all",
        "Show All",
        "Combine the total, page-size selector, and quick jumper.",
    ),
    demo(
        "itemRender",
        "Prev and next",
        "Replace arrow content with Previous and Next labels.",
    ),
    demo(
        "components",
        "Custom component",
        "Use a native InputNumber as the page-size changer.",
    ),
    demo(
        "style-class",
        "Custom semantic dom styling",
        "Style the root and items with an object or a size-aware callback.",
    ),
];

const STATE_IDS: &[&str] = &[
    "basic",
    "align.start",
    "align.center",
    "align.end",
    "more",
    "changer",
    "changer.disabled",
    "jump",
    "jump.disabled",
    "mini.small",
    "mini.small.options",
    "mini.small.total",
    "mini.small.disabled",
    "mini.large",
    "mini.large.options",
    "mini.large.total",
    "mini.large.disabled",
    "simple",
    "simple.read-only",
    "simple.disabled",
    "controlled",
    "total",
    "total.range",
    "all",
    "itemRender",
    "components",
    "style-class",
    "style-class.small",
];

pub(super) struct PaginationPage {
    scope: ConfigScope,
    pub states: BTreeMap<&'static str, Entity<PaginationState>>,
    pub controlled_current: u64,
    pub number: Entity<InputNumberState>,
    number_connection: Rc<RefCell<Option<PaginationSizeChangerRender>>>,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
    _number_events: Subscription,
}

impl PaginationPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let states = STATE_IDS
            .iter()
            .map(|&id| (id, cx.new(|cx| PaginationState::new(window, cx))))
            .collect();
        let number = cx.new(|cx| {
            let mut state = InputNumberState::with_options(
                NumberOptions {
                    minimum: Some("1".parse().unwrap()),
                    precision: Some(0),
                    ..Default::default()
                },
                window,
                cx,
            )
            .expect("valid size changer options");
            state
                .set_value(Some(NumericValue::Number(10.)), window, cx)
                .expect("valid initial page size");
            state
        });
        let number_connection: Rc<RefCell<Option<PaginationSizeChangerRender>>> =
            Rc::new(RefCell::new(None));
        let connection = number_connection.clone();
        let number_events = cx.subscribe_in(&number, window, move |_, _, event, window, cx| {
            if let InputNumberEvent::Change(change) = event
                && let Some(value) = change
                    .value
                    .as_ref()
                    .and_then(|value| value.to_number().ok())
                && value.is_finite()
                && value >= 1.
                && let Some(connection) = connection.borrow().as_ref()
            {
                connection.change_size(value as u64, window, cx);
            }
        });
        Self {
            _config: scope.observe(cx),
            scope,
            states,
            controlled_current: 3,
            number,
            number_connection,
            anchors,
            _number_events: number_events,
        }
    }

    fn pager(&self, id: &'static str, total: u64) -> Pagination {
        Pagination::new(id, &self.states[id])
            .scope(self.scope.clone())
            .total(total)
            .automation_id(format!("gallery.pagination.{id}"))
    }

    fn preview(&self, id: &'static str, cx: &mut Context<Self>) -> AnyElement {
        match id {
            "basic" => self.pager("basic", 50).into_any_element(),
            "align" => column()
                .child(self.pager("align.start", 50).align(PaginationAlign::Start))
                .child(
                    self.pager("align.center", 50)
                        .align(PaginationAlign::Center),
                )
                .child(self.pager("align.end", 50).align(PaginationAlign::End))
                .into_any_element(),
            "more" => self
                .pager("more", 500)
                .default_current(6)
                .into_any_element(),
            "changer" => column()
                .child(
                    self.pager("changer", 500)
                        .default_current(3)
                        .show_size_changer(true),
                )
                .child(
                    self.pager("changer.disabled", 500)
                        .default_current(3)
                        .show_size_changer(true)
                        .disabled(true),
                )
                .into_any_element(),
            "jump" => column()
                .child(
                    self.pager("jump", 500)
                        .default_current(2)
                        .quick_jumper(QuickJumper::Blur),
                )
                .child(
                    self.pager("jump.disabled", 500)
                        .default_current(2)
                        .quick_jumper(QuickJumper::Blur)
                        .disabled(true),
                )
                .into_any_element(),
            "mini" => {
                let mut preview = column();
                for (name, size) in [("small", Size::Small), ("large", Size::Large)] {
                    preview = preview.child(
                        Divider::new((gpui::ElementId::from("pagination.size"), name))
                            .scope(self.scope.clone())
                            .title_placement(DividerTitlePlacement::Start)
                            .label(if size == Size::Small {
                                "Small"
                            } else {
                                "Large"
                            }),
                    );
                    let keys = if name == "small" {
                        [
                            "mini.small",
                            "mini.small.options",
                            "mini.small.total",
                            "mini.small.disabled",
                        ]
                    } else {
                        [
                            "mini.large",
                            "mini.large.options",
                            "mini.large.total",
                            "mini.large.disabled",
                        ]
                    };
                    preview = preview
                        .child(self.pager(keys[0], 50).size(size))
                        .child(
                            self.pager(keys[1], 50)
                                .size(size)
                                .show_size_changer(true)
                                .quick_jumper(QuickJumper::Blur),
                        )
                        .child(
                            self.pager(keys[2], 50)
                                .size(size)
                                .show_total_text(|total, _, _, _| format!("Total {total} items")),
                        )
                        .child(
                            self.pager(keys[3], 50)
                                .size(size)
                                .disabled(true)
                                .show_size_changer(true)
                                .quick_jumper(QuickJumper::Blur)
                                .show_total_text(|total, _, _, _| format!("Total {total} items")),
                        );
                }
                preview.into_any_element()
            }
            "simple" => column()
                .child(
                    self.pager("simple", 50)
                        .default_current(2)
                        .mode(PaginationMode::Simple),
                )
                .child(
                    self.pager("simple.read-only", 50)
                        .default_current(2)
                        .mode(PaginationMode::SimpleReadOnly),
                )
                .child(
                    self.pager("simple.disabled", 50)
                        .default_current(2)
                        .mode(PaginationMode::Simple)
                        .disabled(true),
                )
                .into_any_element(),
            "controlled" => self
                .pager("controlled", 50)
                .current(self.controlled_current)
                .on_change(cx.listener(
                    |page, change: &ant_design_gpui::pagination::PaginationChange, _, cx| {
                        page.controlled_current = change.current;
                        cx.notify();
                    },
                ))
                .into_any_element(),
            "total" => column()
                .child(
                    self.pager("total", 85)
                        .default_page_size(20)
                        .show_total_text(|total, _, _, _| format!("Total {total} items")),
                )
                .child(
                    self.pager("total.range", 85)
                        .default_page_size(20)
                        .show_total_text(|total, range, _, _| {
                            format!("{}-{} of {total} items", range[0], range[1])
                        }),
                )
                .into_any_element(),
            "all" => self
                .pager("all", 85)
                .show_size_changer(true)
                .quick_jumper(QuickJumper::Blur)
                .show_total_text(|total, _, _, _| format!("Total {total} items"))
                .into_any_element(),
            "itemRender" => self
                .pager("itemRender", 500)
                .item_render({
                    let scope = self.scope.clone();
                    move |info, original, window, cx| match info.kind {
                        PaginationItemKind::Previous => {
                            Some(intrinsic_label("Previous", &scope, window, cx).into_any_element())
                        }
                        PaginationItemKind::Next => {
                            Some(intrinsic_label("Next", &scope, window, cx).into_any_element())
                        }
                        _ => Some(original),
                    }
                })
                .into_any_element(),
            "components" => {
                let number = self.number.clone();
                let connection = self.number_connection.clone();
                self.pager("components", 500)
                    .default_current(3)
                    .show_size_changer(true)
                    .size_changer_render(move |info, window, cx| {
                        *connection.borrow_mut() = Some(info.clone());
                        let value = number
                            .read(cx)
                            .value()
                            .and_then(|value| value.to_number().ok());
                        if value != Some(info.value as f64) {
                            number.update(cx, |state, cx| {
                                state
                                    .set_value(
                                        Some(NumericValue::Number(info.value as f64)),
                                        window,
                                        cx,
                                    )
                                    .expect("valid page size");
                            });
                        }
                        InputNumber::new("pagination.custom-size", &number, cx)
                            .scope(info.scope.clone())
                            .size(info.size)
                            .disabled(info.disabled)
                            .accessibility_label(info.label.clone())
                            .w(px(100.))
                    })
                    .into_any_element()
            }
            "style-class" => {
                let root = div()
                    .border_2()
                    .border_dashed()
                    .border_color(Color::rgb(0xcccccc))
                    .p(px(8.))
                    .style()
                    .clone();
                column()
                    .child(self.pager("style-class", 500).styles(PaginationStyles {
                        root: root.clone(),
                        item: div().rounded(px(999.)).style().clone(),
                    }))
                    .child(
                        self.pager("style-class.small", 500)
                            .size(Size::Small)
                            .styles(PaginationStyles {
                                root,
                                ..Default::default()
                            })
                            .styles_fn(|context, _, _| {
                                if context.size == Size::Small {
                                    PaginationStyles {
                                        item: div()
                                            .bg(Color::rgb(0xc8c8c8).with_alpha(0.3))
                                            .when(context.direction == Direction::Ltr, |d| {
                                                d.mr(px(4.))
                                            })
                                            .when(context.direction == Direction::Rtl, |d| {
                                                d.ml(px(4.))
                                            })
                                            .style()
                                            .clone(),
                                        ..Default::default()
                                    }
                                } else {
                                    PaginationStyles::default()
                                }
                            }),
                    )
                    .into_any_element()
            }
            _ => unreachable!("registered Pagination demo"),
        }
    }
}

fn intrinsic_label(
    text: &'static str,
    scope: &ConfigScope,
    window: &Window,
    cx: &gpui::App,
) -> gpui::Div {
    let config = scope.snapshot(cx);
    let run = gpui::TextRun {
        len: text.len(),
        font: config.font.clone(),
        color: config.theme.tokens.color_link.to_hsla(),
        background_color: None,
        underline: None,
        strikethrough: None,
    };
    let width = window
        .text_system()
        .shape_line(
            text.into(),
            px(config.theme.tokens.font_size as f32),
            &[run],
            None,
        )
        .width;
    div()
        .min_w_0()
        .w(width)
        .whitespace_nowrap()
        .text_color(config.theme.tokens.color_link)
        .hover(|style| style.text_color(config.theme.tokens.color_link_hover))
        .child(text)
}

impl Render for PaginationPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.pagination.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                let preview = div()
                    .id((gpui::ElementId::from("pagination-preview"), demo.id))
                    .min_w_0()
                    .w_full()
                    .overflow_x_scroll()
                    .child(self.preview(demo.id, cx));
                div()
                    .min_w_0()
                    .when(
                        !matches!(demo.id, "basic" | "align" | "simple" | "controlled"),
                        |card| card.col_span_full(),
                    )
                    .child(examples::card(
                        "pagination",
                        demo,
                        anchor.clone(),
                        preview,
                        &self.scope,
                        cx,
                    ))
                    .into_any_element()
            }))
    }
}
