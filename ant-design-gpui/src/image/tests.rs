// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{ConfigScope, UiRoot};
use gpui::{
    AppContext, Context, Entity, Focusable, IntoElement, KeyDownEvent, KeyUpEvent, Keystroke,
    MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, PinchEvent, PlatformInput, Point,
    Render, ScrollDelta, ScrollEvent, ScrollSource, TestAppContext, TouchEvent, TouchId,
    TouchPhase, WindowHandle, div, point, prelude::*, px, size,
};
use std::{cell::RefCell, rc::Rc};

struct Page {
    image: Entity<ImageState>,
    preview: Entity<PreviewState>,
    scope: ConfigScope,
    enabled: bool,
    mounted: bool,
}
impl Render for Page {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div().size_full().when(self.mounted, |d| {
            d.child(
                PreviewGroup::new("collection", &self.preview)
                    .scope(self.scope.clone())
                    .child(
                        Image::new("thumbnail", &self.image, cx)
                            .scope(self.scope.clone())
                            .preview_group(&self.preview)
                            .preview(self.enabled)
                            .alt("Test landscape")
                            .automation_id("image.test.thumbnail")
                            .w(px(200.))
                            .h(px(120.)),
                    ),
            )
        })
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    page: Entity<Page>,
    image: Entity<ImageState>,
    preview: Entity<PreviewState>,
    events: Rc<RefCell<Vec<PreviewEvent>>>,
}
impl Fixture {
    fn new(count: usize) -> Self {
        Self::create(count, true)
    }
    fn inherited(count: usize) -> Self {
        Self::create(count, false)
    }
    fn create(count: usize, explicit_options: bool) -> Self {
        let mut cx = TestAppContext::single();
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let mut created = None;
        let window = cx.open_window(size(px(1000.), px(800.)), |w, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let source = picture(cx, 1200, 800, "#386db8");
            let items = (0..count)
                .map(|i| {
                    PreviewItem::new(
                        format!("item-{i}"),
                        if i == 0 {
                            source.clone()
                        } else {
                            picture(cx, 400 + i as u32 * 20, 300, "#af6d32")
                        },
                    )
                    .alt(format!("Picture {}", i + 1))
                })
                .collect();
            let preview = cx.new(|cx| {
                let mut s = PreviewState::new(items, w, cx);
                s.set_scope(scope.clone(), cx);
                if explicit_options {
                    s.set_options(
                        PreviewOptions {
                            on_event: Some(Rc::new(move |event, _, _| {
                                output.borrow_mut().push(event.clone())
                            })),
                            ..Default::default()
                        },
                        cx,
                    );
                }
                s
            });
            let image = cx.new(|cx| ImageState::new(source, w, cx));
            let page = cx.new(|_| Page {
                image: image.clone(),
                preview: preview.clone(),
                scope: scope.clone(),
                enabled: true,
                mounted: true,
            });
            created = Some((page.clone(), image, preview));
            UiRoot::new(scope, page, w, cx)
        });
        let (page, image, preview) = created.unwrap();
        let mut result = Self {
            cx,
            window,
            page,
            image,
            preview,
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
    fn run(&mut self, f: impl FnOnce(&Entity<PreviewState>, &mut Window, &mut App)) {
        let state = self.preview.clone();
        self.cx
            .update_window(self.window.into(), |_, w, cx| f(&state, w, cx))
            .unwrap();
        self.draw();
    }
    fn open(&mut self, current: usize) {
        self.run(|state, w, cx| state.update(cx, |s, cx| s.request_open(current, w, cx)));
    }
    fn action(&mut self, action: PreviewAction) {
        self.run(|state, w, cx| state.update(cx, |s, cx| s.apply_action(action, w, cx)));
    }
    fn is_open(&self) -> bool {
        self.preview.read_with(&self.cx, |s, _| s.is_open())
    }
    fn transform(&self) -> PreviewTransform {
        self.preview.read_with(&self.cx, |s, _| s.transform())
    }
    fn click(&mut self, position: Point<gpui::Pixels>) {
        for down in [true, false] {
            let input = if down {
                PlatformInput::MouseDown(MouseDownEvent {
                    button: MouseButton::Left,
                    position,
                    click_count: 1,
                    ..Default::default()
                })
            } else {
                PlatformInput::MouseUp(MouseUpEvent {
                    button: MouseButton::Left,
                    position,
                    click_count: 1,
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
    }
    fn key(&mut self, key: &'static str, release: bool) {
        let keystroke = Keystroke::parse(key).unwrap();
        let input = if release {
            PlatformInput::KeyUp(KeyUpEvent { keystroke })
        } else {
            PlatformInput::KeyDown(KeyDownEvent {
                keystroke,
                is_held: false,
                prefer_character_input: false,
            })
        };
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                w.dispatch_event(input, cx);
            })
            .unwrap();
        self.draw();
    }
    fn options(&mut self, edit: impl FnOnce(&mut PreviewOptions)) {
        self.preview.update(&mut self.cx, |s, cx| {
            let mut options = s.options().clone();
            edit(&mut options);
            s.set_options(options, cx);
        });
        self.draw();
    }
    fn event(&mut self, event: PlatformInput) {
        self.cx
            .update_window(self.window.into(), |_, w, cx| {
                w.dispatch_event(event, cx);
            })
            .unwrap();
        self.draw();
    }
    fn pinch(&mut self, phase: TouchPhase, delta: f32, position: Point<gpui::Pixels>) {
        self.event(PlatformInput::Pinch(PinchEvent {
            phase,
            delta,
            position,
            ..Default::default()
        }));
    }
    fn touch_scroll(
        &mut self,
        phase: TouchPhase,
        delta: Point<gpui::Pixels>,
        position: Point<gpui::Pixels>,
    ) {
        self.event(PlatformInput::Scroll(ScrollEvent {
            source: ScrollSource::Touch,
            touch_phase: phase,
            delta: ScrollDelta::Pixels(delta),
            position,
            ..Default::default()
        }));
    }
    fn touch(&mut self, id: u64, phase: TouchPhase, position: Point<gpui::Pixels>) {
        self.event(PlatformInput::Touch(TouchEvent {
            id: TouchId(id),
            phase,
            position,
            ..Default::default()
        }));
    }
}

fn picture(cx: &App, width: u32, height: u32, color: &str) -> gpui::ImageSource {
    let svg = format!(
        r#"<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}"><rect width="100%" height="100%" fill="{color}"/></svg>"#
    );
    gpui::ImageSource::Render(
        cx.svg_renderer()
            .render_single_frame(svg.as_bytes(), 1.)
            .unwrap(),
    )
}

#[test]
fn thumbnail_opens_explicit_items_by_source_and_escape_restores_focus() {
    let mut f = Fixture::new(3);
    f.click(point(px(80.), px(60.)));
    assert!(f.is_open());
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.current()), 0);
    f.key("escape", false);
    assert!(!f.is_open());
    f.cx.update_window(f.window.into(), |_, w, cx| {
        assert!(f.image.read(cx).focus_handle(cx).is_focused(w))
    })
    .unwrap();
}

#[test]
fn thumbnail_native_keyboard_click_opens_on_key_release() {
    let mut f = Fixture::new(1);
    f.cx.update_window(f.window.into(), |_, w, cx| {
        w.focus(&f.image.read(cx).focus_handle(cx), cx)
    })
    .unwrap();
    f.key("space", false);
    assert!(!f.is_open());
    f.key("space", true);
    assert!(f.is_open());
}

#[test]
fn disabled_thumbnail_does_not_open_and_registered_items_do_not_duplicate() {
    let mut f = Fixture::new(0);
    for _ in 0..4 {
        f.draw();
    }
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.items().len()), 1);
    f.page.update(&mut f.cx, |p, cx| {
        p.enabled = false;
        cx.notify();
    });
    f.draw();
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.items().len()), 0);
    f.click(point(px(80.), px(60.)));
    assert!(!f.is_open());
}

