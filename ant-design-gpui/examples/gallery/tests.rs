// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

use std::time::Duration;

use ant_design_gpui::{
    ConfigScope, Direction, UiRoot,
    button::{ButtonColor, ButtonStyle, ButtonStyleOptions, ButtonVariant},
    theme::Color,
};
use gpui::{
    AppContext, Bounds, Entity, FocusHandle, KeyDownEvent, KeyUpEvent, Keystroke, Modifiers,
    Pixels, PlatformInput, ScrollDelta, ScrollEvent, TestAppContext, VisualTestContext,
    WindowHandle, point, px, size,
};

use super::{
    Options,
    button::{ButtonPage, DEMOS, Preview, tests_support::Observations},
    catalog::{COMPONENTS, ComponentId},
    shell::Gallery,
};

struct Fixture {
    cx: VisualTestContext,
    gallery: Entity<Gallery>,
    page: Entity<ButtonPage>,
    observations: Observations,
    _window: WindowHandle<UiRoot>,
}

mod context_scroll;
mod input;
mod menu;
mod message;
mod pagination;
mod progress;
mod result;
mod select_slider;
mod spin;
mod tabs;

#[test]
fn alert_gallery_preserves_reference_order_and_supports_native_capture() {
    assert_eq!(
        super::alert::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "alert-basic",
            "alert-style",
            "alert-filled",
            "alert-closable",
            "alert-description",
            "alert-icon",
            "alert-banner",
            "alert-smooth-closed",
            "alert-custom-icon",
            "alert-action",
            "alert-custom-title-alignment",
            "alert-component-token",
            "alert-style-class",
        ],
    );
    let options = Options::parse([
        "--component".into(),
        "alert".into(),
        "--dark".into(),
        "--compact".into(),
        "--rtl".into(),
        "--reduced-motion".into(),
        "--headless".into(),
        "--capture".into(),
        "C:/temp/alert-gallery.png".into(),
    ])
    .unwrap();
    assert_eq!(options.component, ComponentId::Alert);
    assert!(!options.interactive());
    assert!(options.dark && options.compact && options.rtl && options.reduced_motion);
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Alert)
            .unwrap()
            .group,
        "Feedback",
    );
}

#[test]
fn alert_gallery_navigation_anchors_and_cards_follow_the_viewport() {
    let mut fixture = Fixture::new();
    fixture.reveal_sidebar("gallery.navigation.alert");
    fixture.click("gallery.navigation.alert");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Alert
    );
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.card.alert-basic");
        let second = fixture.bounds("gallery.card.alert-style");
        let content = fixture.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    fixture.resize(1280.);
    fixture.reveal_sidebar("gallery.jump.alert-smooth-closed");
    fixture.click("gallery.jump.alert-smooth-closed");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let smooth = fixture.bounds("gallery.card.alert-smooth-closed");
    assert!((smooth.top() - content.top()).abs() <= px(1.));
    fixture.click("gallery.control.top");
    fixture.next_frame();
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    assert_eq!(scroll.offset().y, px(0.));
}

#[test]
fn alert_gallery_close_finishes_before_owner_unmount_and_switch_remounts() {
    let mut fixture = Fixture::new();
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::Alert;
        cx.notify();
    });
    fixture.draw();
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.alert.clone());
    fixture.reveal("alert.smooth.item");
    let focus = page.read_with(&fixture.cx, |page, _| page.smooth_close_focus.clone());
    fixture.cx.update(|window, cx| window.focus(&focus, cx));
    fixture.key("space");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| (
            page.smooth_visible,
            page.close_requests,
            page.close_completions
        )),
        (false, 1, 1)
    );
    fixture.reveal("alert.smooth.toggle");
    fixture.click("alert.smooth.toggle");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| (
            page.smooth_visible,
            page.smooth_epoch
        )),
        (true, 1)
    );
    fixture.reveal("alert.smooth.item");
    fixture.cx.update(|window, cx| window.focus(&focus, cx));
    fixture.key("enter");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| (
            page.smooth_visible,
            page.close_requests,
            page.close_completions
        )),
        (false, 2, 2)
    );
}

#[test]
fn alert_gallery_actions_and_dismissal_survive_presentation_changes() {
    let mut fixture = Fixture::new();
    fixture.gallery.update(&mut fixture.cx, |gallery, cx| {
        gallery.selected = ComponentId::Alert;
        cx.notify();
    });
    fixture.draw();
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.alert.clone());
    fixture.reveal("alert.action.undo");
    fixture.click("alert.action.undo");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| (page.last_action, page.action_count)),
        (Some("UNDO"), 1)
    );
    let success = fixture.bounds("alert.action.success");
    assert!(
        success.size.height > px(0.),
        "the action leaves its alert visible"
    );
    fixture.reveal("alert.smooth.item");
    let focus = page.read_with(&fixture.cx, |page, _| page.smooth_close_focus.clone());
    fixture.cx.update(|window, cx| window.focus(&focus, cx));
    fixture.key("enter");
    assert!(!page.read_with(&fixture.cx, |page, _| page.smooth_visible));
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        fixture.click(control);
        assert_eq!(
            page.read_with(&fixture.cx, |page, _| (page.last_action, page.action_count)),
            (Some("UNDO"), 1)
        );
        assert!(!page.read_with(&fixture.cx, |page, _| page.smooth_visible));
    }
    fixture.reveal("alert.action.undo");
    fixture.click("alert.action.undo");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.action_count), 2);
}

