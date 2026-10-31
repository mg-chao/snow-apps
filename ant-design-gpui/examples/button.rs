// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native Button gallery. Run with `--capture <path.png>` for a rendered frame.

use std::{path::PathBuf, time::Duration};

use ant_design_gpui::{
    Button, ConfigScope, Direction, Size, UiRoot,
    button::{ButtonColor, ButtonShape, ButtonType, ButtonVariant, IconPlacement},
    icons::IconName,
    theme::{AlgorithmChain, PresetColor, ThemeConfig},
};
use gpui::{
    AnyElement, App, AppContext, Bounds, Context, Focusable, FontWeight, IntoElement, Render, Role,
    SharedString, StyleRefinement, Subscription, Task, Window, WindowBounds, WindowOptions, div,
    prelude::*, px, size,
};

#[derive(Default)]
struct Options {
    capture: Option<PathBuf>,
    fixture_state: Option<PathBuf>,
    dark: bool,
    compact: bool,
    rtl: bool,
    reduced_motion: bool,
}

impl Options {
    fn parse() -> Self {
        let mut options = Self::default();
        let mut arguments = std::env::args().skip(1);
        while let Some(argument) = arguments.next() {
            match argument.as_str() {
                "--capture" => {
                    options.capture = Some(arguments.next().expect("capture output path").into());
                }
                "--fixture-state" => {
                    options.fixture_state =
                        Some(arguments.next().expect("fixture state output path").into());
                }
                "--dark" => options.dark = true,
                "--compact" => options.compact = true,
                "--rtl" => options.rtl = true,
                "--reduced-motion" => options.reduced_motion = true,
                argument => panic!("unknown argument: {argument}"),
            }
        }
        options
    }
}

struct Gallery {
    scope: ConfigScope,
    dark: bool,
    compact: bool,
    loading: bool,
    delayed_loading: bool,
    loading_task: Option<Task<()>>,
    delayed_loading_task: Option<Task<()>>,
    invocations: usize,
    last_action: SharedString,
    fixture_state: Option<PathBuf>,
    _config: Subscription,
}

impl Gallery {
    fn new(scope: ConfigScope, options: &Options, cx: &mut Context<Self>) -> Self {
        let subscription = scope.observe(cx);
        if let Some(path) = &options.fixture_state {
            std::fs::write(path, "0\n").expect("write initial fixture state");
        }
        Self {
            scope,
            dark: options.dark,
            compact: options.compact,
            loading: false,
            delayed_loading: false,
            loading_task: None,
            delayed_loading_task: None,
            invocations: 0,
            last_action: "Ready".into(),
            fixture_state: options.fixture_state.clone(),
            _config: subscription,
        }
    }

    fn record(&mut self, label: SharedString, cx: &mut Context<Self>) {
        self.invocations += 1;
        self.last_action = label;
        if let Some(path) = &self.fixture_state {
            std::fs::write(path, format!("{}\n", self.invocations)).expect("write fixture state");
        }
        cx.notify();
    }

    fn demo(
        &self,
        id: impl Into<SharedString>,
        label: impl Into<SharedString>,
        cx: &Context<Self>,
    ) -> Button {
        let id = id.into();
        let label = label.into();
        let action = label.clone();
        let entity = cx.entity().downgrade();
        Button::new(id.clone())
            .automation_id(id)
            .label(label)
            .scope(self.scope.clone())
            .on_click(move |_, _, cx| {
                let _ = entity.update(cx, |gallery, cx| gallery.record(action.clone(), cx));
            })
    }

    fn update_theme(&self, cx: &mut Context<Self>) {
        let algorithms = match (self.dark, self.compact) {
            (false, false) => AlgorithmChain::default(),
            (true, false) => AlgorithmChain::dark(),
            (false, true) => AlgorithmChain::compact(),
            (true, true) => AlgorithmChain::dark_compact(),
        };
        self.scope
            .update_patch(cx, |patch| {
                patch.theme = Some(ThemeConfig {
                    algorithms: Some(algorithms),
                    ..Default::default()
                });
            })
            .expect("valid gallery theme");
    }

