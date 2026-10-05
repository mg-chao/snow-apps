# Pinned-window multi-selection native validation

Use disposable pins and groups for these checks. Record the commit, edition,
OS, display arrangement and scale factors, input device, theme, language,
window count, source sizes, and any skipped native cases. Run Full and Mini on
macOS where both editions are available.

## Focused automated checks

Build only the affected targets. Unit checks do not qualify native input,
WindowServer composition, Spaces, or mixed-display behavior.

```sh
cmake --build --preset build-snow-shot-macos-arm64-debug \
  --target snow-shot-pinned-window-tests snow-shot-pinned-selection-geometry-tests \
    snow-shot-macos-pinned-window-tests
ctest --preset test-snow-shot-macos-arm64-debug \
  -R '^snow-shot-pinned-(multi-selection|selection-geometry|controlled-interaction|retina-interaction|pointer-routing|lock|group-menu)-tests$' \
  --output-on-failure
ctest --test-dir build/snow-shot-macos-arm64-debug \
  -R '^snow-shot-macos-pinned-(window|drag|multi-selection(-geometry)?|pixel-alignment)-tests$' \
  --output-on-failure
```

Native input cases may return skip code 77 when event posting is unavailable
or the desktop is locked.
Record the skip; do not treat it as a native pass or change privacy permissions
as part of an automated run. Repeat the equivalent focused Windows tests on a
Windows host; a macOS run cannot validate Windows hit testing, mouse capture,
or DPI messages.

The native multi-selection fixture exercises real Command clicks, shared
dragging, and ordinary-click clearing of the indicators. Indicator-click
deselection is covered by the deterministic fixture. Run native input separately from other native
pointer fixtures:

```sh
build/snow-shot-macos-arm64-debug/snow_shot/test-bin/snow-shot-pinned-window-tests \
  --multi-selection-native-only -platform cocoa
```

For native geometry qualification without physical mouse injection or modal
menus, use `--multi-selection-geometry-only -platform cocoa`. It exercises
shared geometry, proportional resizing, scale limits, and rollback with Qt
events/controller calls while retaining the Cocoa window platform.

## Native interaction matrix

Run each ordinary image-pin case with 2, 10, and 50 windows. Include odd window
sizes, different zoom percentages, overlaps, rotated images, and at least one
pin near the minimum and maximum zoom. Use a Retina display, a second display
with another scale factor when available, and a display left of or above the
primary display to exercise negative desktop coordinates.

| Area | Native steps and expected behavior |
| --- | --- |
| Select and deselect | Command-click each normal pin on macOS, or Ctrl-click on Windows. Selection changes on release. Each selected pin shows one top-left indicator. Modifier-click and indicator-click deselect only that pin. A plain click on any pin clears selection. |
| Eligibility | Thumbnails, hidden/minimized pins, every Hide-to-Top state, click-through pins, and inactive-group pins cannot join selection. Entering an excluded mode removes an existing member. Showing, restoring, or reusing its pooled window does not restore selection. Locked normal pins remain selectable. |
| Gesture intent | Small movement below the drag threshold still counts as a click. Above the threshold, release never toggles selection. A modifier-drag with fewer than two selected source pins retains existing file export. With multiple selected pins, a modifier-drag on an eligible window-move surface moves the selection and does not export files. |
| Drawing and OCR | Drawing-tool drags and OCR text-selection/editing drags retain their existing behavior. Dragging an eligible OCR background can move the selected windows. Annotation histories, OCR text and selection geometry, original-image previews, and toolbar positions remain correct after moving, scaling, cancellation, and release. |
| Shared move | Drag any selected pin across each display boundary in both directions, then toward the menu bar and Dock/taskbar. Other selected origins receive the same desktop-logical delta; unselected pins stay put. There is no final jump when the button is released. |
| Shared resize | Exercise all eight edge/corner handles. Every member's zoom percentage receives one common relative factor and keeps its own top-left origin. Sizes follow its original aspect ratio with integer pixel rounding. Shrinking past a handle's origin stops at the common minimum without moving origins or mirroring content. Members at different original zoom levels remain within their individual 10–500% ranges. |
| Wheel and pinch | Test mouse-wheel notches, partial angle deltas, phased trackpad scrolling, direction reversals, pinch, and momentum immediately after pinch. Group zoom uses top-left anchors despite the saved single-pin anchor preference. Ctrl/Command-wheel opacity remains per-window. Ordinary OCR text mode blocks wheel/pinch scaling for the complete selection; original-image translation mode retains its existing scaling capability. |
| Interruptions | During moving/resizing press Escape, switch applications, lose mouse capture, hide/close a member, change its mode, or disconnect a display. The group cancels without a stale drag, delayed selection toggle, or unrelated later release. Surviving members roll back together. |
| Lock | Select a mixture of locked and unlocked pins. Drag, resize, wheel/pinch zoom, and alignment move none of them. The menu's Lock action locks all when any are unlocked, and unlocks all when all are locked. |
| Alignment | Check all eight operations against the selected union bounds. Horizontal/vertical distribution needs at least three members. Verify unequal sizes, overlaps, equal centers, and negative coordinates. Sizes and the other axis stay unchanged; indicators remain visible. |
| Menu routing | Right-click a selected member with at least two selected pins. Confirm the disabled selected-count prompt, separators, Close Other Windows, Align Position, Group, Lock, Close, and destructive Destroy styling. Right-click an unselected pin clears selection and opens its ordinary menu. Drawing/OCR surfaces use the multi-selection menu when applicable. |
| Batch lifecycle | Group assignment and New Group affect all selected members together. Existing group-deletion entries retain their existing group-wide meaning. Close Other Windows preserves all selected pins and closes other live pins in the active group. Close preserves restore records; Destroy shows one count-aware confirmation and removes all confirmed records. Canceling confirmation changes nothing. |
| Per-window actions | Keyboard shortcuts, toolbar actions, automation, image transforms, and scale menu presets continue to affect their individual target. Selection alone does not turn them into batch commands. |
| Appearance and access | Check light/dark themes and English, Simplified Chinese, and Traditional Chinese. Indicators remain legible on bright/dark/transparent content, at small pin sizes, and at Retina scale. Check pointer cursor transitions, tooltip/accessibility text, focus, keyboard activation of the indicator, menu placement, and live language changes. |