#[test]
fn popup_gallery_pages_support_navigation_capture_selection_and_responsive_cards() {
    let mut f = Fixture::new();
    for (kind, id, first_id, second_id) in [
        (
            super::popup::Kind::Tooltip,
            ComponentId::Tooltip,
            "gallery.tooltip.card.basic",
            "gallery.tooltip.card.smooth-transition",
        ),
        (
            super::popup::Kind::Popover,
            ComponentId::Popover,
            "gallery.popover.card.basic",
            "gallery.popover.card.triggerType",
        ),
        (
            super::popup::Kind::Popconfirm,
            ComponentId::Popconfirm,
            "gallery.popconfirm.card.basic",
            "gallery.popconfirm.card.locale",
        ),
    ] {
        assert_eq!(
            Options::parse(["--component".into(), kind.name().into()])
                .unwrap()
                .component,
            id
        );
        f.gallery.update(&mut f.cx, |gallery, cx| {
            gallery.selected = id;
            gallery.scroll.set_offset(Default::default());
            cx.notify();
        });
        for width in [1280., 1100., 640.] {
            f.resize(width);
            let first = f.bounds(first_id);
            let second = f.bounds(second_id);
            let content = f.bounds("gallery.content");
            assert!(first.left() >= content.left());
            assert!(second.right() <= content.right());
            if width == 1280. {
                assert_eq!(first.top(), second.top());
            } else {
                assert!(second.top() >= first.bottom());
            }
        }
        f.resize(1280.);
        f.reveal_sidebar("gallery.jump.placement");
        f.click("gallery.jump.placement");
        f.next_frame();
        let placement = match kind {
            super::popup::Kind::Tooltip => "gallery.tooltip.card.placement",
            super::popup::Kind::Popover => "gallery.popover.card.placement",
            super::popup::Kind::Popconfirm => "gallery.popconfirm.card.placement",
        };
        assert!((f.bounds(placement).top() - f.bounds("gallery.content").top()).abs() <= px(1.));
    }
}

#[test]
fn switch_gallery_keeps_reference_order_and_supports_native_capture_selection() {
    assert_eq!(
        super::switch::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "basic",
            "disabled",
            "text",
            "size",
            "loading",
            "component-token",
            "style-class"
        ],
    );
    let component = COMPONENTS
        .iter()
        .find(|component| component.id == ComponentId::Switch)
        .expect("registered Switch page");
    assert_eq!(component.title, "Switch");
    assert_eq!(component.group, "Data Entry");
    assert_eq!(
        Options::parse(["--component".into(), "switch".into()])
            .unwrap()
            .component,
        ComponentId::Switch,
    );
}

#[test]
fn switch_gallery_layout_and_navigation_follow_window_width() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.switch");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Switch,
    );
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.switch.card.basic");
        let second = fixture.bounds("gallery.switch.card.disabled");
        let content = fixture.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    fixture.resize(1280.);
    fixture.reveal_sidebar("gallery.jump.text");
    fixture.click("gallery.jump.text");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let text = fixture.bounds("gallery.switch.card.text");
    assert!((text.top() - content.top()).abs() <= px(1.));
    fixture.click("gallery.control.top");
    fixture.next_frame();
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    assert_eq!(scroll.offset().y, px(0.));
}

#[test]
fn switch_gallery_pointer_keyboard_and_availability_examples_are_interactive() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.switch");
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.switch.clone());
    assert!(page.read_with(&fixture.cx, |page, _| page.checked));
    fixture.click("switch.basic");
    assert!(!page.read_with(&fixture.cx, |page, _| page.checked));
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.changes), 1);
    fixture.key("space");
    assert!(page.read_with(&fixture.cx, |page, _| page.checked));
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.changes), 2);
    fixture.click("switch.disabled");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.changes), 2);
    fixture.click("switch.disabled.toggle");
    assert!(!page.read_with(&fixture.cx, |page, _| page.disabled));
    fixture.click("switch.disabled");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.changes), 3);
    fixture.reveal("switch.loading.normal");
    fixture.click("switch.loading.normal");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.changes), 3);
    fixture.click("switch.loading.toggle");
    assert!(!page.read_with(&fixture.cx, |page, _| page.loading));
    fixture.click("switch.loading.normal");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.changes), 4);
}

#[test]
fn switch_gallery_sizes_tokens_and_state_survive_display_changes() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.switch");
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.switch.clone());
    assert_eq!(
        fixture.bounds("switch.size.normal").size,
        size(px(44.), px(22.))
    );
    assert_eq!(
        fixture.bounds("switch.size.small").size,
        size(px(28.), px(16.))
    );
    assert_eq!(
        fixture.bounds("switch.component-token").size,
        // The default 9px + 24px content margins exceed this theme's 32px minimum.
        size(px(33.), px(14.))
    );
    fixture.click("switch.basic");
    for selector in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        fixture.click(selector);
        assert!(!page.read_with(&fixture.cx, |page, _| page.checked));
        assert_eq!(page.read_with(&fixture.cx, |page, _| page.changes), 1);
    }
    let scope = fixture.scope();
    assert_eq!(
        fixture.cx.update(|_, cx| scope.snapshot(cx).direction),
        Direction::Rtl
    );
    fixture.click("gallery.navigation.button");
    fixture.click("gallery.navigation.switch");
    assert!(!page.read_with(&fixture.cx, |page, _| page.checked));
}

impl Fixture {
    fn new() -> Self {
        let mut cx = TestAppContext::single();
        let mut gallery = None;
        let window = cx.add_window(|window, cx| {
            let scope = ConfigScope::new_window(cx);
            scope
                .update_patch(cx, |patch| patch.motion = Some(false))
                .unwrap();
            let content = cx.new(|cx| Gallery::new(scope.clone(), window, cx));
            gallery = Some(content.clone());
            UiRoot::new(scope, content, window, cx)
        });
        let gallery = gallery.unwrap();
        let page = gallery.read_with(&cx, |gallery, _| gallery.button.clone());
        let observations = page.read_with(&cx, |page, _| page.observations.clone());
        let mut fixture = Self {
            cx: VisualTestContext::from_window(window.into(), &cx),
            gallery,
            page,
            observations,
            _window: window,
        };
        fixture.resize(1280.);
        fixture
    }

