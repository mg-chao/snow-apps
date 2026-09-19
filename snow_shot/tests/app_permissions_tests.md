# macOS App Permissions validation

Build `snow-shot-app-permissions-tests`, `snow-shot-macos-global-mouse-tests`, and
related settings/tray targets using `snow-shot-macos-arm64-debug` (or the x64
counterpart). Run only those named CTest entries. The permission tests use fake
probes and temporary storage; they never request or change real macOS access.
They cover permission requirements, cached dispatch, startup-once policy,
request deduplication, late callbacks, observation lifetimes, standard settings-row rendering,
page navigation, alerts, translations, and permission-button state. The mouse tests additionally
verify cached grants, tap recovery, revocation, and session suspension.

For layout inspection, run `snow-shot-app-permissions-tests --render` from the
repository root with `QT_QPA_PLATFORM=offscreen`. This writes light, dark, and
error-state images under `build/`.

For native inspection, run the rendered page with the cocoa platform and verify each unavailable
button opens the matching System Settings pane. Also inspect light/dark mode, narrow layouts,
VoiceOver, and displays with different scale factors.

A final real-grant check needs the signed Snow Shot development bundle launched
through `scripts/run-snow-shot.sh`. On a dedicated test account, grant/revoke the
four permissions, check refresh on returning to the app, restart if macOS asks,
verify startup and tray redirection, and ensure optional microphone access does
not interrupt startup when recording microphone audio is disabled. Do not reset
TCC or change the developer's existing grants as part of automated tests.

## System Settings permission guide

Build `snow-shot-permission-guide-tests` and run only
`^snow-shot-(permission-guide|app-permissions|translation-catalog)-tests$` with the
macOS debug test preset. The guide tests inject the platform, clock, and permission
backend. They cover window selection and positioning, observer lifetime,
activation/minimization, bounded discovery, fallback copy, grants, microphone
requests, bundle URL payloads, pending drags, destruction, and live translations.
They never open System Settings or request a real grant.

`QT_QPA_PLATFORM=offscreen build/snow-shot-macos-arm64-debug/snow_shot/test-bin/snow-shot-permission-guide-tests --render`
from the repository root writes light/dark PNGs for all three languages under
`build/permission-guide-<locale>-<appearance>.png`.

Build `snow-shot-macos-permission-guide-tests` for the native fixture. Run its
bundle executable under `snow_shot/test-bin/` with `QT_QPA_PLATFORM=cocoa`. It opens
the Screen Recording pane, verifies that a background guide does not change the
frontmost application or become key, checks its bundle file URL, and reports its
existing Screen Recording/Accessibility grants without requesting either.
`--show-guidance` keeps a fake-ungranted controller available for up to three minutes;
close its guide to exit. The fixture is labeled `interactive` and excluded from
normal test presets. Start only one native fixture at a time.

Inspect first-click dragging and Escape cancellation, move/resize System Settings,
switch applications, minimize/restore, and change Spaces/displays. The guide must
follow Settings, hide while it is inactive, and return without activating Snow Shot.
It remains during an active drag and closes only after a confirmed grant or dismissal.
Use a dedicated test account to verify accepted drops into Screen Recording,
Accessibility, and Input Monitoring, and the Microphone request/denied/restricted
paths. Do not reset TCC or change the developer's existing grants. A drop by itself
must not show success; some macOS grants remain unobservable until restart.

Build `snow-shot-permission-guide-benchmark` with
`snow-shot-macos-arm64-performance` only. Run its `.app/Contents/MacOS/` executable
under that preset's `snow_shot/` directory. It opens System Settings and measures
200 native discovery/placement samples. Discovery caches the Settings process and
window ID; steady tracking reads only that window's metadata, without screenshots
or AX calls. Foreground tracking runs at 100 ms, minimized/fallback discovery at
one second, and inactive/dismissed guides do not poll window geometry. Workspace
notifications resume suspended discovery. Screen coordinates are logical points;
no Retina pixel multiplier is applied to `kCGWindowBounds`.
