// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::sync::OnceLock;

use ant_design_gpui::{
    Button, ConfigScope, Direction,
    button::ButtonType,
    icons::{IconName, IconPalette, icon},
    theme::AlgorithmChain,
};
use gpui::{
    AnyElement, AppContext, Context, Entity, FontWeight, IntoElement, Pixels, Render, Role,
    ScrollAnchor, ScrollHandle, Subscription, Window, div, prelude::*, px, relative,
};

use super::{
    button::{ButtonPage, DEMOS, Preview},
    catalog::{COMPONENTS, ComponentId},
    checkbox::{self, CheckboxPage},
    context_menu::{self, ContextMenuPage},
    divider::{self, DividerPage},
    input::{self, InputPage},
    input_examples,
    input_number::{self, InputNumberPage},
    menu::{self, MenuPage},
    pagination::{self, PaginationPage},
    popup::{self, Kind, PopupPage},
    radio::{self, RadioPage},
    scroll_area::{self, ScrollAreaPage},
    segmented::{self, SegmentedPage},
    select::{self, SelectPage},
    slider::{self, SliderPage},
    switch::{self, SwitchPage},
    tag::{self, TagPage},
};

pub(super) fn sidebar_width(viewport_width: Pixels) -> Pixels {
    px(if viewport_width >= px(1040.) {
        232.
    } else {
        72.
    })
}

pub(super) fn page_padding(viewport_width: Pixels) -> Pixels {
    px(if viewport_width < px(800.) { 24. } else { 32. })
}

fn theme_algorithms() -> &'static [AlgorithmChain; 4] {
    static ALGORITHMS: OnceLock<[AlgorithmChain; 4]> = OnceLock::new();
    ALGORITHMS.get_or_init(|| {
        [
            AlgorithmChain::default(),
            AlgorithmChain::dark(),
            AlgorithmChain::compact(),
            AlgorithmChain::dark_compact(),
        ]
    })
}

#[derive(Clone, Copy)]
enum DisplayControl {
    Theme,
    Density,
    Direction,
    Motion,
}

pub(super) struct Gallery {
    pub scope: ConfigScope,
    pub selected: ComponentId,
    pub button: Entity<ButtonPage>,
    pub checkbox: Entity<CheckboxPage>,
    pub divider: Entity<DividerPage>,
    pub input: Entity<InputPage>,
    pub input_number: Entity<InputNumberPage>,
    pub select: Entity<SelectPage>,
    pub slider: Entity<SliderPage>,
    pub radio: Entity<RadioPage>,
    pub segmented: Entity<SegmentedPage>,
    pub switch: Entity<SwitchPage>,
    pub tag: Entity<TagPage>,
    pub tooltip: Entity<PopupPage>,
    pub popover: Entity<PopupPage>,
    pub popconfirm: Entity<PopupPage>,
    pub context_menu: Entity<ContextMenuPage>,
    pub menu: Entity<MenuPage>,
    pub pagination: Entity<PaginationPage>,
    pub scroll_area: Entity<ScrollAreaPage>,
    pub scroll: ScrollHandle,
    anchors: Vec<ScrollAnchor>,
    checkbox_anchors: Vec<ScrollAnchor>,
    divider_anchors: Vec<ScrollAnchor>,
    input_anchors: Vec<ScrollAnchor>,
    input_number_anchors: Vec<ScrollAnchor>,
    select_anchors: Vec<ScrollAnchor>,
    slider_anchors: Vec<ScrollAnchor>,
    radio_anchors: Vec<ScrollAnchor>,
    segmented_anchors: Vec<ScrollAnchor>,
    switch_anchors: Vec<ScrollAnchor>,
    tag_anchors: Vec<ScrollAnchor>,
    tooltip_anchors: Vec<ScrollAnchor>,
    popover_anchors: Vec<ScrollAnchor>,
    popconfirm_anchors: Vec<ScrollAnchor>,
    context_menu_anchors: Vec<ScrollAnchor>,
    menu_anchors: Vec<ScrollAnchor>,
    pagination_anchors: Vec<ScrollAnchor>,
    scroll_area_anchors: Vec<ScrollAnchor>,
    _config: Subscription,
}