    fn draw(&mut self) {
        self.cx.run_until_parked();
        self.cx.update(|window, cx| window.draw(cx).clear(cx));
        self.cx.run_until_parked();
    }

    fn resize(&mut self, width: f32) {
        self.cx.simulate_resize(size(px(width), px(900.)));
        self.draw();
    }

    fn next_frame(&mut self) {
        self.cx.update(|window, cx| {
            window.simulate_next_frame(cx);
        });
        self.draw();
    }

    fn click(&mut self, selector: &'static str) {
        let bounds = self.bounds(selector);
        self.cx.simulate_click(bounds.center(), Modifiers::none());
        self.draw();
    }

    fn click_checkbox(&mut self, selector: &'static str) {
        let bounds = self.bounds(selector);
        // These example wrappers stretch to fill their column; hit the box.
        self.cx.simulate_click(
            point(bounds.left() + px(8.), bounds.center().y),
            Modifiers::none(),
        );
        self.draw();
    }

    fn scope(&self) -> ConfigScope {
        self.gallery
            .read_with(&self.cx, |gallery, _| gallery.scope.clone())
    }

    fn bounds(&mut self, selector: &'static str) -> Bounds<Pixels> {
        self.cx
            .debug_bounds(selector)
            .unwrap_or_else(|| panic!("missing element: {selector}"))
    }

    fn focused(&mut self, id: &str) -> bool {
        let focus = self
            .observations
            .borrow()
            .focus
            .get(id)
            .expect("rendered control")
            .clone();
        self.cx.update(|window, _| focus.is_focused(window))
    }

    fn focus(&mut self, id: &str) -> FocusHandle {
        let focus = self
            .observations
            .borrow()
            .focus
            .get(id)
            .expect("rendered control")
            .clone();
        self.cx.update(|window, cx| window.focus(&focus, cx));
        self.draw();
        focus
    }

    fn scroll(&mut self, position: gpui::Point<Pixels>, delta: f32) {
        self.cx.simulate_event(ScrollEvent {
            position,
            delta: ScrollDelta::Pixels(point(px(0.), px(delta))),
            ..Default::default()
        });
        self.draw();
    }

    fn key(&mut self, stroke: &'static str) {
        // GPUI's simulate_keystrokes sends only KeyDown; Button also needs KeyUp.
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

    fn reveal(&mut self, selector: &'static str) {
        let bounds = self.bounds(selector);
        let scroll = self
            .gallery
            .read_with(&self.cx, |gallery, _| gallery.scroll.clone());
        let offset = scroll.offset();
        scroll.set_offset(point(px(0.), offset.y - bounds.top() + px(180.)));
        self.gallery.update(&mut self.cx, |_, cx| cx.notify());
        self.draw();
    }

    fn reveal_sidebar(&mut self, selector: &'static str) {
        let sidebar = self.bounds("gallery.sidebar").center();
        let target = self.bounds(selector).center();
        self.scroll(sidebar, f32::from(sidebar.y - target.y));
    }
}

#[test]
fn catalogue_keeps_reference_order_and_pending_cards_explain_their_status_without_fake_controls() {
    assert!(COMPONENTS.len() >= 4);
    assert_eq!(COMPONENTS[0].group, "General");
    assert_eq!(COMPONENTS[1].id, ComponentId::Divider);
    assert_eq!(COMPONENTS[1].group, "Layout");
    assert_eq!(COMPONENTS[2].id, ComponentId::ScrollArea);
    assert_eq!(COMPONENTS[2].group, "Layout");
    assert_eq!(COMPONENTS[3].id, ComponentId::ContextMenu);
    assert_eq!(COMPONENTS[3].group, "Navigation");
    assert_eq!(COMPONENTS[4].id, ComponentId::Menu);
    assert_eq!(COMPONENTS[4].group, "Navigation");
    assert_eq!(COMPONENTS[5].id, ComponentId::Pagination);
    assert_eq!(COMPONENTS[5].group, "Navigation");
    assert_eq!(COMPONENTS[6].id, ComponentId::Tabs);
    assert_eq!(COMPONENTS[6].group, "Navigation");
    assert_eq!(COMPONENTS[7].id, ComponentId::Checkbox);
    assert_eq!(COMPONENTS[7].group, "Data Entry");
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Segmented)
            .unwrap()
            .group,
        "Data Entry"
    );
    assert_eq!(
        DEMOS.iter().map(|demo| demo.id).collect::<Vec<_>>(),
        [
            "basic",
            "color-variant",
            "icon",
            "icon-placement",
            "size",
            "disabled",
            "loading",
            "multiple",
            "ghost",
            "danger",
            "block",
            "linear-gradient",
            "wave",
            "custom-disabled-bg",
            "style-class",
        ]
    );
    assert_eq!(
        DEMOS
            .iter()
            .filter(|demo| matches!(demo.preview, Preview::Pending(_)))
            .count(),
        6
    );

    let mut fixture = Fixture::new();
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Button
    );
    assert_eq!(fixture.observations.borrow().semantics.len(), 90);
    assert!(
        fixture
            .observations
            .borrow()
            .semantics
            .contains_key("style-class.Object")
    );
    assert!(
        fixture
            .observations
            .borrow()
            .semantics
            .contains_key("style-class.Function")
    );
    for demo in DEMOS {
        assert!(!demo.description.is_empty());
        if let Preview::Pending(missing) = demo.preview {
            assert!(!missing.is_empty());
            let prefix = format!("{}.", demo.id);
            assert!(
                !fixture
                    .observations
                    .borrow()
                    .semantics
                    .keys()
                    .any(|id| id.starts_with(&prefix))
            );
        }
    }
    for selector in ["gallery.preview.loading", "gallery.preview.style-class"] {
        assert!(fixture.bounds(selector).size.height >= px(120.));
    }
    for selector in [
        "gallery.preview.icon",
        "gallery.preview.size",
        "gallery.preview.wave",
    ] {
        let height = fixture.bounds(selector).size.height;
        assert!(height >= px(64.) && height < px(120.));
    }

    let selectors = [
        "gallery.card.basic",
        "gallery.card.color-variant",
        "gallery.card.icon",
        "gallery.card.icon-placement",
        "gallery.card.size",
        "gallery.card.disabled",
        "gallery.card.loading",
        "gallery.card.multiple",
        "gallery.card.ghost",
        "gallery.card.danger",
        "gallery.card.block",
        "gallery.card.linear-gradient",
        "gallery.card.wave",
        "gallery.card.custom-disabled-bg",
        "gallery.card.style-class",
    ];
    let bounds: Vec<_> = selectors
        .into_iter()
        .map(|selector| fixture.bounds(selector))
        .collect();
    for (index, card) in bounds.iter().enumerate().skip(1) {
        if index % 2 == 0 {
            assert!(card.top() >= bounds[index - 2].bottom());
        } else {
            assert_eq!(card.top(), bounds[index - 1].top());
            assert!(card.left() > bounds[index - 1].left());
        }
    }
    assert_eq!(
        bounds.first().unwrap().size.width,
        bounds.last().unwrap().size.width
    );
}

