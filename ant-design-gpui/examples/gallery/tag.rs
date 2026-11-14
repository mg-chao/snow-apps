// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use std::time::Duration;

use ant_design_gpui::{
    CheckableTag, CheckableTagGroup, ConfigPatch, ConfigScope, Direction, Divider, Tag, TagOption,
    divider::DividerTitlePlacement,
    icons::IconName,
    tag::{TagChange, TagColor, TagTheme, TagTokenPatch, TagVariant},
    theme::{Color, PresetColor, ThemeConfig},
};
use gpui::{
    Animation, AnimationExt, AnyElement, Context, FontWeight, IntoElement, Render, Role,
    ScrollAnchor, StyleRefinement, Subscription, Window, div, prelude::*, px, relative,
};

use super::shell::{page_padding, sidebar_width};

#[derive(Clone, Copy)]
pub(super) enum Preview {
    Basic,
    Colorful,
    Control,
    Checkable,
    Animation,
    Icon,
    Status,
    Customize,
    Draggable,
    ComponentToken,
    Disabled,
    SemanticStyles,
}

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
    pub preview: Preview,
}

// Follow components/tag/index.en-US.md, including the customization examples.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "tag-basic",
        title: "Basic",
        description: "A tag marks a category or property. Close icons remove a tag; a close callback can keep it visible.",
        preview: Preview::Basic,
    },
    Demo {
        id: "tag-colorful",
        title: "Colorful Tag",
        description: "Preset and custom colors support filled, solid, and outlined appearances.",
        preview: Preview::Colorful,
    },
    Demo {
        id: "tag-control",
        title: "Add & Remove Dynamically",
        description: "Add a tag with New Tag, then remove it with its close icon. The first tag stays in place.",
        preview: Preview::Control,
    },
    Demo {
        id: "tag-checkable",
        title: "Checkable",
        description: "Controlled checkable tags support a single choice or several choices. Use Space to toggle a focused tag.",
        preview: Preview::Checkable,
    },
    Demo {
        id: "tag-animation",
        title: "Animate",
        description: "New tags fade into view. The preview motion setting also controls this entrance animation.",
        preview: Preview::Animation,
    },
    Demo {
        id: "tag-icon",
        title: "Icon",
        description: "Icons follow the tag color. Checkable tags can combine an icon with a label.",
        preview: Preview::Icon,
    },
    Demo {
        id: "tag-status",
        title: "Status Tag",
        description: "Success, processing, warning, error, and default colors communicate status across all three variants.",
        preview: Preview::Status,
    },
    Demo {
        id: "tag-customize",
        title: "Customize close",
        description: "Choose a close icon to suit the action. Close controls remain available through keyboard and native accessibility actions.",
        preview: Preview::Customize,
    },
    Demo {
        id: "tag-draggable",
        title: "Draggable Tag",
        description: "Drag a tag onto another tag to reorder the list.",
        preview: Preview::Draggable,
    },
    Demo {
        id: "tag-component-token",
        title: "Component Token",
        description: "A local component theme customizes the default background and text without affecting surrounding tags.",
        preview: Preview::ComponentToken,
    },
    Demo {
        id: "tag-disabled",
        title: "Disabled",
        description: "Disabled tags keep their appearance consistent across colors and variants, and block links, closing, and selection.",
        preview: Preview::Disabled,
    },
    Demo {
        id: "tag-style-class",
        title: "Custom semantic styling",
        description: "Refine the root, icon, content, and close control independently using native GPUI styles.",
        preview: Preview::SemanticStyles,
    },
];

const CHOICES: [&str; 4] = ["Movies", "Books", "Music", "Sports"];
const SOCIALS: [(IconName, &str, u32); 4] = [
    (IconName::TwitterOutlined, "Twitter", 0x55acee),
    (IconName::YoutubeOutlined, "Youtube", 0xcd201f),
    (IconName::FacebookOutlined, "Facebook", 0x3b5999),
    (IconName::LinkedinOutlined, "LinkedIn", 0x55acee),
];

pub(super) struct TagPage {
    pub scope: ConfigScope,
    pub token_scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub checked: bool,
    pub single: Option<usize>,
    pub multiple: [bool; 4],
    social_checked: [bool; 4],
    pub dynamic: Vec<usize>,
    pub animated: Vec<usize>,
    next_dynamic: usize,
    next_animated: usize,
    pub order: Vec<usize>,
    _config: Subscription,
}

