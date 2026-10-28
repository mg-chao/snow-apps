// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
use super::*;
use crate::{Button, ConfigScope, UiRoot};
use gpui::{
    AppContext, Context, Entity, FocusHandle, IntoElement, KeyDownEvent, Keystroke, MouseButton,
    MouseDownEvent, MouseUpEvent, PlatformInput, Point, Render, TestAppContext, WindowHandle, div,
    point, prelude::*, px,
};
use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

struct Consumer {
    state: Entity<ModalState>,
    scope: ConfigScope,
    trigger: FocusHandle,
    visible: bool,
    background_scroll: Option<Entity<crate::ScrollAreaState>>,
    background_clicks: Rc<Cell<usize>>,
}
impl Render for Consumer {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let clicks = self.background_clicks.clone();
        let background = div()
            .flex()
            .flex_col()
            .w_full()
            .child(
                Button::new("trigger")
                    .scope(self.scope.clone())
                    .label("Open")
                    .focus_handle(&self.trigger)
                    .on_click(move |_, _, _| clicks.set(clicks.get() + 1)),
            )
            .when(self.background_scroll.is_some(), |d| {
                d.child(div().h(px(1600.)).flex_none())
            });
        let background = if let Some(scroll) = &self.background_scroll {
            crate::ScrollArea::new("background-scroll", scroll)
                .scope(self.scope.clone())
                .size_full()
                .child(background)
                .into_any_element()
        } else {
            background.into_any_element()
        };
        div().size_full().child(background).when(self.visible, |d| {
            d.child(Modal::new("modal", &self.state, cx).scope(self.scope.clone()))
        })
    }
}
struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    state: Entity<ModalState>,
    consumer: Entity<Consumer>,
    scope: ConfigScope,
    events: Rc<RefCell<Vec<ModalEvent>>>,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut data = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let calls = events.clone();
        let window = cx.add_window(|w, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let state = cx.new(|cx| {
                let mut s = ModalState::new(w, cx);
                s.set_scope(scope.clone(), cx);
                s.options.title = Some(ModalContent::text("Basic Modal"));
                s.options.content = Some(ModalContent::text("Some contents..."));
                s.options.on_event =
                    Some(Rc::new(move |e, _, _| calls.borrow_mut().push(e.clone())));
                s
            });
            let consumer = cx.new(|cx| Consumer {
                state: state.clone(),
                scope: scope.clone(),
                trigger: crate::behavior::focus_handle(cx),
                visible: true,
                background_scroll: None,
                background_clicks: Rc::default(),
            });
            data = Some((scope.clone(), state, consumer.clone()));
            UiRoot::new(scope, consumer, w, cx)
        });
        let (scope, state, consumer) = data.unwrap();
        let mut result = Self {
            cx,
            window,
            state,
            consumer,
            scope,
            events,
        };
        result.draw();
        result
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx
            .update_window(self.window.into(), |_, w, cx| w.draw(cx).clear(cx))
            .unwrap();
        self.cx.run_until_parked();
    }
    fn open(&mut self, open: bool) {
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                self.state.update(cx, |s, cx| s.set_open(open, w, cx))
            })
            .unwrap();
        self.draw();
    }
    fn edit(&mut self, f: impl FnOnce(&mut ModalOptions)) {
        self.state.update(&mut self.cx, |s, cx| {
            let mut o = s.options.clone();
            f(&mut o);
            s.set_options(o, cx);
        });
        self.draw();
    }
    fn is_open(&self) -> bool {
        self.state.read_with(&self.cx, |s, _| s.is_open())
    }
    fn activate(&mut self, reason: Option<ModalCancelReason>) {
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                state::activate(&self.state, reason, w, cx)
            })
            .unwrap();
        self.draw();
    }
    fn pointer(&mut self, down: bool, position: Point<gpui::Pixels>) {
        let input = if down {
            PlatformInput::MouseDown(MouseDownEvent {
                button: MouseButton::Left,
                position,
                ..Default::default()
            })
        } else {
            PlatformInput::MouseUp(MouseUpEvent {
                button: MouseButton::Left,
                position,
                ..Default::default()
            })
        };
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                w.dispatch_event(input, cx);
            })
            .unwrap();
        self.draw();
    }
    fn key(&mut self, key: &'static str) {
        let key = Keystroke::parse(key).unwrap();
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                w.dispatch_event(
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke: key,
                        is_held: false,
                        prefer_character_input: false,
                    }),
                    cx,
                );
            })
            .unwrap();
        self.draw();
    }
}

