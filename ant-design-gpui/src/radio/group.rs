// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Typed radio options with retained selection and native roving keyboard focus.

use std::{collections::HashMap, rc::Rc};

use gpui::{
    App, Context, ElementId, FocusHandle, IntoElement, Refineable, RenderOnce, Role, SharedString,
    StyleRefinement, Styled, Subscription, Window, div, prelude::*, px,
};

use super::{Radio, RadioChange, RadioTheme, paint::ButtonPaintQueue};
use crate::{ConfigScope, Direction, Size, behavior::Accessibility};

/// The appearance of the options within a group.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum RadioOptionType {
    #[default]
    Default,
    Button,
}

/// The appearance of a selected button-style radio.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum RadioButtonStyle {
    #[default]
    Outline,
    Solid,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum RadioNavigation {
    Previous,
    Next,
    First,
    Last,
}

type ChangeHandler<T> = Rc<dyn Fn(&T, &mut Window, &mut App)>;
type OptionChangeHandler = Rc<dyn Fn(&RadioChange, &mut Window, &mut App)>;
type SelectionChange<T> = (T, Option<OptionChangeHandler>, Option<ChangeHandler<T>>);

/// One option with a stable ID independent of its label, value, and list position.
pub struct RadioOption<T> {
    id: ElementId,
    value: T,
    label: SharedString,
    disabled: Option<bool>,
    style: Option<Box<StyleRefinement>>,
    automation_id: Option<SharedString>,
    focus: Option<FocusHandle>,
    on_change: Option<OptionChangeHandler>,
}

impl<T> RadioOption<T> {
    pub fn new(id: impl Into<ElementId>, value: T, label: impl Into<SharedString>) -> Self {
        Self {
            id: id.into(),
            value,
            label: label.into(),
            disabled: None,
            style: None,
            automation_id: None,
            focus: None,
            on_change: None,
        }
    }

    /// An explicit false overrides the group and configuration provider.
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }

    pub fn style(mut self, style: StyleRefinement) -> Self {
        self.style = Some(Box::new(style));
        self
    }

    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.automation_id = Some(id.into());
        self
    }

    pub fn focus_handle(mut self, focus: &FocusHandle) -> Self {
        self.focus = Some(focus.clone());
        self
    }

    /// Runs before the group's callback when this option becomes selected.
    pub fn on_change(
        mut self,
        handler: impl Fn(&RadioChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Rc::new(handler));
        self
    }
}

/// Single selection with a native radio-group role and one keyboard tab stop.
///
/// `.value(...)` controls selection. Without it, `.default_value(...)` initializes
/// retained state once per mounted ID. Values need only `Clone + PartialEq`; use
/// stable option IDs to preserve focus when options are reordered. Arrow keys
/// wrap, skip disabled options and select the focused option. Left and Right
/// follow the configured direction, while Up and Down follow option order.
#[derive(IntoElement)]
pub struct RadioGroup<T: Clone + PartialEq + 'static> {
    id: ElementId,
    scope: Option<ConfigScope>,
    options: Vec<RadioOption<T>>,
    value: Option<T>,
    default_value: Option<T>,
    disabled: Option<bool>,
    vertical: bool,
    block: bool,
    size: Option<Size>,
    option_type: RadioOptionType,
    button_style: RadioButtonStyle,
    accessibility: Accessibility,
    on_change: Option<ChangeHandler<T>>,
    style: StyleRefinement,
}

impl<T: Clone + PartialEq + 'static> RadioGroup<T> {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            options: Vec::new(),
            value: None,
            default_value: None,
            disabled: None,
            vertical: false,
            block: false,
            size: None,
            option_type: RadioOptionType::Default,
            button_style: RadioButtonStyle::Outline,
            accessibility: Accessibility::new(Role::RadioGroup),
            on_change: None,
            style: StyleRefinement::default(),
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    pub fn options(mut self, options: impl IntoIterator<Item = RadioOption<T>>) -> Self {
        self.options = options.into_iter().collect();
        self
    }

    pub fn value(mut self, value: T) -> Self {
        self.value = Some(value);
        self
    }

    pub fn default_value(mut self, value: T) -> Self {
        self.default_value = Some(value);
        self
    }

    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }

    pub fn vertical(mut self, vertical: bool) -> Self {
        self.vertical = vertical;
        self
    }

    /// Fill the available width and distribute options evenly.
    pub fn block(mut self, block: bool) -> Self {
        self.block = block;
        self
    }

    /// Button-style options inherit the provider's size unless overridden here.
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }

    pub fn option_type(mut self, option_type: RadioOptionType) -> Self {
        self.option_type = option_type;
        self
    }

    pub fn button_style(mut self, button_style: RadioButtonStyle) -> Self {
        self.button_style = button_style;
        self
    }

    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(label.into());
        self
    }

    pub fn accessibility_description(mut self, description: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(description.into());
        self
    }

    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(id.into());
        self
    }

    pub fn on_change(mut self, handler: impl Fn(&T, &mut Window, &mut App) + 'static) -> Self {
        self.on_change = Some(Rc::new(handler));
        self
    }
}

