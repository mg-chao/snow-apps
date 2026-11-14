// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::*;
use crate::{
    Size, ValidationStatus,
    theme::{AlgorithmChain, ComponentTheme, ThemeConfig},
};

#[test]
fn form_tracks_resolve_theme_breakpoints_and_clamp_invalid_dimensions() {
    let theme = ThemeConfig::default().resolve();
    let columns = FormColumn::span(8).xs(24).sm(10).md(8).lg(6);
    for (viewport, span) in [
        (575., 24),
        (576., 10),
        (767., 10),
        (768., 8),
        (991., 8),
        (992., 6),
    ] {
        assert_eq!(columns.resolve(viewport, &theme.tokens).span, Some(span));
    }
    assert_eq!(
        FormColumn::span(250)
            .offset(250)
            .resolve(1000., &theme.tokens)
            .span,
        Some(24)
    );
    assert_eq!(FormColumn::width(f32::NAN).base.width, None);
    assert_eq!(FormColumn::flex(-1.).base.flex, None);
    let cascading = FormColumn::default().xs(24).md(8);
    assert_eq!(cascading.resolve(700., &theme.tokens).span, Some(24));
    assert_eq!(cascading.resolve(768., &theme.tokens).span, Some(8));
    let offset = FormColumn::span(16).offset(0).sm(FormColumnSize {
        span: Some(12),
        ..Default::default()
    });
    assert_eq!(offset.resolve(700., &theme.tokens).offset, Some(0));
}

#[test]
fn form_tokens_preserve_reference_spacing_for_all_sizes_and_densities() {
    for compact in [false, true] {
        let mut theme = ThemeConfig::default();
        if compact {
            theme.algorithms = Some(AlgorithmChain::compact());
        }
        let theme = theme.resolve();
        let tokens = theme.component::<FormTheme>();
        assert_eq!(
            tokens.required_mark_font(),
            &crate::theme::FontStack::new(["sans-serif"]).to_gpui_font()
        );
        let g = &theme.tokens;
        for size in [Size::Small, Size::Medium, Size::Large] {
            let style = FormStyle::from_tokens(&tokens, size);
            assert_eq!(style.item_margin_bottom, g.margin_lg);
            assert_eq!(style.vertical_label_padding, g.padding_xs);
            assert_eq!(style.extra_min_height, g.control_height_sm);
            assert_eq!(style.inline_item_margin_bottom, 0.);
            assert_eq!(style.colon_margin_start, g.margin_xxs / 2.);
            assert_eq!(style.colon_margin_end, g.margin_xs);
        }
    }
}

#[test]
fn form_component_patch_rejects_invalid_dimensions_and_retains_negative_margins() {
    let theme = ThemeConfig::default().resolve();
    let mut tokens = FormTheme::default_tokens(&theme.tokens);
    let original = tokens.clone();
    FormTheme::apply(
        &mut tokens,
        &FormTokenPatch {
            label_height: Some(-2.),
            label_font_size: Some(f64::NAN),
            vertical_label_padding: Some(f64::INFINITY),
            item_margin_bottom: Some(-4.),
            label_colon_margin_inline_start: Some(-2.),
            ..Default::default()
        },
    );
    assert_eq!(tokens.label_height, original.label_height);
    assert_eq!(tokens.label_font_size, original.label_font_size);
    assert_eq!(
        tokens.vertical_label_padding,
        original.vertical_label_padding
    );
    assert_eq!(tokens.item_margin_bottom, -4.);
    assert_eq!(tokens.label_colon_margin_inline_start, -2.);
}

#[test]
fn form_config_merges_explicit_false_and_style_slots() {
    use gpui::{Styled, px, rgb};
    let parent = FormConfig {
        label_wrap: true,
        colon: true,
        label_col: Some(FormColumn::span(8)),
        styles: FormStyles {
            label: gpui::StyleRefinement::default().text_color(rgb(0x123456)),
            ..Default::default()
        },
        ..Default::default()
    };
    let child = FormConfigPatch {
        colon: Some(false),
        label_wrap: Some(false),
        styles: FormStyles {
            label: gpui::StyleRefinement::default().p(px(3.)),
            ..Default::default()
        },
        ..Default::default()
    }
    .resolve(&parent);
    assert!(!child.colon && !child.label_wrap);
    assert_eq!(child.label_col, parent.label_col);
    assert_eq!(
        child.styles.label.text.color,
        parent.styles.label.text.color
    );
    assert_eq!(child.styles.label.padding.left, Some(px(3.).into()));
}

mod native {
    use super::*;
    use crate::{ConfigScope, UiRoot};
    use gpui::{
        AppContext, Background, Bounds, Context, Entity, IntoElement, Pixels, Render,
        TestAppContext, Window, WindowHandle, div, point, prelude::*, px, rgb, size,
    };

    const ROOT: u32 = 0x152637;
    const LABEL: u32 = 0x263748;
    const INPUT: u32 = 0x374859;
    const EXPLAIN: u32 = 0x48596a;
    const EXTRA: u32 = 0x596a7b;
    const LABEL_TEXT: u32 = 0x667788;