#[test]
fn modal_owner_decides_closure_and_completion_events_are_once() {
    let mut f = Fixture::new();
    f.open(true);
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.phase()), ModalPhase::Open);
    f.activate(None);
    assert!(f.is_open());
    f.activate(Some(ModalCancelReason::Button));
    assert!(f.is_open());
    f.open(false);
    f.draw();
    f.draw();
    assert_eq!(
        &*f.events.borrow(),
        &[
            ModalEvent::AfterOpenChange(true),
            ModalEvent::OkRequested,
            ModalEvent::CancelRequested(ModalCancelReason::Button),
            ModalEvent::AfterOpenChange(false),
            ModalEvent::AfterClose
        ]
    );
}
#[test]
fn modal_footer_aligns_to_logical_end_in_both_directions() {
    for kind in [ModalKind::Standard, ModalKind::Confirm] {
        for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
            let mut f = Fixture::new();
            f.cx.update(|cx| {
                f.scope
                    .update_patch(cx, |p| p.direction = Some(direction))
                    .unwrap();
            });
            f.edit(|o| o.kind = kind);
            f.open(true);
            f.cx.update_window(f.window.into(), |_, window, cx| {
                let config = f.scope.snapshot(cx);
                let primary = config.theme.tokens.color_primary.to_hsla();
                let body = f.state.read(cx).bounds();
                let button = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| quad.background.as_solid() == Some(primary))
                    .expect("native primary Modal action");
                let scale = window.scale_factor();
                let error = if direction == crate::Direction::Ltr {
                    (button.bounds.origin.x.0 + button.bounds.size.width.0) / scale
                        - f32::from(body.right())
                        + 24.
                } else {
                    button.bounds.origin.x.0 / scale - f32::from(body.left()) - 24.
                };
                assert!(error.abs() < 1., "{kind:?}, {direction:?}: {error}");
            })
            .unwrap();
        }
    }
}

#[test]
fn modal_helper_icons_use_status_colors_and_reference_frames() {
    for kind in [
        ModalKind::Confirm,
        ModalKind::Info,
        ModalKind::Success,
        ModalKind::Warning,
        ModalKind::Error,
    ] {
        let mut f = Fixture::new();
        f.edit(|o| o.kind = kind);
        f.open(true);
        f.cx.update_window(f.window.into(), |_, w, cx| {
            let config = f.scope.snapshot(cx);
            let g = &config.theme.tokens;
            let color = match kind {
                ModalKind::Info => g.color_info,
                ModalKind::Success => g.color_success,
                ModalKind::Error => g.color_error,
                _ => g.color_warning,
            }
            .to_rgba();
            let sprite = w
                .rendered_scene()
                .monochrome_sprites
                .iter()
                .find(|s| {
                    let actual = s.color.to_rgb();
                    [actual.r - color.r, actual.g - color.g, actual.b - color.b]
                        .iter()
                        .all(|delta| delta.abs() < 0.001)
                })
                .expect("helper icon must use its semantic status color");
            assert_eq!(
                sprite.transformation.rotation_scale,
                [[8. / 7., 0.], [0., 8. / 7.]]
            );
        })
        .unwrap();
    }
}

#[test]
fn modal_helper_title_style_affects_native_layout() {
    let mut f = Fixture::new();
    f.edit(|o| o.kind = ModalKind::Confirm);
    f.open(true);
    let before = f.state.read_with(&f.cx, |s, _| s.bounds().size.height);
    f.edit(|o| o.styles.title = gpui::StyleRefinement::default().text_size(px(40.)));
    let after = f.state.read_with(&f.cx, |s, _| s.bounds().size.height);
    assert!(
        after > before,
        "semantic title styling must participate in helper layout"
    );
}

