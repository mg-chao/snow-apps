// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::{
    SelectChangeSource, SelectEvent, SelectOption, SelectRow, SelectState, SelectTheme,
    SelectTokens, SelectValue, state::EventHandler,
};
use crate::{
    ConfigScope, Direction, Input, Size, ValidationStatus, Variant,
    behavior::{Accessibility, FocusRing, MotionPolicy},
    icons::{IconName, IconRenderOptions, icon},
    input::{InputGroupContext, InputGroupItem, InputInteraction, motion::CSS_EASE},
    popup::{Popup, PopupOpenReason, PopupPlacement, PopupTriggers, PopupWidth},
};
use gpui::{
    AnyElement, App, ElementId, Entity, Focusable, IntoElement, MouseButton, Refineable,
    RenderOnce, Role, SharedString, StyleRefinement, Styled, Subscription, WeakEntity, Window,
    accesskit, canvas, div, prelude::*, px, relative, uniform_list,
};
use std::{hash::Hash, rc::Rc, sync::Arc};

fn themed_icon(
    name: IconName,
    size: f32,
    color: crate::theme::Color,
    window: &Window,
) -> crate::icons::Icon {
    icon(name)
        .inherit_color()
        .render_options(IconRenderOptions {
            size: gpui::size(px(size), px(size)),
            scale_factor: window.scale_factor(),
            colors: crate::icons::IconPalette {
                text: color.to_rgba(),
                ..Default::default()
            },
            ..Default::default()
        })
}

/// The parts correspond to Select's semantic styling slots. Layout-critical row
/// height belongs to `list_item_height`, so virtualization has an explicit contract.
#[derive(Clone, Default)]
pub struct SelectStyles {
    pub root: StyleRefinement,
    pub prefix: StyleRefinement,
    pub suffix: StyleRefinement,
    pub content: StyleRefinement,
    pub input: StyleRefinement,
    pub placeholder: StyleRefinement,
    pub item: StyleRefinement,
    pub item_content: StyleRefinement,
    pub item_remove: StyleRefinement,
    pub clear: StyleRefinement,
    pub popup: StyleRefinement,
    pub option: StyleRefinement,
    pub option_content: StyleRefinement,
}
impl SelectStyles {
    fn refine(&mut self, other: &Self) {
        macro_rules! parts { ($($part:ident),+) => { $(self.$part.refine(&other.$part);)+ }; }
        parts!(
            root,
            prefix,
            suffix,
            content,
            input,
            placeholder,
            item,
            item_content,
            item_remove,
            clear,
            popup,
            option,
            option_content
        );
    }
}
#[derive(Clone, Copy, Debug)]
pub struct SelectStyleContext {
    pub size: Size,
    pub variant: Variant,
    pub status: ValidationStatus,
    pub mode: super::SelectMode,
    pub disabled: bool,
    pub focused: bool,
    pub hovered: bool,
    pub open: bool,
}
type StyleResolver = Rc<dyn Fn(&SelectStyleContext, &mut Window, &mut App) -> SelectStyles>;

#[derive(Clone)]
pub struct SelectOptionRender<T> {
    pub option: SelectOption<T>,
    pub index: usize,
    pub selected: bool,
    pub active: bool,
    pub disabled: bool,
}
/// Custom tag content can close itself through this retained, weak connection.
#[derive(Clone)]
pub struct SelectTagRender<T: 'static> {
    pub id: ElementId,
    pub value: SelectValue<T>,
    pub disabled: bool,
    pub closable: bool,
    state: WeakEntity<SelectState<T>>,
}
impl<T: Clone + Eq + Hash + 'static> SelectTagRender<T> {
    pub fn close(&self, window: &mut Window, cx: &mut App) {
        if self.closable {
            let _ = self.state.update(cx, |s, cx| {
                s.remove(&self.value.value, SelectChangeSource::Remove, window, cx)
            });
        }
    }
}
type OptionRenderer<T> = Rc<dyn Fn(&SelectOptionRender<T>, &mut Window, &mut App) -> AnyElement>;
type TagRenderer<T> = Rc<dyn Fn(&SelectTagRender<T>, &mut Window, &mut App) -> AnyElement>;
type LabelRenderer<T> = Rc<dyn Fn(&SelectValue<T>, &mut Window, &mut App) -> AnyElement>;
type ContentRenderer = Rc<dyn Fn(&mut Window, &mut App) -> AnyElement>;
type PopupRenderer<T> =
    Rc<dyn Fn(AnyElement, &Entity<SelectState<T>>, &mut Window, &mut App) -> AnyElement>;
type OverflowRenderer<T> = Rc<dyn Fn(&[SelectValue<T>], &mut Window, &mut App) -> AnyElement>;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SelectListLayout {
    #[default]
    Uniform,
    /// Native GPUI list with measured heights and lazy offscreen content.
    Variable,
}