#[test]
fn group_navigation_is_bounded_and_resets_all_transforms() {
    let mut f = Fixture::new(3);
    f.open(0);
    f.action(PreviewAction::ZoomIn);
    f.action(PreviewAction::RotateRight);
    f.action(PreviewAction::FlipX);
    assert_eq!(
        f.transform(),
        PreviewTransform {
            scale: 1.5,
            rotate: 90.,
            flip_x: true,
            ..Default::default()
        }
    );
    f.action(PreviewAction::Next);
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.current()), 1);
    assert_eq!(f.transform(), PreviewTransform::default());
    f.action(PreviewAction::Next);
    f.action(PreviewAction::Next);
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.current()), 2);
    f.key("left", false);
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.current()), 1);
    assert!(f.events.borrow().contains(&PreviewEvent::Change {
        current: 1,
        previous: 0
    }));
}

#[test]
fn zoom_limits_and_invalid_options_are_sanitized() {
    let mut f = Fixture::new(1);
    f.options(|o| {
        o.min_scale = f32::NAN;
        o.max_scale = 2.;
        o.scale_step = f32::INFINITY;
    });
    f.open(0);
    for _ in 0..5 {
        f.action(PreviewAction::ZoomIn);
    }
    assert_eq!(f.transform().scale, 2.);
    for _ in 0..6 {
        f.action(PreviewAction::ZoomOut);
    }
    assert_eq!(f.transform().scale, 1.);
}