#[test]
fn modal_reference_geometry_and_idle_frames() {
    let mut f = Fixture::new();
    f.open(true);
    f.draw();
    let bounds = f.state.read_with(&f.cx, |s, _| s.bounds());
    assert_eq!(bounds.size.width, px(520.));
    assert_eq!(bounds.origin.y, px(100.));
    assert!(bounds.size.height > px(100.));
    let frames =
        f.cx.update_window(f.window.into(), |_, w, cx| w.simulate_next_frame(cx))
            .unwrap();
    assert_eq!(frames, 0);
}
#[test]
fn modal_loading_blocks_cancel_and_duplicate_ok_requests() {
    let mut f = Fixture::new();
    f.open(true);
    f.state
        .update(&mut f.cx, |s, cx| s.set_confirm_loading(true, cx));
    f.draw();
    f.activate(None);
    for reason in [
        ModalCancelReason::Button,
        ModalCancelReason::Mask,
        ModalCancelReason::Escape,
        ModalCancelReason::Close,
    ] {
        f.activate(Some(reason));
    }
    assert_eq!(&*f.events.borrow(), &[ModalEvent::AfterOpenChange(true)]);
    f.state
        .update(&mut f.cx, |s, cx| s.set_confirm_loading(false, cx));
    f.activate(None);
    assert_eq!(f.events.borrow().last(), Some(&ModalEvent::OkRequested));
}
#[test]
fn modal_mask_requires_press_and_release_outside() {
    let mut f = Fixture::new();
    f.open(true);
    let inside = f.state.read_with(&f.cx, |s, _| s.bounds().center());
    let outside = point(px(4.), px(4.));
    f.pointer(true, inside);
    f.pointer(false, outside);
    assert_eq!(f.events.borrow().len(), 1);
    f.pointer(true, outside);
    f.pointer(false, outside);
    assert_eq!(
        f.events.borrow().last(),
        Some(&ModalEvent::CancelRequested(ModalCancelReason::Mask))
    );
}
#[test]
fn modal_escape_and_focus_traversal_stay_in_dialog() {
    let mut f = Fixture::new();
    f.open(true);
    for _ in 0..8 {
        f.key("tab");
        f.cx.update_window(f.window.into(), |_, w, cx| {
            assert!(f.state.read(cx).focus.contains_focused(w, cx))
        })
        .unwrap();
    }
    for _ in 0..8 {
        f.key("shift-tab");
        f.cx.update_window(f.window.into(), |_, w, cx| {
            assert!(f.state.read(cx).focus.contains_focused(w, cx))
        })
        .unwrap();
    }
    f.key("escape");
    assert_eq!(
        f.events.borrow().last(),
        Some(&ModalEvent::CancelRequested(ModalCancelReason::Escape))
    );
}
#[test]
fn modal_lazy_content_retained_then_destroyed_only_when_requested() {
    struct Body;
    impl Render for Body {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().child("Retained")
        }
    }
    let mut f = Fixture::new();
    let calls = Rc::new(Cell::new(0));
    let count = calls.clone();
    f.edit(|o| {
        o.content = Some(ModalContent::view(move |_, cx| {
            count.set(count.get() + 1);
            cx.new(|_| Body).into()
        }))
    });
    assert_eq!(calls.get(), 0);
    f.edit(|o| o.force_render = true);
    assert_eq!(
        calls.get(),
        1,
        "force_render initializes a closed native view"
    );
    assert!(!f.is_open());
    f.edit(|o| o.force_render = false);
    f.open(true);
    f.draw();
    assert_eq!(calls.get(), 1);
    f.open(false);
    f.open(true);
    assert_eq!(calls.get(), 1);
    f.edit(|o| o.destroy_on_hidden = true);
    f.open(false);
    f.open(true);
    assert_eq!(calls.get(), 2);
}
#[test]
fn modal_helpers_preserve_context_and_return_outcomes() {
    let mut f = Fixture::new();
    let handle =
        f.cx.update_window(f.window.into(), |_, w, cx| {
            let manager = ModalManager::new(f.scope.clone(), w);
            manager.confirm(
                ModalOptions {
                    title: Some(ModalContent::text("Confirm")),
                    ..Default::default()
                },
                w,
                cx,
            )
        })
        .unwrap();
    f.draw();
    let entity = handle.state().unwrap();
    assert_eq!(
        entity.read_with(&f.cx, |s, _| s.bounds().size.width),
        px(416.)
    );
    let mut result = f.cx.update(|cx| handle.result(cx));
    f.cx.update_window(f.window.into(), |_, w, cx| handle.confirm(w, cx))
        .unwrap();
    f.draw();
    assert!(!f.cx.update(|cx| handle.is_open(cx)));
    assert_eq!(
        std::future::Future::poll(
            std::pin::Pin::new(&mut result),
            &mut std::task::Context::from_waker(std::task::Waker::noop())
        ),
        std::task::Poll::Ready(ModalOutcome::Confirmed)
    );
}
#[test]
fn modal_unmount_releases_owned_content_and_blocks_old_requests() {
    let mut f = Fixture::new();
    f.open(true);
    f.consumer.update(&mut f.cx, |s, cx| {
        s.visible = false;
        cx.notify();
    });
    f.draw();
    f.draw();
    assert!(!f.is_open());
    let count = f.events.borrow().len();
    f.activate(None);
    assert_eq!(f.events.borrow().len(), count);
}
#[test]
fn modal_responsive_width_and_catalogs() {
    let g = crate::theme::ThemeConfig::default().resolve().tokens;
    let responsive = ModalWidth::Responsive([
        Some(ModalLength::Fraction(0.9)),
        Some(ModalLength::Pixels(480.)),
        None,
        Some(ModalLength::Pixels(900.)),
        None,
        None,
    ]);
    assert_eq!(
        style::width(Some(&responsive), ModalKind::Standard, 320., &g),
        288.
    );
    assert_eq!(
        style::width(Some(&responsive), ModalKind::Standard, 800., &g),
        480.
    );
    assert_eq!(
        style::width(Some(&responsive), ModalKind::Standard, 1200., &g),
        900.
    );
    assert_eq!(
        crate::locale::Locale::zh_cn().modal.just_ok_text.as_ref(),
        "知道了"
    );
    assert_eq!(
        crate::locale::Locale::zh_tw().modal.ok_text.as_ref(),
        "確定"
    );
}

