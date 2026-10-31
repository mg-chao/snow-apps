// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{Button, ConfigScope, Direction, Size, UiRoot, locale::Locale, theme::AlgorithmChain};
use gpui::{
    AppContext, Bounds, Context, Entity, EntityInputHandler, Focusable, IntoElement, KeyDownEvent,
    KeyUpEvent, Keystroke, Modifiers, MouseButton, MouseDownEvent, MouseUpEvent, Pixels,
    PlatformInput, Point, Render, RenderOnce, StyleRefinement, TestAppContext, VisualTestContext,
    Window, div, prelude::*, px, size,
};
use std::{
    cell::{Cell, RefCell},
    collections::HashMap,
    rc::Rc,
    time::Duration,
};

struct Consumer {
    state: Entity<PaginationState>,
    scope: ConfigScope,
    events: Rc<RefCell<Vec<PaginationEvent>>>,
    renders: Rc<Cell<usize>>,
    shown: bool,
    total: u64,
    total_explicit: bool,
    disabled: Option<bool>,
    size: Option<Size>,
    mode: PaginationMode,
    quick: QuickJumper,
    show_size: bool,
    suppressed: Option<PaginationItemKind>,
    only_jumps: bool,
    responsive: bool,
    custom_go: bool,
    custom_size: bool,
    select_builds: Rc<Cell<usize>>,
    styles: PaginationStyles,
    override_select: bool,
    show_total: bool,
    probe_colors: bool,
    painted_colors: Rc<RefCell<HashMap<&'static str, gpui::Hsla>>>,
}

