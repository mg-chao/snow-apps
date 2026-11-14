// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use std::time::Duration;

use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Direction, Size,
    behavior::Activation,
    button::{ButtonColor, ButtonTheme, ButtonTokenPatch, ButtonType, ButtonVariant},
    icons::IconName,
    theme::{Color, PresetColor, ThemeConfig},
};
use gpui::{
    AnyElement, App, BoxShadow, Context, FontWeight, IntoElement, Render, Role, ScrollAnchor,
    SharedString, StyleRefinement, Subscription, Task, Window, div, point, prelude::*, px,
    relative,
};

use super::shell::{page_padding, sidebar_width};

#[derive(Clone, Copy)]
pub(super) enum Preview {
    Basic,
    ColorVariant,
    Disabled,
    Loading,
    Ghost,
    Danger,
    Block,
    CustomDisabled,
    SemanticStyles,
    Pending(&'static str),
}

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
    pub preview: Preview,
}

// Preserve the public <code> order in components/button/index.en-US.md.
// Pending entries explain the missing capabilities without rendering partial demos.
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "basic",
        title: "Syntactic sugar",
        description: "Through the type syntactic sugar, use the preset button styles: primary buttons, default buttons, dashed buttons, text buttons, and link buttons.",
        preview: Preview::Basic,
    },
    Demo {
        id: "color-variant",
        title: "Color & Variant",
        description: "You can set the color and variant attributes at the same time can derive more variant buttons.",
        preview: Preview::ColorVariant,
    },
    Demo {
        id: "icon",
        title: "Icon",
        description: "You can add an icon using the icon property.",
        // Tooltip is not available yet.
        preview: Preview::Pending("Tooltip"),
    },
    Demo {
        id: "icon-placement",
        title: "Icon Placement",
        description: "You can set the position of a button's icon by setting the iconPlacement to start or end respectively.",
        // Keep the Radio selector, Divider and Tooltip preview together.
        preview: Preview::Pending("Radio, Divider, Tooltip"),
    },
    Demo {
        id: "size",
        title: "Size",
        description: "Ant Design supports three sizes of buttons: small, medium and large.\n\nIf a large or small button is desired, set the size property to either large or small respectively. Omit the size property for a button with the default medium size.",
        preview: Preview::Pending("Radio, Divider"),
    },
    Demo {
        id: "disabled",
        title: "Disabled",
        description: "To mark a button as disabled, add the disabled property to the Button.",
        preview: Preview::Disabled,
    },
    Demo {
        id: "loading",
        title: "Loading",
        description: "A loading indicator can be added to a button by setting the loading property on the Button. The loading.icon can be used to customize the loading icon.",
        preview: Preview::Loading,
    },
    Demo {
        id: "multiple",
        title: "Multiple Buttons",
        description: "If you need several buttons, we recommend that you use 1 primary button + n secondary buttons. If there are more than three operations, you can group some of them into a Dropdown.",
        preview: Preview::Pending("Dropdown, compact grouping"),
    },
    Demo {
        id: "ghost",
        title: "Ghost Button",
        description: "The ghost property will make a button's background transparent, this is commonly used in colored background.",
        preview: Preview::Ghost,
    },
    Demo {
        id: "danger",
        title: "Danger Buttons",
        description: "The danger is a property of buttons after antd 4.0.",
        preview: Preview::Danger,
    },
    Demo {
        id: "block",
        title: "Block Button",
        description: "The block property will make a button fit to its parent width.",
        preview: Preview::Block,
    },
    Demo {
        id: "linear-gradient",
        title: "Gradient Button",
        description: "Buttons with a gradient background.",
        // A fixed gradient would omit the reference's fading hover overlay.
        preview: Preview::Pending("Gradient and hover transition"),
    },
    Demo {
        id: "wave",
        title: "Custom Wave",
        description: "Wave effect brings dynamic. You can also use HappyProvider from @ant-design/happy-work-theme to implement dynamic wave effect.",
        preview: Preview::Pending("Custom wave effects, HappyProvider"),
    },
    Demo {
        id: "custom-disabled-bg",
        title: "Custom disabled backgroundColor",
        description: "Customize the background color with disable (applicable to type default and dashed)",
        preview: Preview::CustomDisabled,
    },
    Demo {
        id: "style-class",
        title: "Custom semantic dom styling",
        description: "You can customize the semantic dom style of Button by passing objects/functions through classNames and styles.",
        preview: Preview::SemanticStyles,
    },
];

