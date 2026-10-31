# Date and time pickers

The DatePicker/TimePicker family follows Ant Design 6.6.7 using native GPUI
editors, calendar cells, scrolling time columns, and the shared popup host. Keep
one state entity per mounted picker and rebuild its lightweight builder during rendering:

```rust,ignore
use ant_design_gpui::date_picker::{DatePicker, DatePickerState, NaiveDate};

let state = cx.new(|cx| DatePickerState::new(window, cx));

// In render:
DatePicker::new("delivery-date", &state)
    .scope(scope.clone())
    .default_value(NaiveDate::from_ymd_opt(2026, 10, 30))
    .on_change(|change, _, _| {
        println!("Selected date: {:?}", change.value);
    });
```

Mount the containing view in `UiRoot` to install shared popup hosting, typography,
and keyboard traversal. Use the same `ConfigScope` for the containing view and
its pickers.

## Typed values and ownership

Each facade has its matching state alias. Values use chrono civil types; opening
a picker never converts a value to another timezone.

| Component | Value | State |
| --- | --- | --- |
| `DatePicker` | `Option<NaiveDate>` | `DatePickerState` |
| `DateTimePicker` | `Option<NaiveDateTime>` | `DateTimePickerState` |
| `MultipleDatePicker` | `Arc<[NaiveDate]>` | `MultipleDatePickerState` |
| `RangePicker` | `PickerRange<NaiveDate>` | `RangePickerState` |
| `DateTimeRangePicker` | `PickerRange<NaiveDateTime>` | `DateTimeRangePickerState` |
| `TimePicker` | `Option<NaiveTime>` | `TimePickerState` |
| `TimeRangePicker` | `PickerRange<NaiveTime>` | `TimeRangePickerState` |

`PickerRange::new(start, end)` supports open endpoints. A `(start, end)` tuple
converts to a complete range. Cleared single values are `None`; cleared ranges
have two empty endpoints; cleared multiple selections are empty arrays.

`default_value` initializes once. `value` makes the accepted value controlled;
accept proposals in `on_change` by updating the parent value. The panel retains
a separate draft while the user chooses a range, edits time, or awaits
confirmation. Programmatic `set_value` updates the value without a change event.
`set_open` changes visibility without an open-change callback. Closing through
`set_open(false)` also flushes a valid user draft when confirmation is disabled.

Date pickers support `.picker_mode(PickerMode::Date | Week | Month | Quarter |
Year)`. The displayed panel has independent date/month/year/decade navigation.
`picker_value`, `default_picker_value`, and `panel_mode` control panel navigation
without changing the selected value. `open` controls popup visibility, while
`default_open` initializes visibility once.

## Selection and constraints

Ordinary dates accept a selected cell immediately. Time and date-time pickers
require confirmation by default. Override with `need_confirm`; Enter and OK
confirm the active endpoint, then advance to the other enabled range endpoint.
Escape discards the current draft. Closing a picker without confirmation accepts
valid drafts only when confirmation is disabled. Multiple selection keeps the
popup open so additional cells can be toggled.

Ranges order their endpoints by default. `order(false)` preserves their order.
`disabled_endpoints([start, end])` preserves disabled endpoint values;
`allow_empty([start, end])` permits open intervals. `disabled(true)` disables the
whole picker. `input_read_only` keeps popup selection available while preventing
typing. Invalid input is restored on blur unless `preserve_invalid_on_blur` is
enabled. `preview_value(false)` disables temporary hover text in the trigger.

`min_date` and `max_date` constrain both selection and panel navigation.
`disabled_date` receives the candidate date and `DisabledDateInfo`, including
the range's first selected date and the current panel granularity. A single
callback can therefore restrict a range relative to its first endpoint.

`TimeOptions` controls 12-hour display, visible columns, hour/minute/second/
millisecond intervals, hidden disabled options, and selection after scrolling.
Milliseconds are optional and default to a 100ms interval when enabled. Time
columns preserve their scroll handles between renders; `change_on_scroll`
selects the nearest enabled option after scrolling settles.

`disabled_time` returns cascading `DisabledTime` callbacks:

```rust,ignore
.disabled_time(|_date, _endpoint| {
    DisabledTime::new()
        .hours(|| (0..9).chain(18..24).collect())
        .minutes(|hour| if hour == 9 { vec![0, 15] } else { vec![] })
        .seconds(|_hour, _minute| vec![55, 56])
})
```

Intervals govern popup choices. Valid typed values between those intervals are
accepted unless the disabled callbacks reject them. When changing one column,
dependent columns align to enabled options. Set `TimeOptions.default_open_value`
to supply the initial time when the selection is empty.

## Formats and localization

Custom patterns use Rust chrono syntax. Day.js tokens such as `YYYY` are literal
text rather than translated format directives; use `%Y` for the year. Compile a
`PickerFormat` once, retain it in the containing view, and clone it into builders
during rendering:

