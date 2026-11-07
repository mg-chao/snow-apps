// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, Input, InputState, UiRoot};
use gpui::{
    AppContext, Context, Entity, Focusable, IntoElement, Render, ScrollHandle, SharedString,
    TestAppContext, Window, WindowHandle, div, prelude::*, px,
};

struct ScrollingForm {
    scope: ConfigScope,
    form: Entity<FormState>,
    fields: Vec<(FormField<SharedString>, Entity<InputState>)>,
    scroll: ScrollHandle,
    tall: bool,
    nested: bool,
    horizontal: bool,
}
impl Render for ScrollingForm {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let viewport = div()
            .id("form-scroll-container")
            .w(px(440.))
            .h(px(200.))
            .overflow_y_scroll()
            .when(self.horizontal, |viewport| viewport.overflow_x_scroll())
            .track_scroll(&self.scroll)
            .child(
                Form::new("scrolling-form", &self.form)
                    .scope(self.scope.clone())
                    .scroll_handle(&self.scroll)
                    .w_full()
                    .when(self.horizontal, |form| form.w(px(900.)))
                    .items(
                        self.fields
                            .iter()
                            .enumerate()
                            .map(|(index, (field, editor))| {
                                let editor = editor.clone();
                                FormItem::new(index)
                                    .label(format!("Field {index}"))
                                    .when(self.tall && index == 19, |item| item.h(px(420.)))
                                    .when(self.horizontal && index == 19, |item| {
                                        item.w(px(300.)).ml(px(500.))
                                    })
                                    .control(field, move |context| Input::new(context.id, &editor))
                            }),
                    ),
            );
        div()
            .when(self.nested, |parent| parent.p(px(20.)))
            .child(viewport)
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    form: Entity<FormState>,
    target: FormField<SharedString>,
    target_editor: Entity<InputState>,
    first_editor: Entity<InputState>,
    view: Entity<ScrollingForm>,
    scroll: ScrollHandle,
}
impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut retained = None;
        let mut retained_view = None;
        let window = cx.add_window(|window, cx| {
            crate::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let form = cx.new(|cx| FormState::new(window, cx));
            let fields: Vec<_> = (0..20)
                .map(|index| {
                    let field = form.update(cx, |form, cx| {
                        let options = if index == 19 {
                            FieldOptions::default().rule(FormRule::required())
                        } else {
                            FieldOptions::default()
                        };
                        form.register(format!("field-{index}"), FormCodec::text(), options, cx)
                            .unwrap()
                    });
                    (field, cx.new(|cx| InputState::new(window, cx)))
                })
                .collect();
            let scroll = ScrollHandle::new();
            retained = Some((
                form.clone(),
                fields[19].0.clone(),
                fields[19].1.clone(),
                fields[0].1.clone(),
                scroll.clone(),
            ));
            let view = cx.new(|_| ScrollingForm {
                scope: scope.clone(),
                form,
                fields,
                scroll,
                tall: false,
                nested: false,
                horizontal: false,
            });
            retained_view = Some(view.clone());
            UiRoot::new(scope, view, window, cx)
        });
        let (form, target, target_editor, first_editor, scroll) = retained.unwrap();
        let mut fixture = Self {
            cx,
            window,
            form,
            target,
            target_editor,
            first_editor,
            view: retained_view.unwrap(),
            scroll,
        };
        fixture.draw();
        fixture
    }
    fn draw(&mut self) {
        for _ in 0..4 {
            self.cx.run_until_parked();
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    window.simulate_next_frame(cx);
                    window.draw(cx).clear(cx);
                })
                .unwrap();
        }
        self.cx.run_until_parked();
    }
    fn assert_target_visible(&mut self) {
        assert!(
            self.scroll.offset().y < px(-500.),
            "the actual parent must scroll"
        );
        self.cx.update(|cx| {
            let bounds = self.target.handle().bounds(cx).unwrap();
            let viewport = self.scroll.bounds();
            assert!(
                bounds.top() >= viewport.top() - px(1.),
                "target starts above viewport: {bounds:?}"
            );
            assert!(
                bounds.bottom() <= viewport.bottom() + px(1.),
                "target ends below viewport: {bounds:?}"
            );
        });
    }
    fn reveal(&mut self, focus: bool) {
        let form = self.form.clone();
        self.cx
            .update_window(self.window.into(), |_, window, cx| {
                form.update(cx, |form, cx| {
                    assert!(form.scroll_to_field("field-19", focus, window, cx));
                });
            })
            .unwrap();
        self.draw();
    }
}

