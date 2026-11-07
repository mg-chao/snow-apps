// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::{
    cell::{Cell, RefCell},
    rc::Rc,
    sync::Arc,
};

use gpui::{
    AppContext, Background, Bounds, Context, Element, Entity, IntoElement, Pixels, Render,
    RenderOnce, Role, TestAppContext, Window, WindowHandle, accesskit, div, point, px, rgb, size,
};

use super::*;
use crate::{
    ConfigPatch, UiRoot,
    theme::{AlgorithmChain, ComponentThemeConfig, ThemeConfig},
};

const ROOT: u32 = 0x13579b;
const ICON: u32 = 0xabc123;
const TITLE: u32 = 0x123456;
const SUBTITLE: u32 = 0x654321;
const EXTRA: u32 = 0xface12;
const BODY: u32 = 0x12face;
const CUSTOM: u32 = 0x987654;
const FIRST: u32 = 0x345678;
const SECOND: u32 = 0x876543;

struct Probe {
    visible: bool,
    renders: Rc<Cell<usize>>,
    scope: ConfigScope,
    status: ResultStatus,
    title: &'static str,
    sub_title: &'static str,
    show_icon: bool,
    custom_icon: bool,
    custom_title: bool,
    custom_subtitle: bool,
    custom_multiline: bool,
    actions: usize,
    extra_text: &'static str,
    multi_text: bool,
    body: bool,
    width: f32,
    styles: ResultStyles,
    root_style: StyleRefinement,
    context: Rc<RefCell<Option<ResultStyleContext>>>,
    semantics: Rc<RefCell<accesskit::Node>>,
}

impl Probe {
    fn new(scope: ConfigScope) -> Self {
        Self {
            visible: true,
            renders: Rc::default(),
            scope,
            status: ResultStatus::Info,
            title: "Complete",
            sub_title: "The operation is complete.",
            show_icon: true,
            custom_icon: false,
            custom_title: false,
            custom_subtitle: false,
            custom_multiline: false,
            actions: 0,
            extra_text: "",
            multi_text: false,
            body: false,
            width: 700.,
            styles: ResultStyles::default(),
            root_style: StyleRefinement::default(),
            context: Rc::default(),
            semantics: Rc::new(RefCell::new(accesskit::Node::new(Role::Group))),
        }
    }
}

impl Render for Probe {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.renders.set(self.renders.get() + 1);
        if !self.visible {
            return div().w(px(self.width));
        }
        let context = self.context.clone();
        let mut styles = ResultStyles {
            root: StyleRefinement::default().bg(rgb(ROOT)),
            icon: StyleRefinement::default().bg(rgb(ICON)),
            title: StyleRefinement::default().bg(rgb(TITLE)),
            sub_title: StyleRefinement::default().bg(rgb(SUBTITLE)),
            extra: StyleRefinement::default().bg(rgb(EXTRA)),
            body: StyleRefinement::default().bg(rgb(BODY)),
        };
        styles.refine(&self.styles);
        let mut result = Result::new("result-probe")
            .scope(self.scope.clone())
            .status(self.status)
            .title(self.title)
            .sub_title(self.sub_title)
            .show_icon(self.show_icon)
            .styles(styles)
            .styles_fn(move |value, _, _| {
                *context.borrow_mut() = Some(*value);
                ResultStyles::default()
            })
            .automation_id("result.probe")
            .accessibility_label("Operation result")
            .accessibility_description("Status and available actions");
        result.style().refine(&self.root_style);
        if self.custom_icon {
            result = result.icon_content(div().w(px(40.)).h(px(30.)).bg(rgb(CUSTOM)));
        }
        if self.custom_title {
            result = result.title_content(div().h(px(32.)));
        }
        if self.custom_subtitle {
            result = result.sub_title_content(div().h(px(22.)));
        }
        if self.custom_multiline {
            result = result.sub_title_content("Completed.\nReady.");
        }
        for index in 0..self.actions {
            result = result.extra(
                div()
                    .w(px(60.))
                    .h(px(32.))
                    .flex_shrink_0()
                    .bg(rgb(if index == 0 { FIRST } else { SECOND })),
            );
        }
        if !self.extra_text.is_empty() {
            result = result.extra(self.extra_text);
        }
        if self.multi_text {
            result = result
                .extra("A second distinct action label")
                .child("First body paragraph with several words")
                .child("Second body paragraph with other words");
        }
        if self.body {
            let config = self.scope.snapshot(cx);
            result = result.child(div().h(px((config.theme.tokens.font_size
                * config.theme.tokens.line_height)
                as f32)));
        }
        let element = result.render(window, cx).into_element();
        let mut node = accesskit::Node::new(element.a11y_role().unwrap());
        element.write_a11y_info(&mut node);
        *self.semantics.borrow_mut() = node;
        div().w(px(self.width)).child(element)
    }
}

