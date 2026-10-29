// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

use gpui::{
    App, AppContext, Background, Bounds, Context, Element, Entity, EntityInputHandler, FocusHandle,
    Focusable, IntoElement, KeyDownEvent, KeyUpEvent, Keystroke, MouseButton, MouseDownEvent,
    MouseUpEvent, Pixels, PlatformInput, Point, Render, RenderOnce, Role, StyleRefinement,
    TestAppContext, Window, WindowHandle, accesskit, div, point, prelude::*, px, rgb, size,
};

use super::*;
use crate::{Button, ConfigPatch, Input, InputState, UiRoot, behavior::focus_handle};

const EDITOR: u32 = 0x129a74;
const HELP: u32 = 0x8532b1;
const SAVE: u32 = 0xa47319;

#[derive(IntoElement)]
struct KeyedEditor {
    state: Rc<RefCell<Option<Entity<InputState>>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
    builds: Rc<Cell<usize>>,
    scope: ConfigScope,
}

impl RenderOnce for KeyedEditor {
    fn render(self, window: &mut Window, cx: &mut App) -> impl IntoElement {
        self.builds.set(self.builds.get() + 1);
        let state = window.use_keyed_state("description-editor-state", cx, InputState::new);
        *self.state.borrow_mut() = Some(state.clone());
        let mut element = Input::new("description-editor", &state)
            .scope(self.scope)
            .automation_id("descriptions.editor")
            .accessibility_label("Account name")
            .w(px(120.))
            .render(window, cx)
            .into_any_element();
        // Input erases its optional native focus-ring adapter into AnyElement.
        // Inspect that actual semantic element, rather than AnyElement's
        // deliberately anonymous delegation layer.
        type InputSemantics = crate::behavior::SemanticElement<gpui::Stateful<gpui::Div>>;
        type Ring = crate::behavior::FocusRingElement<InputSemantics>;
        fn metadata(element: &impl Element) -> accesskit::Node {
            let mut node = accesskit::Node::new(element.a11y_role().expect("native Input role"));
            element.write_a11y_info(&mut node);
            node
        }
        let node = if let Some(input) = element.downcast_mut::<Ring>() {
            metadata(input)
        } else if let Some(input) = element.downcast_mut::<InputSemantics>() {
            metadata(input)
        } else {
            panic!("unrecognized native Input semantic adapter");
        };
        *self.semantics.borrow_mut() = node;
        div().w(px(120.)).bg(rgb(EDITOR)).child(element)
    }
}

struct Probe {
    scope: ConfigScope,
    order: Vec<usize>,
    visible: bool,
    bordered: bool,
    layout: DescriptionsLayout,
    renders: usize,
    editor: Rc<RefCell<Option<Entity<InputState>>>>,
    editor_semantics: Rc<RefCell<accesskit::Node>>,
    editor_builds: Rc<Cell<usize>>,
    context: Rc<RefCell<Option<DescriptionsStyleContext>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
    help_focus: FocusHandle,
    save_focus: FocusHandle,
    activations: Rc<RefCell<Vec<&'static str>>>,
}

impl Render for Probe {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.renders += 1;
        if !self.visible {
            return div().w_full();
        }
        let context = self.context.clone();
        let mut details = Descriptions::new("interactive-descriptions")
            .scope(self.scope.clone())
            .title("Account details")
            .accessibility_description("Review and edit account details")
            .automation_id("descriptions.account")
            .columns(DescriptionsBreakpoints {
                xs: Some(1),
                sm: Some(2),
                md: Some(3),
                ..Default::default()
            })
            .bordered(self.bordered)
            .layout(self.layout)
            .styles_fn(move |value, _, _| {
                *context.borrow_mut() = Some(*value);
                DescriptionsStyles::default()
            });
        for &index in &self.order {
            details = details.item(match index {
                0 => DescriptionItem::new("name")
                    .label("Name")
                    .content_element(KeyedEditor {
                        state: self.editor.clone(),
                        semantics: self.editor_semantics.clone(),
                        builds: self.editor_builds.clone(),
                        scope: self.scope.clone(),
                    }),
                1 => {
                    let activations = self.activations.clone();
                    DescriptionItem::new("help")
                        .accessibility_label("Account help")
                        .label_content(
                            div().w(px(72.)).bg(rgb(HELP)).child(
                                Button::new("description-help")
                                    .scope(self.scope.clone())
                                    .label("Help")
                                    .w(px(72.))
                                    .wave(false)
                                    .focus_handle(&self.help_focus)
                                    .on_click(move |_, _, _| activations.borrow_mut().push("help")),
                            ),
                        )
                        .content("Account help")
                }
                _ => {
                    let activations = self.activations.clone();
                    DescriptionItem::new("save")
                        .label("Actions")
                        .content_element(
                            div().w(px(72.)).bg(rgb(SAVE)).child(
                                Button::new("description-save")
                                    .scope(self.scope.clone())
                                    .label("Save")
                                    .w(px(72.))
                                    .wave(false)
                                    .focus_handle(&self.save_focus)
                                    .on_click(move |_, _, _| activations.borrow_mut().push("save")),
                            ),
                        )
                }
            });
        }
        let element = details.render(window, cx).into_element();
        let mut node = accesskit::Node::new(element.a11y_role().unwrap());
        element.write_a11y_info(&mut node);
        *self.semantics.borrow_mut() = node;
        div().w_full().p(px(31.)).child(element)
    }
}

