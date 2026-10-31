// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{UiRoot, date_picker::*};
use gpui::{AppContext, Modifiers, Render, TestAppContext, VisualTestContext, WindowHandle, size};
use std::cell::Cell;

fn now() -> NaiveDateTime {
    NaiveDate::from_ymd_opt(2024, 2, 20)
        .unwrap()
        .and_hms_opt(13, 24, 35)
        .unwrap()
}

struct Consumer<S: PickerSelection> {
    state: Entity<PickerState<S>>,
    scope: ConfigScope,
    arrow: Option<PopupArrow>,
    placement: Option<PopupPlacement>,
    prefix: Option<SharedString>,
    mode: PickerMode,
    panel_mode: Option<PickerPanelMode>,
    format_label: Option<SharedString>,
    custom_icons: bool,
    tall_footer: bool,
    factory_calls: Rc<Cell<usize>>,
    changes: Rc<Cell<usize>>,
}

impl<S: PickerSelection> Render for Consumer<S> {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        let changes = self.changes.clone();
        let mut picker = Picker::<S>::new("api-picker", &self.state)
            .scope(self.scope.clone())
            .options(PickerOptions {
                mode: self.mode,
                ..Default::default()
            })
            .default_open(true)
            .clock(now)
            .on_change(move |_, _, _| changes.set(changes.get() + 1));
        if let Some(arrow) = self.arrow {
            picker = picker.arrow(arrow);
        }
        if let Some(placement) = self.placement {
            picker = picker.placement(placement);
        }
        if let Some(prefix) = &self.prefix {
            picker = picker.prefix(prefix.clone());
        }
        if let Some(mode) = self.panel_mode {
            picker = picker.panel_mode(mode);
        }
        if let Some(label) = self.format_label.clone() {
            picker = picker.format_render(move |_| label.clone());
        }
        if self.tall_footer {
            picker = picker.extra_footer(|_, _| div().h(px(1000.)).child("Extra content"));
        }
        if self.custom_icons {
            let previous = self.factory_calls.clone();
            let next = self.factory_calls.clone();
            let super_previous = self.factory_calls.clone();
            let super_next = self.factory_calls.clone();
            picker = picker
                .prev_icon(move |_, _| {
                    previous.set(previous.get() + 1);
                    div()
                        .debug_selector(|| "api.prev".into())
                        .size(px(12.))
                        .child("P")
                })
                .next_icon(move |_, _| {
                    next.set(next.get() + 1);
                    div()
                        .debug_selector(|| "api.next".into())
                        .size(px(12.))
                        .child("N")
                })
                .super_prev_icon(move |_, _| {
                    super_previous.set(super_previous.get() + 1);
                    div()
                        .debug_selector(|| "api.super-prev".into())
                        .size(px(12.))
                        .child("S")
                })
                .super_next_icon(move |_, _| {
                    super_next.set(super_next.get() + 1);
                    div()
                        .debug_selector(|| "api.super-next".into())
                        .size(px(12.))
                        .child("T")
                });
        }
        div().size_full().p(px(48.)).child(picker)
    }
}

struct Fixture<S: PickerSelection> {
    cx: VisualTestContext,
    state: Entity<PickerState<S>>,
    consumer: Entity<Consumer<S>>,
    _window: WindowHandle<UiRoot>,
}

