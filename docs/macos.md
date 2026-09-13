# Snow Shot on macOS

The Apple Silicon port targets the Qt/Rust application. It adds native display
and window capture through ScreenCaptureKit, window/control selection through
Accessibility, global keyboard shortcuts and mouse gestures, automatic scrolling,
selected-text translation, login items, region recording, and managed OCR.
Annotation, export, and history use the shared Qt code.

Settings remain discoverable on macOS. Windows-only capture APIs, process
priority classes, and DirectML are disabled with platform-specific explanations;
OCR uses the CPU. Automatic updates still require a macOS update feed, verifier,
and installer. Original display color restoration is not yet implemented for the
sRGB capture path. These incomplete ports are identified as such in settings,
not described as operating-system limitations. The image viewer is not included
in this preset.

## Build

Use an Apple Silicon Mac with Xcode command-line tools, Homebrew, and rustup.
The source requires macOS 14 or later for `SCScreenshotManager`. Homebrew bottles
can require a newer macOS release; the packager computes the minimum supported
version from the actual bundled Mach-O libraries instead of promising that a
binary built with newer bottles will run on macOS 14.

```sh
scripts/bootstrap-macos.sh
scripts/build-macos.sh
```

The bootstrap keeps Qt 6.11.2, the pinned zlib-ng 2.3.3 build, FFmpeg 8.1.2, and the
license collector runtime in `.tools/`. FFmpeg is built from a checksum-verified
source archive under `.tools/ffmpeg-8.1.2`, with `libwebp` enabled for animated WebP
recording. Its codec libraries and other native dependencies use Homebrew. The
source archive, license, and build configuration are included in package notices.
Rust uses the repository's pinned toolchain. The preset uses two build jobs and
disables unity builds so independent translation units also compile correctly.
Override concurrency with `SNOW_BUILD_JOBS` and `CARGO_BUILD_JOBS` when needed.

