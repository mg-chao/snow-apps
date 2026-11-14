// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Native inspection host. The controls here are private probes, not library components.

use ant_design_gpui::{
    ConfigScope, Direction, UiRoot,
    behavior::{Accessibility, ActivationGuard, Availability, focus_handle},
    icons::{IconName, icon},
    locale::{Locale, LocaleId},
    theme::{AlgorithmChain, CubicBezier, ThemeConfig},
};
use gpui::{
    AnimationExt, AnyElement, App, AppContext, Bounds, Context, FocusHandle, Focusable,
    IntoElement, Render, Role, SharedString, Subscription, Window, WindowBounds, WindowOptions,
    div, prelude::*, px, size,
};
use std::{path::PathBuf, time::Duration};

#[derive(Clone, Copy)]
enum Action {
    Theme,
    Density,
    Locale,
    Direction,
    Disabled,
    Busy,
    Motion,
    Invoke,
    DisabledProbe,
    BusyProbe,
}

struct Probe {
    action: Action,
    id: &'static str,
    focus: FocusHandle,
    guard: ActivationGuard,
    _blur: Subscription,
}

struct Foundation {
    scope: ConfigScope,
    probes: Vec<Probe>,
    dark: bool,
    compact: bool,
    invocations: usize,
    fixture: Option<PathBuf>,
    _config: Subscription,
}

impl Foundation {
    fn new(
        scope: ConfigScope,
        fixture: Option<PathBuf>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let probes = [
            (Action::Theme, "foundation.theme"),
            (Action::Density, "foundation.density"),
            (Action::Locale, "foundation.locale"),
            (Action::Direction, "foundation.direction"),
            (Action::Disabled, "foundation.toggle-disabled"),
            (Action::Busy, "foundation.toggle-busy"),
            (Action::Motion, "foundation.motion"),
            (Action::Invoke, "foundation.primary"),
            (Action::DisabledProbe, "foundation.disabled"),
            (Action::BusyProbe, "foundation.busy"),
        ]
        .into_iter()
        .map(|(action, id)| {
            let focus = focus_handle(cx);
            let guard = ActivationGuard::default();
            let subscription = guard.observe_blur(&focus, window, cx);
            Probe {
                action,
                id,
                focus,
                guard,
                _blur: subscription,
            }
        })
        .collect();
        let subscription = scope.observe(cx);
        if let Some(path) = &fixture {
            std::fs::write(path, "0\n").expect("write fixture state");
        }
        Self {
            scope,
            probes,
            dark: false,
            compact: false,
            invocations: 0,
            fixture,
            _config: subscription,
        }
    }

    fn label(&self, action: Action, locale: &LocaleId) -> SharedString {
        let index = match locale {
            LocaleId::ZhCn => 1,
            LocaleId::ZhTw => 2,
            _ => 0,
        };
        let labels = match action {
            Action::Theme => ["Toggle dark theme", "切换深色主题", "切換深色主題"],
            Action::Density => ["Toggle compact density", "切换紧凑密度", "切換緊湊密度"],
            Action::Locale => ["Change language", "切换语言", "切換語言"],
            Action::Direction => ["Toggle layout direction", "切换布局方向", "切換版面方向"],
            Action::Disabled => ["Toggle disabled state", "切换禁用状态", "切換停用狀態"],
            Action::Busy => ["Toggle busy state", "切换忙碌状态", "切換忙碌狀態"],
            Action::Motion => ["Toggle motion", "切换动画", "切換動畫"],
            Action::Invoke => ["Invoke probe", "触发测试", "觸發測試"],
            Action::DisabledProbe => ["Disabled probe", "禁用测试", "停用測試"],
            Action::BusyProbe => ["Busy probe", "忙碌测试", "忙碌測試"],
        };
        labels[index].into()
    }