impl Gallery {
    pub fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let scroll = ScrollHandle::new();
        let anchors: Vec<_> = DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let checkbox_anchors: Vec<_> = checkbox::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let divider_anchors: Vec<_> = divider::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let input_anchors: Vec<_> = input::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let segmented_anchors: Vec<_> = segmented::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let select_anchors: Vec<_> = select::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let slider_anchors: Vec<_> = slider::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let input_number_anchors: Vec<_> = input_number::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let switch_anchors: Vec<_> = switch::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let tag_anchors: Vec<_> = tag::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let radio_anchors: Vec<_> = radio::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let tooltip_anchors: Vec<_> = popup::TOOLTIP_DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let popover_anchors: Vec<_> = popup::POPOVER_DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let popconfirm_anchors: Vec<_> = popup::POPCONFIRM_DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let context_menu_anchors: Vec<_> = context_menu::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let menu_anchors: Vec<_> = menu::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let pagination_anchors: Vec<_> = pagination::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        let scroll_area_anchors: Vec<_> = scroll_area::DEMOS
            .iter()
            .map(|_| ScrollAnchor::for_handle(scroll.clone()))
            .collect();
        Self {
            menu: cx.new(|cx| MenuPage::new(scope.clone(), menu_anchors.clone(), window, cx)),
            pagination: cx.new(|cx| {
                PaginationPage::new(scope.clone(), pagination_anchors.clone(), window, cx)
            }),
            context_menu: cx.new(|cx| {
                ContextMenuPage::new(scope.clone(), context_menu_anchors.clone(), window, cx)
            }),
            scroll_area: cx.new(|cx| {
                ScrollAreaPage::new(scope.clone(), scroll_area_anchors.clone(), window, cx)
            }),
            button: cx.new(|cx| ButtonPage::new(scope.clone(), anchors.clone(), cx)),
            checkbox: cx.new(|cx| CheckboxPage::new(scope.clone(), checkbox_anchors.clone(), cx)),
            divider: cx.new(|cx| DividerPage::new(scope.clone(), divider_anchors.clone(), cx)),
            input: cx.new(|cx| {
                InputPage::new(
                    scope.clone(),
                    input_anchors.clone(),
                    scroll.clone(),
                    window,
                    cx,
                )
            }),
            select: cx.new(|cx| SelectPage::new(scope.clone(), select_anchors.clone(), window, cx)),
            slider: cx.new(|cx| SliderPage::new(scope.clone(), slider_anchors.clone(), window, cx)),
            input_number: cx.new(|cx| {
                InputNumberPage::new(
                    scope.clone(),
                    input_number_anchors.clone(),
                    scroll.clone(),
                    window,
                    cx,
                )
            }),
            radio: cx.new(|cx| RadioPage::new(scope.clone(), radio_anchors.clone(), cx)),
            segmented: cx
                .new(|cx| SegmentedPage::new(scope.clone(), segmented_anchors.clone(), cx)),
            switch: cx.new(|cx| SwitchPage::new(scope.clone(), switch_anchors.clone(), cx)),
            tag: cx.new(|cx| TagPage::new(scope.clone(), tag_anchors.clone(), cx)),
            tooltip: cx.new(|cx| {
                PopupPage::new(Kind::Tooltip, scope.clone(), tooltip_anchors.clone(), cx)
            }),
            popover: cx.new(|cx| {
                PopupPage::new(Kind::Popover, scope.clone(), popover_anchors.clone(), cx)
            }),
            popconfirm: cx.new(|cx| {
                PopupPage::new(
                    Kind::Popconfirm,
                    scope.clone(),
                    popconfirm_anchors.clone(),
                    cx,
                )
            }),
            _config: scope.observe(cx),
            scope,
            selected: ComponentId::Button,
            scroll,
            anchors,
            checkbox_anchors,
            divider_anchors,
            input_anchors,
            input_number_anchors,
            select_anchors,
            slider_anchors,
            radio_anchors,
            segmented_anchors,
            switch_anchors,
            tag_anchors,
            tooltip_anchors,
            popover_anchors,
            popconfirm_anchors,
            context_menu_anchors,
            menu_anchors,
            pagination_anchors,
            scroll_area_anchors,
        }
    }

    fn appearance(&self, cx: &gpui::App) -> (bool, bool) {
        let config = self.scope.snapshot(cx);
        let algorithms = config.theme_config().algorithms.as_ref();
        let presets = theme_algorithms();
        (
            algorithms == Some(&presets[1]) || algorithms == Some(&presets[3]),
            algorithms == Some(&presets[2]) || algorithms == Some(&presets[3]),
        )
    }

    fn toggle(&mut self, control: DisplayControl, cx: &mut Context<Self>) {
        let config = self.scope.snapshot(cx);
        let (mut dark, mut compact) = self.appearance(cx);
        self.scope
            .update_patch(cx, |patch| match control {
                DisplayControl::Theme | DisplayControl::Density => {
                    match control {
                        DisplayControl::Theme => dark = !dark,
                        DisplayControl::Density => compact = !compact,
                        _ => unreachable!(),
                    }
                    patch.theme.get_or_insert_default().algorithms = Some(
                        theme_algorithms()[usize::from(dark) + 2 * usize::from(compact)].clone(),
                    );
                }
                DisplayControl::Direction => {
                    patch.direction = Some(if config.direction == Direction::Ltr {
                        Direction::Rtl
                    } else {
                        Direction::Ltr
                    })
                }
                DisplayControl::Motion => patch.motion = Some(!config.motion),
            })
            .expect("valid gallery display settings");
        cx.notify();
    }

    fn display_control(
        &self,
        control: DisplayControl,
        id: &'static str,
        label: &'static str,
        icon: IconName,
        active: bool,
        cx: &Context<Self>,
    ) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        div()
            .id(id)
            .debug_selector(move || id.into())
            .child(
                Button::new(id)
                    .automation_id(id)
                    .scope(self.scope.clone())
                    .button_type(ButtonType::Text)
                    .label(label)
                    .icon(icon)
                    .accessibility_description(match control {
                        DisplayControl::Theme => "Toggle light and dark themes",
                        DisplayControl::Density => "Toggle standard and compact density",
                        DisplayControl::Direction => {
                            "Toggle left-to-right and right-to-left component previews"
                        }
                        DisplayControl::Motion => "Toggle component animations",
                    })
                    .when(active, |button| {
                        button
                            .bg(tokens.color_primary_bg)
                            .text_color(tokens.color_primary)
                    })
                    .on_click(cx.listener(move |this, _, _, cx| this.toggle(control, cx))),
            )
            .into_any_element()
    }

    fn top_control(&self, id: &'static str, label: &'static str, cx: &Context<Self>) -> AnyElement {
        div()
            .id(id)
            .debug_selector(move || id.into())
            .child(
                Button::new(id)
                    .scope(self.scope.clone())
                    .button_type(ButtonType::Text)
                    .label(label)
                    .icon(IconName::ArrowUpOutlined)
                    .automation_id(id)
                    .accessibility_label("Back to top")
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.scroll.set_offset(Default::default());
                        cx.notify();
                    })),
            )
            .into_any_element()
    }

    fn toolbar(&self, window: &Window, cx: &Context<Self>) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let (dark, compact) = self.appearance(cx);
        div()
            .id("gallery.toolbar")
            .debug_selector(|| "gallery.toolbar".into())
            .role(Role::Group)
            .aria_label("Preview settings")
            .flex_shrink_0()
            .min_h(px(64.))
            .px(px(24.))
            .py(px(12.))
            .border_b_1()
            .border_color(tokens.color_border_secondary)
            .bg(tokens.color_bg_container)
            .flex()
            .flex_wrap()
            .items_center()
            .justify_between()
            .gap(px(8.))
            .when(window.viewport_size().width >= px(1040.), |toolbar| {
                toolbar.child(div().text_color(tokens.color_text_tertiary).child(
                    match self.selected {
                        ComponentId::Button => "General / Button",
                        ComponentId::Divider => "Layout / Divider",
                        ComponentId::Checkbox => "Data Entry / Checkbox",
                        ComponentId::Radio => "Data Entry / Radio",
                        ComponentId::Segmented => "Data Entry / Segmented",
                        ComponentId::Switch => "Data Entry / Switch",
                        ComponentId::Input => "Data Entry / Input",
                        ComponentId::InputNumber => "Data Entry / InputNumber",
                        ComponentId::Select => "Data Entry / Select",
                        ComponentId::Slider => "Data Entry / Slider",
                        ComponentId::Tag => "Data Display / Tag",
                        ComponentId::Tooltip => "Data Display / Tooltip",
                        ComponentId::Popover => "Data Display / Popover",
                        ComponentId::Popconfirm => "Feedback / Popconfirm",
                        ComponentId::ContextMenu => "Navigation / Context Menu",
                        ComponentId::Menu => "Navigation / Menu",
                        ComponentId::Pagination => "Navigation / Pagination",
                        ComponentId::ScrollArea => "Layout / Scroll Area",
                    },
                ))
            })
            .child(
                div()
                    .flex()
                    .flex_wrap()
                    .items_center()
                    .gap(px(4.))
                    .children([
                        self.display_control(
                            DisplayControl::Theme,
                            "gallery.control.theme",
                            if dark { "Dark" } else { "Light" },
                            if dark {
                                IconName::MoonOutlined
                            } else {
                                IconName::SunOutlined
                            },
                            dark,
                            cx,
                        ),
                        self.display_control(
                            DisplayControl::Density,
                            "gallery.control.density",
                            if compact { "Compact" } else { "Comfortable" },
                            IconName::CompressOutlined,
                            compact,
                            cx,
                        ),
                        self.display_control(
                            DisplayControl::Direction,
                            "gallery.control.direction",
                            if config.direction == Direction::Rtl {
                                "RTL"
                            } else {
                                "LTR"
                            },
                            IconName::SwapOutlined,
                            config.direction == Direction::Rtl,
                            cx,
                        ),
                        self.display_control(
                            DisplayControl::Motion,
                            "gallery.control.motion",
                            if config.motion {
                                "Motion on"
                            } else {
                                "Motion off"
                            },
                            if config.motion {
                                IconName::PlayCircleOutlined
                            } else {
                                IconName::PauseCircleOutlined
                            },
                            !config.motion,
                            cx,
                        ),
                        self.top_control("gallery.control.top", "", cx),
                    ]),
            )
            .into_any_element()
    }

    fn navigation(&self, window: &Window, cx: &Context<Self>) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let expanded = sidebar_width(window.viewport_size().width) > px(72.);
        let mut entries = div().flex().flex_col().gap(px(4.));
        let mut group = None;
        for component in COMPONENTS {
            let id = component.id;
            if expanded && group != Some(component.group) {
                entries = entries.child(
                    div()
                        .px(px(12.))
                        .pt(px(if group.is_some() { 16. } else { 0. }))
                        .pb(px(4.))
                        .text_size(px(12.))
                        .text_color(tokens.color_text_tertiary)
                        .child(component.group),
                );
            }
            group = Some(component.group);
            entries = entries.child(
                div()
                    .id(component.navigation_id)
                    .debug_selector(|| component.navigation_id.into())
                    .child(
                        Button::new(component.navigation_id)
                            .automation_id(component.navigation_id)
                            .scope(self.scope.clone())
                            .label(if expanded { component.title } else { "" })
                            .icon(match id {
                                ComponentId::Divider => IconName::BorderHorizontalOutlined,
                                ComponentId::Segmented => IconName::SwitcherOutlined,
                                ComponentId::Switch => IconName::SwapOutlined,
                                ComponentId::Input => IconName::EditOutlined,
                                ComponentId::InputNumber => IconName::FieldNumberOutlined,
                                ComponentId::Select => IconName::DownSquareOutlined,
                                ComponentId::Slider => IconName::SlidersOutlined,
                                ComponentId::Tag => IconName::TagsOutlined,
                                ComponentId::ContextMenu => IconName::MenuOutlined,
                                ComponentId::Menu => IconName::MenuFoldOutlined,
                                ComponentId::Pagination => IconName::EllipsisOutlined,
                                ComponentId::ScrollArea => IconName::SwapOutlined,
                                _ => IconName::AppstoreOutlined,
                            })
                            .accessibility_label(component.title)
                            .accessibility_description(if self.selected == id {
                                "Selected component"
                            } else {
                                "View component examples"
                            })
                            .button_type(ButtonType::Text)
                            .block(true)
                            .w_full()
                            .h(px(40.))
                            .when(expanded, |button| button.justify_start())
                            .when(self.selected == id, |button| {
                                button
                                    .bg(tokens.color_primary_bg)
                                    .text_color(tokens.color_primary)
                            })
                            .on_click(cx.listener(move |this, _, _, cx| {
                                if this.selected != id {
                                    this.selected = id;
                                    this.scroll.set_offset(Default::default());
                                    cx.notify();
                                }
                            })),
                    ),
            );
        }
        if expanded {
            entries = entries.child(
                div()
                    .px(px(12.))
                    .pt(px(24.))
                    .pb(px(8.))
                    .text_size(px(12.))
                    .text_color(tokens.color_text_tertiary)
                    .child("On this page"),
            );
            let links = DEMOS
                .iter()
                .zip(&self.anchors)
                .filter(|_| self.selected == ComponentId::Button)
                .map(|(demo, anchor)| {
                    (
                        demo.id,
                        demo.title,
                        matches!(demo.preview, Preview::Pending(_)),
                        anchor,
                    )
                })
                .chain(
                    checkbox::DEMOS
                        .iter()
                        .zip(&self.checkbox_anchors)
                        .filter(|_| self.selected == ComponentId::Checkbox)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    divider::DEMOS
                        .iter()
                        .zip(&self.divider_anchors)
                        .filter(|_| self.selected == ComponentId::Divider)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    segmented::DEMOS
                        .iter()
                        .zip(&self.segmented_anchors)
                        .filter(|_| self.selected == ComponentId::Segmented)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    input::DEMOS
                        .iter()
                        .zip(&self.input_anchors)
                        .filter(|_| self.selected == ComponentId::Input)
                        .map(|(demo, anchor)| {
                            (demo.id, demo.title, demo.coverage.planned(), anchor)
                        }),
                )
                .chain(
                    select::DEMOS
                        .iter()
                        .zip(&self.select_anchors)
                        .filter(|_| self.selected == ComponentId::Select)
                        .map(|(demo, anchor)| {
                            (demo.id, demo.title, demo.coverage.planned(), anchor)
                        }),
                )
                .chain(
                    slider::DEMOS
                        .iter()
                        .zip(&self.slider_anchors)
                        .filter(|_| self.selected == ComponentId::Slider)
                        .map(|(demo, anchor)| {
                            (demo.id, demo.title, demo.coverage.planned(), anchor)
                        }),
                )
                .chain(
                    input_number::DEMOS
                        .iter()
                        .zip(&self.input_number_anchors)
                        .filter(|_| self.selected == ComponentId::InputNumber)
                        .map(|(demo, anchor)| {
                            (demo.id, demo.title, demo.coverage.planned(), anchor)
                        }),
                )
                .chain(
                    switch::DEMOS
                        .iter()
                        .zip(&self.switch_anchors)
                        .filter(|_| self.selected == ComponentId::Switch)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    tag::DEMOS
                        .iter()
                        .zip(&self.tag_anchors)
                        .filter(|_| self.selected == ComponentId::Tag)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    radio::DEMOS
                        .iter()
                        .zip(&self.radio_anchors)
                        .filter(|_| self.selected == ComponentId::Radio)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    popup::TOOLTIP_DEMOS
                        .iter()
                        .zip(&self.tooltip_anchors)
                        .filter(|_| self.selected == ComponentId::Tooltip)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    popup::POPOVER_DEMOS
                        .iter()
                        .zip(&self.popover_anchors)
                        .filter(|_| self.selected == ComponentId::Popover)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    popup::POPCONFIRM_DEMOS
                        .iter()
                        .zip(&self.popconfirm_anchors)
                        .filter(|_| self.selected == ComponentId::Popconfirm)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                );
            let links = links
                .chain(
                    context_menu::DEMOS
                        .iter()
                        .zip(&self.context_menu_anchors)
                        .filter(|_| self.selected == ComponentId::ContextMenu)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    menu::DEMOS
                        .iter()
                        .zip(&self.menu_anchors)
                        .filter(|_| self.selected == ComponentId::Menu)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    pagination::DEMOS
                        .iter()
                        .zip(&self.pagination_anchors)
                        .filter(|_| self.selected == ComponentId::Pagination)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                )
                .chain(
                    scroll_area::DEMOS
                        .iter()
                        .zip(&self.scroll_area_anchors)
                        .filter(|_| self.selected == ComponentId::ScrollArea)
                        .map(|(demo, anchor)| (demo.id, demo.title, false, anchor)),
                );
            for (id, title, pending, anchor) in links {
                let anchor = anchor.clone();
                let debug_id = format!("gallery.jump.{id}");
                entries = entries.child(
                    div().id(id).debug_selector(move || debug_id.clone()).child(
                        Button::new(id)
                            .scope(self.scope.clone())
                            .button_type(ButtonType::Text)
                            .block(true)
                            .h(px(30.))
                            .px(px(12.))
                            .justify_start()
                            .label(match id {
                                "custom-disabled-bg" => "Disabled background",
                                "style-class" => "Semantic styling",
                                "divider-size" => "Spacing size",
                                "divider-style-class" => "Semantic styling",
                                "radio-style-class" => "Semantic styling",
                                "radio-options" => "Options",
                                "radio-keyboard" => "Keyboard navigation",
                                "segmented-style-class" => "Semantic styling",
                                "tag-style-class" => "Semantic styling",
                                _ => title,
                            })
                            .text_size(px(12.))
                            .text_color(if pending {
                                tokens.color_text_tertiary
                            } else {
                                tokens.color_text_secondary
                            })
                            .accessibility_description(format!(
                                "Jump to {}{}",
                                title,
                                if pending { ", planned preview" } else { "" }
                            ))
                            .on_click(cx.listener(move |_, _, window, cx| {
                                anchor.scroll_to(window, cx);
                                let entity = cx.weak_entity();
                                window.on_next_frame(move |_, cx| {
                                    let _ = entity.update(cx, |_, cx| cx.notify());
                                });
                                cx.notify();
                            })),
                    ),
                );
            }
        }
        div()
            .id("gallery.navigation")
            .role(Role::Navigation)
            .aria_label("Components and examples")
            .debug_selector(|| "gallery.sidebar".into())
            .w(sidebar_width(window.viewport_size().width))
            .h_full()
            .flex_shrink_0()
            .border_r_1()
            .border_color(tokens.color_border_secondary)
            .bg(tokens.color_bg_container)
            .flex()
            .flex_col()
            .child(
                div()
                    .h(px(88.))
                    .flex_shrink_0()
                    .px(px(if expanded { 24. } else { 20. }))
                    .flex()
                    .items_center()
                    .gap(px(12.))
                    .child(
                        icon(IconName::AntDesignOutlined)
                            .size(px(28.))
                            .palette(IconPalette {
                                text: tokens.color_primary.to_rgba(),
                                ..config.icon_palette
                            }),
                    )
                    .when(expanded, |brand| {
                        brand.child(
                            div()
                                .flex()
                                .flex_col()
                                .gap(px(2.))
                                .child(
                                    div()
                                        .text_size(px(16.))
                                        .font_weight(FontWeight::SEMIBOLD)
                                        .child("Ant Design GPUI"),
                                )
                                .child(
                                    div()
                                        .text_size(px(12.))
                                        .text_color(tokens.color_text_tertiary)
                                        .child("Component gallery"),
                                ),
                        )
                    }),
            )
            .child(
                div()
                    .id("gallery.navigation.scroll")
                    .flex_1()
                    .min_h(px(0.))
                    .overflow_y_scroll()
                    .px(px(if expanded { 12. } else { 16. }))
                    .when(expanded, |nav| {
                        nav.child(
                            div()
                                .px(px(12.))
                                .pb(px(8.))
                                .text_size(px(12.))
                                .text_color(tokens.color_text_tertiary)
                                .child("Components"),
                        )
                    })
                    .child(entries),
            )
            .child(
                div()
                    .flex_shrink_0()
                    .border_t_1()
                    .border_color(tokens.color_border_secondary)
                    .p(px(12.))
                    .flex()
                    .flex_col()
                    .gap(px(8.))
                    .child(
                        Button::new("gallery.reference")
                            .scope(self.scope.clone())
                            .button_type(ButtonType::Text)
                            .block(true)
                            .w_full()
                            .label(if expanded { "Ant Design reference" } else { "" })
                            .icon(IconName::BookOutlined)
                            .accessibility_label(match self.selected {
                                ComponentId::Button => "Open Ant Design Button reference",
                                ComponentId::Divider => "Open Ant Design Divider reference",
                                ComponentId::Checkbox => "Open Ant Design Checkbox reference",
                                ComponentId::Radio => "Open Ant Design Radio reference",
                                ComponentId::Segmented => "Open Ant Design Segmented reference",
                                ComponentId::Switch => "Open Ant Design Switch reference",
                                ComponentId::Input => "Open Ant Design Input reference",
                                ComponentId::InputNumber => "Open Ant Design InputNumber reference",
                                ComponentId::Select => "Open Ant Design Select reference",
                                ComponentId::Slider => "Open Ant Design Slider reference",
                                ComponentId::Tag => "Open Ant Design Tag reference",
                                ComponentId::Tooltip => "Open Ant Design Tooltip reference",
                                ComponentId::Popover => "Open Ant Design Popover reference",
                                ComponentId::Popconfirm => "Open Ant Design Popconfirm reference",
                                ComponentId::ContextMenu => "Open Ant Design Dropdown reference",
                                ComponentId::Menu => "Open Ant Design Menu reference",
                                ComponentId::Pagination => "Open Ant Design Pagination reference",
                                ComponentId::ScrollArea => "Open Ant Design design principles",
                            })
                            .href(match self.selected {
                                ComponentId::Button => "https://ant.design/components/button",
                                ComponentId::Divider => "https://ant.design/components/divider",
                                ComponentId::Checkbox => "https://ant.design/components/checkbox",
                                ComponentId::Radio => "https://ant.design/components/radio",
                                ComponentId::Segmented => "https://ant.design/components/segmented",
                                ComponentId::Switch => "https://ant.design/components/switch",
                                ComponentId::Input => "https://ant.design/components/input",
                                ComponentId::InputNumber => {
                                    "https://ant.design/components/input-number"
                                }
                                ComponentId::Select => "https://ant.design/components/select",
                                ComponentId::Slider => "https://ant.design/components/slider",
                                ComponentId::ContextMenu => {
                                    "https://ant.design/components/dropdown"
                                }
                                ComponentId::ScrollArea => "https://ant.design/docs/spec/motion",
                                ComponentId::Menu => "https://ant.design/components/menu",
                                ComponentId::Pagination => {
                                    "https://ant.design/components/pagination"
                                }
                                ComponentId::Tag => "https://ant.design/components/tag",
                                ComponentId::Tooltip => "https://ant.design/components/tooltip",
                                ComponentId::Popover => "https://ant.design/components/popover",
                                ComponentId::Popconfirm => {
                                    "https://ant.design/components/popconfirm"
                                }
                            }),
                    )
                    .when(expanded, |footer| {
                        footer.child(
                            div()
                                .px(px(12.))
                                .text_size(px(12.))
                                .text_color(tokens.color_text_tertiary)
                                .child("Ant Design 6.6.7 · GPUI"),
                        )
                    }),
            )
            .into_any_element()
    }
}