impl<S: PickerSelection> Fixture<S> {
    fn new(value: S::Value) -> Self {
        let mut cx = TestAppContext::single();
        let mut retained = None;
        let window = cx.add_window(|window, cx| {
            window.activate_window();
            crate::init(cx);
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let state = cx.new(|cx| {
                let mut state = PickerState::<S>::new(window, cx);
                state.set_value(value, window, cx);
                state
            });
            let consumer = cx.new(|_| Consumer {
                state: state.clone(),
                scope: scope.clone(),
                arrow: None,
                placement: None,
                prefix: None,
                mode: PickerMode::Date,
                panel_mode: None,
                format_label: None,
                custom_icons: false,
                tall_footer: false,
                factory_calls: Rc::default(),
                changes: Rc::default(),
            });
            retained = Some((state, consumer.clone()));
            UiRoot::new(scope, consumer, window, cx)
        });
        let (state, consumer) = retained.unwrap();
        let mut fixture = Self {
            cx: VisualTestContext::from_window(window.into(), &cx),
            state,
            consumer,
            _window: window,
        };
        fixture.cx.simulate_resize(size(px(900.), px(720.)));
        fixture.draw();
        fixture
    }
    fn draw(&mut self) {
        for _ in 0..2 {
            self.cx.run_until_parked();
            self.cx.update(|window, cx| window.draw(cx).clear(cx));
        }
        self.cx.run_until_parked();
    }
    fn edit(&mut self, update: impl FnOnce(&mut Consumer<S>)) {
        self.consumer.update(&mut self.cx, |consumer, cx| {
            update(consumer);
            cx.notify();
        });
        self.draw();
    }
    fn patch(&mut self, update: impl FnOnce(&mut crate::ConfigPatch)) {
        let scope = self
            .consumer
            .read_with(&self.cx, |consumer, _| consumer.scope.clone());
        self.cx
            .update(|_, cx| scope.update_patch(cx, update).unwrap());
        self.draw();
    }
    fn layout(&mut self) -> crate::popup::PopupLayout {
        self.cx
            .update(|_, cx| self.state.read(cx).popup.layout(cx).unwrap())
    }
    fn range_arrow(&mut self) -> Option<Bounds<Pixels>> {
        let body = self.layout().bounds;
        self.cx.update(|window, _| {
            let scale = window.scale_factor();
            window.rendered_scene().paths.iter().find_map(|path| {
                let bounds = Bounds::new(
                    point(
                        px(path.bounds.origin.x.0 / scale),
                        px(path.bounds.origin.y.0 / scale),
                    ),
                    size(
                        px(path.bounds.size.width.0 / scale),
                        px(path.bounds.size.height.0 / scale),
                    ),
                );
                ((f32::from(bounds.size.width) - 16.).abs() < 0.01
                    && bounds.top() < body.top()
                    && bounds.top() >= body.top() - px(8.01)
                    && bounds.bottom() <= body.top() + px(0.01))
                .then_some(bounds)
            })
        })
    }
    fn text(&self) -> Vec<SharedString> {
        self.state
            .read_with(&self.cx, |state, cx| state.input_values(cx))
    }
}

#[test]
fn picker_api_range_arrow_uses_range_provider_for_date_and_time_ranges() {
    fn check<S: PickerSelection>(value: S::Value) {
        let mut fixture = Fixture::<S>::new(value);
        assert!(
            fixture.range_arrow().is_some(),
            "the range arrow must paint outside the popup body's native scroll mask"
        );
        let default_top = fixture.layout().bounds.top();
        fixture.patch(|patch| {
            patch.range_picker.arrow = Some(false.into());
            patch.range_picker.need_confirm = Some(false);
            patch.time_picker.arrow = Some(true.into());
            patch.time_picker.need_confirm = Some(true);
            patch.date_picker.arrow = Some(true.into());
        });
        assert!(
            fixture.range_arrow().is_none(),
            "the range provider hides the arrow"
        );
        assert!((f32::from(default_top - fixture.layout().bounds.top()) - 10.65625).abs() < 0.02);
        fixture.state.read_with(&fixture.cx, |state, _| {
            assert_eq!(state.options.need_confirm, Some(false));
        });
        fixture.edit(|consumer| {
            consumer.arrow = Some(PopupArrow {
                visible: true,
                point_at_center: true,
            })
        });
        let arrow = fixture
            .range_arrow()
            .expect("explicit arrow overrides the provider");
        let input = fixture
            .state
            .read_with(&fixture.cx, |state, _| state.geometry.inputs[0]);
        assert!((f32::from(arrow.center().x - input.center().x)).abs() < 0.02);
        fixture.edit(|consumer| consumer.arrow = Some(false.into()));
        assert!(fixture.range_arrow().is_none());
    }
    check::<DateRangeSelection>(PickerRange::new(Some(now().date()), Some(now().date())));
    check::<TimeRangeSelection>(PickerRange::new(Some(now().time()), Some(now().time())));
}

#[test]
fn picker_api_single_arrow_uses_shared_popup_alignment_and_live_provider_updates() {
    fn check<S: PickerSelection>(value: S::Value) {
        let mut fixture = Fixture::<S>::new(value);
        assert!(
            fixture.layout().arrow.is_none(),
            "single arrows are hidden by default"
        );
        fixture.patch(|patch| {
            patch.date_picker.arrow = Some(PopupArrow {
                visible: true,
                point_at_center: true,
            });
            patch.time_picker.arrow = Some(PopupArrow {
                visible: true,
                point_at_center: true,
            });
            patch.range_picker.arrow = Some(false.into());
        });
        let layout = fixture.layout();
        let anchor = fixture
            .state
            .read_with(&fixture.cx, |state, _| state.geometry.trigger.unwrap());
        assert!(
            (f32::from(layout.bounds.left() + layout.arrow.unwrap().x - anchor.center().x)).abs()
                < 0.02
        );
        fixture.edit(|consumer| consumer.arrow = Some(false.into()));
        assert!(
            fixture.layout().arrow.is_none(),
            "explicit false overrides its provider"
        );
    }
    check::<DateSelection>(Some(now().date()));
    check::<TimeSelection>(Some(now().time()));
}

