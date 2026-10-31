// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, Divider, Size,
    button::ButtonType,
    divider::{DividerTheme, DividerTitlePlacement, DividerTokenPatch, DividerVariant},
    icons::{IconName, icon},
    theme::{Color, ThemeConfig},
};
use gpui::{
    AnyElement, Context, FontWeight, IntoElement, Render, Role, ScrollAnchor, StyleRefinement,
    Subscription, Window, div, prelude::*, px, relative,
};

use super::shell::{page_padding, sidebar_width};

#[derive(Clone, Copy)]
pub(super) enum Preview {
    Horizontal,
    WithText,
    Size,
    Plain,
    Vertical,
    CustomizeStyle,
    ComponentToken,
    Variant,
    SemanticStyles,
}

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
    pub preview: Preview,
}

// Follow components/divider/index.en-US.md, including its three customization demos.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "divider-horizontal",
        title: "Horizontal",
        description: "The divider defaults to horizontal. Use a dashed line to separate content more subtly.",
        preview: Preview::Horizontal,
    },
    Demo {
        id: "divider-with-text",
        title: "Divider with title",
        description: "Place a title at the center, start, or end. Start and end follow the reading direction; a custom margin controls the distance from the edge.",
        preview: Preview::WithText,
    },
    Demo {
        id: "divider-size",
        title: "Set the spacing size of the divider",
        description: "Small, medium, and large adjust the vertical space around horizontal dividers.",
        preview: Preview::Size,
    },
    Demo {
        id: "divider-plain",
        title: "Text without heading style",
        description: "Use plain text to keep the surrounding text size and regular weight.",
        preview: Preview::Plain,
    },
    Demo {
        id: "divider-vertical",
        title: "Vertical",
        description: "Separate inline text and actions with a vertical divider.",
        preview: Preview::Vertical,
    },
    Demo {
        id: "divider-customize-style",
        title: "Style Customization",
        description: "Adjust the rail color, line width, or height to suit the surrounding content.",
        preview: Preview::CustomizeStyle,
    },
    Demo {
        id: "divider-component-token",
        title: "Component Token",
        description: "Customize title padding, the edge rail proportion, and vertical margins through a local component theme.",
        preview: Preview::ComponentToken,
    },
    Demo {
        id: "divider-variant",
        title: "Variant",
        description: "Choose solid, dotted, or dashed rails. The same variants work with titles and vertical dividers.",
        preview: Preview::Variant,
    },
    Demo {
        id: "divider-style-class",
        title: "Custom semantic styling",
        description: "Style the root, rail, and title independently. A custom title can combine text with an icon or another element.",
        preview: Preview::SemanticStyles,
    },
];

pub(super) struct DividerPage {
    pub scope: ConfigScope,
    pub token_scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
}