#[test]
fn modal_async_failure_retry_and_stale_completion() {
    let mut f = Fixture::new();
    let calls = Rc::new(Cell::new(0));
    let count = calls.clone();
    let fail = Rc::new(Cell::new(true));
    let reject = fail.clone();
    f.edit(|o| {
        o.on_ok = Some(Rc::new(move |_, _, cx| {
            count.set(count.get() + 1);
            let fail = reject.get();
            let timer = cx
                .background_executor()
                .timer(std::time::Duration::from_millis(100));
            ModalAction::Pending(cx.spawn(async move |_| {
                timer.await;
                if fail { Err("Retry".into()) } else { Ok(()) }
            }))
        }))
    });
    f.open(true);
    f.activate(None);
    f.activate(None);
    assert_eq!(calls.get(), 1);
    assert!(f.state.read_with(&f.cx, |s, _| s.is_pending()));
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(100));
    f.draw();
    assert!(f.is_open());
    assert!(!f.state.read_with(&f.cx, |s, _| s.is_pending()));
    assert_eq!(
        f.events.borrow().last(),
        Some(&ModalEvent::ActionFailed("Retry".into()))
    );
    fail.set(false);
    f.activate(None);
    f.open(false);
    f.open(true);
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(100));
    f.draw();
    assert!(
        f.is_open(),
        "earlier completion must not close the reopened dialog"
    );
    f.activate(None);
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(100));
    f.draw();
    assert!(!f.is_open());
    assert_eq!(calls.get(), 3);
}

#[test]
fn modal_disabled_cancel_button_does_not_disable_escape_or_close() {
    let mut f = Fixture::new();
    f.edit(|o| o.cancel_button.disabled = Some(true));
    f.open(true);
    f.activate(Some(ModalCancelReason::Button));
    assert_eq!(f.events.borrow().len(), 1);
    f.key("escape");
    assert_eq!(
        f.events.borrow().last(),
        Some(&ModalEvent::CancelRequested(ModalCancelReason::Escape))
    );
    f.activate(Some(ModalCancelReason::Close));
    assert_eq!(
        f.events.borrow().last(),
        Some(&ModalEvent::CancelRequested(ModalCancelReason::Close))
    );
}

