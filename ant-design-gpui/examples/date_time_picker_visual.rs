// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Independent shared-scene captures through a hidden native platform window.

use ant_design_gpui::{
    ConfigScope, Direction, Size, UiRoot, ValidationStatus, Variant,
    date_picker::{
        DatePickerTheme, DatePickerTokenPatch, DateRangeSelection, DateSelection,
        DateTimeRangeSelection, DateTimeSelection, DisabledTime, MultipleDateSelection, NaiveDate,
        NaiveDateTime, Picker, PickerFormat, PickerGeometry, PickerMode, PickerOptions,
        PickerPanelMode, PickerPreset, PickerSelection, PickerState, TimeOptions,
        TimeRangeSelection, TimeSelection, TimeUnit,
    },
    locale::Locale,
    theme::{AlgorithmChain, Color, ComponentThemeConfig, TokenPatch},
};
use chrono::Datelike;
use gpui::{
    AnyElement, App, AppContext, Bounds, Context, Entity, IntoElement, MouseMoveEvent, Pixels,
    PlatformInput, Render, Window, WindowBounds, WindowOptions, div, point, prelude::*, px, size,
};
use serde_json::{Value, json};
use std::{
    collections::HashMap,
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
    time::Duration,
};

static FOREGROUND_CHECKS: AtomicU64 = AtomicU64::new(0);

#[cfg(target_os = "windows")]
fn assert_background() -> u32 {
    #[link(name = "user32")]
    unsafe extern "system" {
        fn GetForegroundWindow() -> isize;
        fn GetWindowThreadProcessId(window: isize, process: *mut u32) -> u32;
    }
    let mut process = 0;
    unsafe {
        GetWindowThreadProcessId(GetForegroundWindow(), &mut process);
    }
    assert_ne!(
        process,
        std::process::id(),
        "hidden picker capture must preserve foreground focus"
    );
    FOREGROUND_CHECKS.fetch_add(1, Ordering::Relaxed);
    process
}

#[cfg(not(target_os = "windows"))]
fn assert_background() -> u32 {
    0
}

#[cfg(target_os = "windows")]
fn set_native_scale(title: &str, scale: f32, canvas: (f32, f32)) {
    #[repr(C)]
    #[derive(Default)]
    struct Rect {
        left: i32,
        top: i32,
        right: i32,
        bottom: i32,
    }
    #[link(name = "user32")]
    unsafe extern "system" {
        fn FindWindowW(class: *const u16, title: *const u16) -> isize;
        fn GetWindowRect(window: isize, rect: *mut Rect) -> i32;
        fn GetClientRect(window: isize, rect: *mut Rect) -> i32;
        fn SendMessageW(window: isize, message: u32, wparam: usize, lparam: isize) -> isize;
        fn SetWindowPos(
            window: isize,
            after: isize,
            x: i32,
            y: i32,
            width: i32,
            height: i32,
            flags: u32,
        ) -> i32;
    }
    let title: Vec<u16> = title.encode_utf16().chain(Some(0)).collect();
    unsafe {
        assert_background();
        let handle = FindWindowW(std::ptr::null(), title.as_ptr());
        assert_ne!(handle, 0, "owned hidden picker HWND");
        let mut bounds = Rect::default();
        assert_ne!(GetWindowRect(handle, &mut bounds), 0);
        let dpi = (96. * scale).round() as usize;
        SendMessageW(
            handle,
            0x02E0,
            dpi | (dpi << 16),
            (&bounds as *const Rect) as isize,
        );
        let mut client = Rect::default();
        assert_ne!(GetWindowRect(handle, &mut bounds), 0);
        assert_ne!(GetClientRect(handle, &mut client), 0);
        let client_width = (canvas.0 * scale).round() as i32;
        let client_height = (canvas.1 * scale).round() as i32;
        let width = client_width + bounds.right - bounds.left - client.right + client.left;
        let height = client_height + bounds.bottom - bounds.top - client.bottom + client.top;
        assert_ne!(
            SetWindowPos(handle, 0, 0, 0, width, height, 0x2 | 0x4 | 0x10),
            0
        );
        assert_ne!(GetClientRect(handle, &mut client), 0);
        assert_eq!(
            (client.right - client.left, client.bottom - client.top),
            (client_width, client_height)
        );
        assert_background();
    }
}

