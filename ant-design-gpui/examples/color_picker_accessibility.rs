// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0

//! Real Windows UIA fixture. Hidden-window range, edit and invoke actions leave
//! the foreground application and operating-system input stream untouched.
use ant_design_gpui::{
    Button, ColorPicker, ColorPickerEvent, ColorPickerMode, ColorPickerPreset, ColorPickerState,
    ColorPickerValue, ColorStop, ConfigScope, PickerColor, UiRoot, locale::Locale,
};
use gpui::{
    App, AppContext, Bounds, Context, Entity, Focusable, IntoElement, KeyDownEvent, KeyUpEvent,
    Keystroke, PlatformInput, Render, Subscription, Window, WindowBounds, WindowOptions, div,
    prelude::*, px, size,
};
use serde_json::json;
use std::{path::PathBuf, time::Duration};

#[cfg(target_os = "windows")]
mod background_window {
    use std::{
        ffi::c_void,
        sync::atomic::{AtomicBool, AtomicUsize, Ordering},
    };
    type Hwnd = *mut c_void;
    type Subclass = unsafe extern "system" fn(Hwnd, u32, usize, isize, usize, usize) -> isize;
    static INSTALLED: AtomicBool = AtomicBool::new(false);
    static INTERCEPTED: AtomicUsize = AtomicUsize::new(0);
    #[link(name = "user32")]
    unsafe extern "system" {
        fn EnumWindows(
            callback: Option<unsafe extern "system" fn(Hwnd, isize) -> i32>,
            data: isize,
        ) -> i32;
        fn GetWindowThreadProcessId(window: Hwnd, process: *mut u32) -> u32;
        fn GetWindowTextW(window: Hwnd, text: *mut u16, capacity: i32) -> i32;
    }
    #[link(name = "comctl32")]
    unsafe extern "system" {
        fn SetWindowSubclass(
            window: Hwnd,
            callback: Option<Subclass>,
            id: usize,
            data: usize,
        ) -> i32;
        fn DefSubclassProc(window: Hwnd, message: u32, wparam: usize, lparam: isize) -> isize;
    }
    unsafe extern "system" fn guard(
        window: Hwnd,
        message: u32,
        wparam: usize,
        lparam: isize,
        _: usize,
        _: usize,
    ) -> isize {
        if message == 0x0021 {
            // WM_MOUSEACTIVATE, scoped to this fixture's HWND.
            INTERCEPTED.fetch_add(1, Ordering::Relaxed);
            return 3; // MA_NOACTIVATE. GPUI otherwise returns MA_ACTIVATE unconditionally.
        }
        unsafe { DefSubclassProc(window, message, wparam, lparam) }
    }
    unsafe extern "system" fn find(window: Hwnd, _: isize) -> i32 {
        let mut process = 0;
        unsafe {
            GetWindowThreadProcessId(window, &mut process);
        }
        if process != std::process::id() {
            return 1;
        }
        let mut text = [0u16; 256];
        let len = unsafe { GetWindowTextW(window, text.as_mut_ptr(), text.len() as i32) };
        let len = usize::try_from(len).unwrap_or(0).min(text.len());
        if String::from_utf16_lossy(&text[..len]) != "ColorPicker accessibility fixture" {
            return 1;
        }
        if unsafe { SetWindowSubclass(window, Some(guard), 0x4350, 0) } != 0 {
            INSTALLED.store(true, Ordering::Relaxed);
            0
        } else {
            1
        }
    }
    pub fn install() {
        if !INSTALLED.load(Ordering::Relaxed) {
            unsafe {
                EnumWindows(Some(find), 0);
            }
        }
    }
    pub fn status() -> (bool, usize) {
        (
            INSTALLED.load(Ordering::Relaxed),
            INTERCEPTED.load(Ordering::Relaxed),
        )
    }
}
#[cfg(not(target_os = "windows"))]
mod background_window {
    pub fn install() {}
    pub fn status() -> (bool, usize) {
        (false, 0)
    }
}