#[test]
fn picker_api_range_arrow_tracks_global_size_without_changing_reference_body_gap() {
    let mut fixture = Fixture::<DateRangeSelection>::new(PickerRange::new(
        Some(now().date()),
        Some(now().date()),
    ));
    let trigger = fixture
        .state
        .read_with(&fixture.cx, |state, _| state.geometry.trigger.unwrap());
    assert!((f32::from(fixture.layout().bounds.top() - trigger.bottom()) - 14.65625).abs() < 0.02);
    fixture.patch(|patch| {
        let mut theme = crate::theme::ThemeConfig::default();
        theme.tokens.size_popup_arrow = Some(24.);
        patch.theme = Some(theme);
    });
    let layout = fixture.layout();
    let (input, padding) = fixture.consumer.read_with(&fixture.cx, |consumer, cx| {
        let tokens = consumer
            .scope
            .snapshot(cx)
            .theme
            .component::<DatePickerTheme>();
        (
            consumer.state.read(cx).geometry.inputs[0],
            tokens.trigger_metrics(Size::Medium).padding_inline as f32,
        )
    });
    assert!((f32::from(layout.bounds.top() - trigger.bottom()) - 20.).abs() < 0.02);
    assert!(
        (f32::from(layout.bounds.left() + layout.arrow.unwrap().x - input.left())
            - (padding * 1.5 + 12.))
            .abs()
            < 0.02
    );
}

#[test]
fn picker_api_range_arrow_preserves_native_content_scrolling_in_small_viewports() {
    let mut fixture = Fixture::<DateRangeSelection>::new(PickerRange::new(
        Some(now().date()),
        Some(now().date()),
    ));
    assert!(
        fixture.range_arrow().is_some(),
        "normal viewports paint the outside arrow"
    );
    fixture.cx.simulate_resize(size(px(900.), px(220.)));
    fixture.edit(|consumer| consumer.tall_footer = true);
    let layout = fixture.layout();
    assert!(layout.bounds.size.height <= px(220.));
    assert!(layout.arrow.is_some());
    let before = fixture
        .state
        .read_with(&fixture.cx, |state, _| state.geometry.headers[0].top());
    fixture.cx.simulate_event(gpui::ScrollEvent {
        position: layout.bounds.center(),
        delta: gpui::ScrollDelta::Pixels(point(px(0.), px(-120.))),
        ..Default::default()
    });
    fixture.draw();
    fixture.state.read_with(&fixture.cx, |state, _| {
        assert!(
            state.geometry.headers[0].top() < before,
            "the popup's native content viewport scrolls"
        );
        assert!(state.is_open());
        assert_eq!(
            state.value(),
            &PickerRange::new(Some(now().date()), Some(now().date()))
        );
    });
    assert!(fixture.layout().bounds.size.height <= px(220.));
}

#[test]
fn picker_api_format_render_updates_and_removal_refresh_retained_editors_silently() {
    let mut fixture = Fixture::<DateSelection>::new(Some(now().date()));
    fixture.edit(|consumer| consumer.format_label = Some("First label".into()));
    assert_eq!(fixture.text(), vec![SharedString::from("First label")]);
    fixture.edit(|consumer| consumer.format_label = Some("Updated label".into()));
    assert_eq!(fixture.text(), vec![SharedString::from("Updated label")]);
    fixture.edit(|consumer| consumer.format_label = None);
    assert_eq!(fixture.text(), vec![SharedString::from("2024-02-20")]);
    fixture.consumer.read_with(&fixture.cx, |consumer, cx| {
        assert_eq!(consumer.changes.get(), 0);
        assert_eq!(consumer.state.read(cx).value(), &Some(now().date()));
        assert!(!consumer.state.read(cx).options.input_read_only);
    });
}