/// Native Select view. Retain its controller and immutable options between renders.
#[derive(IntoElement)]
pub struct Select<T: Clone + Eq + Hash + 'static> {
    id: ElementId,
    state: Entity<SelectState<T>>,
    scope: Option<ConfigScope>,
    size: Option<Size>,
    variant: Option<Variant>,
    status: ValidationStatus,
    disabled: Option<bool>,
    value: Option<Vec<SelectValue<T>>>,
    default_value: Option<Vec<SelectValue<T>>>,
    open: Option<bool>,
    default_open: bool,
    search_value: Option<SharedString>,
    placeholder: SharedString,
    loading: bool,
    allow_clear: bool,
    prefix: Option<AnyElement>,
    suffix: Option<AnyElement>,
    show_suffix: bool,
    clear_icon: Option<AnyElement>,
    remove_icon: Option<ContentRenderer>,
    selected_icon: Option<ContentRenderer>,
    styles: SelectStyles,
    styles_fn: Option<StyleResolver>,
    accessibility: Accessibility,
    group: Option<InputGroupContext>,
    list_height: f32,
    list_item_height: Option<f32>,
    list_layout: SelectListLayout,
    virtualized: bool,
    popup_width: PopupWidth,
    placement: Option<PopupPlacement>,
    max_tag_count: Option<usize>,
    responsive_tags: bool,
    max_tag_text_length: Option<usize>,
    option_render: Option<OptionRenderer<T>>,
    tag_render: Option<TagRenderer<T>>,
    label_render: Option<LabelRenderer<T>>,
    popup_render: Option<PopupRenderer<T>>,
    overflow_render: Option<OverflowRenderer<T>>,
    empty_render: Option<ContentRenderer>,
    on_event: Option<EventHandler<T>>,
}
impl<T: Clone + Eq + Hash + 'static> Select<T> {
    pub fn new(id: impl Into<ElementId>, state: &Entity<SelectState<T>>) -> Self {
        Self {
            id: id.into(),
            state: state.clone(),
            scope: None,
            size: None,
            variant: None,
            status: ValidationStatus::None,
            disabled: None,
            value: None,
            default_value: None,
            open: None,
            default_open: false,
            search_value: None,
            placeholder: "".into(),
            loading: false,
            allow_clear: false,
            prefix: None,
            suffix: None,
            show_suffix: true,
            clear_icon: None,
            remove_icon: None,
            selected_icon: None,
            styles: SelectStyles::default(),
            styles_fn: None,
            accessibility: Accessibility::new(Role::ComboBox),
            group: None,
            list_height: 256.,
            list_item_height: None,
            list_layout: SelectListLayout::Uniform,
            virtualized: true,
            popup_width: PopupWidth::Trigger,
            placement: None,
            max_tag_count: None,
            responsive_tags: false,
            max_tag_text_length: None,
            option_render: None,
            tag_render: None,
            label_render: None,
            popup_render: None,
            overflow_render: None,
            empty_render: None,
            on_event: None,
        }
    }
    pub fn scope(mut self, scope: ConfigScope) -> Self {
        self.scope = Some(scope);
        self
    }
    pub fn size(mut self, size: Size) -> Self {
        self.size = Some(size);
        self
    }
    pub fn variant(mut self, variant: Variant) -> Self {
        self.variant = Some(variant);
        self
    }
    pub fn status(mut self, status: ValidationStatus) -> Self {
        self.status = status;
        self
    }
    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = Some(disabled);
        self
    }
    pub fn value(mut self, value: impl IntoIterator<Item = SelectValue<T>>) -> Self {
        self.value = Some(value.into_iter().collect());
        self
    }
    pub fn default_value(mut self, value: impl IntoIterator<Item = SelectValue<T>>) -> Self {
        self.default_value = Some(value.into_iter().collect());
        self
    }
    pub fn open(mut self, open: bool) -> Self {
        self.open = Some(open);
        self
    }
    pub fn default_open(mut self, open: bool) -> Self {
        self.default_open = open;
        self
    }
    pub fn search_value(mut self, value: impl Into<SharedString>) -> Self {
        self.search_value = Some(value.into());
        self
    }
    pub fn placeholder(mut self, value: impl Into<SharedString>) -> Self {
        self.placeholder = value.into();
        self
    }
    pub fn loading(mut self, loading: bool) -> Self {
        self.loading = loading;
        self
    }
    pub fn allow_clear(mut self, clear: bool) -> Self {
        self.allow_clear = clear;
        self
    }
    pub fn prefix(mut self, content: impl IntoElement) -> Self {
        self.prefix = Some(content.into_any_element());
        self
    }
    pub fn suffix(mut self, content: impl IntoElement) -> Self {
        self.suffix = Some(content.into_any_element());
        self
    }
    pub fn show_suffix(mut self, show: bool) -> Self {
        self.show_suffix = show;
        self
    }
    pub fn clear_icon(mut self, content: impl IntoElement) -> Self {
        self.clear_icon = Some(content.into_any_element());
        self
    }
    pub fn remove_icon<E: IntoElement>(
        mut self,
        render: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.remove_icon = Some(Rc::new(move |w, c| render(w, c).into_any_element()));
        self
    }
    pub fn selected_icon<E: IntoElement>(
        mut self,
        render: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.selected_icon = Some(Rc::new(move |w, c| render(w, c).into_any_element()));
        self
    }
    pub fn styles(mut self, styles: SelectStyles) -> Self {
        self.styles = styles;
        self
    }
    pub fn styles_fn(
        mut self,
        resolve: impl Fn(&SelectStyleContext, &mut Window, &mut App) -> SelectStyles + 'static,
    ) -> Self {
        self.styles_fn = Some(Rc::new(resolve));
        self
    }
    pub fn accessibility_label(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.label = Some(label.into());
        self
    }
    pub fn accessibility_description(mut self, label: impl Into<SharedString>) -> Self {
        self.accessibility.description = Some(label.into());
        self
    }
    pub fn automation_id(mut self, id: impl Into<SharedString>) -> Self {
        self.accessibility.automation_id = Some(id.into());
        self
    }
    pub fn list_height(mut self, height: f32) -> Self {
        if height.is_finite() && height > 0. {
            self.list_height = height;
        }
        self
    }
    pub fn list_item_height(mut self, height: f32) -> Self {
        if height.is_finite() && height > 0. {
            self.list_item_height = Some(height);
        }
        self
    }
    pub fn virtualized(mut self, virtualized: bool) -> Self {
        self.virtualized = virtualized;
        self
    }
    pub fn list_layout(mut self, layout: SelectListLayout) -> Self {
        self.list_layout = layout;
        self
    }
    pub fn popup_width(mut self, width: PopupWidth) -> Self {
        self.popup_width = width;
        self
    }
    /// Ant Design's numeric popup width: never narrower than the trigger.
    pub fn popup_min_width(self, width: gpui::Pixels) -> Self {
        self.popup_width(PopupWidth::TriggerMinimum(width))
    }
    pub fn placement(mut self, placement: PopupPlacement) -> Self {
        self.placement = Some(placement);
        self
    }
    pub fn max_tag_count(mut self, count: usize) -> Self {
        self.max_tag_count = Some(count);
        self.responsive_tags = false;
        self
    }
    pub fn responsive_tags(mut self, responsive: bool) -> Self {
        self.responsive_tags = responsive;
        self
    }
    pub fn max_tag_text_length(mut self, count: usize) -> Self {
        self.max_tag_text_length = Some(count);
        self
    }
    pub fn option_render<E: IntoElement>(
        mut self,
        render: impl Fn(&SelectOptionRender<T>, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.option_render = Some(Rc::new(move |o, w, c| render(o, w, c).into_any_element()));
        self
    }
    pub fn tag_render<E: IntoElement>(
        mut self,
        render: impl Fn(&SelectTagRender<T>, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.tag_render = Some(Rc::new(move |o, w, c| render(o, w, c).into_any_element()));
        self
    }
    pub fn label_render<E: IntoElement>(
        mut self,
        render: impl Fn(&SelectValue<T>, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.label_render = Some(Rc::new(move |o, w, c| render(o, w, c).into_any_element()));
        self
    }
    pub fn popup_render<E: IntoElement>(
        mut self,
        render: impl Fn(AnyElement, &Entity<SelectState<T>>, &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.popup_render = Some(Rc::new(move |o, s, w, c| {
            render(o, s, w, c).into_any_element()
        }));
        self
    }
    pub fn overflow_render<E: IntoElement>(
        mut self,
        render: impl Fn(&[SelectValue<T>], &mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.overflow_render = Some(Rc::new(move |o, w, c| render(o, w, c).into_any_element()));
        self
    }
    pub fn empty_render<E: IntoElement>(
        mut self,
        render: impl Fn(&mut Window, &mut App) -> E + 'static,
    ) -> Self {
        self.empty_render = Some(Rc::new(move |w, c| render(w, c).into_any_element()));
        self
    }
    pub fn on_event(
        mut self,
        handler: impl Fn(&SelectEvent<T>, &mut Window, &mut App) + 'static,
    ) -> Self {
        self.on_event = Some(Rc::new(handler));
        self
    }
}
impl<T: Clone + Eq + Hash + 'static> Styled for Select<T> {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.styles.root
    }
}
impl<T: Clone + Eq + Hash + 'static> InputGroupItem for Select<T> {
    fn into_group_element(mut self, group: InputGroupContext) -> AnyElement {
        self.scope = self.scope.or_else(|| Some(group.scope.clone()));
        self.size = self.size.or(Some(group.size));
        self.disabled = self.disabled.or(group.disabled);
        self.group = Some(group);
        self.into_any_element()
    }
}
struct Mount<T: 'static> {
    state: WeakEntity<SelectState<T>>,
    _observer: Subscription,
    _release: Subscription,
}