#[test]
fn controlled_open_and_current_emit_requests_without_changing_owner_values() {
    let mut f = Fixture::new(3);
    f.options(|o| {
        o.controlled_open = Some(false);
        o.controlled_current = Some(0);
    });
    f.open(2);
    assert!(!f.is_open());
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.current()), 0);
    assert!(f.events.borrow().contains(&PreviewEvent::Change {
        current: 2,
        previous: 0
    }));
    assert!(f.events.borrow().contains(&PreviewEvent::OpenChange {
        open: true,
        current: 2
    }));
    f.options(|o| {
        o.controlled_open = Some(true);
        o.controlled_current = Some(2);
    });
    assert!(f.is_open());
    assert!(
        f.preview
            .read_with(&f.cx, |s, cx| s.picture().read(cx).source().is_some())
    );
    f.action(PreviewAction::Close);
    assert!(f.is_open());
    assert!(f.events.borrow().contains(&PreviewEvent::OpenChange {
        open: false,
        current: 2
    }));
}

#[test]
fn mask_closing_policy_and_image_click_are_distinct() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.click(point(px(500.), px(400.)));
    assert!(f.is_open());
    f.options(|o| o.mask_closable = false);
    f.click(point(px(10.), px(10.)));
    assert!(f.is_open());
    f.options(|o| o.mask_closable = true);
    f.click(point(px(10.), px(10.)));
    assert!(!f.is_open());
}

#[test]
fn item_replacement_preserves_identity_and_closes_empty_collection() {
    let mut f = Fixture::new(3);
    f.open(1);
    f.preview.update(&mut f.cx, |s, cx| {
        let items = vec![s.items()[2].clone(), s.items()[1].clone()];
        s.set_items(items, cx);
    });
    f.draw();
    assert_eq!(
        f.preview
            .read_with(&f.cx, |s, _| s.current_item().unwrap().id.clone()),
        "item-1"
    );
    f.preview.update(&mut f.cx, |s, cx| {
        s.set_items(Vec::new(), cx);
    });
    // An explicit empty collection takes priority over registered thumbnails.
    f.draw();
    assert!(!f.is_open());
    assert!(f.preview.read_with(&f.cx, |s, _| s.items().is_empty()));
}

#[test]
fn unmount_closes_preview_and_releases_current_picture() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.page.update(&mut f.cx, |p, cx| {
        p.mounted = false;
        cx.notify();
    });
    f.draw();
    f.draw();
    assert!(!f.is_open());
    assert!(
        f.preview
            .read_with(&f.cx, |s, cx| s.picture().read(cx).source().is_none())
    );
}

#[test]
fn custom_preview_renderers_receive_native_content_and_correct_item() {
    let mut f = Fixture::new(3);
    let rendered = Rc::new(RefCell::new(Vec::new()));
    let image_calls = rendered.clone();
    f.options(|o| {
        o.image_render = Some(Rc::new(move |native, info, _, _| {
            image_calls.borrow_mut().push((
                info.current,
                info.total,
                info.image.as_ref().unwrap().id.clone(),
                info.image.as_ref().unwrap().width,
                info.image.as_ref().unwrap().height,
            ));
            native
        }))
    });
    f.open(1);
    assert!(
        rendered
            .borrow()
            .iter()
            .any(|(current, total, id, width, height)| *current == 1
                && *total == 3
                && id.as_ref() == "item-1"
                && (*width - 420.).abs() < 0.001
                && (*height - 300.).abs() < 0.001)
    );
}

#[test]
fn wheel_zoom_keeps_pointer_anchor_and_respects_disabled_wheel() {
    let mut f = Fixture::new(1);
    f.open(0);
    let position = point(px(700.), px(400.));
    f.event(PlatformInput::Scroll(ScrollEvent {
        position,
        delta: ScrollDelta::Pixels(point(px(0.), px(-100.))),
        ..Default::default()
    }));
    assert_eq!(f.transform().scale, 1.5);
    assert!((f.transform().x + 100.).abs() < 0.001);
    f.options(|o| o.wheel = false);
    let old = f.transform();
    f.event(PlatformInput::Scroll(ScrollEvent {
        position,
        delta: ScrollDelta::Pixels(point(px(0.), px(-100.))),
        ..Default::default()
    }));
    assert_eq!(f.transform(), old);
}

