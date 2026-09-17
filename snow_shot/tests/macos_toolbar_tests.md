# macOS toolbar compatibility

The screenshot drawing, pinned drawing, and recording toolbars share the floating
palette window. On macOS their logical size depends only on the normal/small
setting (1.0/0.8). Display DPR still controls raster resolution. The Windows
physical-size controller is not installed on macOS.

Build the affected targets with the provisioned Qt 6.11.1 kit:

```sh
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-floating-toolbar-drag-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-pinned-window-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-screenshot-tool-palette-tests
scripts/build.sh snow-shot-macos-arm64-performance --target snow-shot-screen-recording-controller-tests
ctest --preset test-snow-shot-macos-arm64-performance \
  -R '^snow-shot-(macos-toolbar-logical-[12]x|pinned-toolbar-(parity|layout)|interrupted-toolbar-drag|floating-toolbar-(keyboard-focus|action-layout|recording-hit-test)|toolbar-(popup-recovery|color-input|shared-controls)|screen-recording-(toolbar|controller))-tests$'
```

The logical-size tests use separate offscreen processes at scale factors 1 and 2,
and inject toolbar DPR changes to exercise transitions during dragging and reuse.
They check control-scale contexts, anchors, lazily created styles, masks, ownership,
and native-surface recreation. Injected DPR events do not emulate the Cocoa
compositor or a real display transition.

Run native checks explicitly; normal presets exclude interactive cases:

```sh
ctest --test-dir build/snow-shot-macos-arm64-performance \
  -R '^snow-shot-macos-toolbar-(native|focus)-tests$' --output-on-failure
```

These use real Cocoa windows and isolated settings. The focus case checks that
toolbar editors actually receive keyboard focus, including searchable selects.
The popup-recovery fixture intentionally runs offscreen: its `QCursor::setPos()`
hover injection may not move the system pointer in a macOS automation session.
Validate native hover with a physical pointer rather than treating that fixture's
pointer-injection failure as a popup-rendering failure.

For hardware validation, use the application on both Retina and non-Retina displays:

- Open each toolbar with normal and small sizing. Drag between displays in both
  directions; logical button, text, row, and gap sizes must remain unchanged.
- Open color pickers and searchable font selectors, type in watermark and serial
  number fields, dismiss popovers, and continue drawing. Popovers must remain
  aligned and focus must return to the owner when editing ends.
- Click unused backing-window space outside the visible panels and their shadow
  margins. It must not block the canvas or another application; shadows must remain visible.
- Switch to another application with an active pinned or recording toolbar. The
  toolbar must remain visible; explicit hide/close and recording-area interaction
  must still hide it. Reopen capture on the other display and check its first frame.
- Expand recording export settings and drawing styles near screen edges. All
  visible rows must fit when the screen is large enough, and manual placement must
  survive tool changes.

Keep the existing Windows mixed-DPI cases unchanged. A single-display macOS run
does not qualify physical Retina/non-Retina transitions.