    fn activate(&mut self, action: Action, cx: &mut Context<Self>) {
        let scope = self.scope.clone();
        match action {
            Action::Theme | Action::Density => {
                match action {
                    Action::Theme => self.dark = !self.dark,
                    _ => self.compact = !self.compact,
                }
                let algorithms = match (self.dark, self.compact) {
                    (false, false) => AlgorithmChain::default(),
                    (true, false) => AlgorithmChain::dark(),
                    (false, true) => AlgorithmChain::compact(),
                    (true, true) => AlgorithmChain::dark_compact(),
                };
                scope
                    .update_patch(cx, |patch| {
                        patch.theme = Some(ThemeConfig {
                            algorithms: Some(algorithms),
                            ..Default::default()
                        })
                    })
                    .expect("valid built-in theme");
            }
            Action::Locale => {
                let next = match scope.snapshot(cx).locale.id {
                    LocaleId::EnUs => Locale::zh_cn(),
                    LocaleId::ZhCn => Locale::zh_tw(),
                    _ => Locale::en_us(),
                };
                scope
                    .update_patch(cx, |patch| patch.locale.locale = Some(next))
                    .unwrap();
            }
            Action::Direction => {
                let direction = match scope.snapshot(cx).direction {
                    Direction::Ltr => Direction::Rtl,
                    Direction::Rtl => Direction::Ltr,
                };
                scope
                    .update_patch(cx, |patch| patch.direction = Some(direction))
                    .unwrap();
            }
            Action::Disabled => {
                let disabled = !scope.snapshot(cx).disabled;
                scope
                    .update_patch(cx, |patch| patch.disabled = Some(disabled))
                    .unwrap();
            }
            Action::Busy => {
                let guard = &self.probes[7].guard;
                let availability = guard.availability();
                guard.set_availability(Availability {
                    busy: !availability.busy,
                    ..availability
                });
            }
            Action::Motion => {
                let motion = !scope.snapshot(cx).motion;
                scope
                    .update_patch(cx, |patch| patch.motion = Some(motion))
                    .unwrap();
            }
            Action::Invoke | Action::DisabledProbe | Action::BusyProbe => {
                self.invocations += 1;
                if let Some(path) = &self.fixture {
                    std::fs::write(path, format!("{}\n", self.invocations))
                        .expect("write fixture invocation");
                }
            }
        }
        cx.notify();
    }

    fn probe(&mut self, index: usize, window: &mut Window, cx: &mut Context<Self>) -> AnyElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let probe = &self.probes[index];
        let action = probe.action;
        let label = self.label(action, &config.locale.id);
        let availability = Availability {
            disabled: matches!(action, Action::DisabledProbe)
                || matches!(action, Action::Invoke) && config.disabled,
            busy: matches!(action, Action::BusyProbe)
                || matches!(action, Action::Invoke) && probe.guard.availability().busy,
        };
        probe.guard.update(availability, &probe.focus, window, cx);
        let view = cx.entity().downgrade();
        let element = div()
            .id(probe.id)
            .when(availability.can_focus(), |element| {
                element.track_focus(&probe.focus)
            })
            .h(px(tokens.control_height as f32))
            .px(px(tokens.control_padding_horizontal as f32))
            .flex()
            .items_center()
            .rounded(px(tokens.border_radius as f32))
            .border_1()
            .border_color(tokens.color_border)
            .bg(tokens.color_bg_container)
            .when(!availability.disabled, |element| {
                element.hover(|style| style.bg(tokens.color_primary_bg))
            })
            .when(availability.disabled, |element| element.opacity(0.4))
            .child(label.clone());
        let element = probe.guard.install(element, move |_, _, cx| {
            let _ = view.update(cx, |this, cx| this.activate(action, cx));
        });
        let element = config.focus_ring().apply(element, &probe.focus);
        let mut semantics = Accessibility::new(Role::Button)
            .label(label)
            .automation_id(probe.id);
        semantics.disabled = availability.disabled;
        semantics.busy = availability.busy;
        semantics.apply(element).into_any_element()
    }
}