#[test]
fn dragging_rebounds_both_dimensions_after_pointer_leaves_viewport() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.action(PreviewAction::ZoomIn);
    f.event(PlatformInput::MouseDown(MouseDownEvent {
        position: point(px(500.), px(400.)),
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: point(px(1400.), px(1100.)),
        pressed_button: Some(MouseButton::Left),
        ..Default::default()
    }));
    assert!(f.transform().x > 500.);
    f.event(PlatformInput::MouseUp(MouseUpEvent {
        position: point(px(1400.), px(1100.)),
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    assert!((f.transform().x - 130.).abs() < 0.001);
    assert!((f.transform().y - 20.).abs() < 0.001);
}

#[test]
fn double_click_toggles_zoom_and_preserves_orientation() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.action(PreviewAction::RotateLeft);
    f.action(PreviewAction::FlipY);
    for scale in [1.5, 1.] {
        f.event(PlatformInput::MouseDown(MouseDownEvent {
            position: point(px(500.), px(400.)),
            button: MouseButton::Left,
            click_count: 2,
            ..Default::default()
        }));
        f.event(PlatformInput::MouseUp(MouseUpEvent {
            position: point(px(500.), px(400.)),
            button: MouseButton::Left,
            click_count: 2,
            ..Default::default()
        }));
        assert_eq!(f.transform().scale, scale);
        assert_eq!(f.transform().rotate, -90.);
        assert!(f.transform().flip_y);
    }
}

#[test]
fn preview_inside_modal_owns_focus_and_escape_returns_to_parent() {
    let mut f = Fixture::new(1);
    let mut created = None;
    f.cx.update_window(f.window.into(), |_, w, cx| {
        let scope = f.page.read(cx).scope.clone();
        let source = picture(cx, 400, 300, "#3b8b65");
        let image = cx.new(|cx| ImageState::new(source.clone(), w, cx));
        let preview = cx.new(|cx| {
            let mut s = PreviewState::new(vec![PreviewItem::new("nested", source)], w, cx);
            s.set_scope(scope.clone(), cx);
            s
        });
        let render_preview = preview.clone();
        let outer = crate::ModalManager::new(scope.clone(), w).show(
            crate::modal::ModalOptions {
                title: Some(crate::modal::ModalContent::text("Parent dialog")),
                z_index: 2000,
                content: Some(crate::modal::ModalContent::render(move |_, _, cx| {
                    PreviewGroup::new("nested-images", &render_preview)
                        .scope(scope.clone())
                        .child(
                            Image::new("nested-image", &image, cx)
                                .preview_group(&render_preview)
                                .w(px(200.))
                                .h(px(150.)),
                        )
                })),
                ..Default::default()
            },
            w,
            cx,
        );
        created = Some((preview, outer));
    })
    .unwrap();
    f.draw();
    let (preview, outer) = created.unwrap();
    f.cx.update_window(f.window.into(), |_, w, cx| {
        preview.update(cx, |s, cx| s.request_open(0, w, cx))
    })
    .unwrap();
    f.draw();
    assert!(preview.read_with(&f.cx, |s, _| s.is_open()));
    f.cx.update_window(f.window.into(), |_, w, cx| {
        assert_ne!(
            crate::modal::host::top(w, cx).unwrap().entity_id(),
            outer.state().unwrap().entity_id()
        );
    })
    .unwrap();
    f.key("escape", false);
    assert!(!preview.read_with(&f.cx, |s, _| s.is_open()));
    f.cx.update_window(f.window.into(), |_, w, cx| {
        assert!(outer.is_open(cx));
        assert_eq!(
            crate::modal::host::top(w, cx).unwrap().entity_id(),
            outer.state().unwrap().entity_id()
        );
    })
    .unwrap();
}

#[test]
fn custom_count_and_close_icon_preserve_close_lifecycle() {
    let mut f = Fixture::new(3);
    let calls = Rc::new(RefCell::new(Vec::new()));
    let output = calls.clone();
    f.options(|o| {
        o.close_icon = crate::icons::IconName::CloseCircleOutlined.into();
        o.count_render = Some(Rc::new(move |current, total, _, _| {
            output.borrow_mut().push((current, total));
            div()
                .child(format!("Picture {current} of {total}"))
                .into_any_element()
        }));
    });
    f.open(1);
    assert!(calls.borrow().contains(&(2, 3)));
    assert_eq!(
        f.preview
            .read_with(&f.cx, |s, _| s.options().close_icon.key().to_owned()),
        crate::icons::IconName::CloseCircleOutlined.icon().key()
    );
    f.click(point(px(967.), px(33.)));
    assert!(!f.is_open());
    assert_eq!(
        f.events
            .borrow()
            .iter()
            .filter(|event| matches!(event, PreviewEvent::OpenChange { open: false, .. }))
            .count(),
        1
    );
    assert!(
        f.preview
            .read_with(&f.cx, |s, cx| s.picture().read(cx).source().is_none())
    );
    f.open(1);
    assert!(f.is_open());
}

#[test]
fn unconfigured_groups_inherit_live_image_defaults_and_ignore_modal_card_styles() {
    let mut f = Fixture::inherited(1);
    let scope = f.page.read_with(&f.cx, |p, _| p.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| {
                patch.image.mask = Some(false);
                patch.image.mask_closable = Some(false);
                patch.image.wheel = Some(false);
                patch.image.movable = Some(false);
                patch.modal.styles.root = gpui::StyleRefinement::default()
                    .w(px(10.))
                    .h(px(10.))
                    .p(px(20.));
            })
            .unwrap();
    });
    f.draw();
    f.preview.read_with(&f.cx, |s, _| {
        assert!(!s.options().mask);
        assert!(!s.options().wheel);
        assert!(!s.options().movable);
    });
    f.open(0);
    f.cx.update_window(f.window.into(), |_, w, cx| {
        let mask: gpui::Hsla = scope
            .snapshot(cx)
            .theme
            .component::<ImageTheme>()
            .globals()
            .color_bg_mask
            .into();
        assert!(
            w.rendered_scene()
                .quads
                .iter()
                .all(|quad| quad.background.as_solid() != Some(mask))
        );
    })
    .unwrap();
    // A close target at the viewport edge remains reachable despite inherited
    // modal-card width and padding overrides.
    f.click(point(px(967.), px(33.)));
    assert!(!f.is_open());
}