fn color_probe(
    key: &'static str,
    output: Rc<RefCell<HashMap<&'static str, gpui::Hsla>>>,
) -> impl IntoElement {
    gpui::canvas(
        |_, _, _| (),
        move |_, (), window, _| {
            output.borrow_mut().insert(key, window.text_style().color);
        },
    )
    .absolute()
    .inset_0()
}
impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        let mut content = div()
            .size_full()
            .p(px(20.))
            .flex()
            .flex_col()
            .gap(px(20.))
            .items_start()
            .child(
                div().debug_selector(|| "pagination-before".into()).child(
                    Button::new("pagination-before")
                        .scope(self.scope.clone())
                        .label("Before"),
                ),
            );
        if self.shown {
            let events = self.events.clone();
            let mut pager = Pagination::new("native-pagination", &self.state)
                .scope(self.scope.clone())
                .default_current(6)
                .mode(self.mode)
                .quick_jumper(self.quick)
                .responsive(self.responsive)
                .show_size_changer(self.show_size)
                .styles(self.styles.clone())
                .show_title(false)
                .on_event(move |event, _, _| events.borrow_mut().push(event.clone()));
            if self.total_explicit {
                pager = pager.total(self.total);
            }
            if let Some(disabled) = self.disabled {
                pager = pager.disabled(disabled);
            }
            if let Some(size) = self.size {
                pager = pager.size(size);
            }
            let builds = self.select_builds.clone();
            let override_select = self.override_select;
            pager = pager.size_changer_select(move |select, _, _| {
                builds.set(builds.get() + 1);
                if override_select {
                    select
                        .value([crate::SelectValue::new(20, "Wrong value")])
                        .disabled(false)
                } else {
                    select
                }
            });
            if self.show_total {
                let output = self.painted_colors.clone();
                let probe = self.probe_colors;
                pager = pager.show_total(move |total, _, _, _| {
                    div()
                        .relative()
                        .debug_selector(|| "pagination-total".into())
                        .child(format!("Total {total}"))
                        .when(probe, |div| div.child(color_probe("total", output.clone())))
                });
            }
            if self.custom_go {
                let output = self.painted_colors.clone();
                let probe = self.probe_colors;
                pager = pager.go_button(move |_, _| {
                    div()
                        .relative()
                        .debug_selector(|| "pagination-custom-go".into())
                        .child("Apply")
                        .when(probe, |div| div.child(color_probe("quick", output.clone())))
                });
            }
            if self.custom_size {
                pager = pager.size_changer_render(|info, _, _| {
                    assert!(
                        info.options.contains(&info.value),
                        "custom changers receive the current size among their available choices"
                    );
                    let request = info.clone();
                    div()
                        .debug_selector(|| "pagination-custom-size".into())
                        .child(
                            Button::new("custom-size")
                                .scope(info.scope.clone())
                                .disabled(info.disabled)
                                .label("100 / page")
                                .on_click(move |_, window, cx| {
                                    request.change_size(100, window, cx)
                                }),
                        )
                });
            }
            let suppressed = self.suppressed;
            let only_jumps = self.only_jumps;
            let output = self.painted_colors.clone();
            let probe = self.probe_colors;
            pager = pager.item_render(move |info, original, _, _| {
                if only_jumps
                    && !matches!(
                        info.kind,
                        PaginationItemKind::JumpPrevious | PaginationItemKind::JumpNext
                    )
                {
                    return None;
                }
                let id = super::view::item_id(info.kind);
                (Some(info.kind) != suppressed).then(|| {
                    div()
                        .relative()
                        .debug_selector(move || id.to_string())
                        .child(original)
                        .when(probe, |div| div.child(color_probe("item", output.clone())))
                        .into_any_element()
                })
            });
            let element = pager.render(window, cx).into_element();
            content = content.child(element);
        }
        content.child(
            div().debug_selector(|| "pagination-after".into()).child(
                Button::new("pagination-after")
                    .scope(self.scope.clone())
                    .label("After"),
            ),
        )
    }
}
struct CachedPanel(Entity<Consumer>);
impl Render for CachedPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().size_full().child(
            self.0
                .clone()
                .cached(StyleRefinement::default().size_full()),
        )
    }
}
struct Fixture {
    cx: VisualTestContext,
    consumer: Entity<Consumer>,
    scope: ConfigScope,
    events: Rc<RefCell<Vec<PaginationEvent>>>,
}
impl Fixture {
    fn new() -> Self {
        Self::with_context(TestAppContext::single())
    }
    fn with_context(mut cx: TestAppContext) -> Self {
        let mut parts = None;
        let events = Rc::new(RefCell::new(Vec::new()));
        let output = events.clone();
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            let root_scope = ConfigScope::new_window(cx);
            let scope = root_scope
                .child(
                    crate::ConfigPatch {
                        motion: Some(false),
                        ..Default::default()
                    },
                    cx,
                )
                .unwrap();
            let state = cx.new(|cx| PaginationState::new(window, cx));
            let consumer = cx.new(|_| Consumer {
                state,
                scope: scope.clone(),
                events: output,
                renders: Rc::new(Cell::new(0)),
                shown: true,
                total: 500,
                total_explicit: true,
                disabled: None,
                size: None,
                mode: PaginationMode::Full,
                quick: QuickJumper::Hidden,
                show_size: false,
                suppressed: None,
                only_jumps: false,
                responsive: false,
                custom_go: false,
                custom_size: false,
                select_builds: Rc::new(Cell::new(0)),
                styles: PaginationStyles::default(),
                override_select: false,
                show_total: false,
                probe_colors: false,
                painted_colors: Rc::default(),
            });
            let panel = cx.new(|_| CachedPanel(consumer.clone()));
            parts = Some((consumer, scope));
            UiRoot::new(root_scope, panel, window, cx)
        });
        let (consumer, scope) = parts.unwrap();
        let mut fixture = Self {
            cx: VisualTestContext::from_window(window.into(), &cx),
            consumer,
            scope,
            events,
        };
        fixture.cx.simulate_resize(size(px(1280.), px(720.)));
        fixture.draw();
        fixture
    }
    fn state(&self) -> Entity<PaginationState> {
        self.consumer
            .read_with(&self.cx, |view, _| view.state.clone())
    }
    fn current(&self) -> u64 {
        self.state().read_with(&self.cx, |state, _| state.current())
    }
    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx.update(|window, cx| window.draw(cx).clear(cx));
        self.cx.run_until_parked();
    }
    fn resize(&mut self, width: f32) {
        self.cx.simulate_resize(size(px(width), px(720.)));
        self.draw();
    }
    fn frame(&mut self, milliseconds: u64) -> usize {
        self.cx
            .executor()
            .advance_clock(Duration::from_millis(milliseconds));
        let count = self.cx.update(|window, cx| window.simulate_next_frame(cx));
        self.draw();
        count
    }
    fn hover(&mut self, selector: Option<&'static str>) {
        let position = selector.map_or_else(
            || gpui::point(px(1250.), px(700.)),
            |selector| self.bounds(selector).center(),
        );
        self.cx
            .simulate_mouse_move(position, None, Modifiers::none());
        self.draw();
    }
    fn default_go_point(&mut self) -> Point<Pixels> {
        self.cx.update(|window, _| {
            let scale = window.scale_factor();
            let quad = window
                .rendered_scene()
                .quads
                .iter()
                .find(|quad| {
                    quad.background.as_solid() == Some(gpui::rgb(0xefefef).into())
                        && quad.bounds.size.height.0 / scale > 15.
                        && quad.bounds.size.height.0 / scale < 30.
                })
                .expect("painted default confirmation button");
            gpui::point(
                px(quad.bounds.center().x.0 / scale),
                px(quad.bounds.center().y.0 / scale),
            )
        })
    }
    fn surface_color(&mut self, selector: &'static str) -> gpui::Hsla {
        let position = self.bounds(selector).center();
        self.cx.update(|window, _| {
            let scale = window.scale_factor();
            let x = f32::from(position.x) * scale;
            let y = f32::from(position.y) * scale;
            window
                .rendered_scene()
                .quads
                .iter()
                .filter(|quad| {
                    let bounds = &quad.bounds;
                    bounds.origin.x.0 <= x
                        && bounds.origin.y.0 <= y
                        && bounds.origin.x.0 + bounds.size.width.0 >= x
                        && bounds.origin.y.0 + bounds.size.height.0 >= y
                        && bounds.size.height.0 / scale >= 24.
                        && bounds.size.height.0 / scale <= 40.
                        && bounds.size.width.0 / scale <= 48.
                })
                .filter_map(|quad| quad.background.as_solid())
                .find(|color| color.a > 0.)
                .expect("painted numeric page surface")
        })
    }
    fn go_appearance(&mut self) -> (gpui::Hsla, gpui::Hsla, f32) {
        let position = self.bounds("pagination-go").center();
        self.cx.update(|window, _| {
            let scale = window.scale_factor();
            let contains = |quad: &&gpui::Quad| {
                let bounds = &quad.bounds;
                let x = f32::from(position.x) * scale;
                let y = f32::from(position.y) * scale;
                bounds.origin.x.0 <= x
                    && bounds.origin.y.0 <= y
                    && bounds.origin.x.0 + bounds.size.width.0 >= x
                    && bounds.origin.y.0 + bounds.size.height.0 >= y
                    && (15. ..30.).contains(&(bounds.size.height.0 / scale))
            };
            let quads = &window.rendered_scene().quads;
            let fill = quads
                .iter()
                .filter(contains)
                .filter_map(|quad| quad.background.as_solid())
                .find(|color| color.a > 0.)
                .unwrap();
            let border = quads
                .iter()
                .filter(contains)
                .find(|quad| quad.border_color.a > 0.)
                .unwrap();
            (
                fill,
                border.border_color,
                border.border_widths.top.0 / scale,
            )
        })
    }
    fn key_phase(&mut self, stroke: &'static str, down: bool) {
        let keystroke = Keystroke::parse(stroke).unwrap();
        self.cx.update(|window, cx| {
            window.dispatch_event(
                if down {
                    PlatformInput::KeyDown(KeyDownEvent {
                        keystroke,
                        is_held: false,
                        prefer_character_input: false,
                    })
                } else {
                    PlatformInput::KeyUp(KeyUpEvent { keystroke })
                },
                cx,
            );
        });
        self.draw();
    }
    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            edit(consumer);
            cx.notify();
        });
        self.draw();
    }
    fn bounds(&mut self, selector: &'static str) -> Bounds<Pixels> {
        self.cx
            .debug_bounds(selector)
            .unwrap_or_else(|| panic!("Missing {selector}"))
    }
    fn click(&mut self, selector: &'static str) {
        let center = self.bounds(selector).center();
        self.cx.simulate_click(center, Modifiers::none());
        self.draw();
    }
    fn pointer(&mut self, selector: &'static str, down: bool) {
        let position = self.bounds(selector).center();
        self.cx.update(|window, cx| {
            let event = if down {
                PlatformInput::MouseDown(MouseDownEvent {
                    position,
                    button: MouseButton::Left,
                    click_count: 1,
                    ..Default::default()
                })
            } else {
                PlatformInput::MouseUp(MouseUpEvent {
                    position,
                    button: MouseButton::Left,
                    ..Default::default()
                })
            };
            window.dispatch_event(event, cx);
        });
        self.draw();
    }
    fn key(&mut self, stroke: &'static str) {
        let keystroke = Keystroke::parse(stroke).unwrap();
        self.cx.update(|window, cx| {
            window.dispatch_event(
                PlatformInput::KeyDown(KeyDownEvent {
                    keystroke: keystroke.clone(),
                    is_held: false,
                    prefer_character_input: false,
                }),
                cx,
            );
            window.dispatch_event(PlatformInput::KeyUp(KeyUpEvent { keystroke }), cx);
        });
        self.draw();
    }
    fn focus_editor(&mut self, simple: bool) {
        let state = self.state();
        self.cx.update(|window, cx| {
            let editor = if simple {
                state.read(cx).simple_input()
            } else {
                state.read(cx).quick_input()
            };
            editor.read(cx).focus_handle(cx).focus(window, cx);
        });
        self.draw();
    }
    fn input(&mut self, simple: bool, text: &str) {
        let state = self.state();
        self.cx.update(|window, cx| {
            let editor = if simple {
                state.read(cx).simple_input()
            } else {
                state.read(cx).quick_input()
            }
            .clone();
            editor.update(cx, |input, cx| {
                input.select_all(window, cx);
                input.replace_text_in_range(None, text, window, cx);
            });
        });
        self.draw();
    }
    fn focused_page(&mut self, page: u64) -> bool {
        let state = self.state();
        self.cx.update(|window, cx| {
            state
                .read(cx)
                .items
                .get(&PaginationItemKind::Page(page))
                .is_some_and(|item| item.focus.is_focused(window))
        })
    }
}

