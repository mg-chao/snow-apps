// Copyright (C) 2026 mg-chao
// SPDX-License-Identifier: Apache-2.0
//! Read-only Windows UIA fixture. It never requests OS focus or sends input.

use ant_design_gpui::{
    ConfigScope, UiRoot,
    date_picker::{
        DatePicker, DatePickerState, MultipleDatePicker, MultipleDatePickerState, NaiveDate,
        NaiveDateTime, PickerRange, RangePicker, RangePickerState, TimePicker, TimePickerState,
    },
    locale::Locale,
};
use chrono::Datelike;
use gpui::{
    App, AppContext, Bounds, Context, Entity, IntoElement, Render, Subscription, Window,
    WindowBounds, WindowOptions, div, point, prelude::*, px, size,
};
use serde_json::json;
use std::{path::PathBuf, time::Duration};

fn date(day: u32) -> NaiveDate {
    NaiveDate::from_ymd_opt(2024, 2, day).unwrap()
}
fn clock() -> NaiveDateTime {
    date(15).and_hms_milli_opt(13, 24, 35, 120).unwrap()
}
struct Fixture {
    scope: ConfigScope,
    component: String,
    date: Entity<DatePickerState>,
    time: Entity<TimePickerState>,
    disabled: Entity<DatePickerState>,
    read_only: Entity<DatePickerState>,
    range: Entity<RangePickerState>,
    multiple: Entity<MultipleDatePickerState>,
    ever_native_active: bool,
    _activation: Subscription,
}
impl Render for Fixture {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .p(px(32.))
            .flex()
            .flex_col()
            .items_start()
            .gap(px(24.))
            .child(
                DatePicker::new("date", &self.date)
                    .scope(self.scope.clone())
                    .open(self.component == "date")
                    .value(Some(date(12)))
                    .clock(clock)
                    .disabled_date(|date, _| date.weekday().number_from_monday() >= 6)
                    .accessibility_label("Choose date")
                    .automation_id("picker.date"),
            )
            .child(
                TimePicker::new("time", &self.time)
                    .scope(self.scope.clone())
                    .open(self.component == "time")
                    .value(Some(clock().time()))
                    .clock(clock)
                    .accessibility_label("Choose time")
                    .automation_id("picker.time"),
            )
            .child(
                DatePicker::new("disabled", &self.disabled)
                    .scope(self.scope.clone())
                    .disabled(true)
                    .value(Some(date(12)))
                    .clock(clock)
                    .accessibility_label("Disabled date")
                    .automation_id("picker.disabled"),
            )
            .child(
                DatePicker::new("read-only", &self.read_only)
                    .scope(self.scope.clone())
                    .input_read_only(true)
                    .value(Some(date(12)))
                    .clock(clock)
                    .accessibility_label("Read-only date")
                    .automation_id("picker.read-only"),
            )
            .child(
                RangePicker::new("range", &self.range)
                    .scope(self.scope.clone())
                    .value(PickerRange::new(Some(date(8)), Some(date(21))))
                    .clock(clock)
                    .accessibility_label("Travel dates")
                    .automation_id("picker.range"),
            )
            .child(
                MultipleDatePicker::new("multiple", &self.multiple)
                    .scope(self.scope.clone())
                    .value(std::sync::Arc::from([date(8), date(12), date(21)]))
                    .clock(clock)
                    .accessibility_label("Selected travel dates")
                    .automation_id("picker.multiple"),
            )
    }
}
fn main() {
    let mut args = std::env::args().skip(1);
    let output = PathBuf::from(args.next().expect("state output path"));
    let component = args.next().unwrap_or_else(|| "date".to_owned());
    assert!(["date", "time"].contains(&component.as_str()));
    let locale = args.next().unwrap_or_else(|| "en_US".to_owned());
    assert!(["en_US", "zh_CN", "zh_TW"].contains(&locale.as_str()));
    gpui_platform::application().run(move|cx:&mut App|{
        ant_design_gpui::init(cx);
        cx.on_window_closed(|cx,_|{if cx.windows().is_empty(){cx.quit();}}).detach();
        cx.open_window(WindowOptions{window_bounds:Some(WindowBounds::Windowed(Bounds::new(point(px(0.),px(0.)),size(px(1000.),px(760.))))),show:false,focus:false,..Default::default()},move|window,cx|{
            window.set_window_title("Date/time picker accessibility fixture");
            let scope=ConfigScope::new_window(cx);
            scope.update_patch(cx,|patch|{patch.motion=Some(false);patch.locale.locale=Some(match locale.as_str(){"zh_CN"=>Locale::zh_cn(),"zh_TW"=>Locale::zh_tw(),_=>Locale::en_us()});}).unwrap();
            let date=cx.new(|cx|DatePickerState::new(window,cx));let time=cx.new(|cx|TimePickerState::new(window,cx));
            let disabled=cx.new(|cx|DatePickerState::new(window,cx));let read_only=cx.new(|cx|DatePickerState::new(window,cx));let range=cx.new(|cx|RangePickerState::new(window,cx));
            let multiple=cx.new(|cx|MultipleDatePickerState::new(window,cx));
            let fixture=cx.new(|cx|Fixture{scope:scope.clone(),component,date,time,disabled,read_only,range,multiple,
                ever_native_active:window.is_window_active(),
                _activation:cx.observe_window_activation(window,|this:&mut Fixture,window,cx|{if window.is_window_active(){this.ever_native_active=true;cx.notify();}})});
            let observation=fixture.downgrade();
            window.spawn(cx,async move|cx|{
                for _ in 0..900{
                    cx.background_executor().timer(Duration::from_millis(100)).await;
                    if cx.update(|window,cx|{
                        window.draw(cx).clear(cx);let Some(fixture)=observation.upgrade()else{return;};let f=fixture.read(cx);
                        let value=json!({"process_id":std::process::id(),"component":f.component,"locale":locale,
                            "clock":"2024-02-15T13:24:35.120+08:00","ever_native_active":f.ever_native_active,
                            "date_open":f.date.read(cx).is_open(),"time_open":f.time.read(cx).is_open(),
                            "tree":window.debug_a11y_tree_json().and_then(|s|serde_json::from_str::<serde_json::Value>(&s).ok())});
                        let temporary=output.with_extension("tmp");
                        if let Err(error)=std::fs::write(&temporary,serde_json::to_vec_pretty(&value).unwrap()).and_then(|_|std::fs::rename(temporary,&output)){
                            eprintln!("Date/time accessibility snapshot publication: {error}");
                        }
                    }).is_err(){break;}
                }
                cx.update(|_,cx|cx.quit()).ok();
            }).detach();
            cx.new(|cx|UiRoot::new(scope,fixture,window,cx))
        }).unwrap();
    });
}
