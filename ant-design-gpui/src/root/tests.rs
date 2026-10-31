// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

use gpui::{
    AppContext, Context, Entity, FocusHandle, Focusable, IntoElement, KeyDownEvent, KeyUpEvent,
    Keystroke, PlatformInput, Render, Role, StyleRefinement, Subscription, TestAppContext, Window,
    WindowHandle, div, prelude::*, px,
};

use crate::{
    ConfigPatch, ConfigScope, UiRoot,
    behavior::{Accessibility, ActivationGuard, Availability, focus_handle},
    locale::{Locale, LocaleId},
};

struct Consumer {
    scope: ConfigScope,
    handles: [FocusHandle; 2],
    guards: [ActivationGuard; 2],
    visible: [bool; 2],
    first_disabled: bool,
    render_count: Rc<Cell<usize>>,
    rendered_locale: Rc<RefCell<LocaleId>>,
    _scope_subscription: Subscription,
    _blur_subscriptions: [Subscription; 2],
}

impl Consumer {
    fn new(scope: ConfigScope, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let handles = [focus_handle(cx), focus_handle(cx)];
        let guards = [ActivationGuard::default(), ActivationGuard::default()];
        let blur_subscriptions = [
            guards[0].observe_blur(&handles[0], window, cx),
            guards[1].observe_blur(&handles[1], window, cx),
        ];
        Self {
            _scope_subscription: scope.observe(cx),
            _blur_subscriptions: blur_subscriptions,
            scope,
            handles,
            guards,
            visible: [true, true],
            first_disabled: false,
            render_count: Rc::new(Cell::new(0)),
            rendered_locale: Rc::new(RefCell::new(LocaleId::EnUs)),
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        self.render_count.set(self.render_count.get() + 1);
        *self.rendered_locale.borrow_mut() = config.locale.id.clone();
        let mut content = div().id("consumer").flex().size_full();
        for index in 0..2 {
            if !self.visible[index] {
                continue;
            }
            let availability = Availability {
                disabled: index == 0 && self.first_disabled,
                busy: false,
            };
            self.guards[index].update(availability, &self.handles[index], window, cx);
            let mut control = div().id(index).w(px(80.)).h(px(40.));
            if availability.can_focus() {
                control = control.track_focus(&self.handles[index]);
            }
            let control = self.guards[index].install(control, |_, _, _| {});
            content = content.child(
                Accessibility {
                    disabled: availability.disabled,
                    ..Accessibility::new(Role::Button)
                }
                .label(config.locale.common.close.clone())
                .apply(control),
            );
        }
        content
    }
}

struct Panel(Entity<Consumer>);

impl Render for Panel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        // Caching makes the subscription contract observable: an unrelated root draw
        // must not refresh this consumer unless its own scope or control state changes.
        div().size_full().child(
            self.0
                .clone()
                .cached(StyleRefinement::default().w(px(240.)).h(px(64.))),
        )
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    child_scope: ConfigScope,
}

impl Fixture {
    fn new(visible: [bool; 2]) -> Self {
        let mut cx = TestAppContext::single();
        let mut created_consumer = None;
        let mut created_scope = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            let child_scope = scope.child(ConfigPatch::default(), cx).unwrap();
            let consumer = cx.new(|cx| {
                let mut consumer = Consumer::new(child_scope.clone(), window, cx);
                consumer.visible = visible;
                consumer
            });
            let panel = cx.new(|_| Panel(consumer.clone()));
            created_consumer = Some(consumer);
            created_scope = Some(child_scope);
            UiRoot::new(scope, panel, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            consumer: created_consumer.unwrap(),
            child_scope: created_scope.unwrap(),
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

    fn key(&mut self, stroke: &'static str) {
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                let keystroke = Keystroke::parse(stroke).unwrap();
                window.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: keystroke.clone(),
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
                window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke }), cx);
            })
            .unwrap();
        self.draw();
    }

    fn root_focused(&mut self) -> bool {
        self.cx
            .update_window(self.window.into(), |root, window, cx| {
                root.downcast::<UiRoot>()
                    .unwrap()
                    .read(cx)
                    .focus_handle(cx)
                    .is_focused(window)
            })
            .unwrap()
    }

    fn control_focused(&mut self, index: usize) -> bool {
        let control = self
            .consumer
            .read_with(&self.cx, |consumer, _| consumer.handles[index].clone());
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                control.is_focused(window)
            })
            .unwrap()
    }

    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            edit(consumer);
            cx.notify();
        });
        self.draw();
    }
}

#[test]
fn new_ui_root_receives_initial_tab_and_reverse_traversal() {
    let mut fixture = Fixture::new([true, true]);
    assert!(fixture.root_focused());
    fixture.key("tab");
    assert!(fixture.control_focused(0));
    fixture.key("tab");
    assert!(fixture.control_focused(1));
    fixture.key("shift-tab");
    assert!(fixture.control_focused(0));
}

#[test]
fn disabling_the_focused_control_restores_root_and_skips_it_on_tab() {
    let mut fixture = Fixture::new([true, true]);
    fixture.key("tab");
    assert!(fixture.control_focused(0));
    fixture.edit(|consumer| consumer.first_disabled = true);
    assert!(fixture.root_focused());
    fixture.key("tab");
    assert!(fixture.control_focused(1));
}

#[test]
fn removing_a_focused_child_restores_root_even_while_its_handle_is_retained() {
    let mut fixture = Fixture::new([true, true]);
    fixture.key("tab");
    assert!(fixture.control_focused(0));
    fixture.edit(|consumer| consumer.visible[0] = false);
    assert!(fixture.root_focused());
    fixture.key("tab");
    assert!(fixture.control_focused(1));
}

#[test]
fn empty_roots_survive_last_child_removal_and_resume_navigation_when_children_return() {
    let mut fixture = Fixture::new([false, false]);
    assert!(fixture.root_focused());
    fixture.key("tab");
    assert!(fixture.root_focused());
    fixture.edit(|consumer| consumer.visible[1] = true);
    fixture.key("tab");
    assert!(fixture.control_focused(1));
    fixture.edit(|consumer| consumer.visible[1] = false);
    assert!(fixture.root_focused());
    fixture.key("shift-tab");
    assert!(fixture.root_focused());
    fixture.edit(|consumer| consumer.visible[0] = true);
    fixture.key("tab");
    assert!(fixture.control_focused(0));
}

#[test]
fn a_local_scope_update_invalidates_its_subscribed_cached_consumer() {
    let mut fixture = Fixture::new([true, true]);
    let (count, locale) = fixture.consumer.read_with(&fixture.cx, |consumer, _| {
        (
            consumer.render_count.clone(),
            consumer.rendered_locale.clone(),
        )
    });
    let initial_count = count.get();
    fixture.draw();
    assert_eq!(
        count.get(),
        initial_count,
        "unchanged cached consumer should be reused"
    );
    let scope = fixture.child_scope.clone();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                patch.locale.locale = Some(Locale::zh_tw());
            })
            .unwrap();
    });
    fixture.draw();
    assert!(
        count.get() > initial_count,
        "local scope subscription must invalidate its consumer"
    );
    assert_eq!(*locale.borrow(), LocaleId::ZhTw);
    let updated_count = count.get();
    fixture.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                patch.locale.locale = Some(Locale::zh_tw());
            })
            .unwrap();
    });
    fixture.draw();
    assert_eq!(
        count.get(),
        updated_count,
        "no-op scope updates preserve the rendered cache"
    );
}