struct CachedProbe(Entity<Probe>);
impl Render for CachedProbe {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        self.0
            .clone()
            .cached(StyleRefinement::default().w(px(900.)).h(px(900.)))
    }
}

struct Fixture {
    cx: TestAppContext,
    window: WindowHandle<UiRoot>,
    probe: Entity<Probe>,
    scope: ConfigScope,
}

impl Fixture {
    fn new(edit: impl FnOnce(&mut Probe)) -> Self {
        Self::configured(ConfigPatch::default(), edit)
    }

    fn configured(patch: ConfigPatch, edit: impl FnOnce(&mut Probe)) -> Self {
        let mut cx = TestAppContext::single();
        let mut probe = None;
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let root_scope = ConfigScope::new_window(cx);
            let child_scope = root_scope.child(patch, cx).unwrap();
            let entity = cx.new(|_| {
                let mut value = Probe::new(child_scope.clone());
                edit(&mut value);
                value
            });
            let cached = cx.new(|_| CachedProbe(entity.clone()));
            probe = Some(entity);
            scope = Some(child_scope);
            UiRoot::new(root_scope, cached, window, cx)
        });
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

    fn edit(&mut self, edit: impl FnOnce(&mut Probe)) {
        self.probe.update(&mut self.cx, |probe, cx| {
            edit(probe);
            cx.notify();
        });
        self.draw();
    }

    fn patch(&mut self, edit: impl FnOnce(&mut ConfigPatch)) {
        self.cx
            .update(|cx| self.scope.update_patch(cx, edit).unwrap());
        self.draw();
    }

    fn rect(&mut self, color: u32) -> Option<Bounds<Pixels>> {
        self.cx
            .update_window(self.window.into(), |_, window, _| {
                let scale = window.scale_factor();
                let background: Background = rgb(color).into();
                window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|q| q.background == background)
                    .map(|q| {
                        Bounds::new(
                            point(
                                px(q.bounds.origin.x.0 / scale),
                                px(q.bounds.origin.y.0 / scale),
                            ),
                            size(
                                px(q.bounds.size.width.0 / scale),
                                px(q.bounds.size.height.0 / scale),
                            ),
                        )
                    })
            })
            .unwrap()
    }

    fn bounds(&mut self, color: u32) -> Bounds<Pixels> {
        self.rect(color)
            .unwrap_or_else(|| panic!("visible Result slot {color:06x}"))
    }

    fn context(&self) -> ResultStyleContext {
        self.probe
            .read_with(&self.cx, |probe, _| probe.context.borrow().unwrap())
    }
}

fn near(actual: Pixels, expected: f32) {
    assert!(
        (f32::from(actual) - expected).abs() < 1.01,
        "actual={actual:?}, expected={expected}"
    );
}

#[test]
fn result_defaults_and_empty_content_preserve_slot_presence() {
    let mut f = Fixture::new(|_| {});
    assert_eq!(f.context().status, ResultStatus::Info);
    assert!(f.context().show_icon);
    assert!(f.context().has_title && f.context().has_sub_title);
    assert!(!f.context().has_extra && !f.context().has_body);
    f.edit(|p| {
        p.title = "";
        p.sub_title = "";
        p.show_icon = false;
    });
    assert!(!f.context().has_title && !f.context().has_sub_title && !f.context().show_icon);
    assert!(f.rect(ICON).is_none() && f.rect(TITLE).is_none() && f.rect(SUBTITLE).is_none());
    near(f.bounds(ROOT).size.height, 96.);
}

#[test]
fn result_native_block_layout_matches_reference_margin_collapse() {
    let mut f = Fixture::new(|p| {
        p.actions = 2;
        p.body = true;
    });
    near(f.bounds(ROOT).size.height, 404.);
    near(f.bounds(ICON).origin.y, 48.);
    near(f.bounds(ICON).size.height, 72.);
    near(f.bounds(TITLE).origin.y, 144.);
    near(f.bounds(TITLE).size.height, 32.);
    near(f.bounds(SUBTITLE).origin.y, 184.);
    near(f.bounds(EXTRA).origin.y, 230.);
    near(f.bounds(BODY).origin.y, 286.);
    near(f.bounds(BODY).size.height, 70.);
    f.edit(|p| {
        p.sub_title = "";
        p.actions = 0;
        p.body = false;
    });
    near(f.bounds(ROOT).size.height, 232.);
    f.edit(|p| p.show_icon = false);
    near(f.bounds(TITLE).origin.y, 56.);
    near(f.bounds(ROOT).size.height, 144.);
}