#[cfg(not(target_os = "windows"))]
fn set_native_scale(_: &str, _: f32, _: (f32, f32)) {}

fn date(value: &str) -> NaiveDate {
    NaiveDate::parse_from_str(&value[..10], "%Y-%m-%d").unwrap()
}
fn date_time(value: &str) -> NaiveDateTime {
    if value.contains('T') {
        NaiveDateTime::parse_from_str(value, "%Y-%m-%dT%H:%M:%S%.f").unwrap()
    } else {
        date(value).and_hms_opt(0, 0, 0).unwrap()
    }
}
fn slots(value: &Value) -> Vec<Option<NaiveDateTime>> {
    if let Some(values) = value.as_array() {
        values.iter().map(|v| v.as_str().map(date_time)).collect()
    } else {
        vec![value.as_str().map(date_time)]
    }
}
fn mode(sample: &Value) -> PickerMode {
    match sample["picker"].as_str() {
        Some("week") => PickerMode::Week,
        Some("month") => PickerMode::Month,
        Some("quarter") => PickerMode::Quarter,
        Some("year") => PickerMode::Year,
        _ => PickerMode::Date,
    }
}

fn picker<S: PickerSelection>(
    id: gpui::ElementId,
    state: &Entity<PickerState<S>>,
    sample: &Value,
    scope: ConfigScope,
) -> AnyElement {
    let mut time = TimeOptions {
        use_12_hours: sample["use12Hours"] == true,
        hour_step: sample["hourStep"].as_u64().unwrap_or(1) as u32,
        minute_step: sample["minuteStep"].as_u64().unwrap_or(1) as u32,
        second_step: sample["secondStep"].as_u64().unwrap_or(1) as u32,
        millisecond_step: sample["millisecondStep"].as_u64().unwrap_or(100) as u32,
        hide_disabled_options: sample["hideDisabledOptions"] == true,
        change_on_scroll: sample["changeOnScroll"] == true,
        ..Default::default()
    };
    let mut format = PickerFormat::default();
    if let Some(pattern) = sample["format"].as_str() {
        let pattern = match pattern {
            "HH:mm" => "%H:%M",
            "HH:mm:ss.SSS" => "%H:%M:%S%.3f",
            _ => panic!("unknown scene format"),
        };
        format = PickerFormat::new(pattern).unwrap();
        time.show_millisecond = pattern.contains('f');
    }
    let mut picker = Picker::<S>::new(id, state)
        .scope(scope.clone())
        .value(S::from_slots(&slots(&sample["value"])))
        .open(sample["open"] == true)
        .options(PickerOptions {
            mode: mode(sample),
            time,
            ..Default::default()
        })
        .default_picker_value(date("2024-02-15"))
        .clock(|| date_time("2024-02-15T13:24:35.120"))
        .format(format)
        .allow_clear(true)
        .input_read_only(sample["inputReadOnly"] == true)
        .disabled(sample["disabled"].as_bool().unwrap_or(false))
        .size(match sample["size"].as_str() {
            Some("small") => Size::Small,
            Some("large") => Size::Large,
            _ => Size::Medium,
        })
        .variant(match sample["variant"].as_str() {
            Some("filled") => Variant::Filled,
            Some("borderless") => Variant::Borderless,
            Some("underlined") => Variant::Underlined,
            _ => Variant::Outlined,
        })
        .status(match sample["status"].as_str() {
            Some("error") => ValidationStatus::Error,
            Some("warning") => ValidationStatus::Warning,
            _ => ValidationStatus::None,
        });
    if sample["mode"] == "decade" {
        picker = picker.panel_mode(PickerPanelMode::Decade);
    }
    if let Some(disabled) = sample["disabled"].as_array() {
        picker = picker.disabled_endpoints([disabled[0] == true, disabled[1] == true]);
    }
    if let Some(confirm) = sample["needConfirm"].as_bool() {
        picker = picker.need_confirm(confirm);
    }
    if let Some(value) = sample["minDate"].as_str() {
        picker = picker.min_date(date(value));
    }
    if let Some(value) = sample["maxDate"].as_str() {
        picker = picker.max_date(date(value));
    }
    if let Some(prefix) = sample["prefix"].as_str() {
        picker = picker.prefix(prefix.to_owned());
    }
    if sample["disabledDates"] == "weekends" {
        picker = picker.disabled_date(|date, _| date.weekday().number_from_monday() >= 6);
    }
    if sample["disabledTime"] == true {
        picker = picker.disabled_time(|_, _| {
            DisabledTime::new()
                .hours(|| vec![0, 1, 2, 3, 23])
                .minutes(|_| vec![0, 1, 2, 58, 59])
                .seconds(|_, _| vec![0, 1, 2, 58, 59])
                .milliseconds(|_, _, _| vec![0, 20, 980])
        });
    }
    if sample["presets"] == true {
        let values = if S::RANGE {
            vec![
                (
                    "week",
                    "This week",
                    vec![Some(date_time("2024-02-12")), Some(date_time("2024-02-18"))],
                ),
                (
                    "month",
                    "This month",
                    vec![Some(date_time("2024-02-01")), Some(date_time("2024-02-29"))],
                ),
            ]
        } else {
            vec![
                (
                    "today",
                    "Today",
                    vec![Some(date_time("2024-02-15T13:24:35.120"))],
                ),
                ("tomorrow", "Tomorrow", vec![Some(date_time("2024-02-16"))]),
            ]
        };
        picker =
            picker.presets(values.into_iter().map(|(key, label, slots)| {
                PickerPreset::<S>::new(key, label, S::from_slots(&slots))
            }));
    }
    if sample["extraFooter"] == true {
        picker = picker.extra_footer(|_, _| "Choose an available date");
    }
    if sample["cellRender"] == true {
        picker = picker.cell_render(move |context, origin, _, cx| {
            if context
                .date
                .is_some_and(|date| [1, 15].contains(&date.day()))
                && context.mode == PickerPanelMode::Date
            {
                let tokens = scope.snapshot(cx).theme.component::<DatePickerTheme>();
                let globals = tokens.globals();
                div()
                    .relative()
                    .size(px(tokens.cell_height as f32))
                    .border(px(globals.line_width as f32))
                    .border_color(gpui::rgb(0x1677ff))
                    .rounded(px(4.))
                    .flex()
                    .items_center()
                    .justify_center()
                    .child(context.date.unwrap().day().to_string())
                    // The source's custom root retains the picker-cell-inner
                    // class. Compose its inherited today pseudo inside the
                    // custom border, with no implicit HTML/CSS dependency.
                    .when(context.date == Some(context.today), |cell| {
                        cell.child(
                            div()
                                .absolute()
                                .inset_0()
                                .border(px(globals.line_width as f32))
                                .border_color(globals.color_primary)
                                .rounded(px(4.)),
                        )
                    })
                    .into_any_element()
            } else {
                origin
            }
        });
    }
    picker.into_any_element()
}

