// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples and descriptions adapted from Ant Design 6.6.7 (MIT).

use std::time::Duration;

use ant_design_gpui::{
    Button, Checkbox, ConfigPatch, ConfigScope, Direction, Popconfirm, Popover, Tooltip, UiRoot,
    behavior::Activation,
    button::ButtonType,
    icons::IconName,
    popconfirm::{Confirmation, PopconfirmButtonOptions},
    popup::{PopupArrow, PopupHandle, PopupPlacement, PopupTriggers, TooltipConfigPatch},
    theme::{Color, PresetColor},
};
use gpui::{
    AnyElement, App, AppContext, Bounds, Context, FontWeight, IntoElement, Render, Role,
    ScrollAnchor, SharedString, StyleRefinement, Subscription, Window, WindowBounds, WindowOptions,
    div, prelude::*, px, relative, size,
};

use super::shell::{page_padding, sidebar_width};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Kind {
    Tooltip,
    Popover,
    Popconfirm,
}

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    pub description: &'static str,
}
macro_rules! demos {
    ($($id:literal => ($title:literal, $description:literal)),* $(,)?) => {
        &[$(Demo { id: $id, title: $title, description: $description }),*]
    };
}
pub(super) const TOOLTIP_DEMOS: &[Demo] = demos![
    "basic" => ("Basic", "Hover over text, or focus a button, to reveal a short explanation."),
    "smooth-transition" => ("Smooth Transition", "Move between these buttons to share one tooltip across the group."),
    "placement" => ("Placement", "Twelve physical placements keep the arrow attached to its trigger."),
    "arrow" => ("Arrow", "Show or hide the arrow, or point an edge-aligned arrow at the center of its trigger."),
    "shift" => ("Auto Shift", "Open the edge preview and move the trigger between window corners to see automatic flip and shift."),
    "force-align" => ("Realign after layout changes", "Load additional content above the trigger. Its open tooltip follows the new position."),
    "colorful" => ("Colorful Tooltip", "Use a preset palette color or choose a custom surface color."),
    "disabled" => ("Disabled", "An empty title suppresses the tooltip. The trigger remains usable."),
    "wrap-custom-component" => ("Wrap custom component", "A custom trigger keeps its own layout and content when wrapped in a tooltip."),
    "style-class" => ("Custom semantic styling", "Refine the surface and content independently."),
];
pub(super) const POPOVER_DEMOS: &[Demo] = demos![
    "basic" => ("Basic", "A floating card provides additional content and a title."),
    "triggerType" => ("Three ways to trigger", "Hover, focus, or click can independently open the same floating card. Right-click is also supported."),
    "placement" => ("Placement", "Choose from twelve placements around the trigger."),
    "arrow" => ("Arrow", "Show or hide the arrow, or point it at the center of the trigger."),
    "shift" => ("Auto Shift", "Open the edge preview to inspect overflow adjustment in a small native window."),
    "control" => ("Controlling the close of the dialog", "Visibility belongs to the page. Close the card using its content action or by pressing Escape."),
    "hover-with-click" => ("Hover with click popover", "Hover to preview, then click for a second card. Opening either card closes the other."),
    "style-class" => ("Custom semantic styling", "Style the surface, title, and content independently."),
];
pub(super) const POPCONFIRM_DEMOS: &[Demo] = demos![
    "basic" => ("Basic", "Confirm a consequential action. Cancel and confirmation report separate outcomes."),
    "locale" => ("Locale text", "Customize confirmation and cancellation labels for this dialog."),
    "placement" => ("Placement", "The same confirmation works in all twelve placements."),
    "shift" => ("Auto Shift", "Use the edge preview to inspect confirmation placement near every window corner."),
    "dynamic-trigger" => ("Conditional trigger", "Skip confirmation when direct execution is enabled; otherwise ask before continuing."),
    "icon" => ("Customize icon", "Replace the default warning icon or omit it entirely."),
    "async" => ("Asynchronously close", "The page controls loading and visibility while an operation completes."),
    "promise" => ("Asynchronously close on Task", "Returning a task keeps the dialog open and shows loading. A failed operation leaves it open for retry."),
    "style-class" => ("Custom semantic styling", "Style the title, description, icon, and action row independently."),
];

impl Kind {
    pub fn name(self) -> &'static str {
        match self {
            Self::Tooltip => "tooltip",
            Self::Popover => "popover",
            Self::Popconfirm => "popconfirm",
        }
    }
    pub fn demos(self) -> &'static [Demo] {
        match self {
            Self::Tooltip => TOOLTIP_DEMOS,
            Self::Popover => POPOVER_DEMOS,
            Self::Popconfirm => POPCONFIRM_DEMOS,
        }
    }
    fn sample(
        self,
        id: impl Into<SharedString>,
        scope: &ConfigScope,
        trigger: impl IntoElement,
        placement: PopupPlacement,
        arrow: PopupArrow,
        open: bool,
    ) -> AnyElement {
        let id = id.into();
        match self {
            Self::Tooltip => Tooltip::new(id)
                .scope(scope.clone())
                .title("Prompt text")
                .placement(placement)
                .arrow(arrow)
                .default_open(open)
                .child(trigger)
                .into_any_element(),
            Self::Popover => Popover::new(id)
                .scope(scope.clone())
                .title("Title")
                .text("Content")
                .placement(placement)
                .arrow(arrow)
                .default_open(open)
                .child(trigger)
                .into_any_element(),
            Self::Popconfirm => Popconfirm::new(id)
                .scope(scope.clone())
                .title("Delete the task")
                .description("Are you sure to delete this task?")
                .placement(placement)
                .arrow(arrow)
                .default_open(open)
                .child(trigger)
                .into_any_element(),
        }
    }
}