pub(super) struct ButtonPage {
    pub scope: ConfigScope,
    pub disabled_scope: ConfigScope,
    pub loading: [bool; 4],
    anchors: Vec<ScrollAnchor>,
    loading_tasks: [Option<Task<()>>; 4],
    _config: Subscription,
    #[cfg(test)]
    pub observations: tests_support::Observations,
}

impl ButtonPage {
    pub fn new(scope: ConfigScope, anchors: Vec<ScrollAnchor>, cx: &mut Context<Self>) -> Self {
        assert_eq!(anchors.len(), DEMOS.len(), "one scroll anchor per example");
        let mut theme = ThemeConfig::default();
        theme.components.set::<ButtonTheme>(ButtonTokenPatch {
            default_bg_disabled: Some(Color::BLACK.with_alpha(0.1)),
            dashed_bg_disabled: Some(Color::BLACK.with_alpha(0.4)),
            ..Default::default()
        });
        let disabled_scope = scope
            .child(
                ConfigPatch {
                    theme: Some(theme),
                    ..Default::default()
                },
                cx,
            )
            .expect("valid disabled background customization");
        Self {
            _config: scope.observe(cx),
            scope,
            disabled_scope,
            loading: [false; 4],
            anchors,
            loading_tasks: std::array::from_fn(|_| None),
            #[cfg(test)]
            observations: Default::default(),
        }
    }

    fn control(
        &self,
        id: impl Into<SharedString>,
        label: &'static str,
        configure: impl FnOnce(Button) -> Button,
    ) -> AnyElement {
        self.control_with_action(id, label, configure, |_, _, _| {})
    }

    fn control_with_action(
        &self,
        id: impl Into<SharedString>,
        label: &'static str,
        configure: impl FnOnce(Button) -> Button,
        on_click: impl Fn(&Activation, &mut Window, &mut App) + 'static,
    ) -> AnyElement {
        let id = id.into();
        #[cfg(test)]
        let (observations, observed_id) = (self.observations.clone(), id.clone());
        let button = configure(
            Button::new(id.clone())
                .automation_id(id.clone())
                .label(label)
                .scope(self.scope.clone()),
        )
        .on_click(move |activation, window, cx| {
            on_click(activation, window, cx);
            #[cfg(test)]
            observations
                .borrow_mut()
                .activations
                .push(observed_id.clone());
        });
        #[cfg(test)]
        let button =
            tests_support::ObservedButton::new(button, id.clone(), self.observations.clone());
        div()
            .id(id.clone())
            .debug_selector(move || id.to_string())
            .flex()
            .flex_shrink_0()
            .child(button)
            .into_any_element()
    }

    fn start_loading(&mut self, index: usize, cx: &mut Context<Self>) {
        self.loading[index] = true;
        self.loading_tasks[index] = Some(cx.spawn(async move |page, cx| {
            cx.background_executor().timer(Duration::from_secs(3)).await;
            let _ = page.update(cx, |page, cx| {
                page.loading[index] = false;
                page.loading_tasks[index] = None;
                cx.notify();
            });
        }));
        cx.notify();
    }

    fn row(
        &self,
        controls: impl IntoIterator<Item = AnyElement>,
        cx: &Context<Self>,
    ) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        div()
            .flex()
            .flex_wrap()
            .when(config.direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
            .items_center()
            .gap(px(tokens.margin_xs as f32))
            .children(controls)
            .into_any_element()
    }

    fn column(&self, rows: impl IntoIterator<Item = AnyElement>, cx: &Context<Self>) -> AnyElement {
        let tokens = &self.scope.snapshot(cx).theme.tokens;
        div()
            .w_full()
            .flex()
            .flex_col()
            .gap(px(tokens.margin_xs as f32))
            .children(rows)
            .into_any_element()
    }

    fn ghost(
        &self,
        controls: impl IntoIterator<Item = AnyElement>,
        cx: &Context<Self>,
    ) -> AnyElement {
        let tokens = &self.scope.snapshot(cx).theme.tokens;
        div()
            .p(px(tokens.padding as f32))
            // Exact .site-button-ghost-wrapper background from the reference page.
            .bg(Color::rgb(0xbec8c8))
            .child(self.row(controls, cx))
            .into_any_element()
    }