#[test]
fn picker_api_navigation_icon_factories_keep_native_navigation_and_dimensions() {
    let mut fixture = Fixture::<DateSelection>::new(Some(now().date()));
    let header = fixture
        .state
        .read_with(&fixture.cx, |state, _| state.geometry.headers[0]);
    fixture.edit(|consumer| consumer.custom_icons = true);
    for (selector, expected_year, expected_month) in [
        ("api.prev", 2024, 1),
        ("api.next", 2024, 2),
        ("api.super-prev", 2023, 2),
        ("api.super-next", 2024, 2),
    ] {
        let icon = fixture.cx.debug_bounds(selector).unwrap();
        assert_eq!(icon.size, size(px(12.), px(12.)));
        fixture.cx.simulate_click(icon.center(), Modifiers::none());
        fixture.draw();
        fixture.state.read_with(&fixture.cx, |state, _| {
            assert_eq!(state.view_date.year(), expected_year);
            assert_eq!(state.view_date.month(), expected_month);
            assert_eq!(state.geometry.headers[0], header);
        });
    }
    fixture.consumer.read_with(&fixture.cx, |consumer, _| {
        assert!(consumer.factory_calls.get() > 4);
        assert_eq!(consumer.changes.get(), 0);
    });
    fixture.edit(|consumer| consumer.custom_icons = false);
    assert!(fixture.cx.debug_bounds("api.prev").is_none());
    fixture.state.read_with(&fixture.cx, |state, _| {
        assert_eq!(state.geometry.headers[0], header);
        assert_eq!(state.value(), &Some(now().date()));
    });
}

#[test]
fn picker_api_rtl_range_separator_mirrors_the_native_sprite_with_endpoint_order() {
    fn check<S: PickerSelection>(value: S::Value) {
        let mut fixture = Fixture::<S>::new(value);
        let mirrored_sprites = |fixture: &mut Fixture<S>| {
            fixture.cx.update(|window, _| {
                window
                    .rendered_scene()
                    .monochrome_sprites
                    .iter()
                    .filter(|sprite| sprite.transformation.rotation_scale[0][0] < 0.)
                    .count()
            })
        };
        fixture.state.read_with(&fixture.cx, |state, _| {
            assert!(state.geometry.inputs[0].left() < state.geometry.inputs[1].left());
        });
        assert_eq!(mirrored_sprites(&mut fixture), 0);
        fixture.patch(|patch| patch.direction = Some(Direction::Rtl));
        fixture.state.read_with(&fixture.cx, |state, _| {
            assert!(state.geometry.inputs[0].left() > state.geometry.inputs[1].left());
        });
        assert_eq!(
            mirrored_sprites(&mut fixture),
            1,
            "the separator points from start to end"
        );
        fixture.patch(|patch| patch.direction = Some(Direction::Ltr));
        assert_eq!(mirrored_sprites(&mut fixture), 0);
    }
    check::<DateRangeSelection>(PickerRange::new(Some(now().date()), Some(now().date())));
    check::<TimeRangeSelection>(PickerRange::new(Some(now().time()), Some(now().time())));
}

#[test]
fn picker_api_native_navigation_labels_match_each_panel_navigation_distance() {
    use super::super::control::PickerActionSemanticsProbe;
    let mut fixture = Fixture::<DateSelection>::new(Some(now().date()));
    fixture
        .cx
        .update(|_, cx| cx.set_global(PickerActionSemanticsProbe::default()));
    let locale = fixture.consumer.read_with(&fixture.cx, |consumer, cx| {
        consumer.scope.snapshot(cx).locale.picker.clone()
    });
    let all_navigation_labels = [
        locale.prev_month.clone(),
        locale.next_month.clone(),
        locale.prev_year.clone(),
        locale.next_year.clone(),
        locale.prev_decade.clone(),
        locale.next_decade.clone(),
        locale.prev_century.clone(),
        locale.next_century.clone(),
    ];
    for (mode, mut expected) in [
        (
            PickerPanelMode::Date,
            vec![
                locale.prev_month.clone(),
                locale.next_month.clone(),
                locale.prev_year.clone(),
                locale.next_year.clone(),
            ],
        ),
        (
            PickerPanelMode::Month,
            vec![locale.prev_year.clone(), locale.next_year.clone()],
        ),
        (
            PickerPanelMode::Quarter,
            vec![locale.prev_year.clone(), locale.next_year.clone()],
        ),
        (
            PickerPanelMode::Year,
            vec![locale.prev_decade.clone(), locale.next_decade.clone()],
        ),
        (
            PickerPanelMode::Decade,
            vec![locale.prev_century.clone(), locale.next_century.clone()],
        ),
    ] {
        fixture
            .cx
            .update(|_, cx| cx.global_mut::<PickerActionSemanticsProbe>().nodes.clear());
        fixture.edit(|consumer| consumer.panel_mode = Some(mode));
        let mut actual: Vec<SharedString> = fixture.cx.update(|_, cx| {
            cx.global::<PickerActionSemanticsProbe>()
                .nodes
                .iter()
                .filter(|node| node.role() == Role::Button)
                .filter_map(|node| {
                    let label = node.label()?;
                    if !all_navigation_labels
                        .iter()
                        .any(|known| known.as_ref() == label)
                    {
                        return None;
                    }
                    assert!(node.supports_action(accesskit::Action::Click));
                    assert!(node.supports_action(accesskit::Action::Focus));
                    Some(label.to_owned().into())
                })
                .collect()
        });
        actual.sort();
        actual.dedup();
        expected.sort();
        assert_eq!(actual, expected, "navigation actions in {mode:?}");
    }
}