impl<T: Clone + Eq + Hash + 'static> RenderOnce for Select<T> {
    fn render(mut self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        let scope = self
            .scope
            .clone()
            .unwrap_or_else(|| ConfigScope::application(cx));
        let config = scope.snapshot(cx);
        let tokens = config.theme.component::<SelectTheme>();
        let g = &tokens.globals;
        let state = self.state.clone();
        let observed = state.clone();
        let _mount = window.use_keyed_state(self.id.clone(), cx, move |_, cx| {
            observed.update(cx, |s, _| s.mounted = true);
            Mount {
                state: observed.downgrade(),
                _observer: cx.observe(&observed, |_, _, cx| cx.notify()),
                _release: cx.on_release(|this: &mut Mount<T>, cx| {
                    let _ = this.state.update(cx, |s, _| {
                        s.mounted = false;
                        s.open = false;
                        s.hovered = false;
                    });
                }),
            }
        });
        state.update(cx, |s, cx| {
            s.configure_scope(scope.clone(), window, cx);
            s.disabled_override = self.disabled;
            s.controlled = self.value.is_some();
            s.controlled_open = self.open.is_some();
            s.controlled_search = self.search_value.is_some();
            s.on_event = self.on_event.clone();
            s.rendered_rows = 0;
            let initialize = !s.initialized;
            if let Some(value) = &self.value {
                s.set_value(value.clone(), cx);
            } else if initialize && let Some(value) = &self.default_value {
                s.set_value(value.clone(), cx);
            }
            if let Some(open) = self.open {
                s.set_open(open, cx);
            } else if !s.open_initialized {
                s.set_open(self.default_open, cx);
            }
            if let Some(query) = &self.search_value {
                s.set_search_value(query.clone(), window, cx);
            }
            s.initialized = true;
            if s.disabled(cx) {
                s.open = false;
            }
        });
        let size = self.size.unwrap_or(config.size);
        let variant = self.variant.unwrap_or(config.variant);
        let s = state.read(cx);
        let disabled = s.disabled(cx);
        let focused = s.focus_handle(cx).is_focused(window);
        let focus = s.focus_handle(cx);
        let hovered = s.hovered;
        let multiple = s.model.settings.mode.multiple();
        let searchable = s.model.searchable();
        let open = s.open;
        let values = s.model.values.clone();
        let query = if s.editor.read(cx).is_composing() {
            s.editor.read(cx).value_snapshot()
        } else {
            s.model.query.clone()
        };
        let editor = s.editor.clone();
        let mode = s.model.settings.mode;
        let at_limit = s.model.at_limit();
        if let Some(resolve) = &self.styles_fn {
            let style = resolve(
                &SelectStyleContext {
                    size,
                    variant,
                    status: self.status,
                    mode,
                    disabled,
                    focused,
                    hovered,
                    open,
                },
                window,
                cx,
            );
            self.styles.refine(&style);
        }
        let mut metrics = tokens
            .surface(
                size,
                variant,
                self.status,
                multiple,
                InputInteraction {
                    disabled,
                    hovered,
                    focused,
                    keyboard_focus: window.last_input_was_keyboard(),
                },
            )
            .at_scale(window.scale_factor());
        let mut policy = MotionPolicy::new(config.motion && g.motion, cx.reduce_motion());
        policy.enabled &= window.is_visible() && window.is_window_active();
        let (shadows, rotation, animating) = state.update(cx, |s, cx| {
            let now = cx
                .background_executor()
                .now()
                .saturating_duration_since(s.epoch);
            let (shadows, active) = s.surface.resolve(
                &mut metrics,
                now,
                g.motion_duration_slow,
                g.motion_duration_slow,
                CSS_EASE,
                policy,
            );
            (
                shadows,
                if policy.allows_motion() {
                    (now.as_secs_f32() % 1.) * std::f32::consts::TAU
                } else {
                    0.
                },
                active || (self.loading && policy.allows_motion()),
            )
        });
        let target = state.clone();
        let hover_target = state.clone();
        let key_target = state.clone();
        let keyup_target = state.clone();
        let mut root = div()
            .id(self.id.clone())
            .relative()
            .flex()
            .items_center()
            .w_full()
            .min_w_0()
            .font(config.font.clone())
            .text_size(px(metrics.font_size as f32))
            .line_height(px(metrics.line_height as f32))
            .text_color(metrics.text)
            .bg(metrics.background)
            .border(px(metrics.border_width as f32))
            .border_color(metrics.border)
            .rounded(px(metrics.radius as f32))
            .shadow(shadows)
            .px(px(metrics.padding_inline as f32))
            .py(px(metrics.padding_block as f32))
            .min_h(px(
                (tokens.height(size) - 2. * (g.line_width - metrics.border_width)) as f32,
            ))
            .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse())
            .when(disabled, |d| d.cursor_not_allowed())
            .when(!disabled && searchable, |d| d.cursor_text())
            .when(!disabled && !searchable, |d| d.cursor_pointer())
            .on_hover(move |hovered, _, cx| {
                hover_target.update(cx, |s, cx| {
                    s.hovered = *hovered;
                    cx.notify();
                })
            })
            .on_mouse_down(MouseButton::Left, move |_, w, c| {
                target.update(c, |s, c| {
                    if !s.disabled(c) {
                        s.focus(w, c);
                        let open =
                            s.model.searchable() || s.model.settings.mode.multiple() || !s.open;
                        s.request_open(open, PopupOpenReason::Click, w, c);
                    }
                })
            })
            .capture_key_down(move |event, w, c| {
                if !event.keystroke.modifiers.control
                    && !event.keystroke.modifiers.platform
                    && key_target.update(c, |s, c| s.key(&event.keystroke.key, w, c))
                {
                    w.prevent_default();
                    c.stop_propagation();
                }
            })
            .on_key_up(move |event, _, c| {
                keyup_target.update(c, |s, _| s.key_up(&event.keystroke.key))
            });
        if animating {
            let animation_target = state.clone();
            root = root.child(
                canvas(
                    |_, _, _| (),
                    move |bounds, _, window, cx| {
                        let visible = window.content_mask().bounds.intersect(&bounds);
                        if visible.size.width > px(0.) && visible.size.height > px(0.) {
                            animation_target
                                .update(cx, |state, cx| state.request_frame(window, cx));
                        }
                    },
                )
                .absolute()
                .inset_0(),
            );
        }
        if metrics.underline_only {
            root = root
                .border_0()
                .border_b(px(metrics.border_width as f32))
                .pt(px((metrics.padding_block + metrics.border_width) as f32))
                .px(px((metrics.padding_inline + metrics.border_width) as f32));
        }
        if multiple {
            let start =
                ((tokens.height(size) - tokens.tag_height(size)) / 2. - g.line_width).max(0.);
            root = match config.direction {
                Direction::Ltr => root.pl(px(start as f32)),
                Direction::Rtl => root.pr(px(start as f32)),
            };
        }
        if let Some(prefix) = self.prefix {
            let mut slot = div()
                .flex()
                .flex_shrink_0()
                .items_center()
                .text_color(metrics.affix)
                .child(prefix);
            if config.direction == Direction::Ltr {
                slot = slot.mr(px(g.padding_xxs as f32));
            } else {
                slot = slot.ml(px(g.padding_xxs as f32));
            }
            slot.style().refine(&self.styles.prefix);
            root = root.child(slot);
        }
        let mut input = Input::new("editor", &editor)
            .scope(scope.clone())
            .size(size)
            .disabled(disabled)
            .read_only(!searchable || at_limit)
            .semantic_role(Role::ComboBox);
        input.embedded = true;
        if multiple {
            input.embedded_line_height =
                Some(tokens.tag_height(size) + 2. * (g.padding_xxs / 2.).floor());
        }
        if let Some(label) = &self.accessibility.label {
            input = input.accessibility_label(label.clone());
        }
        if let Some(description) = &self.accessibility.description {
            input = input.accessibility_description(description.clone());
        }
        if let Some(id) = &self.accessibility.automation_id {
            input = input.automation_id(id.clone());
        }
        input.style().refine(&self.styles.input);
        let a11y_target = state.clone();
        let accessible_list = (open && window.is_a11y_active()).then(|| {
            super::accessibility::list(
                &state,
                self.accessibility.automation_id.as_ref(),
                self.virtualized && self.popup_width != PopupWidth::Content,
                cx,
            )
        });
        let accessible_placeholder = self.placeholder.clone();
        let accessible_value: SharedString = if query.is_empty() {
            state.read(cx).model.accessible_value.clone()
        } else {
            query.clone()
        };
        input.frame_decorator = Some(Box::new(move |mut root, _, _, _| {
            // Bound actions run before raw keys in GPUI. Register Select's
            // commands before Input's editing fallbacks on the same focus node.
            macro_rules! command {
                ($action:ty, $key:literal) => {
                    let target = a11y_target.clone();
                    root = root.on_action(move |_: &$action, window, cx| {
                        if target.update(cx, |s, cx| s.key($key, window, cx)) {
                            window.prevent_default();
                        } else {
                            cx.propagate();
                        }
                    });
                };
            }
            command!(crate::input::actions::Up, "up");
            command!(crate::input::actions::Down, "down");
            command!(crate::input::actions::Enter, "enter");
            command!(crate::input::actions::Backspace, "backspace");
            command!(crate::behavior::FocusNext, "tab");
            command!(crate::behavior::FocusPrevious, "tab");
            if let Some(list) = accessible_list {
                root = root.child(list);
            }
            root.aria_value(accessible_value)
                .aria_placeholder(accessible_placeholder)
                .aria_expanded(open)
                .on_a11y_action(accesskit::Action::Expand, {
                    let state = a11y_target.clone();
                    move |_, w, c| {
                        state.update(c, |s, c| {
                            s.request_open(true, PopupOpenReason::Accessibility, w, c)
                        })
                    }
                })
                .on_a11y_action(accesskit::Action::Collapse, move |_, w, c| {
                    a11y_target.update(c, |s, c| {
                        s.request_open(false, PopupOpenReason::Accessibility, w, c)
                    })
                })
        }));
        let mut content = div().relative().flex_1().min_w_0();
        let end = (tokens.show_arrow_padding_inline_end - g.font_size_icon).max(0.);
        if self.show_suffix {
            content = match config.direction {
                Direction::Ltr => content.mr(px(end as f32)),
                Direction::Rtl => content.ml(px(end as f32)),
            };
        }
        if multiple {
            content = content
                .flex()
                .flex_wrap()
                .items_center()
                .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
            let tags = Rc::new(TagSet {
                state: state.clone(),
                values: values.clone(),
                tokens: tokens.clone(),
                styles: self.styles.clone(),
                size,
                variant,
                direction: config.direction,
                disabled,
                max_text: self.max_tag_text_length,
                remove_icon: self.remove_icon.clone(),
                remove_label: config.locale.common.remove_selection.clone(),
                tag_render: self.tag_render.clone(),
                overflow_render: self.overflow_render.clone(),
            });
            let responsive = self.responsive_tags && !values.is_empty();
            if !responsive {
                let count = self.max_tag_count.unwrap_or(values.len()).min(values.len());
                for index in 0..count {
                    content = content.child(tags.tag(index, window, cx));
                }
                if count < values.len() {
                    content = content.child(tags.overflow(count, window, cx));
                }
            }
            // The editing viewport grows only as text grows. Text measurement uses
            // GPUI's text cache, independent of the number of available options.
            let width = if values.is_empty() {
                None
            } else {
                let text = if query.is_empty() {
                    " "
                } else {
                    query.as_ref()
                };
                let run = gpui::TextRun {
                    len: text.len(),
                    font: config.font.clone(),
                    color: metrics.text,
                    background_color: None,
                    underline: None,
                    strikethrough: None,
                };
                Some(
                    window
                        .text_system()
                        .shape_line(
                            text.to_owned().into(),
                            px(metrics.font_size as f32),
                            &[run],
                            None,
                        )
                        .width
                        + px(4.),
                )
            };
            input = input.min_w(px(4.)).max_w_full();
            if let Some(width) = width {
                input = input.w(width);
            } else {
                input = input.w_full();
            }
            let editor = div()
                .flex()
                .items_center()
                .min_w(px(4.))
                .max_w_full()
                .min_h(px(
                    (tokens.tag_height(size) + 2. * (g.padding_xxs / 2.).floor()) as f32,
                ))
                .when(values.is_empty(), |d| d.flex_1())
                .child(input);
            if responsive {
                let overflow = tags.clone();
                content = content.child(super::overflow::ResponsiveTags {
                    count: values.len(),
                    tag: Rc::new(move |i, w, c| tags.tag(i, w, c)),
                    overflow: Rc::new(move |i, w, c| overflow.overflow(i, w, c)),
                    editor: editor.into_any_element(),
                    height: px(
                        (tokens.tag_height(size) + 2. * (g.padding_xxs / 2.).floor()) as f32,
                    ),
                    direction: config.direction,
                });
            } else {
                content = content.child(editor);
            }
        } else {
            content = content.h(px(metrics.line_height as f32)).child(input);
            if query.is_empty()
                && let Some(value) = values.first()
            {
                let label = self.label_render.as_ref().map_or_else(
                    || {
                        div()
                            .truncate()
                            .child(value.label.clone())
                            .into_any_element()
                    },
                    |f| f(value, window, cx),
                );
                let mut overlay = div()
                    .absolute()
                    .inset_0()
                    .overflow_hidden()
                    .opacity(if open { 0.25 } else { 1. })
                    .child(label);
                overlay.style().refine(&self.styles.item_content);
                content = content.child(overlay);
            }
        }
        if values.is_empty() && query.is_empty() && !self.placeholder.is_empty() {
            let mut placeholder = div()
                .absolute()
                .left_0()
                .right_0()
                .top(relative(0.5))
                .mt(px(-metrics.line_height as f32 / 2.))
                .h(px(metrics.line_height as f32))
                .truncate()
                .text_color(metrics.placeholder)
                .child(self.placeholder.clone());
            if multiple {
                let inset = (g.padding_sm
                    - (tokens.height(size) - tokens.tag_height(size)) / 2.
                    - g.line_width)
                    .max(0.);
                placeholder = match config.direction {
                    Direction::Ltr => placeholder.left(px(inset as f32)),
                    Direction::Rtl => placeholder.right(px(inset as f32)),
                };
            }
            placeholder.style().refine(&self.styles.placeholder);
            let semantics = Accessibility {
                hidden: true,
                ..Default::default()
            };
            content = content.child(semantics.apply(placeholder));
        }
        content.style().refine(&self.styles.content);
        root = root.child(content);
        let can_clear = self.allow_clear && !disabled && (!values.is_empty() || !query.is_empty());
        if self.show_suffix {
            let icon_name = if self.loading {
                IconName::LoadingOutlined
            } else if searchable && open {
                IconName::SearchOutlined
            } else {
                IconName::DownOutlined
            };
            let suffix = self.suffix.unwrap_or_else(|| {
                themed_icon(
                    icon_name,
                    g.font_size_icon as f32,
                    g.color_text_quaternary,
                    window,
                )
                .rotation(gpui::radians(if self.loading { rotation } else { 0. }))
                .into_any_element()
            });
            let mut slot = div()
                .flex()
                .items_center()
                .flex_shrink_0()
                .text_size(px(g.font_size_icon as f32))
                .text_color(g.color_text_quaternary)
                .opacity(if can_clear && hovered { 0. } else { 1. })
                .child(suffix);
            slot.style().refine(&self.styles.suffix);
            root = root.child(slot);
        }
        if can_clear {
            let target = state.clone();
            let accessible_target = state.clone();
            let mut clear = div()
                .id("clear")
                .absolute()
                .flex()
                .items_center()
                .justify_center()
                .size(px(g.font_size_icon as f32))
                .text_size(px(g.font_size_icon as f32))
                .text_color(g.color_text_quaternary)
                .hover(|style| style.text_color(g.color_icon))
                .opacity(if hovered { 1. } else { 0. })
                .cursor_pointer()
                .on_mouse_down(MouseButton::Left, move |_, w, c| {
                    if target.read(c).hovered {
                        target.update(c, |s, c| s.clear(w, c));
                        w.prevent_default();
                        c.stop_propagation();
                    }
                })
                .on_click(|_, _, c| c.stop_propagation())
                .on_a11y_action(accesskit::Action::Click, move |_, w, c| {
                    accessible_target.update(c, |s, c| s.clear(w, c));
                })
                .child(self.clear_icon.unwrap_or_else(|| {
                    themed_icon(
                        IconName::CloseCircleFilled,
                        g.font_size_icon as f32,
                        g.color_text_quaternary,
                        window,
                    )
                    .into_any_element()
                }));
            clear = match config.direction {
                Direction::Ltr => clear.right(px(metrics.padding_inline as f32)),
                Direction::Rtl => clear.left(px(metrics.padding_inline as f32)),
            };
            clear.style().refine(&self.styles.clear);
            root = root.child(
                Accessibility::new(Role::Button)
                    .label(config.locale.common.clear_selection.clone())
                    .apply(clear),
            );
        }
        root.style().refine(&self.styles.root);
        let ring = metrics.keyboard_outline.then(|| {
            FocusRing::new(tokens.active_border_color, px(g.line_width as f32))
                .with_offset(px(-g.line_width as f32))
                .radius(px(metrics.radius as f32))
        });
        let root = if let Some(group) = self.group {
            group.set_interaction(disabled, hovered, focused);
            group.apply_style(root.style());
            group.decorate(root, ring, window).into_any_element()
        } else if let Some(ring) = ring {
            ring.apply(root, &focus).into_any_element()
        } else {
            root.into_any_element()
        };
        let mut popup_style = div()
            .p(px(g.padding_xxs as f32))
            .overflow_hidden()
            .style()
            .clone();
        popup_style.refine(&self.styles.popup);
        let rows = Rows {
            list_layout: self.list_layout,
            state: state.clone(),
            tokens: tokens.clone(),
            scope: scope.clone(),
            styles: self.styles.clone(),
            option_render: self.option_render,
            selected_icon: self.selected_icon,
            empty_render: self.empty_render,
            list_height: self.list_height,
            item_height: self.list_item_height.unwrap_or(tokens.option_height as f32),
            virtualized: self.virtualized && self.popup_width != PopupWidth::Content,
            automation_id: self.accessibility.automation_id,
        };
        let popup_render = self.popup_render;
        let popup_state = state.clone();
        let change_state = state.clone();
        Popup::new((self.id, "popup"))
            .scope(scope)
            .child(root)
            .open(open)
            .triggers(PopupTriggers::NONE)
            .arrow(false)
            .width(self.popup_width)
            .gap(px(4.))
            .z_index(tokens.z_index_popup)
            .placement(
                self.placement
                    .unwrap_or(if config.direction == Direction::Ltr {
                        PopupPlacement::BottomLeft
                    } else {
                        PopupPlacement::BottomRight
                    }),
            )
            .surface_style(popup_style)
            .radius(px(g.border_radius_lg as f32))
            .shadows(g.box_shadow_secondary.clone())
            .accessibility_role(Role::Group)
            .fresh(true)
            .on_open_change(move |change, w, c| {
                change_state.update(c, |s, c| s.request_open(change.open, change.reason, w, c))
            })
            .content(move |_, w, c| {
                let list = rows.render(w, c);
                if let Some(render) = &popup_render {
                    render(list, &popup_state, w, c)
                } else {
                    list
                }
            })
    }
}

