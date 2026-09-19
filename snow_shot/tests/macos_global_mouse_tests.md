# macOS global mouse validation

The deterministic `snow-shot-macos-global-mouse-tests` executable uses private Quartz events and
an injected native API. It never installs a system tap, posts system input, requests permissions,
or opens System Settings. It covers all 15 nonempty modifier chords and five buttons, ownership,
synthetic input rejection, modifier masking, permission failures, session loss, tap recovery,
and restart. `snow-shot-global-mouse-tests` covers shared matching, capture buffering, pacing,
and stale callbacks. Settings, schema, and mixed-display geometry have separate targeted tests.

Build using the macOS debug preset, then run only the affected tests:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-(global-mouse-tests|macos-global-mouse-tests|global-mouse-page-tests|global-mouse-storage-tests|global-mouse-settings-runtime-tests|selection-geometry-tests)$'
```

The opt-in Cocoa test uses the real event tap:

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-global-mouse-native-smoke-tests$'
```

This test reports missing Input Monitoring or Accessibility access and exits with CTest skip
code 77 without requesting either permission. With both granted it creates the production tap,
then creates a real tap through the injected API and posts a tagged press/move/release at the
current cursor position. A test-only adapter recognizes that tag and presents the events to the
production translator as physical input; ordinary synthetic input remains rejected. The test
checks the complete Begin/Update/Finish sequence. It does not test hardware-driver behavior.
Run it with the mouse idle; it is labeled `interactive;macos` and excluded from default presets.

For hardware acceptance, launch Snow Shot using Cocoa, open Global mouse settings, and use its
permission action to grant Input Monitoring, then Accessibility. Retry after returning to the
app; if macOS asks to quit and reopen Snow Shot, follow that prompt. Check that the banner becomes
ready, and that missing/revoked access removes the registered appearance of configured bindings.

Exercise Command, Control, Option, and Shift (including both left/right modifier keys) with each
available mouse button. Verify exact matching, additional button press/release draining, Escape
and autorepeat cancellation, no foreground shortcut activation, and ordinary scrolling/cursor
movement. Verify a drag across Retina/non-Retina displays, including a display left of or above
the primary display. Repeat after disconnecting a display, locking/unlocking the session, sleep,
permission revocation/restoration, and application restart. None of these interruptions should
finish a capture or leave a modifier/button logically stuck in the foreground application.