#[test]
fn native_pinch_keeps_its_anchor_and_works_with_wheel_disabled() {
    let mut f = Fixture::new(1);
    f.options(|o| o.wheel = false);
    f.open(0);
    let anchor = point(px(700.), px(400.));
    f.pinch(TouchPhase::Started, 0., anchor);
    f.pinch(TouchPhase::Moved, 0.5, anchor);
    assert_eq!(f.transform().scale, 1.5);
    assert!((f.transform().x + 100.).abs() < 0.001);
    assert!(f.events.borrow().iter().any(|event| matches!(
        event,
        PreviewEvent::Transform {
            action: PreviewAction::Pinch,
            ..
        }
    )));
    f.pinch(TouchPhase::Ended, 0., anchor);
    assert_eq!(f.transform().scale, 1.5);
    assert!((f.transform().x + 100.).abs() < 0.001);
}

#[test]
fn cancelled_pinch_restores_orientation_and_invalid_deltas_cannot_corrupt_it() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.action(PreviewAction::RotateLeft);
    f.action(PreviewAction::FlipX);
    let before = f.transform();
    let anchor = point(px(500.), px(400.));
    f.pinch(TouchPhase::Started, 0., anchor);
    f.pinch(TouchPhase::Moved, 0.5, anchor);
    assert_eq!(f.transform().scale, 1.5);
    f.pinch(TouchPhase::Moved, f32::NAN, anchor);
    assert!(f.transform().scale.is_finite());
    f.pinch(TouchPhase::Cancelled, f32::NAN, anchor);
    assert_eq!(f.transform(), before);
}

#[test]
fn native_touch_pan_moves_without_zooming_and_rebounds_at_end() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.action(PreviewAction::ZoomIn);
    let anchor = point(px(500.), px(400.));
    f.touch_scroll(TouchPhase::Started, point(px(100.), px(10.)), anchor);
    f.touch_scroll(TouchPhase::Moved, point(px(800.), px(690.)), anchor);
    assert_eq!(f.transform().scale, 1.5);
    assert_eq!(f.transform().x, 900.);
    f.touch_scroll(TouchPhase::Ended, Point::default(), anchor);
    assert!((f.transform().x - 130.).abs() < 0.001);
    assert!((f.transform().y - 20.).abs() < 0.001);
}

#[test]
fn cancelled_touch_pan_restores_its_origin_and_disabled_movement_stays_fixed() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.action(PreviewAction::ZoomIn);
    let anchor = point(px(500.), px(400.));
    let before = f.transform();
    f.touch_scroll(TouchPhase::Started, point(px(100.), px(10.)), anchor);
    f.touch_scroll(TouchPhase::Cancelled, point(px(f32::NAN), px(0.)), anchor);
    assert_eq!(f.transform(), before);
    f.options(|o| o.movable = false);
    f.touch_scroll(TouchPhase::Started, point(px(100.), px(10.)), anchor);
    f.touch_scroll(TouchPhase::Ended, Point::default(), anchor);
    assert_eq!(f.transform(), before);
}