impl<T: Clone + PartialEq + 'static> Styled for RadioGroup<T> {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct GroupSelection<T> {
    uncontrolled: Option<T>,
    controlled: Option<T>,
}

impl<T: Clone + PartialEq> GroupSelection<T> {
    fn selected(&self) -> Option<&T> {
        self.controlled.as_ref().or(self.uncontrolled.as_ref())
    }

    fn select(&mut self, value: &T) -> bool {
        if self.selected() == Some(value) {
            return false;
        }
        if self.controlled.is_none() {
            self.uncontrolled = Some(value.clone());
        }
        true
    }
}

struct MountedOption<T> {
    id: ElementId,
    value: T,
    disabled: Option<bool>,
    focus: FocusHandle,
    on_change: Option<OptionChangeHandler>,
}

struct GroupState<T> {
    scope: ConfigScope,
    selection: GroupSelection<T>,
    options: Vec<MountedOption<T>>,
    on_change: Option<ChangeHandler<T>>,
    button_paint: ButtonPaintQueue,
    _config: Subscription,
}

impl<T: Clone + PartialEq + 'static> GroupState<T> {
    fn new(scope: ConfigScope, initial: Option<T>, cx: &mut Context<Self>) -> Self {
        let subscription = scope.observe(cx);
        Self {
            scope,
            selection: GroupSelection {
                uncontrolled: initial,
                controlled: None,
            },
            options: Vec::new(),
            on_change: None,
            button_paint: ButtonPaintQueue::default(),
            _config: subscription,
        }
    }

    fn enabled(&self, index: usize, provider_disabled: bool) -> bool {
        self.options
            .get(index)
            .is_some_and(|option| !option.disabled.unwrap_or(provider_disabled))
    }

    fn tab_stop(&self, cx: &App) -> Option<usize> {
        let disabled = self.scope.snapshot(cx).disabled;
        self.options
            .iter()
            .enumerate()
            .position(|(index, option)| {
                self.selection.selected() == Some(&option.value) && self.enabled(index, disabled)
            })
            .or_else(|| (0..self.options.len()).find(|index| self.enabled(*index, disabled)))
    }

    fn navigate(&self, from: &ElementId, direction: RadioNavigation, cx: &App) -> Option<usize> {
        let count = self.options.len();
        if count == 0 {
            return None;
        }
        let disabled = self.scope.snapshot(cx).disabled;
        match direction {
            RadioNavigation::First => (0..count).find(|index| self.enabled(*index, disabled)),
            RadioNavigation::Last => (0..count)
                .rev()
                .find(|index| self.enabled(*index, disabled)),
            RadioNavigation::Previous | RadioNavigation::Next => {
                let current = self.options.iter().position(|option| option.id == *from)?;
                (1..=count)
                    .map(|offset| match direction {
                        RadioNavigation::Previous => (current + count - offset) % count,
                        _ => (current + offset) % count,
                    })
                    .find(|index| self.enabled(*index, disabled))
            }
        }
    }

    fn sync_tab_stops(&mut self, cx: &App) -> Option<usize> {
        let tab_stop = self.tab_stop(cx);
        for (index, option) in self.options.iter_mut().enumerate() {
            option.focus = option.focus.clone().tab_stop(tab_stop == Some(index));
        }
        tab_stop
    }

    fn select(&mut self, index: usize, cx: &mut Context<Self>) -> Option<SelectionChange<T>> {
        if !self.enabled(index, self.scope.snapshot(cx).disabled) {
            return None;
        }
        let option = &self.options[index];
        if !self.selection.select(&option.value) {
            return None;
        }
        let result = (
            option.value.clone(),
            option.on_change.clone(),
            self.on_change.clone(),
        );
        // Update retained handles alongside selection. The replacement tree
        // registers their new traversal settings in its next paint.
        self.sync_tab_stops(cx);
        cx.notify();
        Some(result)
    }
}

fn dispatch_change<T>(
    change: Option<SelectionChange<T>>,
    activation: crate::behavior::Activation,
    window: &mut Window,
    cx: &mut App,
) {
    if let Some((value, option_callback, group_callback)) = change {
        if let Some(callback) = option_callback {
            callback(
                &RadioChange {
                    checked: true,
                    activation,
                },
                window,
                cx,
            );
        }
        if let Some(callback) = group_callback {
            callback(&value, window, cx);
        }
    }
}