struct Fixture {
    inspect: bool,
    scope: ConfigScope,
    single: Entity<ColorPickerState>,
    disabled: Entity<ColorPickerState>,
    gradient: Entity<ColorPickerState>,
    changes: usize,
    completions: usize,
    ever_native_active: bool,
    _activation: Subscription,
    _events: Vec<Subscription>,
}
impl Render for Fixture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let presets = [ColorPickerPreset::new(
            "recommended",
            "Recommended",
            [
                PickerColor::parse("#ff0000").unwrap().into(),
                PickerColor::parse("#1677ff").unwrap().into(),
            ],
        )];
        div()
            .size_full()
            .p(px(48.))
            .flex()
            .flex_col()
            .gap(px(24.))
            .child(
                ColorPicker::new("single", &self.single)
                    .default_open(self.inspect)
                    .scope(self.scope.clone())
                    .allow_clear(true)
                    .show_text(true)
                    .presets(presets)
                    .automation_id("picker.single"),
            )
            .child(
                ColorPicker::new("disabled", &self.disabled)
                    .scope(self.scope.clone())
                    .disabled(true)
                    .accessibility_label("Disabled color")
                    .automation_id("picker.disabled"),
            )
            .child(
                ColorPicker::new("gradient", &self.gradient)
                    .scope(self.scope.clone())
                    .modes([ColorPickerMode::Gradient])
                    .show_text(true)
                    .accessibility_label("Choose gradient")
                    .automation_id("picker.gradient"),
            )
            .child(
                div().flex().gap(px(8.)).children([
                    Button::new("focus-trigger")
                        .label("Focus color trigger")
                        .automation_id("fixture.focus-trigger")
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.single.read(cx).focus_handle(cx).focus(window, cx);
                        })),
                    Button::new("focus-input")
                        .label("Focus color input")
                        .automation_id("fixture.focus-input")
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.single.update(cx, |s, cx| s.focus_input(0, window, cx))
                        })),
                    Button::new("locale")
                        .label("Use Traditional Chinese")
                        .automation_id("fixture.locale")
                        .on_click(cx.listener(|this, _, _, cx| {
                            this.scope
                                .update_patch(cx, |p| p.locale.locale = Some(Locale::zh_tw()))
                                .unwrap();
                            cx.notify();
                        })),
                    Button::new("focus-stop")
                        .label("Focus middle gradient stop")
                        .automation_id("fixture.focus-stop")
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.gradient
                                .update(cx, |s, cx| s.focus_gradient_stop(1, window, cx))
                        })),
                    Button::new("delete-stop")
                        .label("Delete focused gradient stop")
                        .automation_id("fixture.delete-stop")
                        .on_click(cx.listener(|this, _, window, cx| {
                            this.gradient
                                .update(cx, |s, cx| s.focus_gradient_stop(1, window, cx));
                            window.defer(cx, |window, cx| {
                                let keystroke = Keystroke::parse("delete").unwrap();
                                window.dispatch_event(
                                    PlatformInput::KeyDown(KeyDownEvent {
                                        keystroke: keystroke.clone(),
                                        is_held: false,
                                        prefer_character_input: false,
                                    }),
                                    cx,
                                );
                                window.dispatch_event(
                                    PlatformInput::KeyUp(KeyUpEvent { keystroke }),
                                    cx,
                                );
                            });
                        })),
                ]),
            )
    }
}
fn main() {
    let inspect = std::env::args().any(|arg| arg == "--inspect");
    let interactive = std::env::args().any(|arg| arg == "--interactive");
    let output = PathBuf::from(
        std::env::args()
            .nth(1)
            .expect("color_picker_accessibility <state.json>"),
    );
    gpui_platform::application().run(move |cx: &mut App| {
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx, _| { if cx.windows().is_empty() { cx.quit(); } }).detach();
        cx.open_window(WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(Bounds::new(gpui::point(px(0.), px(0.)), size(px(960.), px(720.))))),
            show: interactive, focus: interactive, ..Default::default()
        }, move |window, cx| {
            window.set_window_title("ColorPicker accessibility fixture");
            let scope = ConfigScope::new_window(cx);
            scope.update_patch(cx, |p| p.motion = Some(false)).unwrap();
            let single = cx.new(|cx| {
                let mut s = ColorPickerState::new(window, cx);
                s.set_value(PickerColor::parse("#1677ff").unwrap().into(), window, cx).unwrap(); s
            });
            let disabled = cx.new(|cx| {
                let mut s = ColorPickerState::new(window, cx);
                s.set_value(PickerColor::parse("#1677ff").unwrap().into(), window, cx).unwrap(); s
            });
            let gradient = cx.new(|cx| {
                let mut s = ColorPickerState::new(window, cx);
                s.set_value(ColorPickerValue::gradient([
                    ColorStop::new(PickerColor::parse("#ff0000").unwrap(), 0.).unwrap(),
                    ColorStop::new(PickerColor::parse("#00ff00").unwrap(), 50.).unwrap(),
                    ColorStop::new(PickerColor::parse("#0000ff").unwrap(), 100.).unwrap(),
                ]).unwrap(), window, cx).unwrap(); s
            });
            let fixture = cx.new(|cx| Fixture {
                inspect,
                _activation: cx.observe_window_activation(window, |this: &mut Fixture, window, cx| {
                    if window.is_window_active() { this.ever_native_active = true; cx.notify(); }
                }),
                _events: [&single, &gradient].map(|s| cx.subscribe(s, |this: &mut Fixture, _, event, cx| {
                    match event { ColorPickerEvent::Change(_) => this.changes += 1,
                        ColorPickerEvent::ChangeComplete(_) => this.completions += 1, _ => {} }
                    cx.notify();
                })).into(),
                scope: scope.clone(), single, disabled, gradient, changes: 0, completions: 0,
                ever_native_active: window.is_window_active(),
            });
            let observation = fixture.downgrade();
            window.spawn(cx, async move |cx| {
                for _ in 0..900 {
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    if cx.update(|window, cx| {
                        if !interactive { background_window::install(); }
                        window.draw(cx).clear(cx);
                        let Some(fixture) = observation.upgrade() else { return; };
                        let f = fixture.read(cx);
                        let gradient_count = match f.gradient.read(cx).value() {
                            ColorPickerValue::Gradient(stops) => stops.len(), _ => 0,
                        };
                        let value = json!({"process_id":std::process::id(),
                            "single":f.single.read(cx).value().to_css_string(),
                            "single_hex":f.single.read(cx).value().active_color(0).to_hex_string(),
                            "single_hsba":f.single.read(cx).value().active_color(0).to_hsb(),
                            "open":f.single.read(cx).is_open(),
                            "trigger_focused":f.single.read(cx).focus_handle(cx).is_focused(window),
                            "gradient_stops":gradient_count,
                            "changes":f.changes,"completions":f.completions,
                            "ever_native_active":f.ever_native_active,
                            "background_guard":background_window::status(),
                            "tree":window.debug_a11y_tree_json().and_then(|s| serde_json::from_str::<serde_json::Value>(&s).ok())});
                        let temporary = output.with_extension("tmp");
                        if let Err(error) = std::fs::write(&temporary, serde_json::to_vec_pretty(&value).unwrap())
                            .and_then(|_| std::fs::rename(temporary, &output)) {
                            eprintln!("Accessibility snapshot publication: {error}");
                        }
                    }).is_err() { break; }
                }
                cx.update(|_, cx| cx.quit()).ok();
            }).detach();
            cx.new(|cx| UiRoot::new(scope, fixture, window, cx))
        }).unwrap();
    });
}