#[test]
fn result_compact_geometry_tracks_effective_aliases() {
    let mut f = Fixture::configured(
        ConfigPatch {
            theme: Some(ThemeConfig {
                algorithms: Some(AlgorithmChain::compact()),
                ..Default::default()
            }),
            ..Default::default()
        },
        |p| {
            p.actions = 2;
            p.body = true;
        },
    );
    near(f.bounds(ROOT).size.height, 308.);
    near(f.bounds(ICON).origin.y, 32.);
    near(f.bounds(ICON).size.height, 60.);
    near(f.bounds(TITLE).origin.y, 108.);
    near(f.bounds(TITLE).size.height, 28.);
    near(f.bounds(SUBTITLE).origin.y, 140.);
    near(f.bounds(EXTRA).origin.y, 176.);
    near(f.bounds(BODY).origin.y, 224.);
}

#[test]
fn result_exception_illustrations_override_custom_and_hidden_icons() {
    let mut f = Fixture::new(|p| {
        p.custom_icon = true;
        p.show_icon = false;
    });
    for status in [
        ResultStatus::Forbidden,
        ResultStatus::NotFound,
        ResultStatus::InternalServerError,
    ] {
        f.edit(|p| p.status = status);
        assert!(f.context().show_icon);
        near(f.bounds(ICON).size.width, 250.);
        near(f.bounds(ICON).size.height, 295.);
        near(f.bounds(ICON).origin.x, 225.);
        assert!(f.rect(CUSTOM).is_none());
    }
    f.edit(|p| {
        p.status = ResultStatus::Success;
        p.show_icon = true;
    });
    near(f.bounds(CUSTOM).size.width, 40.);
    near(f.bounds(CUSTOM).size.height, 30.);
}

#[test]
fn result_custom_slots_replace_text_and_native_actions_follow_direction() {
    let mut f = Fixture::new(|p| {
        p.custom_title = true;
        p.custom_subtitle = true;
        p.actions = 2;
    });
    near(f.bounds(TITLE).size.height, 32.);
    near(f.bounds(SUBTITLE).size.height, 22.);
    near(f.bounds(FIRST).origin.x, 286.);
    near(f.bounds(SECOND).origin.x, 354.);
    f.patch(|p| p.direction = Some(Direction::Rtl));
    assert_eq!(f.context().direction, Direction::Rtl);
    near(f.bounds(FIRST).origin.x, 354.);
    near(f.bounds(SECOND).origin.x, 286.);
}

#[test]
fn result_long_and_bidi_text_wrap_at_available_width_and_multiple_scales() {
    let mut f = Fixture::new(|p| {
        p.title = "Operation complete. A sufficiently long title wraps within the available width.";
        p.sub_title = "مرحبا 500 results. Long explanatory text remains logically accessible.";
    });
    let wide_title = f.bounds(TITLE).size.height;
    let wide_sub = f.bounds(SUBTITLE).size.height;
    f.edit(|p| p.width = 250.);
    assert!(f.bounds(TITLE).size.height > wide_title);
    assert!(f.bounds(SUBTITLE).size.height > wide_sub);
    for scale in [1.5, 2.] {
        f.cx.simulate_window_scale_factor_change(f.window.into(), scale);
        f.patch(|p| p.direction = Some(Direction::Rtl));
        assert!(f.bounds(TITLE).size.height > wide_title);
        assert!(f.bounds(SUBTITLE).size.height > wide_sub);
    }
}

#[test]
fn result_root_has_static_native_semantics_and_never_schedules_idle_frames() {
    let mut f = Fixture::new(|p| {
        p.actions = 2;
        p.body = true;
    });
    let node = f
        .probe
        .read_with(&f.cx, |p, _| p.semantics.borrow().clone());
    assert_eq!(node.role(), Role::Group);
    assert_eq!(node.label(), Some("Operation result"));
    assert_eq!(node.description(), Some("Status and available actions"));
    assert_eq!(node.author_id(), Some("result.probe"));
    assert_eq!(node.live(), None);
    assert!(!node.supports_action(accesskit::Action::Focus));
    assert!(!node.supports_action(accesskit::Action::Click));
    f.cx.update_window(f.window.into(), |_, window, cx| {
        for _ in 0..4 {
            assert_eq!(window.simulate_next_frame(cx), 0);
        }
    })
    .unwrap();
}

