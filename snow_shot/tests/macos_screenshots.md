# macOS screenshot support and validation

Snow Shot uses ScreenCaptureKit on macOS 15+, including when a migrated settings
file selects DXGI, WGC or GDI. Windows capture API and color-filter restoration
controls are hidden without changing their stored values. Pin-to-screen and
recording are outside this screenshot implementation.

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
