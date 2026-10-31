// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
};

use gpui::{
    AppContext, Background, Context, DefiniteLength, Element, Entity, Focusable, IntoElement,
    Render, RenderOnce, Role, StyleRefinement, TestAppContext, Window, WindowHandle, accesskit,
    div, px, rgb,
};

use super::*;
use crate::{ConfigPatch, UiRoot};

const ROOT_COLOR: u32 = 0x93ab75;
const CONTENT_COLOR: u32 = 0xebc7ad;
const RAIL_COLOR: u32 = 0x4387bd;
const CHILD_COLOR: u32 = 0xbd43a9;

struct Consumer {
    scope: ConfigScope,
    label: &'static str,
    vertical: bool,
    plain: bool,
    variant: DividerVariant,
    placement: DividerTitlePlacement,
    size: Option<Size>,
    title_margin: Option<DefiniteLength>,
    custom_child: bool,
    width: f32,
    root_style: StyleRefinement,
    content_style: StyleRefinement,
    rail_style: StyleRefinement,
    semantics: Rc<RefCell<accesskit::Node>>,
    renders: Rc<Cell<usize>>,
}

impl Consumer {
    fn new(scope: ConfigScope) -> Self {
        Self {
            scope,
            label: "",
            vertical: false,
            plain: false,
            variant: DividerVariant::Solid,
            placement: DividerTitlePlacement::Center,
            size: None,
            title_margin: None,
            custom_child: false,
            width: 400.,
            root_style: StyleRefinement::default(),
            content_style: StyleRefinement::default(),
            rail_style: StyleRefinement::default(),
            semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::Splitter))),
            renders: Rc::new(Cell::new(0)),
        }
    }
}

impl Render for Consumer {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        let mut content_style = StyleRefinement::default().bg(rgb(CONTENT_COLOR));
        content_style.refine(&self.content_style);
        let mut rail_style = StyleRefinement::default().border_color(rgb(RAIL_COLOR));
        rail_style.refine(&self.rail_style);
        let mut divider = Divider::new("divider-test")
            .scope(self.scope.clone())
            .label(self.label)
            .vertical(self.vertical)
            .plain(self.plain)
            .variant(self.variant)
            .title_placement(self.placement)
            .content_style(content_style)
            .rail_style(rail_style)
            .automation_id("divider-test.separator")
            .accessibility_description("Separates document sections")
            .bg(rgb(ROOT_COLOR));
        divider.style().refine(&self.root_style);
        if let Some(size) = self.size {
            divider = divider.size(size);
        }
        if let Some(margin) = self.title_margin {
            divider = divider.title_margin(margin);
        }
        if self.custom_child {
            divider = divider.child(div().w(px(24.)).h(px(10.)).bg(rgb(CHILD_COLOR)));
        }
        let element = divider.render(window, cx).into_element();
        let mut node = accesskit::Node::new(element.a11y_role().expect("separator role"));
        element.write_a11y_info(&mut node);
        *self.semantics.borrow_mut() = node;
        div()
            .flex()
            .flex_col()
            .items_start()
            .w(px(self.width))
            .child(div().h(px(10.)).flex_shrink_0())
            .child(element)
            .child(div().h(px(10.)).flex_shrink_0())
    }
}

struct CachedPanel(Entity<Consumer>);

impl Render for CachedPanel {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div().size_full().child(
            self.0
                .clone()
                .cached(StyleRefinement::default().w(px(600.)).h(px(240.))),
        )
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    consumer: Entity<Consumer>,
    scope: ConfigScope,
}

#[derive(Clone, Copy, Debug)]
struct Rect {
    x: f32,
    y: f32,
    width: f32,
    height: f32,
}

impl Rect {
    fn right(self) -> f32 {
        self.x + self.width
    }
}

