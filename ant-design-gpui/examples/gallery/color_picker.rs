// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT).

use std::collections::BTreeMap;

use ant_design_gpui::{
    Button, ConfigScope, Size,
    button::ButtonType,
    color_picker::{
        ColorPicker, ColorPickerChange, ColorPickerFormat, ColorPickerMode, ColorPickerPanel,
        ColorPickerPreset, ColorPickerState, ColorPickerStyles, ColorPickerValue, ColorStop,
        PickerColor,
    },
    icons::{IconName, icon},
    popup::PopupTriggers,
    theme::{Color, PresetColor, generate_palette},
};
use gpui::{
    AnyElement, AppContext, Context, Entity, IntoElement, Render, ScrollAnchor, StyleRefinement,
    Subscription, Window, div, prelude::*, px,
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
        "base",
        "Basic Usage",
        "Select a color from the palette or edit its channels.",
    ),
    demo(
        "size",
        "Trigger size",
        "Small, medium and large triggers, with and without color text.",
    ),
    demo(
        "controlled",
        "controlled mode",
        "The first picker updates during selection; the second accepts the color when selection ends.",
    ),
    demo(
        "line-gradient",
        "Line Gradient",
        "Switch between a single color and an editable linear gradient, or restrict selection to gradients.",
    ),
    demo(
        "text-render",
        "Rendering Trigger Text",
        "Show a color value, render custom text, or add a disclosure icon.",
    ),
    demo(
        "disabled",
        "Disable",
        "A disabled trigger preserves its color and stays out of keyboard traversal.",
    ),
    demo(
        "disabled-alpha",
        "Disabled Alpha",
        "Choose an opaque color without alpha controls.",
    ),
    demo(
        "allowClear",
        "Clear Color",
        "Clear the selected color and choose another color when needed.",
    ),
    demo(
        "trigger",
        "Custom Trigger",
        "A custom button opens the picker and reflects its selected color.",
    ),
    demo(
        "trigger-event",
        "Custom Trigger Event",
        "Hover over the trigger to open the panel.",
    ),
    demo(
        "format",
        "Color Format",
        "Edit and display HEX, HSB and RGB values using the same precise color model.",
    ),
    demo(
        "presets",
        "Preset Colors",
        "Expand the primary, red and green palettes and choose a preset.",
    ),
    demo(
        "panel-render",
        "Custom Render Panel",
        "Add a title or arrange presets beside the picker using native content.",
    ),
    demo(
        "style-class",
        "Custom semantic dom styling",
        "Apply semantic styles directly or resolve them from the trigger size.",
    ),
    demo(
        "presets-line-gradient",
        "Preset Line Gradient",
        "Choose a complete gradient from a preset group.",
    ),
    demo(
        "pure-panel",
        "Pure Render",
        "Embed the same native editing panel directly in a view.",
    ),
];

pub(super) struct ColorPickerPage {
    scope: ConfigScope,
    pub states: BTreeMap<&'static str, Entity<ColorPickerState>>,
    pub controlled: ColorPickerValue,
    pub custom: ColorPickerValue,
    pub disclosure_open: bool,
    anchors: Vec<ScrollAnchor>,
    _config: Subscription,
    _states: Vec<Subscription>,
}

fn color(text: &str) -> PickerColor {
    PickerColor::parse(text).expect("valid demo color")
}
fn gradient(start: &str, end: &str) -> ColorPickerValue {
    ColorPickerValue::gradient([
        ColorStop::new(color(start), 0.).unwrap(),
        ColorStop::new(color(end), 100.).unwrap(),
    ])
    .unwrap()
}

