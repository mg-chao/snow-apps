// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Explicit option composition for native checkbox groups.

use std::rc::Rc;

use gpui::{
    App, Context, ElementId, FocusHandle, IntoElement, Refineable, RenderOnce, Role, SharedString,
    StyleRefinement, Styled, Subscription, Window, div, prelude::*, px,
};

use super::{Checkbox, CheckboxChange, CheckboxTheme};
use crate::{ConfigScope, Direction, behavior::Accessibility};

type ChangeHandler<T> = Rc<dyn Fn(&[T], &mut Window, &mut App)>;
type OptionChangeHandler = Box<dyn Fn(&CheckboxChange, &mut Window, &mut App)>;

/// One option with a stable ID independent of its label, value, and list position.
///
/// Values may be any application type implementing `Clone + PartialEq`. IDs must
/// be unique within the group and should remain stable when options are reordered.
pub struct CheckboxOption<T> {
    id: ElementId,
    value: T,
    label: SharedString,
    disabled: Option<bool>,
    style: StyleRefinement,
    automation_id: Option<SharedString>,
    focus: Option<FocusHandle>,
    on_change: Option<OptionChangeHandler>,
}

impl<T> CheckboxOption<T> {
    pub fn new(id: impl Into<ElementId>, value: T, label: impl Into<SharedString>) -> Self {
        Self {
            id: id.into(),
            value,
            label: label.into(),
            disabled: None,
            style: StyleRefinement::default(),
            automation_id: None,
            focus: None,
            on_change: None,
        }
    }

    /// An explicit false overrides both the group and its configuration provider.
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }

    pub fn style(mut self, style: StyleRefinement) -> Self {
        self.style = style;
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

    /// Runs before the group's selection callback, like the standalone checkbox.
    pub fn on_change(
        mut self,
        handler: impl Fn(&CheckboxChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Box::new(handler));
        self
    }
}

/// A wrapping collection of checkboxes sharing a controlled or retained selection.
///
/// `.value(...)` makes the selection controlled; the callback proposes a new
/// selection for the owner to apply. Without it, `.default_value(...)` seeds the
/// selection once per mounted ID. Callbacks contain only present options, in their
/// display order. A selected value temporarily removed from the options is retained
/// internally and becomes selected again if that option is restored.
#[derive(IntoElement)]
pub struct CheckboxGroup<T: Clone + PartialEq + 'static> {
    id: ElementId,
    scope: Option<ConfigScope>,
    options: Vec<CheckboxOption<T>>,
    value: Option<Vec<T>>,
    default_value: Vec<T>,
    disabled: Option<bool>,
    vertical: bool,
    accessibility: Accessibility,
    on_change: Option<ChangeHandler<T>>,
    style: StyleRefinement,
}

impl<T: Clone + PartialEq + 'static> CheckboxGroup<T> {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            options: Vec::new(),
            value: None,
            default_value: Vec::new(),
            disabled: None,
            vertical: false,
            accessibility: Accessibility::new(Role::Group),
            on_change: None,
            style: StyleRefinement::default(),
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    pub fn options(mut self, options: impl IntoIterator<Item = CheckboxOption<T>>) -> Self {
        self.options = options.into_iter().collect();
        self
    }

    pub fn value(mut self, value: impl IntoIterator<Item = T>) -> Self {
        self.value = Some(value.into_iter().collect());
        self
    }

    pub fn default_value(mut self, value: impl IntoIterator<Item = T>) -> Self {
        self.default_value = value.into_iter().collect();
        self
    }

    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }

    /// Stack options vertically. The default is Ant Design's wrapping inline row.
    pub fn vertical(mut self, vertical: bool) -> Self {
        self.vertical = vertical;
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

    pub fn on_change(mut self, handler: impl Fn(&[T], &mut Window, &mut App) + 'static) -> Self {
        self.on_change = Some(Rc::new(handler));
        self
    }
}

impl<T: Clone + PartialEq + 'static> Styled for CheckboxGroup<T> {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct GroupSelection<T> {
    uncontrolled: Vec<T>,
    controlled: Option<Vec<T>>,
}

impl<T: Clone + PartialEq> GroupSelection<T> {
    fn selected(&self) -> &[T] {
        self.controlled.as_deref().unwrap_or(&self.uncontrolled)
    }