struct TagSet<T: Clone + Eq + Hash + 'static> {
    state: Entity<SelectState<T>>,
    values: Rc<[SelectValue<T>]>,
    tokens: Arc<SelectTokens>,
    styles: SelectStyles,
    size: Size,
    variant: Variant,
    direction: Direction,
    disabled: bool,
    max_text: Option<usize>,
    remove_icon: Option<ContentRenderer>,
    remove_label: SharedString,
    tag_render: Option<TagRenderer<T>>,
    overflow_render: Option<OverflowRenderer<T>>,
}
impl<T: Clone + Eq + Hash + 'static> TagSet<T> {
    fn tag(&self, index: usize, window: &mut Window, cx: &mut App) -> AnyElement {
        let value = &self.values[index];
        let disabled = self.disabled
            || self
                .state
                .read(cx)
                .model
                .options
                .option(&value.value)
                .is_some_and(|o| o.disabled);
        let context = SelectTagRender {
            id: (
                "selected",
                self.state.read(cx).model.selection_id(&value.value),
            )
                .into(),
            value: value.clone(),
            disabled,
            closable: !disabled,
            state: self.state.downgrade(),
        };
        let tag = if let Some(render) = &self.tag_render {
            render(&context, window, cx)
        } else {
            TagStyle {
                tokens: &self.tokens,
                styles: &self.styles,
                size: self.size,
                variant: self.variant,
                direction: self.direction,
                max_text: self.max_text,
                remove_icon: self.remove_icon.as_ref(),
                remove_label: &self.remove_label,
            }
            .render(&context, window, cx)
        };
        div()
            .id(context.id.clone())
            .flex()
            .max_w_full()
            .child(tag)
            .into_any_element()
    }
    fn overflow(&self, first: usize, window: &mut Window, cx: &mut App) -> AnyElement {
        let g = &self.tokens.globals;
        let content = if let Some(render) = &self.overflow_render {
            render(&self.values[first..], window, cx)
        } else {
            div()
                .child(format!("+ {} ...", self.values.len() - first))
                .into_any_element()
        };
        let mut tag = div()
            .flex()
            .items_center()
            .h(px(self.tokens.tag_height(self.size) as f32))
            .my(px((g.padding_xxs / 2.).floor() as f32))
            .px(px(g.padding_xs as f32))
            .rounded(px(self.tokens.tag_radius(self.size) as f32))
            .bg(self.tokens.multiple_item_bg)
            .child(content);
        tag = match self.direction {
            Direction::Ltr => tag.mr(px(g.padding_xxs as f32)),
            Direction::Rtl => tag.ml(px(g.padding_xxs as f32)),
        };
        tag.style().refine(&self.styles.item);
        div()
            .id(("overflow-layout", first))
            .flex()
            .min_w_0()
            .max_w_full()
            .overflow_hidden()
            .child(tag)
            .into_any_element()
    }
}