/// Keep the deterministic test shaper, but emit small glyph textures so scene
/// ordering between native labels and translucent surfaces is observable.
struct PaintTextSystem;
impl gpui::PlatformTextSystem for PaintTextSystem {
    fn add_fonts(&self, fonts: Vec<std::borrow::Cow<'static, [u8]>>) -> gpui::Result<()> {
        gpui::NoopTextSystem.add_fonts(fonts)
    }
    fn all_font_names(&self) -> Vec<String> {
        gpui::NoopTextSystem.all_font_names()
    }
    fn font_id(&self, font: &gpui::Font) -> gpui::Result<gpui::FontId> {
        gpui::NoopTextSystem.font_id(font)
    }
    fn font_metrics(&self, font: gpui::FontId) -> gpui::FontMetrics {
        gpui::NoopTextSystem.font_metrics(font)
    }
    fn typographic_bounds(
        &self,
        font: gpui::FontId,
        glyph: gpui::GlyphId,
    ) -> gpui::Result<Bounds<f32>> {
        gpui::NoopTextSystem.typographic_bounds(font, glyph)
    }
    fn advance(&self, font: gpui::FontId, glyph: gpui::GlyphId) -> gpui::Result<gpui::Size<f32>> {
        gpui::NoopTextSystem.advance(font, glyph)
    }
    fn glyph_for_char(&self, font: gpui::FontId, ch: char) -> Option<gpui::GlyphId> {
        gpui::NoopTextSystem.glyph_for_char(font, ch)
    }
    fn glyph_raster_bounds(
        &self,
        _: &gpui::RenderGlyphParams,
    ) -> gpui::Result<Bounds<gpui::DevicePixels>> {
        Ok(Bounds {
            origin: gpui::point(gpui::DevicePixels(0), gpui::DevicePixels(-8)),
            size: gpui::size(gpui::DevicePixels(3), gpui::DevicePixels(8)),
        })
    }
    fn rasterize_glyph(
        &self,
        _: &gpui::RenderGlyphParams,
        bounds: Bounds<gpui::DevicePixels>,
    ) -> gpui::Result<(gpui::Size<gpui::DevicePixels>, Vec<u8>)> {
        Ok((
            bounds.size,
            vec![255; (bounds.size.width.0 * bounds.size.height.0) as usize],
        ))
    }
    fn layout_line(
        &self,
        text: &str,
        font_size: Pixels,
        runs: &[gpui::FontRun],
    ) -> gpui::LineLayout {
        gpui::NoopTextSystem.layout_line(text, font_size, runs)
    }
    fn recommended_rendering_mode(
        &self,
        font: gpui::FontId,
        size: Pixels,
    ) -> gpui::TextRenderingMode {
        gpui::NoopTextSystem.recommended_rendering_mode(font, size)
    }
}

#[test]
fn pagination_native_disabled_go_paints_translucent_surface_before_label_glyphs() {
    let cx = TestAppContext::build_with_text_system(
        gpui::TestDispatcher::new(0),
        None,
        std::sync::Arc::new(PaintTextSystem),
    );
    let mut f = Fixture::with_context(cx);
    f.edit(|view| {
        view.quick = QuickJumper::Confirm;
        view.disabled = Some(true);
    });
    let bounds = f.bounds("pagination-go");
    f.cx.update(|window, _| {
        let scale = window.scale_factor();
        let contains = |point: Point<gpui::ScaledPixels>| {
            bounds.contains(&gpui::point(px(point.x.0 / scale), px(point.y.0 / scale)))
        };
        let scene = window.rendered_scene();
        let fill = scene
            .quads
            .iter()
            .find(|quad| {
                quad.background
                    .as_solid()
                    .is_some_and(|color| color.a > 0.4 && color.a < 0.5)
            })
            .unwrap();
        let glyph_orders = scene
            .monochrome_sprites
            .iter()
            .filter(|sprite| contains(sprite.bounds.center()))
            .map(|sprite| sprite.order)
            .chain(
                scene
                    .subpixel_sprites
                    .iter()
                    .filter(|sprite| contains(sprite.bounds.center()))
                    .map(|sprite| sprite.order),
            )
            .collect::<Vec<_>>();
        assert!(
            !glyph_orders.is_empty(),
            "paint-order regression must observe real glyph primitives"
        );
        assert!(
            glyph_orders.iter().all(|order| *order > fill.order),
            "disabled surface must paint before every label glyph"
        );
    });
}

#[test]
fn pagination_native_pointer_and_tab_enter_space_activate_once() {
    let mut f = Fixture::new();
    f.click("pagination-page-7");
    assert_eq!(f.current(), 7);
    f.key("space");
    assert_eq!(
        f.events.borrow().len(),
        1,
        "Activating the accepted page is silent"
    );
    f.key("tab");
    f.key("enter");
    assert_eq!(f.current(), 8);
    f.click("pagination-next");
    assert_eq!(f.current(), 9);
    f.key("space");
    assert_eq!(f.current(), 10);
    assert_eq!(f.events.borrow().len(), 4);
}

