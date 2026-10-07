# macOS recording acceptance

Recording requires macOS 15+. The Qt controller uses the existing opaque C session
API; a native worker owns capture, effects, audio, encoding and destruction.
The UI and capture region use desktop points. Output sizing resolves the native
desktop transform (the highest intersecting display density), then applies the
orientation-aware quality cap and output-format alignment. Odd capture dimensions
and negative origins are retained; output dimensions are fixed at startup.

## Focused checks

Use the toolchain described in `../../docs-macos-build.md`:

```sh
scripts/build.sh snow-shot-macos-arm64-debug --target snow_shot
cmake --build build/snow-shot-macos-arm64-debug --parallel 8 --target \
  snow-shot-screen-recording-controller-tests \
  snow-shot-screen-recording-area-window-tests \
  snow-shot-screen-recording-shortcut-tests \
  snow-shot-screen-recording-geometry-tests \
  snow-shot-screenshot-recording-workflow-tests snow-shot-app-permissions-tests
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-(app-permissions|screen-recording-(controller|area-window|geometry|shortcut)|screenshot-recording-workflow|recording-effects-preview|recording-capture-exclusion|macos-recording-capture-exclusion)-tests$'

export FFMPEG_DIR="$PWD/.tools/macos/installed/dynamic/arm64-osx-snow-shot"
export DYLD_LIBRARY_PATH="$FFMPEG_DIR/lib"
cargo test --manifest-path snow-crates/Cargo.toml \
  -p snow-recording-c -p snow-recording-runtime -p snow-recording-effects --lib
cargo test --manifest-path snow-crates/Cargo.toml \
  -p snow-recording-export --lib streaming::tests::
cargo test --manifest-path snow-crates/Cargo.toml -p snow-macos --lib desktop::tests
cargo clippy --manifest-path snow-crates/Cargo.toml \
  -p snow-recording-c -p snow-recording-runtime -p snow-recording-effects \
  -p snow-recording-export --all-targets -- -D warnings
```

The controller fixtures cover deferred startup, permission denial, cancellation,
countdown, pause/resume, stop failure, clipboard publication, exclusions, and stale
asynchronous native sizing. Controlled worker barriers also verify that controller
destruction neither blocks the GUI thread nor releases a finalizing session early. Area tests cover logical dragging, resizing, odd sizes,
negative origins, annotation persistence and countdown placement. The Cocoa shortcut
test checks native click-through, drawing input, stacking and fullscreen Space
policies through repeated show/hide cycles. Native desktop geometry tests exercise
mixed display density without requiring multiple physical displays.

Recording settings and render progress share recording-area ownership and application
modality. Render progress detaches before area teardown so rendering, cancellation,
and retry can continue independently. Run the focused offscreen and Cocoa checks:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-(macos-)?recording-modal-stacking-tests$'
```

The Cocoa fixture uses the controller with a fake recording backend; it does not
capture the desktop or open audio devices. It checks native window levels and order
above both recording controls after raises, cancellation, and retry. On 2026-10-01,
the render-progress check reproduced the detached dialog's stacking failure before
the fix and passed afterward, along with its offscreen ownership/lifecycle check.

Recording border input has a focused Cocoa check (requires permission to post mouse
events; otherwise CTest reports a skip):

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-macos-recording-border-input-tests$'
```

It drags every painted edge and corner to the ten-physical-pixel minimum, across
the fixed boundary, and back before releasing. The same cases run offscreen in
the area-window tests. The native overlay-initialization fixture also verifies
that AppKit cannot take over recording geometry after native surface recreation.

## Cursor orientation and hotspot regression

Cursor bitmaps, effect tiles, and editable cursor assets use top-first rows.
`NSCursor.hotSpot` uses points from the image's top-left; Quartz input locations
use top-left desktop points. Drawing the native CGImage into an untransformed
bitmap context preserves its scanline order. Applying a Cocoa-style Y flip here
inverts only the image, leaving its hotspot unchanged: the pointer tip then appears
below the highlight. Fix this at cursor acquisition, shared by direct and editable
recording, rather than compensating in the highlight or desktop transform.

Focused, offscreen checks (use the FFmpeg environment above for runtime tests):

```sh
cargo test --manifest-path snow-crates/Cargo.toml -p snow-macos --lib cursor::tests
cargo test --manifest-path snow-crates/Cargo.toml -p snow-macos --lib compositor::tests
cargo test --manifest-path snow-crates/Cargo.toml -p snow-recording-runtime --lib macos_effects::
cargo test --manifest-path snow-crates/Cargo.toml -p snow-recording-runtime --lib macos::editable_cursor::
```