impl Fixture {
    fn new(configure: impl FnOnce(&mut Consumer)) -> Self {
        let mut cx = TestAppContext::single();
        let mut consumer = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let root_scope = ConfigScope::new_window(cx);
            let child_scope = root_scope.child(ConfigPatch::default(), cx).unwrap();
            let content = cx.new(|_| {
                let mut content = Consumer::new(child_scope.clone());
                configure(&mut content);
                content
            });
            let panel = cx.new(|_| CachedPanel(content.clone()));
            consumer = Some(content);
            scope = Some(child_scope);
            UiRoot::new(root_scope, panel, window, cx)
        });
        let mut fixture = Self {
            cx,
            window,
            consumer: consumer.unwrap(),
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

    fn edit(&mut self, edit: impl FnOnce(&mut Consumer)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            edit(consumer);
            cx.notify();
        });
        self.draw();
    }

    fn patch(&mut self, edit: impl FnOnce(&mut ConfigPatch)) {
        self.cx
            .update(|cx| self.scope.update_patch(cx, edit).unwrap());
        self.draw();
    }

    fn rects(&mut self, color: u32) -> Vec<Rect> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let background: Background = rgb(color).into();
                let scale = window.scale_factor();
                let mut rects: Vec<_> = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .filter(|quad| quad.background == background)
                    .map(|quad| Rect {
                        x: quad.bounds.origin.x.0 / scale,
                        y: quad.bounds.origin.y.0 / scale,
                        width: quad.bounds.size.width.0 / scale,
                        height: quad.bounds.size.height.0 / scale,
                    })
                    .collect();
                rects.sort_by(|left, right| left.x.total_cmp(&right.x));
                rects
            })
            .unwrap()
    }

    fn root(&mut self) -> Rect {
        let rects = self.rects(ROOT_COLOR);
        assert_eq!(rects.len(), 1, "one separator surface: {rects:?}");
        rects[0]
    }

    fn title(&mut self) -> Rect {
        let rects = self.rects(CONTENT_COLOR);
        assert_eq!(rects.len(), 1, "one title slot: {rects:?}");
        rects[0]
    }

    fn semantics(&self) -> accesskit::Node {
        self.consumer
            .read_with(&self.cx, |consumer, _| consumer.semantics.borrow().clone())
    }

    fn renders(&self) -> usize {
        self.consumer
            .read_with(&self.cx, |consumer, _| consumer.renders.get())
    }
}

fn near(actual: f32, expected: f32) {
    // Scene quads snap both edges independently to device pixels. Text and
    // flex layout can therefore differ by one pixel from their logical metrics.
    assert!(
        (actual - expected).abs() < 1.01,
        "expected {expected}, got {actual}"
    );
}

#[test]
fn horizontal_spacing_distinguishes_unset_and_explicit_sizes() {
    let mut fixture = Fixture::new(|_| {});
    near(fixture.root().y, 34.);
    near(fixture.root().height, 1.);
    near(fixture.root().width, 400.);
    for (size, margin) in [(Size::Small, 8.), (Size::Medium, 16.), (Size::Large, 24.)] {
        fixture.edit(|consumer| consumer.size = Some(size));
        near(fixture.root().y, 10. + margin);
    }
    fixture.edit(|consumer| {
        consumer.label = "Section";
        consumer.size = None;
    });
    near(fixture.root().y, 26.);
    for (size, margin) in [(Size::Small, 8.), (Size::Medium, 16.), (Size::Large, 16.)] {
        fixture.edit(|consumer| consumer.size = Some(size));
        near(fixture.root().y, 10. + margin);
    }
}

#[test]
fn cached_dividers_follow_provider_sizes_and_local_precedence() {
    let mut fixture = Fixture::new(|_| {});
    let initial_renders = fixture.renders();
    for (size, margin) in [(Size::Small, 8.), (Size::Medium, 16.), (Size::Large, 24.)] {
        fixture.patch(|patch| patch.size = Some(size));
        near(fixture.root().y, 10. + margin);
    }
    assert!(fixture.renders() > initial_renders);
    fixture.edit(|consumer| consumer.size = Some(Size::Small));
    fixture.patch(|patch| patch.size = Some(Size::Medium));
    near(fixture.root().y, 18.);
    fixture.edit(|consumer| consumer.size = None);
    near(fixture.root().y, 26.);
    fixture.patch(|patch| patch.size = None);
    near(fixture.root().y, 34.);
    // Explicit Medium changes spacing even though ConfigScope's effective fallback is Medium.
    fixture.patch(|patch| patch.size = Some(Size::Medium));
    near(fixture.root().y, 26.);
}

#[test]
fn title_and_plain_typography_keep_base_line_height_and_em_padding() {
    let mut fixture = Fixture::new(|consumer| consumer.label = "Section");
    near(fixture.root().height, 16. * 22. / 14.);
    let heading_width = fixture.title().width;
    fixture.edit(|consumer| consumer.plain = true);
    near(fixture.root().height, 22.);
    assert!(fixture.title().width < heading_width);
    fixture.edit(|consumer| consumer.content_style = StyleRefinement::default().text_size(px(20.)));
    near(fixture.root().height, 20. * 22. / 14.);
    assert!(fixture.title().width > heading_width);
}