#[test]
fn columns_follow_the_content_width_and_cards_stay_inside_the_pane() {
    let mut fixture = Fixture::new();
    for width in [1440., 1280., 1136., 1135., 1040., 1039., 800., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.card.basic");
        let second = fixture.bounds("gallery.card.color-variant");
        let content = fixture.bounds("gallery.content");
        assert_eq!(
            fixture.bounds("gallery.sidebar").size.width,
            px(if width >= 1040. { 232. } else { 72. })
        );
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        let padding = if width < 800. { 24. } else { 32. };
        if content.size.width - px(padding * 2.) >= px(840.) {
            assert_eq!(first.top(), second.top());
            assert!(second.left() > first.right());
        } else {
            assert_eq!(first.left(), second.left());
            assert!(second.top() >= first.bottom());
        }
        let toolbar = fixture.bounds("gallery.toolbar");
        assert!(toolbar.bottom() <= content.top());
        assert!(toolbar.right() <= px(width));
        assert!(content.size.height > px(300.));
    }
}

#[test]
fn scrolling_moves_only_the_content_and_reselecting_button_preserves_position() {
    let mut fixture = Fixture::new();
    let sidebar = fixture.bounds("gallery.sidebar");
    let navigation = fixture.bounds("gallery.navigation.button");
    let toolbar = fixture.bounds("gallery.toolbar");
    let first = fixture.bounds("gallery.card.basic");
    fixture.scroll(point(px(1000.), px(700.)), -360.);
    assert_eq!(fixture.bounds("gallery.sidebar"), sidebar);
    assert_eq!(fixture.bounds("gallery.navigation.button"), navigation);
    assert_eq!(fixture.bounds("gallery.toolbar"), toolbar);
    assert!(fixture.bounds("gallery.card.basic").top() < first.top());
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    let offset = scroll.offset();
    fixture
        .cx
        .simulate_click(navigation.center(), Modifiers::none());
    fixture.draw();
    assert_eq!(scroll.offset(), offset);
    fixture.scroll(sidebar.center(), -360.);
    assert_eq!(scroll.offset(), offset);
}

#[test]
fn enabled_controls_support_keyboard_and_pointer_activation_and_disabled_controls_do_not() {
    let mut fixture = Fixture::new();
    fixture.focus("basic.Primary");
    assert!(fixture.focused("basic.Primary"));
    fixture.key("enter");
    fixture.key("space");
    assert_eq!(fixture.observations.borrow().activations.len(), 2);
    let primary = fixture.bounds("basic.Primary");
    fixture
        .cx
        .simulate_click(primary.center(), Modifiers::none());
    fixture.draw();
    assert_eq!(fixture.observations.borrow().activations.len(), 3);

    fixture.reveal("disabled.0.false");
    fixture.focus("disabled.0.false");
    fixture.cx.simulate_keystrokes("tab");
    fixture.draw();
    assert!(fixture.focused("disabled.1.false"));
    assert!(fixture.observations.borrow().semantics["disabled.0.true"].is_disabled());
    let count = fixture.observations.borrow().activations.len();
    let disabled = fixture.bounds("disabled.0.true");
    fixture
        .cx
        .simulate_click(disabled.center(), Modifiers::none());
    fixture.draw();
    assert_eq!(fixture.observations.borrow().activations.len(), count);
    assert!(!fixture.focused("disabled.0.true"));

    fixture.reveal("disabled.5.true");
    let disabled_link = fixture.bounds("disabled.5.true");
    fixture
        .cx
        .simulate_click(disabled_link.center(), Modifiers::none());
    fixture.draw();
    assert!(fixture.cx.opened_url().is_none());
    assert_eq!(fixture.observations.borrow().activations.len(), count);
    let enabled_link = fixture.bounds("disabled.5.false");
    fixture
        .cx
        .simulate_click(enabled_link.center(), Modifiers::none());
    fixture.draw();
    assert_eq!(
        fixture.cx.opened_url().as_deref(),
        Some("https://ant.design/index-cn")
    );
}

#[test]
fn custom_disabled_backgrounds_are_local_and_block_buttons_fill_the_preview() {
    let mut fixture = Fixture::new();
    fixture.page.read_with(&fixture.cx, |page, cx| {
        let normal = page.scope.snapshot(cx);
        let custom = page.disabled_scope.snapshot(cx);
        for (variant, alpha) in [(ButtonVariant::Outlined, 0.1), (ButtonVariant::Dashed, 0.4)] {
            let options = ButtonStyleOptions {
                color: ButtonColor::Default,
                variant,
                disabled: true,
                ..Default::default()
            };
            let customized = ButtonStyle::resolve(&custom.theme, options).rest.background;
            assert_eq!(customized, Color::BLACK.with_alpha(alpha));
            assert_ne!(
                ButtonStyle::resolve(&normal.theme, options).rest.background,
                customized
            );
        }
        assert_eq!(
            page.scope.snapshot(cx).theme.tokens.color_primary,
            normal.theme.tokens.color_primary
        );
    });
    let preview = fixture.bounds("gallery.preview.block");
    let primary = fixture.bounds("block.Primary");
    let default = fixture.bounds("block.Default");
    assert_eq!(primary.size.width, preview.size.width - px(48.));
    assert_eq!(primary.size.width, default.size.width);
}