#[test]
fn picker_api_multiple_tag_labels_stay_single_line_at_fractional_scales() {
    let values: Arc<[NaiveDate]> = Arc::from([
        NaiveDate::from_ymd_opt(2024, 2, 10).unwrap(),
        NaiveDate::from_ymd_opt(2025, 3, 20).unwrap(),
        NaiveDate::from_ymd_opt(2026, 10, 30).unwrap(),
    ]);
    let mut fixture = Fixture::<MultipleDateSelection>::new(values.clone());
    let line_height = fixture.consumer.read_with(&fixture.cx, |consumer, cx| {
        let config = consumer.scope.snapshot(cx);
        let metrics = config
            .theme
            .component::<DatePickerTheme>()
            .trigger_metrics(Size::Medium);
        px((metrics.tag_height - 2. * config.theme.tokens.line_width) as f32)
    });
    for mode in [PickerMode::Date, PickerMode::Year] {
        fixture.edit(|consumer| consumer.mode = mode);
        for direction in [Direction::Ltr, Direction::Rtl] {
            fixture.patch(|patch| patch.direction = Some(direction));
            for scale in [1., 1.25, 1.5, 2.] {
                fixture.cx.simulate_scale_factor_change(scale);
                fixture.draw();
                let expected_height = fixture
                    .cx
                    .update(|window, _| window.pixel_snap(line_height));
                fixture.state.read_with(&fixture.cx, |state, _| {
                    assert_eq!(state.value(), &values);
                    assert_eq!(state.geometry.tags.len(), values.len());
                    for tag in &state.geometry.tags {
                        assert!(
                            (tag.content.size.height - expected_height).abs() < px(0.01),
                            "{} wraps in {mode:?}, {direction:?}, {scale}x: {:?}",
                            tag.label,
                            tag.content
                        );
                        assert!(tag.content.top() >= tag.bounds.top() - px(0.01));
                        assert!(tag.content.bottom() <= tag.bounds.bottom() + px(0.01));
                        assert!(tag.content.left() >= tag.bounds.left() - px(0.01));
                        assert!(tag.content.right() <= tag.bounds.right() + px(0.01));
                    }
                });
            }
        }
    }
}