#[test]
fn modal_nested_helper_returns_focus_and_destroy_all_is_manager_local() {
    let mut f = Fixture::new();
    f.open(true);
    let (first, second, a, b) =
        f.cx.update_window(f.window.into(), |_, w, cx| {
            let a = ModalManager::new(f.scope.clone(), w);
            let b = ModalManager::new(f.scope.clone(), w);
            let first = a.confirm(ModalOptions::default(), w, cx);
            let second = b.info(ModalOptions::default(), w, cx);
            (first, second, a, b)
        })
        .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, cx| {
        assert_eq!(
            host::top(w, cx).unwrap().entity_id(),
            second.state().unwrap().entity_id()
        );
        b.destroy_all(w, cx);
    })
    .unwrap();
    f.draw();
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, cx| {
        assert!(first.is_open(cx));
        assert!(!second.is_open(cx));
        assert_eq!(
            host::top(w, cx).unwrap().entity_id(),
            first.state().unwrap().entity_id()
        );
        assert!(
            first
                .state()
                .unwrap()
                .read(cx)
                .focus
                .contains_focused(w, cx)
        );
        a.destroy_all(w, cx);
    })
    .unwrap();
    f.draw();
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, cx| {
        assert_eq!(host::top(w, cx).unwrap().entity_id(), f.state.entity_id());
        assert!(f.state.read(cx).focus.contains_focused(w, cx));
    })
    .unwrap();
}

#[test]
fn modal_child_popup_receives_escape_before_dialog() {
    let mut f = Fixture::new();
    let popup = crate::PopupHandle::new();
    let popup_copy = popup.clone();
    let scope = f.scope.clone();
    f.edit(|o| {
        o.content = Some(ModalContent::render(move |_, _, _| {
            crate::Popover::new("nested-popup")
                .scope(scope.clone())
                .handle(&popup_copy)
                .title("Details")
                .content(|_, _, _| div().child("More information"))
                .child(
                    Button::new("nested-trigger")
                        .scope(scope.clone())
                        .label("Details"),
                )
        }))
    });
    f.open(true);
    f.cx.update_window(f.window.into(), |_, w, cx| popup.set_open(true, w, cx))
        .unwrap();
    f.draw();
    f.key("escape");
    assert!(
        f.events
            .borrow()
            .iter()
            .all(|e| !matches!(e, ModalEvent::CancelRequested(_)))
    );
    f.key("escape");
    assert_eq!(
        f.events.borrow().last(),
        Some(&ModalEvent::CancelRequested(ModalCancelReason::Escape))
    );
}

#[test]
fn modal_completion_callback_can_reopen_without_retiring_new_opening() {
    let mut f = Fixture::new();
    let weak = f.state.downgrade();
    f.edit(|o| {
        o.on_event = Some(Rc::new(move |e, w, cx| {
            if *e == ModalEvent::AfterClose {
                let _ = weak.update(cx, |s, cx| s.set_open(true, w, cx));
            }
        }))
    });
    f.open(true);
    f.open(false);
    f.draw();
    f.draw();
    assert!(f.is_open());
    f.cx.update_window(f.window.into(), |_, w, cx| {
        assert_eq!(
            host::top(w, cx).map(|e| e.entity_id()),
            Some(f.state.entity_id())
        )
    })
    .unwrap();
}

#[test]
fn modal_result_survives_entity_release() {
    let mut f = Fixture::new();
    let handle =
        f.cx.update_window(f.window.into(), |_, w, cx| {
            ModalManager::new(f.scope.clone(), w).info(ModalOptions::default(), w, cx)
        })
        .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, cx| handle.confirm(w, cx))
        .unwrap();
    f.draw();
    f.draw();
    assert!(
        handle.state().is_none(),
        "host must release the closed managed controller"
    );
    let mut result = f.cx.update(|cx| handle.result(cx));
    assert_eq!(
        std::future::Future::poll(
            std::pin::Pin::new(&mut result),
            &mut std::task::Context::from_waker(std::task::Waker::noop())
        ),
        std::task::Poll::Ready(ModalOutcome::Confirmed)
    );
}

