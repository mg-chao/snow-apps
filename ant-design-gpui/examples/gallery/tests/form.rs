// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use super::super::form;
use super::*;
use ant_design_gpui::form::{FormValue, ValidateOptions};

#[test]
fn form_gallery_preserves_all_public_reference_examples_and_capture_options() {
    assert_eq!(
        form::DEMOS.iter().map(|demo| demo.id).collect::<Vec<_>>(),
        [
            "basic",
            "control-hooks",
            "layout",
            "layout-multiple",
            "disabled",
            "variant",
            "required-mark",
            "size",
            "layout-can-wrap",
            "warning-only",
            "useWatch",
            "validate-trigger",
            "validate-only",
            "form-item-path",
            "dynamic-form-item",
            "dynamic-form-items",
            "dynamic-form-items-drag-sorting",
            "dynamic-form-items-complex",
            "nest-messages",
            "complex-form-control",
            "customized-form-controls",
            "global-state",
            "form-context",
            "inline-login",
            "login",
            "register",
            "advanced-search",
            "form-in-modal",
            "time-related-controls",
            "without-form-create",
            "validate-static",
            "dynamic-rule",
            "form-dependencies",
            "getValueProps-normalize",
            "validate-scroll-to-field",
            "validate-other",
            "style-class",
        ]
    );
    assert_eq!(
        form::DEMOS
            .iter()
            .filter(|demo| matches!(
                demo.coverage,
                super::super::input_examples::Coverage::Partial(_)
            ))
            .count(),
        6
    );
    let options = Options::parse([
        "--component".into(),
        "form".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/form-gallery.png".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Form);
    assert!(!options.interactive());
    assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Form)
            .unwrap()
            .group,
        "Data Entry"
    );
}

#[test]
fn form_gallery_basic_submission_and_reset_use_retained_form_state() {
    let mut fixture = Fixture::new();
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::Form;
        cx.notify();
    });
    fixture.draw();
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.form.clone());
    let panel = page.read_with(&fixture.cx, |page, _| page.panels["basic"].clone());
    let form = panel.read_with(&fixture.cx, |panel, _| panel.form.clone());
    fixture.cx.update(|window, cx| {
        form.update(cx, |form, cx| {
            form.submit(window, cx);
        })
    });
    fixture.draw();
    assert!(form.read_with(&fixture.cx, |form, _| {
        form.field_errors()
            .iter()
            .any(|field| !field.errors.is_empty())
    }));
    fixture.cx.update(|window, cx| {
        form.update(cx, |form, cx| {
            form.set_fields_value(
                FormValue::object([("username", "Ada"), ("password", "native-gpui")]),
                window,
                cx,
            )
            .unwrap();
            form.validate_fields(ValidateOptions::default(), window, cx);
        })
    });
    fixture.draw();
    assert!(form.read_with(&fixture.cx, |form, _| {
        form.field_errors()
            .iter()
            .all(|field| field.errors.is_empty())
    }));
    fixture
        .cx
        .update(|window, cx| form.update(cx, |form, cx| form.reset(window, cx)));
    fixture.draw();
    assert!(!form.read_with(&fixture.cx, |form, _| form.is_fields_touched(None, false)));
}

#[test]
fn form_gallery_cards_reflow_and_values_survive_theme_changes() {
    let mut fixture = Fixture::new();
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::Form;
        cx.notify();
    });
    fixture.draw();
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.form.clone());
    let panel = page.read_with(&fixture.cx, |page, _| page.panels["basic"].clone());
    let form = panel.read_with(&fixture.cx, |panel, _| panel.form.clone());
    fixture.cx.update(|window, cx| {
        form.update(cx, |form, cx| {
            form.set_field_value("username", FormValue::from("Kept"), window, cx)
                .unwrap()
        })
    });
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let content = fixture.bounds("gallery.content");
        for demo in form::DEMOS {
            let selector = Box::leak(format!("gallery.form.card.{}", demo.id).into_boxed_str());
            let bounds = fixture.bounds(selector);
            assert!(
                bounds.left() >= content.left() && bounds.right() <= content.right() + px(1.),
                "{} at {width}",
                demo.id
            );
        }
    }
    fixture.resize(1280.);
    for selector in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        fixture.click(selector);
        assert_eq!(
            form.read_with(&fixture.cx, |form, _| form
                .get_field_value("username")
                .as_str()
                .map(str::to_owned)),
            Some("Kept".into())
        );
    }
}

#[test]
fn form_gallery_failed_submission_reveals_the_error_in_the_existing_page_scroll_owner() {
    let mut fixture = Fixture::new();
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::Form;
        cx.notify();
    });
    fixture.draw();
    let (page, scroll) = fixture.gallery.read_with(&fixture.cx, |gallery, _| {
        (gallery.form.clone(), gallery.scroll.clone())
    });
    let panel = page.read_with(&fixture.cx, |page, _| {
        page.panels["validate-scroll-to-field"].clone()
    });
    let form = panel.read_with(&fixture.cx, |panel, _| panel.form.clone());
    assert_eq!(scroll.offset().y, px(0.));
    fixture.cx.update(|window, cx| {
        form.update(cx, |form, cx| {
            form.submit(window, cx);
        });
    });
    fixture.draw();
    for _ in 0..4 {
        fixture.next_frame();
    }
    assert!(scroll.offset().y < px(-500.));
    fixture.cx.update(|_, cx| {
        let error = form
            .read(cx)
            .field_errors()
            .into_iter()
            .find(|field| !field.errors.is_empty())
            .expect("required native field failed");
        let field = form.read(cx).field(&error.name).unwrap();
        let bounds = field.bounds(cx).unwrap();
        let viewport = scroll.bounds();
        assert!(bounds.top() >= viewport.top() - px(1.));
        assert!(bounds.bottom() <= viewport.bottom() + px(1.));
    });
}