macOS pins Qt 6.11.2 to fix [QTBUG-147602](https://bugreports.qt.io/browse/QTBUG-147602),
a color-space lifetime bug in `QImage::toCGImage()` that can crash native cursor
changes. The Windows Qt pin remains unchanged.

Run the development app with:

```sh
build/macos-arm64/snow_shot/snow_shot.app/Contents/MacOS/snow_shot --show-main-window
```

Opening the app manually shows the main window; opening it again restores that
window. `--autostart` starts in the menu bar. The default screenshot shortcut is F1;
depending on the keyboard's function-key setting, this may require Fn+F1.
Allow screen recording in System Settings when macOS requests it. Keyboard
shortcuts use Carbon hot-key registration and do not require an input-monitoring
event tap. Qt's portable `Ctrl` modifier corresponds to Command by default.

“Launch at login” registers the installed app using `SMAppService.mainAppService`.
If macOS requires approval, Snow Shot opens General > Login Items and reflects
whether registration is actually enabled. Disabling the OS login item is respected
on subsequent launches. The startup decision waits for AppKit's login Apple event;
login launches do not open the main window or an Accessibility permission alert.

Global mouse gestures use a session event tap and require Accessibility access.
Command, Control, and Option labels match physical keys, while the stored
configuration remains portable to Windows. Cancelled gestures retain paired
mouse-down/up suppression; synthetic input is excluded. Missing permission is
reported when the main window is opened, and changing a mouse binding or using
its drag button can retry initialization after a failed permission check.

Selected-text translation reads the original foreground application's AX selected
text on a worker with a timeout, without replacing the clipboard. Applications
that do not expose selected text through Accessibility cannot provide it through
this path. Control-level screenshot selection falls back to the containing window
when permission or an AX control is unavailable. Fullscreen hotkey suppression
checks the foreground application's window against each display.

Automatic scrolling sends wheel events to the external window beneath the
selection without moving the pointer. Permission, target, and dispatch failures
stop automatic scrolling and show a message; manual scrolling remains available.
Quartz does not acknowledge whether the target application consumed an event.

Capture failures display a native macOS alert. Permission failures also offer
a button to open Screen Recording settings. Dismissing an alert does not suppress
feedback on a later failed capture, even when macOS no longer shows its initial
permission request. Replacing an ad-hoc signed build changes its
code identity: if macOS still denies capture while the switch is on, add the current
`/Applications/Snow Shot.app` in that settings pane and choose Quit & Reopen.

Screenshot canvases cover the entire display, including the menu bar and notch
area. Their layouts opt out of Qt's automatic safe-area inset to preserve captured
pixel positions. Shortcut hints use each display's available geometry separately
so the Dock does not cover them.

## Recording and OCR

Recording uses a continuous ScreenCaptureKit stream with a bounded latest-frame
buffer, native region cropping, and the shared encoder/pause/resume pipeline.
H.264 hardware encoding uses VideoToolbox when selected and available. System
audio uses ScreenCaptureKit's global mix; microphone capture uses AVFoundation
and does not change the system's selected input. Enable microphone permission
when recording with that input. Disconnecting an input or changing display
geometry during capture requires starting a new recording.

Mouse trails and click effects use an 8 ms read-only pointer sampler. A complete
press/release shorter than that interval may not be observed. Keyboard display
uses a session-scoped, listen-only event tap and native CoreText keycaps. It
requires Input Monitoring permission; without permission, enabling keyboard
display reports an error and the default recording path remains available.

macOS display origins are points, while frame sizes are backing pixels. The port
uses the largest connected display scale for physical origins and each display's
own scale for local pixels. This prevents overlapping capture regions on Retina
setups. Mixed-DPI layouts may contain gaps in this physical coordinate space;
selecting a region within one display preserves its native resolution.

The app bundles its native OCR helper and ONNX Runtime. The managed manifest
pins their exact signed bytes; downloaded models retain the upstream HTTPS URLs,
sizes, and SHA-256 checks. The DMG includes the Small model for offline Chinese
and English recognition. Other model choices download into the writable managed
cache on first use. The build stages the native manifest automatically; the
packager regenerates it after signing nested code. See
[OCR distribution details](../snow_shot/packaging/ocr-models.md).

## Focused validation

Keep the repository's full test suite disabled for port validation:

```sh
scripts/build-macos.sh -DSNOW_SHOT_BUILD_MACOS_TESTS=ON
cmake --build build/macos-arm64 --parallel 2 --target \
    snow-shot-macos-shortcut-tests snow-shot-macos-capture-smoke-test \
    snow-shot-macos-recording-smoke-test snow-shot-macos-ocr-tests
ctest --test-dir build/macos-arm64 -R '^snow-shot-macos-shortcut-tests$' --output-on-failure
cmake -P cmake/tests/rust-target-tests.cmake

cargo test --manifest-path snow-crates/Cargo.toml -p snow-macos -p snow-ui-selector-c --lib
cargo test --manifest-path snow-crates/Cargo.toml -p snow-capture --lib macos_layout
cargo test --manifest-path snow-crates/Cargo.toml -p snow-capture --lib convert::
cargo clippy --manifest-path snow-crates/Cargo.toml \
    -p snow-macos -p snow-capture -p snow-ui-selector-c --lib -- -D warnings
```

The deterministic settings, login, gesture, AX-context, and permission-dialog
checks can be built and run separately:

```sh
cmake --build --preset build-macos-arm64 --parallel 2 --target \
    snow-shot-macos-settings-tests snow-shot-macos-autostart-tests \
    snow-shot-macos-application-launch-tests snow-shot-macos-global-mouse-tests \
    snow-shot-macos-capture-context-tests snow-shot-macos-accessibility-permission-tests \
    snow-shot-macos-scroll-input-tests
ctest --test-dir build/macos-arm64 --output-on-failure \
    -R '^snow-shot-macos-(settings|autostart|application-launch|global-mouse|capture-context|accessibility-permission|scroll-input)-tests$'
```

These tests use injected login/AX/input boundaries and do not modify login items,
read other apps' selected text, post mouse events, or prove physical input delivery.
Validate actual login/logout, permission changes, mouse gestures, AX controls, and
automatic scrolling with the installed signed app in a desktop session.

The shortcut test covers mapping, native registration, application event
dispatch, and releasing registrations. It does not synthesize physical keystrokes.

The recording smoke test launches its own animated fixture in a separate process,
records a region through the app's public FFI, and exercises pause/resume. Supply
an explicit output path; optional flags select the second display, system audio,
or hardware H.264:

```sh
build/macos-arm64/snow_shot/snow-shot-macos-recording-smoke-test /tmp/snow-recording.mp4
build/macos-arm64/snow_shot/snow-shot-macos-recording-smoke-test /tmp/snow-recording-hw.mp4 --hardware --secondary
ctest --test-dir build/macos-arm64 -R '^snow-shot-macos-ocr-tests$' --output-on-failure
```

Inspect the resulting video with FFprobe and decode frames; file existence alone
is insufficient validation. Native audio probe instructions are in the
[audio crate](../snow-crates/crates/snow-audio-recorder/README.md).

The capture smoke test is interactive and requires an active desktop session
and screen-recording permission. It displays a temporary red/blue fixture and
checks pixel orientation, RGBA/BGRA channel order, alpha, non-overlapping display
regions, continuous cropping, and repeated stream teardown:

```sh
ctest --test-dir build/macos-arm64 -R '^snow-shot-macos-capture-smoke-test$' --output-on-failure
```

## Package

```sh
python3 scripts/package-macos.py
```

The script copies the app to temporary staging, generates a self-contained Qt
bundle, includes the exact native dependencies and license notices, verifies
every Mach-O load command, runs the built-in startup probe with isolated Qt and
dyld search paths, and creates an ARM64 DMG plus a SHA-256 file under
`build/macos-arm64/package/`. Existing packages are not overwritten; use
`--output-dir` for another build. `--pwsh` selects an existing PowerShell runtime.

The default signature is ad hoc and is not Apple notarization. Distribution
with a Developer ID and notarization requires a separate signing setup.

Before sharing a package, mount the DMG and launch a copy outside the build
tree. Verify selecting a region, drawing an annotation, saving the result,
repeating a capture with the global shortcut, and capture on each connected
display. Also record and decode an MP4, verify pause/resume duration and audio,
and recognize a known Chinese/English image with offline and freshly downloaded
models. A successful link alone is not an end-to-end application test.

## Review boundaries

Generic Clang fixes make integer conversions and default member initialization
explicit, remove unused internal helpers, and place Qt metatype declarations
at global scope. Strict warnings remain errors. On macOS, the app uses the ordinary
`-Wswitch` coverage from `-Wall`: it permits intentional catch-all Qt event
filters and exhaustive enum switches without requiring both styles at once.
Exact floating-point equality remains intentional for cache identity checks.

macOS platform code is conditionally selected. Windows presets, native capture
implementations, and release packaging remain the Windows path; a macOS build
cannot substitute for running the Windows validation before upstream merge.