#[test]
fn logical_and_physical_title_placement_follow_runtime_direction() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.label = "Section";
        consumer.placement = DividerTitlePlacement::Start;
    });
    let start = fixture.title();
    near(start.x, (400. - start.width) * 0.05);
    fixture.patch(|patch| patch.direction = Some(Direction::Rtl));
    let rtl_start = fixture.title();
    near(rtl_start.x, (400. - rtl_start.width) * 0.95);
    fixture.edit(|consumer| consumer.placement = DividerTitlePlacement::End);
    near(fixture.title().x, start.x);
    fixture.edit(|consumer| consumer.placement = DividerTitlePlacement::Left);
    near(fixture.title().x, start.x);
    fixture.edit(|consumer| consumer.placement = DividerTitlePlacement::Right);
    near(fixture.title().x, rtl_start.x);
    fixture.edit(|consumer| consumer.placement = DividerTitlePlacement::Center);
    let centered = fixture.title();
    near(centered.x, (400. - centered.width) / 2.);
}

#[test]
fn empty_and_zero_titles_select_the_correct_native_structure() {
    let mut fixture = Fixture::new(|_| {});
    assert!(fixture.rects(CONTENT_COLOR).is_empty());
    assert_eq!(fixture.rects(RAIL_COLOR).len(), 1);
    fixture.edit(|consumer| consumer.label = "0");
    assert!(fixture.title().width > 32.);
    assert_eq!(fixture.rects(RAIL_COLOR).len(), 2);
    assert_eq!(fixture.semantics().label(), Some("0"));
    fixture.edit(|consumer| consumer.label = "");
    assert!(fixture.rects(CONTENT_COLOR).is_empty());
    near(fixture.root().height, 1.);
}

#[test]
fn vertical_dividers_fill_their_height_and_ignore_title_and_size() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.vertical = true;
        consumer.custom_child = true;
    });
    let root = fixture.root();
    near(root.x, 8.);
    near(root.y, 10. - 0.84);
    assert!(
        root.y < 10.,
        "vertical separators retain Ant's baseline offset"
    );
    near(root.width, 1.);
    near(root.height, 12.6);
    assert!(fixture.rects(CONTENT_COLOR).is_empty());
    assert!(fixture.rects(CHILD_COLOR).is_empty());
    let rails = fixture.rects(RAIL_COLOR);
    assert_eq!(rails.len(), 1);
    near(rails[0].height, 13.);
    for size in [Size::Small, Size::Medium, Size::Large] {
        fixture.edit(|consumer| {
            consumer.size = Some(size);
            consumer.plain = true;
        });
        near(fixture.root().height, 12.6);
        near(fixture.root().x, 8.);
    }
    fixture.edit(|consumer| consumer.root_style = StyleRefinement::default().h(px(60.)));
    near(fixture.root().height, 60.);
    near(fixture.rects(RAIL_COLOR)[0].height, 60.);
}

#[test]
fn separators_have_names_orientation_and_no_input_actions() {
    let mut fixture = Fixture::new(|consumer| consumer.label = "Section");
    let node = fixture.semantics();
    assert_eq!(node.role(), Role::Splitter);
    assert_eq!(node.label(), Some("Section"));
    assert_eq!(node.description(), Some("Separates document sections"));
    assert_eq!(node.author_id(), Some("divider-test.separator"));
    assert_eq!(node.orientation(), Some(accesskit::Orientation::Horizontal));
    assert!(!node.supports_action(accesskit::Action::Focus));
    assert!(!node.supports_action(accesskit::Action::Click));
    fixture.edit(|consumer| consumer.vertical = true);
    assert_eq!(
        fixture.semantics().orientation(),
        Some(accesskit::Orientation::Vertical)
    );
    assert_eq!(fixture.semantics().label(), None);
    fixture
        .cx
        .update_window(fixture.window.into(), |root, window, cx| {
            window.focus_next(cx);
            assert!(
                root.downcast::<UiRoot>()
                    .unwrap()
                    .read(cx)
                    .focus_handle(cx)
                    .is_focused(window)
            );
        })
        .unwrap();
}

#[test]
fn edge_title_margin_including_zero_replaces_the_short_rail() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.label = "Section";
        consumer.placement = DividerTitlePlacement::Start;
        consumer.title_margin = Some(px(0.).into());
    });
    near(fixture.title().x, 0.);
    assert_eq!(fixture.rects(RAIL_COLOR).len(), 1);
    fixture.edit(|consumer| consumer.title_margin = Some(px(28.).into()));
    near(fixture.title().x, 28.);
    fixture.patch(|patch| patch.direction = Some(Direction::Rtl));
    near(fixture.title().right(), 372.);
    fixture.edit(|consumer| consumer.placement = DividerTitlePlacement::Center);
    let centered = fixture.title();
    near(centered.x, (400. - centered.width) / 2.);
    assert_eq!(fixture.rects(RAIL_COLOR).len(), 2);
}