#[test]
fn native_touch_tap_opens_thumbnail_double_tap_zooms_and_backdrop_tap_closes() {
    let mut f = Fixture::new(1);
    let thumbnail = point(px(80.), px(60.));
    f.touch(1, TouchPhase::Started, thumbnail);
    f.touch(1, TouchPhase::Ended, thumbnail);
    assert!(f.is_open());
    let center = point(px(500.), px(400.));
    f.touch(2, TouchPhase::Started, center);
    f.touch(2, TouchPhase::Ended, center);
    f.touch(3, TouchPhase::Started, center);
    f.touch(3, TouchPhase::Ended, center);
    assert_eq!(f.transform().scale, 1.5);
    f.action(PreviewAction::ZoomOut);
    let backdrop = point(px(10.), px(10.));
    f.touch(4, TouchPhase::Started, backdrop);
    f.touch(4, TouchPhase::Ended, backdrop);
    assert!(!f.is_open());
}

#[test]
fn moving_or_cancelled_touch_on_mask_does_not_close_the_preview() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.touch(1, TouchPhase::Started, point(px(10.), px(10.)));
    f.touch(1, TouchPhase::Moved, point(px(50.), px(20.)));
    f.touch(1, TouchPhase::Ended, point(px(50.), px(20.)));
    assert!(f.is_open());
    f.touch(2, TouchPhase::Started, point(px(10.), px(10.)));
    f.touch(2, TouchPhase::Cancelled, point(px(10.), px(10.)));
    assert!(f.is_open());
}

#[test]
fn button_zoom_scales_existing_translation_about_the_viewport_center() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.action(PreviewAction::ZoomIn);
    f.event(PlatformInput::MouseDown(MouseDownEvent {
        position: point(px(500.), px(400.)),
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: point(px(600.), px(400.)),
        pressed_button: Some(MouseButton::Left),
        ..Default::default()
    }));
    f.event(PlatformInput::MouseUp(MouseUpEvent {
        position: point(px(600.), px(400.)),
        button: MouseButton::Left,
        click_count: 1,
        ..Default::default()
    }));
    assert_eq!(f.transform().x, 100.);
    f.action(PreviewAction::ZoomIn);
    assert_eq!(f.transform().scale, 2.25);
    assert_eq!(f.transform().x, 150.);
}

#[test]
fn controlled_callbacks_can_accept_requests_and_reopen_during_close_dispatch() {
    let mut f = Fixture::new(3);
    let state = f.preview.downgrade();
    f.options(|o| {
        o.controlled_open = Some(false);
        o.controlled_current = Some(0);
        o.on_event = Some(Rc::new(move |event, _, cx| {
            let _ = state.update(cx, |s, cx| {
                let mut options = s.options().clone();
                match event {
                    PreviewEvent::OpenChange { open, current } => {
                        options.controlled_open = Some(*open);
                        options.controlled_current = Some(*current);
                    }
                    PreviewEvent::Change { current, .. } => {
                        options.controlled_current = Some(*current)
                    }
                    _ => return,
                }
                s.set_options(options, cx);
            });
        }));
    });
    f.open(2);
    assert!(f.is_open());
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.current()), 2);
    f.run(|state, w, cx| {
        state.update(cx, |s, cx| {
            s.request_close(w, cx);
            s.request_open(1, w, cx);
        })
    });
    assert!(f.is_open());
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.current()), 1);
    f.action(PreviewAction::Close);
    assert!(!f.is_open());
    assert!(
        f.preview
            .read_with(&f.cx, |s, cx| s.picture().read(cx).source().is_none())
    );
}

#[test]
fn disabled_custom_toolbar_control_does_not_dismiss_the_mask() {
    let mut f = Fixture::new(1);
    f.options(|o| {
        o.actions_render = Some(Rc::new(|original, _, _, _| {
            div()
                .flex()
                .gap(px(12.))
                .items_center()
                .child(
                    crate::Button::new("disabled-custom-previous")
                        .icon(crate::icons::IconName::LeftOutlined)
                        .disabled(true)
                        .w(px(42.))
                        .h(px(42.)),
                )
                .child(original)
                .into_any_element()
        }))
    });
    f.open(0);
    f.click(point(px(314.), px(747.)));
    assert!(f.is_open());
    f.click(point(px(662.), px(747.)));
    assert!(f.is_open());
    assert_eq!(f.transform().scale, 1.5);
    // The transparent margin beside the intrinsic footer remains part of mask.
    f.action(PreviewAction::ZoomOut);
    f.click(point(px(10.), px(747.)));
    assert!(!f.is_open());
}