pub(super) struct PopupPage {
    pub kind: Kind,
    pub scope: ConfigScope,
    unique_scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub controlled: bool,
    pub hovered: bool,
    pub clicked: bool,
    pub direct: bool,
    pub conditional_open: bool,
    pub async_open: bool,
    pub async_loading: bool,
    async_generation: u64,
    pub reject_next: bool,
    pub enabled: bool,
    pub loaded: bool,
    loading_content: bool,
    align: PopupHandle,
    native_focus: gpui::FocusHandle,
    native_scroll: gpui::ScrollHandle,
    pub status: SharedString,
    _config: Subscription,
}

impl PopupPage {
    pub fn new(
        kind: Kind,
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        cx: &mut Context<Self>,
    ) -> Self {
        assert_eq!(anchors.len(), kind.demos().len());
        let unique_scope = scope
            .child(
                ConfigPatch {
                    tooltip: TooltipConfigPatch {
                        unique: Some(true),
                        ..Default::default()
                    },
                    ..Default::default()
                },
                cx,
            )
            .expect("valid unique tooltip scope");
        Self {
            kind,
            unique_scope,
            anchors,
            controlled: false,
            hovered: false,
            clicked: false,
            direct: true,
            conditional_open: false,
            async_open: false,
            async_loading: false,
            async_generation: 0,
            reject_next: false,
            enabled: false,
            loaded: false,
            loading_content: false,
            align: PopupHandle::new(),
            native_focus: cx.focus_handle().tab_index(0),
            native_scroll: gpui::ScrollHandle::new(),
            status: "Ready".into(),
            _config: scope.observe(cx),
            scope,
        }
    }
    fn confirmation(
        cx: &Context<Self>,
        callback: impl Fn(&mut Self, &Activation, &mut Window, &mut Context<Self>) -> Confirmation
        + 'static,
    ) -> impl Fn(&Activation, &mut Window, &mut App) -> Confirmation + 'static {
        let weak = cx.weak_entity();
        move |activation, window, cx| {
            weak.update(cx, |this, cx| callback(this, activation, window, cx))
                .unwrap_or(Confirmation::Close)
        }
    }
    fn button(&self, id: impl Into<SharedString>, label: impl Into<SharedString>) -> Button {
        let id = id.into();
        Button::new(id.clone())
            .scope(self.scope.clone())
            .automation_id(id)
            .label(label)
    }
    fn row(children: impl IntoIterator<Item = AnyElement>) -> AnyElement {
        div()
            .flex()
            .flex_wrap()
            .items_center()
            .gap(px(12.))
            .children(children)
            .into_any_element()
    }
    fn column(children: impl IntoIterator<Item = AnyElement>) -> AnyElement {
        div()
            .flex()
            .flex_col()
            .gap(px(16.))
            .children(children)
            .into_any_element()
    }
    fn placement(&self, arrow: PopupArrow) -> AnyElement {
        // Physical arrangement follows the reference; reading direction changes labels only.
        let entries = [
            (1, "TL", 1, 2),
            (0, "Top", 1, 3),
            (2, "TR", 1, 4),
            (7, "LT", 2, 1),
            (10, "RT", 2, 5),
            (6, "Left", 3, 1),
            (9, "Right", 3, 5),
            (8, "LB", 4, 1),
            (11, "RB", 4, 5),
            (4, "BL", 5, 2),
            (3, "Bottom", 5, 3),
            (5, "BR", 5, 4),
        ];
        div()
            .w_full()
            .grid()
            .grid_cols(5)
            .gap(px(4.))
            .children(entries.into_iter().map(|(index, label, row, col)| {
                div().col_start(col).row_start(row).child(
                    self.kind.sample(
                        format!("placement-{index}"),
                        &self.scope,
                        self.button(format!("{}.placement.{label}", self.kind.name()), label)
                            .w_full()
                            .px(px(4.)),
                        PopupPlacement::ALL[index],
                        arrow,
                        false,
                    ),
                )
            }))
            .into_any_element()
    }
    fn preview(&self, demo: &'static Demo, cx: &Context<Self>) -> AnyElement {
        let name = self.kind.name();
        let tokens = &self.scope.snapshot(cx).theme.tokens;
        match demo.id {
            "placement" => return self.placement(PopupArrow::default()),
            "arrow" => {
                return Self::row(
                    [
                        ("Show", PopupArrow::default()),
                        ("Hide", false.into()),
                        (
                            "Center",
                            PopupArrow {
                                visible: true,
                                point_at_center: true,
                            },
                        ),
                    ]
                    .into_iter()
                    .map(|(label, arrow)| {
                        self.kind.sample(
                            format!("arrow-{label}"),
                            &self.scope,
                            self.button(format!("{name}.arrow.{label}"), label),
                            PopupPlacement::TopLeft,
                            arrow,
                            false,
                        )
                    }),
                );
            }
            "shift" => {
                let scope = self.scope.clone();
                let kind = self.kind;
                return self
                    .button(format!("{name}.shift.open"), "Open edge preview")
                    .on_click(move |_, _, cx| {
                        open_edge_preview(kind, scope.clone(), cx);
                    })
                    .into_any_element();
            }
            _ => {}
        }
        match (self.kind, demo.id) {
            (Kind::Tooltip, "basic") => Tooltip::new("basic")
                .scope(self.scope.clone())
                .title("Prompt text")
                .child(div().child("Tooltip will show on mouse enter."))
                .into_any_element(),
            (Kind::Tooltip, "smooth-transition") => div()
                .grid()
                .grid_cols(2)
                .gap(px(8.))
                .children((0_usize..4).map(|i| {
                    Tooltip::new(("unique", i))
                        .scope(self.unique_scope.clone())
                        .title("Hello, Ant Design!")
                        .placement(if i < 2 {
                            PopupPlacement::Top
                        } else {
                            PopupPlacement::Bottom
                        })
                        .child(
                            self.button(format!("tooltip.unique.{i}"), "Button")
                                .button_type(ButtonType::Primary),
                        )
                }))
                .into_any_element(),
            (Kind::Tooltip, "force-align") => Self::column([
                self.button(
                    "tooltip.force-align.load",
                    if self.loaded { "Reset" } else { "Load content" },
                )
                .loading(self.loading_content)
                .on_click(cx.listener(|this, _, window, cx| {
                    if this.loaded {
                        this.loaded = false;
                        this.align.force_align(cx);
                        cx.notify();
                        return;
                    }
                    this.loading_content = true;
                    cx.spawn_in(window, async |this, cx| {
                        cx.background_executor().timer(Duration::from_secs(2)).await;
                        let _ = this.update(cx, |this, cx| {
                            this.loaded = true;
                            this.loading_content = false;
                            this.align.force_align(cx);
                            cx.notify();
                        });
                    })
                    .detach();
                    cx.notify();
                }))
                .into_any_element(),
                div()
                    .pt(px(48.))
                    .when(self.loaded, |el| {
                        el.child(div().h(px(80.)).child("Async content loaded."))
                    })
                    .child(
                        Tooltip::new("aligned")
                            .scope(self.scope.clone())
                            .handle(&self.align)
                            .title("Tooltip stays aligned")
                            .open(true)
                            .child(self.button("tooltip.force-align.trigger", "Trigger")),
                    )
                    .into_any_element(),
            ]),
            (Kind::Tooltip, "colorful") => Self::column([
                Self::row(PresetColor::ALL.into_iter().map(|color| {
                    Tooltip::new(("color", color as usize))
                        .scope(self.scope.clone())
                        .title(color.name())
                        .color(color)
                        .child(self.button(format!("tooltip.color.{}", color.name()), color.name()))
                        .into_any_element()
                })),
                Self::row(
                    [0xf50, 0x2db7f5, 0x87d068, 0x108ee9]
                        .into_iter()
                        .enumerate()
                        .map(|(i, value)| {
                            let value = if value == 0xf50 { 0xff5500 } else { value };
                            Tooltip::new(("custom-color", i))
                                .scope(self.scope.clone())
                                .title(format!("#{value:06x}"))
                                .color(Color::rgb(value))
                                .child(self.button(
                                    format!("tooltip.color.custom.{i}"),
                                    format!("#{value:06x}"),
                                ))
                                .into_any_element()
                        }),
                ),
            ]),
            (Kind::Tooltip, "disabled") => Self::column([
                Checkbox::new("enabled")
                    .scope(self.scope.clone())
                    .label("Enable tooltip")
                    .checked(self.enabled)
                    .on_change(cx.listener(
                        |this, event: &ant_design_gpui::checkbox::CheckboxChange, _, cx| {
                            this.enabled = event.checked;
                            cx.notify();
                        },
                    ))
                    .into_any_element(),
                Tooltip::new("disabled")
                    .scope(self.scope.clone())
                    .title(if self.enabled { "Prompt text" } else { "" })
                    .child(self.button("tooltip.disabled.trigger", "Hover me"))
                    .into_any_element(),
            ]),
            (Kind::Tooltip, "wrap-custom-component") => Tooltip::new("custom")
                .scope(self.scope.clone())
                .title("Prompt text")
                .child(
                    div()
                        .p(px(12.))
                        .rounded(px(6.))
                        .bg(tokens.color_fill_secondary)
                        .child("This is a custom trigger"),
                )
                .into_any_element(),
            (Kind::Tooltip, "style-class") => Tooltip::new("styled")
                .scope(self.scope.clone())
                .title("Customized tooltip")
                .color(Color::rgb(0x108ee9))
                .surface_style(StyleRefinement::default().rounded(px(12.)).p(px(12.)))
                .content_style(StyleRefinement::default().font_weight(FontWeight::SEMIBOLD))
                .child(self.button("tooltip.style.trigger", "Custom styles"))
                .into_any_element(),
            (Kind::Popover, "basic") => Popover::new("basic")
                .scope(self.scope.clone())
                .title("Title")
                .content(|_, _, _| div().child("Content").child(div().child("Content")))
                .child(
                    self.button("popover.basic.trigger", "Hover me")
                        .button_type(ButtonType::Primary),
                )
                .into_any_element(),
            (Kind::Popover, "triggerType") => Self::row(
                [
                    ("Hover", PopupTriggers::HOVER),
                    ("Focus", PopupTriggers::FOCUS),
                    ("Click", PopupTriggers::CLICK),
                    ("Right-click", PopupTriggers::CONTEXT_MENU),
                ]
                .into_iter()
                .map(|(label, triggers)| {
                    Popover::new(format!("trigger-{label}"))
                        .scope(self.scope.clone())
                        .title("Title")
                        .text("Content")
                        .triggers(triggers)
                        .child(self.button(format!("popover.trigger.{label}"), label))
                        .into_any_element()
                }),
            ),
            (Kind::Popover, "control") => {
                let weak = cx.weak_entity();
                let scope = self.scope.clone();
                Popover::new("controlled")
                    .scope(self.scope.clone())
                    .title("Title")
                    .open(self.controlled)
                    .triggers(PopupTriggers::CLICK)
                    .on_open_change(cx.listener(
                        |this, change: &ant_design_gpui::popup::PopupOpenChange, _, cx| {
                            this.controlled = change.open;
                            cx.notify();
                        },
                    ))
                    .content(move |_, _, _| {
                        let weak = weak.clone();
                        Button::new("close")
                            .scope(scope.clone())
                            .label("Close")
                            .button_type(ButtonType::Link)
                            .automation_id("popover.control.close")
                            .on_click(move |_, _, cx| {
                                let _ = weak.update(cx, |this, cx| {
                                    this.controlled = false;
                                    cx.notify();
                                });
                            })
                    })
                    .child(self.button("popover.control.trigger", "Click me"))
                    .into_any_element()
            }
            (Kind::Popover, "hover-with-click") => {
                let weak = cx.weak_entity();
                let scope = self.scope.clone();
                Popover::new("hover")
                    .scope(self.scope.clone())
                    .title("Hover title")
                    .text("This is hover content.")
                    .open(self.hovered)
                    .triggers(PopupTriggers::HOVER)
                    .on_open_change(cx.listener(
                        |this, change: &ant_design_gpui::popup::PopupOpenChange, _, cx| {
                            this.hovered = change.open;
                            if change.open {
                                this.clicked = false;
                            }
                            cx.notify();
                        },
                    ))
                    .child(
                        Popover::new("click")
                            .scope(self.scope.clone())
                            .title("Click title")
                            .open(self.clicked)
                            .triggers(PopupTriggers::CLICK)
                            .on_open_change(cx.listener(
                                |this, change: &ant_design_gpui::popup::PopupOpenChange, _, cx| {
                                    this.clicked = change.open;
                                    if change.open {
                                        this.hovered = false;
                                    }
                                    cx.notify();
                                },
                            ))
                            .content(move |_, _, _| {
                                let weak = weak.clone();
                                div().child("This is click content.").child(
                                    Button::new("close")
                                        .scope(scope.clone())
                                        .label("Close")
                                        .automation_id("popover.combined.close")
                                        .button_type(ButtonType::Link)
                                        .on_click(move |_, _, cx| {
                                            let _ = weak.update(cx, |this, cx| {
                                                this.clicked = false;
                                                this.hovered = false;
                                                cx.notify();
                                            });
                                        }),
                                )
                            })
                            .child(self.button("popover.combined.trigger", "Hover and click")),
                    )
                    .into_any_element()
            }
            (Kind::Popover, "style-class") => Popover::new("styled")
                .scope(self.scope.clone())
                .title("Custom title")
                .text("Custom content")
                .surface_style(
                    StyleRefinement::default()
                        .border_1()
                        .border_color(tokens.color_primary)
                        .rounded(px(16.)),
                )
                .title_style(StyleRefinement::default().text_color(tokens.color_primary))
                .content_style(StyleRefinement::default().italic())
                .child(self.button("popover.style.trigger", "Custom styles"))
                .into_any_element(),
            (Kind::Popconfirm, "basic" | "locale") => Popconfirm::new("basic")
                .scope(self.scope.clone())
                .title("Delete the task")
                .description("Are you sure to delete this task?")
                .when(demo.id == "locale", |popup| {
                    popup.ok_text("Yes").cancel_text("No")
                })
                .on_confirm(Self::confirmation(cx, |this, _, _, cx| {
                    this.status = "Task deleted".into();
                    cx.notify();
                    Confirmation::Close
                }))
                .on_cancel(cx.listener(|this, _, _, cx| {
                    this.status = "Deletion canceled".into();
                    cx.notify();
                }))
                .child(
                    self.button(format!("popconfirm.{}.trigger", demo.id), "Delete")
                        .danger(true),
                )
                .into_any_element(),
            (Kind::Popconfirm, "dynamic-trigger") => Self::column([
                Popconfirm::new("conditional")
                    .scope(self.scope.clone())
                    .title("Delete the task")
                    .description("Are you sure to delete this task?")
                    .ok_text("Yes")
                    .cancel_text("No")
                    .open(self.conditional_open)
                    .on_open_change(cx.listener(
                        |this, change: &ant_design_gpui::popup::PopupOpenChange, _, cx| {
                            if change.open && this.direct {
                                this.status = "Next step.".into();
                                this.conditional_open = false;
                            } else {
                                this.conditional_open = change.open;
                            }
                            cx.notify();
                        },
                    ))
                    .on_confirm(Self::confirmation(cx, |this, _, _, cx| {
                        this.conditional_open = false;
                        this.status = "Next step.".into();
                        cx.notify();
                        Confirmation::Close
                    }))
                    .on_cancel(cx.listener(|this, _, _, cx| {
                        this.status = "Click on cancel.".into();
                        cx.notify();
                    }))
                    .child(
                        self.button("popconfirm.conditional.trigger", "Delete a task")
                            .danger(true),
                    )
                    .into_any_element(),
                Checkbox::new("direct")
                    .scope(self.scope.clone())
                    .label("Execute directly")
                    .checked(self.direct)
                    .automation_id("popconfirm.conditional.direct")
                    .on_change(cx.listener(
                        |this, event: &ant_design_gpui::checkbox::CheckboxChange, _, cx| {
                            this.direct = event.checked;
                            cx.notify();
                        },
                    ))
                    .into_any_element(),
            ]),
            (Kind::Popconfirm, "icon") => Self::row([
                Popconfirm::new("custom-icon")
                    .scope(self.scope.clone())
                    .title("Delete the task")
                    .icon(IconName::QuestionCircleOutlined)
                    .icon_style(StyleRefinement::default().text_color(tokens.color_error))
                    .child(self.button("popconfirm.icon.custom", "Custom icon"))
                    .into_any_element(),
                Popconfirm::new("no-icon")
                    .scope(self.scope.clone())
                    .title("Delete the task")
                    .show_icon(false)
                    .child(self.button("popconfirm.icon.none", "No icon"))
                    .into_any_element(),
            ]),
            (Kind::Popconfirm, "async") => Popconfirm::new("async")
                .scope(self.scope.clone())
                .title("Title")
                .description("Open Popconfirm with async logic")
                .open(self.async_open)
                .ok_button(PopconfirmButtonOptions {
                    loading: self.async_loading,
                    ..Default::default()
                })
                .on_open_change(cx.listener(
                    |this, change: &ant_design_gpui::popup::PopupOpenChange, _, cx| {
                        this.async_open = change.open;
                        if !change.open {
                            this.async_generation += 1;
                            this.async_loading = false;
                        }
                        cx.notify();
                    },
                ))
                .on_confirm(Self::confirmation(cx, |this, _, window, cx| {
                    this.async_loading = true;
                    this.async_generation += 1;
                    let generation = this.async_generation;
                    cx.spawn_in(window, async move |this, cx| {
                        cx.background_executor().timer(Duration::from_secs(2)).await;
                        let _ = this.update(cx, |this, cx| {
                            if this.async_generation == generation {
                                this.async_open = false;
                                this.async_loading = false;
                                this.status = "Operation completed".into();
                                cx.notify();
                            }
                        });
                    })
                    .detach();
                    cx.notify();
                    Confirmation::KeepOpen
                }))
                .child(
                    self.button("popconfirm.async.trigger", "Open async confirmation")
                        .button_type(ButtonType::Primary),
                )
                .into_any_element(),
            (Kind::Popconfirm, "promise") => Self::column([
                Popconfirm::new("task")
                    .scope(self.scope.clone())
                    .title("Title")
                    .description("Wait for the operation to complete")
                    .on_confirm(Self::confirmation(cx, |this, _, _, cx| {
                        let reject = this.reject_next;
                        this.reject_next = false;
                        this.status = "Working…".into();
                        cx.notify();
                        Confirmation::Pending(cx.spawn(async move |this, cx| {
                            cx.background_executor().timer(Duration::from_secs(3)).await;
                            if reject {
                                return Err("The operation failed. Please try again.".into());
                            }
                            let _ = this.update(cx, |this, cx| {
                                this.status = "Operation completed".into();
                                cx.notify();
                            });
                            Ok(())
                        }))
                    }))
                    .on_confirm_error(cx.listener(|this, error: &SharedString, _, cx| {
                        this.status = error.clone();
                        cx.notify();
                    }))
                    .child(
                        self.button("popconfirm.promise.trigger", "Open task confirmation")
                            .button_type(ButtonType::Primary),
                    )
                    .into_any_element(),
                Checkbox::new("reject")
                    .scope(self.scope.clone())
                    .label("Fail the next attempt")
                    .checked(self.reject_next)
                    .on_change(cx.listener(
                        |this, event: &ant_design_gpui::checkbox::CheckboxChange, _, cx| {
                            this.reject_next = event.checked;
                            cx.notify();
                        },
                    ))
                    .into_any_element(),
            ]),
            (Kind::Popconfirm, "style-class") => Popconfirm::new("styled")
                .scope(self.scope.clone())
                .title("Custom title")
                .description("Custom description")
                .surface_style(
                    StyleRefinement::default()
                        .border_1()
                        .border_color(tokens.color_primary),
                )
                .title_style(StyleRefinement::default().text_color(tokens.color_primary))
                .description_style(StyleRefinement::default().italic())
                .buttons_style(StyleRefinement::default().pt(px(8.)))
                .child(self.button("popconfirm.style.trigger", "Custom styles"))
                .into_any_element(),
            _ => unreachable!("every public popup demo is implemented"),
        }
    }
}