#[test]
fn loading_demo_preserves_handlers_blocks_repeat_activation_and_resets_after_three_seconds() {
    let mut fixture = Fixture::new();
    fixture.reveal("loading.start");
    let initial = fixture.observations.borrow().activations.len();
    for (id, key) in [
        ("loading.start", "enter"),
        ("loading.end", "space"),
        ("loading.replace", "enter"),
        ("loading.only", "space"),
    ] {
        fixture.focus(id);
        fixture.key(key);
        assert!(fixture.focused(id));
        assert!(fixture.observations.borrow().semantics[id].is_busy());
        assert!(
            fixture.observations.borrow().semantics[id]
                .label()
                .is_some()
        );
        fixture.key("enter");
        fixture.key("space");
    }
    assert_eq!(fixture.observations.borrow().activations.len(), initial + 4);
    assert_eq!(
        fixture.page.read_with(&fixture.cx, |page, _| page.loading),
        [true; 4]
    );
    // The last two controls intentionally share the fourth request in the reference demo.
    assert!(fixture.observations.borrow().semantics["loading.custom-action"].is_busy());

    fixture
        .cx
        .executor()
        .advance_clock(Duration::from_millis(2999));
    fixture.draw();
    assert_eq!(
        fixture.page.read_with(&fixture.cx, |page, _| page.loading),
        [true; 4]
    );
    fixture
        .cx
        .executor()
        .advance_clock(Duration::from_millis(1));
    fixture.draw();
    assert_eq!(
        fixture.page.read_with(&fixture.cx, |page, _| page.loading),
        [false; 4]
    );
    for id in [
        "loading.start",
        "loading.end",
        "loading.replace",
        "loading.only",
        "loading.custom-action",
    ] {
        assert!(!fixture.observations.borrow().semantics[id].is_busy());
    }
    assert!(fixture.observations.borrow().semantics["loading.always"].is_busy());

    fixture.reveal("loading.custom-action");
    let button = fixture.bounds("loading.custom-action");
    fixture
        .cx
        .simulate_click(button.center(), Modifiers::none());
    fixture.draw();
    assert_eq!(fixture.observations.borrow().activations.len(), initial + 5);
    assert!(fixture.observations.borrow().semantics["loading.only"].is_busy());
    assert!(fixture.observations.borrow().semantics["loading.custom-action"].is_busy());
}

#[test]
fn toolbar_controls_support_keyboard_activation_and_update_the_live_scope() {
    let mut fixture = Fixture::new();
    let scope = fixture.scope();
    let initial = fixture.cx.update(|_, cx| scope.snapshot(cx));
    fixture.click("gallery.control.theme");
    let dark = fixture.cx.update(|_, cx| scope.snapshot(cx));
    assert_ne!(
        dark.theme.tokens.color_bg_layout,
        initial.theme.tokens.color_bg_layout
    );
    fixture.key("space");
    assert_eq!(
        fixture
            .cx
            .update(|_, cx| scope.snapshot(cx).theme.tokens.color_bg_layout),
        initial.theme.tokens.color_bg_layout
    );
    fixture.key("tab");
    fixture.key("space");
    let compact = fixture.cx.update(|_, cx| scope.snapshot(cx));
    assert!(compact.theme.tokens.control_height < initial.theme.tokens.control_height);
    fixture.key("shift-tab");
    fixture.key("enter");
    let dark_compact = fixture.cx.update(|_, cx| scope.snapshot(cx));
    assert_ne!(
        dark_compact.theme.tokens.color_bg_layout,
        compact.theme.tokens.color_bg_layout
    );
    assert_eq!(
        dark_compact.theme.tokens.control_height,
        compact.theme.tokens.control_height
    );
    fixture.click("gallery.control.direction");
    assert_eq!(
        fixture.cx.update(|_, cx| scope.snapshot(cx).direction),
        Direction::Rtl
    );
    fixture.click("gallery.control.motion");
    assert!(fixture.cx.update(|_, cx| scope.snapshot(cx).motion));
    fixture.click("gallery.control.motion");
    assert!(!fixture.cx.update(|_, cx| scope.snapshot(cx).motion));
}

#[test]
fn demo_jumps_follow_current_layout_and_top_control_restores_the_start() {
    let mut fixture = Fixture::new();
    fixture.resize(1440.);
    fixture.scroll(point(px(1000.), px(700.)), -250.);
    let loading_before_sidebar_scroll = fixture.bounds("gallery.card.loading");
    // The growing component catalogue puts later jump links below the sidebar
    // viewport. Reveal them through its own scroll area before pointer input.
    fixture.reveal_sidebar("gallery.jump.loading");
    assert_eq!(
        fixture.bounds("gallery.card.loading"),
        loading_before_sidebar_scroll
    );
    fixture.click("gallery.jump.loading");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let loading = fixture.bounds("gallery.card.loading");
    assert!((loading.top() - content.top()).abs() <= px(1.));
    fixture.resize(1100.);
    fixture.reveal_sidebar("gallery.jump.ghost");
    fixture.click("gallery.jump.ghost");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let ghost = fixture.bounds("gallery.card.ghost");
    assert!((ghost.top() - content.top()).abs() <= px(1.));
    fixture.click("gallery.control.top");
    fixture.next_frame();
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    assert_eq!(scroll.offset().y, px(0.));
    assert!(fixture.bounds("gallery.card.basic").top() >= fixture.bounds("gallery.content").top());
}

