# Date/time picker verification

Reference: Ant Design 6.6.7, revision
`f8f5595ab5e891fff6a2a06c5b3b98af643d008e`, rc-picker 1.12.2.
The independent fixture executes the original React source, with the clock
fixed at 2024-02-15 13:24:35.120 Asia/Shanghai.

## Evidence status

The independent browser capture completed **1,344 of 1,344 scenes** on
2026-10-30 at 05:00:54 UTC. All today marks point to the fixed 2024-02-15 date.
The packed offline baseline contains all **448 scenes at 100% scale**. The
comparator's **69 focused controls passed**. A focused 72-case week-row
recapture adds actual row-selection/range flags and painted-band checks while
preserving the complete original and supplemental provenance records.
An additional **360-case reference recapture** removes Chromium's inherited
scrollbar-suppression flag and records actual Windows thin scrollbar paint,
client clipping, scroll extents and OK-button geometry. The original and all
supplemental captures remain preserved with source and image hashes. A further
120-case tag recapture records actual visible tag/content/remove geometry and
the hidden HTML focus-helper metadata. An isolated 24-case milliseconds-step-1
probe checks the actual 1,000-row browser scrollbar, including its 11px minimum
thumb; it remains separate from the official 56-scene manifest.
A 264-case range recapture additionally records source arrow geometry and
checks actual arrow fill above the popup's content clip edge.
All 672 RTL configurations were recaptured with the component's actual default
placement after removing a forced placement from the developer fixture. Outer
dropdown placement/style and inner panel geometry are recorded independently.
Another 24-case recapture records the prefix slot and its text range.
The final independent native/browser comparison passes **1,344 of 1,344
configurations with zero failed checks**: all 56 scenes, four themes, both
directions and 100/150/200% scales. Four native shards used the same frozen
source and executable; their merge preserves every attestation, metric and PNG
hash, plus 9,412 foreground ownership checks with zero violations.
Native Windows UIA read-only inspection passed **126 checks across six profiles**
(date/time in en_US/zh_CN/zh_TW), with no native activation or foreground change.
The rebuilt native UIA source/executable hashes and per-profile results are
preserved in [accessibility-windows.json](../fixtures/date_time_picker/accessibility-windows.json).
The quiet, attested Release run passes all **17 benchmark paths**, including
retained identity, primitive limits and zero settled idle frame requests.
Compact evidence is preserved in
[visual-validation-windows.json](../fixtures/date_time_picker/visual-validation-windows.json),
[accessibility-windows.json](../fixtures/date_time_picker/accessibility-windows.json)
and [performance-windows.json](../fixtures/date_time_picker/performance-windows.json).
All three share the same 2,001-file source-map digest
`a42d34b678817ae5e94bf406fa59d1731ac8bbef50ddb03000bc564813f0ce40`.
Visual evidence completed at 11:46:39 UTC and performance at 11:52:19 UTC on
2026-10-30. Geometry/solid-paint parity is established within the declared
limits; browser and GPUI text/shadow rasterization is not byte-identical.
The generated [per-scene matrix](../fixtures/date_time_picker/coverage-matrix.json)
is the source of truth for reference/native/strict-comparison completeness.

## Coverage matrix

Every visual row below runs in default/dark/compact/dark-compact themes,
LTR/RTL and 100/150/200% scales. Individual scene IDs, counts and current status
are explicit in the linked JSON matrix.

| Feature | Reference scenes | Native behavior/visual evidence |
| --- | --- | --- |
| Date selection and calendar | empty, selected, today, hover, focus | Passed |
| Date constraints | weekends, min/max, disabled trigger, read-only | Passed |
| Date periods | week, month, quarter, year, decade panel | Passed |
| Multiple values | date, week, month, quarter, year | Passed |
| Date range | complete, partial hover, disabled cells, partially disabled inputs | Passed |
| Period ranges | week, month, quarter, year | Passed |
| Date plus time | single, range, disabled time | Passed |
| Time | empty, selected, 12-hour, hour/minute, milliseconds | Passed |
| Time configuration | steps, disabled values, hidden disabled values, scroll commit | Passed |
| Time range | selected start/end | Passed |
| Trigger states | small/normal/large, clear hover, prefix, error/warning | Passed |
| Variants | outlined, filled, borderless, underlined | Passed |
| Customization | presets, extra footer, custom date cells, date/time tokens | Passed |
| Locale | English, Simplified Chinese, Traditional Chinese | Passed |