struct TagStyle<'a> {
    tokens: &'a SelectTokens,
    styles: &'a SelectStyles,
    size: Size,
    variant: Variant,
    direction: Direction,
    max_text: Option<usize>,
    remove_icon: Option<&'a ContentRenderer>,
    remove_label: &'a str,
}
impl TagStyle<'_> {
    fn render<T: Clone + Eq + Hash + 'static>(
        &self,
        tag: &SelectTagRender<T>,
        window: &mut Window,
        cx: &mut App,
    ) -> AnyElement {
        let Self {
            tokens,
            styles,
            size,
            variant,
            direction,
            max_text,
            remove_icon,
            remove_label,
        } = *self;
        let g = &tokens.globals;
        let label: SharedString = match max_text {
            Some(n) if tag.value.label.chars().count() > n => {
                format!("{}...", tag.value.label.chars().take(n).collect::<String>()).into()
            }
            _ => tag.value.label.clone(),
        };
        let mut text = div().min_w_0().truncate().child(label);
        text.style().refine(&styles.item_content);
        let mut root = div()
            .id("tag")
            .flex()
            .items_center()
            .min_w_0()
            .max_w_full()
            .h(px(tokens.tag_height(size) as f32))
            .my(px((g.padding_xxs / 2.).floor() as f32))
            .border(px(g.line_width as f32))
            .rounded(px(tokens.tag_radius(size) as f32))
            .border_color(if tag.disabled {
                tokens.multiple_item_border_color_disabled
            } else if variant == Variant::Filled {
                g.color_split
            } else {
                tokens.multiple_item_border_color
            })
            .bg(if variant == Variant::Filled {
                g.color_bg_container
            } else {
                tokens.multiple_item_bg
            })
            .when(tag.disabled, |d| {
                d.text_color(tokens.multiple_item_color_disabled)
            })
            .child(text)
            .when(direction == Direction::Rtl, |d| d.flex_row_reverse())
            .on_mouse_down(MouseButton::Left, |_, w, c| {
                w.prevent_default();
                c.stop_propagation();
            })
            .on_click(|_, _, c| c.stop_propagation());
        root = match direction {
            Direction::Ltr => root
                .pl(px(g.padding_xs as f32))
                .pr(px(g.padding_xxs as f32))
                .mr(px(g.padding_xxs as f32)),
            Direction::Rtl => root
                .pr(px(g.padding_xs as f32))
                .pl(px(g.padding_xxs as f32))
                .ml(px(g.padding_xxs as f32)),
        };
        if tag.closable {
            let context = tag.clone();
            let accessible_context = tag.clone();
            let mut close = div()
                .id("remove")
                .flex()
                .items_center()
                .justify_center()
                .text_size(px(10.))
                .text_color(g.color_icon)
                .cursor_pointer()
                .hover(|s| s.text_color(g.color_icon_hover))
                .on_mouse_down(MouseButton::Left, move |_, w, c| {
                    context.close(w, c);
                    w.prevent_default();
                    c.stop_propagation();
                })
                .on_a11y_action(accesskit::Action::Click, move |_, w, c| {
                    accessible_context.close(w, c);
                })
                .child(match remove_icon {
                    None => themed_icon(IconName::CloseOutlined, 10., g.color_icon, window)
                        .into_any_element(),
                    Some(f) => f(window, cx),
                });
            close = match direction {
                Direction::Ltr => close.ml(px(g.padding_xxs as f32)),
                Direction::Rtl => close.mr(px(g.padding_xxs as f32)),
            };
            close.style().refine(&styles.item_remove);
            root = root.child(
                Accessibility::new(Role::Button)
                    .label(remove_label.replace("{label}", &tag.value.label))
                    .apply(close),
            );
        }
        root.style().refine(&styles.item);
        root.into_any_element()
    }
}