#[test]
fn modal_closing_surface_is_inert_and_reopening_preserves_motion() {
    let mut f = Fixture::new();
    f.cx.update(|cx| f.scope.update_patch(cx, |p| p.motion = Some(true)).unwrap());
    let calls = Rc::new(Cell::new(0));
    let click_calls = calls.clone();
    f.edit(|o| {
        o.content = Some(ModalContent::render(move |_, _, _| {
            let click_calls = click_calls.clone();
            div().debug_selector(|| "modal-custom-action".into()).child(
                Button::new("custom-action")
                    .label("Action")
                    .on_click(move |_, _, _| click_calls.set(click_calls.get() + 1)),
            )
        }))
    });
    f.open(true);
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(400));
    f.cx.update_window(f.window.into(), |_, w, cx| w.simulate_next_frame(cx))
        .unwrap();
    f.draw();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.phase()), ModalPhase::Open);
    let mut visual = gpui::VisualTestContext::from_window(f.window.into(), &f.cx);
    let bounds = visual.debug_bounds("modal-custom-action").unwrap();
    f.open(false);
    assert_eq!(
        f.state.read_with(&f.cx, |s, _| s.phase()),
        ModalPhase::Closing
    );
    f.pointer(true, bounds.center());
    f.pointer(false, bounds.center());
    assert_eq!(calls.get(), 0);
    f.open(true);
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(400));
    f.cx.update_window(f.window.into(), |_, w, cx| w.simulate_next_frame(cx))
        .unwrap();
    f.draw();
    assert_eq!(f.state.read_with(&f.cx, |s, _| s.phase()), ModalPhase::Open);
    assert!(!f.events.borrow().contains(&ModalEvent::AfterClose));
}

#[test]
fn modal_scoped_button_options_merge_and_explicit_false_overrides() {
    let mut f = Fixture::new();
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |p| {
                p.modal.ok_button = Some(ModalButtonOptions {
                    disabled: Some(true),
                    danger: Some(true),
                    ..Default::default()
                });
            })
            .unwrap()
    });
    f.open(true);
    f.activate(None);
    assert!(!f.events.borrow().contains(&ModalEvent::OkRequested));
    f.edit(|o| o.ok_button.disabled = Some(false));
    f.activate(None);
    assert_eq!(f.events.borrow().last(), Some(&ModalEvent::OkRequested));
}

#[test]
fn modal_reentrant_callbacks_are_guarded_before_dispatch() {
    let mut f = Fixture::new();
    let calls = Rc::new(Cell::new(0));
    let count = calls.clone();
    f.edit(|o| {
        o.on_ok = Some(Rc::new(move |handle, w, cx| {
            count.set(count.get() + 1);
            handle.confirm(w, cx);
            ModalAction::KeepOpen
        }))
    });
    f.open(true);
    f.activate(None);
    assert_eq!(calls.get(), 1);
    f.activate(None);
    assert_eq!(calls.get(), 2);
}

#[test]
fn modal_pending_actions_complete_independently_and_survive_close_in_callback() {
    let mut f = Fixture::new();
    let finished = Rc::new(Cell::new(false));
    let done = finished.clone();
    f.edit(|o| {
        o.on_ok = Some(Rc::new(move |h, w, cx| {
            h.close(w, cx);
            let timer = cx
                .background_executor()
                .timer(std::time::Duration::from_millis(100));
            let done = done.clone();
            ModalAction::Pending(cx.spawn(async move |_| {
                timer.await;
                done.set(true);
                Ok(())
            }))
        }))
    });
    f.open(true);
    f.activate(None);
    f.open(true);
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(100));
    f.draw();
    assert!(finished.get());
    assert!(f.is_open());
    f.edit(|o| {
        o.on_ok = Some(Rc::new(|_, _, cx| {
            let timer = cx
                .background_executor()
                .timer(std::time::Duration::from_millis(200));
            ModalAction::Pending(cx.spawn(async move |_| {
                timer.await;
                Err("OK rejected".into())
            }))
        }));
        o.on_cancel = Some(Rc::new(|_, _, cx| {
            let timer = cx
                .background_executor()
                .timer(std::time::Duration::from_millis(100));
            ModalAction::Pending(cx.spawn(async move |_| {
                timer.await;
                Err("Cancel rejected".into())
            }))
        }));
    });
    f.activate(None);
    f.activate(Some(ModalCancelReason::Button));
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(100));
    f.draw();
    assert!(f.state.read_with(&f.cx, |s, _| s.pending_ok));
    assert!(!f.state.read_with(&f.cx, |s, _| s.pending_cancel));
}