#[test]
fn presentation_changes_keep_scroll_position_and_live_loading_requests() {
    let mut fixture = Fixture::new();
    fixture.reveal("loading.start");
    fixture.focus("loading.start");
    fixture.key("enter");
    assert!(
        fixture
            .page
            .read_with(&fixture.cx, |page, _| page.loading[0])
    );
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    for selector in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        let offset = scroll.offset();
        fixture.click(selector);
        // Compact density can shorten the document below the previous offset.
        // Preserve position within the new native scroll range instead of
        // demanding an out-of-range offset or resetting to the top.
        assert_eq!(scroll.offset().x, offset.x);
        assert_eq!(scroll.offset().y, offset.y.max(-scroll.max_offset().y));
        assert!(
            fixture
                .page
                .read_with(&fixture.cx, |page, _| page.loading[0])
        );
        assert!(fixture.observations.borrow().semantics["loading.start"].is_busy());
    }
    fixture.focus("loading.start");
    let count = fixture.observations.borrow().activations.len();
    fixture.key("enter");
    fixture.key("space");
    assert_eq!(fixture.observations.borrow().activations.len(), count);
    fixture.cx.executor().advance_clock(Duration::from_secs(3));
    fixture.draw();
    assert!(
        !fixture
            .page
            .read_with(&fixture.cx, |page, _| page.loading[0])
    );
    assert!(!fixture.observations.borrow().semantics["loading.start"].is_busy());
}

#[test]
fn gallery_options_accept_presentation_flags_and_capture_and_reject_invalid_arguments() {
    let options = Options::parse(
        [
            "--capture",
            "C:/temp/gallery.png",
            "--reduced-motion",
            "--dark",
            "--compact",
            "--rtl",
        ]
        .map(String::from),
    )
    .unwrap();
    assert!(options.reduced_motion);
    assert!(options.dark && options.compact && options.rtl);
    let defaults = Options::parse([]).unwrap();
    assert!(!defaults.dark && !defaults.compact && !defaults.rtl && !defaults.reduced_motion);
    assert_eq!(
        options.capture.unwrap(),
        std::path::PathBuf::from("C:/temp/gallery.png")
    );
    assert!(Options::parse(["--capture".into()]).is_err());
    assert!(Options::parse(["--capture".into(), "--reduced-motion".into()]).is_err());
    assert!(Options::parse(["--unknown".into()]).is_err());
    assert_eq!(
        Options::parse(["--window-size".into(), "640x720".into()])
            .unwrap()
            .window_size,
        Some((640, 720))
    );
    for value in ["small", "640xNaN", "639x720", "640x479"] {
        assert!(Options::parse(["--window-size".into(), value.into()]).is_err());
    }
    assert!(Options::parse(["--window-size".into()]).is_err());
}

#[test]
fn gallery_capture_options_keep_navigation_pages_noninteractive_with_or_without_headless() {
    assert_eq!(
        Options::parse(["--headless".into()]).err().as_deref(),
        Some("--headless requires --capture"),
    );
    for (name, component) in [
        ("menu", ComponentId::Menu),
        ("pagination", ComponentId::Pagination),
        ("tabs", ComponentId::Tabs),
        ("modal", ComponentId::Modal),
        ("message", ComponentId::Message),
        ("progress", ComponentId::Progress),
        ("result", ComponentId::Result),
        ("spin", ComponentId::Spin),
    ] {
        let interactive = Options::parse(["--component".into(), name.into()]).unwrap();
        assert_eq!(interactive.component, component);
        assert!(interactive.interactive());
        assert!(!interactive.headless);
        for headless in [false, true] {
            let mut arguments = vec!["--component", name, "--capture", "gallery.png"];
            if headless {
                arguments.push("--headless");
            }
            let capture = Options::parse(arguments.into_iter().map(String::from)).unwrap();
            assert_eq!(capture.component, component);
            assert!(!capture.interactive());
            assert_eq!(capture.headless, headless);
            assert_eq!(capture.capture, Some("gallery.png".into()));
        }
    }
}

#[test]
fn color_matrix_uses_the_references_responsive_component_size() {
    let mut fixture = Fixture::new();
    let scope = fixture.scope();
    for width in [1280., 1599., 1600., 1920.] {
        fixture.resize(width);
        let expected = fixture.cx.update(|_, cx| {
            let config = scope.snapshot(cx);
            if width < config.theme.tokens.screen_xxl as f32 {
                px(config.theme.tokens.control_height_sm as f32)
            } else {
                px(config.theme.tokens.control_height as f32)
            }
        });
        assert_eq!(
            fixture.bounds("color-variant.0.Solid").size.height,
            expected
        );
    }
}

#[test]
fn checkbox_gallery_keeps_reference_order_and_native_responsive_layout() {
    assert_eq!(
        super::checkbox::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "basic",
            "disabled",
            "controller",
            "group",
            "check-all",
            "layout",
            "style-class"
        ],
    );
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.checkbox");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Checkbox,
    );
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.checkbox.card.basic");
        let second = fixture.bounds("gallery.checkbox.card.disabled");
        let content = fixture.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    fixture.resize(1280.);
    fixture.reveal_sidebar("gallery.jump.controller");
    fixture.click("gallery.jump.controller");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let card = fixture.bounds("gallery.checkbox.card.controller");
    assert!((card.top() - content.top()).abs() <= px(1.));
    fixture.click("gallery.control.top");
    fixture.next_frame();
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    assert_eq!(scroll.offset().y, px(0.));
}

