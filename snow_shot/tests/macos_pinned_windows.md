# macOS pinned-window validation

The implementation is available, but full platform qualification remains open.
The automated results and outstanding hardware checks are recorded below.

Pinned windows use a display identity, display-local logical position, and
logical size. Image pixels remain separate: a 300×200 selection on a 2× display
pins at exactly 300×200 logical pixels while retaining its 600×400 raster. Moving
between displays preserves logical size, zoom, and the grabbed content point.
Toolbars, click-through controls, and hide-to-top handles also use logical geometry.
The Cocoa adapter owns AppKit observers and native policy restoration; Qt continues
to own each window and delegate.

Pins and associated palettes, popups, click-through controls, and hide-to-top
handles join all Spaces and full-screen auxiliary Spaces at the floating level.
Public AppKit policies determine availability over system surfaces. Ordinary
interaction and passive hover do not require a global input event tap.

Persistence format 2 uses `pinned_windows_v2` under the settings directory.
Placements explicitly store `geometry_units` and `window_size`; records store
`initial_window_size` separately from raster content. The unreleased schema is
updated directly; older development data is not migrated.

## Automated checks

Build only the affected targets with `scripts/build.sh PRESET --target TARGET`.
The primary targets are `snow-shot-pinned-window-tests`,
`snow-shot-pinned-window-repository-tests`, `snow-shot-pinned-window-group-tests`,
`snow-shot-pinned-placement-tests`, `snow-shot-physical-cursor-tests`, and
`snow-shot-macos-pinned-window-tests`. The geometry helper targets are
`snow-shot-pinned-resize-geometry-tests`,
`snow-shot-pinned-native-geometry-controller-tests`, and
`snow-shot-pinned-restore-geometry-tests`.

Run the pinned unit tests only:

```sh
ctest --preset test-snow-shot-macos-arm64-debug -R '^snow-shot-pinned-'
```

The placement test covers negative desktop origins, fractional points, repeated
mixed-scale transitions, menu bar/Dock recovery, oversized images, and geometry
transactions without a real display. The controlled interaction case exercises
all resize edges, cancellation, scroll accumulation, momentum suppression, pinch
magnification, and opacity. Synthetic display geometry does not qualify the
WindowServer compositor on mixed-display hardware.

Run Cocoa checks explicitly; presets exclude interactive tests:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug \
  -R '^snow-shot-macos-pinned-(window|focus|delivery|pixel-alignment)-tests$' \
  --output-on-failure
```

The native policy case checks frame readback, odd logical extents, input
transparency, Space flags, native-surface recreation, and observer cleanup.
The separate focus case checks explicit activation, editor focus, input-method
commits, and passive auxiliary controls; it skips on a locked desktop. The pixel
alignment case renders a checkerboard through the actual pinned canvas on Cocoa.
The delivery
case starts a separate receiver process to test complete cross-application
clicks and dismissal release ownership. It returns CTest skip code 77 when the
host lacks event-posting permission. It never prompts for permission or changes
privacy settings. Basic pinning does not require this test permission.

## Recorded validation — 2026-09-21

Host: macOS 27.0, arm64; Qt 6.11.1; deployment target macOS 15.0.

- The Debug application and affected test targets built with strict warnings.
- All 67 selected unit cases passed after the affected thumbnail export cases
  were corrected and rerun. Coverage includes screenshot export, placement,
  Retina interaction, rendered pixel alignment, imports, storage, groups, editing,
  OCR, thumbnails, and cursor movement. No full repository suite or benchmarks ran.
- All four Cocoa checks passed: native geometry/policies, focus/IME,
  cross-application click-through and dismissal, and rendered pixel alignment.
  Pixel alignment also verifies full-resolution viewport export and that backing
  notifications preserve oversized/cross-display placement.
- Formatting and `git diff --check` passed. Windows native regression checks,
  Intel/macOS 15 execution, and the hardware scenarios below remain unqualified.

## Recorded validation — 2026-09-18

Host: macOS 27.0 (26A428), arm64; Qt 6.11.1; deployment target macOS 15.0.
This does not establish runtime qualification on macOS 15 or Intel hardware.

- The arm64 Debug application and affected test targets built successfully with
  strict compiler warnings enabled.
- All **58 affected portable tests passed**, including the rebuilt hide-to-top
  cancellation, click-through translation, and resize-geometry cases. Selection:
  `^snow-shot-pinned-|^snow-shot-(auto-filter|image-conversion)-pinned-tests$|^snow-shot-physical-cursor-tests$`,
  excluding labels `windows|interactive|benchmark`.
- Both Cocoa policy/cleanup and native pixel-alignment checks passed.
- Cocoa focus/IME and delivery checks were **skipped**, not passed: the desktop
  was locked, and `CGPreflightPostEventAccess()` reported no posting permission.
- All **17 affected translation units passed x86_64 syntax compilation** against
  the universal Qt frameworks. A complete x64 application link was not run; the
  x64 vcpkg dependencies are not installed on this host.
- Formatting passed for all 27 touched C++/Objective-C++ files, and
  `git diff --check` passed. No benchmark or full repository test suite was run.

Windows native regression testing and the manual scenarios below remain
unqualified. In particular, synthetic mixed-scale tests and public AppKit Space
flags do not establish real display-transition, Stage Manager, full-screen Space,
sleep/wake, or unplug behavior. These checks are required before declaring mature
macOS support complete.

## Hardware qualification

Use the packaged app and a Retina plus non-Retina display, with one monitor above
or left of the primary display. For each case, record macOS version, architecture,
display geometry/scaling, and pass/fail:

- Drag every edge and corner, change scale and opacity using a mouse and trackpad,
  pinch around different image positions, and cancel a drag with Escape. Check
  stable aspect ratio, logical zoom, keyboard nudges, and absence of resize flashes.
- Move pins between displays repeatedly, including while drawing, viewing OCR,
  showing a thumbnail, or dragging click-through controls. Check the image anchor,
  sharp border, toolbar placement, and retained expansion size.
- Switch Spaces and enter a different application's full-screen Space. Exercise
  Stage Manager and application switching with popups and editable toolbar fields.
  Check that passive controls do not take focus and explicit edits accept IME.
- Enter click-through; use the underlying application, then move the pin, change
  opacity, and exit through its controls. Close via each configured mouse action
  and check that no click reaches the application below.
- Hide several pins to the top. Test hover while the application is inactive,
  overlapping reservations, a notched display, and menu bar/Dock auto-hide changes.
- Unplug a monitor, rearrange displays, change resolution, and sleep/wake. Check
  that pins and recovery controls remain reachable without image rescaling.
- Restore each pin mode after restart and group switching. Replace content from a
  file and clipboard, edit annotations/text, use recognition and translation,
  copy original/current content, and save through native dialogs.

Windows native move/resize and mixed-DPI regression tests must be run on Windows.
An arm64 build or an x64 syntax check does not establish Intel runtime qualification.
