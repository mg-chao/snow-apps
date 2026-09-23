# macOS Smart selection

The shared Rust C service runs foreground and refinement workers. macOS workers
reconstruct Accessibility references locally from an immutable Quartz window and
display snapshot; references never move between workers. Windows retains its
UIA/MSAA providers. Queued refreshes and queries coalesce, and superseded requests
still receive terminal delivery. Closing the service shuts off callbacks before
asynchronous worker cleanup.

Quartz windows are cached front to back. Desktop elements, invalid or transparent
windows, Dock-owned surfaces at the Dock level, and excluded `CGWindowID`s are
removed before hit testing. The Dock's Quartz rectangle can cover the entire display even
when its visible content does not; excluding that owner/level combination prevents
it from masking application windows and their Accessibility children. Menus and floating panels
remain selectable, including other applications at the Dock level. Ownership is
identified through the owner PID and system executable path, not the localized
Quartz owner name. Qt's `WId` on macOS is an `NSView` pointer and must not be used
as the exclusion ID.
When no eligible window contains the pointer (including the Dock area), both
window and element selection return the full queried display in capture pixels.
The display ID keeps this fallback local to one screen on mixed-scale desktops.

Coordinates use the capture contract: a display's Quartz desktop origin plus
pixel offsets inside that display. Backing scale comes from the display mode,
including Retina and rotated modes. A query carries its display ID because these
physical rectangles can overlap on mixed-scale desktops. Selection is clipped to
the queried display and the cached window; output edges round outward. The same
display ID accompanies initial and refinement results through canvas mapping.
A refresh is required after a display layout change.

The read-only smart-selection toolbar and manual-selection editing toolbar both
show canvas units (points on macOS), matching wheel increments, the resize dialog,
and selection effects. Unit descriptions retranslate on language changes; `px` and
`pt` remain abbreviations in each catalog.

Accessibility hit testing targets the selected window's application, then verifies
the owning PID and AX window bounds before publishing any children. Ancestors are
bounded to 64 steps and 48 rectangles, with duplicate and cycle detection. Each
provider call receives the remaining deadline, capped at 168 ms for foreground
and 500 ms for refinement (1.5 s total). Cancellation and provider errors retain
a window fallback. A moved window or unsupported provider safely falls back to
the cached window until the next refresh.

Permission checks during capture never prompt. Missing Accessibility permission
returns `PERMISSION_REQUIRED` with `ok=1` and the window rectangle; Qt applies that
rectangle, skips refinement, and warns once per capture session. Function settings
→ Screenshot shows the permission status, an explicit request action, a separate
System Settings action, and Retry. Revocation during refinement also replaces the
child path with the window fallback, unless the user has already pressed or
confirmed a selection.
The old Window Element API configuration remains readable but is ignored on macOS.

Run only the related tests:

```sh
cargo test --manifest-path snow-crates/Cargo.toml -p snow-ui-selector -p snow-ui-selector-c
ctest --preset test-snow-shot-macos-arm64-debug \
  -R 'snow-shot-(selector-policy|selector-phased|window-element-api|smart-selection-permission|settings-catalog|selection-geometry|capture-workflow)-tests'
ctest --preset test-snow-shot-macos-arm64-debug \
  -R '^snow-shot-macos-smart-selection-smoke$' -LE '^$'
```

Build those CTest targets first with the macOS debug preset. The native smoke test
uses a visible Cocoa fixture, verifies physical window bounds, child traversal,
Retina geometry, and excluded overlays. Without Accessibility permission it checks
window fallback before returning skip code 77. It also skips when there is no
active desktop. It never requests or changes TCC permission. Grant permission to
the test executable manually to exercise native AX traversal; deterministic tests
cover denied permission without modifying TCC. Use the x64 presets on Intel Macs.