#[test]
fn modal_destroy_all_does_not_destroy_another_managers_later_dialog() {
    let mut f = Fixture::new();
    let (a, first, second) =
        f.cx.update_window(f.window.into(), |_, w, cx| {
            let a = ModalManager::new(f.scope.clone(), w);
            let b = ModalManager::new(f.scope.clone(), w);
            let first = a.confirm(ModalOptions::default(), w, cx);
            let second = b.confirm(ModalOptions::default(), w, cx);
            (a, first, second)
        })
        .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, cx| a.destroy_all(w, cx))
        .unwrap();
    f.draw();
    f.draw();
    f.cx.update(|cx| {
        assert!(!first.is_open(cx));
        assert!(second.is_open(cx));
    });
}

#[test]
fn modal_scroll_lock_and_background_pointer_isolation_are_independent() {
    let mut f = Fixture::new();
    let scroll =
        f.cx.update_window(f.window.into(), |_, w, cx| {
            cx.new(|cx| crate::ScrollAreaState::new(w, cx))
        })
        .unwrap();
    f.consumer.update(&mut f.cx, |s, cx| {
        s.background_scroll = Some(scroll.clone());
        cx.notify();
    });
    f.draw();
    f.open(true);
    let wheel = |f: &mut Fixture| {
        f.cx.update_window(f.window.into(), |_, w, cx| {
            w.dispatch_event(
                PlatformInput::Scroll(gpui::ScrollEvent {
                    position: point(px(10.), px(50.)),
                    delta: gpui::ScrollDelta::Pixels(point(px(0.), px(-100.))),
                    ..Default::default()
                }),
                cx,
            );
        })
        .unwrap();
        f.draw();
    };
    wheel(&mut f);
    assert_eq!(scroll.read_with(&f.cx, |s, _| s.offset().y), px(0.));
    f.edit(|o| o.scroll_lock = false);
    wheel(&mut f);
    assert!(
        scroll.read_with(&f.cx, |s, _| s.offset().y) < px(0.),
        "unlocked wheel must reach the background scroller"
    );
    f.pointer(true, point(px(10.), px(16.)));
    f.pointer(false, point(px(10.), px(16.)));
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |s, _| s.background_clicks.get()),
        0
    );
}

#[test]
fn modal_retains_all_view_slots_and_loading_does_not_construct_body() {
    struct Body;
    impl Render for Body {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().child("View")
        }
    }
    let mut f = Fixture::new();
    let counters = [
        Rc::new(Cell::new(0)),
        Rc::new(Cell::new(0)),
        Rc::new(Cell::new(0)),
    ];
    let content = |count: Rc<Cell<usize>>| {
        ModalContent::view(move |_, cx| {
            count.set(count.get() + 1);
            cx.new(|_| Body).into()
        })
    };
    f.edit(|o| {
        o.title = Some(content(counters[0].clone()));
        o.content = Some(content(counters[1].clone()));
        o.footer = ModalFooter::Content(content(counters[2].clone()));
        o.loading = true;
    });
    f.open(true);
    assert_eq!(counters.each_ref().map(|c| c.get()), [1, 0, 0]);
    f.edit(|o| o.loading = false);
    f.draw();
    assert_eq!(counters.each_ref().map(|c| c.get()), [1, 1, 1]);
    f.open(false);
    f.open(true);
    assert_eq!(counters.each_ref().map(|c| c.get()), [1, 1, 1]);
    f.edit(|o| o.destroy_on_hidden = true);
    f.open(false);
    f.open(true);
    assert_eq!(counters.each_ref().map(|c| c.get()), [2, 2, 2]);
}

#[test]
fn modal_skeleton_animation_stops_when_loading_or_motion_stops() {
    let mut f = Fixture::new();
    f.cx.update(|cx| f.scope.update_patch(cx, |p| p.motion = Some(true)).unwrap());
    f.edit(|o| o.loading = true);
    f.open(true);
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(400));
    let frame = |f: &mut Fixture| {
        f.cx.update_window(f.window.into(), |_, w, cx| w.simulate_next_frame(cx))
            .unwrap()
    };
    assert!(frame(&mut f) > 0);
    f.draw();
    assert!(frame(&mut f) > 0);
    f.draw();
    f.edit(|o| o.loading = false);
    frame(&mut f);
    f.draw();
    assert_eq!(frame(&mut f), 0);
    f.edit(|o| o.loading = true);
    f.cx.update(|cx| {
        f.scope
            .update_patch(cx, |p| p.motion = Some(false))
            .unwrap()
    });
    frame(&mut f);
    f.draw();
    assert_eq!(frame(&mut f), 0);
}