#[test]
fn result_styles_and_token_overrides_preserve_native_precedence() {
    let mut f = Fixture::new(|p| {
        p.styles.root = StyleRefinement::default().p(px(10.));
        p.root_style = StyleRefinement::default().p(px(20.));
        p.styles.title = StyleRefinement::default().text_size(px(30.));
    });
    near(f.bounds(ICON).origin.y, 20.);
    near(f.bounds(TITLE).size.height, 40.);
    f.patch(|p| {
        let mut theme = ThemeConfig::default();
        theme.components.set::<ResultTheme>(ResultTokenPatch {
            icon_font_size: Some(40.),
            subtitle_font_size: Some(20.),
            ..Default::default()
        });
        p.theme = Some(theme);
    });
    near(f.bounds(ICON).size.height, 40.);
    near(f.bounds(SUBTITLE).size.height, 31.);
}

#[test]
fn result_component_tokens_merge_cache_and_resolve_all_algorithms() {
    for algorithms in [
        AlgorithmChain::default(),
        AlgorithmChain::dark(),
        AlgorithmChain::compact(),
        AlgorithmChain::dark_compact(),
    ] {
        let theme = ThemeConfig {
            algorithms: Some(algorithms),
            ..Default::default()
        }
        .resolve();
        let tokens = theme.component::<ResultTheme>();
        assert!(Arc::ptr_eq(&tokens, &theme.component::<ResultTheme>()));
        let g = tokens.globals();
        assert_eq!(tokens.title_font_size, g.font_size_heading3);
        assert_eq!(tokens.subtitle_font_size, g.font_size);
        assert_eq!(tokens.icon_font_size, g.font_size_heading3 * 3.);
        assert_eq!(
            tokens.extra_margin,
            ResultMargin::new(g.padding_lg, 0., 0., 0.)
        );
        assert_eq!(
            ResultStyle::from_tokens(&tokens, ResultStatus::Success).icon_color,
            g.color_success.to_hsla()
        );
    }
    let mut parent = ThemeConfig::default();
    parent.components.set::<ResultTheme>(ResultTokenPatch {
        title_font_size: Some(30.),
        ..Default::default()
    });
    let mut child = ThemeConfig::default();
    child.components.set_theme::<ResultTheme>(
        ComponentThemeConfig::new(ResultTokenPatch {
            icon_font_size: Some(40.),
            extra_margin: Some(ResultMargin::all(3.)),
            ..Default::default()
        })
        .with_tokens(crate::theme::TokenPatch {
            font_size: Some(18.),
            ..Default::default()
        }),
    );
    let theme = ThemeConfig::merged(&parent, &child).resolve();
    let tokens = theme.component::<ResultTheme>();
    assert_eq!(tokens.title_font_size, 30.);
    assert_eq!(tokens.icon_font_size, 40.);
    assert_eq!(tokens.extra_margin, ResultMargin::all(3.));
    assert_eq!(tokens.globals().font_size, 18.);
    assert_eq!(theme.tokens.font_size, 14.);
}

#[test]
fn result_config_and_resolver_preserve_inheritance_identity_and_thread_safety() {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<ResultConfig>();
    assert_send_sync::<ResultConfigPatch>();
    assert_send_sync::<ResultStyleResolver>();
    let resolver = ResultStyleResolver::new(|_, _, _| ResultStyles::default());
    assert_eq!(resolver, resolver.clone());
    assert_ne!(
        resolver,
        ResultStyleResolver::new(|_, _, _| ResultStyles::default())
    );
    let parent = ResultConfig {
        styles: ResultStyles {
            root: StyleRefinement::default().p(px(10.)),
            ..Default::default()
        },
        styles_fn: Some(resolver.clone()),
    };
    let child = ResultConfigPatch {
        styles: ResultStyles {
            root: StyleRefinement::default().pt(px(20.)),
            ..Default::default()
        },
        styles_fn: None,
    }
    .resolve(&parent);
    assert_eq!(child.styles.root.padding.top, Some(px(20.).into()));
    assert_eq!(child.styles.root.padding.bottom, Some(px(10.).into()));
    assert_eq!(child.styles_fn, Some(resolver));
}

