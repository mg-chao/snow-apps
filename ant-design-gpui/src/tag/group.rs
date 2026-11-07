// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native composition and retained selection for checkable tag groups.

use std::rc::Rc;

use gpui::{
    App, Context, ElementId, FocusHandle, IntoElement, Refineable, RenderOnce, Role, SharedString,
    StyleRefinement, Styled, Subscription, Window, div, prelude::*, px,
};

use super::{CheckableTag, TagChange, TagTheme};
use crate::{ConfigScope, Direction, behavior::Accessibility, icons::IconRef};

type ChangeHandler<T> = Rc<dyn Fn(&[T], &mut Window, &mut App)>;
type OptionChangeHandler = Box<dyn Fn(&TagChange, &mut Window, &mut App)>;

/// One selectable tag with a stable identity independent of its value and position.
///
/// IDs must be unique within the group and remain stable when options reorder.
pub struct TagOption<T> {
    id: ElementId,
    value: T,
    label: SharedString,
    icon: Option<IconRef>,
    disabled: Option<bool>,
    style: StyleRefinement,
    automation_id: Option<SharedString>,
    focus: Option<FocusHandle>,
    on_change: Option<OptionChangeHandler>,
}

impl<T> TagOption<T> {
    pub fn new(id: impl Into<ElementId>, value: T, label: impl Into<SharedString>) -> Self {
        Self {
            id: id.into(),
            value,
            label: label.into(),
            icon: None,
            disabled: None,
            style: StyleRefinement::default(),
            automation_id: None,
            focus: None,
            on_change: None,
        }
    }

    pub fn icon(mut self, icon: impl Into<IconRef>) -> Self {
        self.icon = Some(icon.into());
        self
    }

    /// An explicit false overrides both the group and its provider.
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

    /// Runs before the group's selection callback.
    pub fn on_change(
        mut self,
        handler: impl Fn(&TagChange, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_change = Some(Box::new(handler));
        self
    }
}

/// A wrapping group supporting optional single selection or multiple selection.
///
/// Single selection is the default and can be cleared by activating the selected
/// tag. `.multiple(true)` enables independent toggles. Selection callbacks contain
/// zero or one values in single mode, or values in selection order in multiple mode.
/// Controlled values are proposals until the caller supplies a new `.value(...)`.
#[derive(IntoElement)]
pub struct CheckableTagGroup<T: Clone + PartialEq + 'static> {
    id: ElementId,
    scope: Option<ConfigScope>,
    options: Vec<TagOption<T>>,
    value: Option<Vec<T>>,
    default_value: Vec<T>,
    multiple: bool,
    disabled: Option<bool>,
    accessibility: Accessibility,
    on_change: Option<ChangeHandler<T>>,
    style: StyleRefinement,
    item_style: StyleRefinement,
}

impl<T: Clone + PartialEq + 'static> CheckableTagGroup<T> {
    pub fn new(id: impl Into<ElementId>) -> Self {
        Self {
            id: id.into(),
            scope: None,
            options: Vec::new(),
            value: None,
            default_value: Vec::new(),
            multiple: false,
            disabled: None,
            accessibility: Accessibility::new(Role::Group),
            on_change: None,
            style: StyleRefinement::default(),
            item_style: StyleRefinement::default(),
        }
    }

    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }

    pub fn options(mut self, options: impl IntoIterator<Item = TagOption<T>>) -> Self {
        self.options = options.into_iter().collect();
        self
    }

    pub fn multiple(mut self, multiple: bool) -> Self {
        self.multiple = multiple;
        self
    }

    /// Supply controlled selection. Single mode uses the first value only.
    pub fn value(mut self, value: impl IntoIterator<Item = T>) -> Self {
        self.value = Some(value.into_iter().collect());
        self
    }

    /// Seed retained selection on mount. Subsequent renders preserve user input.
    pub fn default_value(mut self, value: impl IntoIterator<Item = T>) -> Self {
        self.default_value = value.into_iter().collect();
        self
    }

    /// Convenient controlled value for optional single selection.
    pub fn selected(self, value: Option<T>) -> Self {
        self.value(value)
    }

    /// Convenient initial value for optional single selection.
    pub fn default_selected(self, value: Option<T>) -> Self {
        self.default_value(value)
    }

    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
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

    /// Refine every tag root. Option-specific styles take precedence.
    pub fn item_style(mut self, style: StyleRefinement) -> Self {
        self.item_style = style;
        self
    }
}

impl<T: Clone + PartialEq + 'static> Styled for CheckableTagGroup<T> {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

struct GroupSelection<T> {
    uncontrolled: Vec<T>,
    controlled: Option<Vec<T>>,
    multiple: bool,
}