impl DividerPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let mut theme = ThemeConfig::default();
        theme.tokens.margin = Some(24.);
        theme.tokens.margin_lg = Some(48.);
        theme.tokens.line_width = Some(5.);
        theme.tokens.color_split = Some(Color::rgb(0x1677ff));
        theme.components.set::<DividerTheme>(DividerTokenPatch {
            text_padding_inline: Some(px(16.).into()),
            orientation_margin: Some(0.2),
            vertical_margin_inline: Some(16.),
        });
        let token_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .expect("valid Divider component token customization");
        Self {
            _config: scope.observe(cx),
            scope,
            token_scope,
            anchors,
        }
    }

    fn divider(&self, id: &'static str) -> Divider {
        Divider::new(id).scope(self.scope.clone()).automation_id(id)
    }

    fn paragraph() -> AnyElement {
        div()
            .child("Lorem ipsum dolor sit amet, consectetur adipiscing elit. Sed nonne merninisti licere mihi ista probare, quae sunt a te dicta? Refert tamen, quo modo.")
            .into_any_element()
    }

    fn column(children: impl IntoIterator<Item = AnyElement>) -> AnyElement {
        div()
            .w_full()
            .min_w(px(0.))
            .flex()
            .flex_col()
            .children(children)
            .into_any_element()
    }

    fn preview(&self, preview: Preview, cx: &Context<Self>) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        match preview {
            Preview::Horizontal => Self::column([
                Self::paragraph(),
                self.divider("divider.horizontal.solid").into_any_element(),
                Self::paragraph(),
                self.divider("divider.horizontal.dashed")
                    .variant(DividerVariant::Dashed)
                    .into_any_element(),
                Self::paragraph(),
            ]),
            Preview::WithText => Self::column([
                self.divider("divider.title.center")
                    .label("Text")
                    .into_any_element(),
                self.divider("divider.title.start")
                    .title_placement(DividerTitlePlacement::Start)
                    .label("Left Text")
                    .into_any_element(),
                self.divider("divider.title.end")
                    .title_placement(DividerTitlePlacement::End)
                    .label("Right Text")
                    .into_any_element(),
                self.divider("divider.title.zero-margin")
                    .title_placement(DividerTitlePlacement::Start)
                    .title_margin(px(0.))
                    .label("Text at the edge")
                    .into_any_element(),
                self.divider("divider.title.custom-margin")
                    .title_placement(DividerTitlePlacement::End)
                    .title_margin(px(50.))
                    .label("50px margin")
                    .into_any_element(),
            ]),
            Preview::Size => Self::column(
                [
                    ("divider.size.small", Size::Small, "Small"),
                    ("divider.size.medium", Size::Medium, "Medium"),
                    ("divider.size.large", Size::Large, "Large"),
                ]
                .into_iter()
                .flat_map(|(id, size, label)| {
                    [
                        div().child(label).into_any_element(),
                        self.divider(id).size(size).into_any_element(),
                    ]
                })
                .chain(std::iter::once(div().child("Content").into_any_element())),
            ),
            Preview::Plain => Self::column([
                Self::paragraph(),
                self.divider("divider.plain.center")
                    .plain(true)
                    .label("Text")
                    .into_any_element(),
                self.divider("divider.plain.start")
                    .plain(true)
                    .title_placement(DividerTitlePlacement::Start)
                    .label("Left Text")
                    .into_any_element(),
                self.divider("divider.plain.end")
                    .plain(true)
                    .title_placement(DividerTitlePlacement::End)
                    .label("Right Text")
                    .into_any_element(),
            ]),
            Preview::Vertical => div()
                .flex()
                .items_center()
                .when(config.direction == Direction::Rtl, |row| {
                    row.flex_row_reverse()
                })
                .child("Text")
                .child(self.divider("divider.vertical.first").vertical(true))
                .child(
                    Button::new("divider.vertical.link.first")
                        .scope(self.scope.clone())
                        .button_type(ButtonType::Link)
                        .label("Link")
                        .accessibility_label("Open Divider reference")
                        .href("https://ant.design/components/divider")
                        .h(px(22.))
                        .px(px(0.))
                        .py(px(0.)),
                )
                .child(self.divider("divider.vertical.second").vertical(true))
                .child(
                    Button::new("divider.vertical.link.second")
                        .scope(self.scope.clone())
                        .button_type(ButtonType::Link)
                        .label("Link")
                        .accessibility_label("Open Ant Design reference")
                        .href("https://ant.design")
                        .h(px(22.))
                        .px(px(0.))
                        .py(px(0.)),
                )
                .into_any_element(),
            Preview::CustomizeStyle => Self::column([
                self.divider("divider.custom.thick")
                    .rail_style(StyleRefinement::default().border_t(px(2.)))
                    .border_color(Color::rgb(0x7cb305))
                    .into_any_element(),
                self.divider("divider.custom.dashed")
                    .variant(DividerVariant::Dashed)
                    .border_color(Color::rgb(0x7cb305))
                    .label("Text")
                    .into_any_element(),
                div()
                    .flex()
                    .items_center()
                    .child("Solid")
                    .child(
                        self.divider("divider.custom.vertical.solid")
                            .vertical(true)
                            .h(px(60.))
                            .border_color(Color::rgb(0x7cb305)),
                    )
                    .child("Dashed")
                    .child(
                        self.divider("divider.custom.vertical.dashed")
                            .vertical(true)
                            .variant(DividerVariant::Dashed)
                            .h(px(60.))
                            .border_color(Color::rgb(0x7cb305)),
                    )
                    .into_any_element(),
            ]),
            Preview::ComponentToken => self.component_tokens(cx),
            Preview::Variant => Self::column(
                [
                    ("divider.variant.solid", DividerVariant::Solid, "Solid"),
                    ("divider.variant.dotted", DividerVariant::Dotted, "Dotted"),
                    ("divider.variant.dashed", DividerVariant::Dashed, "Dashed"),
                ]
                .into_iter()
                .map(|(id, variant, label)| {
                    self.divider(id)
                        .variant(variant)
                        .label(label)
                        .border_color(Color::rgb(0x7cb305))
                        .into_any_element()
                }),
            ),
            Preview::SemanticStyles => Self::column([
                self.divider("divider.semantic.italic")
                    .label("Styled title")
                    .variant(DividerVariant::Dashed)
                    .content_style(StyleRefinement::default().italic())
                    .rail_style(StyleRefinement::default().opacity(0.6))
                    .into_any_element(),
                self.divider("divider.semantic.custom")
                    .title_placement(DividerTitlePlacement::Start)
                    .accessibility_label("Custom title")
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(8.))
                            .when(config.direction == Direction::Rtl, |title| {
                                title.flex_row_reverse()
                            })
                            .child(
                                icon(IconName::StarOutlined)
                                    .size(px(16.))
                                    .palette(config.icon_palette),
                            )
                            .child("Custom title"),
                    )
                    .content_style(StyleRefinement::default().text_color(tokens.color_primary))
                    .rail_style(
                        StyleRefinement::default().border_color(tokens.color_primary_border),
                    )
                    .into_any_element(),
            ]),
        }
    }

    fn component_tokens(&self, _: &Context<Self>) -> AnyElement {
        Self::column([
            self.divider("divider.token.center")
                .scope(self.token_scope.clone())
                .label("Text")
                .into_any_element(),
            self.divider("divider.token.start")
                .scope(self.token_scope.clone())
                .title_placement(DividerTitlePlacement::Start)
                .label("Left Text")
                .into_any_element(),
            self.divider("divider.token.end")
                .scope(self.token_scope.clone())
                .title_placement(DividerTitlePlacement::End)
                .label("Right Text")
                .into_any_element(),
        ])
    }
}

