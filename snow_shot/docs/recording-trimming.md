# Recording trimming

`RecordingTrimSession` owns preparation, playback, and repeated exports independently of the
recording windows. Capture stops before entering preparation. Closing detaches the UI; a pending
export may finish and original media remains available. The capture rectangle stays immutable
while the preview window moves.

The 40 px secondary toolbar shares the Selection Tool's 32 px controls, 24 px icons,
and control scale. Replay uses the native Ant Design text button. The timeline uses a 4 px
rail, a soft primary selection track, primary playback progress, and a compact capped playhead.
Rounded 10 × 20 px trim grips retain separate 16 × 32 px drag targets, including for one-frame
selections. Hover, keyboard focus, and disabled states use Ant Design theme tokens; trim tooltips
show the boundary time in minutes, seconds, and milliseconds. The selection is a
half-open interval of presentation frames. End scrubbing displays the last included frame. The
Rust `TrimRange` converts integer microseconds to rational frame and audio sample boundaries.

`ClipSource` retains either finalized media or a deferred recording bundle. Deferred preview reads
only original media; export reconstructs source-time input effects and rebases progress/time
overlays to the selected duration. Audio tracks stay separate in exports and are mixed for preview.
Full-range finalized Copy reuses the original file; Save copies its bytes. Other exports decode and encode the selected
interval with the recorded settings. Publication uses temporary files and never replaces a source.
The editor caches the latest successful range for repeated Copy or Save.

A single playback worker owns FFmpeg and the native audio sink. Seek commands coalesce, revision
numbers reject stale frames, and cancellation interrupts forward decoding. Hardware frames stay on
the GPU until the requested presentation frame needs CPU pixels. The worker keeps current/lookahead
video frames, one published RGBA frame, reusable conversion buffers, and a bounded device audio
queue. A missing or stalled audio device falls back to the silent playback clock. Frame leases can
outlive the clip handle. Clip opening, seeking, encoding, audio, and resource teardown never join on
the GUI thread.

## Focused validation

Build the application and the two recording test executables, then run:

```powershell
ctest --preset test-windows-msvc-debug -R "^(snow-shot-recording-trimming-tests|snow-shot-screen-recording-area-window-tests|snow-shot-recording-post-processing-tests|snow-shot-recording-finalization-close-tests)$"
```

Set `SNOW_TRIM_VISUAL_OUTPUT` to a directory to render the actual trim toolbar in light/dark themes
at 100%, 125%, 150%, and 200% control scales, plus the complete main and secondary rows. The trimming test includes live translation,
keyboard/scrub constraints, immutable geometry, Save cancellation/publication, failure/retry,
repeated Copy, and Close during preparation/export.

Affected Rust checks (with the repository FFmpeg environment configured):

```powershell
cargo test -p snow-recording-model --lib -- trim:: video_index::
cargo test -p snow-recording-effects --lib recorded::tests
cargo test -p snow-recording-export --lib -- clip:: playback:: deferred::tests streaming:: hardware_decoder
cargo test -p snow-recording-c --lib
# Optional native adapter check; plays only silence and requires an output device.
cargo test -p snow-recording-export --lib native_output_plays_silence -- --ignored
cargo clippy -p snow-recording-model -p snow-recording-effects -p snow-recording-export -p snow-recording-c --all-targets --all-features -- -D warnings
```

The fixtures cover all four formats, variable animation delays, one-frame clips, non-keyframe cuts,
separate/silent audio, HDR, effects spanning the cut, retained sources, cancellation, fake audio and
clock shutdown races, and decoder fallback.

## Release benchmark

Use only `windows-msvc-performance`:

```powershell
cmake --build --preset build-windows-msvc-performance --target snow-shot-recording-trim-performance-benchmark
```

The benchmark reports source opening, first frame, seeks, one second of playback, one-second trim
export, peak sampled private-memory growth, and the hardware/software pixel difference. It checks
pixel agreement and a 768 MiB memory-growth ceiling. The 30-minute fixture is sparse (one stored
frame per second on a 60 fps presentation timeline); the 1080p60 and 4K60 fixtures are dense.
Generated media is reused for subsequent runs.

Measured on the Windows development host on 2026-10-02 (Release; milliseconds):

| Fixture | Open | First frame | Seek median / maximum | Trim export | Memory growth |
| --- | ---: | ---: | ---: | ---: | ---: |
| 1080p60 | 9.68 | 117.73 | 99.27 / 363.89 | 1049.28 | 318.93 MiB |
| 4K60 | 8.44 | 424.95 | 482.79 / 772.81 | 3905.90 | 595.28 MiB |
| 30 minutes, sparse | 5.15 | 181.62 | 200.33 / 388.27 | 954.71 | 329.95 MiB |

These are synthetic-fixture measurements, not performance guarantees for every GPU or codec.
The Windows native controller/area checks and WASAPI output/reset check passed.
The macOS Audio Queue adapter and native macOS window behavior require validation on macOS.