impl<T: Clone + PartialEq> GroupSelection<T> {
    fn selected(&self) -> &[T] {
        let selected = self.controlled.as_deref().unwrap_or(&self.uncontrolled);
        if self.multiple {
            selected
        } else {
            &selected[..selected.len().min(1)]
        }
    }

    fn change(&mut self, value: &T, checked: bool) -> Vec<T> {
        let next = if self.multiple {
            let mut next = self.selected().to_vec();
            if checked {
                if !next.contains(value) {
                    next.push(value.clone());
                }
            } else {
                next.retain(|selected| selected != value);
            }
            next
        } else if checked {
            vec![value.clone()]
        } else {
            Vec::new()
        };
        if self.controlled.is_none() {
            self.uncontrolled.clone_from(&next);
        }
        next
    }
}

struct GroupState<T> {
    scope: ConfigScope,
    selection: GroupSelection<T>,
    _config: Subscription,
}

impl<T: Clone + PartialEq + 'static> GroupState<T> {
    fn new(scope: ConfigScope, initial: Vec<T>, multiple: bool, cx: &mut Context<Self>) -> Self {
        let subscription = scope.observe(cx);
        Self {
            scope,
            selection: GroupSelection {
                uncontrolled: initial,
                controlled: None,
                multiple,
            },
            _config: subscription,
        }
    }
}