    fn preview(&self, preview: Preview, window: &Window, cx: &Context<Self>) -> AnyElement {
        const TYPES: [(ButtonType, &str); 5] = [
            (ButtonType::Primary, "Primary"),
            (ButtonType::Default, "Default"),
            (ButtonType::Dashed, "Dashed"),
            (ButtonType::Text, "Text"),
            (ButtonType::Link, "Link"),
        ];
        match preview {
            Preview::Basic => self.row(
                TYPES.map(|(kind, _)| {
                    let label = match kind {
                        ButtonType::Primary => "Primary Button",
                        ButtonType::Default => "Default Button",
                        ButtonType::Dashed => "Dashed Button",
                        ButtonType::Text => "Text Button",
                        ButtonType::Link => "Link Button",
                    };
                    self.control(format!("basic.{kind:?}"), label, |button| {
                        button.button_type(kind)
                    })
                }),
                cx,
            ),
            Preview::ColorVariant => {
                // Match the reference demo's responsive ConfigProvider size.
                let breakpoint = self.scope.snapshot(cx).theme.tokens.screen_xxl;
                let size = if window.viewport_size().width >= px(breakpoint as f32) {
                    Size::Medium
                } else {
                    Size::Small
                };
                self.column(
                    [
                        ButtonColor::Default,
                        ButtonColor::Primary,
                        ButtonColor::Danger,
                        ButtonColor::Preset(PresetColor::Pink),
                        ButtonColor::Preset(PresetColor::Purple),
                        ButtonColor::Preset(PresetColor::Cyan),
                    ]
                    .into_iter()
                    .enumerate()
                    .map(|(index, color)| {
                        self.row(
                            [
                                (ButtonVariant::Solid, "Solid"),
                                (ButtonVariant::Outlined, "Outlined"),
                                (ButtonVariant::Dashed, "Dashed"),
                                (ButtonVariant::Filled, "Filled"),
                                (ButtonVariant::Text, "Text"),
                                (ButtonVariant::Link, "Link"),
                            ]
                            .map(|(variant, label)| {
                                self.control(
                                    format!("color-variant.{index}.{label}"),
                                    label,
                                    |button| button.color(color).variant(variant).size(size),
                                )
                            }),
                            cx,
                        )
                    }),
                    cx,
                )
            }
            Preview::Disabled => {
                let pairs = [
                    (
                        ButtonType::Primary,
                        "Primary",
                        "Primary(disabled)",
                        false,
                        false,
                    ),
                    (
                        ButtonType::Default,
                        "Default",
                        "Default(disabled)",
                        false,
                        false,
                    ),
                    (
                        ButtonType::Dashed,
                        "Dashed",
                        "Dashed(disabled)",
                        false,
                        false,
                    ),
                    (ButtonType::Text, "Text", "Text(disabled)", false, false),
                    (ButtonType::Link, "Link", "Link(disabled)", false, false),
                    (
                        ButtonType::Primary,
                        "Href Primary",
                        "Href Primary(disabled)",
                        false,
                        true,
                    ),
                    (
                        ButtonType::Default,
                        "Danger Default",
                        "Danger Default(disabled)",
                        true,
                        false,
                    ),
                    (
                        ButtonType::Text,
                        "Danger Text",
                        "Danger Text(disabled)",
                        true,
                        false,
                    ),
                    (
                        ButtonType::Link,
                        "Danger Link",
                        "Danger Link(disabled)",
                        true,
                        false,
                    ),
                ]
                .into_iter()
                .enumerate()
                .map(|(index, (kind, enabled, disabled, danger, href))| {
                    self.row(
                        [(false, enabled), (true, disabled)].map(|(disabled, label)| {
                            self.control(format!("disabled.{index}.{disabled}"), label, |button| {
                                button
                                    .button_type(kind)
                                    .disabled(disabled)
                                    .danger(danger)
                                    .when(href, |button| button.href("https://ant.design/index-cn"))
                            })
                        }),
                        cx,
                    )
                });
                self.column(
                    pairs.chain(std::iter::once(self.ghost(
                        [(false, "Ghost"), (true, "Ghost(disabled)")].map(|(disabled, label)| {
                            self.control(format!("disabled.ghost.{disabled}"), label, |button| {
                                button.ghost(true).disabled(disabled)
                            })
                        }),
                        cx,
                    ))),
                    cx,
                )
            }
            Preview::Ghost => self.ghost(
                [
                    (ButtonType::Primary, "Primary", false),
                    (ButtonType::Default, "Default", false),
                    (ButtonType::Dashed, "Dashed", false),
                    (ButtonType::Primary, "Danger", true),
                ]
                .map(|(kind, label, danger)| {
                    self.control(format!("ghost.{label}"), label, |button| {
                        button.button_type(kind).danger(danger).ghost(true)
                    })
                }),
                cx,
            ),
            Preview::Loading => {
                let permanent = self.row(
                    [
                        self.control("loading.always", "Loading", |button| {
                            button.primary().loading(true)
                        }),
                        self.control("loading.small", "Loading", |button| {
                            button.primary().size(Size::Small).loading(true)
                        }),
                        self.control("loading.icon", "", |button| {
                            button
                                .primary()
                                .icon(IconName::PoweroffOutlined)
                                .loading(true)
                                .accessibility_label("Loading")
                        }),
                        self.control("loading.custom", "Loading Icon", |button| {
                            button
                                .primary()
                                .loading(true)
                                .loading_icon(IconName::SyncOutlined)
                                .loading_icon_spin(true)
                        }),
                    ],
                    cx,
                );
                let interactive = self.row(
                    [
                        ("start", "Icon Start", 0, false, false),
                        ("end", "Icon End", 2, false, false),
                        ("replace", "Icon Replace", 1, true, false),
                        ("only", "", 3, true, false),
                        ("custom-action", "Loading Icon", 3, true, true),
                    ]
                    .map(|(id, label, index, icon, custom)| {
                        let page = cx.entity().downgrade();
                        self.control_with_action(
                            format!("loading.{id}"),
                            label,
                            |button| {
                                button
                                    .primary()
                                    .loading(self.loading[index])
                                    .when(id == "end", |button| {
                                        button.icon_placement(
                                            ant_design_gpui::button::IconPlacement::End,
                                        )
                                    })
                                    .when(icon, |button| button.icon(IconName::PoweroffOutlined))
                                    .when(custom, |button| {
                                        button
                                            .loading_icon(IconName::SyncOutlined)
                                            .loading_icon_spin(true)
                                    })
                                    .when(label.is_empty(), |button| {
                                        button.accessibility_label("Power")
                                    })
                            },
                            move |_, _, cx| {
                                let _ = page.update(cx, |page, cx| page.start_loading(index, cx));
                            },
                        )
                    }),
                    cx,
                );
                self.column([permanent, interactive], cx)
            }
            Preview::Danger => self.row(
                TYPES.map(|(kind, label)| {
                    self.control(format!("danger.{label}"), label, |button| {
                        button.button_type(kind).danger(true)
                    })
                }),
                cx,
            ),
            Preview::Block => self.column(
                [
                    (ButtonType::Primary, "Primary", false),
                    (ButtonType::Default, "Default", false),
                    (ButtonType::Dashed, "Dashed", false),
                    (ButtonType::Default, "disabled", true),
                    (ButtonType::Text, "text", false),
                    (ButtonType::Link, "Link", false),
                ]
                .map(|(kind, label, disabled)| {
                    self.control(format!("block.{label}"), label, |button| {
                        button.button_type(kind).disabled(disabled).block(true)
                    })
                }),
                cx,
            ),
            Preview::CustomDisabled => self.row(
                [
                    (ButtonType::Primary, "Primary Button"),
                    (ButtonType::Default, "Default Button"),
                    (ButtonType::Dashed, "Dashed Button"),
                ]
                .map(|(kind, label)| {
                    self.control(format!("custom-disabled-bg.{kind:?}"), label, |button| {
                        button
                            .scope(self.disabled_scope.clone())
                            .button_type(kind)
                            .disabled(true)
                    })
                }),
                cx,
            ),
            Preview::SemanticStyles => {
                let tokens = &self.scope.snapshot(cx).theme.tokens;
                self.row(
                    [
                        (ButtonType::Default, "Object"),
                        (ButtonType::Primary, "Function"),
                    ]
                    .map(|(kind, label)| {
                        self.control(format!("style-class.{label}"), label, |button| {
                            button
                                .button_type(kind)
                                .h_auto()
                                .py(px(tokens.padding_xs as f32))
                                .px(px(tokens.padding as f32))
                                .border(px(tokens.line_width as f32))
                                .border_color(tokens.color_border)
                                .rounded(px(tokens.border_radius as f32))
                                .content_style(
                                    StyleRefinement::default().text_color(tokens.color_text),
                                )
                                .when(kind == ButtonType::Default, |button| {
                                    button.shadow(vec![BoxShadow {
                                        color: Color::BLACK.with_alpha(0.05).to_hsla(),
                                        offset: point(px(0.), px(1.)),
                                        blur_radius: px(2.),
                                        spread_radius: px(0.),
                                        inset: false,
                                    }])
                                })
                                .when(kind == ButtonType::Primary, |button| {
                                    button.bg(Color::rgb(0x171717)).content_style(
                                        StyleRefinement::default().text_color(Color::WHITE),
                                    )
                                })
                        })
                    }),
                    cx,
                )
            }
            Preview::Pending(missing) => {
                debug_assert!(
                    !missing.is_empty(),
                    "pending demos document their missing capabilities"
                );
                let tokens = &self.scope.snapshot(cx).theme.tokens;
                div()
                    .text_size(px(tokens.font_size_sm as f32))
                    .line_height(relative(tokens.line_height as f32))
                    .text_color(tokens.color_text_tertiary)
                    .child(format!(
                        "The full reference example will be available with {missing}."
                    ))
                    .into_any_element()
            }
        }
    }
}

