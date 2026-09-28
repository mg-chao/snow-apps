# macOS screenshot support and validation

Snow Shot uses ScreenCaptureKit on macOS 15+, including when a migrated settings
file selects DXGI, WGC or GDI. Windows capture API and color-filter restoration
controls are hidden without changing their stored values. Screenshot selections
can be pinned through the shared export pipeline; screen recording remains
unavailable on macOS.

## Workflows and sizing

Ordinary, delayed, fixed-region and smart selections share the editor, annotations,
OCR/QR, clipboard, image export, autosave and history pipeline. Direct capture
resolves the display under the pointer or an eligible window of the frontmost
application before dispatching work. Window captures preserve transparency.
Cursor-enabled snapshots request ScreenCaptureKit's embedded cursor once.

The macOS canvas uses desktop **points**, with the desktop's minimum origin
translated to (0, 0). Display adjacency, rotation, offsets and gaps are preserved.
The selection toolbar edits points and separately shows output pixel dimensions.
For a selection, the maximum backing scale of displays with positive-area
intersection determines the output size:

`ceil(selection width × scale)` by `ceil(selection height × scale)`.

Touching a display edge does not select its scale. Each source is smoothly
resampled through the same canvas transform; gaps remain transparent (formats
without alpha use the existing flattening policy). Annotations, rounded corners
and shadows are rendered at the output resolution. Exported pixel dimensions do
not depend on a Qt device-pixel-ratio tag.

Two adjacent, top-aligned 1920 × 1080-point displays at 2× and 1× therefore produce
**7680 × 2160 pixels**; each display occupies 3840 × 2160 pixels. Selecting only the
1× display produces 1920 × 1080 pixels. History format 2 stores complete source
canvas rectangles, coordinate space and backing scale independently of image
size. Version 1 records retain their original pixel semantics. Restored point
geometry does not depend on which monitors are currently attached.

Scrolling captures pass desktop points to native acquisition and a fixed pixel
viewport to the stitcher. Pause/resume and direction changes retain that viewport.
Display changes invalidate the session and present a recoverable error. Explicit
window exclusions remain active. Automatic scrolling sends events to the
application beneath the selection, excluding Snow Shot's own windows, without
moving the pointer. The isolated macOS input adapter resolves CoreGraphics'
`CGEventSetWindowLocation` bridge at runtime because PID-directed wheel events
do not populate AppKit's local position. This symbol is exported but not in the
public SDK; if unavailable on a future macOS release, automatic scrolling fails
recoverably instead of moving the pointer. Requalify it when upgrading macOS.

## Permission recovery

Screen Recording permission is required for capture. Use the existing App
Permissions page to open System Settings, enable the deployed Snow Shot copy,
and relaunch when macOS requests it. Permission denial is reported without a
capture retry loop. A locked desktop or disconnected display is unavailable,
even when Screen Recording permission was previously granted.

Accessibility enables smart element selection and automatic scroll input. Without
it, smart selection keeps window fallback; the automatic-scroll action opens the
existing permission guidance. See the signing and stale Accessibility-grant
recovery instructions in `docs-macos-build.md`. Do not reset unrelated TCC grants.

## Targeted validation

Set up build tools using `source scripts/snow-build-environment.sh` followed by
`snow_setup_tools`. Build the app and the named test executables before running
these filtered checks (never the full suite):

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^(snow-shot-(selection-render|screenshot-export-service|screenshot-history|capture-history-repository|direct-capture-native|direct-capture-workflow|direct-capture-frame|ocr-cpu-recognition-service|ocr-background|qr-recognition-service|capture-worker|physical-cursor|selection-geometry|capture-workflow|scrolling-image-replay|scrolling-auto-scroll|scrolling-capture-exclusion|selection-resize-workflow|screenshot-color-picker|settings-catalog|macos-shell-policy)-tests|snow-canvas-(filter-render|smart-erase)-tests)$'
cargo test --manifest-path snow-crates/Cargo.toml -p snow-capture -p snow-capture-c -p snow-macos --lib
cargo clippy --manifest-path snow-crates/Cargo.toml -p snow-capture -p snow-capture-c -p snow-macos --all-targets -- -D warnings
```

Selection fixtures exercise the required 7680 × 2160 result, seams, smooth
resampling, individual scales, partial/edge-only intersections, negative origins,
vertical offsets, portrait displays, transparent gaps, scale-only reconfiguration,
per-display cursor mapping, and legacy Windows output sizes. Export and history
fixtures check annotations, pixel dimensions, PNG data and disconnected sources.
Scrolling replay covers both directions, pause/resume, stale generations, and
changed pixel viewports. Native-frame tests cover validation and lease lifetime.

Run the interactive smoke check only on an unlocked desktop with existing grants:

```sh
cmake --build build/snow-shot-macos-arm64-debug --target snow-shot-macos-screenshot-native-smoke
ctest --test-dir build/snow-shot-macos-arm64-debug \
  -R '^snow-shot-macos-screenshot-native-smoke$' -V
