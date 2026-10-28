// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
// Examples adapted from Ant Design 6.6.7 (MIT), using native GPUI content and tasks.
use super::shell::{page_padding, sidebar_width};
use ant_design_gpui::{
    Button, ConfigPatch, ConfigScope, Modal, ModalManager, ModalState,
    locale::Locale,
    modal::{
        ModalAction, ModalContent, ModalFooter, ModalKind, ModalLength, ModalOptions, ModalStyles,
        ModalTheme, ModalTokenPatch, ModalWidth,
    },
    theme::{Color, ThemeConfig},
};
use gpui::{
    AnyElement, App, AppContext, Context, Entity, FontWeight, IntoElement, MouseButton,
    MouseMoveEvent, MouseUpEvent, Pixels, Point, Render, Role, ScrollAnchor, StyleRefinement,
    Subscription, Window, canvas, div, point, prelude::*, px, relative,
};
use std::{cell::RefCell, rc::Rc, time::Duration};

pub(super) struct Demo {
    pub id: &'static str,
    pub title: &'static str,
    description: &'static str,
}
pub(super) const DEMOS: &[Demo] = &[
    Demo {
        id: "modal-basic",
        title: "Basic",
        description: "A controlled native dialog. OK, Cancel, Escape, and the mask request closing.",
    },
    Demo {
        id: "modal-async",
        title: "Asynchronously close",
        description: "OK shows progress until the application task finishes.",
    },
    Demo {
        id: "modal-footer",
        title: "Customized Footer",
        description: "Replace the footer with application actions, or omit it.",
    },
    Demo {
        id: "modal-mask",
        title: "Mask",
        description: "Dimmed and transparent masks. This GPUI revision uses dimming for the browser's blur effect.",
    },
    Demo {
        id: "modal-loading",
        title: "Loading",
        description: "A four-row skeleton replaces content and hides the footer while loading.",
    },
    Demo {
        id: "modal-footer-render",
        title: "Customized Footer render function",
        description: "Compose additional actions around the built-in Cancel and OK buttons.",
    },
    Demo {
        id: "modal-hooks",
        title: "Use context",
        description: "Managed dialogs inherit the caller's theme, locale, direction, and component defaults.",
    },
    Demo {
        id: "modal-locale",
        title: "Internationalization",
        description: "English, Simplified Chinese, and Traditional Chinese labels come from scoped catalogs.",
    },
    Demo {
        id: "modal-manual",
        title: "Manual update and destroy",
        description: "A retained handle updates a countdown, then destroys only its own dialog.",
    },
    Demo {
        id: "modal-position",
        title: "Customize the position",
        description: "Use a custom top offset or center the dialog in the window.",
    },
    Demo {
        id: "modal-button-props",
        title: "Customize footer buttons",
        description: "Disabled and danger states use the shared native Button component.",
    },
    Demo {
        id: "modal-render",
        title: "Custom modal content render",
        description: "Drag the title to reposition a native dialog. Its bounds stay inside the window.",
    },
    Demo {
        id: "modal-width",
        title: "Customize the width",
        description: "Fixed, fractional, and responsive widths respect the window's edge gutters.",
    },
    Demo {
        id: "modal-static-info",
        title: "Information dialogs",
        description: "Information, success, warning, and error dialogs have appropriate status icons.",
    },
    Demo {
        id: "modal-confirm",
        title: "Confirmation",
        description: "Confirm a destructive action; a failed async action keeps the dialog open for retry.",
    },
    Demo {
        id: "modal-destroy-all",
        title: "Destroy confirmation dialogs",
        description: "Close this manager's dialogs together, including stacked confirmations.",
    },
    Demo {
        id: "modal-style",
        title: "Custom semantic styling",
        description: "Theme tokens and semantic slot refinements customize the native surface.",
    },
    Demo {
        id: "modal-nested",
        title: "Nested dialogs and popups",
        description: "Child popups render above their dialog; Escape dismisses the topmost surface first.",
    },
    Demo {
        id: "modal-tall",
        title: "Tall content",
        description: "The complete dialog scrolls within a small window without losing its header or footer.",
    },
    Demo {
        id: "modal-retained",
        title: "Retained content",
        description: "Closing preserves the owned native view. Destroy on hidden constructs a fresh view on reopening.",
    },
];