impl ColorPickerPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let extras = [
            "size.small",
            "size.large",
            "size.small.text",
            "size.medium.text",
            "size.large.text",
            "controlled.complete",
            "line-gradient.only",
            "text-render.custom",
            "text-render.icon",
            "format.hsb",
            "format.rgb",
            "panel-render.horizontal",
            "style-class.large",
        ];
        let states: BTreeMap<_, _> = DEMOS
            .iter()
            .map(|demo| demo.id)
            .chain(extras)
            .map(|key| {
                let state = cx.new(|cx| {
                    let mut state = ColorPickerState::new(window, cx);
                    let initial = if key.starts_with("line-gradient") {
                        gradient("rgb(16, 142, 233)", "rgb(135, 208, 104)")
                    } else if key == "style-class.large" {
                        color("#722ed1").into()
                    } else if key == "format.hsb" {
                        color("hsb(215, 91%, 100%)").into()
                    } else {
                        color("#1677ff").into()
                    };
                    state.set_value(initial, window, cx).unwrap();
                    if key == "format.hsb" {
                        state.set_format(ColorPickerFormat::Hsb, window, cx);
                    }
                    if key == "format.rgb" {
                        state.set_format(ColorPickerFormat::Rgb, window, cx);
                    }
                    state
                });
                (key, state)
            })
            .collect();
        let observers = states
            .values()
            .map(|state| cx.observe(state, |_, _, cx| cx.notify()))
            .collect();
        Self {
            _config: scope.observe(cx),
            _states: observers,
            scope,
            states,
            controlled: color("#1677ff").into(),
            custom: color("#1677ff").into(),
            disclosure_open: false,
            anchors,
        }
    }
    fn picker(&self, key: &'static str) -> ColorPicker {
        ColorPicker::new(key, &self.states[key])
            .scope(self.scope.clone())
            .accessibility_label(format!("{} color", key.replace(['.', '-'], " ")))
            .automation_id(format!("gallery.color-picker.{key}"))
    }
    fn presets(&self, cx: &gpui::App, include_cyan: bool) -> Vec<ColorPickerPreset> {
        let config = self.scope.snapshot(cx);
        let g = &config.theme.tokens;
        let mut palettes = vec![
            ("primary", generate_palette(g.color_primary, false)),
            ("red", g.palettes.get(PresetColor::Red).tones),
            ("green", g.palettes.get(PresetColor::Green).tones),
        ];
        if include_cyan {
            palettes.push(("cyan", g.palettes.get(PresetColor::Cyan).tones));
        }
        palettes
            .into_iter()
            .map(|(key, colors)| {
                ColorPickerPreset::new(key, key, colors.into_iter().map(ColorPickerValue::from))
            })
            .collect()
    }
    fn preview(&self, id: &'static str, cx: &mut Context<Self>) -> AnyElement {
        let disclosure_open = self.disclosure_open;
        let disclosure_state = cx.entity().downgrade();
        match id {
            "base" => self.picker(id).into_any_element(),
            "size" => row(&self.scope, cx)
                .child(
                    column()
                        .w_auto()
                        .child(self.picker("size.small").size(Size::Small))
                        .child(self.picker("size"))
                        .child(self.picker("size.large").size(Size::Large)),
                )
                .child(
                    column()
                        .w_auto()
                        .child(
                            self.picker("size.small.text")
                                .size(Size::Small)
                                .show_text(true),
                        )
                        .child(self.picker("size.medium.text").show_text(true))
                        .child(
                            self.picker("size.large.text")
                                .size(Size::Large)
                                .show_text(true),
                        ),
                )
                .into_any_element(),
            "controlled" => row(&self.scope, cx)
                .child(
                    self.picker(id)
                        .value(self.controlled.clone())
                        .on_change(cx.listener(|this, change: &ColorPickerChange, _, cx| {
                            this.controlled = change.value.clone();
                            cx.notify();
                        })),
                )
                .child(
                    self.picker("controlled.complete")
                        .value(self.controlled.clone())
                        .on_change_complete(cx.listener(
                            |this, change: &ColorPickerChange, _, cx| {
                                this.controlled = change.value.clone();
                                cx.notify();
                            },
                        )),
                )
                .into_any_element(),
            "line-gradient" => column()
                .child(
                    self.picker(id)
                        .modes([ColorPickerMode::Single, ColorPickerMode::Gradient])
                        .allow_clear(true)
                        .show_text(true),
                )
                .child(
                    self.picker("line-gradient.only")
                        .modes([ColorPickerMode::Gradient])
                        .allow_clear(true)
                        .show_text(true),
                )
                .into_any_element(),
            "text-render" => column()
                .child(self.picker(id).show_text(true).allow_clear(true))
                .child(
                    self.picker("text-render.custom")
                        .text_render(|value, _, _| {
                            format!("Custom Text ({})", value.active_color(0).to_hex_string())
                        }),
                )
                .child(
                    self.picker("text-render.icon")
                        .open(self.disclosure_open)
                        .on_open_change(move |open, _, cx| {
                            let _ = disclosure_state.update(cx, |this, cx| {
                                this.disclosure_open = open;
                                cx.notify();
                            });
                        })
                        .text_render(move |_, _, _| {
                            icon(IconName::DownOutlined)
                                .size(px(12.))
                                .inherit_color()
                                .text_color(Color::BLACK.with_alpha(0.25))
                                .rotation(gpui::radians(if disclosure_open {
                                    std::f32::consts::PI
                                } else {
                                    0.
                                }))
                        }),
                )
                .into_any_element(),
            "disabled" => self.picker(id).disabled(true).into_any_element(),
            "disabled-alpha" => self.picker(id).disabled_alpha(true).into_any_element(),
            "allowClear" => self.picker(id).allow_clear(true).into_any_element(),
            "trigger" => self
                .picker(id)
                .value(self.custom.clone())
                .on_change(cx.listener(|this, change: &ColorPickerChange, _, cx| {
                    this.custom = change.value.clone();
                    cx.notify();
                }))
                .child(
                    Button::new("color-picker.custom.trigger")
                        .scope(self.scope.clone())
                        .button_type(ButtonType::Primary)
                        .label("open")
                        .bg(self.custom.active_color(0).to_theme_color()),
                )
                .into_any_element(),
            "trigger-event" => self
                .picker(id)
                .trigger(PopupTriggers::HOVER)
                .into_any_element(),
            "format" => column()
                .children(
                    [
                        ("format", "HEX"),
                        ("format.hsb", "HSB"),
                        ("format.rgb", "RGB"),
                    ]
                    .into_iter()
                    .map(|(key, label)| {
                        let value = self.states[key].read(cx).value().active_color(0);
                        let text = match label {
                            "HSB" => value.to_hsb_string(),
                            "RGB" => value.to_rgb_string(),
                            _ => value.to_hex_string(),
                        };
                        row(&self.scope, cx)
                            .child(self.picker(key))
                            .child(format!("{label}: {text}"))
                    }),
                )
                .into_any_element(),
            "presets" => self
                .picker(id)
                .presets(self.presets(cx, false))
                .into_any_element(),
            "presets-line-gradient" => self
                .picker(id)
                .modes([ColorPickerMode::Single, ColorPickerMode::Gradient])
                .presets([ColorPickerPreset::new(
                    "liner",
                    "Liner",
                    [
                        gradient("rgb(42,188,191)", "rgb(56,54,221)"),
                        gradient("rgb(34,73,254)", "rgb(199,74,168)"),
                        gradient("rgb(255,111,4)", "rgb(243,48,171)"),
                        gradient("rgb(244,170,6)", "rgb(229,70,49)"),
                    ],
                )])
                .into_any_element(),
            "panel-render" => column()
                .child(row(&self.scope, cx).child("Add title:").child(
                    self.picker(id).panel_render(|parts, window, cx| {
                        column()
                            .gap(px(8.))
                            .child(div().text_size(px(12.)).child("Color Picker"))
                            .child(parts.panel(window, cx))
                    }),
                ))
                .child(
                    row(&self.scope, cx).child("Horizontal layout:").child(
                        self.picker("panel-render.horizontal")
                            .presets(self.presets(cx, true))
                            .panel_render(|parts, window, cx| {
                                div()
                                    .flex()
                                    .gap(px(12.))
                                    .w(px(480.))
                                    .child(parts.presets(window, cx))
                                    .child(div().w(px(1.)).bg(Color::rgb(0xf0f0f0)))
                                    .child(parts.picker(window, cx))
                            }),
                    ),
                )
                .into_any_element(),
            "style-class" => row(&self.scope, cx)
                .child(
                    self.picker(id).arrow(false).styles(ColorPickerStyles {
                        popup: StyleRefinement::default()
                            .border_1()
                            .border_color(Color::WHITE),
                        ..Default::default()
                    }),
                )
                .child(
                    self.picker("style-class.large")
                        .size(Size::Large)
                        .arrow(false)
                        .styles_fn(|context, _, _| ColorPickerStyles {
                            popup: if context.size == Size::Large {
                                StyleRefinement::default()
                                    .border_1()
                                    .border_color(Color::rgb(0x722ed1))
                            } else {
                                StyleRefinement::default()
                            },
                            ..Default::default()
                        }),
                )
                .into_any_element(),
            "pure-panel" => ColorPickerPanel::new(id, &self.states[id])
                .scope(self.scope.clone())
                .into_any_element(),
            _ => unreachable!("registered color picker demo"),
        }
    }
}
impl Render for ColorPickerPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        examples::grid(&self.scope, window, cx)
            .id("gallery.color-picker.examples")
            .children(DEMOS.iter().zip(&self.anchors).map(|(demo, anchor)| {
                examples::card(
                    "color-picker",
                    demo,
                    anchor.clone(),
                    self.preview(demo.id, cx),
                    &self.scope,
                    cx,
                )
            }))
    }
}