    struct Probe {
        state: Entity<FormState>,
        scope: ConfigScope,
        layout: FormLayout,
        help: f32,
        extra: f32,
        hidden: bool,
        no_style: bool,
        size: Size,
        placeholder: bool,
        item_margin: Option<f32>,
        label_inspection: Option<std::rc::Rc<std::cell::Cell<Option<Bounds<Pixels>>>>>,
    }
    impl Render for Probe {
        fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
            let mut item = FormItem::new("item")
                .label("Name")
                .hidden(self.hidden)
                .no_style(self.no_style)
                .content(div().w_full().h(px(20.)).bg(rgb(INPUT)));
            if self.placeholder {
                item = item.label_placeholder();
            }
            if let Some(margin) = self.item_margin {
                item = item.mb(px(margin));
            }
            if let Some(inspection) = self.label_inspection.clone() {
                item = item.inspect_label_layout(move |bounds| inspection.set(Some(bounds)));
            }
            if self.help > 0. {
                item = item.help_content(div().h(px(self.help)).child("Help"));
            }
            if self.extra > 0. {
                item = item.extra_content(div().h(px(self.extra)).child("Extra"));
            }
            div().w(px(640.)).p(px(20.)).child(
                Form::new("form", &self.state)
                    .scope(self.scope.clone())
                    .layout(self.layout)
                    .size(self.size)
                    .when(self.layout != FormLayout::Vertical, |form| {
                        form.label_col(6).wrapper_col(18)
                    })
                    .styles(FormStyles {
                        root: gpui::StyleRefinement::default().bg(rgb(ROOT)),
                        label_column: gpui::StyleRefinement::default().bg(rgb(LABEL)),
                        label: gpui::StyleRefinement::default().bg(rgb(LABEL_TEXT)),
                        explain: gpui::StyleRefinement::default().bg(rgb(EXPLAIN)),
                        extra: gpui::StyleRefinement::default().bg(rgb(EXTRA)),
                        ..Default::default()
                    })
                    .item(item),
            )
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
                let root = ConfigScope::new_window(cx);
                let state = cx.new(|cx| FormState::new(window, cx));
                let content = cx.new(|_| Probe {
                    state,
                    scope: root.clone(),
                    layout: FormLayout::Horizontal,
                    help: 0.,
                    extra: 0.,
                    hidden: false,
                    no_style: false,
                    size: Size::Medium,
                    placeholder: false,
                    item_margin: None,
                    label_inspection: None,
                });
                probe = Some(content.clone());
                scope = Some(root.clone());
                UiRoot::new(root, content, window, cx)
            });
            let mut result = Self {
                cx,
                window,
                probe: probe.unwrap(),
                scope: scope.unwrap(),
            };
            result
                .cx
                .simulate_window_resize(result.window.into(), size(px(900.), px(700.)));
            result.draw();
            result
        }
        fn draw(&mut self) {
            self.cx.run_until_parked();
            self.cx
                .update_window(self.window.into(), |_, window, cx| {
                    window.draw(cx).clear(cx)
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
        fn bounds(&mut self, color: u32) -> Option<Bounds<Pixels>> {
            self.cx
                .update_window(self.window.into(), |_, window, _| {
                    let scale = window.scale_factor();
                    let color: Background = rgb(color).into();
                    window
                        .rendered_scene()
                        .quads
                        .iter()
                        .find(|quad| quad.background == color)
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
                })
                .unwrap()
        }
    }
    fn near(actual: Pixels, expected: f32) {
        assert!(
            (f32::from(actual) - expected).abs() <= 1.,
            "{actual:?} != {expected}"
        );
    }

    #[test]
    fn form_horizontal_tracks_and_help_use_the_reference_margin_budget() {
        let mut f = Fixture::new();
        near(f.bounds(ROOT).unwrap().size.width, 600.);
        near(f.bounds(LABEL).unwrap().size.width, 150.);
        near(f.bounds(INPUT).unwrap().left(), 170.);
        near(f.bounds(ROOT).unwrap().size.height, 32.);
        f.edit(|probe| probe.help = 22.);
        near(f.bounds(ROOT).unwrap().size.height, 56.);
        near(f.bounds(EXPLAIN).unwrap().size.height, 22.);
        f.edit(|probe| probe.help = 44.);
        near(f.bounds(ROOT).unwrap().size.height, 76.);
        f.edit(|probe| {
            probe.help = 22.;
            probe.extra = 24.;
        });
        near(f.bounds(ROOT).unwrap().size.height, 80.);
        assert_eq!(
            f.bounds(EXTRA).unwrap().top(),
            f.bounds(EXPLAIN).unwrap().bottom()
        );
        f.edit(|probe| probe.help = 0.);
        near(f.bounds(ROOT).unwrap().size.height, 56.);
    }

    #[test]
    fn form_vertical_and_responsive_horizontal_tracks_stack_without_overflow() {
        let mut f = Fixture::new();
        f.edit(|probe| probe.layout = FormLayout::Vertical);
        let label = f.bounds(LABEL).unwrap();
        let input = f.bounds(INPUT).unwrap();
        near(label.size.width, 600.);
        near(input.size.width, 600.);
        assert!(input.top() >= label.bottom());
        f.edit(|probe| probe.layout = FormLayout::Horizontal);
        f.cx.simulate_window_resize(f.window.into(), size(px(575.), px(700.)));
        f.draw();
        near(f.bounds(LABEL).unwrap().size.width, 600.);
        assert!(f.bounds(INPUT).unwrap().top() >= f.bounds(LABEL).unwrap().bottom());
        f.cx.simulate_window_resize(f.window.into(), size(px(576.), px(700.)));
        f.draw();
        near(f.bounds(LABEL).unwrap().size.width, 150.);
        near(f.bounds(INPUT).unwrap().left(), 170.);
        // A physical viewport can lie between CSS max-width:575 and the
        // next min-width:576 at fractional display scales. Keep that interval.
        f.cx.simulate_window_resize(f.window.into(), size(px(575.3333), px(700.)));
        f.draw();
        near(f.bounds(LABEL).unwrap().size.width, 150.);
        near(f.bounds(INPUT).unwrap().left(), 170.);
        f.cx.update(|cx| {
            f.scope
                .update_patch(cx, |patch| patch.direction = Some(crate::Direction::Rtl))
                .unwrap();
        });
        for (width, label_width, control_width) in [
            (575., 600., 600.),
            (575.3333, 150., 450.),
            (576., 150., 450.),
        ] {
            f.cx.simulate_window_resize(f.window.into(), size(px(width), px(700.)));
            f.draw();
            near(f.bounds(LABEL).unwrap().size.width, label_width);
            near(f.bounds(INPUT).unwrap().size.width, control_width);
            near(f.bounds(INPUT).unwrap().left(), 20.);
        }
    }

    #[test]
    fn form_rtl_keeps_logical_columns_and_hidden_items_out_of_layout() {
        let mut f = Fixture::new();
        f.cx.update(|cx| {
            f.scope
                .update_patch(cx, |patch| patch.direction = Some(crate::Direction::Rtl))
                .unwrap()
        });
        f.draw();
        near(f.bounds(LABEL).unwrap().left(), 470.);
        near(f.bounds(INPUT).unwrap().left(), 20.);
        f.edit(|probe| probe.hidden = true);
        assert!(f.bounds(LABEL).is_none() && f.bounds(INPUT).is_none());
        f.edit(|probe| {
            probe.hidden = false;
            probe.no_style = true;
        });
        assert!(f.bounds(LABEL).is_none());
        near(f.bounds(ROOT).unwrap().size.height, 20.);
    }
    #[test]
    fn form_effective_item_margin_controls_help_budget_and_null_label_offsets() {
        let mut f = Fixture::new();
        f.edit(|probe| {
            probe.help = 22.;
            probe.item_margin = Some(0.);
        });
        near(f.bounds(ROOT).unwrap().size.height, 54.);
        f.edit(|probe| {
            probe.help = 0.;
            probe.placeholder = true;
        });
        f.cx.simulate_window_resize(f.window.into(), size(px(520.), px(700.)));
        f.draw();
        near(f.bounds(INPUT).unwrap().left(), 170.);
        near(f.bounds(ROOT).unwrap().size.height, 32.);
    }
    #[test]
    fn form_vertical_label_content_uses_logical_start_in_both_directions() {
        let mut f = Fixture::new();
        f.edit(|probe| probe.layout = FormLayout::Vertical);
        near(f.bounds(LABEL_TEXT).unwrap().left(), 20.);
        f.cx.update(|cx| {
            f.scope
                .update_patch(cx, |patch| patch.direction = Some(crate::Direction::Rtl))
                .unwrap()
        });
        f.draw();
        near(f.bounds(LABEL_TEXT).unwrap().right(), 620.);
    }

    #[test]
    fn form_label_inspection_reports_existing_element_bounds_without_changing_layout() {
        let mut f = Fixture::new();
        let label = f.bounds(LABEL_TEXT).unwrap();
        let input = f.bounds(INPUT).unwrap();
        let root = f.bounds(ROOT).unwrap();
        let observation = std::rc::Rc::new(std::cell::Cell::new(None));
        f.edit(|probe| probe.label_inspection = Some(observation.clone()));
        assert_eq!(observation.get(), Some(label));
        assert_eq!(f.bounds(LABEL_TEXT), Some(label));
        assert_eq!(f.bounds(INPUT), Some(input));
        assert_eq!(f.bounds(ROOT), Some(root));
        observation.set(None);
        f.edit(|probe| probe.label_inspection = None);
        assert!(observation.get().is_none());
        assert_eq!(f.bounds(LABEL_TEXT), Some(label));
        assert_eq!(f.bounds(INPUT), Some(input));
        assert_eq!(f.bounds(ROOT), Some(root));
    }

    #[test]
    fn form_field_notifications_rebuild_only_the_changed_retained_item() {
        use std::{cell::Cell, rc::Rc};
        struct Reactive {
            form: Entity<FormState>,
            fields: Vec<FieldHandle>,
            scope: ConfigScope,
            owner_renders: Rc<Cell<usize>>,
            item_renders: Vec<Rc<Cell<usize>>>,
        }
        impl Render for Reactive {
            fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
                self.owner_renders.set(self.owner_renders.get() + 1);
                let mut form = Form::new("reactive", &self.form)
                    .scope(self.scope.clone())
                    .w(px(500.));
                for (index, field) in self.fields.iter().enumerate() {
                    let renders = self.item_renders[index].clone();
                    form = form.item(
                        FormItem::new(index)
                            .label(format!("Field {index}"))
                            .control_with(field.clone(), move |context| {
                                renders.set(renders.get() + 1);
                                div()
                                    .h(px(32.))
                                    .w(px(200.))
                                    .child(format!("{:?}", context.status))
                                    .into_any_element()
                            }),
                    );
                }
                form
            }
        }
        let owner = Rc::new(Cell::new(0));
        let items: Vec<_> = (0..3).map(|_| Rc::new(Cell::new(0))).collect();
        let mut cx = TestAppContext::single();
        let mut field = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            let form = cx.new(|cx| FormState::new(window, cx));
            let fields = form.update(cx, |form, cx| {
                (0..3)
                    .map(|index| {
                        form.register_raw(format!("field-{index}"), FieldOptions::default(), cx)
                            .unwrap()
                    })
                    .collect::<Vec<_>>()
            });
            field = Some(fields[1].clone());
            let content = cx.new(|_| Reactive {
                form,
                fields,
                scope: scope.clone(),
                owner_renders: owner.clone(),
                item_renders: items.clone(),
            });
            UiRoot::new(scope, content, window, cx)
        });
        let draw = |cx: &mut TestAppContext| {
            cx.run_until_parked();
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        };
        draw(&mut cx);
        for _ in 0..3 {
            let frames = cx
                .update_window(window.into(), |_, window, cx| {
                    window.simulate_next_frame(cx)
                })
                .unwrap();
            if frames == 0 {
                break;
            }
            draw(&mut cx);
        }
        let before: Vec<_> = items.iter().map(|value| value.get()).collect();
        assert!(
            !cx.update_window(window.into(), |_, window, _| window.is_a11y_active())
                .unwrap()
        );
        cx.update_window(window.into(), |_, window, cx| {
            field
                .as_ref()
                .unwrap()
                .set_value(FormValue::from("updated"), window, cx)
                .unwrap()
        })
        .unwrap();
        draw(&mut cx);
        assert_eq!(
            items[0].get(),
            before[0],
            "first unrelated field reconstructed"
        );
        assert!(items[1].get() > before[1], "changed field did not rerender");
        assert_eq!(
            items[2].get(),
            before[2],
            "last unrelated field reconstructed"
        );
        assert_eq!(
            cx.update_window(window.into(), |_, window, cx| window
                .simulate_next_frame(cx))
                .unwrap(),
            0,
            "settled form requested an idle frame"
        );
    }

    #[test]
    fn form_compound_no_style_metadata_stops_at_the_nearest_styled_parent() {
        use std::{cell::RefCell, rc::Rc};
        type Statuses = Rc<RefCell<Vec<(ValidationStatus, Option<gpui::SharedString>)>>>;
        struct Compound {
            form: Entity<FormState>,
            scope: ConfigScope,
            fields: Vec<FieldHandle>,
            parent: Rc<RefCell<ValidationStatus>>,
            nested: Rc<RefCell<ValidationStatus>>,
            controls: Statuses,
        }
        impl Render for Compound {
            fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
                let parent = self.parent.clone();
                let nested = self.nested.clone();
                let controls = self.controls.clone();
                let nested_controls = self.controls.clone();
                Form::new("compound-form", &self.form)
                    .scope(self.scope.clone())
                    .w(px(600.))
                    .label_col(6)
                    .wrapper_col(18)
                    .item(
                        FormItem::new("contact")
                            .label("Contact")
                            .styles_fn(move |context, _, _| {
                                *parent.borrow_mut() = context.status;
                                FormStyles::default()
                            })
                            .item(FormItem::new("given").no_style(true).control_with(
                                self.fields[0].clone(),
                                move |context| {
                                    controls
                                        .borrow_mut()
                                        .push((context.status, context.description));
                                    div().w(px(180.)).h(px(32.)).into_any_element()
                                },
                            ))
                            .item(
                                FormItem::new("nested")
                                    .label("Nested")
                                    .layout(FormLayout::Vertical)
                                    .w(px(200.))
                                    .styles_fn(move |context, _, _| {
                                        *nested.borrow_mut() = context.status;
                                        FormStyles::default()
                                    })
                                    .item(FormItem::new("inner").no_style(true).control_with(
                                        self.fields[1].clone(),
                                        move |context| {
                                            nested_controls
                                                .borrow_mut()
                                                .push((context.status, context.description));
                                            div().w(px(180.)).h(px(32.)).into_any_element()
                                        },
                                    )),
                            ),
                    )
            }
        }
        let mut cx = TestAppContext::single();
        let mut form = None;
        let parent = Rc::new(RefCell::new(ValidationStatus::None));
        let nested = Rc::new(RefCell::new(ValidationStatus::None));
        let controls: Statuses = Rc::default();
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let state = cx.new(|cx| FormState::new(window, cx));
            let fields = state.update(cx, |state, cx| {
                ["given", "inner"]
                    .into_iter()
                    .map(|name| {
                        state
                            .register_raw(name, FieldOptions::default(), cx)
                            .unwrap()
                    })
                    .collect()
            });
            form = Some(state.clone());
            let content = cx.new(|_| Compound {
                form: state,
                scope: scope.clone(),
                fields,
                parent: parent.clone(),
                nested: nested.clone(),
                controls: controls.clone(),
            });
            UiRoot::new(scope, content, window, cx)
        });
        let draw = |cx: &mut TestAppContext| {
            cx.run_until_parked();
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        };
        draw(&mut cx);
        cx.update_window(window.into(), |_, window, cx| {
            form.as_ref().unwrap().update(cx, |form, cx| {
                form.set_fields(
                    [FieldPatch {
                        name: "inner".into(),
                        errors: Some(vec!["Inner problem".into()]),
                        ..Default::default()
                    }],
                    window,
                    cx,
                )
                .unwrap()
            })
        })
        .unwrap();
        draw(&mut cx);
        assert_eq!(*parent.borrow(), ValidationStatus::None);
        assert_eq!(*nested.borrow(), ValidationStatus::Error);
        controls.borrow_mut().clear();
        cx.update_window(window.into(), |_, window, cx| {
            form.as_ref().unwrap().update(cx, |form, cx| {
                form.set_fields(
                    [FieldPatch {
                        name: "given".into(),
                        errors: Some(vec!["Given problem".into()]),
                        ..Default::default()
                    }],
                    window,
                    cx,
                )
                .unwrap()
            })
        })
        .unwrap();
        draw(&mut cx);
        assert_eq!(*parent.borrow(), ValidationStatus::Error);
        assert_eq!(*nested.borrow(), ValidationStatus::Error);
        assert!(controls.borrow().iter().any(|(status, description)| {
            *status == ValidationStatus::Error
                && description
                    .as_ref()
                    .is_some_and(|description| description.contains("Given problem"))
        }));
    }
    #[test]
    fn form_cached_compound_children_preserve_declared_width_and_first_layout_positions() {
        const FIRST: u32 = 0x7b3201;
        const SECOND: u32 = 0x7b3202;
        struct Widths {
            form: Entity<FormState>,
            scope: ConfigScope,
            fields: Vec<FieldHandle>,
        }
        impl Render for Widths {
            fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
                let mut group = FormItem::new("pair").label("Pair");
                for (index, color) in [FIRST, SECOND].into_iter().enumerate() {
                    group = group.item(
                        FormItem::new(index)
                            .label("Name")
                            .layout(FormLayout::Vertical)
                            .w(px(200.))
                            .mb(px(0.))
                            .control_with(self.fields[index].clone(), move |_| {
                                div().w_full().h(px(32.)).bg(rgb(color)).into_any_element()
                            }),
                    );
                }
                div().p(px(20.)).child(
                    Form::new("widths", &self.form)
                        .scope(self.scope.clone())
                        .w(px(600.))
                        .label_col(6)
                        .wrapper_col(18)
                        .item(group),
                )
            }
        }
        let mut cx = TestAppContext::single();
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            let form = cx.new(|cx| FormState::new(window, cx));
            let fields = form.update(cx, |form, cx| {
                ["a", "b"]
                    .into_iter()
                    .map(|name| {
                        form.register_raw(name, FieldOptions::default(), cx)
                            .unwrap()
                    })
                    .collect()
            });
            let view = cx.new(|_| Widths {
                form,
                scope: scope.clone(),
                fields,
            });
            UiRoot::new(scope, view, window, cx)
        });
        let draw = |cx: &mut TestAppContext| {
            cx.run_until_parked();
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        };
        let bounds = |cx: &mut TestAppContext, color: u32| {
            cx.update_window(window.into(), |_, window, _| {
                let background: Background = rgb(color).into();
                let scale = window.scale_factor();
                let quad = window
                    .rendered_scene()
                    .quads
                    .iter()
                    .find(|quad| quad.background == background)
                    .unwrap();
                (
                    quad.bounds.origin.x.0 / scale,
                    quad.bounds.size.width.0 / scale,
                )
            })
            .unwrap()
        };
        draw(&mut cx);
        let first = bounds(&mut cx, FIRST);
        let second = bounds(&mut cx, SECOND);
        assert_eq!(first, (170., 200.));
        assert_eq!(second, (378., 200.));
        for _ in 0..3 {
            let pending = cx
                .update_window(window.into(), |_, window, cx| {
                    window.simulate_next_frame(cx)
                })
                .unwrap();
            if pending == 0 {
                break;
            }
            draw(&mut cx);
        }
        assert_eq!(bounds(&mut cx, FIRST), first);
        assert_eq!(bounds(&mut cx, SECOND), second);
        assert_eq!(
            cx.update_window(window.into(), |_, window, cx| window
                .simulate_next_frame(cx))
                .unwrap(),
            0
        );
    }

    #[test]
    fn form_inline_literal_labels_round_the_combined_native_intrinsic_budget_once() {
        const COLOR: u32 = 0x731e46;
        struct Inline {
            form: Entity<FormState>,
            scope: ConfigScope,
        }
        impl Render for Inline {
            fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
                Form::new("inline-labels", &self.form)
                    .scope(self.scope.clone())
                    .layout(FormLayout::Inline)
                    .styles(FormStyles {
                        label: gpui::StyleRefinement::default().bg(rgb(COLOR)),
                        ..Default::default()
                    })
                    .item(
                        FormItem::new("username")
                            .label("Username")
                            .content(div().w(px(160.)).h(px(32.))),
                    )
                    .item(
                        FormItem::new("password")
                            .label("Password")
                            .content(div().w(px(160.)).h(px(32.))),
                    )
            }
        }
        let mut cx = TestAppContext::single();
        let mut scope = None;
        let window = cx.add_window(|window, cx| {
            let root = ConfigScope::new_window(cx);
            root.update_patch(cx, |patch| {
                patch.theme = Some(ThemeConfig {
                    algorithms: Some(AlgorithmChain::compact()),
                    ..Default::default()
                });
            })
            .unwrap();
            let form = cx.new(|cx| FormState::new(window, cx));
            let view = cx.new(|_| Inline {
                form,
                scope: root.clone(),
            });
            scope = Some(root.clone());
            UiRoot::new(root, view, window, cx)
        });
        cx.run_until_parked();
        cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
            .unwrap();
        cx.update_window(window.into(), |_, window, cx| {
            let config = scope.as_ref().unwrap().snapshot(cx);
            let tokens = config.theme.component::<FormTheme>();
            let metrics = FormStyle::from_tokens(&tokens, Size::Medium);
            let advance = |text: &str| {
                let run = gpui::TextRun {
                    len: text.len(),
                    font: tokens.font().clone(),
                    color: gpui::Hsla::default(),
                    background_color: None,
                    underline: None,
                    strikethrough: None,
                };
                window
                    .text_system()
                    .shape_line(
                        text.to_owned().into(),
                        px(metrics.label_font_size as f32),
                        &[run],
                        None,
                    )
                    .width()
            };
            let background: Background = rgb(COLOR).into();
            let actual = window
                .rendered_scene()
                .quads
                .iter()
                .filter(|quad| quad.background == background)
                .map(|quad| quad.bounds.size.width.0 / window.scale_factor())
                .collect::<Vec<_>>();
            assert_eq!(actual.len(), 2);
            for (actual, label) in actual.into_iter().zip(["Username", "Password"]) {
                let raw = advance(label)
                    + advance(":")
                    + px((metrics.colon_margin_start + metrics.colon_margin_end) as f32);
                near(px(actual), f32::from(window.pixel_snap(raw)));
            }
        })
        .unwrap();
    }

    #[test]
    fn form_literal_required_labels_budget_once_and_preserve_semantic_box_and_font_overrides() {
        use gpui::{Font, FontWeight, TextRun};
        use std::{cell::RefCell, rc::Rc};

        struct Labels {
            form: Entity<FormState>,
            scope: ConfigScope,
            bounds: Rc<RefCell<Vec<Option<Bounds<Pixels>>>>>,
        }
        impl Render for Labels {
            fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
                let mut form = Form::new("literal-label-budgets", &self.form)
                    .scope(self.scope.clone())
                    .w(px(536.))
                    .label_col(8)
                    .wrapper_col(16);
                for index in 0..8 {
                    let bounds = self.bounds.clone();
                    let mut item = FormItem::new(index)
                        .label(if index == 7 {
                            " \tUser\n name  "
                        } else {
                            "Username"
                        })
                        .required(true)
                        .inspect_label_layout(move |value| bounds.borrow_mut()[index] = Some(value))
                        .content(div().w_full().h(px(32.)));
                    item = match index {
                        1 => item.styles(FormStyles {
                            label: gpui::StyleRefinement::default().w(px(123.)),
                            ..Default::default()
                        }),
                        2 => item.styles(FormStyles {
                            label: gpui::StyleRefinement::default().pl(px(7.)).pr(px(5.)),
                            ..Default::default()
                        }),
                        3 => item.styles(FormStyles {
                            required_mark: gpui::StyleRefinement::default().w(px(20.)),
                            ..Default::default()
                        }),
                        4 => item.required_mark_content(|_, _, _| {
                            div().w(px(13.)).h(px(10.)).into_any_element()
                        }),
                        5 => item.styles(FormStyles {
                            required_mark: gpui::StyleRefinement::default()
                                .text_size(px(18.))
                                .font_weight(FontWeight::BOLD),
                            ..Default::default()
                        }),
                        6 => item.styles(FormStyles {
                            label: gpui::StyleRefinement::default()
                                .text_size(px(17.))
                                .font_weight(FontWeight::BOLD),
                            ..Default::default()
                        }),
                        _ => item,
                    };
                    form = form.item(item);
                }
                div().p(px(20.)).child(form)
            }
        }

        for (direction, scale) in [(crate::Direction::Ltr, 1.), (crate::Direction::Rtl, 1.5)] {
            for label_size in [14., 16.] {
                let mut cx = TestAppContext::single();
                let observations = Rc::new(RefCell::new(vec![None; 8]));
                let mut scope = None;
                let window = cx.add_window(|window, cx| {
                    window.set_scale_factor(scale);
                    let root = ConfigScope::new_window(cx);
                    root.update_patch(cx, |patch| {
                        patch.direction = Some(direction);
                        let theme = patch.theme.get_or_insert_default();
                        theme.components.set::<FormTheme>(FormTokenPatch {
                            label_font_size: Some(label_size),
                            label_colon_margin_inline_start: (label_size == 16.).then_some(8.),
                            label_colon_margin_inline_end: (label_size == 16.).then_some(8.),
                            ..Default::default()
                        });
                    })
                    .unwrap();
                    let form = cx.new(|cx| FormState::new(window, cx));
                    let content = cx.new(|_| Labels {
                        form,
                        scope: root.clone(),
                        bounds: observations.clone(),
                    });
                    scope = Some(root.clone());
                    UiRoot::new(root, content, window, cx)
                });
                cx.run_until_parked();
                cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                    .unwrap();
                cx.update_window(window.into(), |_, window, cx| {
                    let config = scope.as_ref().unwrap().snapshot(cx);
                    let tokens = config.theme.component::<FormTheme>();
                    let metrics = FormStyle::from_tokens(&tokens, Size::Medium);
                    let shape = |text: &str, font: Font, font_size: f64| {
                        let run = TextRun {
                            len: text.len(),
                            font,
                            color: gpui::Hsla::default(),
                            background_color: None,
                            underline: None,
                            strikethrough: None,
                        };
                        window
                            .text_system()
                            .shape_line(text.to_owned().into(), px(font_size as f32), &[run], None)
                            .width()
                    };
                    let label = shape("Username", tokens.font().clone(), label_size);
                    let colon = shape(":", tokens.font().clone(), label_size);
                    let mut mark_font = tokens.font().clone();
                    mark_font.family = tokens.required_mark_font().family.clone();
                    let mark = shape("*", mark_font.clone(), metrics.font_size);
                    let margins = px((metrics.mark_margin
                        + metrics.colon_margin_start
                        + metrics.colon_margin_end) as f32);
                    let snapped_margins = window.pixel_snap(px(metrics.mark_margin as f32))
                        + window.pixel_snap(px(metrics.colon_margin_start as f32))
                        + window.pixel_snap(px(metrics.colon_margin_end as f32));
                    let observations = observations.borrow();
                    let actual = |index: usize| {
                        observations[index]
                            .expect("actual label prepaint")
                            .size
                            .width
                    };
                    near(
                        actual(0),
                        f32::from(window.pixel_snap(label + colon + mark + margins)),
                    );
                    near(actual(1), f32::from(window.pixel_snap(px(123.))));
                    near(
                        actual(2),
                        f32::from(
                            window.pixel_snap(label)
                                + window.pixel_snap(colon)
                                + window.pixel_snap(mark)
                                + snapped_margins
                                + window.pixel_snap(px(7.))
                                + window.pixel_snap(px(5.)),
                        ),
                    );
                    near(
                        actual(3),
                        f32::from(
                            window.pixel_snap(label)
                                + window.pixel_snap(colon)
                                + window.pixel_snap(px(20.))
                                + snapped_margins,
                        ),
                    );
                    near(
                        actual(4),
                        f32::from(
                            window.pixel_snap(label)
                                + window.pixel_snap(colon)
                                + window.pixel_snap(px(13.))
                                + window.pixel_snap(px(metrics.colon_margin_start as f32))
                                + window.pixel_snap(px(metrics.colon_margin_end as f32)),
                        ),
                    );
                    mark_font.weight = FontWeight::BOLD;
                    near(
                        actual(5),
                        f32::from(window.pixel_snap(
                            label + colon + shape("*", mark_font.clone(), 18.) + margins,
                        )),
                    );
                    let mut bold = tokens.font().clone();
                    bold.weight = FontWeight::BOLD;
                    near(
                        actual(6),
                        f32::from(window.pixel_snap(
                            shape("Username", bold.clone(), 17.)
                                + shape(":", bold, 17.)
                                + shape("*", mark_font, metrics.font_size)
                                + margins,
                        )),
                    );
                    near(
                        actual(7),
                        f32::from(window.pixel_snap(
                            shape("User name", tokens.font().clone(), label_size)
                                + colon
                                + mark
                                + margins,
                        )),
                    );
                    let bounds = observations[0].unwrap();
                    if direction == crate::Direction::Rtl {
                        near(
                            bounds.left(),
                            f32::from(window.pixel_snap(px(20. + 536. * 16. / 24.))),
                        );
                    } else {
                        near(
                            bounds.right(),
                            f32::from(window.pixel_snap(px(20. + 536. * 8. / 24.))),
                        );
                    }
                })
                .unwrap();
            }
        }
    }

    #[test]
    fn form_native_editor_keeps_actual_validity_when_visual_status_is_overridden() {
        use crate::input::{Input, InputState};
        use gpui::{Element, RenderOnce, Role, accesskit};
        use std::{cell::RefCell, rc::Rc};

        #[derive(IntoElement)]
        struct Editor {
            state: Entity<InputState>,
            context: FormItemContext,
            node: Rc<RefCell<accesskit::Node>>,
        }
        impl RenderOnce for Editor {
            fn render(self, window: &mut Window, cx: &mut gpui::App) -> impl IntoElement {
                let target = self.node.clone();
                let mut element = Input::new("actual-editor", &self.state)
                    .disabled(false)
                    .status(ValidationStatus::Success)
                    .inspect_accessibility(move |node| *target.borrow_mut() = node.clone())
                    .in_form(&self.context, window, cx)
                    .render(window, cx)
                    .into_any_element();
                // Input erases its shared native frame for composite controls.
                // Inspect that actual frame through its transparent focus ring;
                // offscreen platforms do not activate an accessibility adapter.
                type Frame = crate::behavior::SemanticElement<gpui::Stateful<gpui::Div>>;
                let mut semantics = accesskit::Node::new(Role::TextInput);
                if let Some(frame) =
                    element.downcast_mut::<crate::behavior::FocusRingElement<Frame>>()
                {
                    frame.write_a11y_info(&mut semantics);
                } else {
                    element
                        .downcast_mut::<Frame>()
                        .expect("native Input frame")
                        .write_a11y_info(&mut semantics);
                }
                let mut controls = div().child(element);
                for loading in [false, true] {
                    let mut semantics = crate::behavior::Accessibility::new(Role::Switch);
                    semantics.in_form(&self.context);
                    let switch = semantics.apply(
                        crate::switch::Switch::new(if loading { "loading" } else { "validating" })
                            .loading(loading)
                            .render(window, cx)
                            .into_element(),
                    );
                    let mut node = accesskit::Node::new(Role::Switch);
                    switch.write_a11y_info(&mut node);
                    assert_eq!(node.is_busy(), self.context.busy || loading);
                    assert_eq!(node.supports_action(accesskit::Action::Click), !loading);
                    controls = controls.child(switch);
                }
                controls
            }
        }
        struct Consumer {
            form: Entity<FormState>,
            field: FieldHandle,
            input: Entity<InputState>,
            scope: ConfigScope,
            node: Rc<RefCell<accesskit::Node>>,
            form_node: Rc<RefCell<accesskit::Node>>,
            status: Rc<RefCell<ValidationStatus>>,
            status_override: Option<ValidationStatus>,
            feedback: bool,
        }
        impl Render for Consumer {
            fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
                let input = self.input.clone();
                let node = self.node.clone();
                let status = self.status.clone();
                let item = FormItem::new("name")
                    .label("Name")
                    .has_feedback(self.feedback)
                    .when_some(self.status_override, |item, status| {
                        item.validate_status(status)
                    })
                    .control_with(self.field.clone(), move |context| {
                        *status.borrow_mut() = context.status;
                        Editor {
                            state: input.clone(),
                            context,
                            node: node.clone(),
                        }
                        .into_any_element()
                    });
                let form = Form::new("native-validity", &self.form)
                    .scope(self.scope.clone())
                    .disabled(true)
                    .w(px(400.))
                    .item(item)
                    .render(window, cx)
                    .into_element();
                let mut node = accesskit::Node::new(Role::Form);
                form.write_a11y_info(&mut node);
                *self.form_node.borrow_mut() = node;
                form
            }
        }
        let mut cx = TestAppContext::single();
        let node = Rc::new(RefCell::new(accesskit::Node::new(Role::TextInput)));
        let form_node = Rc::new(RefCell::new(accesskit::Node::new(Role::Form)));
        let status = Rc::new(RefCell::new(ValidationStatus::None));
        let mut retained = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            let form = cx.new(|cx| FormState::new(window, cx));
            let field = form.update(cx, |form, cx| {
                form.register_raw(
                    "name",
                    FieldOptions::default()
                        .initial_value("accepted")
                        .rule(FormRule::required()),
                    cx,
                )
                .unwrap()
            });
            let input = cx.new(|cx| InputState::new(window, cx));
            let consumer = cx.new(|_| Consumer {
                form,
                field,
                input,
                scope: scope.clone(),
                node: node.clone(),
                form_node: form_node.clone(),
                status: status.clone(),
                status_override: Some(ValidationStatus::Success),
                feedback: false,
            });
            retained = Some(consumer.clone());
            UiRoot::new(scope, consumer, window, cx)
        });
        let consumer = retained.unwrap();
        let form = consumer.read_with(&cx, |consumer, _| consumer.form.clone());
        let draw = |cx: &mut TestAppContext| {
            cx.run_until_parked();
            cx.update_window(window.into(), |_, window, cx| window.draw(cx).clear(cx))
                .unwrap();
            cx.run_until_parked();
        };
        draw(&mut cx);
        cx.update_window(window.into(), |_, window, cx| {
            form.update(cx, |form, cx| {
                form.set_fields(
                    [FieldPatch {
                        name: "name".into(),
                        errors: Some(vec!["Name is unavailable".into()]),
                        validating: Some(true),
                        ..Default::default()
                    }],
                    window,
                    cx,
                )
                .unwrap()
            })
        })
        .unwrap();
        draw(&mut cx);
        assert_eq!(*status.borrow(), ValidationStatus::Success);
        assert_eq!(node.borrow().invalid(), Some(accesskit::Invalid::True));
        assert!(node.borrow().is_busy() && node.borrow().is_required());
        assert_eq!(node.borrow().label(), Some("Name"));
        assert_eq!(node.borrow().description(), Some("Name is unavailable"));
        assert!(!node.borrow().is_disabled());
        assert!(!form_node.borrow().is_disabled());

        consumer.update(&mut cx, |consumer, cx| {
            consumer.status_override = None;
            cx.notify();
        });
        cx.update_window(window.into(), |_, window, cx| {
            form.update(cx, |form, cx| {
                form.set_fields(
                    [FieldPatch {
                        name: "name".into(),
                        touched: Some(true),
                        errors: Some(Vec::new()),
                        validating: Some(false),
                        ..Default::default()
                    }],
                    window,
                    cx,
                )
                .unwrap()
            })
        })
        .unwrap();
        draw(&mut cx);
        assert_eq!(*status.borrow(), ValidationStatus::Success);
        assert_eq!(node.borrow().invalid(), None);
        assert!(!node.borrow().is_busy());

        cx.update_window(window.into(), |_, window, cx| {
            form.update(cx, |form, cx| {
                form.set_fields(
                    [FieldPatch {
                        name: "name".into(),
                        touched: Some(false),
                        ..Default::default()
                    }],
                    window,
                    cx,
                )
                .unwrap();
                form.validate_fields(ValidateOptions::default(), window, cx)
            })
        })
        .unwrap();
        draw(&mut cx);
        assert!(consumer.read_with(&cx, |consumer, cx| consumer.field.meta(cx).validated));
        assert_eq!(*status.borrow(), ValidationStatus::None);
        consumer.update(&mut cx, |consumer, cx| {
            consumer.feedback = true;
            cx.notify();
        });
        draw(&mut cx);
        assert_eq!(*status.borrow(), ValidationStatus::Success);
    }
}