#[test]
fn compact_preview_chrome_matches_independent_browser_geometry() {
    for dpi in [1., 1.5, 2.] {
        let mut f = Fixture::new(3);
        f.cx.simulate_window_scale_factor_change(f.window.into(), dpi);
        let scope = f.page.read_with(&f.cx, |p, _| p.scope.clone());
        f.cx.update(|cx| {
            scope
                .update_patch(cx, |patch| {
                    patch.theme = Some(crate::theme::ThemeConfig {
                        algorithms: Some(crate::theme::AlgorithmChain::compact()),
                        ..Default::default()
                    })
                })
                .unwrap()
        });
        f.draw();
        f.open(0);
        f.cx.update_window(f.window.into(), |_, w, _| {
            // Genuine current Image reference: compact31pxcontrols,258pxtoolbar,
            // bottom32px. Width/height are checked on actual painted primitives.
            let scale = w.scale_factor();
            assert!(w.rendered_scene().quads.iter().any(|quad| {
                (quad.bounds.size.width.0 / scale - 258.).abs() < 0.01
                    && (quad.bounds.size.height.0 / scale - 31.).abs() < 0.5
                    && (quad.bounds.origin.x.0 / scale - 371.).abs() < 0.5
                    && (quad.bounds.origin.y.0 / scale - 737.).abs() < 0.5
            }));
        })
        .unwrap();
        f.click(point(px(976.5), px(23.5)));
        assert!(!f.is_open());
    }
}