Repeat grow/shrink and drag/cancel sequences while checking that odd sizes,
zoom readouts, image orientation, and restore positions do not drift. Close and
restore the fixtures: geometry and normal window state persist; selection does
not.

Include a very small pin whose extent rounds to one pixel at several zoom
levels. Returning to its starting zoom restores its original aspect and size;
repeated zoom steps must not compound pixel rounding into a larger image.

## Performance observations

Measure geometry-helper costs only with a Release performance preset:

```sh
cmake --build --preset build-snow-shot-macos-arm64-performance \
  --target snow-shot-pinned-selection-performance-benchmark
build/snow-shot-macos-arm64-performance/snow_shot/snow-shot-pinned-selection-performance-benchmark \
  --samples 21 --iterations 1000
```

The benchmark reports p50/p95 for 2, 10, and 50 rectangles. Its
`geometry-helper` measurement excludes native window updates, painting, OCR,
input delivery, and persistence; do not present it as end-to-end drag latency.
It also counts successful scalar/array/aligned C++ `operator new` calls and
their requested bytes on the sampling thread in a separate untimed pass.
Direct `malloc` and Qt container allocations are excluded; zero C++ calls do
not mean zero heap allocations.

The real controller fixture also measures 120 updates per 2/10/50-window case,
counts move/resize/paint events, and verifies that every pin saves only its
final state. Run it on Cocoa to include native geometry and painting, or on
offscreen for a deterministic comparison without WindowServer qualification:

```sh
cmake --build --preset build-snow-shot-macos-arm64-performance \
  --target snow-shot-pinned-window-tests
build/snow-shot-macos-arm64-performance/snow_shot/test-bin/snow-shot-pinned-window-tests \
  --multi-selection-performance-only -platform cocoa
```

The fixture drives the controller directly; it does not measure physical mouse
delivery or input-to-photon latency. Use the native interaction matrix for
those remaining checks.

For native 2/10/50-window sessions, record input-to-frame p50/p95, visible lag or
catch-up on release, redraw frequency, and memory before and after repeated
gestures. Use small pins for the 50-window case and separately test fewer large
Retina pins. Confirm that updates grow with the selected count, motion events
are coalesced without losing the release position, persistence is deferred
during the gesture, and source pixels, OCR jobs, and annotation documents are
not rebuilt merely because native geometry changes. Record hardware-sensitive
results with their setup instead of comparing unrelated machines.

## Qualification record

- 2026-10-05, macOS 27.0.1 arm64, working tree based on `850d27c1`:
  all 15 affected Debug unit checks passed, including multi-selection routing,
  menu/lifecycle behavior, live language/theme changes, geometry, batch group
  assignment, repository persistence, and the existing affected regressions.
  The final manager-teardown and native-platform fixture changes were rechecked
  with five focused unit tests, all passing. Full and Mini Debug applications
  and the Release performance test target built successfully.
  Test log: `build/snow-shot-macos-arm64-debug/pinned-multi-selection-final-tests.log`.
- Cocoa window-policy, pixel-alignment, and focused multi-selection geometry
  tests passed.
  This qualifies the native geometry backend with controller/Qt input, not
  physical mouse delivery, mixed-display movement, or input-to-photon latency.
- Physical Command-click/shared-drag qualification returned skip code 77
  because this validation desktop was locked. Repeat on an unlocked desktop;
  the native interaction matrix remains pending.
- Windows native qualification remains pending on a Windows host. The native
  fixture includes actual Ctrl-click and shared-drag input via `SendInput`.
- Release helper and controller benchmarks are recorded separately from the
  hardware interaction checks. Controller benchmarks include real 2/10/50-pin
  drag and repeated 100–110% shared zoom, deferred persistence, source/document
  identity checks, and resident-memory deltas; they do not measure physical
  input latency or allocations outside the explicitly reported C++ counters.

Release controller samples used 160×96 source pixels per pin and 120 updates
per stage, alternating 100% and 110% zoom. The results below were collected on
the same macOS arm64 host; values are p95 update time in milliseconds.

| Pins | Offscreen drag | Offscreen zoom | Cocoa drag | Cocoa zoom |
| --- | ---: | ---: | ---: | ---: |
| 2 | 0.017 | 0.555 | 0.508 | 3.249 |
| 10 | 0.062 | 3.167 | 1.137 | 12.410 |
| 50 | 0.401 | 13.139 | 4.706 | 76.506 |

All stages preserved source/document identity and deferred serialization until
completion, committing one state snapshot per pin. The 50-pin Cocoa zoom
sample shows a material native resizing/painting limit; it does not qualify
smooth 60 Hz zoom at that window count. Its resident-memory increase was about
50 MiB during the sampled burst. This observation includes native backing
surfaces and event-loop cleanup; it does not establish an allocation count or
memory leak. Physical input and final WindowServer composition remain pending
on an unlocked desktop.

Controller logs:
`build/snow-shot-macos-arm64-performance/pinned-multi-selection-window-benchmark.log`
and
`build/snow-shot-macos-arm64-performance/pinned-multi-selection-cocoa-benchmark.log`.
