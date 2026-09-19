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