#[test]
fn pagination_native_disabling_and_unmount_cancel_held_pointer_actions() {
    let mut f = Fixture::new();
    f.pointer("pagination-next", true);
    f.edit(|view| view.disabled = Some(true));
    f.edit(|view| view.disabled = Some(false));
    f.pointer("pagination-next", false);
    assert_eq!(f.current(), 6);
    assert!(f.events.borrow().is_empty());
    f.pointer("pagination-next", true);
    f.edit(|view| view.shown = false);
    assert!(!f.state().read_with(&f.cx, |state, _| state.mounted));
    f.edit(|view| view.shown = true);
    f.pointer("pagination-next", false);
    assert_eq!(f.current(), 6);
    f.click("pagination-next");
    assert_eq!(f.current(), 7);
}

#[test]
fn pagination_native_scope_updates_and_state_notifications_reach_cached_parent() {
    let mut f = Fixture::new();
    let before = f.bounds("pagination-page-6").size.height;
    let renders = f.consumer.read_with(&f.cx, |view, _| view.renders.get());
    let scope = f.scope.clone();
    f.cx.update(|_, cx| {
        scope
            .update_patch(cx, |patch| {
                patch.size = Some(Size::Small);
                patch.locale.locale = Some(Locale::zh_cn());
                patch.theme.get_or_insert_default().algorithms = Some(AlgorithmChain::dark());
            })
            .unwrap()
    });
    f.draw();
    assert!(f.bounds("pagination-page-6").size.height < before);
    assert!(f.consumer.read_with(&f.cx, |view, _| view.renders.get()) > renders);
    if let Some(tree) = f.cx.update(|window, _| window.debug_a11y_tree_json()) {
        assert!(tree.contains("分页"));
    }
    let state = f.state();
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.set_current(20, window, cx)));
    f.draw();
    assert!(f.cx.debug_bounds("pagination-page-20").is_some());
    assert!(f.events.borrow().is_empty());
}

#[test]
fn pagination_native_quick_enter_blur_and_page_click_suppression() {
    let mut f = Fixture::new();
    f.edit(|view| view.quick = QuickJumper::Blur);
    f.focus_editor(false);
    f.input(false, "12");
    f.key("enter");
    assert_eq!(f.current(), 12);
    f.input(false, "25");
    f.click("pagination-after");
    assert_eq!(f.current(), 25);
    f.focus_editor(false);
    f.input(false, "12");
    f.click("pagination-next");
    assert_eq!(f.current(), 26);
    assert_eq!(f.events.borrow().len(), 3);
}

#[test]
fn pagination_native_select_keyboard_orders_size_change_before_page_change() {
    let mut f = Fixture::new();
    f.edit(|view| view.show_size = true);
    let state = f.state();
    f.cx.update(|window, cx| {
        state
            .read(cx)
            .size_selector()
            .read(cx)
            .focus_handle(cx)
            .focus(window, cx)
    });
    f.draw();
    f.key("down");
    f.key("down");
    f.key("enter");
    assert_eq!(state.read_with(&f.cx, |state, _| state.page_size()), 20);
    assert_eq!(f.current(), 6);
    let events = f.events.borrow();
    assert!(
        matches!(events[0], PaginationEvent::ShowSizeChange(change) if change.current == 6 && change.page_size == 20)
    );
    assert!(
        matches!(events[1], PaginationEvent::Change(change) if change.current == 6 && change.page_size == 20)
    );
    assert_eq!(events.len(), 2);
}

#[test]
fn pagination_native_simple_editor_arrows_and_readonly_keep_correct_focus() {
    let mut f = Fixture::new();
    f.edit(|view| view.mode = PaginationMode::Simple);
    f.focus_editor(true);
    f.input(true, "12");
    f.key("enter");
    assert_eq!(f.current(), 12);
    f.key("up");
    assert_eq!(f.current(), 11);
    f.key("down");
    assert_eq!(f.current(), 12);
    f.edit(|view| view.mode = PaginationMode::SimpleReadOnly);
    assert!(f.state().read_with(&f.cx, |state, cx| {
        state.simple_input().read(cx).is_disabled()
    }));
    f.key("down");
    assert_eq!(f.current(), 12);
    f.click("pagination-next");
    assert_eq!(f.current(), 13);
}

#[test]
fn pagination_native_replacing_state_under_stable_id_mounts_new_controller() {
    let mut f = Fixture::new();
    let old = f.state();
    let replacement =
        f.cx.update(|window, cx| cx.new(|cx| PaginationState::new(window, cx)));
    f.edit(|view| view.state = replacement.clone());
    assert!(replacement.read_with(&f.cx, |state, _| state.mounted));
    assert!(!old.read_with(&f.cx, |state, _| state.mounted));
    f.click("pagination-next");
    assert_eq!(f.current(), 7);
    assert_eq!(old.read_with(&f.cx, |state, _| state.current()), 6);
}

#[test]
fn pagination_native_disappearing_page_and_suppressed_item_do_not_trap_focus() {
    let mut f = Fixture::new();
    f.click("pagination-page-5");
    assert!(f.focused_page(5));
    let state = f.state();
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.set_current(30, window, cx)));
    f.draw();
    assert!(f.focused_page(30));
    f.edit(|view| view.suppressed = Some(PaginationItemKind::Page(30)));
    assert!(f.cx.debug_bounds("pagination-page-30").is_none());
    assert!(
        !f.focused_page(30),
        "A suppressed item must not own focus or a tab stop"
    );
}

#[test]
fn pagination_native_responsive_breakpoints_preserve_explicit_and_provider_sizes() {
    let mut f = Fixture::new();
    f.edit(|view| {
        view.responsive = true;
        view.show_size = true;
        view.quick = QuickJumper::Blur;
    });
    for provider in [None, Some(Size::Small), Some(Size::Large)] {
        for explicit in [None, Some(Size::Small), Some(Size::Large)] {
            let scope = f.scope.clone();
            f.cx.update(|_, cx| {
                scope
                    .update_patch(cx, |patch| patch.size = provider)
                    .unwrap()
            });
            f.edit(|view| view.size = explicit);
            f.resize(1280.);
            let height = f.bounds("pagination-page-6").size.height;
            for width in [575., 576., 577., 991., 992., 993.] {
                let builds = f
                    .consumer
                    .read_with(&f.cx, |view, _| view.select_builds.clone());
                builds.set(0);
                f.resize(width);
                assert_eq!(
                    f.bounds("pagination-page-6").size.height,
                    height,
                    "provider={provider:?}, explicit={explicit:?}, width={width}"
                );
                assert_eq!(
                    builds.get() > 0,
                    width > 576.,
                    "size changer visibility at {width}"
                );
                let state = f.state();
                state.read_with(&f.cx, |state, cx| {
                    assert_eq!(state.show_size_changer, width > 576.);
                    assert_eq!(state.quick_visible(), width > 576.);
                    assert_eq!(state.quick_input().read(cx).is_disabled(), width <= 576.);
                });
                for selector in ["pagination-page-5", "pagination-page-7"] {
                    assert_eq!(
                        f.cx.debug_bounds(selector).is_some(),
                        width > 992.,
                        "ellipsis neighbor {selector} visibility at {width}"
                    );
                }
            }
        }
    }
    assert!(f.events.borrow().is_empty());
}

