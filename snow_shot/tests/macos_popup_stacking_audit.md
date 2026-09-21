# macOS toolbar popup stacking audit

## Cause and correction

Qt 6.11.1's Cocoa backend gives `Qt::Tool | WindowStaysOnTopHint`
windows `NSModalPanelWindowLevel`. Its transient-parent handling raises a tool to
at least its owner's level but does not establish native child-window ordering.
The pinned editing toolbar and its AdQt popup can consequently occupy the same
level. Raising the toolbar during placement or activation can cover the popup.
The screenshot toolbar already has an explicit native layer policy that places
transient descendants above their owners, explaining the different behavior.

AdQt's shared `syncTopLevelToolTransientParent()` now also establishes Cocoa
child ownership for visible tool popups. AppKit keeps each popup above its owner
when either is raised. The relationship follows Qt transient-owner changes and
is removed on hide, native-surface release, and destruction; retained widgets can
recreate their native surfaces. No additional global window level is introduced.
The screenshot layer policy remains authoritative for screenshot windows.

## Related paths inspected

| Component | Stacking path and result |
| --- | --- |
| Pinned and screenshot main/sub-tool popovers | Shared overlay popup controller and tool ownership helper; covered by the fix. |
| Recording cursor/settings popovers | Same palette/popover path; covered by the shared correction. |
| Toolbar color pickers | AdColorPicker uses AdPopover; native regression verifies its surface. |
| Toolbar font/template selectors, translation/model selectors | AdSelect calls the same tool ownership helper; native regression verifies a select popup. |
| Popup option tooltips | TopLevelTransient tooltips use the overlay controller and shared helper; native ownership and ordering verified. |
| Nested tool popups | Each popup owns its native child; tested after raising both the outer owner and the parent popup. |
| Screenshot selection editors, recognition windows, and modals | Existing screenshot layer policy; existing native suite passes, with an added real AdPopover compatibility check. |
| Context menus and native file panels | Separate Qt platform-menu / Cocoa panel paths; not users of the affected QtTool popup helper. No changes made. |
| Pinned click-through controls, hide-to-top handles, recording area, color sampler | Independent application tool windows, not AdQt popup surfaces. Their ownership/level policies remain separate. |

This audit concerns popup ordering and ownership, not every focus, input, or
multi-monitor interaction of these components.

## Verification

The WindowServer-order regression fails with the new native-ownership call
removed and passes with it enabled. It verifies real ordering, not just equal
window levels or `QWidget::isVisible()`. Coverage includes nested surfaces,
hide/show reuse, changing and clearing the transient owner, native recreation,
and actual popover/select/color-picker/tooltip widgets. An autorelease pool
exercises Cocoa teardown. Offscreen coverage checks Qt ownership and lifecycle.

Focused Debug CTest entries passed:

- `adqt-popup-ownership-tests` (offscreen).
- `adqt-popup-stacking-native-tests` (Cocoa).
- `adqt-qt-tool-popup-tests` (offscreen).
- `snow-shot-macos-overlay-initialization-tests` (offscreen).
- `snow-shot-macos-screenshot-stacking-native-tests` (Cocoa).

No full suite was run. This build has clang-tidy disabled.