#[test]
fn checkbox_controller_and_check_all_examples_update_application_state() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.checkbox");
    fixture.reveal("checkbox.controller");
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.checkbox.clone());
    fixture.click_checkbox("checkbox.controller");
    assert!(!page.read_with(&fixture.cx, |page, _| page.checked));
    fixture.click("checkbox.controller.disable");
    assert!(page.read_with(&fixture.cx, |page, _| page.disabled));
    fixture.click_checkbox("checkbox.controller");
    assert!(!page.read_with(&fixture.cx, |page, _| page.checked));
    fixture.click("checkbox.controller.toggle");
    assert!(page.read_with(&fixture.cx, |page, _| page.checked));

    fixture.reveal("checkbox.check-all");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| page.checked_list.len()),
        2
    );
    fixture.click_checkbox("checkbox.check-all");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| page.checked_list.clone()),
        ["Apple", "Pear", "Orange"]
    );
    fixture.click_checkbox("checkbox.check-all");
    assert!(page.read_with(&fixture.cx, |page, _| page.checked_list.is_empty()));
    fixture.click("gallery.control.theme");
    fixture.click("gallery.control.density");
    fixture.click("gallery.control.direction");
    assert!(page.read_with(&fixture.cx, |page, _| page.checked_list.is_empty()));
    fixture.click("gallery.navigation.button");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Button
    );
}

#[test]
fn checkbox_gallery_can_be_selected_for_native_capture() {
    assert_eq!(
        Options::parse(["--component".into(), "checkbox".into()])
            .unwrap()
            .component,
        ComponentId::Checkbox,
    );
    assert!(Options::parse(["--component".into()]).is_err());
    assert!(Options::parse(["--component".into(), "missing".into()]).is_err());
}

#[test]
fn divider_gallery_navigation_uses_its_own_previews_and_scroll_anchors() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.divider");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Divider,
    );
    for selector in [
        "gallery.preview.divider-horizontal",
        "gallery.preview.divider-with-text",
        "gallery.preview.divider-vertical",
    ] {
        let preview = fixture.bounds(selector);
        assert!(preview.size.height >= px(120.));
    }
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.card.divider-horizontal");
        let second = fixture.bounds("gallery.card.divider-with-text");
        let content = fixture.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    fixture.resize(1280.);
    fixture.reveal_sidebar("gallery.jump.divider-vertical");
    fixture.click("gallery.jump.divider-vertical");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let vertical = fixture.bounds("gallery.card.divider-vertical");
    assert!((vertical.top() - content.top()).abs() <= px(1.));
    fixture.click("gallery.control.top");
    fixture.next_frame();
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    assert_eq!(scroll.offset().y, px(0.));
    fixture.reveal_sidebar("gallery.navigation.button");
    fixture.click("gallery.navigation.button");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Button,
    );
}

#[test]
fn divider_gallery_can_be_selected_for_native_capture() {
    assert_eq!(
        Options::parse(["--component".into(), "divider".into()])
            .unwrap()
            .component,
        ComponentId::Divider,
    );
}

#[test]
fn tag_gallery_follows_reference_order_and_keeps_its_own_navigation_and_layout() {
    assert_eq!(
        super::tag::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "tag-basic",
            "tag-colorful",
            "tag-control",
            "tag-checkable",
            "tag-animation",
            "tag-icon",
            "tag-status",
            "tag-customize",
            "tag-draggable",
            "tag-component-token",
            "tag-disabled",
            "tag-style-class"
        ],
    );
    assert_eq!(
        Options::parse(["--component".into(), "tag".into()])
            .unwrap()
            .component,
        ComponentId::Tag
    );
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.tag");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Tag
    );
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.card.tag-basic");
        let second = fixture.bounds("gallery.card.tag-colorful");
        let content = fixture.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    fixture.resize(1280.);
    fixture.reveal_sidebar("gallery.jump.tag-checkable");
    fixture.click("gallery.jump.tag-checkable");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let checkable = fixture.bounds("gallery.card.tag-checkable");
    assert!((checkable.top() - content.top()).abs() <= px(1.));
    fixture.click("gallery.control.top");
    fixture.next_frame();
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    assert_eq!(scroll.offset().y, px(0.));
}

#[test]
fn tag_gallery_dynamic_and_checkable_examples_update_retained_application_state() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.tag");
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.tag.clone());
    fixture.reveal("tag.dynamic.add");
    fixture.click("tag.dynamic.add");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| page.dynamic.clone()),
        [1, 2, 3, 4]
    );
    let item = fixture.bounds("tag.dynamic.item.4");
    fixture.cx.simulate_click(
        point(item.right() - px(10.), item.center().y),
        Modifiers::none(),
    );
    fixture.draw();
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| page.dynamic.clone()),
        [1, 2, 3]
    );
    fixture.reveal("tag.checkable.yes");
    fixture.click("tag.checkable.yes");
    assert!(!page.read_with(&fixture.cx, |page, _| page.checked));
    fixture.key("space");
    assert!(page.read_with(&fixture.cx, |page, _| page.checked));
    for control in [
        "gallery.control.theme",
        "gallery.control.density",
        "gallery.control.direction",
    ] {
        fixture.click(control);
        assert_eq!(
            page.read_with(&fixture.cx, |page, _| page.dynamic.clone()),
            [1, 2, 3]
        );
        assert!(page.read_with(&fixture.cx, |page, _| page.checked));
    }
    fixture.reveal("tag.animated.add");
    fixture.click("tag.animated.add");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| page.animated.clone()),
        [1, 2, 3, 4]
    );
}

#[test]
fn tag_gallery_drag_reorders_stable_tag_identities() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.tag");
    fixture.reveal("tag.drag.1");
    let source = fixture.bounds("tag.drag.1").center();
    let target = fixture.bounds("tag.drag.3").center();
    fixture
        .cx
        .simulate_mouse_down(source, gpui::MouseButton::Left, Modifiers::none());
    fixture
        .cx
        .simulate_mouse_move(target, gpui::MouseButton::Left, Modifiers::none());
    fixture.draw();
    fixture
        .cx
        .simulate_mouse_up(target, gpui::MouseButton::Left, Modifiers::none());
    fixture.draw();
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.tag.clone());
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| page.order.clone()),
        [2, 3, 1]
    );
}