impl Render for Foundation {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let config = self.scope.snapshot(cx);
        let tokens = &config.theme.tokens;
        let palette = config.icon_palette;
        let index = match config.locale.id {
            LocaleId::ZhCn => 1,
            LocaleId::ZhTw => 2,
            _ => 0,
        };
        let title = [
            "Ant Design · foundation",
            "Ant Design · 基础设施",
            "Ant Design · 基礎設施",
        ][index];
        let description = [
            "Theme, locale, icons, focus, accessibility, and motion",
            "主题、语言、图标、焦点、无障碍与动画",
            "主題、語言、圖示、焦點、無障礙與動畫",
        ][index];
        let hint = [
            "Use Tab / Shift+Tab to navigate; Enter or Space to activate.",
            "使用 Tab / Shift+Tab 导航，Enter 或空格触发。",
            "使用 Tab / Shift+Tab 導覽，Enter 或空白鍵觸發。",
        ][index];
        let tools: Vec<_> = (0..7).map(|index| self.probe(index, window, cx)).collect();
        let probes: Vec<_> = (7..10).map(|index| self.probe(index, window, cx)).collect();
        let spinner = icon(IconName::LoadingOutlined)
            .palette(palette)
            .size(px(24.));
        let spinner = if let Some(animation) = config.motion_policy(cx).animation(
            Duration::from_secs(1),
            CubicBezier {
                x1: 0.,
                y1: 0.,
                x2: 1.,
                y2: 1.,
            },
        ) {
            spinner
                .with_animation(
                    "foundation-loading",
                    animation.repeat(),
                    |element, phase| element.rotation(gpui::radians(phase * std::f32::consts::TAU)),
                )
                .into_any_element()
        } else {
            spinner.into_any_element()
        };
        div()
            .size_full()
            .flex()
            .flex_col()
            .p_8()
            .gap_5()
            .child(div().text_size(px(30.)).child(title))
            .child(
                div()
                    .text_color(tokens.color_text_secondary)
                    .child(description),
            )
            .child(
                div()
                    .w_full()
                    .flex()
                    .flex_wrap()
                    .gap_2()
                    .when(config.direction == Direction::Rtl, |row| {
                        row.flex_row_reverse()
                    })
                    .children(tools),
            )
            .child(
                div()
                    .flex()
                    .gap_4()
                    .items_center()
                    .when(config.direction == Direction::Rtl, |row| {
                        row.flex_row_reverse()
                    })
                    .child(
                        icon(IconName::SearchOutlined)
                            .palette(palette)
                            .size(px(24.)),
                    )
                    .child(icon(IconName::HeartFilled).palette(palette).size(px(24.)))
                    .child(
                        icon(IconName::CheckCircleTwoTone)
                            .palette(palette)
                            .size(px(24.)),
                    )
                    .child(
                        icon(IconName::ArrowRightOutlined)
                            .palette(palette)
                            .mirror(config.direction == Direction::Rtl)
                            .size(px(24.)),
                    )
                    .child(spinner),
            )
            .child(
                div()
                    .flex()
                    .gap_3()
                    .when(config.direction == Direction::Rtl, |row| {
                        row.flex_row_reverse()
                    })
                    .children(probes),
            )
            .child(
                div()
                    .id("foundation-status")
                    .role(Role::Status)
                    .child(format!(
                        "{} · {} · {} · {}",
                        config.locale.id.as_str(),
                        if self.dark { "dark" } else { "light" },
                        if self.compact { "compact" } else { "standard" },
                        self.invocations
                    )),
            )
            .child(
                div()
                    .text_color(tokens.color_text_secondary)
                    .child(config.locale.common.placeholder.clone()),
            )
            .child(div().text_color(tokens.color_text_secondary).child(hint))
    }
}

fn main() {
    let mut arguments = std::env::args().skip(1);
    let fixture = match arguments.next().as_deref() {
        Some("--uia-fixture") => Some(PathBuf::from(
            arguments.next().expect("fixture output path"),
        )),
        None => None,
        Some(argument) => panic!("unknown argument: {argument}"),
    };
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        let scope = ConfigScope::new_window(cx);
        let bounds = Bounds::centered(None, size(px(1050.), px(560.)), cx);
        cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                ..Default::default()
            },
            |window, cx| {
                window.set_window_title("Ant Design GPUI foundation");
                let content = cx.new(|cx| Foundation::new(scope.clone(), fixture, window, cx));
                let root = cx.new(|cx| UiRoot::new(scope, content, window, cx));
                window.focus(&root.focus_handle(cx), cx);
                window.activate_window();
                root
            },
        )
        .expect("open foundation example");
        cx.activate(true);
    });
}
