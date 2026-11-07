// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! A logical list belongs to the focused editor, even though PopupHost paints the
//! menu in a deferred layer. This is the same bounded active-neighbour strategy
//! as rc-select's virtual list. It preserves native editor/IME focus and gives
//! GPUI a real ancestor chain for active-descendant focus without tracking the
//! editor's FocusHandle on a second element.

use super::{SelectChangeSource, SelectState};
use crate::behavior::Accessibility;
use gpui::{AnyElement, App, Entity, IntoElement, Role, SharedString, accesskit, div, prelude::*};
use std::hash::Hash;

pub(super) fn list<T: Clone + Eq + Hash + 'static>(
    state: &Entity<SelectState<T>>,
    automation_id: Option<&SharedString>,
    virtualized: bool,
    cx: &App,
) -> AnyElement {
    let model = &state.read(cx).model;
    let count = model.row_count();
    let active = model.active;
    let center = active.unwrap_or(0);
    let range = if virtualized {
        center.saturating_sub(1)..(center + 2).min(count)
    } else {
        0..count
    };
    let multiple = model.settings.mode.multiple();
    div()
        .id("accessible-options")
        .absolute()
        .size_0()
        .overflow_hidden()
        .role(Role::ListBox)
        .a11y_synthetic_children(move |builder| {
            if multiple {
                builder.parent_node().set_multiselectable();
            }
        })
        .children(range.filter_map(|index| {
            let option = model.option(index)?;
            let (position, total) = model.option_position(index)?;
            let disabled = !model.enabled(index);
            let stable_id = option.id.clone();
            let target = state.clone();
            let focus_target = state.clone();
            let focus_id = stable_id.clone();
            let mut item = div()
                .id(stable_id.clone())
                .size_0()
                .aria_selected(model.selected.contains(&option.value))
                .aria_position_in_set(position)
                .aria_size_of_set(total)
                .when(active == Some(index), |item| item.aria_active_descendant());
            if !disabled {
                item = item
                    .on_a11y_action(accesskit::Action::Click, move |_, window, cx| {
                        target.update(cx, |state, cx| {
                            if state.model.option(index).is_some_and(|o| o.id == stable_id) {
                                state.choose(index, SelectChangeSource::Accessibility, window, cx);
                            }
                        });
                    })
                    .on_a11y_action(accesskit::Action::Focus, move |_, window, cx| {
                        focus_target.update(cx, |state, cx| {
                            if state.model.option(index).is_some_and(|o| o.id == focus_id) {
                                state.focus(window, cx);
                                state.activate(index, window, cx);
                            }
                        });
                    });
            }
            let mut semantics = Accessibility::new(Role::ListBoxOption).label(option.label.clone());
            semantics.disabled = disabled;
            semantics.automation_id = automation_id.map(|id| format!("{id}.option.{index}").into());
            Some(semantics.apply(item))
        }))
        .into_any_element()
}