macro_rules! state_variants {
    ($($variant:ident: $selection:ty),+ $(,)?) => {
        enum NativeState { $($variant(Entity<PickerState<$selection>>),)+ }
        impl NativeState {
            fn render(&self, id: gpui::ElementId, sample: &Value, scope: ConfigScope) -> AnyElement {
                match self { $(Self::$variant(state)=>picker(id,state,sample,scope),)+ }
            }
            fn geometry(&self, cx: &App) -> PickerGeometry {
                match self { $(Self::$variant(state)=>state.read(cx).geometry().clone(),)+ }
            }
            fn input_values(&self, cx: &App) -> Vec<gpui::SharedString> {
                match self { $(Self::$variant(state)=>state.read(cx).input_values(cx),)+ }
            }
            fn focus_input(&self, index: usize, window: &mut Window, cx: &mut App) {
                match self { $(Self::$variant(state)=>state.update(cx,|s,cx|s.focus_input(index,window,cx)),)+ }
            }
        }
    };
}
state_variants! { Date:DateSelection, DateTime:DateTimeSelection, Multiple:MultipleDateSelection,
DateRange:DateRangeSelection, DateTimeRange:DateTimeRangeSelection, Time:TimeSelection, TimeRange:TimeRangeSelection }

struct Capture {
    scope: ConfigScope,
    manifest: Value,
    scene: String,
    state: NativeState,
    generation: u64,
}
impl Capture {
    fn state(sample: &Value, window: &mut Window, cx: &mut App) -> NativeState {
        if sample["component"] == "time" {
            if sample["range"] == true {
                NativeState::TimeRange(cx.new(|cx| PickerState::new(window, cx)))
            } else {
                NativeState::Time(cx.new(|cx| PickerState::new(window, cx)))
            }
        } else if sample["showTime"] == true {
            if sample["range"] == true {
                NativeState::DateTimeRange(cx.new(|cx| PickerState::new(window, cx)))
            } else {
                NativeState::DateTime(cx.new(|cx| PickerState::new(window, cx)))
            }
        } else if sample["range"] == true {
            NativeState::DateRange(cx.new(|cx| PickerState::new(window, cx)))
        } else if sample["multiple"] == true {
            NativeState::Multiple(cx.new(|cx| PickerState::new(window, cx)))
        } else {
            NativeState::Date(cx.new(|cx| PickerState::new(window, cx)))
        }
    }
    fn configure(&mut self, scene: &str, window: &mut Window, cx: &mut Context<Self>) {
        self.scene = scene.to_owned();
        self.state = Self::state(&self.manifest["scenes"][scene], window, cx);
        self.generation += 1;
        cx.notify();
    }
}
impl Render for Capture {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let sample = &self.manifest["scenes"][&self.scene];
        let config = self.scope.snapshot(cx);
        div()
            .w(px(1000.))
            .h(px(760.))
            .p(px(32.))
            .bg(config.theme.tokens.color_bg_container.to_hsla())
            .child(self.state.render(
                gpui::ElementId::from(format!("capture-{}", self.generation)),
                sample,
                self.scope.clone(),
            ))
    }
}