#[test]
fn result_embedded_illustrations_parse_and_share_bounded_raster_cache() {
    use crate::icons::{IconPalette, IconRenderOptions, IconRenderer};
    let mut f = Fixture::new(|_| {});
    f.cx.update_window(f.window.into(), |_, _window, cx| {
        let renderer = cx.svg_renderer();
        let assets = cx.asset_source().clone();
        let mut cache = IconRenderer::default();
        for status in [
            ResultStatus::Forbidden,
            ResultStatus::NotFound,
            ResultStatus::InternalServerError,
        ] {
            let reference = status.illustration().unwrap();
            assert_eq!(reference.color_model(), IconColorModel::FullColor);
            let options = IconRenderOptions {
                size: size(px(status.illustration_width().unwrap()), px(294.)),
                scale_factor: 1.,
                colors: IconPalette::default(),
                ..Default::default()
            };
            let first = cache
                .rasterize(&renderer, &*assets, &reference, &options)
                .unwrap();
            let second = cache
                .rasterize(
                    &renderer,
                    &*assets,
                    &status.illustration().unwrap(),
                    &options,
                )
                .unwrap();
            assert!(Arc::ptr_eq(&first, &second));
        }
        assert_eq!(cache.statistics().parsed_entries, 3);
        assert_eq!(cache.statistics().raster_entries, 3);
        assert_eq!(cache.statistics().raster_misses, 3);
        assert_eq!(cache.statistics().raster_hits, 3);
    })
    .unwrap();
    assert!(ResultStatus::Info.illustration().is_none());
}

#[test]
fn result_mounted_illustrations_reuse_scene_leases_at_each_scale_and_release_on_unmount() {
    use crate::icons::{IconRenderer, trim_icon_cache};
    for scale in [1., 1.5, 2.] {
        for status in [
            ResultStatus::Forbidden,
            ResultStatus::NotFound,
            ResultStatus::InternalServerError,
        ] {
            let mut f = Fixture::new(|p| p.status = status);
            f.cx.update_window(f.window.into(), |_, window, cx| {
                trim_icon_cache(0, window, cx)
            })
            .unwrap();
            f.cx.simulate_window_scale_factor_change(f.window.into(), scale);
            f.draw();
            let cold = f.cx.update(|cx| cx.global::<IconRenderer>().statistics());
            assert!(
                cold.resident_bytes > 0,
                "mounted {status:?} at scale {scale}"
            );
            if scale == 2. {
                assert_eq!(
                    cold.raster_entries, 0,
                    "2x illustration exceeds the default per-raster CPU cap"
                );
            }
            for _ in 0..4 {
                // Notify the actual cached consumer to force its paint path again.
                f.edit(|_| {});
            }
            let warm = f.cx.update(|cx| cx.global::<IconRenderer>().statistics());
            assert_eq!(
                warm.parse_misses, cold.parse_misses,
                "warm parse {status:?} at {scale}"
            );
            assert_eq!(
                warm.raster_misses, cold.raster_misses,
                "warm raster {status:?} at {scale}"
            );
            // Remove CPU ownership while retaining the live scene, then unmount it.
            f.cx.update_window(f.window.into(), |_, window, cx| {
                trim_icon_cache(0, window, cx)
            })
            .unwrap();
            assert!(
                f.cx.update(|cx| cx.global::<IconRenderer>().statistics())
                    .resident_bytes
                    > 0
            );
            f.edit(|p| p.visible = false);
            f.draw();
            let released = f.cx.update(|cx| cx.global::<IconRenderer>().statistics());
            assert_eq!(
                released.resident_bytes, 0,
                "unmounted {status:?} at {scale}"
            );
            assert_eq!(released.resident_rasters, 0);
        }
    }
}

#[test]
fn result_plain_text_collapses_css_whitespace_and_custom_content_keeps_explicit_breaks() {
    let mut f = Fixture::new(|p| {
        p.title = "  \tOperation \ncomplete\r\n  ";
        p.sub_title = "任务已完成。\nملف report.pdf جاهز للتنزيل。";
    });
    near(f.bounds(TITLE).size.height, 32.);
    near(f.bounds(SUBTITLE).size.height, 22.);
    f.edit(|p| p.styles.sub_title = StyleRefinement::default().whitespace_nowrap());
    near(f.bounds(SUBTITLE).size.height, 22.);
    f.edit(|p| {
        p.custom_multiline = true;
        p.styles.sub_title = StyleRefinement::default();
    });
    near(f.bounds(SUBTITLE).size.height, 44.);
    f.edit(|p| {
        p.title = " \t\n\r\u{000c} ";
        p.sub_title = "";
        p.custom_multiline = false;
    });
    assert!(
        f.context().has_title,
        "whitespace text remains a present semantic title"
    );
    assert!(!f.context().has_sub_title);
    let mut visual = gpui::VisualTestContext::from_window(f.window.into(), &f.cx);
    let empty_title = visual
        .debug_bounds("result-title-slot")
        .expect("present zero-height title");
    near(empty_title.origin.y, 144.);
    near(empty_title.size.height, 0.);
    near(empty_title.size.width, 636.);
    // The empty title's margins collapse through it, matching browser block layout.
    near(f.bounds(ROOT).size.height, 192.);
}