#[test]
fn toolbar_hover_uses_css_ease_and_stops_requesting_frames_when_settled() {
    let mut f = Fixture::new(1);
    f.open(0);
    let scope = f.page.read_with(&f.cx, |p, _| p.scope.clone());
    f.cx.update(|cx| {
        scope
            .update_patch(cx, |patch| patch.motion = Some(true))
            .unwrap()
    });
    f.draw();
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: point(px(635.), px(747.)),
        ..Default::default()
    }));
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(150));
    f.cx.update_window(f.window.into(), |_, w, cx| {
        w.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    f.cx.update_window(f.window.into(), |_, w, _| {
        let scale = w.scale_factor();
        let glyph = w
            .rendered_scene()
            .monochrome_sprites
            .iter()
            .find(|sprite| {
                (sprite.bounds.center().x.0 / scale - 635.).abs() < 1.
                    && (sprite.bounds.center().y.0 / scale - 747.).abs() < 1.
            })
            .expect("native zoom-in glyph");
        let expected = (0.810_480_7_f32 * 255.).round() / 255.;
        assert!(
            (glyph.color.a - expected).abs() < 1e-5,
            "hover alpha {}",
            glyph.color.a
        );
    })
    .unwrap();
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(200));
    f.cx.update_window(f.window.into(), |_, w, cx| {
        w.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    f.event(PlatformInput::MouseMove(MouseMoveEvent {
        position: point(px(10.), px(10.)),
        ..Default::default()
    }));
    f.cx.executor()
        .advance_clock(std::time::Duration::from_millis(350));
    f.cx.update_window(f.window.into(), |_, w, cx| {
        w.simulate_next_frame(cx);
    })
    .unwrap();
    f.draw();
    let callbacks =
        f.cx.update_window(f.window.into(), |_, w, cx| w.simulate_next_frame(cx))
            .unwrap();
    assert_eq!(callbacks, 0);
}

#[test]
fn single_item_group_shows_one_of_one_and_explicit_count_policy_overrides_it() {
    let mut f = Fixture::new(1);
    let calls = Rc::new(RefCell::new(Vec::new()));
    let output = calls.clone();
    f.options(|o| {
        o.count_render = Some(Rc::new(move |current, total, _, _| {
            output.borrow_mut().push((current, total));
            div().into_any_element()
        }))
    });
    f.open(0);
    assert!(calls.borrow().contains(&(1, 1)));
    f.options(|o| o.show_count = Some(false));
    calls.borrow_mut().clear();
    f.draw();
    assert!(calls.borrow().is_empty());
}

#[test]
fn explicit_empty_collection_can_switch_back_to_registered_thumbnails() {
    let mut f = Fixture::new(1);
    f.open(0);
    f.preview
        .update(&mut f.cx, |s, cx| s.set_items(Vec::new(), cx));
    f.draw();
    f.click(point(px(80.), px(60.)));
    assert!(!f.is_open());
    f.preview
        .update(&mut f.cx, |s, cx| s.use_registered_items(cx));
    f.draw();
    f.click(point(px(80.), px(60.)));
    assert!(f.is_open());
    assert_eq!(f.preview.read_with(&f.cx, |s, _| s.items().len()), 1);
}

#[test]
fn controlled_open_binding_recovers_when_an_explicit_collection_is_refilled() {
    let mut f = Fixture::new(1);
    let items = f.preview.read_with(&f.cx, |s, _| s.items().to_vec());
    f.options(|o| o.controlled_open = Some(true));
    assert!(f.is_open());
    f.preview
        .update(&mut f.cx, |s, cx| s.set_items(Vec::new(), cx));
    f.draw();
    assert!(!f.is_open());
    f.preview.update(&mut f.cx, |s, cx| s.set_items(items, cx));
    f.draw();
    assert!(f.is_open());
}

#[test]
fn group_style_resolver_shares_one_snapshot_with_mask_and_body() {
    use std::sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    };
    struct StyledGroup {
        state: Entity<PreviewState>,
        scope: ConfigScope,
        renders: Arc<AtomicUsize>,
    }
    impl Render for StyledGroup {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            self.renders.fetch_add(1, Ordering::SeqCst);
            PreviewGroup::new("styled-group", &self.state).scope(self.scope.clone())
        }
    }
    let mut cx = TestAppContext::single();
    let renders = Arc::new(AtomicUsize::new(0));
    let calls = Arc::new(Mutex::new(Vec::new()));
    let output = calls.clone();
    let counter = renders.clone();
    let mut retained = None;
    let window = cx.open_window(size(px(1000.), px(800.)), |w, cx| {
        let scope = ConfigScope::new_window(cx);
        scope
            .update_patch(cx, |patch| {
                patch.motion = Some(false);
                patch.image.styles.popup_mask =
                    gpui::StyleRefinement::default().bg(gpui::rgb(0xff0000));
                patch.image.styles_fn = Some(ImageStyleResolver::new(move |context, _, _| {
                    let mut calls = output.lock().unwrap();
                    calls.push(*context);
                    let color = gpui::rgb(0x237000 + calls.len() as u32);
                    ImageStyles {
                        popup_mask: gpui::StyleRefinement::default().bg(color),
                        popup_body: gpui::StyleRefinement::default()
                            .border_1()
                            .border_color(color),
                        ..Default::default()
                    }
                }));
            })
            .unwrap();
        let source = picture(cx, 1200, 800, "#386db8");
        let state = cx.new(|cx| {
            let mut state = PreviewState::new(vec![PreviewItem::new("photo", source)], w, cx);
            state.set_scope(scope.clone(), cx);
            state.set_open(true, w, cx);
            state
        });
        retained = Some((state.clone(), scope.clone()));
        let page = cx.new(|_| StyledGroup {
            state,
            scope: scope.clone(),
            renders: counter,
        });
        UiRoot::new(scope, page, w, cx)
    });
    let (state, scope) = retained.unwrap();
    for direction in [crate::Direction::Ltr, crate::Direction::Rtl] {
        if direction == crate::Direction::Rtl {
            state.update(&mut cx, |s, cx| {
                let mut options = s.options().clone();
                options.styles.popup_mask =
                    gpui::StyleRefinement::default().bg(gpui::rgb(0x0000ff));
                options.styles.popup_body = gpui::StyleRefinement::default()
                    .border_1()
                    .border_color(gpui::rgb(0x0000ff));
                s.set_options(options, cx);
            });
        }
        cx.update(|cx| {
            scope
                .update_patch(cx, |patch| patch.direction = Some(direction))
                .unwrap();
        });
        cx.run_until_parked();
        cx.update_window(window.into(), |_, w, cx| w.draw(cx).clear(cx))
            .unwrap();
        cx.run_until_parked();
        let resolved = calls.lock().unwrap();
        assert_eq!(resolved.len(), renders.load(Ordering::SeqCst));
        assert_eq!(resolved.last().unwrap().direction, direction);
        assert!(
            resolved
                .iter()
                .all(|context| context.preview && !context.failed)
        );
        let expected: gpui::Hsla = gpui::rgb(if direction == crate::Direction::Rtl {
            0x0000ff
        } else {
            0x237000 + resolved.len() as u32
        })
        .into();
        drop(resolved);
        cx.update_window(window.into(), |_, w, _| {
            let scene = w.rendered_scene();
            assert!(
                scene
                    .quads
                    .iter()
                    .any(|quad| quad.background.as_solid() == Some(expected)),
                "the actual Modal mask must use the resolved color"
            );
            assert!(
                scene.quads.iter().any(|quad| quad.border_color == expected),
                "body and mask must share the same resolver invocation"
            );
        })
        .unwrap();
    }
    assert!(state.read_with(&cx, |s, _| s.is_open()));
    state.update(&mut cx, |s, cx| {
        let mut options = s.options().clone();
        options.mask = false;
        s.set_options(options, cx);
    });
    cx.run_until_parked();
    cx.update_window(window.into(), |_, w, cx| w.draw(cx).clear(cx))
        .unwrap();
    cx.update_window(window.into(), |_, w, _| {
        let masked: gpui::Hsla = gpui::rgb(0x0000ff).into();
        assert!(
            w.rendered_scene()
                .quads
                .iter()
                .all(|quad| quad.background.as_solid() != Some(masked)),
            "mask=false must omit even an explicit mask fill"
        );
    })
    .unwrap();
}