#[test]
fn long_titles_in_narrow_containers_do_not_wrap_or_paint_negative_rails() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.label = "A long section title that cannot fit in this narrow container";
        consumer.width = 40.;
    });
    near(fixture.root().width, 40.);
    near(fixture.root().height, 16. * 22. / 14.);
    assert!(fixture.title().width > 40.);
    assert!(fixture.rects(RAIL_COLOR).is_empty());
}

#[test]
fn native_root_content_and_rail_refinements_control_real_layout_and_strokes() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.label = "Section";
        consumer.root_style = StyleRefinement::default()
            .my(px(7.))
            .ml(px(12.))
            .text_size(px(20.));
        consumer.content_style = StyleRefinement::default().px(px(3.)).h(px(32.));
        consumer.rail_style = StyleRefinement::default().border_t(px(3.));
    });
    let root = fixture.root();
    near(root.x, 12.);
    near(root.y, 17.);
    near(root.height, 32.);
    let rails = fixture.rects(RAIL_COLOR);
    assert_eq!(rails.len(), 2);
    for rail in rails {
        near(rail.height, 3.);
        assert!(rail.width > 0.);
    }
    let refined_title_width = fixture.title().width;
    fixture.edit(|consumer| consumer.content_style = StyleRefinement::default().h(px(32.)));
    // Restoring the 1em default adds 17px on each side at the refined 20px size.
    near(fixture.title().width - refined_title_width, 34.);
}

#[test]
fn configuration_subscription_moves_with_a_reused_divider_id() {
    let mut fixture = Fixture::new(|_| {});
    let old_scope = fixture.scope.clone();
    let new_scope = fixture.cx.update(|cx| {
        ConfigScope::application(cx)
            .child(
                ConfigPatch {
                    size: Some(Size::Small),
                    ..Default::default()
                },
                cx,
            )
            .unwrap()
    });
    fixture.edit(|consumer| consumer.scope = new_scope.clone());
    near(fixture.root().y, 18.);
    let renders = fixture.renders();
    fixture.cx.update(|cx| {
        old_scope
            .update_patch(cx, |patch| patch.size = Some(Size::Large))
            .unwrap()
    });
    fixture.draw();
    assert_eq!(
        fixture.renders(),
        renders,
        "the old scope no longer invalidates this divider"
    );
    fixture.cx.update(|cx| {
        new_scope
            .update_patch(cx, |patch| patch.size = Some(Size::Medium))
            .unwrap()
    });
    fixture.draw();
    near(fixture.root().y, 26.);
    assert!(fixture.renders() > renders);
}

#[test]
fn native_patterns_cover_both_orientations_and_never_schedule_idle_frames() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.label = "Section";
        consumer.variant = DividerVariant::Dashed;
    });
    let paths = |fixture: &mut Fixture| {
        fixture
            .cx
            .update_window(fixture.window.into(), |_, window, _| {
                let color: Background = rgb(RAIL_COLOR).into();
                window
                    .rendered_scene()
                    .paths
                    .iter()
                    .filter(|path| path.color == color)
                    .map(|path| {
                        (
                            path.bounds.size.width.0 / window.scale_factor(),
                            path.bounds.size.height.0 / window.scale_factor(),
                        )
                    })
                    .collect::<Vec<_>>()
            })
            .unwrap()
    };
    assert_eq!(paths(&mut fixture).len(), 2);
    assert!(fixture.rects(RAIL_COLOR).is_empty());
    fixture.edit(|consumer| {
        consumer.vertical = true;
        consumer.variant = DividerVariant::Dotted;
        consumer.rail_style = StyleRefinement::default().border_l(px(5.));
    });
    let vertical = paths(&mut fixture);
    assert_eq!(vertical.len(), 1);
    near(vertical[0].0, 5.);
    assert!(vertical[0].1 > 5.);
    fixture.edit(|consumer| {
        consumer.vertical = false;
        consumer.variant = DividerVariant::Solid;
        consumer.rail_style = StyleRefinement::default().border_dashed();
    });
    assert_eq!(
        paths(&mut fixture).len(),
        2,
        "native border-style overrides refine the rail pattern"
    );
    fixture
        .cx
        .update_window(fixture.window.into(), |_, window, cx| {
            assert_eq!(window.simulate_next_frame(cx), 0);
        })
        .unwrap();
}

#[test]
fn vertical_em_metrics_follow_native_font_size_refinements() {
    let mut fixture = Fixture::new(|consumer| {
        consumer.vertical = true;
        consumer.root_style = StyleRefinement::default().text_size(px(20.));
    });
    near(fixture.root().height, 18.);
    near(fixture.root().y, 8.8);
    near(fixture.rects(RAIL_COLOR)[0].height, 18.);
}