```

It shows temporary fixture windows, captures the desktop and a translucent window
with both cursor modes, checks versioned geometry, cancellation and retained
leases, reads an excluded-window region stream, and round-trips PNG and clipboard
images. It restores clipboard formats after testing. If Accessibility is granted,
it also tests direct target resolution and horizontal/vertical scroll dispatch to
a helper process while checking that the pointer stays fixed. Missing native
prerequisites are reported explicitly, not treated as validated functionality.

## Hardware acceptance checklist

On a Retina + non-Retina pair, repeat ordinary/delayed/fixed and direct captures;
annotate across the shared edge, copy/paste, save PNG/JPEG, run OCR/QR, and restore
history after disconnecting a monitor. Confirm the exact sizing example above,
negative origins, portrait layouts and desktop gaps. Check cursor inclusion on
both displays. Scroll in each direction, pause/resume, and change a display scale
while scrolling; the session must stop with a recoverable error.

Development validation used one Apple Silicon Mac with a 3840 × 2160 capture
surface. Physical mixed-scale displays, live monitor reconfiguration, Intel
hardware and Windows runtime tests were unavailable. Rust cross-checks for
x86_64-apple-darwin and x86_64-pc-windows-msvc supplement the native ARM64 build;
they do not replace physical hardware acceptance.

The ARM64 app and affected targets built successfully. Validation passed 23
unique filtered CTest tests, the permission-enabled native screenshot smoke,
89 `snow-capture` tests (3 ignored), 40 `snow-capture-c` tests and 15 `snow-macos`
tests. The three capture crates passed Clippy with warnings denied. Translation
extraction found no unfinished entries in English, Simplified Chinese or
Traditional Chinese. OCR coverage includes a translated point canvas rendered
at 2×; QR fixtures cover large images. These checks do not constitute a manual
end-to-end qualification of every editor action.

The ancillary non-Windows updater build cleanup passed its service tests and
Clippy. Its transaction fixtures require a canonical temporary directory on
macOS (`TMPDIR=/private/tmp`); the default `/var` alias is intentionally rejected
by the updater's symlink protection.

## Recapture cursor ownership

Recapture excludes only its editing surfaces. Its cursor owner can be another
Snow Shot window or a foreign floating panel. Scrolling's normal-window and
foreign-process filters must not be reused for this selection.

The recapture transaction uses AppKit's native mouse hit test and steps below
explicitly excluded window IDs. WindowServer bounds alone are insufficient:
click-through windows and decorative system surfaces can cover the pointer
without receiving input. The selected native ID is used to resolve process
metadata, and the same hit test runs again before cursor refresh. Windows owned
by Snow Shot are activated through their retained `NSWindow`; external windows
use Accessibility. A local mouse refresh is dispatched to the verified window,
then its actual view handles a cursor update, including when the stationary
pointer remains inside an existing tracking area. Both Qt and native input
transparency last through capture and are restored when the transaction ends.

Run the focused checks with:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug \
  -R '^snow-shot-macos-(screenshot-window-target|recapture-(focus|native|floating|local|local-floating))-tests$' \
  --output-on-failure
```

The target-policy tests need no live desktop or permissions. Native cases cover
normal and floating cursor owners in both processes, a separate click-through
surface, repeated keyboard recapture, input/focus restoration, and actual captured
I-beam pixels. Native cases skip without Accessibility or Screen Recording access.

Validation on 2026-09-21: the Debug application build and all ten related
capture-worker, capture-workflow, target-policy, recapture, and overlay checks
passed. Repeated same-process native cases also passed five runs each. A temporary
binary using the old scrolling target policy failed both the foreign floating
and same-process regressions; the floating fixture explicitly verifies that its
WindowServer layer remains nonzero while inactive. Changed C++/Objective-C++
files passed clang-format, and `git diff --check` passed. No full suite was run;
clang-tidy is disabled in this build.

## Fullscreen screenshot focus

Screenshot windows use nonactivating NSPanels: they can own keyboard focus while
another application remains foreground in its fullscreen Space. Revealing the
menu bar must not make the screenshot lose hover input. Ordinary windows,
recording surfaces, and tools released from screenshot ownership retain Qt's
normal activation behavior.

