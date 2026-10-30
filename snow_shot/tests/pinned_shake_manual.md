# Shake pinned windows

The switch lives in **Settings > Pin to screen > Interaction** and defaults on.
A shake is six alternating legs of at least 48 logical pixels on either axis,
with five reversals, a maximum of 250 ms per leg, and 700 ms for the sequence.
This requires three rapid round trips; short back-and-forth adjustments and two
broad round trips must leave other pinned windows visible.
Small counter-movements are absorbed into the current turning point. Recognition
is independent of the pointer event rate and latches until the drag ends.

On Windows, the detector retains the setting chosen at the pointer press and
rebases when USER32 actually enters its move loop. Time spent waiting for Qt's
posted move request is not part of the first gesture leg. A native DPI change
discards incomplete recognition immediately; an already recognized shake remains
latched. The native regression covers both transitions.
Native movement and detection share the application's physical-cursor provider,
which uses [physical coordinates](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getphysicalcursorpos)
for the physical placement contract.

## Native acceptance

On Windows and macOS, use at least two pins in the active group and another group
with saved pins. Include a text pin, an animated image, a thumbnail, and a pin
hidden to the top edge among the peers.

1. Drag one pin rapidly back and forth horizontally, vertically, and diagonally.
   Peers disappear during the drag, and the grabbed pin shows **Other windows
   hidden.** for three seconds. Continue dragging: capture, position, focus, and
   cursor behavior must remain correct. Additional shaking during the same drag
   does nothing.
2. Use Show All. Hidden peers return with their contents, placement, and modes
   intact. Other groups and closed history must not change. A second shake hides
   peers again; it does not restore them.
3. Ordinary dragging, small jitter, slow zigzags, resizing, drawing, selecting
   text, exporting by drag, keyboard cursor movement, and dragging a multi-window
   selection must not hide peers. The existing duplicate-content attention
   animation must not trigger the gesture.
4. Disable the switch with pins already open. Shaking stops immediately. Enabling
   during an existing drag requires a new press. Interaction reset enables it;
   appearance reset preserves the preference. Restart to confirm persistence.
5. Cancel an incomplete shake with Escape or interrupt capture. It must not carry
   into the next drag. Cancellation before a queued hide executes discards it;
   normal release after recognition preserves it. Cancellation after peers have
   disappeared does not restore them.
6. On mixed-DPI monitors (Windows) or Retina/non-Retina displays (macOS), move a
   pin across the display boundary during an incomplete gesture. Crossing must
   not trigger hiding; a fresh complete shake on either display must work at the
   same logical amplitude. Check fractional Windows scaling as well.
7. Switch languages while the success message is open. The text must update in
   place without extending its deadline. Check a small pin and ensure the message
   remains scoped to its owner without taking keyboard focus.

macOS native checks require a macOS host; offscreen tests exercise the shared
controlled interaction path but do not replace native capture/display validation.
The available Windows setup did not meet the existing cross-DPI test's required
higher-DPI monitor to the right of a lower-DPI monitor. Deterministic tests cover
the DPI-event reset and logical/physical coordinate normalization.

## Focused checks

Build the pinned-window, settings-catalog, settings-page-loading, and detector
test targets in `windows-msvc-debug`, then run:

```powershell
ctest --preset test-windows-msvc-debug -R '^snow-shot-pinned-shake-(detector|window|settings|settings-catalog)-tests$' --output-on-failure
ctest --test-dir build/windows-msvc-debug -C Debug -R '^snow-shot-pinned-shake-native-tests$' --output-on-failure
```

For performance, build `snow-shot-pinned-shake-performance-benchmark` and
`snow-shot-pinned-window-tests` with **windows-msvc-performance / Release**.
Run only the two shake benchmarks:

```powershell
ctest --test-dir build/windows-msvc-performance -C Release -R '^snow-shot-pinned-shake-(drag-)?performance-benchmark$' -V
```

The detector benchmark checks C++ allocation calls during sampling and reports
fixed state size and median/worst trial time. The drag benchmark measures the
actual enabled and disabled sampling entry point after arming a window, excluding
setup, rendering, and the one-time hide operation. It intentionally has no
machine-dependent timing assertion. Sampling must not enumerate windows, persist
state, allocate memory, or use a polling timer.

Local Windows x64 measurements before the sensitivity adjustment on 2026-10-30,
using that Release preset:

| Measurement | Result |
| --- | --- |
| Detector state | 112 bytes |
| Detector sampling, median / worst trial | 4.15 / 4.21 ns |
| C++ allocations during detector sampling | 0 |
| Disabled drag sampling, median / worst trial | 0.66 / 0.67 ns |
| Enabled drag sampling, median / worst trial | 30.02 / 30.20 ns |
| Added median drag sampling cost | 29.36 ns |

The detector uses eleven trials of two million mixed samples; the window sampling
measurement uses nine trials of 250,000 ordinary movement samples per setting.
These are local observations, not portable timing guarantees.

With six 48-pixel legs, the detector benchmark uses 64-pixel movements and
resets every 40 samples so each drag can complete three round trips. With that
workload, the Release detector benchmark measured 4.87 / 5.10 ns per sample
(median / worst trial), a 112-byte state, and zero C++ allocations during sampling.

The existing real-mouse keyboard movement check reported one initial two-pixel
startup offset. Subsequent comparisons passed, including three consecutive runs
with shaking enabled and three with it disabled. The expected keyboard positions
and assertions remain unchanged. Its optional `--shake-disabled`
argument supports repeating that comparison independently of the gesture tests.

Original implementation validation passed seven focused unit CTest entries, the
Windows capture-loss and real-mouse keyboard regressions, and the new native gesture checks in Debug
and Release. The new offscreen and native checks also passed three consecutive
runs each. All three translation catalogs compile, and the 21 translation-tooling
tests, canonical catalog validation, changed-range formatting, and whitespace
checks passed. The full suite was not run.

Sensitivity adjustment validation passed the focused detector, offscreen window,
and Windows native gesture CTest entries in Debug, plus the detector benchmark in
Release. The new regression fails with the original 28-pixel/four-leg thresholds
and passes with the revised thresholds. It covers repeated short corrections,
two broad round trips, slow adjustments, and intentional shaking; the window
checks also cover cancellation, settings changes, DPI reset, peer content, and
continued dragging inside the real Windows move loop. Formatting and whitespace
checks passed. The full suite was not run.

After restoring three round trips, the detector, offscreen window, and Release
detector benchmark checks passed. The first Windows native run hid the peer and
showed feedback inside the move loop but failed its continued-dragging check;
an immediate diagnostic rerun passed.