impl Render for ButtonPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let viewport_width = window.viewport_size().width;
        let content_width =
            viewport_width - sidebar_width(viewport_width) - page_padding(viewport_width) * 2.;
        let columns = if content_width >= px(840.) { 2 } else { 1 };
        div()
            .id("gallery.button.examples")
            .w_full()
            .grid()
            .grid_cols(columns)
            .items_start()
            .when(config.direction == Direction::Rtl, |page| page.text_right())
            .gap(px(tokens.margin_lg as f32))
            .children(DEMOS.iter().enumerate().map(|(index, demo)| {
                let pending = matches!(demo.preview, Preview::Pending(_));
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
                            .flex()
                            .items_center()
                            .justify_between()
                            .gap(px(tokens.margin_sm as f32))
                            .child(
                                div()
                                    .min_w(px(0.))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child(demo.title),
                            )
                            .when(pending, |header| {
                                header.child(
                                    div()
                                        .flex_shrink_0()
                                        .rounded(px(tokens.border_radius_sm as f32))
                                        .bg(tokens.color_fill_quaternary)
                                        .px(px(tokens.padding_xs as f32))
                                        .py(px(tokens.padding_xxs as f32))
                                        .text_size(px(tokens.font_size_sm as f32))
                                        .text_color(tokens.color_text_tertiary)
                                        .child("Planned"),
                                )
                            }),
                    )
                    .child(
                        div()
                            .id("preview")
                            .role(Role::Group)
                            .aria_label(format!("{} preview", demo.title))
                            .debug_selector(|| format!("gallery.preview.{}", demo.id))
                            .when(!pending, |preview| {
                                preview.min_h(px(120.)).p(px(tokens.padding_lg as f32))
                            })
                            .when(pending, |preview| {
                                preview
                                    .min_h(px(64.))
                                    .px(px(tokens.padding as f32))
                                    .pb(px(tokens.padding as f32))
                            })
                            .flex()
                            .items_center()
                            .child(self.preview(demo.preview, window, cx)),
                    )
                    .child(
                        div()
                            .border_t_1()
                            .border_color(tokens.color_border_secondary)
                            .p(px(tokens.padding as f32))
                            .flex()
                            .flex_col()
                            .gap(px(tokens.margin_xs as f32))
                            .text_size(px(tokens.font_size_sm as f32))
                            .line_height(relative(tokens.line_height as f32))
                            .text_color(tokens.color_text_secondary)
                            .child(div().child(demo.description)),
                    )
            }))
    }
}