fn rect(value: Option<Bounds<Pixels>>) -> Value {
    value.map_or(Value::Null, |b| {
        json!({"x":f32::from(b.origin.x),"y":f32::from(b.origin.y),
        "width":f32::from(b.size.width),"height":f32::from(b.size.height)})
    })
}
fn intersects(a: Bounds<Pixels>, b: Bounds<Pixels>) -> bool {
    a.origin.y < b.origin.y + b.size.height && a.origin.y + a.size.height > b.origin.y
}
fn unit_order(unit: TimeUnit) -> u8 {
    match unit {
        TimeUnit::Hour => 0,
        TimeUnit::Minute => 1,
        TimeUnit::Second => 2,
        TimeUnit::Millisecond => 3,
        TimeUnit::Meridiem => 4,
    }
}
fn metrics(view: &Capture, window: &Window, cx: &App) -> Value {
    let geometry = view.state.geometry(cx);
    let values = view.state.input_values(cx);
    let time_cell_margin = view
        .scope
        .snapshot(cx)
        .theme
        .component::<DatePickerTheme>()
        .globals()
        .margin_xxs as f32;
    let mut calendar: Vec<_> = geometry
        .cells
        .values()
        .filter(|c| c.unit.is_none())
        .collect();
    let rtl = view.scope.snapshot(cx).direction == Direction::Rtl;
    calendar.sort_by(|a, b| {
        a.panel
            .cmp(&b.panel)
            .then_with(|| f32::from(a.bounds.origin.y).total_cmp(&f32::from(b.bounds.origin.y)))
            .then_with(|| {
                let order = f32::from(a.bounds.origin.x).total_cmp(&f32::from(b.bounds.origin.x));
                if rtl { order.reverse() } else { order }
            })
    });
    let mut time: Vec<_> = geometry
        .cells
        .values()
        .filter(|c| c.unit.is_some())
        .collect();
    time.sort_by_key(|c| (unit_order(c.unit.unwrap()), c.value));
    let mut units: Vec<_> = time.iter().map(|c| c.unit.unwrap()).collect();
    units.sort_by_key(|u| unit_order(*u));
    units.dedup();
    let mut bars: Vec<_> = geometry.scrollbars.iter().collect();
    bars.sort_by_key(|bar| bar.column);
    let scrollbars:Vec<_>=bars.into_iter().map(|bar|json!({"column":bar.column,"track":rect(Some(bar.rail)),
        "side":if geometry.time_columns.get(bar.column).is_some_and(|column|bar.rail.center().x<column.center().x){"left"}else{"right"},
        "scrollTop":-f32::from(bar.offset),"scrollHeight":f32::from(bar.content),"clientHeight":f32::from(bar.viewport),
        "scrollingTrack":rect(Some(bar.track)),"thumb":rect(Some(bar.thumb)),
        "decrement":rect(Some(bar.decrement)),"increment":rect(Some(bar.increment))})).collect();
    let border_candidates = |inner: Bounds<Pixels>| {
        let scale = window.scale_factor();
        let logical = |bounds: Bounds<gpui::ScaledPixels>| {
            Bounds::new(
                point(px(bounds.origin.x.0 / scale), px(bounds.origin.y.0 / scale)),
                size(
                    px(bounds.size.width.0 / scale),
                    px(bounds.size.height.0 / scale),
                ),
            )
        };
        window.rendered_scene().quads.iter().filter_map(|quad| {
            if quad.border_color.a == 0. || quad.border_widths.top.0 <= 0. { return None; }
            let bounds = logical(quad.bounds);
            if !inner.contains(&bounds.center())
                || f32::from(bounds.size.width - inner.size.width).abs() > 6.
                || f32::from(bounds.size.height - inner.size.height).abs() > 6. { return None; }
            let color = quad.border_color.to_rgb();
            Some(json!({"order":quad.order,"rect":rect(Some(bounds)), "clip":rect(Some(logical(quad.content_mask.bounds))),
                "rgba":[color.r,color.g,color.b,color.a],
                "widths":[quad.border_widths.top.0/scale,quad.border_widths.right.0/scale,
                    quad.border_widths.bottom.0/scale,quad.border_widths.left.0/scale]}))
        }).collect::<Vec<_>>()
    };
    json!({"trigger":rect(geometry.trigger),"prefix":rect(geometry.prefix),"inputs":geometry.inputs.iter().enumerate().map(|(index,b)|{
        let mut r=rect(Some(*b));r["value"]=json!(values.get(index).map(|v|v.as_ref()).unwrap_or(""));r}).collect::<Vec<_>>(),
        "popup":rect(geometry.popup),"panels":geometry.panels.iter().map(|b|rect(Some(*b))).collect::<Vec<_>>(),
        "headers":geometry.headers.iter().map(|b|rect(Some(*b))).collect::<Vec<_>>(),
        "tables":geometry.tables.iter().map(|b|rect(Some(*b))).collect::<Vec<_>>(),
        "footer":rect(geometry.footer),"presets":rect(geometry.presets),"activeBar":rect(geometry.active_bar),
        "timeColumns":geometry.time_columns.iter().map(|b|rect(Some(*b))).collect::<Vec<_>>(),
        "timeViewports":geometry.time_viewports.iter().map(|b|rect(Some(*b))).collect::<Vec<_>>(),
        "scrollbars":scrollbars,
        "footerActions":geometry.footer_actions.iter().map(|action|json!({"key":action.key.as_ref(),"label":action.label.as_ref(),"rect":rect(Some(action.bounds))})).collect::<Vec<_>>(),
        "tags":geometry.tags.iter().map(|tag|{
            let mut remove=rect(tag.remove);
            if let Some(slot)=tag.remove {
                // Read the actual tinted SVG sprite from the completed native
                // scene. Transform scaled coordinates before returning to logical
                // units; paint_size expands the artwork about its nominal slot.
                let colors:Vec<_>=window.rendered_scene().monochrome_sprites.iter().filter_map(|sprite|{
                    let center=sprite.bounds.center();let matrix=sprite.transformation.rotation_scale;
                    let translation=sprite.transformation.translation;let scale=window.scale_factor();
                    let center=point(px((matrix[0][0]*center.x.0+matrix[0][1]*center.y.0+translation[0])/scale),
                        px((matrix[1][0]*center.x.0+matrix[1][1]*center.y.0+translation[1])/scale));
                    if !slot.contains(&center){return None;}
                    let color=sprite.color.to_rgb();Some([color.r,color.g,color.b,color.a])
                }).collect();
                remove["paintRgbaCandidates"]=json!(colors);
            }
            json!({"key":tag.key.as_ref(),"label":tag.label.as_ref(),"rect":rect(Some(tag.bounds)),
                "content":rect(Some(tag.content)),"remove":remove})
        }).collect::<Vec<_>>(),
        "cells":calendar.into_iter().map(|c|json!({"key":c.key.as_ref(),"title":c.date.map(|d|d.to_string()),"text":c.text.as_ref(),
            "selected":c.selected,"today":c.today,"disabled":c.disabled,"inView":c.in_view,"inRange":c.in_range,
            "rangeStart":c.range_start,"rangeEnd":c.range_end,"rangeHover":c.range_hover,
            "weekSelected":c.week_selected,"weekRangeStart":c.week_range_start,"weekRangeEnd":c.week_range_end,
            "weekRangeHover":c.week_range_hover,"weekHover":c.week_hover,
            "paintBorderCandidates":if view.manifest["scenes"][&view.scene]["cellRender"]==true
                &&c.date.is_some_and(|date|[1,15].contains(&date.day())){border_candidates(c.inner)}else{Vec::new()},
            "rect":rect(Some(c.bounds)),"inner":rect(Some(c.inner))})).collect::<Vec<_>>(),
        "timeCells":time.into_iter().filter_map(|c|{
            let col=units.iter().position(|u|Some(*u)==c.unit).unwrap();
            if geometry.time_columns.get(col).is_none_or(|b|!intersects(c.bounds,*b)){return None;}
            Some(json!({"key":format!("{col}:{}",c.value.unwrap()),"column":col,"value":c.value.unwrap().to_string(),"text":c.text.as_ref(),
                "selected":c.selected,"disabled":c.disabled,
                "rect":rect(Some(Bounds::new(point(c.bounds.origin.x+px(time_cell_margin),c.bounds.origin.y),
                    size((c.bounds.size.width-px(time_cell_margin*2.)).max(px(0.)),c.bounds.size.height)))),
                "inner":rect(Some(c.inner))}))}).collect::<Vec<_>>()})
}