pub(super) struct ModalPage {
    scope: ConfigScope,
    anchors: Vec<ScrollAnchor>,
    pub state: Entity<ModalState>,
    manager: ModalManager,
    _config: Subscription,
}
fn contents(rows: usize) -> ModalContent {
    ModalContent::render(move |handle, _, cx| {
        let direction = handle
            .state()
            .unwrap()
            .read(cx)
            .scope()
            .snapshot(cx)
            .direction;
        div()
            .flex()
            .flex_col()
            .gap(px(14.))
            .children((0..rows).map(|i| {
                div()
                    .id(("paragraph", i))
                    .child(ant_design_gpui::behavior::paragraph_text(
                        "text",
                        "Some contents...",
                        direction,
                    ))
            }))
    })
}
fn options(title: impl Into<gpui::SharedString>) -> ModalOptions {
    ModalOptions {
        title: Some(ModalContent::text(title)),
        content: Some(contents(3)),
        on_ok: Some(Rc::new(|_, _, _| ModalAction::Close)),
        on_cancel: Some(Rc::new(|_, _, _| ModalAction::Close)),
        ..Default::default()
    }
}
fn pending(cx: &mut App, reject: bool) -> ModalAction {
    let timer = cx.background_executor().timer(Duration::from_secs(1));
    ModalAction::Pending(cx.spawn(async move |_| {
        timer.await;
        if reject {
            Err("The operation failed. Please retry.".into())
        } else {
            Ok(())
        }
    }))
}
impl ModalPage {
    pub fn new(
        scope: ConfigScope,
        anchors: Vec<ScrollAnchor>,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let state = cx.new(|cx| {
            let mut state = ModalState::new(w, cx);
            state.set_scope(scope.clone(), cx);
            state.set_options(options("Basic Modal"), cx);
            state
        });
        Self {
            manager: ModalManager::new(scope.clone(), w),
            state,
            _config: scope.observe(cx),
            scope,
            anchors,
        }
    }
    fn button(
        &self,
        id: String,
        label: &'static str,
        action: impl Fn(&mut Self, &mut Window, &mut Context<Self>) + 'static,
        cx: &Context<Self>,
    ) -> AnyElement {
        Button::new(id.clone())
            .scope(self.scope.clone())
            .label(label)
            .automation_id(id)
            .on_click(cx.listener(move |page, _, w, cx| action(page, w, cx)))
            .into_any_element()
    }
    pub fn open_demo(
        &mut self,
        id: &'static str,
        variant: usize,
        w: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let mut o = options("Basic Modal");
        match id {
            "modal-basic" => {
                self.state.update(cx, |s, cx| {
                    s.set_options(o, cx);
                    s.set_open(true, w, cx)
                });
                return;
            }
            "modal-async" => o.on_ok = Some(Rc::new(|_, _, cx| pending(cx, false))),
            "modal-footer" => {
                o.footer = if variant == 1 {
                    ModalFooter::None
                } else {
                    ModalFooter::Content(ModalContent::render(|h, _, _| {
                        let close = h.clone();
                        div()
                            .flex()
                            .justify_end()
                            .gap(px(8.))
                            .child(
                                Button::new("back")
                                    .label("Return")
                                    .on_click(move |_, w, cx| close.close(w, cx)),
                            )
                            .child(Button::new("submit").label("Submit").primary().on_click({
                                let h = h.clone();
                                move |_, w, cx| h.confirm(w, cx)
                            }))
                    }))
                };
            }
            "modal-mask" => {
                o.kind = ModalKind::Confirm;
                o.mask = Some(variant == 0);
            }
            "modal-loading" => {
                o.loading = true;
                let h = self.manager.show(o, w, cx);
                cx.spawn_in(w, async move |_, cx| {
                    cx.background_executor().timer(Duration::from_secs(2)).await;
                    let _ = cx.update(|_, cx| h.update(|o| o.loading = false, cx));
                })
                .detach();
                return;
            }
            "modal-footer-render" => {
                o.footer = ModalFooter::Render(Rc::new(|buttons, h, _, _| {
                    let close = h.clone();
                    div()
                        .flex()
                        .justify_end()
                        .gap(px(8.))
                        .child(
                            Button::new("custom")
                                .label("Custom action")
                                .on_click(move |_, w, cx| close.close(w, cx)),
                        )
                        .children(buttons.cancel)
                        .child(buttons.ok)
                        .into_any_element()
                }))
            }
            "modal-hooks" | "modal-locale" => {
                let mut patch = ConfigPatch::default();
                patch.locale.locale = Some(match variant {
                    0 => Locale::en_us(),
                    1 => Locale::zh_cn(),
                    _ => Locale::zh_tw(),
                });
                let scope = self.scope.child(patch, cx).unwrap();
                o.content = Some(ModalContent::text(
                    "This dialog inherits its caller's configuration.",
                ));
                ModalManager::new(scope, w).confirm(o, w, cx);
                return;
            }
            "modal-manual" => {
                o.title = Some(ModalContent::text("This dialog will close in 5 seconds"));
                let h = self.manager.info(o, w, cx);
                cx.spawn_in(w, async move |_, cx| {
                    for remaining in (0..5).rev() {
                        cx.background_executor().timer(Duration::from_secs(1)).await;
                        let keep = cx
                            .update(|w, cx| {
                                if !h.is_open(cx) {
                                    return false;
                                }
                                if remaining == 0 {
                                    h.destroy(w, cx);
                                } else {
                                    h.update(
                                        |o| {
                                            o.title = Some(ModalContent::text(format!(
                                                "This dialog will close in {remaining} seconds"
                                            )))
                                        },
                                        cx,
                                    );
                                }
                                true
                            })
                            .unwrap_or(false);
                        if !keep {
                            break;
                        }
                    }
                })
                .detach();
                return;
            }
            "modal-position" => {
                o.centered = Some(variant == 1);
                if variant == 0 {
                    o.top = 20.;
                }
            }
            "modal-button-props" => {
                o.cancel_button.disabled = Some(true);
                o.ok_button.danger = Some(true);
                o.ok_text = Some("Delete".into());
            }
            "modal-render" => {
                o = draggable();
            }
            "modal-width" => {
                o.width = Some(match variant {
                    0 => 1000_f32.into(),
                    1 => ModalWidth::Fixed(ModalLength::Fraction(0.8)),
                    _ => ModalWidth::Responsive([
                        Some(ModalLength::Fraction(0.9)),
                        Some(480_f32.into()),
                        None,
                        Some(800_f32.into()),
                        None,
                        None,
                    ]),
                })
            }
            "modal-static-info" => {
                o.kind = [
                    ModalKind::Info,
                    ModalKind::Success,
                    ModalKind::Warning,
                    ModalKind::Error,
                ][variant];
                o.title = Some(ModalContent::text(
                    ["Information", "Success", "Warning", "Error"][variant],
                ));
                o.content = Some(ModalContent::text("This is a native context-aware dialog."));
            }
            "modal-confirm" => {
                o.kind = ModalKind::Confirm;
                o.title = Some(ModalContent::text("Do you want to delete these items?"));
                o.ok_button.danger = Some(true);
                let fail = Rc::new(std::cell::Cell::new(variant == 1));
                o.on_ok = Some(Rc::new(move |_, _, cx| pending(cx, fail.replace(false))));
                o.on_event = Some(Rc::new(|e, _, _| {
                    if let ant_design_gpui::ModalEvent::ActionFailed(message) = e {
                        eprintln!("{message}");
                    }
                }));
            }
            "modal-destroy-all" => {
                for i in 1..=3 {
                    let manager = self.manager.clone();
                    let mut dialog = options(format!("Confirmation {i}"));
                    dialog.content = Some(ModalContent::render(move |_, _, _| {
                        let manager = manager.clone();
                        Button::new("destroy-all")
                            .label("Destroy all dialogs")
                            .on_click(move |_, w, cx| manager.destroy_all(w, cx))
                    }));
                    self.manager.confirm(dialog, w, cx);
                }
                return;
            }
            "modal-style" => {
                let mut theme = ThemeConfig::default();
                theme.components.set::<ModalTheme>(ModalTokenPatch {
                    title_font_size: Some(20.),
                    header_margin_bottom: Some(16.),
                    ..Default::default()
                });
                let scope = self
                    .scope
                    .child(
                        ConfigPatch {
                            theme: Some(theme),
                            ..Default::default()
                        },
                        cx,
                    )
                    .unwrap();
                o.styles.container = StyleRefinement::default()
                    .border_1()
                    .border_color(Color::rgb(0x1677ff));
                o.styles_fn = Some(Rc::new(|_, _, _| ModalStyles {
                    title: StyleRefinement::default().text_color(Color::rgb(0x1677ff)),
                    ..Default::default()
                }));
                ModalManager::new(scope, w).show(o, w, cx);
                return;
            }
            "modal-nested" => {
                let manager = self.manager.clone();
                let scope = self.scope.clone();
                o.content = Some(ModalContent::render(move |_, _, _| {
                    let manager = manager.clone();
                    div()
                        .flex()
                        .gap(px(8.))
                        .child(
                            Button::new("nested")
                                .scope(scope.clone())
                                .label("Open child")
                                .on_click(move |_, w, cx| {
                                    manager.confirm(options("Nested confirmation"), w, cx);
                                }),
                        )
                        .child(
                            ant_design_gpui::Popover::new("details")
                                .scope(scope.clone())
                                .title("Details")
                                .content(|_, _, _| div().child("Popup above the dialog"))
                                .child(
                                    Button::new("details-trigger")
                                        .scope(scope.clone())
                                        .label("Show details"),
                                ),
                        )
                }));
            }
            "modal-tall" => {
                o.content = Some(contents(40));
                o.centered = Some(true);
            }
            "modal-retained" => {
                self.state.update(cx, |s, cx| {
                    if s.options().automation_id.as_deref() != Some("modal.retained") {
                        o.automation_id = Some("modal.retained".into());
                        o.content = Some(ModalContent::view(|_, cx| cx.new(|_| Counter(0)).into()));
                        s.set_options(o, cx);
                    }
                    let mut o = s.options().clone();
                    o.destroy_on_hidden = variant == 1;
                    s.set_options(o, cx);
                    s.set_open(true, w, cx);
                });
                return;
            }
            _ => {}
        }
        self.manager.show(o, w, cx);
    }
}
struct Counter(usize);
impl Render for Counter {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        Button::new("counter")
            .label(format!("Count: {}", self.0))
            .on_click(cx.listener(|s, _, _, cx| {
                s.0 += 1;
                cx.notify();
            }))
    }
}
#[derive(Default)]
struct Drag {
    origin: Option<Point<Pixels>>,
    offset: Point<Pixels>,
    start: Point<Pixels>,
}
fn draggable() -> ModalOptions {
    let mut o = options("Draggable Modal");
    let drag = Rc::new(RefCell::new(Drag::default()));
    o.title = Some(ModalContent::render(move |handle, _, _| {
        let start = drag.clone();
        let moving = drag.clone();
        let stop = drag.clone();
        let h = handle.clone();
        div()
            .id("drag-title")
            .cursor_move()
            .child("Draggable Modal")
            .on_mouse_down(MouseButton::Left, move |e, _, cx| {
                let mut d = start.borrow_mut();
                d.origin = Some(e.position);
                d.start = d.offset;
                cx.stop_propagation();
            })
            .child(
                canvas(
                    |_, _, _| (),
                    move |_, (), w, _| {
                        let drag = moving.clone();
                        let h = h.clone();
                        w.on_mouse_event(move |e: &MouseMoveEvent, phase, w, cx| {
                            if !phase.capture() {
                                return;
                            }
                            let mut d = drag.borrow_mut();
                            let Some(origin) = d.origin else {
                                return;
                            };
                            let Some(state) = h.state() else {
                                return;
                            };
                            let b = state.read(cx).bounds();
                            let viewport = w.viewport_size();
                            let delta = e.position - origin;
                            let next = d.start + delta;
                            let base = b.origin - d.offset;
                            d.offset = point(
                                next.x.clamp(
                                    -base.x,
                                    (viewport.width - b.size.width - base.x).max(-base.x),
                                ),
                                next.y.clamp(
                                    -base.y,
                                    (viewport.height - b.size.height - base.y).max(-base.y),
                                ),
                            );
                            let offset = d.offset;
                            h.update(
                                |o| {
                                    o.styles.root =
                                        StyleRefinement::default().left(offset.x).top(offset.y)
                                },
                                cx,
                            );
                            w.refresh();
                            cx.stop_propagation();
                        });
                        let drag = stop.clone();
                        w.on_mouse_event(move |_: &MouseUpEvent, phase, _, _| {
                            if phase.capture() {
                                drag.borrow_mut().origin = None;
                            }
                        });
                    },
                )
                .absolute()
                .size_0(),
            )
    }));
    o.surface_render = Some(Rc::new(|surface, _, _, _| {
        div().relative().child(surface).into_any_element()
    }));
    o
}
impl Render for ModalPage {
    fn render(&mut self, w: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let t = &config.theme.tokens;
        let width = w.viewport_size().width;
        div()
            .id("gallery.modal.examples")
            .w_full()
            .child(Modal::new("declarative", &self.state, cx).scope(self.scope.clone()))
            .child(
                div()
                    .grid()
                    .grid_cols(
                        if width - sidebar_width(width) - page_padding(width) * 2. >= px(840.) {
                            2
                        } else {
                            1
                        },
                    )
                    .items_start()
                    .gap(px(t.margin_lg as f32))
                    .children(DEMOS.iter().enumerate().map(|(index, demo)| {
                        let labels: &[&'static str] = match demo.id {
                            "modal-footer" => &["Custom footer", "No footer"],
                            "modal-mask" => &["Dimmed mask", "No mask"],
                            "modal-position" => &["Top 20px", "Centered"],
                            "modal-locale" => &["English", "简体中文", "繁體中文"],
                            "modal-width" => &["1000px", "80%", "Responsive"],
                            "modal-static-info" => &["Info", "Success", "Warning", "Error"],
                            "modal-confirm" => &["Confirm", "Reject, then retry"],
                            "modal-retained" => &["Retain", "Destroy on hidden"],
                            _ => &["Open Modal"],
                        };
                        div()
                            .id(demo.id)
                            .anchor_scroll(Some(self.anchors[index].clone()))
                            .role(Role::Group)
                            .aria_label(demo.title)
                            .debug_selector(|| format!("gallery.card.{}", demo.id))
                            .min_w_0()
                            .border_1()
                            .border_color(t.color_border_secondary)
                            .rounded(px(t.border_radius_lg as f32))
                            .bg(t.color_bg_container)
                            .child(
                                div()
                                    .p(px(t.padding as f32))
                                    .font_weight(FontWeight::SEMIBOLD)
                                    .child(demo.title),
                            )
                            .child(
                                div()
                                    .p(px(t.padding_lg as f32))
                                    .min_h(px(100.))
                                    .flex()
                                    .flex_wrap()
                                    .gap(px(8.))
                                    .children(labels.iter().enumerate().map(|(variant, label)| {
                                        self.button(
                                            format!("{}.{}", demo.id, variant),
                                            label,
                                            move |page, w, cx| {
                                                page.open_demo(demo.id, variant, w, cx)
                                            },
                                            cx,
                                        )
                                    })),
                            )
                            .child(
                                div()
                                    .border_t_1()
                                    .border_color(t.color_border_secondary)
                                    .p(px(t.padding as f32))
                                    .text_size(px(t.font_size_sm as f32))
                                    .line_height(relative(t.line_height as f32))
                                    .text_color(t.color_text_secondary)
                                    .child(demo.description),
                            )
                    })),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn draggable_modal_tracks_pointer_clamps_to_viewport_and_stops_on_release() {
        use gpui::{MouseDownEvent, PlatformInput};
        let mut cx = gpui::TestAppContext::single();
        let window = cx.add_window(|w, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let body = cx.new(|_| gpui::Empty);
            ant_design_gpui::UiRoot::new(scope, body, w, cx)
        });
        cx.update_window(window.into(), |_, w, cx| w.draw(cx).clear(cx))
            .unwrap();
        let handle = cx
            .update_window(window.into(), |_, w, cx| {
                ModalManager::new(ConfigScope::application(cx), w).show(draggable(), w, cx)
            })
            .unwrap();
        let mut draw = |input: Option<PlatformInput>| {
            cx.update_window(window.into(), |_, w, cx| {
                if let Some(input) = input {
                    w.dispatch_event(input, cx);
                }
                w.draw(cx).clear(cx);
                (handle.state().unwrap().read(cx).bounds(), w.viewport_size())
            })
            .unwrap()
        };
        let (initial, _) = draw(None);
        let start = initial.origin + point(px(50.), px(26.));
        draw(Some(PlatformInput::MouseDown(MouseDownEvent {
            button: MouseButton::Left,
            position: start,
            ..Default::default()
        })));
        let motion = |position| {
            PlatformInput::MouseMove(MouseMoveEvent {
                position,
                pressed_button: Some(MouseButton::Left),
                ..Default::default()
            })
        };
        let (moved, _) = draw(Some(motion(start + point(px(30.), px(20.)))));
        assert_eq!(moved.origin, initial.origin + point(px(30.), px(20.)));
        let (clamped, viewport) = draw(Some(motion(point(px(10000.), px(10000.)))));
        assert_eq!(clamped.right(), viewport.width);
        assert_eq!(clamped.bottom(), viewport.height);
        draw(Some(PlatformInput::MouseUp(MouseUpEvent {
            button: MouseButton::Left,
            position: point(px(10000.), px(10000.)),
            ..Default::default()
        })));
        let (released, _) = draw(Some(motion(start)));
        assert_eq!(released, clamped);
    }

    #[test]
    fn modal_gallery_examples_open_and_release_managed_dialogs() {
        let mut cx = gpui::TestAppContext::single();
        let mut page = None;
        let window = cx.add_window(|w, cx| {
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let scroll = gpui::ScrollHandle::new();
            let anchors = DEMOS
                .iter()
                .map(|_| ScrollAnchor::for_handle(scroll.clone()))
                .collect();
            let scene = cx.new(|cx| ModalPage::new(scope.clone(), anchors, w, cx));
            page = Some(scene.clone());
            ant_design_gpui::UiRoot::new(scope, scene, w, cx)
        });
        let page = page.unwrap();
        for demo in DEMOS {
            cx.update_window(window.into(), |_, w, cx| {
                page.update(cx, |p, cx| p.open_demo(demo.id, 0, w, cx))
            })
            .unwrap();
            cx.run_until_parked();
            cx.update_window(window.into(), |_, w, cx| w.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
            cx.update_window(window.into(), |_, w, cx| {
                page.update(cx, |p, cx| {
                    p.manager.destroy_all(w, cx);
                    p.state.update(cx, |s, cx| s.set_open(false, w, cx));
                })
            })
            .unwrap();
            cx.run_until_parked();
            cx.update_window(window.into(), |_, w, cx| w.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        }
    }
}