impl PopupPage {
    fn native_preview(&mut self, cx: &mut Context<Self>) -> AnyElement {
        match self.kind {
            Kind::Tooltip => div().id("native-tooltip-scroll").w_full().h(px(144.))
                .overflow_y_scroll().track_scroll(&self.native_scroll)
                .child(div().flex().flex_col().items_start().gap(px(24.)).pt(px(24.)).pb(px(160.))
                    .child(Tooltip::new("native-scroll-tip").scope(self.scope.clone())
                        .title("This tooltip follows its trigger while you scroll and can extend beyond the scroll area.")
                        .child(self.button("tooltip.native.scroll", "Scroll with this tooltip")))
                    .child(Tooltip::new("native-disabled-tip").scope(self.scope.clone())
                        .title("Choose an item before using this action.")
                        .child(self.button("tooltip.native.disabled", "Why unavailable?").disabled(true))))
                .into_any_element(),
            Kind::Popover => {
                let scope = self.scope.clone();
                let focus = self.native_focus.clone();
                Popover::new("native-nested-card").scope(scope.clone())
                    .title("Nested actions").triggers(PopupTriggers::CLICK).initial_focus(&focus)
                    .content(move |handle, _, _| {
                        let handle = handle.clone();
                        div().flex().flex_col().items_start().gap(px(12.))
                            .child("Escape closes the innermost popup first.")
                            .child(Popconfirm::new("native-nested-confirm").scope(scope.clone())
                                .title("Delete this item?").description("The parent card stays open after this confirmation closes.")
                                .placement(PopupPlacement::Right)
                                .child(Button::new("native-nested-delete").scope(scope.clone())
                                    .automation_id("popover.native.delete").focus_handle(&focus).label("Delete item").danger(true)))
                            .child(Button::new("native-parent-close").scope(scope.clone()).label("Close card")
                                .on_click(move |_, window, cx| handle.close(window, cx)))
                    })
                    .child(self.button("popover.native.nested", "Open nested actions"))
                    .into_any_element()
            },
            Kind::Popconfirm => Popconfirm::new("native-keyboard-confirm").scope(self.scope.clone())
                .title("Delete the selected item?").description("Cancel receives focus first. Escape returns focus to the trigger.")
                .on_confirm(Self::confirmation(cx, |this, _, _, cx| {
                    this.status = "Confirmed with native focus handling".into(); cx.notify(); Confirmation::Close
                }))
                .on_cancel(cx.listener(|this, _: &Activation, _, cx| {
                    this.status = "Canceled with native focus handling".into(); cx.notify();
                }))
                .child(self.button("popconfirm.native.keyboard", "Try with the keyboard").danger(true))
                .into_any_element(),
        }
    }
}

