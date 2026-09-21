# macOS recording acceptance

Recording requires macOS 15+. The Qt controller uses the existing opaque C session
API; a native worker owns capture, effects, audio, encoding and destruction.
The UI and capture region use desktop points. Output sizing resolves the native
desktop transform (the highest intersecting display density), then applies the
orientation-aware quality cap and output-format alignment. Odd capture dimensions
and negative origins are retained; output dimensions are fixed at startup.

## Focused checks

Use the toolchain described in `../../docs-macos-build.md`:

```sh
scripts/build.sh snow-shot-macos-arm64-debug --target snow_shot
cmake --build build/snow-shot-macos-arm64-debug --parallel 8 --target \
  snow-shot-screen-recording-controller-tests \
  snow-shot-screen-recording-area-window-tests \
  snow-shot-screen-recording-shortcut-tests \
  snow-shot-screen-recording-geometry-tests \
  snow-shot-screenshot-recording-workflow-tests snow-shot-app-permissions-tests
ctest --test-dir build/snow-shot-macos-arm64-debug --output-on-failure \
  -R '^snow-shot-(app-permissions|screen-recording-(controller|area-window|geometry|shortcut)|screenshot-recording-workflow|recording-effects-preview|recording-capture-exclusion|macos-recording-capture-exclusion)-tests$'

export FFMPEG_DIR="$PWD/.tools/macos/installed/arm64-osx-snow-shot"
export DYLD_LIBRARY_PATH="$FFMPEG_DIR/lib"
cargo test --manifest-path snow-crates/Cargo.toml \
  -p snow-recording-c -p snow-recording-runtime -p snow-recording-effects --lib
cargo test --manifest-path snow-crates/Cargo.toml \
  -p snow-recording-export --lib streaming::tests::
cargo test --manifest-path snow-crates/Cargo.toml -p snow-macos --lib desktop::tests
cargo clippy --manifest-path snow-crates/Cargo.toml \
  -p snow-recording-c -p snow-recording-runtime -p snow-recording-effects \
  -p snow-recording-export --all-targets -- -D warnings
```

The controller fixtures cover deferred startup, permission denial, cancellation,
countdown, pause/resume, stop failure, clipboard publication, exclusions, and stale
asynchronous native sizing. Controlled worker barriers also verify that controller
destruction neither blocks the GUI thread nor releases a finalizing session early. Area tests cover logical dragging, resizing, odd sizes,
negative origins, annotation persistence and countdown placement. The Cocoa shortcut
test checks native click-through, drawing input, stacking and fullscreen Space
policies through repeated show/hide cycles. Native desktop geometry tests exercise
mixed display density without requiring multiple physical displays.

## Native export probe

Deploy with the normal bundle installer, then use the diagnostic entry point:

```sh
cmake --install build/snow-shot-macos-arm64-debug --component SnowShot \
  --prefix "$PWD/build/snow-shot-macos-arm64-debug/run"
build/snow-shot-macos-arm64-debug/run/snow_shot.app/Contents/MacOS/snow_shot \
  --recording-macos-probe "$PWD/build/recording.mp4" system-audio effects
```

The probe uses the ordinary C API and a Cocoa event loop, records a 321×239-point
region, pauses for 300 ms between two 800 ms active intervals, and finalizes.
Choose `.gif`, `.apng`, or `.webp` to exercise animation export. Optional arguments
are `system-audio`, `microphone`, `effects`, `software`, and `hevc`. Omit audio
arguments for animations. The probe does not modify application settings.
Check decoded dimensions, duration, codec, every animation frame and loop metadata;
play a known sound throughout startup and recording to distinguish silence from
failed system-audio capture.

## Coverage on 2026-09-22

Hardware: Apple M4 Mac mini, one 1920×1080 display at 1× scale.

The arm64 debug application build and bundle deployment passed. All nine focused
CTest checks passed, including the Cocoa checks. After the shutdown change, all
four controller/exclusion checks passed again. Rust checks passed: 29 C facade,
100 recording runtime, 41 effects, 22 streaming-export tests (one existing ignored),
and five native desktop geometry tests. Clippy, formatting, translation extraction
and catalog completeness checks passed. The full test suite was not run.

- MP4 hardware H.264, software H.264 and hardware HEVC export completed. On this
  display MP4 output is 320×238; the capture region remains 321×239 points.
- GIF, APNG and WebP exported at 321×239. Pillow decoded every frame and confirmed
  infinite-loop metadata in moving-content probes. A static WebP may collapse to
  one frame. The deployed bundle also exported and decoded all four formats.
  FFmpeg's GIF decoder reported LZW errors for a GIF that
  Pillow decoded completely; use an independent decoder when checking GIF output.
- System audio produced an AAC stream with a nonzero decoded signal from a known
  system sound. A pause/resume probe produced approximately 1.67 s of output,
  excluding its 300 ms pause. Effects and cursor-highlight probes completed.
- No microphone input device is installed on this machine. Requested microphone
  recording correctly failed with device unavailable; audible microphone capture
  and combined-source synchronization remain unverified.
- Physical Retina/mixed-density displays, Intel hardware and Windows execution are
  unavailable. Fullscreen application switching, visual effect/layout comparison,
  long recordings, and actual input delivery to another application's controls
  still need interactive acceptance. Native window-policy assertions do not replace
  those checks. Release DMG/notarization acceptance is separate from the debug app.

Manual acceptance should also draw during recording, change toolbar settings before
start, verify the toolbar stays excluded, copy each format to the clipboard, open
the recording folder, repeat start/stop, and revoke optional input permissions to
confirm basic recording remains usable when those effects are disabled.