```rust,ignore
.format(PickerFormat::new("%d/%m/%Y")?)
.format(PickerFormat::patterns(["%Y-%m-%d", "%d/%m/%Y"])?)
.format(PickerFormat::mask("%Y-%m-%d")?)
```

The first configured pattern controls display; parsing tries each pattern in
order. `PickerFormat::custom` supplies custom read-only display logic.
`format_render` customizes the builder's displayed value and makes the input
read-only. Replacing or removing its callback refreshes the retained editor
without changing the selected value. Numeric masks provide
segment selection, digit input, increment/decrement, and date repair. Formats
represent civil dates and times; timezone/offset formats are rejected.

Default formats are `YYYY-MM-DD` in appearance, `YYYY-MM` for months, `YYYY` for
years, `YYYY-Qn` for quarters, and zero-padded `HH:mm:ss` for times. The default
12-hour display is `hh:mm:ss AM/PM`. Default week labels use locale week-years
and ordinals, independently of explicit chrono `%G`/`%V` ISO formats.

`Locale::en_us()`, `Locale::zh_cn()`, and `Locale::zh_tw()` include complete
`PickerLocale` catalogs. English and Traditional Chinese start weeks on Sunday
and count the week containing January 1 as week 1. Simplified Chinese starts on
Monday and counts the week containing January 4 as week 1. For example,
December 31, 2023 displays `2024-1st`, `2023-52周`, and `2024-1週` respectively.
`LocalePatch::picker` overrides labels, month/weekday arrays, week rules, and
ordinals. Changing locale through a scope updates mounted pickers without
changing layout direction.

## Appearance and composition

Size, disabled state, direction, variants, theme, locale, and motion inherit
through `ConfigScope`. Inputs support outlined, filled, borderless, and
underlined variants, three sizes, and error/warning status. Use `date_picker`,
`range_picker`, and `time_picker` in `ConfigPatch` for independent provider
defaults, including semantic styles and dynamic `PickerStyleResolver` values.
Every range, including `TimeRangePicker`, inherits the range provider. Single
date and time pickers inherit their respective providers.

Popup arrows are hidden for singles and visible for ranges by default. Override
the provider's `arrow` or call `.arrow(false)` on an individual picker.
`.arrow(PopupArrow { visible: true, point_at_center: true })` points at the
trigger midpoint, or the active input midpoint for a range. Unset `placement`
defaults to `BottomLeft` in LTR and `BottomRight` in RTL, including live direction
changes. Explicit builder and provider placements retain their physical
alignment; the builder takes precedence. `PickerConfig::placement` is optional
so providers can inherit that direction-aware default. Popups use the shared
overflow and alignment behavior. Popup content remains scrollable
when the window constrains its height; arrows stay outside that scroll viewport.

`PickerStyles` exposes root, prefix, input, suffix, popup, container, content,
body, header, item, footer, presets, and time-column slots. `styles_fn` resolves
styles from the current picker interaction. Component theme overrides use
`DatePickerTheme` and `DatePickerTokenPatch`; calendar dimensions, cell colors,
time dimensions, input metrics, and preset dimensions derive from theme tokens.

`PickerPreset::new(key, label, value)` creates a keyed shortcut. `dynamic` resolves
its value at selection time. `extra_footer` inserts native footer content;
`cell_render` decorates a cell's default content while retaining its hit target
and semantics. `input_render` receives the retained native `Input` for each
endpoint so custom editor content keeps its editing state. `tag_render` receives
the selected date, disabled state, default tag, and a weak `remove` command; use
that command to build custom removable tags without retaining the picker.
`prefix` and `suffix_icon` accept native elements. `panel_render`
receives reusable panel parts for custom native composition. `DatePickerPanel`
and `TimePickerPanel` embed the panel directly without a popup.
`prev_icon`, `next_icon`, `super_prev_icon`, and `super_next_icon` accept retained
content factories such as `.prev_icon(|_, _| icon(IconName::LeftOutlined))`.
The native navigation buttons retain their labels, focus, disabled state, and
actions around that content.

## Keyboard, gallery, and focused checks

Enter opens or confirms; Escape cancels. Calendar navigation supports arrow keys,
Home/End, and Page Up/Down. Native text editors retain clipboard, text selection,
undo/redo, IME, and accessibility support. Focus moves within the picker and its
popup without treating internal focus changes as outside dismissal.

```powershell
cargo run --locked --offline --example gallery -- --component date-picker
cargo run --locked --offline --example gallery -- --component time-picker
cargo test --locked --offline --lib date_picker
cargo test --locked --offline --example gallery date_time_picker
```

Gallery pages support the usual `--dark`, `--compact`, `--rtl`,
`--reduced-motion`, and `--capture` options. Verification details are recorded
separately in `date-time-picker-verification.md`.