The asymmetric native pixel fixture failed with reversed rows before the correction
and passed afterward. Coverage includes premultiplied alpha, scaled hotspots at
1x/1.5x/2x, negative desktop origins, destination offsets, tile boundaries, editable
straight-alpha assets, and native GPU overlay/highlight orientation with padded
rows and clipping at the canvas edge. For visual acceptance, record with highlight
and separate cursor enabled, switch between arrow and text cursors, and verify the
hotspot stays at the highlight center in both direct output and editable export.

## Export completion notifications

macOS notifications use `UserNotifications.framework` rather than Qt's deprecated
`NSUserNotification` tray backend. Enable **Notify after export completes** in the
recording settings. The first notification requests macOS alert and sound permission
before delivery; later notifications respect changes in System Settings. Delivery requires
an app bundle with a bundle identifier. Running an unbundled executable reports
unavailable delivery instead of invoking the native API.

The content requests the default notification sound; the delegate permits foreground
banners, sound and Notification Center entries. macOS still controls presentation:
Snow Shot must have **Desktop** notifications and a temporary or persistent alert
style enabled. **Play sound for notification** controls whether sounds are audible.
Focus and **When mirroring or sharing the display → Notifications Off** can silence
an accepted notification while keeping its Notification Center entry. During active
screen sharing, choose **Allow Notifications** to permit pop-ups; this system-wide
preference affects all apps and can expose notification contents on the shared screen.
An actual silent export was traced in the `usernoted` log to `resolutionReason:
display shared` with `interruptionSuppression: silence`. Delivery acceptance alone
does not establish that macOS displayed a banner. Do not bypass system policy with
critical alerts, retries or fixed delays.
After explicitly allowing notifications during sharing, a normal six-second UI
recording/export was verified in the native log with `interruptionSuppression: none`,
foreground presentation options `sound`, `list`, `banner`, an alert displayed by
Notification Center, and `Playing notification sound` for the Snow Shot bundle.

Each export notification retains its own file path, so clicking an older notification
reveals that export in Finder even after another export or an update notification arrives.
Native delivery and notification actions are independent of tray visibility: disabling
the tray hides only its icon and menu on macOS.

Every request reports its own accepted, unavailable, denied or failed result. Rejected
requests activate the main window and show the original message with its original
severity. This shared fallback also covers capture failures, update notices, background
file-pinning errors and unavailable features. Native errors remain in diagnostic logs;
they do not replace the user-facing message. Accepted requests do not activate a fallback
window, including when macOS suppresses their presentation. Denied exports are not
replayed after permission is enabled.

Run only the focused regression checks:

```sh
cmake --build build/snow-shot-macos-arm64-debug --parallel 8 --target \
  snow-shot-macos-system-notification-tests snow-shot-system-tray-controller-tests \
  snow-shot-system-notification-controller-tests snow-shot-screen-recording-controller-tests
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-(macos-system-notification|system-notification-controller|recording-export-tray|recording-export-notification)-tests$'
```

The native API fixtures run without requesting real permission or posting alerts.
They cover alert/sound authorization ordering, concurrent exports, default sound,
foreground banner/list/sound presentation with ordinary interruption priority,
per-notification actions, per-request denial, permission changes, scheduling errors,
out-of-order completion and shutdown callbacks. The portable notification controller
tests exercise visible fallback content/severity, accepted delivery without window
activation, worker-thread completion and controller shutdown. The tray fixture verifies
native delivery and old/new actions with a hidden tray. The recording controller tests
cover successful direct, post-processed and trimmed exports,
auto-exit, failures and the disabled notification setting.

For native acceptance, use the signed installed app with notifications enabled:

1. Export a short video and allow Snow Shot notifications when macOS prompts.
   Verify the banner and sound while Snow Shot is active and again while another
   app is active, with sound and Desktop alerts enabled and Focus disabled. Check
   the screen-sharing notification preference if macOS suppresses the banner.
2. Export two different files, then click the older entry in Notification Center.
   Confirm Finder selects the older file. Repeat with a trimmed export and with recording
   auto-exit enabled.