Apple documents the native contract in
[nonactivatingPanel](https://developer.apple.com/documentation/appkit/nswindow/stylemask-swift.struct/nonactivatingpanel)
and distinguishes ordering from key/main focus in
[orderFront](https://developer.apple.com/documentation/appkit/nswindow/orderfront(_:)).
The Qt integration is version-specific, not a public Qt API:

- The adapter adds the panel style at QNSPanel initialization and retains it
  through Qt's initial style rewrites. Native experiments found that changing
  only the style bit after creation did not preserve hover. Apple's settable
  `styleMask` documentation does not guarantee this construction-time behavior.
- A pooled tool changes native panels when it enters or leaves screenshot
  ownership. Its QWidget, content view, geometry and requested ordinary settings
  survive. Observe native surface creation as well as showing: a hidden tool can
  acquire its transient owner before its first show.
- Initial ownership is available before native creation. AdQt popup, tooltip and
  busy-indicator creation, floating palettes, and the canvas sampler publish a
  `ScopedWindowCreationOwner` while creating their surface and setting its Qt
  transient parent. The Cocoa adapter reads this temporary, guarded owner during
  panel initialization. The context is then removed; it does not cache ownership
  or change QObject lifetime. Initial popup assignment therefore keeps the first
  native panel, including when an old QObject parent belongs to another family.
- Screenshot toolbars declare their permanent role in their constructor without
  creating a native surface. The role survives detaching, hiding and native
  retirement. Shared tools still follow their live owner; native replacement is
  retained for actual changes of activation behavior. The sampler destroys its
  session surface directly, avoiding an ordinary replacement just before teardown.
- In [Qt 6.11.1 QCocoaWindow::raise](https://github.com/qt/qtbase/blob/v6.11.1/src/plugins/platforms/cocoa/qcocoawindow.mm),
  Qt calls `orderFront:` with the native window itself as sender, then activates
  the application unconditionally. The adapter disables that final unconditional
  step and performs the same request in the concrete QNSWindow/QNSPanel ordering
  method only for windows outside screenshot ownership. Native ordering with a
  different sender, including ordinary nonactivating show, keeps its behavior.
  The cached `QT_MAC_SET_RAISE_PROCESS=0` option must never be used alone: doing so
  breaks ordinary and recording activation from an inactive application.
- Ordinary raises retain Qt's `activateIgnoringOtherApps:` request. Replacing it
  with the newer cooperative `activate` is not equivalent: Apple documents that
  [activate](https://developer.apple.com/documentation/appkit/nsapplication/activate())
  does not guarantee activation, and the inactive-app regression reproduces that
  difference. The local deprecation suppression preserves the pinned Qt behavior.
- The shared explicit focus helper also honors nonactivating panels, so recapture
  restoration does not bypass the panel contract. During recapture's input
  handoff, a key nonactivating panel explicitly releases borrowed focus with
  `deactivate`. Apple says this method should normally be left to AppKit; this is
  a narrow exception verified by repeated recapture/cursor tests. Merely activating
  an already-foreground target or invoking `resignKeyWindow` did not complete the
  handoff in the native reproduction.

The adapter changes only concrete Qt classes, not NSApplication or AppKit's
window classes. It depends on private QNSPanel initialization, QPA native
recreation and Qt's ordering convention. CMake pins Qt 6.11.1 exactly. Qt's
[QPA documentation](https://doc.qt.io/qt-6/qpa.html) provides no source or binary
compatibility guarantees, so a Qt upgrade must revalidate these assumptions and
the following focused tests:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-macos-(screenshot-activation(-native)?|recapture-(native|floating|local|local-floating))-tests$'
```

Offscreen coverage checks role inheritance before native creation and ownership
changes. Native activation coverage first establishes an inactive application,
then checks actual native key focus and typed input without clicking for ordinary
windows, recording tools and released pooled tools, through both QWidget and
QWindow raises. It also checks nonactivating shows while a screenshot is visible.
The fullscreen fixture checks top-edge-to-center hover before clicking, toolbar
text input, pooled popups, recreated surfaces, and explicit focus restoration.
Test native activation directly: Qt can report an active window even when AppKit
has no key window.

Creation regressions run in `snow-shot-macos-overlay-initialization-tests`
(offscreen) and `snow-shot-macos-screenshot-stacking-native-tests` (Cocoa). They
check ownership at the first native surface event and retain that first NSWindow
to verify that assigning the initial owner does not replace it. The native
identity assertion failed before creation context was added. Scoped-owner
nesting, owner destruction and toolbar role retention are also covered offscreen
by `snow-shot-macos-screenshot-activation-tests`. These checks establish avoided
native recreation, not an elapsed-time performance claim.
