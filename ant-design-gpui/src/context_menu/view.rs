// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    ContextMenuState, ContextMenuTheme, MenuCheck, MenuEntry, MenuEvent,
    state::{self, EventHandler},
    style::metric,
};
use crate::{
    ConfigScope, Direction,
    behavior::Accessibility,
    icons::{IconName, IconRenderOptions, icon},
    popup::{Popup, PopupOpenReason, PopupPlacement, PopupTriggers, PopupWidth},
};
use gpui::{
    AnyElement, App, ElementId, Entity, IntoElement, MouseButton, Pixels, Refineable, RenderOnce,
    Role, SharedString, StyleRefinement, Subscription, Toggled, WeakEntity, Window, accesskit,
    canvas, div, prelude::*, px, size,
};
use std::rc::Rc;

/// Compose a fresh trigger each render while retaining the menu controller.
/// Right click, Menu and Shift+F10 open the same native popup branch.
#[derive(IntoElement)]
pub struct ContextMenu {
    id: ElementId,
    state: Entity<ContextMenuState>,
    scope: Option<ConfigScope>,
    child: AnyElement,
    disabled: Option<bool>,
    label: Option<SharedString>,
    automation_id: Option<SharedString>,
    width: Option<Pixels>,
    max_height: Option<Pixels>,
    style: StyleRefinement,
    on_event: Option<EventHandler>,
}
impl ContextMenu {
    pub fn new(id: impl Into<ElementId>, state: &Entity<ContextMenuState>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            child: div().into_any_element(),
            disabled: None,
            label: None,
            automation_id: None,
            width: None,
            max_height: None,
            style: StyleRefinement::default(),
            on_event: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn child(mut self, child: impl IntoElement) -> Self {
        self.child = child.into_any_element();
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.label = Some(label.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.automation_id = Some(id.into());
        self
    }
    pub fn width(mut self, width: Pixels) -> Self {
        self.width = Some(width);
        self
    }
    pub fn max_height(mut self, height: Pixels) -> Self {
        self.max_height = Some(height);
        self
    }
    pub fn surface_style(mut self, style: StyleRefinement) -> Self {
        self.style = style;
        self
    }
    pub fn on_event(
        mut self,
        callback: impl Fn(&MenuEvent, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_event = Some(Rc::new(callback));
        self
    }
}
struct Mount {
    state: WeakEntity<ContextMenuState>,
    _observer: Subscription,
    _release: Subscription,
}
impl RenderOnce for ContextMenu {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let disabled = self.disabled.unwrap_or(config.disabled);
        let state = self.state;
        let instance_id: ElementId = (
            self.id,
            SharedString::from(format!("controller-{}", state.entity_id())),
        )
            .into();
        let observed = state.clone();
        let _mount = window.use_keyed_state(instance_id.clone(), cx, move |_, cx| {
            observed.update(cx, |s, _| s.mounted = true);
            Mount {
                state: observed.downgrade(),
                _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                _release: cx.on_release(|mount: &mut Mount, cx| {
                    let _ = mount.state.update(cx, |s, _| s.unmount());
                }),
            }
        });
        state.update(cx, |s, cx| {
            s.bind_scope(scope.clone(), cx);
            s.disabled = disabled;
            s.on_event = self.on_event;
            s.automation_id = self.automation_id.clone();
        });
        let (popup, trigger) = {
            let s = state.read(cx);
            (s.popup.clone(), s.trigger.clone())
        };
        let focus = state.update(cx, |s, cx| {
            let pending = std::mem::take(&mut s.focus_pending);
            let owned = s
                .focus_repair_from
                .iter()
                .any(|f| f.contains_focused(window, cx));
            s.focus_repair_from.clear();
            (pending && owned && s.open).then(|| s.levels[0].focus.clone())
        });
        if let Some(focus) = focus {
            window.focus(&focus, cx)
        }
        if disabled && state.read(cx).open {
            let popup = popup.clone();
            let disabled_state = state.clone();
            window.defer(cx, move |window, cx| {
                popup.request(false, PopupOpenReason::Cancel, window, cx);
                if disabled_state.read(cx).open {
                    state::opened(
                        &disabled_state,
                        &crate::popup::PopupOpenChange {
                            open: false,
                            reason: PopupOpenReason::Programmatic,
                        },
                        window,
                        cx,
                    );
                }
            });
        }
        let keys = state.clone();
        let open_popup = popup.clone();
        let measured = state.clone();
        let bound = popup.clone();
        let show_state = state.clone();
        let generic_trigger = state.read(cx).trigger_tab_stop;
        let expanded = state.read(cx).open;
        let child = div()
            .id("context-menu-trigger")
            .relative()
            .track_focus(&trigger)
            .child(self.child)
            .child(
                canvas(
                    |_, _, _| (),
                    move |bounds, _, window, cx| {
                        let child_focus = bound.trigger_focus_handle(cx);
                        measured.update(cx, |s, cx| {
                            s.trigger_bounds = bounds;
                            let tab_stop = child_focus.is_none() && !s.disabled;
                            if s.trigger_tab_stop != tab_stop {
                                s.trigger_tab_stop = tab_stop;
                                s.trigger = s.trigger.clone().tab_stop(tab_stop);
                                cx.notify();
                                window.request_animation_frame();
                            }
                        });
                    },
                )
                .absolute()
                .inset_0(),
            )
            .on_key_down(move |event, window, cx| {
                if !keys.read(cx).disabled
                    && !event.is_held
                    && !event.keystroke.modifiers.control
                    && !event.keystroke.modifiers.platform
                    && !event.keystroke.modifiers.alt
                    && (event.keystroke.key == "menu" && !event.keystroke.modifiers.shift
                        || event.keystroke.key == "contextmenu" && !event.keystroke.modifiers.shift
                        || (event.keystroke.key == "f10" && event.keystroke.modifiers.shift))
                {
                    let bounds = keys.read(cx).trigger_bounds;
                    let anchor = gpui::point(bounds.left() + px(4.), bounds.bottom());
                    open_popup.open_at(anchor, window, cx);
                    window.prevent_default();
                    cx.stop_propagation();
                }
            })
            .when(!disabled && generic_trigger, |child| {
                child.on_a11y_action(accesskit::Action::ShowContextMenu, move |_, window, cx| {
                    state::show_context_menu(&show_state, window, cx)
                })
            });
        let changed = state.clone();
        let content = state.clone();
        let label = self.label.unwrap_or_else(|| {
            config
                .locale
                .text(crate::locale::MessageKey::ContextMenu)
                .clone()
        });
        let width = self.width;
        let max_height = self.max_height;
        let child = config
            .focus_ring()
            .with_offset(px(-2.))
            .radius(px(config.theme.tokens.border_radius as f32))
            .apply(child, &trigger);
        let child = super::trigger::semantics(child, label.clone(), disabled, expanded);
        let mut popup = popup_surface(
            Popup::new((instance_id, "popup")),
            &scope,
            width,
            &self.style,
            cx,
        )
        .menu_trigger()
        .handle(&popup)
        .child(child)
        .triggers(PopupTriggers::CONTEXT_MENU)
        .disabled(disabled)
        .destroy_on_hidden(true)
        .fresh(true)
        .on_open_change(move |change, window, cx| state::opened(&changed, change, window, cx))
        .content(move |_, window, cx| {
            menu_level(&content, 0, &label, width, max_height, window, cx)
        });
        if let Some(id) = self.automation_id {
            popup = popup.automation_id(id)
        }
        popup
    }
}

fn popup_surface(
    popup: Popup,
    scope: &ConfigScope,
    width: Option<Pixels>,
    style: &StyleRefinement,
    cx: &App,
) -> Popup {
    let config = scope.snapshot(cx);
    let tokens = config.theme.component::<ContextMenuTheme>();
    let g = &config.theme.tokens;
    let mut surface = div()
        .p(metric(tokens.padding, 4.))
        .max_w(px(440.))
        .overflow_hidden();
    surface.style().refine(style);
    let popup = popup
        .constrain_to_viewport(px(8.))
        .scope(scope.clone())
        .placement(if config.direction == Direction::Rtl {
            PopupPlacement::BottomRight
        } else {
            PopupPlacement::BottomLeft
        })
        .arrow(false)
        .gap(px(4.))
        .accessibility_role(Role::GenericContainer)
        .appearance(
            surface.style().clone(),
            tokens.background.into(),
            metric(tokens.radius, 8.),
            g,
            g.border_radius_outer,
        )
        .z_index(g.z_index_popup_base as usize + 50);
    if let Some(width) = width {
        popup.width(PopupWidth::Fixed(width))
    } else {
        popup
    }
}

fn menu_icon(name: IconName, dimension: Pixels) -> impl IntoElement {
    icon(name)
        .inherit_color()
        .render_options(IconRenderOptions {
            size: size(dimension, dimension),
            ..Default::default()
        })
}

fn menu_level(
    state: &Entity<ContextMenuState>,
    depth: usize,
    label: &SharedString,
    width: Option<Pixels>,
    max_height: Option<Pixels>,
    window: &mut Window,
    cx: &mut App,
) -> AnyElement {
    let Some(scope) = state.read(cx).scope.clone() else {
        return div().into_any_element();
    };
    let config = scope.snapshot(cx);
    let tokens = config.theme.component::<ContextMenuTheme>();
    let g = &config.theme.tokens;
    let (items, active, focus, scroll) = {
        let s = state.read(cx);
        let Some(level) = s.levels.get(depth) else {
            return div().into_any_element();
        };
        (
            level.items.clone(),
            level.active,
            level.focus.clone(),
            level.scroll.clone(),
        )
    };
    let available_width =
        (window.viewport_size().width - px(16.) - 2. * metric(tokens.padding, 4.)).max(px(0.));
    let maximum_height = max_height.unwrap_or(px(384.)).max(px(0.)).min(
        (window.viewport_size().height - px(16.) - 2. * metric(tokens.padding, 4.)).max(px(0.)),
    );
    let cached = state.read(cx).levels[depth]
        .width_cache
        .as_ref()
        .filter(|cache| {
            std::sync::Arc::ptr_eq(&cache.items.0, &items.0)
                && std::sync::Arc::ptr_eq(&cache.tokens, &tokens)
                && cache.font == config.font
                && cache.font_size == g.font_size
                && cache.font_size_sm == g.font_size_sm
        })
        .map(|cache| (cache.checks, cache.icons, cache.width));
    let (checks, icons, measured_width) = if let Some(cached) = cached {
        cached
    } else {
        let checks = items
            .entries()
            .iter()
            .filter_map(MenuEntry::item)
            .any(|item| !matches!(item.check, MenuCheck::None));
        let icons = items
            .entries()
            .iter()
            .filter_map(MenuEntry::item)
            .any(|item| item.icon.is_some());
        let dimension = metric(tokens.icon_size, 14.);
        // GPUI's text system caches shaping. Measuring actual text gives each branch
        // its own width and keeps keyboard hints visible without a second layout pass.
        let measure = |text: &SharedString, font_size: f64| {
            let text: SharedString = text.replace(['\r', '\n'], " ").into();
            let run = gpui::TextRun {
                len: text.len(),
                font: config.font.clone(),
                color: tokens.text.into(),
                background_color: None,
                underline: None,
                strikethrough: None,
            };
            window
                .text_system()
                .shape_line(text, px(font_size as f32), &[run], None)
                .width
        };
        let measured_width = items
            .entries()
            .iter()
            .filter_map(MenuEntry::item)
            .map(|item| {
                let mut result =
                    measure(&item.label, g.font_size) + 2. * metric(tokens.padding_inline, 12.);
                for present in [checks, icons, item.has_submenu()] {
                    if present {
                        result += dimension + metric(tokens.gap, 8.);
                    }
                }
                if let Some(badge) = &item.badge {
                    result += measure(badge, g.font_size_sm).min(px(96.))
                        + px(10.)
                        + metric(tokens.gap, 8.);
                }
                if let Some(shortcut) = &item.shortcut {
                    result +=
                        measure(shortcut, g.font_size_sm).min(px(120.)) + metric(tokens.gap, 8.);
                }
                result
            })
            .fold(metric(tokens.minimum_width, 160.), Pixels::max);

        state.update(cx, |s, _| {
            let level = &mut s.levels[depth];
            level.width_cache = Some(MetricsCache {
                items: items.clone(),
                tokens: tokens.clone(),
                font: config.font.clone(),
                font_size: g.font_size,
                font_size_sm: g.font_size_sm,
                checks,
                icons,
                width: measured_width,
            });
            if let Some(list) = &level.native_list {
                list.remeasure();
            }
        });
        (checks, icons, measured_width)
    };
    let key_state = state.clone();
    let hover_state = state.clone();
    let scroll_state = state.clone();
    let next_state = state.clone();
    let previous_state = state.clone();
    let mut menu = div()
        .id(("menu-level", depth))
        .flex()
        .flex_col()
        .w(measured_width)
        .min_w(metric(tokens.minimum_width, 160.).min(available_width))
        .max_w(available_width.min(px(432.)))
        .max_h(maximum_height)
        .overflow_y_scroll()
        .track_scroll(&scroll)
        .track_focus(&focus)
        .font(config.font.clone())
        .text_size(px(g.font_size as f32))
        .line_height(gpui::relative(g.line_height as f32))
        .text_color(tokens.text)
        .role(Role::Menu)
        .tab_index(-1)
        .on_key_down(move |event, window, cx| state::key(&key_state, depth, event, window, cx))
        .on_action(move |_: &crate::behavior::FocusNext, window, cx| {
            state::traverse(&next_state, false, window, cx)
        })
        .on_action(move |_: &crate::behavior::FocusPrevious, window, cx| {
            state::traverse(&previous_state, true, window, cx)
        })
        .on_scroll(move |event, window, cx| {
            if scroll_state
                .read(cx)
                .levels
                .get(depth)
                .is_some_and(|level| level.scroll.bounds().contains(&event.position))
            {
                retire_scrolled_anchor(&scroll_state, depth, window, cx);
            }
        })
        .on_hover(move |hovered, window, cx| {
            if *hovered && depth > 0 {
                let focus = hover_state
                    .read(cx)
                    .levels
                    .get(depth)
                    .map(|l| l.focus.clone());
                if let Some(focus) = focus {
                    window.focus(&focus, cx)
                }
            }
        });
    if let Some(width) = width {
        menu = menu.w((width - 2. * metric(tokens.padding, 4.)).max(px(0.)))
    }
    let rows = Rc::new(MenuRows {
        state: state.clone(),
        depth,
        items: items.clone(),
        scope: scope.clone(),
        config: config.clone(),
        tokens: tokens.clone(),
        active,
        checks,
        icons,
        max_height,
    });
    let weak_list_owner = state.downgrade();
    let list_state = state.update(cx, |s, _| {
        let level = &mut s.levels[depth];
        if items.len() > 64 {
            let list = level.native_list.get_or_insert_with(|| {
                let list = gpui::ListState::new(0, gpui::ListAlignment::Top, px(64.));
                list.reset_with_uniform_height(items.len(), metric(tokens.item_height, 32.));
                list.set_scroll_handler(move |_, window, cx| {
                    if let Some(state) = weak_list_owner.upgrade() {
                        retire_scrolled_anchor(&state, depth, window, cx);
                    }
                });
                list
            });
            Some(list.clone())
        } else {
            None
        }
    });
    if let Some(list_state) = list_state {
        let rows = rows.clone();
        menu = menu.child(
            gpui::list(list_state, move |index, window, cx| {
                rows.row(index, window, cx)
            })
            .with_sizing_behavior(gpui::ListSizingBehavior::Infer)
            .w_full()
            .max_h(maximum_height),
        );
    } else {
        menu = menu.children((0..items.len()).map(|index| rows.row(index, window, cx)));
    }
    menu_semantics(state, depth, label, cx)
        .apply(menu)
        .into_any_element()
}

pub(super) fn menu_semantics(
    state: &Entity<ContextMenuState>,
    depth: usize,
    label: &SharedString,
    cx: &App,
) -> Accessibility {
    let mut semantics = Accessibility::new(Role::Menu).label(label.clone());
    semantics.automation_id = state.read(cx).automation_id.as_ref().map(|id| {
        if depth == 0 {
            id.clone()
        } else {
            format!("{id}.submenu.{depth}").into()
        }
    });
    semantics
}

fn retire_scrolled_anchor(
    state: &Entity<ContextMenuState>,
    depth: usize,
    window: &mut Window,
    cx: &mut App,
) {
    let Some((index, branch_focus)) = state
        .read(cx)
        .levels
        .get(depth + 1)
        .and_then(|level| level.parent_index.map(|index| (index, level.focus.clone())))
    else {
        return;
    };
    let weak = state.downgrade();
    // GPUI delivers the list's scroll callback while mutably borrowing its list
    // state, and its visible_range describes the preceding layout. Inspect the
    // committed logical offset after that listener releases its borrow.
    window.defer(cx, move |window, cx| {
        let Some(state) = weak.upgrade() else { return };
        let hidden = {
            let s = state.read(cx);
            if !s.open
                || !s.levels.get(depth + 1).is_some_and(|level| {
                    level.parent_index == Some(index) && level.focus == branch_focus
                })
            {
                return;
            }
            let level = &s.levels[depth];
            let viewport = level.scroll.bounds();
            let bounds = if let Some(list) = &level.native_list {
                list.bounds_for_item(index)
            } else {
                level.scroll.bounds_for_item(index).map(|mut bounds| {
                    bounds.origin += level.scroll.offset();
                    bounds
                })
            };
            bounds.is_none_or(|bounds| bounds.intersect(&viewport).is_empty())
        };
        if hidden {
            state::close_branch(&state, depth + 1, window, cx);
        }
    });
}

pub(super) struct MetricsCache {
    items: super::MenuItems,
    tokens: std::sync::Arc<super::ContextMenuTokens>,
    font: gpui::Font,
    font_size: f64,
    font_size_sm: f64,
    checks: bool,
    icons: bool,
    width: Pixels,
}

struct MenuRows {
    state: Entity<ContextMenuState>,
    depth: usize,
    items: super::MenuItems,
    scope: ConfigScope,
    config: std::sync::Arc<crate::ResolvedConfig>,
    tokens: std::sync::Arc<super::ContextMenuTokens>,
    active: Option<usize>,
    checks: bool,
    icons: bool,
    max_height: Option<Pixels>,
}
fn row_current(
    state: &Entity<ContextMenuState>,
    depth: usize,
    index: usize,
    id: &ElementId,
    cx: &App,
) -> bool {
    let state = state.read(cx);
    state.open
        && !state.disabled
        && state
            .levels
            .get(depth)
            .and_then(|level| level.items.item(index))
            .is_some_and(|item| item.id == *id && !item.disabled)
}
impl MenuRows {
    fn row(&self, index: usize, _window: &mut Window, cx: &mut App) -> AnyElement {
        let Self {
            state,
            depth,
            scope,
            config,
            tokens,
            active,
            checks,
            icons,
            max_height,
            ..
        } = self;
        let depth = *depth;
        let active = *active;
        let checks = *checks;
        let icons = *icons;
        let max_height = *max_height;
        let g = &config.theme.tokens;
        let rtl = config.direction == Direction::Rtl;
        let dimension = metric(tokens.icon_size, 14.);
        match &self.items.entries()[index] {
            MenuEntry::Separator => div()
                .id(("separator", index))
                .h(px(1.))
                .my(px(4.))
                .bg(tokens.divider)
                .role(Role::Splitter)
                .into_any_element(),
            MenuEntry::Group(label) => div()
                .id(("group", index))
                .px(metric(tokens.padding_inline, 12.))
                .py(px(6.))
                .text_size(px(g.font_size_sm as f32))
                .text_color(tokens.secondary_text)
                .child(label.clone())
                .into_any_element(),
            MenuEntry::Item(item) => {
                let (position, total) = self.items.item_position(index);
                let expanded = state
                    .read(cx)
                    .levels
                    .get(depth + 1)
                    .is_some_and(|l| l.parent_index == Some(index));
                let selected = active == Some(index);
                let foreground = if item.disabled {
                    tokens.disabled_text
                } else if item.danger {
                    tokens.danger_text
                } else {
                    tokens.text
                };
                let target = state.clone();
                let hover = state.clone();
                let a11y = state.clone();
                let focus_state = state.clone();
                let click_id = item.id.clone();
                let hover_id = item.id.clone();
                let a11y_id = item.id.clone();
                let focus_id = item.id.clone();
                let mut row = div()
                    .id(item.id.clone())
                    .aria_position_in_set(position)
                    .aria_size_of_set(total)
                    .flex()
                    .items_center()
                    .gap(metric(tokens.gap, 8.))
                    .min_h(metric(tokens.item_height, 32.))
                    .px(metric(tokens.padding_inline, 12.))
                    .py(px(4.))
                    .rounded(metric(tokens.item_radius, 4.))
                    .text_color(foreground)
                    .when(rtl, |row| row.flex_row_reverse())
                    .when(selected && !item.disabled, |row| {
                        row.bg(if item.danger {
                            tokens.danger_hover_background
                        } else {
                            tokens.hover_background
                        })
                    })
                    .when(item.disabled, |row| row.cursor_not_allowed())
                    .when(!item.disabled, |row| row.cursor_pointer())
                    .when(selected, |row| row.aria_active_descendant());
                if checks {
                    row = row.child(
                        div()
                            .flex()
                            .items_center()
                            .justify_center()
                            .w(dimension)
                            .flex_shrink_0()
                            .text_color(if item.disabled {
                                tokens.disabled_text
                            } else {
                                tokens.checkmark
                            })
                            .when(item.check.checked() == Some(true), |slot| {
                                if matches!(item.check, MenuCheck::Radio { .. }) {
                                    slot.child(div().size(px(6.)).rounded_full().bg(
                                        if item.disabled {
                                            tokens.disabled_text
                                        } else {
                                            tokens.checkmark
                                        },
                                    ))
                                } else {
                                    slot.child(menu_icon(IconName::CheckOutlined, dimension))
                                }
                            }),
                    );
                }
                if icons {
                    row = row.child(
                        div()
                            .w(dimension)
                            .flex_shrink_0()
                            .when_some(item.icon, |slot, name| {
                                slot.child(menu_icon(name, dimension))
                            }),
                    );
                }
                row = row.child(
                    div()
                        .flex_1()
                        .min_w_0()
                        .truncate()
                        .when(rtl, |d| d.text_right())
                        .child(item.label.clone()),
                );
                if let Some(badge) = &item.badge {
                    row = row.child(
                        div()
                            .flex_shrink_0()
                            .max_w(px(106.))
                            .truncate()
                            .px(px(5.))
                            .rounded(px(3.))
                            .text_size(px(g.font_size_sm as f32))
                            .text_color(if item.disabled {
                                tokens.disabled_text
                            } else {
                                tokens.secondary_text
                            })
                            .bg(g.color_fill_quaternary)
                            .child(badge.clone()),
                    );
                }
                if let Some(shortcut) = &item.shortcut {
                    row = row.child(
                        div()
                            .flex_shrink_0()
                            .max_w(px(120.))
                            .truncate()
                            .text_size(px(g.font_size_sm as f32))
                            .text_color(if item.disabled {
                                tokens.disabled_text
                            } else {
                                tokens.secondary_text
                            })
                            .child(shortcut.clone()),
                    );
                }
                if item.has_submenu() {
                    row = row
                        .child(div().flex_shrink_0().child(menu_icon(
                            if rtl {
                                IconName::LeftOutlined
                            } else {
                                IconName::RightOutlined
                            },
                            px(10.),
                        )))
                        .aria_expanded(expanded)
                        .a11y_synthetic_children(|builder| {
                            builder
                                .parent_node()
                                .set_has_popup(accesskit::HasPopup::Menu)
                        });
                }
                if let Some(checked) = item.check.checked() {
                    row = row.aria_toggled(if checked {
                        Toggled::True
                    } else {
                        Toggled::False
                    });
                }
                if !item.disabled {
                    row = row
                        .on_hover(move |hovered, window, cx| {
                            if row_current(&hover, depth, index, &hover_id, cx) {
                                if *hovered {
                                    state::hover(&hover, depth, index, window, cx)
                                } else {
                                    state::leave_row(&hover, depth, index, cx)
                                }
                            }
                        })
                        .on_mouse_down(MouseButton::Left, |_, _, cx| cx.stop_propagation())
                        .on_click(move |_, window, cx| {
                            if row_current(&target, depth, index, &click_id, cx) {
                                state::activate(
                                    &target,
                                    depth,
                                    index,
                                    super::MenuActivationSource::Pointer,
                                    window,
                                    cx,
                                )
                            }
                        })
                        .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
                            if row_current(&a11y, depth, index, &a11y_id, cx) {
                                state::activate(
                                    &a11y,
                                    depth,
                                    index,
                                    super::MenuActivationSource::Accessibility,
                                    window,
                                    cx,
                                )
                            }
                        })
                        .on_a11y_action(accesskit::Action::Focus, move |_, window, cx| {
                            if !row_current(&focus_state, depth, index, &focus_id, cx) {
                                return;
                            }
                            focus_state.update(cx, |s, cx| {
                                if let Some(level) = s.levels.get_mut(depth) {
                                    level.active = Some(index);
                                    level.reveal(index);
                                    window.focus(&level.focus, cx);
                                    cx.notify();
                                }
                            });
                        });
                }
                let role = match item.check {
                    MenuCheck::None => Role::MenuItem,
                    MenuCheck::Checkbox(_) => Role::MenuItemCheckBox,
                    MenuCheck::Radio { .. } => Role::MenuItemRadio,
                };
                let mut semantics = Accessibility::new(role).label(item.label.clone());
                semantics.disabled = item.disabled;
                if let Some(shortcut) = &item.shortcut {
                    semantics.description = Some(shortcut.clone());
                }
                let row = semantics.apply(row);
                if item.has_submenu() {
                    let submenu_state = state.clone();
                    let submenu_label = item.label.clone();
                    let closed = state.clone();
                    let popup = popup_surface(
                        Popup::new(item.id.clone()),
                        scope,
                        None,
                        &StyleRefinement::default(),
                        cx,
                    )
                    .placement(if rtl {
                        PopupPlacement::LeftTop
                    } else {
                        PopupPlacement::RightTop
                    })
                    .gap(px(4.))
                    .triggers(PopupTriggers::NONE)
                    .child(row)
                    .open(expanded)
                    .fresh(true)
                    .destroy_on_hidden(true)
                    .on_open_change(move |change, window, cx| {
                        if !change.open
                            && matches!(
                                change.reason,
                                PopupOpenReason::Escape | PopupOpenReason::OutsidePress
                            )
                        {
                            state::close_branch(&closed, depth + 1, window, cx)
                        }
                    })
                    .content(move |_, window, cx| {
                        menu_level(
                            &submenu_state,
                            depth + 1,
                            &submenu_label,
                            None,
                            max_height,
                            window,
                            cx,
                        )
                    });
                    popup.into_any_element()
                } else {
                    row.into_any_element()
                }
            }
        }
    }
}