// Observe the actual rendered controls in offscreen tests without a production hook.
#[cfg(test)]
pub(super) mod tests_support {
    use std::{cell::RefCell, collections::HashMap, rc::Rc};

    use ant_design_gpui::{Button, behavior::focus_handle};
    use gpui::{
        App, Element, FocusHandle, IntoElement, RenderOnce, SharedString, Window, accesskit,
    };

    #[derive(Default)]
    pub struct Observation {
        pub focus: HashMap<SharedString, FocusHandle>,
        pub semantics: HashMap<SharedString, accesskit::Node>,
        pub activations: Vec<SharedString>,
    }

    pub type Observations = Rc<RefCell<Observation>>;

    #[derive(IntoElement)]
    pub struct ObservedButton {
        button: Button,
        id: SharedString,
        observations: Observations,
    }

    impl ObservedButton {
        pub fn new(button: Button, id: SharedString, observations: Observations) -> Self {
            Self {
                button,
                id,
                observations,
            }
        }
    }

    impl RenderOnce for ObservedButton {
        fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
            let focus = self
                .observations
                .borrow_mut()
                .focus
                .entry(self.id.clone())
                .or_insert_with(|| focus_handle(cx))
                .clone();
            let button = self.button.focus_handle(&focus);
            let element = button.render(window, cx).into_element();
            let mut semantics =
                accesskit::Node::new(element.a11y_role().expect("button semantics"));
            element.write_a11y_info(&mut semantics);
            self.observations
                .borrow_mut()
                .semantics
                .insert(self.id, semantics);
            element
        }
    }
}
