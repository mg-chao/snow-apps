# Recording toolbar across displays

Build the existing controller fixture:

```powershell
./scripts/build.ps1 -Preset windows-msvc-debug -Target snow-shot-screen-recording-controller-tests
```

Run only the two native display cases (the default test preset excludes interactive tests):

```powershell
ctest --test-dir build/windows-msvc-debug -C Debug -R '^snow-shot-screen-recording-toolbar-(ready-)?display-tests$' --output-on-failure
```

Requirements: a Windows desktop with display A at 150% and display B at 100%,
with B's right physical edge adjacent to A's left physical edge and overlapping
vertical bounds. Names and primary-display status do not matter. Missing topology
returns CTest skip code 77; it does not count as a successful reproduction.

Both cases use the real recording controller, area, toolbar, Qt Windows surfaces,
and native DPI changes. The controller fixture substitutes the capture backend and
uses temporary settings, so these tests do not capture desktop video or export media.

- `snow-shot-screen-recording-toolbar-display-tests` starts capture on A first.
- `snow-shot-screen-recording-toolbar-ready-display-tests` opens the recording tool
  on A but leaves capture idle, isolating the toolbar transition in the currently
  supported area-editing state.

Each case opens Export Settings to enable region editing and verifies native
`HTCAPTION` hit-testing before attempting movement. It replays `WM_ENTERSIZEMOVE`,
`SetWindowPos`, and `WM_EXITSIZEMOVE` on the real area window. This deterministically
exercises the native interaction handlers and Windows DPI transition without mouse
input injection; it is not an end-to-end test of the operating system's mouse drag loop.

Assertions cover the source 144-DPI window, balanced interaction notifications,
toolbar hiding during movement and reappearance on release, target 96-DPI native and
Qt surfaces, native/Qt client-size agreement, toolbar row and child-control bounds,
containment on B, and alignment below the final area with a four-DIP gap. Target
checks run immediately after release and again after queued layout events.

The rendering assertion captures the real composited desktop pixels with GDI
**before** calling `QWidget::render`, which could repaint and conceal a stale backing
store. It then compares opaque pixels against a fresh render at the window's current
DPI. Transparent background, shadows, and blended edges are excluded. RGB channel
differences up to 40 are tolerated, and at most 2% of opaque reference pixels may
exceed that threshold to accommodate minor font/animation differences. The same
assertion must pass on A before the move, validating the capture and comparison path.
This checks presented content against the widget's intended rendering; it is not a
golden-image test for defects shared by both render paths.

The test saves `source-desktop.png`, `source-render.png`, `destination-desktop.png`,
and `destination-render.png` under
`build/windows-msvc-debug/snow_shot/toolbar-display-artifacts/`. The active-capture
case uses an `active-` filename prefix. Run on an unobstructed interactive desktop;
another window covering the toolbar will also cause the compositor comparison to fail.

The test does not invoke toolbar placement or repair methods after the move. The
production controller must restore and position the toolbar itself. Failures remain
ordinary failing assertions; neither test uses `WILL_FAIL`.

Observed on 2026-09-09 with A `(0,0,3840,2160)` at 150% and B
`(-3840,0,3840,2160)` at 100%:

- Active capture fails the drag prerequisite. The current area implementation
  permits region editing only while idle, even when its requested input mode is
  `RegionEditing`. Post-drag toolbar checks cannot run in this state.
- The idle/ready case **fails the destination rendering assertion**, reproducing the
  enlarged content clipped at the right and bottom edges in the reported screenshot.
  Native DPI is 96, Qt DPR is 1.0, and both the native client and Qt window are
  1242 by 142 pixels; all geometry assertions pass. The source comparison has zero
  mismatches among 139,233 opaque pixels. After the move, 32,681 of 62,932 opaque
  reference pixels differ (about 52%). The desktop image is clipped while the fresh
  target-DPI render shows the complete toolbar. The earlier geometry-only version
  passed because it never inspected the pixels actually presented by Windows.

## Fix and verification (2026-09-09)

The ready case reproduced the same 32,681 mismatches before the fix. Diagnostic
inspection found that the backing store still contained the 1863-by-213 source
image even though the native client had become 1242 by 142. Updating the row widgets
and invalidating their graphics effects did not repair it. A full repaint after
the DPI transaction completed reduced destination mismatches to zero.

The shared floating toolbar deliberately disables painting while reconciling its
native frame and control scale. That ordering remains necessary to avoid painting
against an intermediate frame. Qt's `QWidgetWindow::scheduleRepaint` skips hidden
windows or windows with updates disabled, and a later `update()` can coalesce with
already pending dirty state. The transaction now calls `repaint()` after restoring
updates in its existing queued completion callback. This completes the backing-store
repaint at the final scale; it requires no geometry adjustment, extra timer, surface
recreation, or test-side repair.

This is a local presentation-transaction defect in the shared floating toolbar.
The ready native display regression passes with zero mismatches at both source
and destination. The offscreen DPI frame test also covers a commit with unchanged
logical dimensions, both while visible and when hidden then shown before completion.
The related recording controller and screenshot capture-screen-switch tests pass.
The active-capture case retains its separate, pre-existing drag prerequisite failure;
the fix does not enable editing the capture area during recording.

Geometry-only assertions missed the defect because all client and control bounds
were correct. Keep the desktop comparison before `QWidget::render` as a regression
constraint on actual presented pixels. Existing workspace changes were preserved.