impl Render for Gallery {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let component = COMPONENTS
            .iter()
            .find(|component| component.id == self.selected)
            .expect("registered component page");
        let live = match self.selected {
            ComponentId::Button => DEMOS
                .iter()
                .filter(|demo| !matches!(demo.preview, Preview::Pending(_)))
                .count(),
            ComponentId::Checkbox => checkbox::DEMOS.len(),
            ComponentId::Divider => divider::DEMOS.len(),
            ComponentId::Radio => radio::DEMOS.len(),
            ComponentId::Segmented => segmented::DEMOS.len(),
            ComponentId::Switch => switch::DEMOS.len(),
            ComponentId::Input => input::DEMOS.len(),
            ComponentId::InputNumber => input_number::DEMOS.len(),
            ComponentId::Select => select::DEMOS.len(),
            ComponentId::Slider => slider::DEMOS.len(),
            ComponentId::Tag => tag::DEMOS.len(),
            ComponentId::Tooltip => popup::TOOLTIP_DEMOS.len(),
            ComponentId::Popover => popup::POPOVER_DEMOS.len(),
            ComponentId::Popconfirm => popup::POPCONFIRM_DEMOS.len(),
            ComponentId::ContextMenu => context_menu::DEMOS.len(),
            ComponentId::Menu => menu::DEMOS.len(),
            ComponentId::Pagination => pagination::DEMOS.len(),
            ComponentId::ScrollArea => scroll_area::DEMOS.len(),
        };
        let coverage = match self.selected {
            ComponentId::Input => input_examples::summary(input::DEMOS),
            ComponentId::InputNumber => input_examples::summary(input_number::DEMOS),
            ComponentId::Select => input_examples::summary(select::DEMOS),
            ComponentId::Slider => input_examples::summary(slider::DEMOS),
            ComponentId::Menu => input_examples::summary(menu::DEMOS),
            ComponentId::Pagination => input_examples::summary(pagination::DEMOS),
            _ => format!("{live} live examples"),
        };
        div()
            .id("gallery")
            .size_full()
            .flex()
            .overflow_hidden()
            .text_left()
            .child(self.navigation(window, cx))
            .child(
                div()
                    .flex_1()
                    .min_w(px(0.))
                    .h_full()
                    .flex()
                    .flex_col()
                    .child(self.toolbar(window, cx))
                    .child(
                        div()
                            .id("gallery.content")
                            .role(Role::Main)
                            .aria_label(format!("{} examples", component.title))
                            .debug_selector(|| "gallery.content".into())
                            .flex_1()
                            .min_h(px(0.))
                            .overflow_y_scroll()
                            .track_scroll(&self.scroll)
                            .child(
                                div()
                                    .w_full()
                                    .max_w(px(1360.))
                                    .mx_auto()
                                    .p(page_padding(window.viewport_size().width))
                                    .flex()
                                    .flex_col()
                                    .gap(px(24.))
                                    .child(
                                        div()
                                            .flex()
                                            .flex_wrap()
                                            .items_start()
                                            .justify_between()
                                            .gap(px(16.))
                                            .child(
                                                div()
                                                    .flex()
                                                    .flex_col()
                                                    .gap(px(8.))
                                                    .child(
                                                        div()
                                                            .text_size(px(12.))
                                                            .text_color(tokens.color_text_tertiary)
                                                            .child(component.group),
                                                    )
                                                    .child(
                                                        div()
                                                            .id("gallery.title")
                                                            .role(Role::Heading)
                                                            .text_size(px(32.))
                                                            .line_height(relative(1.25))
                                                            .font_weight(FontWeight::SEMIBOLD)
                                                            .child(component.title),
                                                    )
                                                    .child(
                                                        div()
                                                            .text_color(tokens.color_text_secondary)
                                                            .child(component.description),
                                                    ),
                                            )
                                            .child(
                                                div()
                                                    .text_size(px(12.))
                                                    .text_color(tokens.color_text_tertiary)
                                                    .child(coverage),
                                            ),
                                    )
                                    .child(match self.selected {
                                        ComponentId::Button => {
                                            self.button.clone().into_any_element()
                                        }
                                        ComponentId::Checkbox => {
                                            self.checkbox.clone().into_any_element()
                                        }
                                        ComponentId::Divider => {
                                            self.divider.clone().into_any_element()
                                        }
                                        ComponentId::Radio => {
                                            self.radio.clone().into_any_element()
                                        }
                                        ComponentId::Segmented => {
                                            self.segmented.clone().into_any_element()
                                        }
                                        ComponentId::Switch => {
                                            self.switch.clone().into_any_element()
                                        }
                                        ComponentId::Input => self.input.clone().into_any_element(),
                                        ComponentId::InputNumber => self.input_number.clone().into_any_element(),
                                        ComponentId::Select => self.select.clone().into_any_element(),
                                        ComponentId::Slider => self.slider.clone().into_any_element(),
                                        ComponentId::Tag => {
                                            self.tag.clone().into_any_element()
                                        }
                                        ComponentId::Tooltip => self.tooltip.clone().into_any_element(),
                                        ComponentId::Popover => self.popover.clone().into_any_element(),
                                        ComponentId::Popconfirm => self.popconfirm.clone().into_any_element(),
                                        ComponentId::ContextMenu => self.context_menu.clone().into_any_element(),
                                        ComponentId::Menu => self.menu.clone().into_any_element(),
                                        ComponentId::Pagination => self.pagination.clone().into_any_element(),
                                        ComponentId::ScrollArea => self.scroll_area.clone().into_any_element(),

                                    })
                                    .child(
                                        div()
                                            .flex()
                                            .flex_wrap()
                                            .items_center()
                                            .justify_between()
                                            .gap(px(16.))
                                            .text_size(px(12.))
                                            .text_color(tokens.color_text_tertiary)
                                            .child(match self.selected {
                                                ComponentId::Select => "Type to search · Arrow keys to navigate · Enter to select · Escape to dismiss",
                                                ComponentId::ContextMenu => "Right-click to open · Shift+F10 from the keyboard · Arrow keys to navigate · Escape to dismiss",
                                                ComponentId::Menu => "Tab to enter · Arrow keys to navigate · Enter or Space to select · Escape to close a submenu",
                                                ComponentId::ScrollArea => "Wheel or drag to scroll · Shift+wheel for horizontal · Arrow and Page keys when focused",
                                                ComponentId::Slider => "Tab to navigate · Arrow keys to step · Home or End to reach bounds",
                                                ComponentId::Pagination => "Tab to navigate · Enter or Space to change page · Enter to jump to a page",
                                                ComponentId::Button => {
                                                    "Tab to navigate · Enter or Space to activate"
                                                }
                                                ComponentId::Checkbox => {
                                                    "Tab to navigate · Space to toggle"
                                                }
                                                ComponentId::Divider => {
                                                    "Start and end follow the preview direction"
                                                }
                                                ComponentId::Radio => {
                                                    "Tab to enter a group · Arrows to select · Space to select"
                                                }
                                                ComponentId::Segmented => {
                                                    "Tab to enter a group · Arrow keys select · Space to select"
                                                }
                                                ComponentId::Switch => {
                                                    "Tab to navigate · Enter or Space to toggle · Arrow keys set state"
                                                }
                                                ComponentId::Input => "Tab to navigate · Enter to commit · Ctrl/Cmd+Z to undo",
                                                ComponentId::InputNumber => "Tab to navigate · Arrow keys to step · Enter to commit",
                                                ComponentId::Tooltip | ComponentId::Popover | ComponentId::Popconfirm => "Tab to navigate · Enter or Space to open · Escape to dismiss",
                                                ComponentId::Tag => {
                                                    "Tab to navigate · Space to select · Enter or Space to close"
                                                }
                                            })
                                            .child(
                                                div()
                                                    .id("gallery.footer.top")
                                                    .debug_selector(|| "gallery.footer.top".into())
                                                    .child(
                                                        Button::new("gallery.top")
                                                            .scope(self.scope.clone())
                                                            .button_type(ButtonType::Text)
                                                            .label("Back to top")
                                                            .icon(IconName::ArrowUpOutlined)
                                                            .on_click(cx.listener(
                                                                |this, _, _, cx| {
                                                                    this.scroll.set_offset(
                                                                        Default::default(),
                                                                    );
                                                                    cx.notify();
                                                                },
                                                            )),
                                                    ),
                                            ),
                                    ),
                            ),
                    ),
            )
    }
}