impl TagPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let mut theme = ThemeConfig::default();
        theme.components.set::<TagTheme>(TagTokenPatch {
            default_bg: Some(Color::rgb(0xf9f0ff)),
            default_color: Some(Color::rgb(0x4b34d3)),
            ..Default::default()
        });
        let token_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .expect("valid Tag component theme");
        Self {
            _config: scope.observe(cx),
            scope,
            token_scope,
            anchors,
            checked: true,
            single: Some(1),
            multiple: [true, false, true, false],
            social_checked: [true, false, false, false],
            dynamic: vec![1, 2, 3],
            animated: vec![1, 2, 3],
            next_dynamic: 4,
            next_animated: 4,
            order: vec![1, 2, 3],
        }
    }

    fn tag(&self, id: impl Into<gpui::ElementId>) -> Tag {
        let id = id.into();
        let automation_id = id.to_string();
        Tag::new(id)
            .scope(self.scope.clone())
            .automation_id(automation_id.clone())
            .close_automation_id(format!("{automation_id}.close"))
    }

    fn row(&self, cx: &Context<Self>) -> gpui::Div {
        div()
            .w_full()
            .min_w(px(0.))
            .flex()
            .flex_wrap()
            .items_center()
            .gap(px(8.))
            .when(self.scope.snapshot(cx).direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
    }

    fn column(children: impl IntoIterator<Item = AnyElement>) -> AnyElement {
        div()
            .w_full()
            .min_w(px(0.))
            .flex()
            .flex_col()
            .gap(px(16.))
            .children(children)
            .into_any_element()
    }

    fn heading(
        &self,
        id: impl Into<gpui::ElementId>,
        label: impl Into<gpui::SharedString>,
    ) -> AnyElement {
        Divider::new(id)
            .scope(self.scope.clone())
            .label(label)
            .plain(true)
            .title_placement(DividerTitlePlacement::Start)
            .my(px(12.))
            .into_any_element()
    }

    fn dynamic_tags(&self, animated: bool, cx: &Context<Self>) -> AnyElement {
        let mut row = self.row(cx);
        let tags = if animated {
            &self.animated
        } else {
            &self.dynamic
        };
        for &id in tags {
            let tag = self
                .tag((
                    if animated {
                        "tag.animated.item"
                    } else {
                        "tag.dynamic.item"
                    },
                    id,
                ))
                .label(if !animated && id == 1 {
                    "Unremovable".to_string()
                } else {
                    format!("Tag {id}")
                })
                .closable(animated || id != 1)
                .on_close(cx.listener(move |this, _, _, cx| {
                    (if animated {
                        &mut this.animated
                    } else {
                        &mut this.dynamic
                    })
                    .retain(|value| *value != id);
                    cx.notify();
                }));
            let selector = format!(
                "tag.{}.item.{id}",
                if animated { "animated" } else { "dynamic" }
            );
            let item = div()
                .id(("item", id))
                .debug_selector(move || selector.clone())
                .child(tag);
            row = if animated && self.scope.snapshot(cx).motion_policy(cx).allows_motion() {
                row.child(item.with_animation(
                    "appear",
                    Animation::new(Duration::from_millis(100)),
                    |item, progress| item.opacity(progress),
                ))
            } else {
                row.child(item)
            };
        }
        let selector = if animated {
            "tag.animated.add"
        } else {
            "tag.dynamic.add"
        };
        row.child(
            div()
                .id(selector)
                .debug_selector(move || selector.into())
                .child(
                    self.tag(selector)
                        .label("New Tag")
                        .icon(IconName::PlusOutlined)
                        .variant(TagVariant::Outlined)
                        .border_dashed()
                        .bg(self.scope.snapshot(cx).theme.tokens.color_bg_container)
                        .on_click(cx.listener(move |this, _, _, cx| {
                            if animated {
                                this.animated.push(this.next_animated);
                                this.next_animated += 1;
                            } else {
                                this.dynamic.push(this.next_dynamic);
                                this.next_dynamic += 1;
                            }
                            cx.notify();
                        })),
                ),
        )
        .into_any_element()
    }

    fn checkable(&self, cx: &Context<Self>) -> AnyElement {
        Self::column([
            self.row(cx)
                .child(div().w(px(72.)).child("Checkable"))
                .child(
                    div()
                        .id("tag.checkable.yes")
                        .debug_selector(|| "tag.checkable.yes".into())
                        .child(
                            CheckableTag::new("yes")
                                .scope(self.scope.clone())
                                .automation_id("tag.checkable.yes")
                                .label("Yes")
                                .checked(self.checked)
                                .on_change(cx.listener(|this, change: &TagChange, _, cx| {
                                    this.checked = change.checked;
                                    cx.notify();
                                })),
                        ),
                )
                .into_any_element(),
            self.row(cx)
                .child(div().w(px(72.)).child("Single"))
                .child(
                    CheckableTagGroup::new("tag.single")
                        .scope(self.scope.clone())
                        .automation_id("tag.single")
                        .accessibility_label("Single category")
                        .options(CHOICES.iter().enumerate().map(|(index, &label)| {
                            TagOption::new(("choice", index), index, label)
                                .automation_id(format!("tag.single.{index}"))
                        }))
                        .selected(self.single)
                        .on_change(cx.listener(|this, values: &[usize], _, cx| {
                            this.single = values.first().copied();
                            cx.notify();
                        })),
                )
                .into_any_element(),
            self.row(cx)
                .child(div().w(px(72.)).child("Multiple"))
                .child(
                    CheckableTagGroup::new("tag.multiple")
                        .scope(self.scope.clone())
                        .automation_id("tag.multiple")
                        .multiple(true)
                        .accessibility_label("Multiple categories")
                        .options(CHOICES.iter().enumerate().map(|(index, &label)| {
                            TagOption::new(("choice", index), index, label)
                                .automation_id(format!("tag.multiple.{index}"))
                        }))
                        .value(
                            self.multiple
                                .iter()
                                .enumerate()
                                .filter_map(|(index, selected)| selected.then_some(index)),
                        )
                        .on_change(cx.listener(|this, values: &[usize], _, cx| {
                            this.multiple = std::array::from_fn(|index| values.contains(&index));
                            cx.notify();
                        })),
                )
                .into_any_element(),
        ])
    }

    fn colors(&self, cx: &Context<Self>) -> AnyElement {
        let presets = [
            PresetColor::Magenta,
            PresetColor::Red,
            PresetColor::Volcano,
            PresetColor::Orange,
            PresetColor::Gold,
            PresetColor::Lime,
            PresetColor::Green,
            PresetColor::Cyan,
            PresetColor::Blue,
            PresetColor::Geekblue,
            PresetColor::Purple,
        ];
        let mut children = Vec::new();
        for (group, custom) in [("Presets", false), ("Custom", true)] {
            for variant in [TagVariant::Filled, TagVariant::Solid, TagVariant::Outlined] {
                let name = match variant {
                    TagVariant::Filled => "filled",
                    TagVariant::Solid => "solid",
                    TagVariant::Outlined => "outlined",
                };
                children.push(self.heading(
                    format!("tag.color.heading.{group}.{name}"),
                    format!("{group} ({name})"),
                ));
                let mut row = self.row(cx);
                if custom {
                    for (index, color) in [0xff5500, 0x2db7f5, 0x87d068, 0x108ee9]
                        .into_iter()
                        .enumerate()
                    {
                        row = row.child(
                            self.tag(format!("tag.custom.{name}.{index}"))
                                .label(format!("#{color:06x}"))
                                .color(TagColor::Custom(Color::rgb(color)))
                                .variant(variant),
                        );
                    }
                } else {
                    for color in presets {
                        row = row.child(
                            self.tag(format!("tag.preset.{name}.{}", color.name()))
                                .label(color.name())
                                .color(TagColor::Preset(color))
                                .variant(variant),
                        );
                    }
                }
                children.push(row.into_any_element());
            }
        }
        div()
            .w_full()
            .flex()
            .flex_col()
            .children(children)
            .into_any_element()
    }

    fn statuses(&self, cx: &Context<Self>) -> AnyElement {
        Self::column(
            [TagVariant::Filled, TagVariant::Solid, TagVariant::Outlined]
                .into_iter()
                .enumerate()
                .map(|(index, variant)| {
                    let name = match variant {
                        TagVariant::Filled => "filled",
                        TagVariant::Solid => "solid",
                        TagVariant::Outlined => "outlined",
                    };
                    div()
                        .child(
                            self.heading(("tag.status.heading", index), format!("Status ({name})")),
                        )
                        .child(
                            self.row(cx).children(
                                [
                                    ("success", TagColor::Success, IconName::CheckCircleOutlined),
                                    ("processing", TagColor::Processing, IconName::SyncOutlined),
                                    (
                                        "warning",
                                        TagColor::Warning,
                                        IconName::ExclamationCircleOutlined,
                                    ),
                                    ("error", TagColor::Error, IconName::CloseCircleOutlined),
                                    ("default", TagColor::Default, IconName::ClockCircleOutlined),
                                ]
                                .into_iter()
                                .map(|(label, color, icon)| {
                                    self.tag(format!("tag.status.{name}.{label}"))
                                        .label(label)
                                        .color(color)
                                        .variant(variant)
                                        .icon(icon)
                                }),
                            ),
                        )
                        .into_any_element()
                }),
        )
    }

    fn preview(&self, preview: Preview, cx: &Context<Self>) -> AnyElement {
        match preview {
            Preview::Basic => self
                .row(cx)
                .child(self.tag("tag.basic.one").label("Tag 1"))
                .child(
                    self.tag("tag.basic.link")
                        .label("Link")
                        .href("https://github.com/ant-design/ant-design/issues/1862"),
                )
                .child(
                    self.tag("tag.basic.prevent")
                        .label("Prevent Default")
                        .closable(true)
                        .on_close(|event, _, _| event.prevent_default()),
                )
                .child(
                    self.tag("tag.basic.two")
                        .label("Tag 2")
                        .close_icon(IconName::CloseCircleOutlined),
                )
                .child(
                    self.tag("tag.basic.three")
                        .label("Tag 3")
                        .close_icon(IconName::DeleteOutlined),
                )
                .into_any_element(),
            Preview::Colorful => self.colors(cx),
            Preview::Control => self.dynamic_tags(false, cx),
            Preview::Checkable => self.checkable(cx),
            Preview::Animation => self.dynamic_tags(true, cx),
            Preview::Icon => {
                Self::column([
                    self.heading("tag.icon.heading", "Tag with icon"),
                    self.row(cx)
                        .children(SOCIALS.into_iter().enumerate().map(
                            |(index, (icon, label, color))| {
                                self.tag(("tag.icon", index))
                                    .label(label)
                                    .icon(icon)
                                    .color(TagColor::Custom(Color::rgb(color)))
                            },
                        ))
                        .into_any_element(),
                    self.heading("tag.checkable.icon.heading", "CheckableTag with icon"),
                    self.row(cx)
                        .children(SOCIALS.into_iter().enumerate().map(
                            |(index, (icon, label, _))| {
                                CheckableTag::new(("tag.checkable.icon", index))
                                    .scope(self.scope.clone())
                                    .label(label)
                                    .icon(icon)
                                    .checked(self.social_checked[index])
                                    .on_change(cx.listener(
                                        move |this, change: &TagChange, _, cx| {
                                            this.social_checked[index] = change.checked;
                                            cx.notify();
                                        },
                                    ))
                            },
                        ))
                        .into_any_element(),
                    self.row(cx)
                        .child(
                            self.tag("tag.icon.star")
                                .label("Star")
                                .icon(IconName::StarFilled)
                                .color(TagColor::Custom(Color::rgb(0xff5500))),
                        )
                        .child(
                            self.tag("tag.icon.heart")
                                .label("Heart")
                                .icon(IconName::HeartFilled)
                                .color(TagColor::Custom(Color::rgb(0xeb2f96))),
                        )
                        .child(
                            self.tag("tag.icon.close")
                                .label("Closable")
                                .icon(IconName::HeartFilled)
                                .closable(true),
                        )
                        .into_any_element(),
                ])
            }
            Preview::Status => self.statuses(cx),
            Preview::Customize => self
                .row(cx)
                .child(
                    self.tag("tag.customize.circle")
                        .label("Tag 1")
                        .close_icon(IconName::CloseCircleOutlined),
                )
                .child(
                    self.tag("tag.customize.delete")
                        .label("Tag 2")
                        .close_icon(IconName::DeleteOutlined),
                )
                .into_any_element(),
            Preview::Draggable => self
                .row(cx)
                .children(self.order.iter().map(|&id| {
                    let scope = self.scope.clone();
                    div()
                        .id(("tag.drag", id))
                        .debug_selector(move || format!("tag.drag.{id}"))
                        .cursor_move()
                        .child(
                            self.tag(("tag.drag.item", id))
                                .label(format!("Tag {id}"))
                                .cursor_move(),
                        )
                        .on_drag(TagDrag(id), move |_, _, _, cx| {
                            cx.new(|_| DragPreview {
                                scope: scope.clone(),
                                id,
                            })
                        })
                        .on_drop(cx.listener(move |this, dragged: &TagDrag, _, cx| {
                            if let (Some(from), Some(to)) = (
                                this.order.iter().position(|value| *value == dragged.0),
                                this.order.iter().position(|value| *value == id),
                            ) {
                                let value = this.order.remove(from);
                                this.order.insert(to, value);
                                cx.notify();
                            }
                        }))
                }))
                .into_any_element(),
            Preview::ComponentToken => self
                .row(cx)
                .child(
                    self.tag("tag.token.default")
                        .scope(self.token_scope.clone())
                        .label("Custom default"),
                )
                .child(
                    self.tag("tag.token.link")
                        .scope(self.token_scope.clone())
                        .label("Link")
                        .href("https://ant.design"),
                )
                .child(
                    self.tag("tag.token.close")
                        .scope(self.token_scope.clone())
                        .label("Closable")
                        .closable(true),
                )
                .child(
                    self.tag("tag.token.preset")
                        .scope(self.token_scope.clone())
                        .label("magenta")
                        .color(TagColor::Preset(PresetColor::Magenta)),
                )
                .into_any_element(),
            Preview::Disabled => Self::column([
                self.row(cx)
                    .child(
                        self.tag("tag.disabled.basic")
                            .label("Basic Tag")
                            .disabled(true),
                    )
                    .child(
                        self.tag("tag.disabled.link")
                            .label("Link Tag")
                            .href("https://ant.design")
                            .disabled(true),
                    )
                    .child(
                        self.tag("tag.disabled.icon")
                            .label("Icon Tag")
                            .color(TagColor::Success)
                            .icon(IconName::CheckCircleOutlined)
                            .disabled(true),
                    )
                    .into_any_element(),
                self.row(cx)
                    .children(
                        [TagVariant::Filled, TagVariant::Solid, TagVariant::Outlined]
                            .into_iter()
                            .enumerate()
                            .map(|(index, variant)| {
                                self.tag(("tag.disabled.color", index))
                                    .label(match variant {
                                        TagVariant::Filled => "Filled",
                                        TagVariant::Solid => "Solid",
                                        TagVariant::Outlined => "Outlined",
                                    })
                                    .variant(variant)
                                    .color(TagColor::Custom(Color::rgb(0xff5500)))
                                    .disabled(true)
                            }),
                    )
                    .into_any_element(),
                self.row(cx)
                    .children(CHOICES.iter().enumerate().map(|(index, &label)| {
                        CheckableTag::new(("tag.disabled.checkable", index))
                            .scope(self.scope.clone())
                            .automation_id(format!("tag.disabled.checkable.{index}"))
                            .label(label)
                            .checked(index == 1)
                            .disabled(true)
                    }))
                    .into_any_element(),
                self.row(cx)
                    .child(
                        self.tag("tag.disabled.close")
                            .label("Closable Tag")
                            .closable(true)
                            .disabled(true),
                    )
                    .child(
                        self.tag("tag.disabled.close.icon")
                            .label("Closable with Icon")
                            .color(TagColor::Success)
                            .icon(IconName::CheckCircleOutlined)
                            .close_icon(IconName::CloseCircleOutlined)
                            .disabled(true),
                    )
                    .into_any_element(),
            ]),
            Preview::SemanticStyles => self
                .row(cx)
                .child(
                    self.tag("tag.semantic.object")
                        .label("Object")
                        .icon(IconName::CheckCircleOutlined)
                        .closable(true)
                        .py(px(2.))
                        .px(px(6.))
                        .rounded(px(4.))
                        .bg(Color::rgb(0xe6f7ff))
                        .icon_style(StyleRefinement::default().text_color(Color::rgb(0x52c41a)))
                        .content_style(StyleRefinement::default().text_color(Color::rgb(0x262626)))
                        .close_style(StyleRefinement::default().text_color(Color::rgb(0x52c41a))),
                )
                .child(
                    self.tag("tag.semantic.function")
                        .label("Function")
                        .icon(IconName::CloseCircleOutlined)
                        .py(px(2.))
                        .px(px(6.))
                        .rounded(px(4.))
                        .bg(Color::rgb(0xf5efff))
                        .icon_style(StyleRefinement::default().text_color(Color::rgb(0x8f87f1)))
                        .content_style(StyleRefinement::default().text_color(Color::rgb(0x8f87f1))),
                )
                .into_any_element(),
        }
    }
}

#[derive(Clone)]
struct TagDrag(usize);

struct DragPreview {
    scope: ConfigScope,
    id: usize,
}

impl Render for DragPreview {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        Tag::new("drag-preview")
            .scope(self.scope.clone())
            .label(format!("Tag {}", self.id))
            .opacity(0.8)
    }
}

impl Render for TagPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        div()
            .id("gallery.tag.examples")
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