struct Rows<T: Clone + Eq + Hash + 'static> {
    list_layout: SelectListLayout,
    state: Entity<SelectState<T>>,
    tokens: Arc<SelectTokens>,
    scope: ConfigScope,
    styles: SelectStyles,
    option_render: Option<OptionRenderer<T>>,
    selected_icon: Option<ContentRenderer>,
    empty_render: Option<ContentRenderer>,
    list_height: f32,
    item_height: f32,
    virtualized: bool,
    automation_id: Option<SharedString>,
}
impl<T: Clone + Eq + Hash + 'static> Clone for Rows<T> {
    fn clone(&self) -> Self {
        Self {
            list_layout: self.list_layout,
            state: self.state.clone(),
            tokens: self.tokens.clone(),
            scope: self.scope.clone(),
            styles: self.styles.clone(),
            option_render: self.option_render.clone(),
            selected_icon: self.selected_icon.clone(),
            empty_render: self.empty_render.clone(),
            list_height: self.list_height,
            item_height: self.item_height,
            virtualized: self.virtualized,
            automation_id: self.automation_id.clone(),
        }
    }
}
impl<T: Clone + Eq + Hash + 'static> Rows<T> {
    fn render(&self, window: &mut Window, cx: &mut App) -> AnyElement {
        let count = self.state.read(cx).model.row_count();
        if count == 0 {
            if let Some(render) = &self.empty_render {
                return render(window, cx);
            }
            let locale = self.scope.snapshot(cx).locale.clone();
            let g = &self.tokens.globals;
            return div()
                .px(px(self.tokens.option_padding_inline as f32))
                .py(px(self.tokens.option_padding_block as f32))
                .child(
                    div()
                        .flex()
                        .flex_col()
                        .items_center()
                        .m(px(g.margin_xs as f32))
                        .text_color(g.color_text_description)
                        .child(super::empty::illustration(g, window))
                        .child(
                            div()
                                .mt(px(g.margin_xs as f32))
                                .child(locale.common.no_data.clone()),
                        ),
                )
                .into_any_element();
        }
        let height = px(self.list_height.min(count as f32 * self.item_height));
        if self.virtualized && self.list_layout == SelectListLayout::Variable {
            let list_state = self.state.update(cx, |s, _| {
                let key = (
                    s.model.filter_revision,
                    self.item_height,
                    Arc::as_ptr(&self.tokens) as usize,
                );
                let list = s.variable_list.get_or_insert_with(|| {
                    gpui::ListState::new(0, gpui::ListAlignment::Top, px(self.item_height))
                });
                if s.variable_key != Some(key) {
                    list.reset_with_uniform_height(count, px(self.item_height));
                    if let Some(active) = s.model.active {
                        list.scroll_to(gpui::ListOffset {
                            item_ix: active,
                            offset_in_item: px(0.),
                        });
                    }
                    s.variable_key = Some(key);
                }
                list.clone()
            });
            let rows = self.clone();
            gpui::list(list_state, move |i, w, c| rows.row(i, w, c))
                .with_sizing_behavior(gpui::ListSizingBehavior::Infer)
                .w_full()
                .max_h(px(self.list_height))
                .into_any_element()
        } else if self.virtualized {
            let rows = self.clone();
            uniform_list("options", count, move |range, w, c| {
                range.map(|i| rows.row(i, w, c)).collect::<Vec<_>>()
            })
            .w_full()
            .h(height)
            .track_scroll(&self.state.read(cx).scroll)
            .into_any_element()
        } else {
            div()
                .id("options")
                .w_full()
                .max_h(px(self.list_height))
                .overflow_y_scroll()
                .children((0..count).map(|i| self.row(i, window, cx)))
                .into_any_element()
        }
    }
    fn row(&self, index: usize, window: &mut Window, cx: &mut App) -> AnyElement {
        self.state.update(cx, |s, _| s.rendered_rows += 1);
        let config = self.scope.snapshot(cx);
        let s = self.state.read(cx);
        let g = &self.tokens.globals;
        let row = s.model.row(index);
        let option = s.model.option(index).cloned();
        let grouped = matches!(row, Some(SelectRow::Option { group: Some(_), .. }));
        let group = match row {
            Some(SelectRow::Group { id, label }) => Some((id.clone(), label.clone())),
            _ => None,
        };
        let selected = option
            .as_ref()
            .is_some_and(|o| s.model.selected.contains(&o.value));
        let previous_selected = index
            .checked_sub(1)
            .and_then(|i| s.model.option(i))
            .is_some_and(|o| s.model.selected.contains(&o.value));
        let next_selected = s
            .model
            .option(index + 1)
            .is_some_and(|o| s.model.selected.contains(&o.value));
        let disabled = !s.model.enabled(index);
        let active = s.model.active == Some(index);
        let multiple = s.model.settings.mode.multiple();
        let mut item = div()
            .flex()
            .items_center()
            .w_full()
            .min_w_0()
            .min_h(px(self.item_height))
            .py(px(self.tokens.option_padding_block as f32))
            .px(px(self.tokens.option_padding_inline as f32))
            .text_size(px(self.tokens.option_font_size as f32))
            .line_height(relative(self.tokens.option_line_height as f32))
            .rounded(px(g.border_radius_sm as f32))
            .when(selected && previous_selected, |d| d.rounded_t_none())
            .when(selected && next_selected, |d| d.rounded_b_none())
            .when(config.direction == Direction::Rtl, |d| d.flex_row_reverse());
        if self.virtualized && self.list_layout == SelectListLayout::Uniform {
            item = item.h(px(self.item_height)).overflow_hidden();
        }
        if let Some((id, label)) = group {
            return item
                .id(id)
                .text_size(px(g.font_size_sm as f32))
                .text_color(g.color_text_description)
                .child(label)
                .into_any_element();
        }
        let Some(option) = option else {
            return item.into_any_element();
        };
        if grouped {
            item = match config.direction {
                Direction::Ltr => item.pl(px((2. * g.control_padding_horizontal) as f32)),
                Direction::Rtl => item.pr(px((2. * g.control_padding_horizontal) as f32)),
            };
        }
        let background = if disabled && selected {
            g.color_bg_container_disabled
        } else if disabled {
            crate::theme::Color::TRANSPARENT
        } else if selected && active {
            g.control_item_bg_active_hover
        } else if selected {
            self.tokens.option_selected_bg
        } else if active {
            self.tokens.option_active_bg
        } else {
            crate::theme::Color::TRANSPARENT
        };
        let context = SelectOptionRender {
            option: option.clone(),
            index,
            selected,
            active,
            disabled,
        };
        let mut content =
            div()
                .flex_1()
                .min_w_0()
                .truncate()
                .child(if let Some(render) = &self.option_render {
                    render(&context, window, cx)
                } else {
                    option.label.clone().into_any_element()
                });
        content.style().refine(&self.styles.option_content);
        item = item
            .bg(background)
            .text_color(if disabled {
                g.color_text_disabled
            } else if selected {
                self.tokens.option_selected_color
            } else {
                g.color_text
            })
            .when(selected && !disabled, |d| {
                d.font_weight(gpui::FontWeight(self.tokens.option_selected_font_weight))
            })
            .child(content);
        if selected && multiple {
            item = item.child(
                div().flex_shrink_0().text_color(g.color_primary).child(
                    match self.selected_icon.as_ref() {
                        None => themed_icon(
                            IconName::CheckOutlined,
                            g.font_size as f32,
                            g.color_primary,
                            window,
                        )
                        .into_any_element(),
                        Some(f) => f(window, cx),
                    },
                ),
            );
        }
        item.style().refine(&self.styles.option);
        let target = self.state.clone();
        let hover_target = self.state.clone();
        let option_id = option.id.clone();
        let hover_id = option.id.clone();
        let item = item
            .id(option.id.clone())
            .when(disabled, |d| d.cursor_not_allowed())
            .when(!disabled, |d| d.cursor_pointer())
            .on_mouse_down(MouseButton::Left, move |_, w, c| {
                target.update(c, |s, c| {
                    if s.model.option(index).is_some_and(|o| o.id == option_id) {
                        s.choose(index, SelectChangeSource::Option, w, c);
                        s.focus(w, c);
                    }
                });
                w.prevent_default();
                c.stop_propagation();
            })
            .on_hover(move |hovered, w, c| {
                if *hovered {
                    hover_target.update(c, |s, c| {
                        if s.model.option(index).is_some_and(|o| o.id == hover_id) {
                            s.activate(index, w, c);
                        }
                    });
                }
            });
        let mut a11y = Accessibility::new(Role::ListBoxOption).label(option.label);
        a11y.disabled = disabled;
        // The logical options under the editor provide the accessible list.
        // Hiding the painted copies avoids duplicate options and focus owners.
        a11y.hidden = true;
        if let Some(id) = &self.automation_id {
            a11y.automation_id = Some(format!("{id}.option.{index}").into());
        }
        a11y.apply(item).into_any_element()
    }
}