Functional tests must separately establish typed parsing/formatting, leap and
century boundaries, week numbering across years, disabled callbacks, allowed
partial ranges, range ordering, draft/committed values, confirmation, clear,
controlled acceptance/rejection, live prop updates, keyboard traversal, Escape,
outside press, focus restoration, IME-safe retained editing and locale updates.
A static screenshot is not evidence for those interactions.

## Visual acceptance

Use the commands and metric contract in the
[fixture guide](../fixtures/date_time_picker/README.md). Keep independent browser
and native measurements; do not derive expected geometry from native code.
Logical geometry allows 1px, flat colors/marks 1 RGB byte. Actual selected
fills, range bands and today outlines are sampled independently. Report residual
failures with exact scenes and magnitudes; capture completion does not mean
the strict comparison passed. Record full-image text/shadow raster error
separately and retain unmasked PNGs.
Tag fill is measured against the adjacent gap on the same scanline because
popup shadows overlap the trigger; absolute chip/gap colors remain recorded.
Chip labels, content/removal boxes and actual remove silhouettes remain strict.
Custom cell borders additionally compare source computed border boxes/colors
against the last matching painted native Quad at each side, including every
side's width and clip. Rounded-border fragments may have different draw orders;
their visible straight sides are verified at twelve independent points.
Today outlines independently locate the declared full border color
within one physical pixel of each renderer's top-center probe; the color limit
remains one byte and authored boxes retain their one logical pixel limit.
Popup fill probes occupy the clear top band, avoiding time scrollbar rails.

Native capture must use an owned hidden HWND, no-activate resizing, local
synthetic pointer events and explicit foreground ownership assertions. Windows
UIA inspection should default to read-only background checks. UIA Invoke can
activate its target window; foreground actuation belongs to an explicitly
interactive test profile.

Native visual/UIA evidence in this task is Windows-only. Unmasked text and
shadow statistics remain part of the report; a strict geometry/solid-paint pass
does not assert byte-identical browser and GPUI rasterization. Scrollbar ink
extraction excludes the outer 1px of the rail, which can include a neighboring
separator or selected-fill fringe at fractional scales; rail/client geometry
and all unmasked pixels remain recorded. Silhouette checks use source stroke
coverage of at least 50% against the other renderer's complete painted ink,
symmetrically, with the same 1 logical pixel limit. Distance runs from a strong
pixel's center to the other image's occupied pixel square, accounting for half
a physical pixel of sampling uncertainty on each axis. Both images must retain
a strong core. Authored slot/rail geometry remains a separate strict check;
AA threshold-derived extent boxes and raw width/edge differences are diagnostics.
For example, a compact 150% quarter chip had a source core center at
(240.5,64.5), contrast 55 against a 54.5 threshold, and its nearest native
painted center at (242.5,64.5): center distance is 1.333 logical pixels, while
distance to the occupied pixel square is exactly 1. Its raw AA extent differs
by 1.333px even though its authored SVG slot differs by only 0.594px horizontally
and -0.333px vertically. These residuals remain in the reports.
Complete antialias-fringe residuals are reported independently. Tiny remove
glyphs require achromatic ink, a visible strong core, and the exact source
computed RGBA tint within one byte of the actual native monochrome sprite's
RGBA. The sprite is matched after its scene transformation, with exactly one
candidate required inside the authored slot. Gray raster brightness remains a
coverage diagnostic. Fractional-phase positives and missing, displaced, hollow,
wrong-color, wrong-hue and faded-opacity negatives verify these distinctions.

The final unmasked raster residuals remain explicit:

| Region | Mean channel error (0–255) | Pixels with any channel error >16 |
| --- | ---: | ---: |
| Full canvas | 0.438 | 0.417% |
| Component plus shadow | 2.510 | 2.492% |
| Cell/header/footer/input viewports | 4.306 | 5.073% |
| Popup shadow | 3.239 | 0.881% |

Maximum unmasked channel error is 233. The complete AA fringe has 32 source
pixels outside the one-logical-pixel neighborhood and zero native pixels;
every strong-core contour passes. The largest raw AA extent edge/width residual
is 1.333 logical pixels. Full metrics and unmasked PNGs remain under
`../build/ant-design-gpui/date-time-picker-reference/`; the checked-in evidence
records all aggregate statistics, representative residuals and artifact hashes.

The pinned AccessKit adapter has a scrollbar limitation: in
`accesskit_consumer-0.38.0/src/node.rs`, `is_read_only_supported` (line844)
omits `Role::ScrollBar`; `is_read_only` (line452) therefore returns true.
`accesskit_windows-0.34.0/src/node.rs` exposes that result through native
RangeValue.IsReadOnly. The consumer source SHA-256 is
`99e7ebd71e596c68b822eaab9443f93d533092f7953d79e859149ae5260ad841`.
The fixture preserves the truthful ScrollBar role and records this flag.
It checks actual native numeric bounds/offsets and registered SetValue and
SetScrollOffset handlers; UIA write actuation is not claimed. Pointer and
keyboard scrolling have separate deterministic behavior coverage.

## Focused tests and performance

Run component-filtered library and gallery tests, plus only related shared
Input/Popup regression filters when those internals change. Never run the full
suite for this work. Format and compile/lint the affected targets.

Final frozen-source checks pass: 122 picker tests, 78 Popup tests, 25 Config
tests, 31 ScrollArea tests, three related gallery tests under a 1MiB test-thread
stack, and 69 comparator controls. Formatting/diff checks and Clippy with
warnings denied pass for the affected library, tests, visual/accessibility
fixtures, gallery and benchmark targets. No full repository test suite ran.

Benchmarks must use `windows-msvc-performance` and Cargo Release. Measure date
arithmetic/parse/format, closed/open redraw, period navigation, range preview and
time-column scrolling. Assert stable retained input identities, bounded cells,
and zero idle animation-frame requests. Preserve source/toolchain/executable
hashes with CPU timing distributions and allocation/byte counts. These
measurements do not establish production font-shaping performance, GPU presentation time or physical input
latency: the benchmark uses offscreen TestAppContext and its deterministic test
text system. The native visual captures separately use the actual Windows
platform window, fonts and GPU.

The 17 benchmark paths cover compiled parse/format, checked leap/month-end
navigation, bounded calendar-grid construction, closed/open date, active-window
motion-enabled settled idle, actual popup open/close cycles, real pointer
navigation, range redraw/preview,
date-time redraw, time redraw/real wheel scrolling, 1,000-row millisecond redraw
and selection synchronization, and 100 mounted closed controls. Motion-enabled
setup explicitly activates the offscreen TestWindow, asserts it is visible and
actually schedules transition frames, then settles before measuring idle.
Structural budgets are 1,200 paint primitives for a single open picker and
6,400 for 100 closed controls, stable retained editor IDs and zero idle frame
requests. Timing percentiles and mean allocation/byte counts are machine-specific
diagnostics;
there is no numeric latency SLA or post-hoc timing threshold.

The final quiet run on Windows x64 (Ryzen 9 5950X, Rust 1.98.1) records:

| CPU path | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
| Closed date redraw | 0.199ms | 0.785ms | 5.321ms |
| Open date redraw | 2.452ms | 4.120ms | 7.442ms |
| Open time redraw | 1.307ms | 2.024ms | 2.544ms |
| 1,000-row millisecond redraw | 1.994ms | 3.278ms | 3.799ms |
| 100 mounted closed controls | 18.758ms | 25.336ms | 27.559ms |

All 17 CPU timing distributions and mean allocation/byte counts are in the linked
performance evidence.
Motion actually scheduled 14 opening callbacks in an active, visible test
window with reduced motion disabled, then zero callbacks after settling.