#[test]
fn result_plain_extra_text_wraps_inside_the_available_action_area() {
    let mut f = Fixture::new(|p| {
        p.width = 200.;
        p.title = "Text actions";
        p.sub_title = "";
        p.extra_text = "A longer action description that wraps naturally.";
    });
    near(f.bounds(EXTRA).size.width, 136.);
    assert!(f.bounds(EXTRA).size.height > px(22.));
    assert!(f.context().has_extra);
}

#[test]
fn result_empty_plain_optional_content_is_absent_and_whitespace_remains_present() {
    let empty = Result::new("empty-slots")
        .extra("")
        .extra(String::new())
        .child("")
        .child(String::new());
    assert!(empty.extra.is_empty() && empty.children.is_empty());
    let spaces = Result::new("spaces").extra(" \t\n ").child(" \t\n ");
    assert_eq!(spaces.extra.len(), 1);
    assert_eq!(spaces.children.len(), 1);
    let native = Result::new("native")
        .extra(div().size(px(0.)))
        .child(div().size(px(0.)));
    assert_eq!(native.extra.len(), 1);
    assert_eq!(native.children.len(), 1);
}

#[test]
fn result_css_long_tokens_overflow_while_ordinary_words_wrap() {
    let mut f = Fixture::new(|p| {
        p.width = 360.;
        p.title = "LongUnbrokenResultTitleThatCannotBreakAtAnOrdinaryWordBoundary";
    });
    near(f.bounds(TITLE).size.height, 32.);
    f.edit(|p| {
        p.title = "A long result title wraps to multiple lines when the container is narrow enough."
    });
    assert!(f.bounds(TITLE).size.height > px(32.));
}

#[test]
fn result_repeated_text_children_own_distinct_stable_layout_caches() {
    use crate::behavior::text::css_layout_statistics;
    let mut f = Fixture::new(|p| {
        p.extra_text = "First action text";
        p.multi_text = true;
    });
    let cold = f.cx.update(|cx| css_layout_statistics(cx));
    assert_eq!(
        cold.created, 6,
        "title, subtitle, two actions and two body labels"
    );
    for _ in 0..3 {
        f.edit(|_| {});
    }
    let warm = f.cx.update(|cx| css_layout_statistics(cx));
    assert_eq!(
        warm.created, cold.created,
        "distinct global IDs retain every text child's layout"
    );
    assert!(warm.reused > cold.reused);
    f.edit(|p| p.width = 300.);
    assert_eq!(
        f.cx.update(|cx| css_layout_statistics(cx)).created,
        cold.created,
        "width changes reuse the bounded per-label cache"
    );
    f.edit(|p| p.extra_text = "Changed first action text");
    assert_eq!(
        f.cx.update(|cx| css_layout_statistics(cx)).created,
        cold.created + 1,
        "only the changed logical label rebuilds"
    );
}