impl Render for DividerPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        div()
            .id("gallery.divider.examples")
            .w_full()
            .grid()
            .grid_cols(if content_width >= px(840.) { 2 } else { 1 })
            .items_start()
            .when(config.direction == Direction::Rtl, |page| page.text_right())
            .gap(px(tokens.margin_lg as f32))
            .children(DEMOS.iter().enumerate().map(|(index, demo)| {
                div()
                    .id(demo.id)
                    .anchor_scroll(Some(self.anchors[index].clone()))
                    .role(Role::Group)
                    .aria_label(demo.title)
                    .debug_selector(|| format!("gallery.card.{}", demo.id))
                    .min_w(px(0.))
                    .overflow_hidden()
                    .border_1()
                    .border_color(tokens.color_border_secondary)
                    .rounded(px(tokens.border_radius_lg as f32))
                    .bg(tokens.color_bg_container)
                    .child(
                        div()
                            .p(px(tokens.padding as f32))
                            .font_weight(FontWeight::SEMIBOLD)
                            .child(demo.title),
                    )
                    .child(
                        div()
                            .id("preview")
                            .role(Role::Group)
                            .aria_label(format!("{} preview", demo.title))
                            .debug_selector(|| format!("gallery.preview.{}", demo.id))
                            .min_h(px(120.))
                            .p(px(tokens.padding_lg as f32))
                            .flex()
                            .items_center()
                            .child(self.preview(demo.preview, cx)),
                    )
                    .child(
                        div()
                            .border_t_1()
                            .border_color(tokens.color_border_secondary)
                            .p(px(tokens.padding as f32))
                            .text_size(px(tokens.font_size_sm as f32))
                            .line_height(relative(tokens.line_height as f32))
                            .text_color(tokens.color_text_secondary)
                            .child(demo.description),
                    )
            }))
    }
}

#[cfg(test)]
mod tests {
    use ant_design_gpui::{ConfigScope, Direction, UiRoot, theme::AlgorithmChain};
    use gpui::{
        AppContext, Bounds, Entity, Pixels, ScrollAnchor, ScrollHandle, TestAppContext,
        VisualTestContext, WindowHandle, px, size,
    };

    use super::{DEMOS, DividerPage};

    struct Fixture {
        cx: VisualTestContext,
        page: Entity<DividerPage>,
        scope: ConfigScope,
        _window: WindowHandle<UiRoot>,
    }