#[test]
fn modal_parent_retirement_closes_nested_declarative_dialog_and_popup() {
    let mut f = Fixture::new();
    let child =
        f.cx.update_window(f.window.into(), |_, w, cx| {
            cx.new(|cx| {
                let mut s = ModalState::new(w, cx);
                s.set_scope(f.scope.clone(), cx);
                s
            })
        })
        .unwrap();
    let child_view = child.clone();
    f.edit(|o| {
        o.content = Some(ModalContent::render(move |_, _, cx| {
            Modal::new("child", &child_view, cx)
        }))
    });
    f.open(true);
    f.cx.update_window(f.window.into(), |_, w, cx| {
        child.update(cx, |s, cx| s.set_open(true, w, cx))
    })
    .unwrap();
    f.draw();
    f.open(false);
    f.draw();
    assert!(!child.read_with(&f.cx, |s, _| s.is_open()));
    f.open(true);
    f.cx.update_window(f.window.into(), |_, w, cx| {
        child.update(cx, |s, cx| s.set_open(true, w, cx))
    })
    .unwrap();
    f.draw();
    f.consumer.update(&mut f.cx, |s, cx| {
        s.visible = false;
        cx.notify();
    });
    f.draw();
    f.draw();
    assert!(!child.read_with(&f.cx, |s, _| s.is_open()));
}

#[test]
fn modal_empty_footer_and_disappearing_focus_fall_back_inside_dialog() {
    let mut f = Fixture::new();
    f.edit(|o| {
        o.footer = ModalFooter::None;
        o.auto_focus = ModalAutoFocus::Ok;
        o.closable = Some(false);
    });
    f.open(true);
    for key in ["tab", "shift-tab"] {
        f.key(key);
        f.cx.update_window(f.window.into(), |_, w, cx| {
            assert!(f.state.read(cx).focus.contains_focused(w, cx))
        })
        .unwrap();
    }
    f.edit(|o| o.footer = ModalFooter::Default);
    f.key("tab");
    f.edit(|o| {
        o.ok_button.disabled = Some(true);
        o.cancel_button.disabled = Some(true);
    });
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, cx| {
        assert!(f.state.read(cx).focus.contains_focused(w, cx))
    })
    .unwrap();
    f.cx.update_window(f.window.into(), |_, w, cx| {
        let focus = f.consumer.read(cx).trigger.clone();
        w.focus(&focus, cx);
    })
    .unwrap();
    f.key("enter");
    assert_eq!(
        f.consumer
            .read_with(&f.cx, |s, _| s.background_clicks.get()),
        0
    );
}

#[test]
fn modal_windows_keep_hosts_managers_and_results_isolated() {
    let mut f = Fixture::new();
    f.open(true);
    struct Empty;
    impl Render for Empty {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            div().size_full()
        }
    }
    let mut scope = None;
    let second = f.cx.add_window(|w, cx| {
        let config = ConfigScope::new_window(cx);
        config.update_patch(cx, |p| p.motion = Some(false)).unwrap();
        scope = Some(config.clone());
        let empty = cx.new(|_| Empty);
        UiRoot::new(config, empty, w, cx)
    });
    f.cx.update_window(second.into(), |_, w, cx| {
        w.draw(cx).clear(cx);
        assert!(host::top(w, cx).is_none());
    })
    .unwrap();
    let handle =
        f.cx.update_window(second.into(), |_, w, cx| {
            ModalManager::new(scope.unwrap(), w).confirm(ModalOptions::default(), w, cx)
        })
        .unwrap();
    f.cx.update_window(second.into(), |_, w, cx| w.draw(cx).clear(cx))
        .unwrap();
    f.cx.run_until_parked();
    f.open(false);
    f.cx.update(|cx| assert!(handle.is_open(cx)));
    let mut result = f.cx.update(|cx| handle.result(cx));
    f.cx.update_window(second.into(), |_, w, _| w.remove_window())
        .unwrap();
    f.cx.run_until_parked();
    assert_eq!(
        std::future::Future::poll(
            std::pin::Pin::new(&mut result),
            &mut std::task::Context::from_waker(std::task::Waker::noop())
        ),
        std::task::Poll::Ready(ModalOutcome::Destroyed)
    );
}
