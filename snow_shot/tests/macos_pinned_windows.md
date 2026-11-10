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
  -R '^snow-shot-macos-pinned-(window|focus|delivery|drag|pixel-alignment)-tests$' \
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

## Persistent stacking restoration

Build `snow-shot-pinned-window-stacking-tests` and `snow-shot-pinned-window-tests`.
Run the focused model, storage, and Cocoa application checks:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug \
  -R '^snow-shot-pinned-(window-stacking(-native)?|stacking-restore(-native)?)-tests$' \
  --output-on-failure
```

The native adapter checks order permutations, raise/lower, native levels, auxiliary
ownership, surface rebinding, geometry, and the frontmost application/key window.
The application case covers storage restart, group switches, hide/show, and explicit
show commands while image loading is blocked. Cocoa capture uses visible window
numbers so NSPanel pin surfaces participate.

These Cocoa cases have been added but have not been run on a macOS host in the
Windows implementation session. Native macOS qualification remains required;
Windows and offscreen results do not qualify AppKit behavior. Include click-through,
hide-to-top handles, and application reactivation in the native host pass.

## Input interruption and boundary audit — 2026-09-21

The follow-up audit covers input behavior independently of the event source or
remote desktop client. Two missing-release regressions were reproduced: pins
continued moving after a move reported no left button, and deferred mouse actions
retained capture and could run on a later release. Both now cancel the owned
gesture on that button-state transition. Normal releases still apply their final
position; hover after a delivered release does not discard an already queued action.

A reverse native drag also reproduced rollback at the menu bar. AppKit constrains
that frame below the menu bar, so the adapter now resolves `constrainFrameRect`
before applying and verifying Qt/native geometry. The native tests check that
this adjustment remains a valid drag and that Qt geometry matches native readback.

Regression coverage includes lost releases during moving and resizing, another
button remaining held, coalesced movement followed by a release, repeated native
fractional movement in both directions, and the menu-bar boundary. These are
reproducible input sequences, not certification of a particular remote desktop
client or every reconnect/mixed-display scenario.

All 15 selected checks passed: shared/Cocoa global input, physical cursor,
mouse-release actions, pinned movement shortcuts, interrupted toolbar dragging,
toolbar geometry at 1×/2× and on Cocoa, controlled pin interaction at 1×/2×,
and native pin policies, input delivery, dragging, and pixel alignment. The three
new failure cases were observed before their corrections. No full suite ran.

## Fractional pointer drag regression — 2026-09-21

Physical pointer events can contain fractional logical coordinates. The Cocoa
adapter now rounds the origin once to QWidget's integer logical frame, applies
the menu-bar constraint, and commits through Qt. Writing a second fractional
NSWindow frame after QWidget::setGeometry created competing geometry authorities:
AppKit readback could differ from the requested backing-aligned position and
cancel the drag. Rounding the origin separately preserves the window size.

The offscreen interaction case covers fractional movement and cancellation.
The Cocoa placement case verifies exact logical-frame readback. The native drag
case sends fractional intermediate positions, crosses each attached display in
both directions, and verifies the cursor anchor, size, and released position.
It skips with code 77 without event-posting permission.

## Hidden placement and pin creation regression — 2026-09-22

Pin presentation verifies native geometry before showing its first frame. The
screenshot export path commonly checks out a prewarmed, hidden window whose
NSWindow already exists with an older frame. QWidget::setGeometry only updates
its cached geometry while hidden; using native readback immediately afterward
therefore rejected creation. A fresh pin could also fail when placement needed
to apply the menu-bar constraint before show.

The Cocoa adapter commits hidden geometry through QWindow::setGeometry before
verification. Both widget and native state retain the same integer logical target,
without a second direct NSWindow frame write or briefly showing the wrong frame.

The native placement fixture now checks hidden placement, show, hide/replacement,
and every attached display. The creation fixture exercises fresh and prewarmed
ScreenshotPinnedWindow instances through successful first-frame completion. It
runs offscreen and on Cocoa; the native fixture reproduced the rejection before
the correction. Run only these and the related pin drag checks:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-((macos-)?pinned-creation|macos-pinned-(window|drag|pixel-alignment)|pinned-(controlled|retina)-interaction)-tests$'
```

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

## System chrome stacking regression — 2026-09-28

Topmost pins and their auxiliary controls now occupy the level immediately below
recording's reserved band, above both the menu bar and Dock. The shared native
level calculation preserves `screenshot > recording (including popups) > pin`.
Disabling always-on-top still selects the normal window level.

AppKit's frame constraints depend on the window level: topmost pins can now
cross the menu bar, while normal pins retain the constraint described in the
older geometry audits above. Placement still commits through Qt and verifies
native readback, including hidden preparation and reuse.

The native pin regression failed at the system-chrome level assertion before
the correction. Coverage checks menu-bar and Dock overlap, the normal-level
opt-out, auxiliary focus, and surface recreation. The offscreen layer test checks
the band boundaries; the Cocoa stacking test checks WindowServer ordering after
raising windows, toggling topmost, reparenting popups, and recreating surfaces.

The WindowServer-order test also reproduced a startup registration race on the
original implementation. It now waits up to five seconds for all fixture windows
to enter the on-screen list, then asserts their order without retrying a wrong
order. The native stacking check passed three consecutive runs after this change.
All five focused Debug checks passed (pin ownership, native pin policies, focus,
offscreen overlay/layer policy, and native capture stacking). Changed-line format
and diff whitespace checks passed; this build has clang-tidy disabled. No full
suite was run.

