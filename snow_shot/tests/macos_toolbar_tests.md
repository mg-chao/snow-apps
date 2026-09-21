# macOS toolbar compatibility

The screenshot drawing, pinned drawing, and recording toolbars share the floating
palette window. On macOS their logical size depends only on the normal/small
setting (1.0/0.8). Display DPR still controls raster resolution. The Windows
physical-size controller is not installed on macOS. Both platforms use the fixed
1242 × 142 frame preset, scaled by the toolbar size setting. On macOS a panel mask
excludes the unused native frame from input routing, while Cocoa draws the shadow.

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
- Click shadow margins, transparent rounded corners, and unused backing-window
  space. They must not block the canvas or another application; shadows must remain visible.
- Switch to another application with an active pinned or recording toolbar. The
  toolbar must remain visible; explicit hide/close and recording-area interaction
  must still hide it. Reopen capture on the other display and check its first frame.
- Expand recording export settings and drawing styles near screen edges. All
  visible rows must fit when the screen is large enough, and manual placement must
  survive tool changes.

Keep the existing Windows mixed-DPI cases unchanged. A single-display macOS run
does not qualify physical Retina/non-Retina transitions.

## Shadow click-through regression

Cocoa treats painted shadows as native window content. The popover's 24-point
bottom shadow margin overlaps toolbar triggers, and the toolbar's earlier mask
explicitly expanded each panel to include its shadow. Bubble-only hover tests and
Windows `WM_NCHITTEST` handling do not change macOS event routing.

Native edge-click coverage also exposed a separate hit-test mismatch: `AdButton`
paints its own full-size surface but inherited `QPushButton::hitButton()`, which
uses the platform's `SE_PushButtonBevel`. The macOS bevel excludes painted edges.
Non-circular custom buttons now test their own widget bounds; circles retain their
shape-aware hit test. A deterministic offscreen proxy style with an inset bevel
reproduces the original miss and verifies edge clicks, circle corners, and disabled
buttons independently of the machine's style.

The earlier investigation inferred that QWidget masks could not provide native
click-through from Qt's masked-event responder path. That inference was too broad:
real window-server clicks on an oversized toolbar reached the underlying canvas
when the panel mask excluded the reserve, with native shadows enabled or disabled.
Removing the mask blocked those clicks. The earlier content-fitting assertion
checked a chosen window-size policy, not whether clicks reached their destination.
Floating toolbars therefore keep the shared fixed frame and panel mask. Top-level
macOS popups retain their separate zero-shadow-margin layout.
Toolbar panels no longer paint a QGraphicsDropShadowEffect on macOS. Shadows are
rendered by Cocoa outside the masked panel surfaces; Qt window flags own their visibility,
including after recreation. The shared native helper only invalidates the shadow.

Popup hover ownership follows the precise bubble and arrow paths. Native masks
instead cover their rectangular painted bounds, including border coverage and
one mask cell for fractional-DPR rounding, so Cocoa cannot clip antialiased
corners or arrows. These masks still exclude the
unused arrow gutters; removing the mask entirely regresses native click delivery
there. Toolbar masks expand the rounded surface paths for antialiased coverage before
rounding their coordinates to integer mask cells. This excludes fully transparent
corners as well as row gaps and unused frame space. A rectangular panel mask can
still intercept clicks at an alpha-zero corner after switching row arrangements;
repainting does not correct its input shape. In-window popups and other
platforms retain their painted shadows.

Focused checks (Debug or performance builds):

```sh
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^(adqt-popup-input-(shape|native)|adqt-popup-hover-recovery|snow-shot-macos-toolbar-(logical-[12]x|shadow-input)|snow-shot-toolbar-(stable-tool-frame|popup-recovery|popover-lifecycle))-tests$'
```

The offscreen tests cover hover ownership, unclipped popup rendering at 1x, 1.5x,
and 2x DPR, all four arrow sides, normal/small sizing, native recreation, and
top-level/child transitions. The rendering regression compares the full painted
surface with its unmasked reference, including antialiased corners and arrows;
it fails with the old integer popup mask.

The interactive tests require Accessibility event-posting access. They post real
mouse movement and press/release events through the window server, then check
button activation. The popup fixture verifies both the underlying button and a
popup option, including clicks through transparent corners and arrow gutters on
all four sides after native recreation. The toolbar fixture opens real grouped-button
popovers by hover, clicks trigger edges and centers, and checks shadow/corner
click-through to the underlying canvas at both sizes and after recreation. It also
switches selection, barcode, and drawing rows in both row arrangements, clicks the
unused native reserve with native shadows enabled, and verifies that toolbar
buttons still receive their clicks. It uses
production owner-window relationships and screenshot window stacking configuration,
without artificially raising the popup. It asserts
that the popup native frame does not overlap its trigger and restores the pointer
when done.

The stable-frame test checks the main row and native window geometry while
switching secondary rows, including barcode recognition with no secondary row. It
records Move and Resize events, so returning to the right final position does not
hide an intermediate native geometry change. The same check runs offscreen and
inside the native toolbar geometry fixture. Main-row anchoring is applied within
the geometry commit rather than repaired by a second move.

A negative control restoring the old popup shadow margins fails at the
trigger-overlap assertion.
The earlier window-number query and mask-only fixtures were insufficient evidence
of click delivery; native fixtures now verify complete clicks at their destinations.
These checks do not replace physical-pointer validation on the affected machine.