3. Deny notifications in System Settings and export again. Verify the file still
   completes, the main window shows the export-completion message and the denial appears
   in the diagnostic log. With the main window hidden, also try an unavailable feature
   or pin an invalid selected file and verify its original warning is visible.
   Re-enable notifications
   and verify a new export produces a notification without replaying denied exports.
4. While another app shares the display, verify **Notifications Off** suppresses
   the pop-up, then explicitly allow notifications during sharing and verify the
   next export can present a banner. Also disable Snow Shot notification sounds
   and verify a new export remains visually present without sound.
5. Disable the tray icon and export again. Verify native delivery still works, including
   clicking an older notification to reveal its own file. Restore the original tray and
   notification preferences after checking.

## Native export probe

Deploy with the normal bundle installer, then use the diagnostic entry point:

```sh
cmake --install build/snow-shot-macos-arm64-debug --component SnowShot \
  --prefix "$PWD/build/snow-shot-macos-arm64-debug/run"
build/snow-shot-macos-arm64-debug/run/snow_shot.app/Contents/MacOS/snow_shot \
  --recording-macos-probe "$PWD/build/recording.mp4" system-audio effects
```

The probe uses the ordinary C API and a Cocoa event loop, records a 321×239-point
region, pauses for 300 ms between two 800 ms active intervals, and finalizes.
Choose `.gif`, `.apng`, or `.webp` to exercise animation export. Optional arguments
are `system-audio`, `microphone`, `effects`, `software`, and `hevc`. Omit audio
arguments for animations. The probe does not modify application settings.
Check decoded dimensions, duration, codec, every animation frame and loop metadata;
play a known sound throughout startup and recording to distinguish silence from
failed system-audio capture.

## Concurrent screenshot input

With system audio enabled, record a small region while taking a screenshot. Move
the pointer across screenshot toolbar buttons, open their popups, and drag the
selection. Verify smooth input and stable cursor changes with recorded cursor
visibility both enabled and disabled. Repeat after pause/resume and with a large
recording region. Play a known sound and confirm the exported video still contains
system audio and follows the selected cursor visibility setting.

The audio stream has no screen-output consumer and must disable cursor capture and
click visualization. Its permission-free regression runs with
`cargo test --manifest-path snow-crates/Cargo.toml -p snow-macos --lib audio::tests`.
On 2026-09-26, user-assisted verification of the rebuilt arm64 debug app confirmed
smooth screenshot-toolbar input with system audio enabled and audible sound in
the exported recording. The configuration regression failed before the correction
and all three focused audio tests passed afterward.

## Coverage on 2026-09-22

Hardware: Apple M4 Mac mini, one 1920×1080 display at 1× scale.

The arm64 debug application build and bundle deployment passed. All nine focused
CTest checks passed, including the Cocoa checks. After the shutdown change, all
four controller/exclusion checks passed again. Rust checks passed: 29 C facade,
100 recording runtime, 41 effects, 22 streaming-export tests (one existing ignored),
and five native desktop geometry tests. Clippy, formatting, translation extraction
and catalog completeness checks passed. The full test suite was not run.

- MP4 hardware H.264, software H.264 and hardware HEVC export completed. On this
  display MP4 output is 320×238; the capture region remains 321×239 points.
- GIF, APNG and WebP exported at 321×239. Pillow decoded every frame and confirmed
  infinite-loop metadata in moving-content probes. A static WebP may collapse to
  one frame. The deployed bundle also exported and decoded all four formats.
  FFmpeg's GIF decoder reported LZW errors for a GIF that
  Pillow decoded completely; use an independent decoder when checking GIF output.
- System audio produced an AAC stream with a nonzero decoded signal from a known
  system sound. A pause/resume probe produced approximately 1.67 s of output,
  excluding its 300 ms pause. Effects and cursor-highlight probes completed.
- No microphone input device is installed on this machine. Requested microphone
  recording correctly failed with device unavailable; audible microphone capture
  and combined-source synchronization remain unverified.
- Physical Retina/mixed-density displays, Intel hardware and Windows execution are
  unavailable. Fullscreen application switching, visual effect/layout comparison,
  long recordings, and actual input delivery to another application's controls
  still need interactive acceptance. Native window-policy assertions do not replace
  those checks. Release DMG/notarization acceptance is separate from the debug app.

Manual acceptance should also draw during recording, change toolbar settings before
start, verify the toolbar stays excluded, copy each format to the clipboard, open
the recording folder, repeat start/stop, and revoke optional input permissions to
confirm basic recording remains usable when those effects are disabled.