#[test]
fn result_reused_id_moves_scope_subscription_and_releases_obsolete_observers() {
    let mut f = Fixture::new(|_| {});
    let old_scope = f.scope.clone();
    let new_scope = f.cx.update(|cx| {
        ConfigScope::application(cx)
            .child(
                ConfigPatch {
                    direction: Some(Direction::Rtl),
                    ..Default::default()
                },
                cx,
            )
            .unwrap()
    });
    f.edit(|p| p.scope = new_scope.clone());
    assert_eq!(f.context().direction, Direction::Rtl);
    let renders = f.probe.read_with(&f.cx, |p, _| p.renders.get());
    f.cx.update(|cx| {
        old_scope
            .update_patch(cx, |p| p.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    assert_eq!(f.probe.read_with(&f.cx, |p, _| p.renders.get()), renders);
    f.cx.update(|cx| {
        new_scope
            .update_patch(cx, |p| p.direction = Some(Direction::Ltr))
            .unwrap()
    });
    f.draw();
    assert_eq!(f.context().direction, Direction::Ltr);
    assert!(f.probe.read_with(&f.cx, |p, _| p.renders.get()) > renders);
    f.edit(|p| p.visible = false);
    let renders = f.probe.read_with(&f.cx, |p, _| p.renders.get());
    f.cx.update(|cx| {
        new_scope
            .update_patch(cx, |p| p.direction = Some(Direction::Rtl))
            .unwrap()
    });
    f.draw();
    assert_eq!(
        f.probe.read_with(&f.cx, |p, _| p.renders.get()),
        renders,
        "unmounted Result releases its observer"
    );
}

#[test]
fn result_exception_images_keep_intrinsic_size_and_logical_start_overflow_at_each_scale() {
    for scale in [1., 1.5, 2.] {
        for direction in [Direction::Ltr, Direction::Rtl] {
            let mut f = Fixture::configured(
                ConfigPatch {
                    direction: Some(direction),
                    ..Default::default()
                },
                |p| p.status = ResultStatus::Forbidden,
            );
            f.cx.simulate_window_scale_factor_change(f.window.into(), scale);
            f.draw();
            for status in [
                ResultStatus::Forbidden,
                ResultStatus::NotFound,
                ResultStatus::InternalServerError,
            ] {
                f.edit(|p| p.status = status);
                let wrapper = f.bounds(ICON);
                let image =
                    f.cx.update_window(f.window.into(), |_, window, _| {
                        let sprite = window
                            .rendered_scene()
                            .polychrome_sprites
                            .first()
                            .expect("exception image sprite");
                        Bounds::new(
                            point(
                                px(sprite.bounds.origin.x.0 / scale),
                                px(sprite.bounds.origin.y.0 / scale),
                            ),
                            size(
                                px(sprite.bounds.size.width.0 / scale),
                                px(sprite.bounds.size.height.0 / scale),
                            ),
                        )
                    })
                    .unwrap();
                let width = status.illustration_width().unwrap();
                near(image.size.width, width);
                near(image.size.height, 294.);
                near(image.origin.y - wrapper.origin.y, 0.);
                near(
                    image.origin.x - wrapper.origin.x,
                    if direction == Direction::Rtl {
                        250. - width
                    } else {
                        0.
                    },
                );
            }
        }
    }
}

#[test]
fn result_narrow_exception_wrappers_follow_reference_inline_start_at_each_scale() {
    for scale in [1., 1.5, 2.] {
        for direction in [Direction::Ltr, Direction::Rtl] {
            for width in [200., 260.] {
                let mut f = Fixture::configured(
                    ConfigPatch {
                        direction: Some(direction),
                        ..Default::default()
                    },
                    |p| {
                        p.status = ResultStatus::Forbidden;
                        p.width = width;
                        p.title = "";
                        p.sub_title = "";
                    },
                );
                f.cx.simulate_window_scale_factor_change(f.window.into(), scale);
                f.draw();
                for status in [
                    ResultStatus::Forbidden,
                    ResultStatus::NotFound,
                    ResultStatus::InternalServerError,
                ] {
                    f.edit(|p| p.status = status);
                    let root = f.bounds(ROOT);
                    let wrapper = f.bounds(ICON);
                    let expected_x = if direction == Direction::Rtl {
                        width - 32. - 250.
                    } else {
                        32.
                    };
                    near(wrapper.origin.x - root.origin.x, expected_x);
                    near(wrapper.origin.y - root.origin.y, 48.);
                    near(wrapper.size.width, 250.);
                    near(wrapper.size.height, 295.);
                    let image =
                        f.cx.update_window(f.window.into(), |_, window, _| {
                            let image = window.rendered_scene().polychrome_sprites.first().unwrap();
                            (
                                image.bounds.origin.x.0 / scale,
                                image.bounds.origin.y.0 / scale,
                                image.bounds.size.width.0 / scale,
                                image.bounds.size.height.0 / scale,
                            )
                        })
                        .unwrap();
                    let natural_width = status.illustration_width().unwrap();
                    near(
                        px(image.0) - root.origin.x,
                        expected_x
                            + if direction == Direction::Rtl {
                                250. - natural_width
                            } else {
                                0.
                            },
                    );
                    near(px(image.1) - root.origin.y, 48.);
                    near(px(image.2), natural_width);
                    near(px(image.3), 294.);
                    near(root.size.height, 415.);
                }
            }
        }
    }
}

#[test]
fn result_exception_alignment_preserves_refined_vertical_margins_and_hidden_layout() {
    let mut f = Fixture::new(|p| {
        p.status = ResultStatus::NotFound;
        p.sub_title = "";
        p.styles.icon = StyleRefinement::default().mt(px(10.)).mb(px(12.));
    });
    near(f.bounds(ICON).origin.y, 58.);
    near(f.bounds(ICON).size.width, 250.);
    near(f.bounds(TITLE).origin.y, 365.);
    near(f.bounds(ROOT).size.height, 453.);
    f.edit(|p| p.styles.icon = StyleRefinement::default().hidden());
    assert!(f.rect(ICON).is_none());
    near(f.bounds(TITLE).origin.y, 56.);
    near(f.bounds(ROOT).size.height, 144.);
}

#[test]
fn result_native_slots_match_independent_browser_presence_geometry_and_themes() {
    let reference: serde_json::Value =
        serde_json::from_str(include_str!("../../fixtures/result/reference.json")).unwrap();
    let mut compared = 0;
    let mut f = Fixture::new(|_| {});
    for case in reference["cases"].as_array().unwrap() {
        let algorithms = match case["theme"].as_str().unwrap() {
            "default" => AlgorithmChain::default(),
            "dark" => AlgorithmChain::dark(),
            "compact" => AlgorithmChain::compact(),
            "dark-compact" => AlgorithmChain::dark_compact(),
            value => panic!("reference theme {value}"),
        };
        f.patch(|p| {
            p.theme = Some(ThemeConfig {
                algorithms: Some(algorithms),
                ..Default::default()
            })
        });
        let results = case["results"].as_array().unwrap();
        let per_direction = results.len() / 2;
        for browser in results {
            let options = &browser["options"];
            // The first 7 * 18 samples in each direction are the complete slot
            // presence matrix plus custom/hidden icons. Visual-demo samples follow.
            if browser["index"].as_u64().unwrap() as usize % per_direction >= 7 * 18 {
                continue;
            }
            if ["customIcon", "rich", "tokenCase", "semantic"]
                .iter()
                .any(|name| options[*name] == true)
                || options["width"].is_number()
            {
                continue;
            }
            let direction = if options["direction"] == "rtl" {
                Direction::Rtl
            } else {
                Direction::Ltr
            };
            f.patch(|p| p.direction = Some(direction));
            f.edit(|p| {
                p.status = match options["status"].as_str() {
                    Some("success") => ResultStatus::Success,
                    Some("error") => ResultStatus::Error,
                    Some("warning") => ResultStatus::Warning,
                    Some("403") => ResultStatus::Forbidden,
                    Some("404") => ResultStatus::NotFound,
                    Some("500") => ResultStatus::InternalServerError,
                    _ => ResultStatus::Info,
                };
                p.title = if options["title"].is_null() {
                    ""
                } else {
                    "Result title"
                };
                p.sub_title = if options["subTitle"].is_null() {
                    ""
                } else {
                    "Additional information about this result."
                };
                p.show_icon = options["showIcon"] != false;
                p.actions = options["actions"].as_u64().unwrap_or(0) as usize;
                p.body = options["body"] == true;
            });
            let root = f.bounds(ROOT);
            let expected_height = browser["height"].as_f64().unwrap() as f32;
            assert!(
                (f32::from(root.size.height) - expected_height).abs() < 1.01,
                "theme={} index={} options={options} native={root:?} browser-height={expected_height}",
                case["theme"],
                browser["index"]
            );
            for (name, marker) in [
                ("icon", ICON),
                ("title", TITLE),
                ("subTitle", SUBTITLE),
                ("extra", EXTRA),
                ("body", BODY),
            ] {
                let actual = f.rect(marker);
                let expected = &browser[name];
                assert_eq!(
                    actual.is_some(),
                    !expected.is_null(),
                    "theme={} options={options} slot={name}",
                    case["theme"]
                );
                if let Some(actual) = actual {
                    for (value, expected) in [
                        (
                            actual.origin.x - root.origin.x,
                            expected["x"].as_f64().unwrap() - browser["x"].as_f64().unwrap(),
                        ),
                        (
                            actual.origin.y - root.origin.y,
                            expected["y"].as_f64().unwrap() - browser["y"].as_f64().unwrap(),
                        ),
                        (actual.size.width, expected["width"].as_f64().unwrap()),
                        (actual.size.height, expected["height"].as_f64().unwrap()),
                    ] {
                        assert!(
                            (f64::from(f32::from(value)) - expected).abs() < 1.01,
                            "theme={} index={} options={options} slot={name} native={actual:?} expected={expected}",
                            case["theme"],
                            browser["index"]
                        );
                    }
                }
            }
            compared += 1;
        }
    }
    assert_eq!(compared, 952);
}