#[test]
fn picker_api_wide_multiple_popup_follows_direction_and_preserves_explicit_placement() {
    let mut fixture = Fixture::<MultipleDateSelection>::new(Arc::from([now().date()]));
    fixture.cx.simulate_resize(size(px(1032.), px(720.)));
    fixture.draw();
    let check_alignment = |fixture: &mut Fixture<MultipleDateSelection>, placement| {
        let layout = fixture.layout();
        let trigger = fixture
            .state
            .read_with(&fixture.cx, |state, _| state.geometry.trigger.unwrap());
        assert!((f32::from(trigger.size.width) - 936.).abs() < 0.02);
        assert_eq!(layout.placement, placement);
        let delta = if placement == PopupPlacement::BottomRight {
            layout.bounds.right() - trigger.right()
        } else {
            layout.bounds.left() - trigger.left()
        };
        assert!(
            delta.abs() < px(0.02),
            "{placement:?}: popup {:?}, trigger {trigger:?}",
            layout.bounds
        );
    };
    check_alignment(&mut fixture, PopupPlacement::BottomLeft);
    fixture.patch(|patch| patch.direction = Some(Direction::Rtl));
    check_alignment(&mut fixture, PopupPlacement::BottomRight);
    fixture.patch(|patch| patch.direction = Some(Direction::Ltr));
    check_alignment(&mut fixture, PopupPlacement::BottomLeft);
    fixture.patch(|patch| patch.direction = Some(Direction::Rtl));
    check_alignment(&mut fixture, PopupPlacement::BottomRight);

    fixture.edit(|consumer| consumer.placement = Some(PopupPlacement::BottomLeft));
    check_alignment(&mut fixture, PopupPlacement::BottomLeft);
    fixture.edit(|consumer| consumer.placement = None);
    check_alignment(&mut fixture, PopupPlacement::BottomRight);

    fixture.patch(|patch| patch.date_picker.placement = Some(PopupPlacement::BottomLeft));
    check_alignment(&mut fixture, PopupPlacement::BottomLeft);
    fixture.patch(|patch| patch.direction = Some(Direction::Ltr));
    check_alignment(&mut fixture, PopupPlacement::BottomLeft);
    fixture.patch(|patch| patch.date_picker.placement = Some(PopupPlacement::BottomRight));
    check_alignment(&mut fixture, PopupPlacement::BottomRight);
    fixture.edit(|consumer| consumer.placement = Some(PopupPlacement::BottomLeft));
    check_alignment(&mut fixture, PopupPlacement::BottomLeft);
    fixture.edit(|consumer| consumer.placement = None);
    check_alignment(&mut fixture, PopupPlacement::BottomRight);

    fixture.patch(|patch| patch.date_picker.placement = None);
    check_alignment(&mut fixture, PopupPlacement::BottomLeft);
    fixture.patch(|patch| patch.direction = Some(Direction::Rtl));
    check_alignment(&mut fixture, PopupPlacement::BottomRight);
}

#[test]
fn picker_api_recorded_geometry_matches_native_painted_boxes_at_fractional_scales() {
    fn check<S: PickerSelection>(value: S::Value) {
        let mut fixture = Fixture::<S>::new(value);
        for scale in [1., 1.25, 1.5, 2.] {
            fixture.cx.simulate_scale_factor_change(scale);
            fixture.draw();
            let geometry = fixture
                .state
                .read_with(&fixture.cx, |state, _| state.geometry.clone());
            let painted: Vec<Bounds<Pixels>> = fixture.cx.update(|window, _| {
                window
                    .rendered_scene()
                    .quads
                    .iter()
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
                    .collect()
            });
            let assert_painted = |name, recorded: Bounds<Pixels>| {
                let difference = |actual: &Bounds<Pixels>| {
                    f32::from(
                        (actual.left() - recorded.left()).abs()
                            + (actual.top() - recorded.top()).abs()
                            + (actual.size.width - recorded.size.width).abs()
                            + (actual.size.height - recorded.size.height).abs(),
                    )
                };
                let actual = painted
                    .iter()
                    .min_by(|a, b| difference(a).total_cmp(&difference(b)))
                    .expect("native painted rectangle");
                assert!(
                    difference(actual) < 0.03,
                    "{name} at {scale}x: recorded {recorded:?}, painted {actual:?}"
                );
            };
            assert_painted("trigger", geometry.trigger.unwrap());
            for header in geometry.headers {
                assert_painted("header", header);
            }
            if let Some(footer) = geometry.footer {
                assert_painted("footer", footer);
            }
            for tag in geometry.tags {
                assert_painted("tag", tag.bounds);
            }
        }
    }
    check::<DateSelection>(Some(now().date()));
    check::<DateTimeSelection>(Some(now()));
    check::<MultipleDateSelection>(Arc::from([now().date()]));
}

#[test]
fn picker_api_prefix_geometry_records_native_slot_and_clears_when_removed() {
    let mut fixture = Fixture::<DateSelection>::new(Some(now().date()));
    fixture.cx.simulate_scale_factor_change(1.5);
    fixture.edit(|consumer| consumer.prefix = Some("Date".into()));
    fixture.state.read_with(&fixture.cx, |state, _| {
        let prefix = state.geometry.prefix.expect("measured native prefix slot");
        let trigger = state.geometry.trigger.unwrap();
        assert!(prefix.size.width > px(0.));
        assert_eq!(prefix.size.width, state.prefix_width);
        assert!(prefix.left() >= trigger.left());
        assert!(prefix.right() <= trigger.right());
    });
    fixture.edit(|consumer| consumer.prefix = None);
    fixture.state.read_with(&fixture.cx, |state, _| {
        assert!(state.geometry.prefix.is_none());
        assert_eq!(state.prefix_width, px(0.));
    });
}