impl<T: Clone + PartialEq + 'static> RenderOnce for CheckableTagGroup<T> {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self.scope.unwrap_or_else(|| ConfigScope::application(cx));
        let state = window.use_keyed_state(self.id.clone(), cx, |_, cx| {
            GroupState::new(
                scope.clone(),
                std::mem::take(&mut self.default_value),
                self.multiple,
                cx,
            )
        });
        state.update(cx, |state, cx| {
            if state.scope != scope {
                state._config = scope.observe(cx);
                state.scope = scope.clone();
            }
            state.selection.controlled = self.value.take();
            state.selection.multiple = self.multiple;
        });
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<TagTheme>();
        let global = tokens.globals();
        let mut root = div()
            .id(self.id)
            .flex()
            .flex_wrap()
            .gap(px(global.padding_xs as f32))
            .items_start()
            .font(tokens.font().clone())
            .text_size(px(global.font_size as f32))
            .line_height(gpui::relative(global.line_height as f32))
            .text_color(global.color_text.to_hsla())
            .when(config.direction == Direction::Rtl, |root| {
                root.flex_row_reverse()
            });
        root.style().refine(&self.style);
        for option in self.options {
            let checked = state.read(cx).selection.selected().contains(&option.value);
            let disabled = option.disabled.or(self.disabled);
            let state = state.downgrade();
            let callback = self.on_change.clone();
            let mut tag = CheckableTag::new(option.id)
                .scope(scope.clone())
                .label(option.label)
                .checked(checked);
            if let Some(icon) = option.icon {
                tag = tag.icon(icon);
            }
            if let Some(disabled) = disabled {
                tag = tag.disabled(disabled);
            }
            if let Some(id) = option.automation_id {
                tag = tag.automation_id(id);
            }
            if let Some(focus) = option.focus {
                tag = tag.focus_handle(&focus);
            }
            tag.style().refine(&self.item_style);
            tag.style().refine(&option.style);
            root = root.child(tag.on_change(move |change, window, cx| {
                let updated = state.update(cx, |state, cx| {
                    // Read retained owner state: child snapshots may precede a
                    // second activation arriving before the next native frame.
                    let change = TagChange {
                        checked: !state.selection.selected().contains(&option.value),
                        activation: change.activation,
                    };
                    let selected = state.selection.change(&option.value, change.checked);
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
        AppContext, Entity, KeyDownEvent, KeyUpEvent, Keystroke, PlatformInput, Render,
        TestAppContext, WindowHandle,
    };

    use super::*;
    use crate::{ConfigPatch, UiRoot, behavior::focus_handle};

    #[test]
    fn optional_single_selection_replaces_and_clears() {
        let mut selection = GroupSelection {
            uncontrolled: vec!["books"],
            controlled: None,
            multiple: false,
        };
        assert_eq!(selection.change(&"music", true), ["music"]);
        assert_eq!(selection.selected(), ["music"]);
        assert!(selection.change(&"music", false).is_empty());
        assert!(selection.selected().is_empty());
    }

    #[test]
    fn multiple_selection_preserves_hidden_values_and_insertion_order() {
        let mut selection = GroupSelection {
            uncontrolled: vec!["hidden", "music"],
            controlled: None,
            multiple: true,
        };
        assert_eq!(
            selection.change(&"books", true),
            ["hidden", "music", "books"]
        );
        assert_eq!(selection.change(&"music", false), ["hidden", "books"]);
        assert_eq!(
            selection.change(&"music", true),
            ["hidden", "books", "music"]
        );
        assert_eq!(
            selection.change(&"music", true),
            ["hidden", "books", "music"]
        );
    }

    #[test]
    fn controlled_selection_waits_for_owner_and_mode_change_has_single_invariant() {
        let mut selection = GroupSelection {
            uncontrolled: vec![1],
            controlled: Some(vec![2, 3]),
            multiple: true,
        };
        assert_eq!(selection.change(&1, true), [2, 3, 1]);
        assert_eq!(selection.selected(), [2, 3]);
        selection.multiple = false;
        assert_eq!(selection.selected(), [2]);
        assert_eq!(selection.change(&3, true), [3]);
        assert_eq!(selection.selected(), [2]);
        selection.controlled = Some(Vec::new());
        assert!(selection.selected().is_empty());
    }

    struct Consumer {
        scope: ConfigScope,
        focus: [FocusHandle; 3],
        value: Option<Vec<u8>>,
        defaults: Vec<u8>,
        multiple: bool,
        order: Vec<u8>,
        disabled: bool,
        first_enabled: bool,
        changes: Rc<RefCell<Vec<Vec<u8>>>>,
    }

    impl Consumer {
        fn new(scope: ConfigScope, cx: &mut Context<Self>) -> Self {
            Self {
                scope,
                focus: std::array::from_fn(|_| focus_handle(cx)),
                value: None,
                defaults: vec![2],
                multiple: false,
                order: vec![1, 2, 3],
                disabled: false,
                first_enabled: false,
                changes: Rc::default(),
            }
        }
    }

    impl Render for Consumer {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let changes = self.changes.clone();
            let options = self.order.iter().map(|value| {
                let index = usize::from(*value - 1);
                let mut option =
                    TagOption::new(("tag-option", index), *value, format!("Option {value}"))
                        .focus_handle(&self.focus[index]);
                if self.first_enabled && *value == 1 {
                    option = option.disabled(false);
                }
                option
            });
            let mut group = CheckableTagGroup::new("tag-group-test")
                .scope(self.scope.clone())
                .options(options)
                .multiple(self.multiple)
                .default_value(self.defaults.clone())
                .disabled(self.disabled)
                .on_change(move |value, _, _| changes.borrow_mut().push(value.to_vec()));
            if let Some(value) = &self.value {
                group = group.value(value.clone());
            }
            div().p(px(12.)).child(group)
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
                UiRoot::new(scope, content, window, cx)
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

        fn toggle(&mut self, value: u8) {
            self.toggle_without_draw(value);
            self.draw();
        }

        fn changes(&self) -> Vec<Vec<u8>> {
            self.consumer
                .read_with(&self.cx, |consumer, _| consumer.changes.borrow().clone())
        }
    }

    #[test]
    fn native_single_group_retains_selection_and_clears_on_second_activation() {
        let mut fixture = Fixture::new(|_| {});
        fixture.toggle(1);
        fixture.edit(|consumer| {
            consumer.defaults = vec![3];
            consumer.order = vec![3, 2, 1];
        });
        fixture.toggle(1);
        assert_eq!(fixture.changes(), [vec![1], vec![]]);
        fixture.toggle(3);
        assert_eq!(fixture.changes().last().unwrap(), &[3]);
    }

    #[test]
    fn native_multiple_group_reads_live_selection_before_another_frame() {
        let mut fixture = Fixture::new(|consumer| consumer.multiple = true);
        fixture.toggle_without_draw(1);
        fixture.toggle_without_draw(1);
        assert_eq!(fixture.changes(), [vec![2, 1], vec![2]]);
        fixture.draw();
        fixture.edit(|consumer| consumer.order = vec![3, 2, 1]);
        fixture.toggle(3);
        assert_eq!(fixture.changes().last().unwrap(), &[2, 3]);
    }

    #[test]
    fn native_controlled_group_waits_for_owner_and_honors_option_availability() {
        let mut fixture = Fixture::new(|consumer| consumer.value = Some(vec![2]));
        fixture.toggle(1);
        fixture.toggle(1);
        assert_eq!(fixture.changes(), [vec![1], vec![1]]);
        fixture.edit(|consumer| {
            consumer.value = Some(vec![1]);
            consumer.disabled = true;
            consumer.first_enabled = true;
        });
        fixture.toggle(2);
        assert_eq!(fixture.changes().len(), 2);
        fixture.toggle(1);
        assert!(fixture.changes().last().unwrap().is_empty());
    }
}