impl<T: Clone + PartialEq + 'static> RenderOnce for RadioGroup<T> {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |_, cx| {
            GroupState::new(scope.clone(), self.default_value.take(), cx)
        });
        let tab_stop = state.update(cx, |state, cx| {
            if state.scope != scope {
                state._config = scope.observe(cx);
                state.scope = scope.clone();
            }
            state.selection.controlled = self.value.take();
            state.on_change = self.on_change.take();
            let mut old_focus: HashMap<_, _> = state
                .options
                .drain(..)
                .map(|option| (option.id, option.focus))
                .collect();
            state.options = self
                .options
                .iter()
                .map(|option| MountedOption {
                    id: option.id.clone(),
                    value: option.value.clone(),
                    disabled: option.disabled.or(self.disabled),
                    focus: option.focus.clone().unwrap_or_else(|| {
                        old_focus
                            .remove(&option.id)
                            .unwrap_or_else(|| cx.focus_handle().tab_index(0))
                    }),
                    on_change: option.on_change.clone(),
                })
                .collect();
            state.sync_tab_stops(cx)
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<RadioTheme>();
        let global = tokens.globals();
        let count = self.options.len();
        let button_paint = state.read(cx).button_paint.clone();
        button_paint.reset(if self.option_type == RadioOptionType::Button {
            count
        } else {
            0
        });
        let mut root = div()
            .id(self.id)
            .relative()
            .aria_orientation(if self.vertical {
                gpui::accesskit::Orientation::Vertical
            } else {
                gpui::accesskit::Orientation::Horizontal
            })
            .flex()
            .when(self.option_type == RadioOptionType::Default, |root| {
                root.flex_wrap().gap_x(px(
                    super::RadioStyle::from_tokens(&tokens, false, false).group_gap as f32
                ))
            })
            .text_size(px(global.font_size as f32))
            .line_height(gpui::relative(global.line_height as f32))
            .text_color(global.color_text.to_hsla())
            .font(tokens.font().clone())
            .when(self.block, |root| root.w_full())
            .when(self.vertical, |root| {
                root.flex_col().when_else(
                    self.option_type == RadioOptionType::Button,
                    |root| root.items_stretch(),
                    |root| root.items_start().gap_y(px(global.margin_xs as f32)),
                )
            })
            .when(
                !self.vertical && config.direction == Direction::Rtl,
                |root| root.flex_row_reverse(),
            )
            .when(
                self.vertical
                    && self.option_type == RadioOptionType::Default
                    && config.direction == Direction::Rtl,
                |root| root.items_end(),
            );
        root.style().refine(&self.style);
        for (index, option) in self.options.into_iter().enumerate() {
            let selected = state.read(cx).selection.selected() == Some(&option.value);
            let focus = state.read(cx).options[index].focus.clone();
            let option_id = option.id.clone();
            let navigation_state = state.downgrade();
            let selection_state = state.downgrade();
            let mut radio = Radio::new(option.id)
                .scope(scope.clone())
                .label(option.label)
                .checked(selected)
                .focus_handle(&focus)
                .group_tab_stop(tab_stop == Some(index))
                .option_type(self.option_type)
                .button_style(self.button_style)
                .button_position(index, count)
                .group_vertical(self.vertical)
                .group_block(self.block && !self.vertical)
                .on_navigate(move |direction, window, cx| {
                    let result = navigation_state.update(cx, |state, cx| {
                        let target = state.navigate(&option_id, direction, cx)?;
                        state.options[target].focus.focus(window, cx);
                        state.select(target, cx)
                    });
                    if let Ok(change) = result {
                        dispatch_change(
                            change,
                            crate::behavior::Activation::KeyboardNavigation,
                            window,
                            cx,
                        );
                    }
                })
                .on_change(move |change, window, cx| {
                    let result = selection_state.update(cx, |state, cx| {
                        // Read the live owner rather than the child's controlled
                        // snapshot, including repeated inputs before a redraw.
                        state.select(index, cx)
                    });
                    if let Ok(updated) = result {
                        dispatch_change(updated, change.activation, window, cx);
                    }
                });
            if self.option_type == RadioOptionType::Button {
                radio = radio.button_paint(button_paint.clone(), index);
            }
            if let Some(disabled) = option.disabled.or(self.disabled) {
                radio = radio.disabled(disabled);
            }
            if let Some(size) = self.size {
                radio = radio.size(size);
            }
            if let Some(id) = option.automation_id {
                radio = radio.automation_id(id);
            }
            if let Some(style) = &option.style {
                radio.style().refine(style);
            }
            root = root.child(radio);
        }
        if self.option_type == RadioOptionType::Button {
            root = root.child(button_paint.element());
        }
        self.accessibility.apply(root)
    }
}

#[cfg(test)]
mod tests;