    fn change(&mut self, value: &T, checked: bool, order: &[T]) -> Vec<T> {
        let mut next = self.selected().to_vec();
        if checked {
            if !next.contains(value) {
                next.push(value.clone());
            }
        } else {
            next.retain(|selected| selected != value);
        }
        let mut emitted = Vec::with_capacity(next.len().min(order.len()));
        for option in order {
            if next.contains(option) && !emitted.contains(option) {
                emitted.push(option.clone());
            }
        }
        if self.controlled.is_none() {
            self.uncontrolled = next;
        }
        emitted
    }
}

struct GroupState<T> {
    scope: ConfigScope,
    selection: GroupSelection<T>,
    _config: Subscription,
}

impl<T: Clone + PartialEq + 'static> GroupState<T> {
    fn new(scope: ConfigScope, initial: Vec<T>, cx: &mut Context<Self>) -> Self {
        let subscription = scope.observe(cx);
        Self {
            scope,
            selection: GroupSelection {
                uncontrolled: initial,
                controlled: None,
            },
            _config: subscription,
        }
    }
}

impl<T: Clone + PartialEq + 'static> RenderOnce for CheckboxGroup<T> {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |_, cx| {
            GroupState::new(scope.clone(), std::mem::take(&mut self.default_value), cx)
        });
        state.update(cx, |state, cx| {
            if state.scope != scope {
                state._config = scope.observe(cx);
                state.scope = scope.clone();
            }
            state.selection.controlled = self.value.take();
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<CheckboxTheme>();
        let global = tokens.globals();
        let order: Rc<[T]> = self
            .options
            .iter()
            .map(|option| option.value.clone())
            .collect();
        let mut root = div()
            .id(self.id)
            .flex()
            .flex_wrap()
            .gap_x(px(global.margin_xs as f32))
            .text_size(px(global.font_size as f32))
            .line_height(gpui::relative(global.line_height as f32))
            .text_color(global.color_text.to_hsla())
            .font(tokens.font().clone())
            .when(self.vertical, |root| root.flex_col().items_start())
            .when(
                !self.vertical && config.direction == Direction::Rtl,
                |root| root.flex_row_reverse(),
            )
            .when(
                self.vertical && config.direction == Direction::Rtl,
                |root| root.items_end(),
            );
        root.style().refine(&self.style);
        for option in self.options {
            let checked = state.read(cx).selection.selected().contains(&option.value);
            let disabled = option.disabled.or(self.disabled);
            let state = state.downgrade();
            let callback = self.on_change.clone();
            let order = order.clone();
            let mut checkbox = Checkbox::new(option.id)
                .scope(scope.clone())
                .label(option.label)
                .checked(checked);
            if let Some(disabled) = disabled {
                checkbox = checkbox.disabled(disabled);
            }
            if let Some(id) = option.automation_id {
                checkbox = checkbox.automation_id(id);
            }
            if let Some(focus) = option.focus {
                checkbox = checkbox.focus_handle(&focus);
            }
            checkbox.style().refine(&option.style);
            root = root.child(checkbox.on_change(move |change, window, cx| {
                let updated = state.update(cx, |state, cx| {
                    // Children receive controlled snapshots. Derive the proposal
                    // from the live owner so rapid activations before rendering
                    // toggle an uncontrolled group correctly.
                    let change = CheckboxChange {
                        checked: !state.selection.selected().contains(&option.value),
                        activation: change.activation,
                    };
                    let selected = state
                        .selection
                        .change(&option.value, change.checked, &order);
                    cx.notify();
                    (change, selected)
                });
                if let Ok((change, selected)) = updated {
                    if let Some(callback) = &option.on_change {
                        callback(&change, window, cx);
                    }
                    if let Some(callback) = &callback {
                        callback(&selected, window, cx);
                    }
                }
            }));
        }
        self.accessibility.apply(root)
    }
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use gpui::{
        AppContext, Entity, FocusHandle, KeyDownEvent, KeyUpEvent, Keystroke, PlatformInput,
        Render, TestAppContext, WindowHandle,
    };

    use super::*;
    use crate::{ConfigPatch, UiRoot, behavior::focus_handle};

    #[test]
    fn uncontrolled_selection_preserves_hidden_values_and_emits_display_order() {
        let mut selection = GroupSelection {
            uncontrolled: vec!["hidden", "pear"],
            controlled: None,
        };
        assert_eq!(
            selection.change(&"apple", true, &["apple", "pear", "orange"]),
            ["apple", "pear"]
        );
        assert_eq!(selection.selected(), ["hidden", "pear", "apple"]);
        assert_eq!(
            selection.change(&"orange", true, &["pear", "hidden", "orange", "apple"]),
            ["pear", "hidden", "orange", "apple"]
        );
        assert_eq!(
            selection.change(&"pear", false, &["pear", "apple", "orange"]),
            ["apple", "orange"]
        );
    }

    #[test]
    fn controlled_selection_proposes_changes_without_mutating_owner_value() {
        let mut selection = GroupSelection {
            uncontrolled: vec![4],
            controlled: Some(vec![3, 1]),
        };
        assert_eq!(selection.change(&2, true, &[1, 2, 3]), [1, 2, 3]);
        assert_eq!(selection.selected(), [3, 1]);
        assert_eq!(selection.change(&3, false, &[1, 2, 3]), [1]);
        assert_eq!(selection.selected(), [3, 1]);
        selection.controlled = Some(vec![2]);
        assert_eq!(selection.change(&1, true, &[3, 2, 1]), [2, 1]);
    }

    #[test]
    fn duplicate_values_have_set_semantics_and_arbitrary_application_types_work() {
        #[derive(Clone, Debug, PartialEq)]
        struct Setting {
            name: &'static str,
            threshold: f32,
        }
        let setting = Setting {
            name: "alerts",
            threshold: 0.5,
        };
        let mut selection = GroupSelection {
            uncontrolled: vec![setting.clone(), setting.clone()],
            controlled: None,
        };
        let order = [setting.clone(), setting.clone()];
        assert_eq!(
            selection.change(&setting, true, &order).as_slice(),
            std::slice::from_ref(&setting)
        );
        assert!(selection.change(&setting, false, &order).is_empty());
        assert!(selection.selected().is_empty());
    }

    struct Consumer {
        scope: ConfigScope,
        focus: [FocusHandle; 3],
        value: Option<Vec<u8>>,
        defaults: Vec<u8>,
        order: Vec<u8>,
        disabled: Option<bool>,
        first_enabled: bool,
        external_focus: bool,
        changes: Rc<RefCell<Vec<Vec<u8>>>>,
        events: Rc<RefCell<Vec<&'static str>>>,
    }

    impl Consumer {
        fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
            Self {
                scope,
                focus: std::array::from_fn(|_| focus_handle(cx)),
                value: None,
                defaults: vec![3],
                order: vec![1, 2, 3],
                disabled: None,
                first_enabled: false,
                external_focus: true,
                changes: Rc::default(),
                events: Rc::default(),
            }
        }
    }

    impl Render for Consumer {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let changes = self.changes.clone();
            let events = self.events.clone();
            let options = self.order.iter().map(|value| {
                let index = usize::from(*value - 1);
                let events = self.events.clone();
                let mut option =
                    CheckboxOption::new(("option", index), *value, format!("Option {value}"))
                        .on_change(move |_, _, _| events.borrow_mut().push("option"));
                if self.external_focus {
                    option = option.focus_handle(&self.focus[index]);
                }
                if self.first_enabled && *value == 1 {
                    option = option.disabled(false);
                }
                option
            });
            let mut group = CheckboxGroup::new("group-test")
                .scope(self.scope.clone())
                .options(options)
                .default_value(self.defaults.clone())
                .vertical(true)
                .on_change(move |selected, _, _| {
                    events.borrow_mut().push("group");
                    changes.borrow_mut().push(selected.to_vec());
                });
            if let Some(value) = &self.value {
                group = group.value(value.clone());
            }
            if let Some(disabled) = self.disabled {
                group = group.disabled(disabled);
            }
            div().p(px(12.)).child(group)
        }
    }

    struct CachedPanel(Entity<Consumer>);

    impl Render for CachedPanel {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().size_full().child(
                self.0
                    .clone()
                    .cached(StyleRefinement::default().w(px(360.)).h(px(180.))),
            )
        }
    }

    struct Fixture {
        cx: TestAppContext,
        window: WindowHandle<UiRoot>,
        consumer: Entity<Consumer>,
    }

    impl Fixture {
        fn new(configure: impl FnOnce(&mut Consumer)) -> Self {
            let mut cx = TestAppContext::single();
            let mut consumer = None;
            let window = cx.add_window(|window, cx| {
                let scope = ConfigScope::new_window(cx);
                scope
                    .set_patch(
                        ConfigPatch {
                            motion: Some(false),
                            ..Default::default()
                        },
                        cx,
                    )
                    .unwrap();
                let content = cx.new(|cx| {
                    let mut content = Consumer::new(scope.clone(), cx);
                    configure(&mut content);
                    content
                });
                consumer = Some(content.clone());
                let panel = cx.new(|_| CachedPanel(content));
                UiRoot::new(scope, panel, window, cx)
            });
            let mut fixture = Self {
                cx,
                window,
                consumer: consumer.unwrap(),
            };
            fixture.draw();
            fixture
        }

        fn draw(&mut self) {
            self.cx.run_until_parked();
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    window.draw(cx).clear(cx);
                })
                .unwrap();
            self.cx.run_until_parked();
        }

        fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
            self.consumer.update(&mut self.cx, |consumer, cx| {
                edit(consumer);
                cx.notify();
            });
            self.draw();
        }

        fn toggle(&mut self, value: u8) {
            self.toggle_without_draw(value);
            self.draw();
        }

        fn toggle_without_draw(&mut self, value: u8) {
            let focus = self.consumer.read_with(&self.cx, |consumer, _| {
                consumer.focus[usize::from(value - 1)].clone()
            });
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    window.focus(&focus, cx);
                    for event in [
                        PlatformInput::KeyDown(KeyDownEvent {
                            keystroke: Keystroke::parse("space").unwrap(),
                            is_held: false,
                            prefer_character_input: false,
                        }),
                        PlatformInput::KeyUp(KeyUpEvent {
                            keystroke: Keystroke::parse("space").unwrap(),
                        }),
                    ] {
                        window.dispatch_event(event, cx);
                    }
                })
                .unwrap();
        }

        fn changes(&self) -> Vec<Vec<u8>> {
            self.consumer
                .read_with(&self.cx, |consumer, _| consumer.changes.borrow().clone())
        }
    }

    #[test]
    fn native_group_retains_selection_across_renders_and_reorders_callback_values() {
        let mut fixture = Fixture::new(|_| {});
        fixture.toggle(1);
        assert_eq!(fixture.changes(), [vec![1, 3]]);
        fixture.edit(|consumer| consumer.defaults = vec![2]);
        fixture.toggle(2);
        assert_eq!(fixture.changes().last().unwrap(), &[1, 2, 3]);
        fixture.edit(|consumer| consumer.order = vec![3, 2, 1]);
        fixture.toggle(2);
        assert_eq!(fixture.changes().last().unwrap(), &[3, 1]);
        let events = fixture
            .consumer
            .read_with(&fixture.cx, |consumer, _| consumer.events.borrow().clone());
        assert_eq!(
            events,
            ["option", "group", "option", "group", "option", "group"]
        );
    }

    #[test]
    fn native_controlled_group_waits_for_owner_and_respects_option_disabled_override() {
        let mut fixture = Fixture::new(|consumer| consumer.value = Some(vec![2]));
        fixture.toggle(1);
        fixture.toggle(1);
        assert_eq!(fixture.changes(), [vec![1, 2], vec![1, 2]]);
        fixture.edit(|consumer| {
            consumer.value = Some(vec![1, 2]);
            consumer.disabled = Some(true);
            consumer.first_enabled = true;
        });
        fixture.toggle(2);
        assert_eq!(fixture.changes().len(), 2);
        fixture.toggle(1);
        assert_eq!(fixture.changes().last().unwrap(), &[2]);
    }

    #[test]
    fn rapid_native_activations_read_the_live_uncontrolled_group_selection() {
        let mut fixture = Fixture::new(|consumer| consumer.defaults.clear());
        fixture.toggle_without_draw(1);
        fixture.toggle_without_draw(1);
        assert_eq!(fixture.changes(), [vec![1], vec![]]);
        fixture.draw();
        fixture.toggle(1);
        assert_eq!(fixture.changes().last().unwrap(), &[1]);
    }

    #[test]
    fn native_option_internal_focus_follows_its_stable_id_when_reordered() {
        let mut fixture = Fixture::new(|consumer| consumer.external_focus = false);
        let focus = fixture
            .cx
            .update_window(fixture.window.into(), |_, window, cx| {
                window.focus_next(cx);
                window.focused(cx).expect("first option is focused")
            })
            .unwrap();
        fixture.draw();
        fixture.edit(|consumer| consumer.order = vec![3, 2, 1]);
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, cx| {
                assert!(focus.is_focused(window));
                for event in [
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: Keystroke::parse("space").unwrap(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    PlatformInput::KeyUp(KeyUpEvent {
                        keystroke: Keystroke::parse("space").unwrap(),
                    }),
                ] {
                    window.dispatch_event(event, cx);
                }
            })
            .unwrap();
        fixture.draw();
        assert_eq!(fixture.changes(), [vec![3, 1]]);
    }
}