#[test]
fn pagination_native_hiding_focused_quick_input_does_not_commit_pending_draft() {
    let mut f = Fixture::new();
    f.edit(|view| {
        view.quick = QuickJumper::Blur;
        view.show_size = true;
    });
    f.resize(577.);
    f.focus_editor(false);
    f.input(false, "12");
    f.resize(576.);
    let state = f.state();
    f.cx.update(|window, cx| {
        assert!(state.read(cx).quick_input().read(cx).is_disabled());
        assert!(
            !state
                .read(cx)
                .quick_input()
                .read(cx)
                .focus_handle(cx)
                .is_focused(window)
        );
        assert!(!state.read(cx).quick_visible());
    });
    assert_eq!(f.current(), 6);
    assert!(f.events.borrow().is_empty());
    f.resize(577.);
    assert!(!state.read_with(&f.cx, |state, cx| {
        state.quick_input().read(cx).is_disabled()
    }));
    assert_eq!(f.current(), 6);
}

#[test]
fn pagination_native_default_and_custom_go_support_pointer_and_enter() {
    let mut f = Fixture::new();
    f.edit(|view| view.quick = QuickJumper::Confirm);
    f.focus_editor(false);
    f.input(false, "12");
    let point = f.default_go_point();
    f.cx.simulate_click(point, Modifiers::none());
    f.draw();
    assert_eq!(f.current(), 12);
    f.focus_editor(false);
    f.input(false, "25");
    f.key("tab");
    f.key("enter");
    assert_eq!(f.current(), 25);
    f.edit(|view| view.custom_go = true);
    f.focus_editor(false);
    f.input(false, "17");
    f.click("pagination-custom-go");
    assert_eq!(f.current(), 17);
    f.focus_editor(false);
    f.input(false, "19");
    f.key("tab");
    f.key("enter");
    assert_eq!(f.current(), 19);
    assert_eq!(f.events.borrow().len(), 4);
    f.edit(|view| view.disabled = Some(true));
    f.click("pagination-custom-go");
    assert_eq!(f.events.borrow().len(), 4);
}

#[test]
fn pagination_native_custom_size_renderer_uses_guarded_weak_request_connection() {
    let mut f = Fixture::new();
    f.edit(|view| {
        view.show_size = true;
        view.custom_size = true;
    });
    let state = f.state();
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.set_page_size(11, window, cx)));
    f.draw();
    assert!(f.events.borrow().is_empty());
    f.click("pagination-custom-size");
    assert_eq!(
        f.state().read_with(&f.cx, |state, _| state.page_size()),
        100
    );
    assert_eq!(f.current(), 5);
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.set_page_size(10, window, cx)));
    f.draw();
    f.key("enter");
    assert_eq!(state.read_with(&f.cx, |state, _| state.page_size()), 100);
    assert_eq!(f.events.borrow().len(), 4);
    f.edit(|view| view.disabled = Some(true));
    f.click("pagination-custom-size");
    assert_eq!(f.events.borrow().len(), 4);
}

#[test]
fn pagination_native_retained_total_updates_silently_and_explicit_zero_overrides() {
    let mut f = Fixture::new();
    f.edit(|view| view.total_explicit = false);
    let state = f.state();
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.set_total(1200, window, cx)));
    f.draw();
    assert_eq!(state.read_with(&f.cx, |state, _| state.total()), 1200);
    assert!(f.cx.debug_bounds("pagination-page-120").is_some());
    f.edit(|view| {
        view.total_explicit = true;
        view.total = 0;
    });
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.set_total(800, window, cx)));
    f.draw();
    assert_eq!(state.read_with(&f.cx, |state, _| state.total()), 0);
    assert_eq!(state.read_with(&f.cx, |state, _| state.page_count()), 0);
    f.edit(|view| view.total_explicit = false);
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.set_total(800, window, cx)));
    f.draw();
    assert_eq!(state.read_with(&f.cx, |state, _| state.total()), 800);
    assert!(f.cx.debug_bounds("pagination-page-80").is_some());
    assert!(f.events.borrow().is_empty());
}

#[test]
fn pagination_native_motion_stops_after_settling_and_respects_lifecycle() {
    let mut f = Fixture::new();
    let scope = f.scope.clone();
    f.cx.update(|window, cx| {
        window.activate_window();
        scope
            .update_patch(cx, |patch| patch.motion = Some(true))
            .unwrap();
    });
    f.draw();
    assert_eq!(f.frame(0), 0);
    f.hover(Some("pagination-jump-next"));
    assert_eq!(
        f.frame(100),
        1,
        "hover transition schedules one retained frame"
    );
    assert!(f.state().read_with(&f.cx, |state, _| state.frame_pending));
    assert_eq!(f.frame(200), 1);
    assert_eq!(f.frame(16), 0, "settled hover is idle");
    f.hover(None);
    assert_eq!(f.frame(250), 1);
    assert_eq!(f.frame(16), 0);

    f.click("pagination-page-7");
    f.key("tab");
    f.key("tab");
    let state = f.state();
    f.cx.update(|window, cx| {
        assert!(
            state.read(cx).items[&PaginationItemKind::JumpNext]
                .focus
                .is_focused(window)
        )
    });
    assert!(
        f.frame(100) > 0,
        "keyboard focus animates the jump affordance"
    );
    f.frame(250);
    assert_eq!(f.frame(16), 0);
    f.cx.update(|window, cx| window.blur(cx));
    f.draw();
    let window = f.cx.windows()[0];
    f.cx.simulate_window_visibility_change(window, gpui::WindowVisibility::Hidden);
    f.frame(0);
    assert_eq!(f.frame(16), 0, "a hidden window settles pending motion");

    f.cx.simulate_window_visibility_change(window, gpui::WindowVisibility::Visible);
    f.cx.update(|window, _| window.activate_window());
    f.draw();
    f.hover(Some("pagination-jump-next"));
    f.cx.update(|_, cx| cx.set_reduce_motion(true));
    f.draw();
    f.frame(0);
    assert_eq!(
        f.frame(16),
        0,
        "reduced motion settles without a frame loop"
    );
    f.cx.update(|_, cx| cx.set_reduce_motion(false));
    f.hover(None);
    f.edit(|view| view.disabled = Some(true));
    f.frame(250);
    assert_eq!(f.frame(16), 0);
    f.hover(Some("pagination-jump-next"));
    assert_eq!(f.frame(16), 0, "disabled hover cannot start motion");

    f.edit(|view| view.disabled = Some(false));
    f.frame(250);
    f.hover(None);
    f.frame(250);
    f.hover(Some("pagination-jump-next"));
    f.edit(|view| view.shown = false);
    f.frame(0);
    assert_eq!(f.frame(16), 0, "unmount cancels scheduled work");
}