#[test]
fn radio_gallery_preserves_reference_order_and_responsive_navigation() {
    assert_eq!(
        COMPONENTS
            .iter()
            .find(|component| component.id == ComponentId::Radio)
            .unwrap()
            .group,
        "Data Entry",
    );
    assert_eq!(
        super::radio::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "radio-basic",
            "radio-disabled",
            "radio-group",
            "radio-vertical",
            "radio-block",
            "radio-options",
            "radio-button",
            "radio-keyboard",
            "radio-size",
            "radio-solid",
            "radio-style-class",
            "radio-component-token",
        ],
    );
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.radio");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Radio,
    );
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.radio.card.radio-basic");
        let second = fixture.bounds("gallery.radio.card.radio-disabled");
        let content = fixture.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    fixture.resize(1280.);
    fixture.reveal_sidebar("gallery.jump.radio-vertical");
    fixture.click("gallery.jump.radio-vertical");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let vertical = fixture.bounds("gallery.radio.card.radio-vertical");
    assert!((vertical.top() - content.top()).abs() <= px(1.));
    fixture.click("gallery.control.top");
    fixture.next_frame();
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.scroll.offset().y),
        px(0.),
    );
}

#[test]
fn radio_gallery_controlled_selection_and_availability_survive_appearance_changes() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.radio");
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.radio.clone());
    assert!(page.read_with(&fixture.cx, |page, _| page.disabled));
    fixture.click("radio.disabled.toggle");
    assert!(!page.read_with(&fixture.cx, |page, _| page.disabled));

    fixture.reveal("radio.controlled");
    fixture.click_checkbox("radio.controlled");
    fixture.key("right");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.value), 2);
    fixture.key("right");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.value), 3);
    fixture.key("space");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.value), 3);
    fixture.click("gallery.control.theme");
    fixture.click("gallery.control.density");
    fixture.click("gallery.control.direction");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.value), 3);
    assert!(!page.read_with(&fixture.cx, |page, _| page.disabled));
}

#[test]
fn radio_gallery_can_be_selected_for_native_capture() {
    assert_eq!(
        Options::parse(["--component".into(), "radio".into()])
            .unwrap()
            .component,
        ComponentId::Radio,
    );
}

#[test]
fn segmented_gallery_keeps_reference_order_and_supports_capture_selection() {
    assert_eq!(
        super::segmented::DEMOS
            .iter()
            .map(|demo| demo.id)
            .collect::<Vec<_>>(),
        [
            "segmented-basic",
            "segmented-vertical",
            "segmented-block",
            "segmented-shape",
            "segmented-disabled",
            "segmented-controlled",
            "segmented-custom",
            "segmented-dynamic",
            "segmented-size",
            "segmented-with-icon",
            "segmented-icon-only",
            "segmented-with-name",
            "segmented-style-class",
        ],
    );
    assert_eq!(
        Options::parse(["--component".into(), "segmented".into()])
            .unwrap()
            .component,
        ComponentId::Segmented,
    );
}

#[test]
fn segmented_gallery_navigation_is_responsive_and_has_independent_anchors() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.segmented");
    assert_eq!(
        fixture
            .gallery
            .read_with(&fixture.cx, |gallery, _| gallery.selected),
        ComponentId::Segmented
    );
    for width in [1280., 1100., 640.] {
        fixture.resize(width);
        let first = fixture.bounds("gallery.segmented.card.segmented-basic");
        let second = fixture.bounds("gallery.segmented.card.segmented-vertical");
        let content = fixture.bounds("gallery.content");
        assert!(first.left() >= content.left());
        assert!(second.right() <= content.right());
        if width == 1280. {
            assert_eq!(first.top(), second.top());
        } else {
            assert!(second.top() >= first.bottom());
        }
    }
    fixture.resize(1280.);
    fixture.reveal_sidebar("gallery.jump.segmented-custom");
    fixture.click("gallery.jump.segmented-custom");
    fixture.next_frame();
    let content = fixture.bounds("gallery.content");
    let custom = fixture.bounds("gallery.segmented.card.segmented-custom");
    assert!((custom.top() - content.top()).abs() <= px(1.));
    fixture.click("gallery.control.top");
    fixture.next_frame();
    let scroll = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.scroll.clone());
    assert_eq!(scroll.offset().y, px(0.));
}

#[test]
fn segmented_gallery_controlled_and_dynamic_examples_keep_state_across_display_changes() {
    let mut fixture = Fixture::new();
    fixture.click("gallery.navigation.segmented");
    fixture.reveal("segmented.controlled");
    let page = fixture
        .gallery
        .read_with(&fixture.cx, |gallery, _| gallery.segmented.clone());
    let control = fixture.bounds("segmented.controlled");
    fixture.cx.simulate_click(
        point(control.left() + px(8.), control.center().y),
        Modifiers::none(),
    );
    fixture.draw();
    fixture.key("right");
    assert_eq!(page.read_with(&fixture.cx, |page, _| page.value), "Transit");
    fixture.key("right");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| page.value),
        "Satellite"
    );
    fixture.reveal("segmented.dynamic.load");
    fixture.click("segmented.dynamic.load");
    assert!(page.read_with(&fixture.cx, |page, _| page.more_loaded));
    fixture.click("gallery.control.theme");
    fixture.click("gallery.control.density");
    fixture.click("gallery.control.direction");
    assert_eq!(
        page.read_with(&fixture.cx, |page, _| page.value),
        "Satellite"
    );
    assert!(page.read_with(&fixture.cx, |page, _| page.more_loaded));
    fixture.click("gallery.navigation.button");
    fixture.click("gallery.navigation.segmented");
    assert!(page.read_with(&fixture.cx, |page, _| page.more_loaded));
}