    fn start_loading(&mut self, delayed: bool, cx: &mut Context<Self>) {
        self.record(
            if delayed {
                "Delayed loading"
            } else {
                "Loading"
            }
            .into(),
            cx,
        );
        if delayed {
            self.delayed_loading = true;
        } else {
            self.loading = true;
        }
        let task = cx.spawn(async move |entity, cx| {
            cx.background_executor().timer(Duration::from_secs(3)).await;
            let _ = entity.update(cx, |gallery, cx| {
                if delayed {
                    gallery.delayed_loading = false;
                    gallery.delayed_loading_task = None;
                } else {
                    gallery.loading = false;
                    gallery.loading_task = None;
                }
                cx.notify();
            });
        });
        if delayed {
            self.delayed_loading_task = Some(task);
        } else {
            self.loading_task = Some(task);
        }
        cx.notify();
    }

    fn row(&self, children: impl IntoIterator<Item = AnyElement>, cx: &App) -> AnyElement {
        div()
            .flex()
            .flex_wrap()
            .items_center()
            .gap(px(8.))
            .when(self.scope.snapshot(cx).direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
            .children(children)
            .into_any_element()
    }

    fn panel(&self, title: &'static str, content: AnyElement, cx: &App) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        div()
            .flex_1()
            .min_w(px(430.))
            .p(px(20.))
            .border_1()
            .border_color(tokens.color_border_secondary)
            .rounded(px(8.))
            .bg(tokens.color_bg_container)
            .flex()
            .flex_col()
            .gap(px(16.))
            .child(
                div()
                    .text_size(px(16.))
                    .font_weight(FontWeight::SEMIBOLD)
                    .child(title),
            )
            .child(content)
            .into_any_element()
    }

    fn panels(&self, children: [AnyElement; 2], cx: &App) -> AnyElement {
        div()
            .flex()
            .flex_wrap()
            .items_stretch()
            .gap(px(20.))
            .when(self.scope.snapshot(cx).direction == Direction::Rtl, |row| {
                row.flex_row_reverse()
            })
            .children(children)
            .into_any_element()
    }

    fn toolbar(&self, cx: &mut Context<Self>) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let entity = cx.entity().downgrade();
        let theme = Button::new("gallery.theme")
            .automation_id("gallery.theme")
            .label(if self.dark {
                "Light theme"
            } else {
                "Dark theme"
            })
            .scope(self.scope.clone())
            .disabled(false)
            .on_click(move |_, _, cx| {
                let _ = entity.update(cx, |gallery, cx| {
                    gallery.dark = !gallery.dark;
                    gallery.update_theme(cx);
                });
            });
        let entity = cx.entity().downgrade();
        let density = Button::new("gallery.density")
            .automation_id("gallery.density")
            .label(if self.compact {
                "Standard density"
            } else {
                "Compact density"
            })
            .scope(self.scope.clone())
            .disabled(false)
            .on_click(move |_, _, cx| {
                let _ = entity.update(cx, |gallery, cx| {
                    gallery.compact = !gallery.compact;
                    gallery.update_theme(cx);
                });
            });
        let scope = self.scope.clone();
        let direction = Button::new("gallery.direction")
            .automation_id("gallery.direction")
            .label(if config.direction == Direction::Rtl {
                "Left to right"
            } else {
                "Right to left"
            })
            .scope(scope.clone())
            .disabled(false)
            .on_click(move |_, _, cx| {
                let next = match scope.snapshot(cx).direction {
                    Direction::Ltr => Direction::Rtl,
                    Direction::Rtl => Direction::Ltr,
                };
                scope
                    .update_patch(cx, |patch| patch.direction = Some(next))
                    .unwrap();
            });
        let scope = self.scope.clone();
        let availability = Button::new("gallery.disabled")
            .automation_id("gallery.disabled")
            .label(if config.disabled {
                "Enable buttons"
            } else {
                "Disable buttons"
            })
            .scope(scope.clone())
            .disabled(false)
            .on_click(move |_, _, cx| {
                let disabled = !scope.snapshot(cx).disabled;
                scope
                    .update_patch(cx, |patch| patch.disabled = Some(disabled))
                    .unwrap();
            });
        let scope = self.scope.clone();
        let motion = Button::new("gallery.motion")
            .automation_id("gallery.motion")
            .label(if config.motion {
                "Reduce motion"
            } else {
                "Enable motion"
            })
            .scope(scope.clone())
            .disabled(false)
            .on_click(move |_, _, cx| {
                let motion = !scope.snapshot(cx).motion;
                scope
                    .update_patch(cx, |patch| patch.motion = Some(motion))
                    .unwrap();
            });
        self.row(
            [theme, density, direction, availability, motion].map(IntoElement::into_any_element),
            cx,
        )
    }
}