    impl Fixture {
        fn new() -> Self {
            let mut cx = TestAppContext::single();
            let mut content = None;
            let mut root_scope = None;
            let window = cx.add_window(|window, cx| {
                let scope = ConfigScope::new_window(cx);
                let scroll = ScrollHandle::new();
                let anchors = DEMOS
                    .iter()
                    .map(|_| ScrollAnchor::for_handle(scroll.clone()))
                    .collect();
                let page = cx.new(|cx| DividerPage::new(scope.clone(), anchors, cx));
                content = Some(page.clone());
                root_scope = Some(scope.clone());
                UiRoot::new(scope, page, window, cx)
            });
            let mut fixture = Self {
                cx: VisualTestContext::from_window(window.into(), &cx),
                page: content.unwrap(),
                scope: root_scope.unwrap(),
                _window: window,
            };
            fixture.resize(1280.);
            fixture
        }

        fn draw(&mut self) {
            self.cx.run_until_parked();
            self.cx.update(|window, cx| window.draw(cx).clear(cx));
            self.cx.run_until_parked();
        }

        fn resize(&mut self, width: f32) {
            self.cx.simulate_resize(size(px(width), px(1600.)));
            self.draw();
        }

        fn bounds(&mut self, selector: &'static str) -> Bounds<Pixels> {
            self.cx
                .debug_bounds(selector)
                .unwrap_or_else(|| panic!("missing {selector}"))
        }
    }

    #[test]
    fn divider_demos_follow_the_reference_order_and_render_all_nine_previews() {
        assert_eq!(
            DEMOS.iter().map(|demo| demo.id).collect::<Vec<_>>(),
            [
                "divider-horizontal",
                "divider-with-text",
                "divider-size",
                "divider-plain",
                "divider-vertical",
                "divider-customize-style",
                "divider-component-token",
                "divider-variant",
                "divider-style-class",
            ]
        );
        let mut fixture = Fixture::new();
        for demo in DEMOS {
            assert!(!demo.description.is_empty());
        }
        for selector in [
            "gallery.preview.divider-horizontal",
            "gallery.preview.divider-with-text",
            "gallery.preview.divider-size",
            "gallery.preview.divider-plain",
            "gallery.preview.divider-vertical",
            "gallery.preview.divider-customize-style",
            "gallery.preview.divider-component-token",
            "gallery.preview.divider-variant",
            "gallery.preview.divider-style-class",
        ] {
            let bounds = fixture.bounds(selector);
            assert!(bounds.size.width > px(0.));
            assert!(bounds.size.height >= px(120.));
        }
    }

    #[test]
    fn divider_cards_reflow_at_the_same_breakpoint_as_the_gallery() {
        let mut fixture = Fixture::new();
        for width in [1440., 1136., 1135., 1040., 1039., 800., 640.] {
            fixture.resize(width);
            let first = fixture.bounds("gallery.card.divider-horizontal");
            let second = fixture.bounds("gallery.card.divider-with-text");
            let content_width = px(width)
                - crate::shell::sidebar_width(px(width))
                - crate::shell::page_padding(px(width)) * 2.;
            if content_width >= px(840.) {
                assert_eq!(first.top(), second.top());
                assert!(second.left() > first.right());
            } else {
                assert_eq!(first.left(), second.left());
                assert!(second.top() >= first.bottom());
            }
        }
    }

    #[test]
    fn divider_component_tokens_remain_local_and_follow_parent_direction_and_theme() {
        let mut fixture = Fixture::new();
        fixture.page.read_with(&fixture.cx, |page, cx| {
            let normal = page.scope.snapshot(cx);
            let custom = page.token_scope.snapshot(cx);
            assert_eq!(normal.theme.tokens.line_width, 1.);
            assert_eq!(custom.theme.tokens.line_width, 5.);
            assert_ne!(
                normal.theme.tokens.color_split,
                custom.theme.tokens.color_split
            );
        });
        let scope = fixture.scope.clone();
        fixture.cx.update(|_, cx| {
            scope
                .update_patch(cx, |patch| {
                    patch.direction = Some(Direction::Rtl);
                    patch.theme.get_or_insert_default().algorithms =
                        Some(AlgorithmChain::dark_compact());
                })
                .unwrap();
        });
        fixture.draw();
        fixture.page.read_with(&fixture.cx, |page, cx| {
            let normal = page.scope.snapshot(cx);
            let custom = page.token_scope.snapshot(cx);
            assert_eq!(custom.direction, Direction::Rtl);
            assert_eq!(normal.theme.tokens.line_width, 1.);
            assert_eq!(custom.theme.tokens.line_width, 5.);
            assert_eq!(
                normal.theme.tokens.color_bg_container,
                custom.theme.tokens.color_bg_container
            );
        });
    }
}