impl Render for PopupPage {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let width = window.viewport_size().width;
        let content_width = width - sidebar_width(width) - page_padding(width) * 2.;
        let kind = self.kind;
        div()
            .w_full()
            .flex()
            .flex_col()
            .gap(px(16.))
            .when(kind == Kind::Popconfirm, |page| {
                page.child(
                    div()
                        .id("status")
                        .role(Role::Status)
                        .aria_label("Confirmation result")
                        .debug_selector(|| "gallery.popconfirm.status".into())
                        .text_color(tokens.color_text_secondary)
                        .child(self.status.clone()),
                )
            })
            .child(
                div()
                    .id("examples")
                    .w_full()
                    .grid()
                    .grid_cols(if content_width >= px(840.) { 2 } else { 1 })
                    .items_start()
                    .gap(px(tokens.margin_lg as f32))
                    .when(config.direction == Direction::Rtl, |page| page.text_right())
                    .children(kind.demos().iter().enumerate().map(|(index, demo)| {
                        div()
                            .id(demo.id)
                            .anchor_scroll(Some(self.anchors[index].clone()))
                            .role(Role::Group)
                            .aria_label(demo.title)
                            .debug_selector(move || {
                                format!("gallery.{}.card.{}", kind.name(), demo.id)
                            })
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
                                    .min_h(px(132.))
                                    .p(px(tokens.padding_lg as f32))
                                    .flex()
                                    .items_center()
                                    .child(self.preview(demo, cx)),
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
                    })),
            )
            .child(div().id("native-interactions").role(Role::Group).aria_label("Native interactions")
                .border_1().border_color(tokens.color_border_secondary).rounded(px(tokens.border_radius_lg as f32))
                .bg(tokens.color_bg_container).p(px(tokens.padding_lg as f32)).flex().flex_col().gap(px(16.))
                .child(div().font_weight(FontWeight::SEMIBOLD).child("Native interactions"))
                .child(self.native_preview(cx))
                .child(div().text_color(tokens.color_text_secondary).child(match kind {
                    Kind::Tooltip => "Hover an action, then scroll. Disabled controls still provide explanations.",
                    Kind::Popover => "Open the card and its nested confirmation. Escape dismisses one level at a time; closing the parent removes both.",
                    Kind::Popconfirm => "Use Tab to reach the trigger, then Enter or Space. Confirm and Cancel restore focus without adding a wrapper tab stop.",
                })))
    }
}