impl Render for Gallery {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let types = self.row(
            [
                ("primary", "Primary", ButtonType::Primary),
                ("default", "Default", ButtonType::Default),
                ("dashed", "Dashed", ButtonType::Dashed),
                ("text", "Text", ButtonType::Text),
                ("link", "Link", ButtonType::Link),
            ]
            .map(|(id, label, kind)| {
                self.demo(id, label, cx)
                    .button_type(kind)
                    .into_any_element()
            }),
            cx,
        );
        let danger = self.row(
            [
                ("danger-primary", "Primary", ButtonType::Primary),
                ("danger-default", "Default", ButtonType::Default),
                ("danger-dashed", "Dashed", ButtonType::Dashed),
                ("danger-text", "Text", ButtonType::Text),
                ("danger-link", "Link", ButtonType::Link),
            ]
            .map(|(id, label, kind)| {
                self.demo(id, label, cx)
                    .button_type(kind)
                    .danger(true)
                    .into_any_element()
            }),
            cx,
        );
        let basic = self.panel(
            "Button types",
            div()
                .flex()
                .flex_col()
                .gap(px(16.))
                .child(types)
                .child(danger)
                .into_any_element(),
            cx,
        );
        let variants = self.panel(
            "Color and variant",
            div()
                .flex()
                .flex_col()
                .gap(px(12.))
                .children(
                    [ButtonColor::Primary, ButtonColor::Danger]
                        .into_iter()
                        .enumerate()
                        .map(|(index, color)| {
                            self.row(
                                [
                                    ("Solid", ButtonVariant::Solid),
                                    ("Outlined", ButtonVariant::Outlined),
                                    ("Dashed", ButtonVariant::Dashed),
                                    ("Filled", ButtonVariant::Filled),
                                    ("Text", ButtonVariant::Text),
                                    ("Link", ButtonVariant::Link),
                                ]
                                .map(|(label, variant)| {
                                    self.demo(
                                        SharedString::from(format!("variant-{index}-{label}")),
                                        label,
                                        cx,
                                    )
                                    .color(color)
                                    .variant(variant)
                                    .into_any_element()
                                }),
                                cx,
                            )
                        }),
                )
                .into_any_element(),
            cx,
        );
        let sizes = self.panel(
            "Sizes",
            div()
                .flex()
                .flex_col()
                .gap(px(12.))
                .children(
                    [
                        ("large", "Large", Size::Large),
                        ("medium", "Default", Size::Medium),
                        ("small", "Small", Size::Small),
                    ]
                    .map(|(id, label, size)| {
                        self.row(
                            [
                                self.demo(id, label, cx)
                                    .primary()
                                    .size(size)
                                    .icon(IconName::DownloadOutlined),
                                self.demo(format!("{id}-icon"), "", cx)
                                    .primary()
                                    .size(size)
                                    .icon(IconName::DownloadOutlined)
                                    .accessibility_label("Download"),
                                self.demo(format!("{id}-round"), "", cx)
                                    .primary()
                                    .size(size)
                                    .shape(ButtonShape::Round)
                                    .icon(IconName::DownloadOutlined)
                                    .accessibility_label("Download"),
                                self.demo(format!("{id}-circle"), "", cx)
                                    .primary()
                                    .size(size)
                                    .shape(ButtonShape::Circle)
                                    .icon(IconName::DownloadOutlined)
                                    .accessibility_label("Download"),
                            ]
                            .map(IntoElement::into_any_element),
                            cx,
                        )
                    }),
                )
                .into_any_element(),
            cx,
        );
        let shapes = self.panel(
            "Shapes",
            self.row(
                [
                    self.demo("shape-default", "Default", cx),
                    self.demo("shape-round", "Round", cx)
                        .shape(ButtonShape::Round),
                    self.demo("shape-square", "", cx)
                        .shape(ButtonShape::Square)
                        .icon(IconName::SearchOutlined)
                        .accessibility_label("Search"),
                    self.demo("shape-circle", "", cx)
                        .button_type(ButtonType::Primary)
                        .shape(ButtonShape::Circle)
                        .icon(IconName::SearchOutlined)
                        .accessibility_label("Search"),
                ]
                .map(IntoElement::into_any_element),
                cx,
            ),
            cx,
        );
        let icons = self.panel(
            "Icons and placement",
            self.row(
                [
                    self.demo("icon-search", "Search", cx)
                        .button_type(ButtonType::Primary)
                        .icon(IconName::SearchOutlined),
                    self.demo("icon-download", "Download", cx)
                        .icon(IconName::DownloadOutlined),
                    self.demo("icon-end", "Next", cx)
                        .icon(IconName::ArrowRightOutlined)
                        .icon_placement(IconPlacement::End),
                    self.demo("icon-only", "", cx)
                        .icon(IconName::EllipsisOutlined)
                        .accessibility_label("More actions"),
                ]
                .map(IntoElement::into_any_element),
                cx,
            ),
            cx,
        );
        let entity = cx.entity().downgrade();
        let start_loading = self
            .demo("start-loading", "Click to load", cx)
            .button_type(ButtonType::Primary)
            .loading(self.loading)
            .on_click(move |_, _, cx| {
                let _ = entity.update(cx, |gallery, cx| gallery.start_loading(false, cx));
            });
        let entity = cx.entity().downgrade();
        let delayed_loading = self
            .demo("delayed-loading", "Delayed loading", cx)
            .loading(self.delayed_loading)
            .loading_delay(Duration::from_millis(600))
            .on_click(move |_, _, cx| {
                let _ = entity.update(cx, |gallery, cx| gallery.start_loading(true, cx));
            });
        let loading = self.panel(
            "Loading",
            div()
                .flex()
                .flex_col()
                .gap(px(12.))
                .child(
                    self.row(
                        [
                            self.demo("loading", "Loading", cx)
                                .button_type(ButtonType::Primary)
                                .loading(true),
                            start_loading,
                            delayed_loading,
                        ]
                        .map(IntoElement::into_any_element),
                        cx,
                    ),
                )
                .child(
                    self.row(
                        [
                            self.demo("loading-end", "Loading", cx)
                                .icon_placement(IconPlacement::End)
                                .loading(true),
                            self.demo("loading-replace", "Loading", cx)
                                .icon(IconName::PoweroffOutlined)
                                .loading(true),
                            self.demo("loading-custom", "Loading", cx)
                                .loading_icon(IconName::SmileOutlined)
                                .loading(true),
                            self.demo("loading-custom-spin", "Syncing", cx)
                                .loading_icon(IconName::SyncOutlined)
                                .loading_icon_spin(true)
                                .loading(true),
                            self.demo("loading-icon-only", "", cx)
                                .shape(ButtonShape::Circle)
                                .loading(true)
                                .accessibility_label("Loading results"),
                        ]
                        .map(IntoElement::into_any_element),
                        cx,
                    ),
                )
                .into_any_element(),
            cx,
        );
        let disabled = self.panel(
            "Disabled",
            self.row(
                [
                    self.demo("disabled-primary", "Primary", cx)
                        .button_type(ButtonType::Primary)
                        .disabled(true),
                    self.demo("disabled-default", "Default", cx).disabled(true),
                    self.demo("disabled-dashed", "Dashed", cx)
                        .button_type(ButtonType::Dashed)
                        .disabled(true),
                    self.demo("disabled-text", "Text", cx)
                        .button_type(ButtonType::Text)
                        .disabled(true),
                    self.demo("disabled-link", "Link", cx)
                        .button_type(ButtonType::Link)
                        .disabled(true),
                ]
                .map(IntoElement::into_any_element),
                cx,
            ),
            cx,
        );
        let ghost = self.panel(
            "Ghost",
            div()
                .p(px(16.))
                .rounded(px(6.))
                .bg(gpui::rgb(0x8c8c8c))
                .child(
                    self.row(
                        [
                            self.demo("ghost-primary", "Primary", cx)
                                .button_type(ButtonType::Primary)
                                .ghost(true),
                            self.demo("ghost-default", "Default", cx).ghost(true),
                            self.demo("ghost-dashed", "Dashed", cx)
                                .button_type(ButtonType::Dashed)
                                .ghost(true),
                            self.demo("ghost-danger", "Danger", cx)
                                .danger(true)
                                .ghost(true),
                        ]
                        .map(IntoElement::into_any_element),
                        cx,
                    ),
                )
                .into_any_element(),
            cx,
        );
        let colors = self.panel(
            "Preset colors",
            self.row(
                PresetColor::ALL.into_iter().map(|color| {
                    self.demo(
                        SharedString::from(format!("color-{}", color.name())),
                        color.name(),
                        cx,
                    )
                    .color(ButtonColor::Preset(color))
                    .variant(ButtonVariant::Solid)
                    .into_any_element()
                }),
                cx,
            ),
            cx,
        );
        let block = self.panel(
            "Block",
            div()
                .flex()
                .flex_col()
                .gap(px(8.))
                .child(
                    self.demo("block-primary", "Primary", cx)
                        .button_type(ButtonType::Primary)
                        .block(true),
                )
                .child(self.demo("block-default", "Default", cx).block(true))
                .into_any_element(),
            cx,
        );
        let labels = self.panel(
            "Label spacing",
            self.row(
                [
                    self.demo("label-spaced", "按钮", cx),
                    self.demo("label-unspaced", "按钮", cx)
                        .auto_insert_space(false),
                    self.demo("label-icon", "按钮", cx)
                        .icon(IconName::SearchOutlined),
                    self.demo("label-text", "按钮", cx)
                        .button_type(ButtonType::Text),
                    self.demo("label-loading", "按钮", cx).loading(true),
                ]
                .map(IntoElement::into_any_element),
                cx,
            ),
            cx,
        );
        let custom_layout = self.panel(
            "Custom height and padding",
            self.row(
                [
                    self.demo("layout-tall", "Text", cx).h(px(60.)),
                    self.demo("layout-tall-icon", "Search", cx)
                        .h(px(60.))
                        .icon(IconName::SearchOutlined),
                    self.demo("layout-wide", "Wide", cx).px(px(32.)),
                ]
                .map(IntoElement::into_any_element),
                cx,
            ),
            cx,
        );
        let semantic_styles = self.panel(
            "Content and icon styles",
            self.row(
                [
                    self.demo("semantic-icon", "Accent icon", cx)
                        .icon(IconName::StarFilled)
                        .icon_style(StyleRefinement::default().text_color(gpui::rgb(0xfaad14))),
                    self.demo("semantic-content", "Accent label", cx)
                        .icon(IconName::CheckOutlined)
                        .content_style(
                            StyleRefinement::default()
                                .text_color(gpui::rgb(0x722ed1))
                                .font_weight(FontWeight::SEMIBOLD),
                        ),
                ]
                .map(IntoElement::into_any_element),
                cx,
            ),
            cx,
        );
        let waves = self.panel(
            "Click wave",
            self.row(
                [
                    self.demo("wave-default", "Default wave", cx).primary(),
                    self.demo("wave-disabled", "Wave disabled", cx)
                        .primary()
                        .wave(false),
                ]
                .map(IntoElement::into_any_element),
                cx,
            ),
            cx,
        );
        div()
            .id("button-gallery-scroll")
            .size_full()
            .overflow_y_scroll()
            .child(
                div()
                    .w_full()
                    .p(px(28.))
                    .flex()
                    .flex_col()
                    .gap(px(20.))
                    .child(
                        div()
                            .flex()
                            .flex_col()
                            .gap(px(6.))
                            .child(
                                div()
                                    .text_size(px(30.))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child("Button"),
                            )
                            .child(
                                div()
                                    .text_color(tokens.color_text_secondary)
                                    .child("Ant Design · native GPUI components"),
                            ),
                    )
                    .child(self.toolbar(cx))
                    .child(
                        div()
                            .id("gallery.status")
                            .role(Role::Status)
                            .text_color(tokens.color_text_secondary)
                            .child(format!(
                                "{} activations · {} · Tab to navigate, Enter or Space to activate",
                                self.invocations, self.last_action
                            )),
                    )
                    .children([
                        self.panels([basic, variants], cx),
                        self.panels([sizes, shapes], cx),
                        self.panels([icons, loading], cx),
                        self.panels([disabled, ghost], cx),
                        self.panels([colors, block], cx),
                        self.panels([labels, custom_layout], cx),
                        self.panels([semantic_styles, waves], cx),
                    ]),
            )
    }
}

