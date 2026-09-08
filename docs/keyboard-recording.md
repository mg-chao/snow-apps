# Keyboard recording

Keyboard recording is an opt-in Windows feature of Snow Shot's direct recorder. The
Export Settings button follows the cursor button. `screen_recording/show_keyboard`
defaults to `false`, persists through `RecordingSettings`, and participates in the
recording settings reset. Export options are locked while recording or paused.

## Behavior

- Record physical key presses, including ordinary typing. Do not assemble typed words
  or IME composition text. Resolve printable names using the foreground keyboard layout;
  special-key names come from Qt translations captured at session start.
- Held keys form one row. Ctrl, Shift, then S extends the same row. Holding Ctrl while
  pressing/releasing C and then V produces separate Ctrl+C and Ctrl+V rows. Auto-repeat
  does not create rows. Left/right modifiers have separate physical states and shared
  names; the synthetic Ctrl belonging to AltGr is hidden.
- Retain a held combination until its non-modifier keys are released. Modifier-only
  rows remain active until their modifiers are released. Preserve released chord labels.
- Show at most four rows, newest at bottom right. Move rows over 180 ms with cubic
  ease-out, retain released rows for 1,200 ms, then fade over 400 ms.
- Use the starting theme's elevated background at 80% opacity, text color, and a subtle
  border. At 1080p, keycaps are 80 px high with 44 px Segoe UI text, 16 px radii, 12 px key
  gaps, 16 px row gaps, and 48 px edge margins. Apply a 2× display multiplier after
  scaling by output height with the original 0.5–4 clamp;
  fit unusually wide chords to the available output width and clip safely at frame edges.
- Pause discards pending input, releases active model state, and freezes history on the
  recording clock. Resume begins a new input epoch and resynchronizes physical modifiers.
  No pre-start, paused, post-stop, or finalization input is exported.

## Implementation contract

Input correctness and recording stability take priority over rendering cost and visual
polish. The hook thread only copies observations and always forwards input. Its queue
has 256 entries; overflow increments an epoch so the consumer clears stale held state.
Hook ownership is session-scoped, and disabled sessions do not create an observer or
text renderer. Keystrokes are neither logged nor stored as a separate recording track.

The Qt controller snapshots colors and translated names into version 2 of the direct
recording C configuration. Rust validates and copies label data during creation. Version
1 callers remain supported with keyboard recording off: validation includes their ABI
tail padding, while copying excludes padding that could otherwise resemble new options.
No saved-configuration migration or third-party package addition is required.

The Rust compositor draws cached, premultiplied RGBA keycaps produced by DirectWrite and
Direct2D software rendering. Grayscale antialiasing avoids ClearType fringes. Blending
touches only keycap rectangles. History and cache retention are independent of recording
duration. Static capture still schedules motion, fade, and the final clean frame; a settled
held row does not request continuous animation frames. Startup validates the native
renderer, and initialization failure returns through the translated recording error UI.

## Focused verification

Run the recording toolbar and application storage CTest selections only. Rust unit tests
in `snow-recording-runtime` and `snow-capture-c` cover key grouping, repeats, modifiers,
overflow, native hook lifecycle, pause timing, static-frame expiration, clipping, alpha
blending, and old/new C configuration decoding. The keyboard compositor tests use an
injected deterministic rasterizer and do not require desktop capture.

The explicitly ignored
`keyboard_observer_receives_shortcut_from_isolated_native_window` test briefly focuses
its own native window and sends a tagged Ctrl+Shift+S sequence. Run that exact test with
`-- --ignored` for native input integration; it verifies foreground ownership before
sending input and restores the previous window afterward. It is excluded from ordinary
offscreen test runs.

Build `snow-shot-keyboard-recording-performance-benchmark` using
`windows-msvc-performance`. Run its `keyboard_overlay_benchmark.exe` with a new output
directory. It measures warmed four-row composition at 1080p/4K in both themes and creates
MP4, GIF, APNG, and WebP fixtures. The benchmark decodes MP4 through FFmpeg. Run
`python snow_shot/tools/verify_keyboard_recording.py <output-directory>` to independently
decode animated images through Pillow and save visual samples. GIF comparison filters
palette dithering before checking for keycap disappearance.

For a manual integration check, enable the button, record typing and shortcuts in another
application, pause while typing, resume, and stop. Confirm that the output retains the
starting theme, paused input is absent, keys combine correctly, and the button remembers
its choice. Turning the feature off restores the existing recording path.