#[test]
fn pagination_native_hover_and_held_press_paint_distinct_reference_surfaces() {
    let mut f = Fixture::new();
    f.hover(Some("pagination-page-7"));
    let state = f.state();
    assert!(state.read_with(&f.cx, |state, _| {
        state.items[&PaginationItemKind::Page(7)].hovered
    }));
    let scope = f.scope.clone();
    let (hover, pressed) = f.cx.update(|_, cx| {
        let config = scope.snapshot(cx);
        (
            config.theme.tokens.color_bg_text_hover.to_hsla(),
            config.theme.tokens.color_bg_text_active.to_hsla(),
        )
    });
    assert_eq!(f.surface_color("pagination-page-7"), hover);
    f.pointer("pagination-page-7", true);
    assert!(state.read_with(&f.cx, |state, _| {
        state.items[&PaginationItemKind::Page(7)].pressed
    }));
    assert_eq!(
        f.surface_color("pagination-page-7"),
        pressed,
        "held press takes precedence even when GPUI suppresses hover"
    );
    assert_ne!(hover, pressed);
    f.cx.simulate_mouse_up(
        gpui::point(px(1250.), px(700.)),
        MouseButton::Left,
        Modifiers::none(),
    );
    f.draw();
    assert!(!state.read_with(&f.cx, |state, _| {
        state.items[&PaginationItemKind::Page(7)].pressed
    }));
    assert_eq!(f.current(), 6);
    assert!(f.events.borrow().is_empty());
}

#[test]
fn pagination_native_semantic_item_margin_overrides_defaults_in_both_directions() {
    let mut f = Fixture::new();
    for direction in [Direction::Ltr, Direction::Rtl] {
        let scope = f.scope.clone();
        f.cx.update(|_, cx| {
            scope
                .update_patch(cx, |patch| patch.direction = Some(direction))
                .unwrap()
        });
        f.edit(|view| {
            view.size = Some(Size::Small);
            view.styles.item = if direction == Direction::Ltr {
                div().mr(px(13.)).style().clone()
            } else {
                div().ml(px(13.)).style().clone()
            };
        });
        let before = f.bounds("pagination-page-5").center();
        let current = f.bounds("pagination-page-6").center();
        assert_eq!(
            (current.x - before.x).abs(),
            px(24. + 13.),
            "custom inline margin in {direction:?}"
        );
    }
}

#[test]
fn pagination_native_select_customization_cannot_replace_owned_value_or_availability() {
    let mut f = Fixture::new();
    f.edit(|view| {
        view.show_size = true;
        view.override_select = true;
    });
    let state = f.state();
    assert_eq!(
        state.read_with(&f.cx, |state, cx| state.size_selector().read(cx).values()
            [0]
        .value),
        10
    );
    assert_eq!(state.read_with(&f.cx, |state, _| state.page_size()), 10);
    f.edit(|view| view.disabled = Some(true));
    assert!(state.read_with(&f.cx, |state, cx| {
        state
            .size_selector()
            .read(cx)
            .input_state()
            .read(cx)
            .is_disabled()
    }));
    f.cx.update(|window, cx| {
        state
            .read(cx)
            .size_selector()
            .read(cx)
            .focus_handle(cx)
            .focus(window, cx)
    });
    f.key("down");
    assert!(!state.read_with(&f.cx, |state, cx| state.size_selector().read(cx).is_open()));
    assert!(f.events.borrow().is_empty());
}

#[test]
fn pagination_native_confirm_press_cannot_cross_controller_replacement() {
    let mut f = Fixture::new();
    f.edit(|view| view.custom_go = true);
    f.focus_editor(false);
    f.input(false, "12");
    f.pointer("pagination-custom-go", true);
    let old = f.state();
    let replacement = f.cx.update(|window, cx| {
        let state = cx.new(|cx| PaginationState::new(window, cx));
        let editor = state.read(cx).quick_input().clone();
        editor.update(cx, |input, cx| input.set_value("19", window, cx));
        state
    });
    f.edit(|view| view.state = replacement.clone());
    f.pointer("pagination-custom-go", false);
    assert_eq!(f.current(), 6);
    assert!(f.events.borrow().is_empty());
    f.click("pagination-custom-go");
    assert_eq!(f.current(), 19);
    assert_eq!(old.read_with(&f.cx, |state, _| state.current()), 6);
    assert_eq!(f.events.borrow().len(), 1);
}

#[test]
fn pagination_native_go_paints_reference_pointer_space_enter_and_focus_states() {
    let mut f = Fixture::new();
    f.edit(|view| view.quick = QuickJumper::Confirm);
    let rgb = |value| gpui::Hsla::from(gpui::rgb(value));
    assert_eq!(f.go_appearance(), (rgb(0xefefef), rgb(0x767676), 1.));
    f.hover(Some("pagination-go"));
    assert_eq!(f.go_appearance(), (rgb(0xe5e5e5), rgb(0x4f4f4f), 1.));
    f.pointer("pagination-go", true);
    assert_eq!(f.go_appearance(), (rgb(0xf5f5f5), rgb(0x8d8d8d), 1.));
    f.cx.simulate_mouse_move(
        gpui::point(px(1250.), px(700.)),
        Some(MouseButton::Left),
        Modifiers::none(),
    );
    f.draw();
    assert_eq!(f.go_appearance().0, rgb(0xefefef));
    let position = f.bounds("pagination-go").center();
    f.cx.simulate_mouse_move(position, Some(MouseButton::Left), Modifiers::none());
    f.draw();
    assert_eq!(f.go_appearance().0, rgb(0xf5f5f5));
    f.pointer("pagination-go", false);
    assert_eq!(f.go_appearance().0, rgb(0xe5e5e5));
    f.hover(None);
    f.focus_editor(false);
    f.input(false, "12");
    f.key("tab");
    assert_eq!(f.go_appearance(), (rgb(0xefefef), rgb(0x101010), 2.));
    f.key_phase("space", true);
    assert_eq!(f.go_appearance(), (rgb(0xf5f5f5), rgb(0x101010), 2.));
    assert!(f.events.borrow().is_empty());
    f.key_phase("space", false);
    assert_eq!(f.current(), 12);
    assert_eq!(f.go_appearance().0, rgb(0xefefef));
    f.focus_editor(false);
    f.input(false, "19");
    f.key("tab");
    f.key_phase("enter", true);
    assert_eq!(f.go_appearance(), (rgb(0xefefef), rgb(0x101010), 2.));
    assert!(
        !f.state()
            .read_with(&f.cx, |state, _| state.confirm_space_pressed)
    );
    f.key_phase("enter", false);
    assert_eq!(f.current(), 19);
    assert_eq!(f.events.borrow().len(), 2);
    assert_eq!(
        f.frame(16),
        0,
        "native Go states schedule no idle animation"
    );
}