fn open_edge_preview(kind: Kind, scope: ConfigScope, cx: &mut App) {
    cx.open_window(
        WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::centered(
                None,
                size(px(400.), px(320.)),
                cx,
            ))),
            ..Default::default()
        },
        |window, cx| {
            window.set_window_title("Popup edge preview");
            let view = cx.new(|_| EdgePreview {
                kind,
                scope: scope.clone(),
                corner: 0,
            });
            cx.new(|cx| UiRoot::new(scope, view, window, cx))
        },
    )
    .expect("open popup edge preview");
}

struct EdgePreview {
    kind: Kind,
    scope: ConfigScope,
    corner: usize,
}
impl Render for EdgePreview {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let tokens = &self.scope.snapshot(cx).theme.tokens;
        div()
            .size_full()
            .bg(tokens.color_bg_container)
            .child(
                div()
                    .absolute()
                    .inset_0()
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(
                        div()
                            .w(px(210.))
                            .flex()
                            .flex_wrap()
                            .justify_center()
                            .gap(px(4.))
                            .children(
                                ["Top left", "Top right", "Bottom left", "Bottom right"]
                                    .into_iter()
                                    .enumerate()
                                    .map(|(i, label)| {
                                        Button::new(("corner", i))
                                            .scope(self.scope.clone())
                                            .label(label)
                                            .on_click(cx.listener(move |this, _, _, cx| {
                                                this.corner = i;
                                                cx.notify();
                                            }))
                                    }),
                            ),
                    ),
            )
            .child(
                div()
                    .absolute()
                    .when(self.corner.is_multiple_of(2), |el| el.left(px(0.)))
                    .when(!self.corner.is_multiple_of(2), |el| el.right(px(0.)))
                    .when(self.corner < 2, |el| el.top(px(0.)))
                    .when(self.corner >= 2, |el| el.bottom(px(0.)))
                    .child(
                        self.kind.sample(
                            "edge",
                            &self.scope,
                            Button::new("trigger")
                                .scope(self.scope.clone())
                                .label("Trigger"),
                            if self.corner < 2 {
                                PopupPlacement::Top
                            } else {
                                PopupPlacement::Bottom
                            },
                            PopupArrow::default(),
                            true,
                        ),
                    ),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui::{
        Entity, KeyDownEvent, KeyUpEvent, Keystroke, Modifiers, PlatformInput, ScrollHandle,
        TestAppContext, VisualTestContext,
    };

    struct Preview {
        page: Entity<PopupPage>,
        demo: &'static Demo,
    }
    impl Render for Preview {
        fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
            let content = self.page.update(cx, |page, cx| page.preview(self.demo, cx));
            div().size_full().child(
                div()
                    .id("preview")
                    .debug_selector(|| "preview".into())
                    .absolute()
                    .left(px(100.))
                    .top(px(160.))
                    .child(content),
            )
        }
    }
    struct Fixture {
        cx: VisualTestContext,
        page: Entity<PopupPage>,
    }
    impl Fixture {
        fn new(kind: Kind, id: &'static str) -> Self {
            let mut cx = TestAppContext::single();
            let mut page = None;
            let window = cx.add_window(|window, cx| {
                let scope = ConfigScope::new_window(cx);
                scope
                    .update_patch(cx, |patch| patch.motion = Some(false))
                    .unwrap();
                let anchors = kind
                    .demos()
                    .iter()
                    .map(|_| ScrollAnchor::for_handle(ScrollHandle::new()))
                    .collect();
                let content = cx.new(|cx| PopupPage::new(kind, scope.clone(), anchors, cx));
                page = Some(content.clone());
                let demo = kind.demos().iter().find(|demo| demo.id == id).unwrap();
                let preview = cx.new(|_| Preview {
                    page: content,
                    demo,
                });
                window.activate_window();
                UiRoot::new(scope, preview, window, cx)
            });
            let mut this = Self {
                cx: VisualTestContext::from_window(window.into(), &cx),
                page: page.unwrap(),
            };
            this.cx.simulate_resize(size(px(1000.), px(650.)));
            this.draw();
            this
        }
        fn draw(&mut self) {
            self.cx.run_until_parked();
            // Preview intentionally renders through its retained page so page
            // notifications are reflected without a cached parent test shortcut.
            self.cx.update(|window, cx| {
                window.refresh();
                window.draw(cx).clear(cx);
            });
            self.cx.run_until_parked();
        }
        fn click_trigger(&mut self) {
            let bounds = self.cx.debug_bounds("preview").unwrap();
            self.cx.simulate_click(
                gpui::point(bounds.left() + px(20.), bounds.top() + px(16.)),
                Modifiers::none(),
            );
            self.draw();
        }
        fn key(&mut self, key: &'static str) {
            let keystroke = Keystroke::parse(key).unwrap();
            self.cx.update(|window, cx| {
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: keystroke.clone(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
                window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke }), cx);
            });
            self.draw();
        }
        fn edit(&mut self, edit: impl FnOnce(&mut PopupPage)) {
            self.page.update(&mut self.cx, |page, cx| {
                edit(page);
                cx.notify();
            });
            self.draw();
        }
        fn read<R>(&self, read: impl FnOnce(&PopupPage) -> R) -> R {
            self.page.read_with(&self.cx, |page, _| read(page))
        }
        fn advance(&mut self, seconds: u64) {
            self.cx
                .executor()
                .advance_clock(Duration::from_secs(seconds));
            self.draw();
        }
        fn keyboard_confirm(&mut self) {
            self.click_trigger();
            self.key("escape");
            self.key("enter");
            self.key("tab");
            self.key("enter");
        }
    }

    #[test]
    fn all_public_popup_examples_render_and_match_reference_order() {
        for (kind, expected) in [
            (
                Kind::Tooltip,
                vec![
                    "basic",
                    "smooth-transition",
                    "placement",
                    "arrow",
                    "shift",
                    "force-align",
                    "colorful",
                    "disabled",
                    "wrap-custom-component",
                    "style-class",
                ],
            ),
            (
                Kind::Popover,
                vec![
                    "basic",
                    "triggerType",
                    "placement",
                    "arrow",
                    "shift",
                    "control",
                    "hover-with-click",
                    "style-class",
                ],
            ),
            (
                Kind::Popconfirm,
                vec![
                    "basic",
                    "locale",
                    "placement",
                    "shift",
                    "dynamic-trigger",
                    "icon",
                    "async",
                    "promise",
                    "style-class",
                ],
            ),
        ] {
            assert_eq!(
                kind.demos().iter().map(|demo| demo.id).collect::<Vec<_>>(),
                expected
            );
            for demo in kind.demos() {
                let mut fixture = Fixture::new(kind, demo.id);
                let bounds = fixture.cx.debug_bounds("preview").unwrap();
                assert!(
                    bounds.size.width > px(0.) && bounds.size.height > px(0.),
                    "{} {}",
                    kind.name(),
                    demo.id
                );
            }
        }
    }
    #[test]
    fn controlled_popover_accepts_open_and_escape_requests() {
        let mut f = Fixture::new(Kind::Popover, "control");
        f.click_trigger();
        assert!(f.read(|page| page.controlled));
        f.key("escape");
        assert!(!f.read(|page| page.controlled));
    }
    #[test]
    fn conditional_confirmation_can_skip_then_require_confirmation() {
        let mut f = Fixture::new(Kind::Popconfirm, "dynamic-trigger");
        f.click_trigger();
        assert!(!f.read(|page| page.conditional_open));
        assert_eq!(f.read(|page| page.status.clone()), "Next step.");
        f.edit(|page| page.direct = false);
        f.click_trigger();
        assert!(f.read(|page| page.conditional_open));
        f.key("escape");
        assert!(!f.read(|page| page.conditional_open));
    }
    #[test]
    fn controlled_async_confirmation_finishes_and_cancels_stale_completion() {
        let mut f = Fixture::new(Kind::Popconfirm, "async");
        f.keyboard_confirm();
        assert!(f.read(|page| page.async_loading));
        f.advance(2);
        assert!(!f.read(|page| page.async_open || page.async_loading));
        assert_eq!(f.read(|page| page.status.clone()), "Operation completed");
        f.keyboard_confirm();
        assert!(f.read(|page| page.async_loading));
        f.key("escape");
        f.click_trigger();
        f.advance(2);
        assert!(f.read(|page| page.async_open));
        assert!(!f.read(|page| page.async_loading));
    }
    #[test]
    fn task_confirmation_exposes_failure_and_supports_retry() {
        let mut f = Fixture::new(Kind::Popconfirm, "promise");
        f.edit(|page| page.reject_next = true);
        f.keyboard_confirm();
        assert_eq!(f.read(|page| page.status.clone()), "Working…");
        f.advance(3);
        assert_eq!(
            f.read(|page| page.status.clone()),
            "The operation failed. Please try again."
        );
        // Confirmation retains focus after rejection; retry in the same dialog.
        f.key("enter");
        f.advance(3);
        assert_eq!(f.read(|page| page.status.clone()), "Operation completed");
    }
    #[test]
    fn forced_alignment_follows_delayed_content_without_polling() {
        let mut f = Fixture::new(Kind::Tooltip, "force-align");
        let handle = f.read(|page| page.align.clone());
        let before = f.cx.update(|_, cx| handle.layout(cx).unwrap());
        f.click_trigger();
        f.advance(2);
        let after = f.cx.update(|_, cx| handle.layout(cx).unwrap());
        assert!(f.read(|page| page.loaded));
        assert_eq!(after.bounds.top() - before.bounds.top(), px(80.));
    }
}
