# macOS cursor coordination

Screenshot, recognition, recording and pinned-image windows share one cursor
coordinator. Qt selects the cursor (including child widgets, custom bitmap DPI
and application overrides); the coordinator decides which window may apply it
without changing keyboard focus. Transient ownership is evaluated at update
time so pooled controls follow their current owner.

## Native compatibility boundary

The repository's current Qt kit is exactly pinned to Qt 6.12.0.
`QCocoaWindow::setWindowCursor` caches the converted cursor on its native view,
but gates immediate application on key-window or titled-utility status. The
adapter calls the public `NSResponder::cursorUpdate:` callback to apply that
cache after Qt has handled the event. It does not access Qt's private fields,
convert cursors itself, or alter the responder chain or window activation.

The public callback's use of Qt's cache is an implementation dependency. Run
the native contract test when upgrading Qt or macOS; the offscreen test alone
cannot qualify it.

AppKit can also reset the cursor internally during a later display cycle without
delivering an `NSEvent`. A native run-loop observer compares the current cursor
at the idle boundary after AppKit's work. Only a change invalidates the
coordinator; its own updates advance the snapshot immediately. This uses no
timer, performs no idle window hit-testing, and routes reconciliation through
the same activation, popup, modal, transparency and drag-ownership checks.
The observer and retained snapshot are released with the coordinator.

## Build integration

Consumers call `snow_shot_link_macos_window_platform(target)` next to their target
declaration. The static `snow_shot_macos_window_platform` target compiles the
native adapters once and owns their cursor dependency. Do not compile the
adapter sources separately or infer linkage by scanning target source filenames.

## Focused validation

With the repository's macOS build tools on PATH:

```sh
cmake --build build/snow-shot-macos-arm64-debug --target \
  snow-shot-macos-cursor-tests snow-shot-macos-cursor-native-tests
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-macos-cursor(-native)?-tests$'
```

The offscreen test covers coalescing, window pooling, current transient ownership,
transparency, modal suspension, explicit/implicit grabs, activation, native
surface recreation, destruction and cursor overrides.

The native test requires an unlocked desktop and existing event-posting
permission. It exercises real non-key windows, focus preservation, native and
non-native child cursors, custom bitmap samples and logical size, stationary
display-cycle reconciliation, nested override restoration, click-through,
occlusion, drags and every connected display. It uses elevated fixture windows
and restores the pointer and previous application afterward. Pin adapter state
changes restore production stacking, so the test reapplies its isolation level
before checking overlapping click-through windows. Bitmap checks compare source
samples rather than display-dependent color conversions.

For a manual negative control, run the native executable with
`-platform cocoa --without-coordinator`. It must fail at the initial non-key
cursor-change assertion. A permission skip, focus failure or hover setup failure
is not a successful negative control.