#[test]
fn pagination_native_go_held_visuals_cancel_on_every_lifecycle_edge() {
    for reason in [
        "blur",
        "disabled",
        "hidden",
        "responsive",
        "unmount",
        "replacement",
    ] {
        let mut f = Fixture::new();
        f.edit(|view| view.quick = QuickJumper::Confirm);
        f.focus_editor(false);
        f.input(false, "12");
        f.key("tab");
        let old = f.state();
        f.key_phase("space", true);
        assert!(old.read_with(&f.cx, |state, _| state.confirm_space_pressed));
        match reason {
            "blur" => {
                f.cx.update(|window, cx| window.blur(cx));
                f.draw();
            }
            "disabled" => {
                f.edit(|view| view.disabled = Some(true));
                f.edit(|view| view.disabled = Some(false));
            }
            "hidden" => {
                f.edit(|view| view.quick = QuickJumper::Hidden);
                f.edit(|view| view.quick = QuickJumper::Confirm);
            }
            "responsive" => {
                f.resize(576.);
                f.resize(1280.);
            }
            "unmount" => {
                f.edit(|view| view.shown = false);
                f.edit(|view| view.shown = true);
            }
            "replacement" => {
                let replacement =
                    f.cx.update(|window, cx| cx.new(|cx| PaginationState::new(window, cx)));
                f.edit(|view| view.state = replacement);
            }
            _ => unreachable!(),
        }
        assert!(
            old.read_with(&f.cx, |state, _| !state.confirm_pointer_pressed
                && !state.confirm_space_pressed),
            "{reason}"
        );
        f.key_phase("space", false);
        assert_eq!(f.current(), 6, "{reason}");
        assert!(f.events.borrow().is_empty(), "{reason}");
        assert_eq!(f.frame(16), 0, "{reason}: no idle frame");
    }
}

#[test]
fn pagination_native_disabled_go_composites_fill_and_outline_over_the_parent_independently() {
    let mut f = Fixture::new();
    f.cx.simulate_scale_factor_change(1.);
    f.edit(|view| {
        view.quick = QuickJumper::Confirm;
        view.disabled = Some(true);
    });
    let (fill, outline, width) = f.go_appearance();
    assert_eq!(width, 1.);
    let composite = |color: gpui::Hsla, parent: [u8; 3]| {
        let color = gpui::Rgba::from(color);
        [color.r, color.g, color.b]
            .map(|channel| channel * 255.)
            .into_iter()
            .zip(parent)
            .map(|(channel, parent)| {
                (channel * color.a + f32::from(parent) * (1. - color.a)).round() as u8
            })
            .collect::<Vec<_>>()
    };
    // Independent source PNG probes on five backdrops; no theme-specific
    // opaque disabled color can satisfy these observations.
    for (parent, expected_fill, expected_outline) in [
        ([255, 255, 255], [238, 238, 238], [208, 208, 208]),
        ([20, 20, 20], [106, 106, 106], [81, 81, 81]),
        ([51, 102, 153], [123, 152, 181], [98, 125, 152]),
        ([128, 128, 128], [167, 167, 167], [139, 139, 139]),
        ([36, 104, 172], [115, 153, 191], [89, 126, 163]),
    ] {
        for (actual, expected) in composite(fill, parent).into_iter().zip(expected_fill) {
            assert!(actual.abs_diff(expected) <= 1);
        }
        for (actual, expected) in composite(outline, parent).into_iter().zip(expected_outline) {
            assert!(actual.abs_diff(expected) <= 1);
        }
    }
    let point = f.bounds("pagination-go").center();
    f.cx.update(|window, _| {
        let scale = window.scale_factor();
        let fill = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| {
                quad.background
                    .as_solid()
                    .is_some_and(|color| color.a > 0.4 && color.a < 0.5)
            })
            .unwrap();
        // The fill starts inside the painted stroke, rather than beneath it.
        let outer = window
            .rendered_scene()
            .quads
            .iter()
            .find(|quad| quad.border_color == outline)
            .unwrap();
        assert!((fill.bounds.origin.x.0 - outer.bounds.origin.x.0 - width * scale).abs() < 0.01);
        assert!((fill.bounds.origin.y.0 - outer.bounds.origin.y.0 - width * scale).abs() < 0.01);
        assert!(fill.bounds.origin.x.0 < f32::from(point.x) * scale);
    });
    f.hover(Some("pagination-go"));
    f.pointer("pagination-go", true);
    assert_eq!(f.go_appearance(), (fill, outline, width));
    f.pointer("pagination-go", false);
    assert!(f.events.borrow().is_empty());
}

#[test]
fn pagination_native_disabled_go_resolves_parent_and_fill_backed_border_bands() {
    for scale in [1., 1.5, 2.] {
        let mut f = Fixture::new();
        f.cx.simulate_scale_factor_change(scale);
        f.edit(|view| {
            view.quick = QuickJumper::Confirm;
            view.disabled = Some(true);
        });
        let (fill, outline, width) = f.go_appearance();
        let composite = |color: gpui::Hsla, parent: f32| {
            let color = gpui::Rgba::from(color);
            color.r * 255. * color.a + parent * (1. - color.a)
        };
        f.cx.update(|window, _| {
            let scene = window.rendered_scene();
            let fill = scene
                .quads
                .iter()
                .find(|quad| {
                    quad.background
                        .as_solid()
                        .is_some_and(|color| color.a > 0.4 && color.a < 0.5)
                })
                .unwrap();
            let border = scene
                .quads
                .iter()
                .find(|quad| quad.border_color == outline)
                .unwrap();
            let expected_inset = width * scale * if scale >= 2. { 0.5 } else { 1. };
            assert!(
                (fill.bounds.origin.x.0 - border.bounds.origin.x.0 - expected_inset).abs() < 0.01
            );
            assert!(
                (fill.bounds.origin.y.0 - border.bounds.origin.y.0 - expected_inset).abs() < 0.01
            );
        });
        if scale >= 2. {
            for (parent, expected_outer, expected_inner) in [(255., 214., 202.), (20., 49., 110.)] {
                assert!((composite(outline, parent).round() - expected_outer).abs() <= 1.);
                assert!(
                    (composite(outline, composite(fill, parent)).round() - expected_inner).abs()
                        <= 1.
                );
            }
        } else {
            for (parent, expected) in [(255., 208.), (20., 81.)] {
                assert!((composite(outline, parent).round() - expected).abs() <= 1.);
            }
        }
    }
}