#[test]
fn form_scroll_to_field_reaches_a_cached_offscreen_native_editor_and_focuses_it() {
    let mut f = Fixture::new();
    assert_eq!(f.scroll.offset().y, px(0.));
    let form = f.form.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        form.update(cx, |form, cx| {
            assert!(!form.scroll_to_field("missing", true, window, cx));
            assert!(form.scroll_to_field("field-19", true, window, cx));
        });
    })
    .unwrap();
    f.draw();
    f.assert_target_visible();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert!(f.target_editor.read(cx).focus_handle(cx).is_focused(window));
    })
    .unwrap();
}

#[test]
fn form_failed_submit_scrolls_to_the_first_error_and_focuses_the_native_control() {
    let mut f = Fixture::new();
    let form = f.form.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        form.update(cx, |form, cx| {
            form.set_scroll_to_first_error(true, true);
            form.submit(window, cx);
        });
    })
    .unwrap();
    f.draw();
    f.assert_target_visible();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert_eq!(f.target.handle().meta(cx).errors.len(), 1);
        assert!(
            f.target_editor.read(cx).focus_handle(cx).is_focused(window),
            "failed submit must focus its real editor"
        );
    })
    .unwrap();
}

#[test]
fn form_scroll_without_focus_preserves_the_exact_previously_focused_native_editor() {
    let mut f = Fixture::new();
    let first = f.first_editor.clone();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        first.read(cx).focus_handle(cx).focus(window, cx);
    })
    .unwrap();
    f.draw();
    f.reveal(false);
    f.assert_target_visible();
    f.cx.update_window(f.window.into(), |_, window, cx| {
        assert!(first.read(cx).focus_handle(cx).is_focused(window));
        assert!(!f.target_editor.read(cx).focus_handle(cx).is_focused(window));
    })
    .unwrap();
}

#[test]
fn form_scroll_aligns_tall_fields_to_the_start_of_a_nested_native_viewport() {
    let mut f = Fixture::new();
    f.view.update(&mut f.cx, |view, cx| {
        view.tall = true;
        view.nested = true;
        cx.notify();
    });
    f.draw();
    f.reveal(false);
    f.cx.update(|cx| {
        let bounds = f.target.handle().bounds(cx).unwrap();
        let viewport = f.scroll.bounds();
        assert_eq!(viewport.top(), px(20.));
        assert_eq!(bounds.size.height, px(420.));
        assert!((f32::from(bounds.top() - viewport.top())).abs() <= 1.);
    });
    assert_eq!(
        f.cx.update_window(f.window.into(), |_, window, cx| window
            .simulate_next_frame(cx))
            .unwrap(),
        0
    );
}

#[test]
fn form_scroll_reveals_both_axes_using_the_actual_native_owner_bounds() {
    let mut f = Fixture::new();
    f.view.update(&mut f.cx, |view, cx| {
        view.horizontal = true;
        view.nested = true;
        cx.notify();
    });
    f.draw();
    f.reveal(false);
    f.assert_target_visible();
    assert!(f.scroll.offset().x < px(-100.));
    f.cx.update(|cx| {
        let bounds = f.target.handle().bounds(cx).unwrap();
        let viewport = f.scroll.bounds();
        assert!(bounds.left() >= viewport.left() - px(1.));
        assert!((f32::from(bounds.right() - viewport.right())).abs() <= 1.);
    });
}

#[test]
fn form_cached_fields_read_a_replaced_native_scroll_owner_without_stale_handles() {
    let mut f = Fixture::new();
    let previous = f.scroll.clone();
    let next = ScrollHandle::new();
    f.view.update(&mut f.cx, |view, cx| {
        view.scroll = next.clone();
        cx.notify();
    });
    f.scroll = next;
    f.draw();
    f.reveal(false);
    f.assert_target_visible();
    assert_eq!(previous.offset().y, px(0.));
}