struct CachedPanel(Entity<Probe>);
impl Render for CachedPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.0
            .clone()
            .cached(StyleRefinement::default().w_full().h_full())
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe>,
    scope: ConfigScope,
}

impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut probe = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let root = ConfigScope::new_window(cx);
            let local = root
                .child(
                    ConfigPatch {
                        motion: Some(false),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let entity = cx.new(|cx| Probe {
                scope: local.clone(),
                order: vec![0, 1, 2],
                visible: true,
                bordered: false,
                layout: DescriptionsLayout::Horizontal,
                renders: 0,
                editor: Rc::default(),
                editor_semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::TextInput))),
                editor_builds: Rc::default(),
                context: Rc::default(),
                semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::DescriptionList))),
                help_focus: focus_handle(cx),
                save_focus: focus_handle(cx),
                activations: Rc::default(),
            });
            let panel = cx.new(|_| CachedPanel(entity.clone()));
            probe = Some(entity);
            scope = Some(local);
            UiRoot::new(root, panel, window, cx)
        });
        cx.simulate_window_resize(window.into(), size(px(1280.), px(720.)));
        let mut fixture = Self {
            cx,
            window,
            probe: probe.unwrap(),
            scope: scope.unwrap(),
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

    fn edit(&mut self, change: impl FnOnce(&mut Probe)) {
        self.probe.update(&mut self.cx, |probe, cx| {
            change(probe);
            cx.notify();
        });
        self.draw();
    }

    fn editor(&self) -> Entity<InputState> {
        self.probe
            .read_with(&self.cx, |probe, _| probe.editor.borrow().clone().unwrap())
    }

    fn rect(&mut self, color: u32) -> Bounds<Pixels> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                let background: Background = rgb(color).into();
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| quad.background == background)
                    .map(|quad| {
                        Bounds::new(
                            point(
                                px(quad.bounds.origin.x.0 / scale),
                                px(quad.bounds.origin.y.0 / scale),
                            ),
                            size(
                                px(quad.bounds.size.width.0 / scale),
                                px(quad.bounds.size.height.0 / scale),
                            ),
                        )
                    })
                    .unwrap_or_else(|| panic!("missing marker {color:06x}"))
            })
            .unwrap()
    }

    fn click(&mut self, position: Point<Pixels>) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::MouseDown(MouseDownEvent {
                        button: MouseButton::Left,
                        position,
                        click_count: 1,
                        ..Default::default()
                    }),
                    cx,
                );
                window.dispatch_event(
                    PlatformInput::MouseUp(MouseUpEvent {
                        button: MouseButton::Left,
                        position,
                        click_count: 1,
                        ..Default::default()
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }

    fn key(&mut self, key: &'static str) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: Keystroke::parse(key).unwrap(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
                window.dispatch_event(
                    PlatformInput::KeyUp(KeyUpEvent {
                        keystroke: Keystroke::parse(key).unwrap(),
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }

    fn focused(&mut self, focus: &FocusHandle) -> bool {
        self.cx
            .update_window(self.window.into(), |_, window, _| focus.is_focused(window))
            .unwrap()
    }

    fn renders(&self) -> usize {
        self.probe.read_with(&self.cx, |probe, _| probe.renders)
    }
    fn activations(&self) -> Vec<&'static str> {
        self.probe
            .read_with(&self.cx, |probe, _| probe.activations.borrow().clone())
    }
    fn context(&self) -> DescriptionsStyleContext {
        self.probe
            .read_with(&self.cx, |probe, _| probe.context.borrow().unwrap())
    }
}

#[test]
fn description_list_metadata_preserves_interactive_rich_descendants() {
    let mut f = Fixture::new();
    let node = f
        .probe
        .read_with(&f.cx, |probe, _| probe.semantics.borrow().clone());
    assert_eq!(node.role(), Role::DescriptionList);
    assert_eq!(node.label(), Some("Account details"));
    assert_eq!(node.description(), Some("Review and edit account details"));
    assert_eq!(node.author_id(), Some("descriptions.account"));
    assert!(!node.supports_action(accesskit::Action::Click));
    assert!(!node.supports_action(accesskit::Action::Focus));
    let input = f
        .probe
        .read_with(&f.cx, |probe, _| probe.editor_semantics.borrow().clone());
    assert_eq!(input.role(), Role::TextInput);
    assert_eq!(input.label(), Some("Account name"));
    assert!(input.supports_action(accesskit::Action::Focus));
    assert!(input.supports_action(accesskit::Action::SetValue));
    let button = f.rect(SAVE);
    f.click(button.center());
    assert_eq!(f.activations(), ["save"]);
}

#[test]
fn measured_rich_hitboxes_use_final_parent_coordinates_in_all_layouts() {
    for layout in [DescriptionsLayout::Horizontal, DescriptionsLayout::Vertical] {
        for bordered in [false, true] {
            let mut f = Fixture::new();
            f.edit(|probe| {
                probe.layout = layout;
                probe.bordered = bordered;
            });
            f.click(point(px(4.), px(4.)));
            assert!(
                f.activations().is_empty(),
                "intrinsic measurement must not register stale origin hitboxes"
            );
            let help = f.rect(HELP);
            let save = f.rect(SAVE);
            assert!(help.left() >= px(31.) && help.top() >= px(31.));
            assert!(save.left() >= px(31.) && save.top() >= px(31.));
            f.click(help.center());
            f.click(save.center());
            assert_eq!(
                f.activations(),
                ["help", "save"],
                "{layout:?} bordered={bordered}"
            );
        }
    }
}

#[test]
fn keyed_editor_value_selection_and_focus_survive_reorder_and_responsive_repacking() {
    let mut f = Fixture::new();
    let state = f.editor();
    let id = state.entity_id();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        state.update(cx, |state, cx| {
            state.focus(window, cx);
            state.replace_text_in_range(None, "Edited account", window, cx);
        })
    })
    .unwrap();
    f.draw();
    let focus = state.read_with(&f.cx, |state, cx| state.focus_handle(cx));
    let selection = state.read_with(&f.cx, |state, _| state.selection());
    let initial = f.rect(EDITOR);
    f.edit(|probe| probe.order.reverse());
    assert_eq!(f.editor().entity_id(), id);
    assert_ne!(f.rect(EDITOR).origin, initial.origin);
    assert!(f.focused(&focus));
    f.cx.simulate_window_resize(f.window.into(), size(px(520.), px(720.)));
    f.draw();
    assert_eq!(f.context().columns, 1);
    assert_eq!(f.editor().entity_id(), id);
    assert!(f.focused(&focus));
    assert_eq!(
        state.read_with(&f.cx, |state, _| state.value().to_owned()),
        "Edited account"
    );
    assert_eq!(
        state.read_with(&f.cx, |state, _| state.selection()),
        selection
    );
    f.key("ctrl-a");
    f.cx.update_window(f.window.into(), |_, window, cx| {
        state.update(cx, |state, cx| {
            state.replace_text_in_range(None, "Changed after reflow", window, cx);
        })
    })
    .unwrap();
    f.draw();
    assert_eq!(
        state.read_with(&f.cx, |state, _| state.value().to_owned()),
        "Changed after reflow"
    );
}

#[test]
fn rich_controls_keep_native_tab_order_and_keyboard_activation_after_repacking() {
    let mut f = Fixture::new();
    let editor = f.editor();
    let editor_focus = editor.read_with(&f.cx, |state, cx| state.focus_handle(cx));
    let (help, save) = f.probe.read_with(&f.cx, |probe, _| {
        (probe.help_focus.clone(), probe.save_focus.clone())
    });
    f.key("tab");
    assert!(f.focused(&editor_focus));
    f.key("tab");
    assert!(f.focused(&help));
    f.key("enter");
    assert_eq!(f.activations(), ["help"]);
    f.key("tab");
    assert!(f.focused(&save));
    f.cx.simulate_window_resize(f.window.into(), size(px(520.), px(720.)));
    f.draw();
    assert!(f.focused(&save));
    f.key("shift-tab");
    assert!(f.focused(&help));
    f.key("space");
    assert_eq!(f.activations(), ["help", "help"]);
}

#[test]
fn cached_parent_observes_active_scope_and_releases_replaced_and_unmounted_scopes() {
    let mut f = Fixture::new();
    let original = f.scope.clone();
    let replacement = f.cx.update(|cx| {
        ConfigScope::application(cx)
            .child(
                ConfigPatch {
                    direction: Some(Direction::Rtl),
                    motion: Some(false),
                    ..Default::default()
                },
                cx,
            )
            .unwrap()
    });
    f.edit(|probe| probe.scope = replacement.clone());
    assert_eq!(f.context().direction, Direction::Rtl);
    let count = f.renders();
    f.cx.update(|cx| {
        original
            .update_patch(cx, |patch| patch.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    assert_eq!(f.renders(), count);
    f.cx.update(|cx| {
        replacement
            .update_patch(cx, |patch| patch.size = Some(Size::Small))
            .unwrap()
    });
    f.draw();
    assert_eq!(f.context().size, Size::Small);
    assert!(f.renders() > count);
    let count = f.renders();
    assert!(!f.cx.update(|cx| {
        replacement
            .update_patch(cx, |patch| patch.size = Some(Size::Small))
            .unwrap()
    }));
    f.draw();
    assert_eq!(f.renders(), count);
    f.edit(|probe| probe.visible = false);
    let count = f.renders();
    f.cx.update(|cx| {
        replacement
            .update_patch(cx, |patch| patch.direction = Some(Direction::Ltr))
            .unwrap()
    });
    f.draw();
    assert_eq!(
        f.renders(),
        count,
        "unmounted component and rich children release their scope observers"
    );
}