fn main() {
    let mut args = std::env::args().skip(1);
    assert_eq!(args.next().as_deref(), Some("--output"));
    let output = PathBuf::from(args.next().expect("output directory"));
    let mut filters = HashMap::new();
    while let Some(option) = args.next() {
        assert!(["--scene", "--theme", "--direction", "--scale"].contains(&option.as_str()));
        filters.insert(option, args.next().expect("filter value"));
    }
    std::fs::create_dir_all(&output).unwrap();
    gpui_platform::application().run(move|cx:&mut App|{
        ant_design_gpui::init(cx);
        let scope=ConfigScope::new_window(cx);
        scope.update_patch(cx,|patch|patch.motion=Some(false)).unwrap();
        cx.open_window(WindowOptions{window_bounds:Some(WindowBounds::Windowed(Bounds::centered(None,size(px(1000.),px(760.)),cx))),
            show:false,focus:false,..Default::default()},|window,cx|{
            let title=format!("Date/time picker hidden capture {}",std::process::id());window.set_window_title(&title);assert_background();
            let manifest:Value=serde_json::from_str(include_str!("../fixtures/date_time_picker/visual-cases.json")).unwrap();
            let names:Vec<_>=manifest["scenes"].as_object().unwrap().keys().filter(|name|filters.get("--scene").is_none_or(|f|f.split(',').any(|s|s==name.as_str()))).cloned().collect();
            assert!(!names.is_empty(),"unknown capture scene");
            let state=Capture::state(&manifest["scenes"][&names[0]],window,cx);
            let capture=cx.new(|_|Capture{scope:scope.clone(),manifest,scene:names[0].clone(),state,generation:0});
            let root=cx.new(|cx|UiRoot::new(scope.clone(),capture.clone(),window,cx));
            window.spawn(cx,async move|cx|{
                let mut cases=Vec::new();
                cx.background_executor().timer(Duration::from_millis(150)).await;
                for scene in names{
                    for(theme,algorithms)in[("default",AlgorithmChain::default()),("dark",AlgorithmChain::dark()),("compact",AlgorithmChain::compact()),("dark-compact",AlgorithmChain::dark_compact())]{
                        if filters.get("--theme").is_some_and(|f|!f.split(',').any(|v|v==theme)){continue;}
                        for(direction,direction_value)in[("ltr",Direction::Ltr),("rtl",Direction::Rtl)]{
                            if filters.get("--direction").is_some_and(|f|!f.split(',').any(|v|v==direction)){continue;}
                            for scale in [1_f32,1.5,2.]{
                                if filters.get("--scale").is_some_and(|f|!f.split(',').any(|v|v.parse::<f32>().unwrap()==scale)){continue;}
                                cx.update(|window,cx|{
                                    capture.update(cx,|v,cx|v.configure(&scene,window,cx));
                                    let sample=capture.read(cx).manifest["scenes"][&scene].clone();
                                    scope.update_patch(cx,|patch|{
                                        patch.direction=Some(direction_value);
                                        patch.locale.locale=Some(match sample["locale"].as_str(){Some("zh_CN")=>Locale::zh_cn(),Some("zh_TW")=>Locale::zh_tw(),_=>Locale::en_us()});
                                        let theme=patch.theme.get_or_insert_default();theme.algorithms=Some(algorithms.clone());
                                        theme.components.set_theme::<DatePickerTheme>(if sample["tokenCase"]==true{
                                            ComponentThemeConfig::new(DatePickerTokenPatch{cell_width:Some(40.),cell_height:Some(28.),
                                                cell_active_with_range_bg:Some(Color::rgb(0xefdbff)),time_column_width:Some(64.),time_cell_height:Some(32.),..Default::default()})
                                                .with_tokens(TokenPatch{color_primary:Some(Color::rgb(0x722ed1)),..Default::default()})
                                        }else{ComponentThemeConfig::new(DatePickerTokenPatch::default())});
                                    }).unwrap();
                                }).unwrap();
                                set_native_scale(&title,scale,(1000.,760.));
                                cx.update(|window,cx|{
                                    assert_background();window.bounds_changed(cx);window.refresh();
                                    #[cfg(not(target_os="windows"))]window.set_scale_factor(scale);
                                    for _ in 0..3{window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent{position:point(px(0.),px(0.)),..Default::default()}),cx);window.draw(cx).clear(cx);}
                                    assert_background();
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(150)).await;
                                cx.update(|window,cx|{
                                    window.bounds_changed(cx);assert_background();
                                    window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent{position:point(px(0.),px(0.)),..Default::default()}),cx);window.draw(cx).clear(cx);
                                    let sample=capture.read(cx).manifest["scenes"][&scene].clone();
                                    if let Some(index)=sample["focusInput"].as_u64(){
                                        capture.update(cx,|view,cx|view.state.focus_input(index as usize,window,cx));window.draw(cx).clear(cx);
                                    }
                                    let g=capture.read(cx).state.geometry(cx);
                                    let hover=if sample["hoverTrigger"]==true{g.trigger}else{sample["hoverDate"].as_str().and_then(|target|g.cells.values().find(|c|c.in_view&&c.date.is_some_and(|d|d.to_string()==target)).map(|c|c.bounds))};
                                    let pointer=hover.map_or(point(px(0.),px(0.)),|b|b.center());
                                    for _ in 0..2{window.dispatch_event(PlatformInput::MouseMove(MouseMoveEvent{position:pointer,..Default::default()}),cx);window.draw(cx).clear(cx);}
                                }).unwrap();
                                cx.background_executor().timer(Duration::from_millis(40)).await;
                                cx.update(|window,cx|{
                                    assert_background();window.draw(cx).clear(cx);
                                    assert!((window.scale_factor()-scale).abs()<0.001,"native scale mismatch");
                                    let name=format!("{scene}-{theme}-{direction}-{}",(scale*100.)as u32);
                                    let image=window.render_to_image().expect("native picker GPU frame");
                                    assert_eq!((image.width(),image.height()),((1000.*scale)as u32,(760.*scale)as u32));
                                    image.save(output.join(format!("{name}.png"))).unwrap();
                                    let foreground=assert_background();let view=capture.read(cx);
                                    let metric=json!({"name":name,"scene":scene,"theme":theme,"direction":direction,"scale":scale,
                                        "clock":"2024-02-15T13:24:35.120+08:00","timezone":"Asia/Shanghai","canvas":{"width":1000,"height":760,"padding":32},
                                        "options":view.manifest["scenes"][&scene],"geometry":metrics(view,window,cx),"foreground_pid":foreground,"capture_pid":std::process::id()});
                                    // Publish only after the complete PNG exists. These immutable,
                                    // unattested progress records let diagnostics inspect early scenes
                                    // without confusing an interrupted run with a completed capture.
                                    let progress=output.join(format!("{name}.metrics.json"));
                                    let temporary=output.join(format!("{name}.metrics.tmp"));
                                    std::fs::write(&temporary,serde_json::to_vec(&metric).unwrap()).unwrap();
                                    std::fs::rename(temporary,progress).unwrap();
                                    cases.push(metric);
                                }).unwrap();
                            }
                            println!("Captured native Date/time picker {scene}/{theme}/{direction}");
                        }
                    }
                }
                assert!(!cases.is_empty(),"no capture cases");
                std::fs::write(output.join("native-metrics.json"),serde_json::to_vec_pretty(&json!({"schema_version":1,"cases":cases})).unwrap()).unwrap();
                std::fs::write(output.join("capture-context.json"),serde_json::to_vec_pretty(&json!({"process_id":std::process::id(),
                    "foreground_checks":FOREGROUND_CHECKS.load(Ordering::Relaxed),"foreground_violations":0,"cases":cases.len(),
                    "resize":"owned HWND SetWindowPos SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE","pointer_policy":"neutral local synthetic input after bounds polling; explicit scene focus and hover"})).unwrap()).unwrap();
                cx.update(|_,cx|cx.quit()).unwrap();
            }).detach();root
        }).expect("hidden native picker capture window");
    });
}