## Safe-area content alignment — 2026-10-22

Investigation of [issue #2129](https://github.com/mg-chao/snow-shot/issues/2129)
found a reproducible content-layout defect consistent with the reported border
and image displacement near the menu bar. Qt enables safe-area contents margins
for top-level widgets by default. Its Cocoa adapter includes the screen's notch
safe area when the window overlaps that band. The pin's layout then insets and
shrinks its canvas, while its manually positioned border retains the full window
rectangle. Zero layout margins do not disable these widget contents margins.

Pins now explicitly disable `WA_ContentsMarginsRespectsSafeArea`, matching the
screenshot overlay's full-surface coordinate contract. This preserves canvas,
border, and image alignment while retaining intentional menu-bar overlap.

The deterministic offscreen regression supplies changing platform safe-area
margins, including entering and leaving the top band and insets on all sides.
It checks fresh and prewarmed pins at 1× and 2×. Before the correction, a top
inset of 10 moved the canvas down and shortened it while leaving the border
unchanged. Both regression cases failed before the correction and passed after
it. Related offscreen interaction/pixel checks and Cocoa pixel alignment also
passed; no full suite ran.

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-(pinned-(retina-)?safe-area|pinned-(pixel-alignment|retina-interaction|controlled-interaction)|macos-pinned-pixel-alignment)-tests$'
```

The investigation host's attached external display reports zero safe-area
insets. The reporter's exact hardware and visual symptom remain unconfirmed.
On a notched MacBook display, manually move a pin across the menu-bar/notch band
and beyond the display's top edge, then back down, with menu-bar auto-hide both
enabled and disabled. Check that image landmarks retain their position relative
to the border without cropping or extra border lines.

### Related full-screen canvas and recording audit

The screenshot overlay already opts out of safe-area contents margins. The
full-screen canvas and recording area instead position their drawing children
explicitly from the window rectangle or capture selection; changing safe-area
insets does not shift those surfaces.

The canvas audit did reproduce a related interaction defect: Qt delivers a
same-size resize event for safe-area changes, and the canvas resize handler
cancelled color sampling and recalculated the camera on every notification.
The handler now updates only when the drawing surface actually changes. Both
new canvas cases failed before this correction and passed afterward; the tests
also check that a real resize still cancels sampling and preserves pan/zoom.

Recording checks passed without production changes for full-display and partial
regions extending above the display, in drawing and trim-preview modes. They
verify canvas/border alignment, unchanged capture regions, annotation transforms,
and input regions under changing top and all-side insets at 1× and 2×.

All six focused checks passed; no full suite ran:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-(global-canvas|global-canvas-safe-area-[12]|recording-safe-area-[12]|overlay-safe-area)-tests$'
```

These are deterministic QPA-inset tests, not native notched-display or encoded
recording validation. The hardware check described above remains outstanding.

## Image drag-and-drop regression — 2026-11-04

Pinned windows and their descendants now use a reserved native band above the
menu bar, Dock, and desktop toolbar, but below AppKit's dragging window. The
previous screen-saver-relative band prevented native drop delivery. Recording,
global canvas, and screenshot windows retain their existing capture bands;
turning off always-on-top still restores the normal level.

Qt 6.12's Cocoa backend can return `IgnoreAction` after an external AppKit
receiver has accepted a copy. Immediate cleanup then removes the exported image
before the receiver opens its URL. On macOS every published drag now retains its
file for the existing 30-minute transfer grace. The 64-file/512-MiB budgets,
expiry, and failed-deletion retry remain enforced. Preparation failures still
remove unpublished files immediately. Windows cancellation behavior is unchanged.

The native regression uses the actual pin gesture and asynchronous exporter. It
checks an AppKit receiver that decodes the image and copies its file after the
drag returns, pin-to-pin replacement, external file URLs and TIFF pixels dropped
onto a pin, and pin exports into Image Stitching and Image Splitting. It also
checks that the system clipboard remains unchanged. With the old cleanup
restored, the external receiver accepted the image, Qt returned `IgnoreAction`,
and the post-drag file read failed. The corrected path passes.

The related audit found that the desktop floating toolbar already uses a band
below AppKit dragging; its native file, image, text, and HTML tests pass. Collage
reordering and settings rearrangement use internal payloads and do not share the
exported-file lease. No additional production change was needed for these paths.
The floating-toolbar native test target's missing export dependencies were added
so its existing coverage can link and run.

The arm64 Debug application build and all 12 focused checks passed with Qt 6.12.0.
Changed C++/Objective-C++ formatting and `git diff --check` also passed. No full
suite or benchmarks ran; this build has clang-tidy disabled.

Build `snow_shot`, `snow-shot-pinned-window-tests`,
`snow-shot-macos-overlay-initialization-tests`, `snow-shot-macos-pinned-window-tests`,
`snow-shot-macos-floating-toolbar-drop-tests`, and `snow-shot-global-canvas-tests`
with `snow-shot-macos-arm64-debug`. Run only the focused checks:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-(pinned-(drag-export|file-drop|auxiliary-ownership)|macos-(pinned-content-drag-.*|overlay-initialization|pinned-window|screenshot-stacking-native|floating-toolbar-drop-policy|floating-toolbar-content-drop-native)|global-canvas)-tests$'
```

Native input checks require existing event-posting permission and do not request
or change privacy grants. The AppKit receiver covers Finder-style deferred file
copying; it is not an automated test of Finder itself. Intel/macOS 15 and Windows
native execution were not part of this validation.