fn main() {
    let options = Options::parse();
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| {
                patch.direction = Some(if options.rtl {
                    Direction::Rtl
                } else {
                    Direction::Ltr
                });
                patch.motion = Some(!options.reduced_motion);
                patch.theme = Some(ThemeConfig {
                    algorithms: Some(match (options.dark, options.compact) {
                        (false, false) => AlgorithmChain::default(),
                        (true, false) => AlgorithmChain::dark(),
                        (false, true) => AlgorithmChain::compact(),
                        (true, true) => AlgorithmChain::dark_compact(),
                    }),
                    ..Default::default()
                });
            })
            .expect("valid initial gallery configuration");
        let bounds = Bounds::centered(None, size(px(1180.), px(1040.)), cx);
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                ..Default::default()
            },
            |window, cx| {
                window.set_window_title("Ant Design GPUI · Button");
                let gallery = cx.new(|cx| Gallery::new(scope.clone(), &options, cx));
                let root = cx.new(|cx| UiRoot::new(scope, gallery, window, cx));
                window.focus(&root.focus_handle(cx), cx);
                window.activate_window();
                if let Some(path) = options.capture {
                    window
                        .spawn(cx, async move |cx| {
                            cx.background_executor()
                                .timer(Duration::from_millis(300))
                                .await;
                            let result = cx.update(|window, cx| {
                                window.draw(cx).clear(cx);
                                match window.render_to_image() {
                                    Ok(image) => {
                                        if let Err(error) = image.save(&path) {
                                            eprintln!("Could not save gallery capture: {error}");
                                        }
                                    }
                                    Err(error) => {
                                        eprintln!("Could not render gallery capture: {error}");
                                    }
                                }
                            });
                            if let Err(error) = result {
                                eprintln!("Gallery closed before capture: {error}");
                            }
                        })
                        .detach();
                }
                root
            },
        )
        .expect("open Button gallery");
        cx.activate(true);
    });
}