#[test]
fn pagination_native_disabled_static_text_keeps_reference_color_and_controls_gray() {
    let mut f = Fixture::new();
    f.edit(|view| {
        view.disabled = Some(true);
        view.quick = QuickJumper::Blur;
        view.show_total = true;
        view.mode = PaginationMode::SimpleReadOnly;
        view.custom_go = true;
        view.probe_colors = true;
    });
    let scope = f.scope.clone();
    let (normal, disabled) = f.cx.update(|_, cx| {
        let config = scope.snapshot(cx);
        (
            config.theme.tokens.color_text.to_hsla(),
            config.theme.tokens.color_text_disabled.to_hsla(),
        )
    });
    // Test text rasterizers can omit glyph sprites. Read the actual inherited
    // text style during painting in the same native composition instead.
    let colors = f
        .consumer
        .read_with(&f.cx, |view, _| view.painted_colors.borrow().clone());
    assert_eq!(colors["total"], normal);
    assert_eq!(colors["quick"], normal);
    assert_eq!(colors["item"], disabled);
}

#[test]
fn pagination_native_simple_confirm_uses_the_adjacent_quick_draft_for_every_activation() {
    // Intentional correction of rc-pagination's historical combined-simple-mode
    // Go wiring: Enter and the adjacent confirmation button share one draft.
    for mode in [PaginationMode::Simple, PaginationMode::SimpleReadOnly] {
        let mut f = Fixture::new();
        f.edit(|view| {
            view.mode = mode;
            view.quick = QuickJumper::Confirm;
        });
        f.focus_editor(false);
        f.input(false, "12");
        let point = f.default_go_point();
        f.cx.simulate_click(point, Modifiers::none());
        f.draw();
        assert_eq!(f.current(), 12);
        f.focus_editor(false);
        f.input(false, "19");
        f.key("enter");
        assert_eq!(f.current(), 19);
        f.focus_editor(false);
        f.input(false, "25");
        f.key("tab");
        f.key("enter");
        assert_eq!(f.current(), 25);
        assert_eq!(f.events.borrow().len(), 3);
        assert!(f.events.borrow().iter().all(|event| matches!(event, PaginationEvent::Change(change) if change.source == PaginationChangeSource::QuickJumper)));
    }
}

#[test]
fn pagination_native_confirmation_focus_blurs_and_clears_with_availability() {
    let mut f = Fixture::new();
    f.edit(|view| view.custom_go = true);
    let state = f.state();
    f.focus_editor(false);
    f.input(false, "12");
    f.key("tab");
    f.cx.update(|window, cx| {
        assert!(
            state
                .read(cx)
                .confirm_focus
                .as_ref()
                .unwrap()
                .is_focused(window)
        )
    });
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.blur(window, cx)));
    f.draw();
    f.cx.update(|window, cx| {
        assert!(
            !state
                .read(cx)
                .confirm_focus
                .as_ref()
                .unwrap()
                .is_focused(window)
        )
    });

    f.focus_editor(false);
    f.key("tab");
    f.edit(|view| {
        view.custom_go = false;
        view.quick = QuickJumper::Hidden;
    });
    assert!(state.read_with(&f.cx, |state, _| state.confirm_focus.is_none()));
    f.edit(|view| view.custom_go = true);
    f.focus_editor(false);
    f.key("tab");
    f.resize(576.);
    assert!(state.read_with(&f.cx, |state, _| state.confirm_focus.is_none()));
    f.resize(1280.);
    f.focus_editor(false);
    f.key("tab");
    f.edit(|view| view.disabled = Some(true));
    assert!(state.read_with(&f.cx, |state, _| state.confirm_focus.is_none()));
    f.edit(|view| view.disabled = Some(false));
    f.focus_editor(false);
    f.key("tab");
    f.edit(|view| view.shown = false);
    assert!(state.read_with(&f.cx, |state, _| !state.mounted
        && state.confirm_focus.is_none()));
    assert!(
        f.events.borrow().is_empty(),
        "availability changes must not submit the pending draft"
    );
}

#[test]
fn pagination_native_focus_uses_only_rendered_eligible_controls() {
    let mut f = Fixture::new();
    let state = f.state();
    for mode in [PaginationMode::Full, PaginationMode::SimpleReadOnly] {
        f.click("pagination-before");
        let before =
            f.cx.update(|window, cx| window.focused(cx).expect("focused Before button"));
        f.edit(|view| {
            view.total = 0;
            view.mode = mode;
        });
        f.cx.update(|window, cx| state.update(cx, |state, cx| state.focus(window, cx)));
        f.draw();
        f.cx.update(|window, _| {
            assert!(
                before.is_focused(window),
                "empty {mode:?} must not focus an unattached fallback"
            )
        });
    }
    f.edit(|view| view.mode = PaginationMode::Simple);
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.focus(window, cx)));
    f.draw();
    f.cx.update(|window, cx| {
        assert!(
            state
                .read(cx)
                .simple_input()
                .read(cx)
                .focus_handle(cx)
                .is_focused(window),
            "the visible editable simple field remains focusable when empty"
        )
    });

    f.edit(|view| {
        view.total = 500;
        view.mode = PaginationMode::Full;
        view.only_jumps = true;
    });
    f.click("pagination-before");
    f.cx.update(|window, cx| state.update(cx, |state, cx| state.focus(window, cx)));
    f.draw();
    f.cx.update(|window, cx| {
        let state = state.read(cx);
        let first = &state.items[&PaginationItemKind::JumpPrevious].focus;
        assert_eq!(state.focus_handle(cx), *first);
        assert!(
            first.is_focused(window),
            "fallback follows the first enabled rendered item in model order"
        );
    });
    assert!(f.events.borrow().is_empty());
}
