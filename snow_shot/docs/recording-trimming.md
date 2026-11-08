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
native picture timestamps use the timeline's floor convention, including fractional-microsecond
60/144 FPS boundaries, so cuts and preview select the requested picture.
Rust `TrimRange` converts integer microseconds to rational frame and audio sample boundaries.

`ClipSource` retains either finalized media or a deferred recording bundle. Deferred preview reads
only original media; export reconstructs source-time input effects and rebases progress/time
overlays to the selected duration. Audio tracks stay separate in exports and are mixed for preview.
The speed selector sits between Replay and the timeline. Each new editor starts at `1.0x` and
offers `0.25x`, `0.5x`, `0.75x`, `1.0x`, `1.25x`, `1.5x`, `1.75x`, `2.0x`, and `3.0x`.
It controls preview and both Copy and Save. The source timeline and trim-grip timecodes retain
their original times. A rate change preserves the worker's exact source position and actual
play/pause state; changing speed at the end does not restart playback. Selection survives
exports, cancellation, retry, and appearance changes within the same editor session.

Video exports keep the configured presentation FPS and sample the source using checked rational
time, repeating or skipping pictures. `FixedFrameTimeline` retains the native source endpoint
and recovers exact presentation-grid boundaries from the editor's microsecond timecodes.
Finalized and deferred exports derive output slots, duration, and retimed audio sample counts
from the same `RetimedFrameTiming`. Rounding occurs only when converting to the consuming clock;
a partial final source frame keeps its actual duration rather than becoming a complete FPS slot.
Animated-image boundaries use cumulative retimed delays
and container precision rather than independently rounded per-frame delays. Deferred cursor,
click, trail, and keyboard effects use source time; playback badges and progress use output time.
Separate audio tracks keep their metadata and pitch through bounded FFmpeg
[`atempo`](https://ffmpeg.org/ffmpeg-filters.html#atempo) graphs;
preview mixes the transformed tracks. Unity speed bypasses audio filtering.
Very short audible cuts can lack enough context for tempo analysis. If the bounded filtered
result is entirely silent, retain the selected native-pitch PCM prefix within the exact output
endpoint; never read neighboring audio outside the cut.

Full-range finalized Copy at `1.0x` reuses the original file; Save copies its bytes. Other exports decode and encode the selected
interval with the recorded settings. Publication uses temporary files and never replaces a source.
The editor caches the latest successful range and speed for repeated Copy or Save. Each export
retains its immutable submitted selection independently of later UI state.

Normal-speed partial MP4 exports preserve sparse decoded pictures and coalesce identical SDR
frames. Stored samples can cover multiple presentation slots; the configured FPS, selected first
picture, audio clock, and exact endpoint remain intact. HDR trims retain 10-bit PQ pictures and
their original holds. Speed-adjusted MP4 exports continue to store every output presentation slot.
Finalized audio tracks retain their source bitrate budget and mono/stereo layout instead of
defaulting to 192 kbps stereo. Replay PCM remains 48 kHz stereo and is converted to each track's
encoded layout. Explicit lower AAC budgets, including 96 kbps mono, are honored.
New MP4 recordings retain the intended per-track bitrate in `snow_audio_bitrates_kbps`
container metadata, including through recovery remuxing and repeated trims. This prevents long
silences from lowering the budget for a later speech selection. Older files without this metadata
use supplied audio settings when available, otherwise the measured container bitrate or defaults.

Deferred rendering, partial trims, and generated exports use an explicit offline encoder policy.
NVENC uses HQ/CQ VBR with lookahead and B-frames; AMF uses its quality/transcoding preset with
preanalysis enabled and frame reordering disabled so single-picture exports can drain; QSV uses
its slow preset with lookahead and B-frames. VideoToolbox disables real-time mode and permits
reordering. Media Foundation uses the archive scenario with its existing
quality control. Software encoders retain the selected CRF and speed preset. Live capture and clean
recording sources retain bounded buffering; the offline policy rejects live GPU surface pools and
segmented recovery. Encoder draining and duration finalization include the offline buffered tail.

The FFmpeg overlay backports VideoToolbox's two-frame H.264 reorder-depth correction and
CMTime rescaling. For reordered H.264/HEVC it also derives decode timestamps from the submitted
presentation timeline delayed by frames, rather than subtracting a fixed time offset. This keeps
`PTS >= DTS` for both regular output and sparse trims with changing frame intervals, while
preserving hardware encoding, B-frames, original presentation times, and the final hold.

Cropped Copy exports and Save suggestions expand the configured video filename format at export
time, keeping the original output directory and extension. They share normal recording exports'
`_1`, `_2`, and subsequent collision suffixes. Repeated Copy reuses the cached clip and its filename.

Recording filename and save-directory templates also support `{AN}` (application name) and
`{WN}` (window title). Both names come from the window under the selected recording region's
center when capture starts, after any countdown, ignoring the recording controls even when the
toolbar is included in the video. Trimmed exports retain those names as other windows or titles
change. Values use the same safe path characters as screenshot exports; unavailable names become
`Unknown`. Without a recording context, directory pickers and folder resolution use `Unknown`.

A single playback worker owns FFmpeg and the native audio sink. Seek commands coalesce, revision
numbers reject stale frames, and cancellation interrupts forward decoding. Hardware frames stay on
the GPU until the requested presentation frame needs CPU pixels. The worker keeps current/lookahead
video frames, one published RGBA frame, reusable conversion buffers, and a bounded device audio
queue. A missing or stalled audio device falls back to the silent playback clock. Frame leases can
outlive the clip handle. Clip opening, seeking, encoding, audio, and resource teardown never join on
the GUI thread.

## Focused validation

Recording diagnostics retain the error text shown by the application and FFmpeg's native error
messages in the normal daily log. Trim preparation and export events include a session operation
ID, export attempt, submitted frame/time boundaries, speed, requested codec settings, cache reuse,
publication stage, and detached-window state. Failed deferred renders retain their error and stage.
Video packet mux failures also log the actual/requested encoder, hardware fallback, pixel format,
dimensions/FPS, native and submitted packet timing, both time bases, previous successful video DTS,
and drain state. Packet values are captured before FFmpeg consumes the packet. These diagnostics
do not change the encoder's timestamp policy or add packet details to the error dialog.
Use **Export logs** → **Copy** in storage settings after a failure; include the approximate failure
time when reporting it. Ordinary FFmpeg progress remains on its existing console path.

Build the application and the recording and diagnostics test executables, then run:

```powershell
ctest --preset test-windows-msvc-debug -R "^(snow-shot-recording-(trimming|export-naming|post-processing|finalization-close)-tests|snow-shot-screen-recording-area-window-tests)$"
ctest --preset test-windows-msvc-debug -R '^snow-shot-(recording-diagnostics|diagnostics)-tests$'
```

Set `SNOW_TRIM_VISUAL_OUTPUT` to a directory to render the actual trim toolbar in light/dark themes
at 100%, 125%, 150%, and 200% control scales, plus the complete main and secondary rows. The trimming test includes live translation,
keyboard/scrub constraints, immutable geometry, Save cancellation/publication, failure/retry,
repeated Copy, and Close during preparation/export.

The native Windows controls fixture avoids clipboard and file-dialog dependencies while the
offscreen fixture retains the complete export tests:

```powershell
ctest --test-dir build/windows-msvc-debug -C Debug -R '^snow-shot-recording-trim-ui-tests$' --output-on-failure
```

Affected Rust checks (with the repository FFmpeg environment configured):

```powershell
cargo test -p snow-recording-model --lib trim::
cargo test -p snow-recording-effects --lib recorded::tests
cargo test -p snow-recording-export --lib clip::tests
cargo test -p snow-recording-export --lib tempo::tests
cargo test -p snow-recording-export --lib playback::tests
cargo test -p snow-recording-export --lib deferred::tests
cargo test -p snow-recording-export --lib codec::tests
cargo test -p snow-recording-export --lib software_presets_buffer_reorder_and_flush_variable_duration_frames
# Optional native encoder validation; no screen capture or audio output.
cargo test -p snow-recording-export --lib offline_hardware_sparse_frames_preserve_timestamps_and_endpoint -- --ignored
# macOS VideoToolbox packet ordering, sparse holds, short clips and decoding.
# Requires FFmpeg rebuilt with the repository overlay; no capture or audio output.
cargo test -p snow-recording-export --test videotoolbox_timing -- --ignored
# Optional AMD AMF single-picture drain regression; no capture or audio output.
cargo test -p snow-recording-export --lib offline_amf_single_picture_drains_without_a_successor -- --ignored
cargo test -p snow-recording-export --test retimed_av_sync
cargo test -p snow-recording-c --lib clip::tests
# Optional native adapter check; plays only silence and requires an output device.
cargo test -p snow-recording-export --lib native_output_plays_silence -- --ignored
cargo clippy -p snow-recording-model -p snow-recording-effects -p snow-recording-export -p snow-recording-c --all-targets --all-features -- -D warnings
```

The fixtures cover video and animated formats, variable animation delays, one-frame clips,
non-keyframe cuts, separate/silent audio, HDR, effects spanning the cut, retained sources,
cancellation, fake audio and
clock shutdown races, and decoder fallback.

The minimal FFmpeg profiles and linked-filter audit have focused contract checks:

```powershell
scripts/test-snow-shot-ffmpeg-contract.ps1 -ComponentsOnly
scripts/test-snow-shot-ffmpeg-filter-audit.ps1
python scripts/test-windows-architecture.py WindowsArchitecture.test_minimal_recording_profiles_enable_only_required_tempo_filters
```

## Release benchmark

Use only `windows-msvc-performance`:

```powershell
cmake --build --preset build-windows-msvc-performance --target snow-shot-recording-trim-performance-benchmark
& build/windows-msvc-performance/cargo/x86_64-pc-windows-msvc/release/examples/recording_trim_benchmark.exe build/windows-msvc-performance/recording-trim-performance
```

The target builds the benchmark; run the generated Release `recording_trim_benchmark` executable
with the fixture directory as its argument. The benchmark reports source opening, first frame,
seeks, playback, trim export at `0.25x`, `1.0x`, and `3.0x`, peak sampled private-memory growth
including export, and the hardware/software pixel difference. Audio and long slow-speed selections
exercise tempo processing and bounded buffers. It checks
pixel agreement and a 768 MiB memory-growth ceiling. The 30-minute fixture is sparse (one stored
frame per second on a 60 fps presentation timeline); the 1080p60 and 4K60 fixtures are dense.
Generated media is reused for subsequent runs.

For a fixed-frame comparison of live and offline hardware policies:

```powershell
cmake --build --preset build-windows-msvc-performance --target snow-shot-recording-encoder-performance-benchmark
& build/windows-msvc-performance/cargo/x86_64-pc-windows-msvc/release/examples/recording_encoder_benchmark.exe build/windows-msvc-performance/live.mp4 10 0 1 h264 80 ui live
& build/windows-msvc-performance/cargo/x86_64-pc-windows-msvc/release/examples/recording_encoder_benchmark.exe build/windows-msvc-performance/offline.mp4 10 0 1 h264 80 ui offline
```

The final argument defaults to `live`; replace `ui` with `gradient` for dense moving content.
Both profiles use the same requested quality, dimensions, frame count, and GOP. CSV output reports
the encoder actually opened, file size, sampled decoded RGB PSNR, and timing. On the Radeon RX
6750 XT development host (2026-10-18, Release, one run per fixture, 1080p30, 300 frames, quality 80),
both profiles used AMF and decoded all 300 frames. These measurements predate disabling AMF
frame reordering for single-picture exports:

| Fixture | Live bytes | Offline bytes | Reduction | Live / offline PSNR | Live / offline encode seconds |
| --- | ---: | ---: | ---: | ---: | ---: |
| UI | 147,107 | 124,558 | 15.3% | 38.874 / 38.897 dB | 4.19 / 6.54 |
| Moving gradient | 27,311,177 | 23,447,640 | 14.1% | 24.806 / 24.799 dB | 3.35 / 4.77 |

These measurements exclude setup and final draining from encode time. They show the expected
compression/time tradeoff, not guaranteed savings for arbitrary recordings. NVENC, QSV, Media
Foundation, and VideoToolbox still require native validation on their respective hardware.

Optional arguments select one fixture (`1080p60`, `4k60`, `long-30min`, or `separate-audio`)
and one validated integer speed percentage. Set `SNOW_TRIM_MEMORY_TRACE=1` for absolute
before/peak/after export memory and 100 ms samples. The ceiling and scenario baseline stay
the same in focused runs.

Measured on the Windows development host on 2026-10-12 (Release; three runs;
median export milliseconds). The baseline was captured before the speed changes with the
same retained fixtures. No material `1.0x` regression was observed:

| Fixture | Baseline `1.0x` | Current `1.0x` | Change |
| --- | ---: | ---: | ---: |
| 1080p60 | 956.92 | 857.51 | -10.39% |
| 4K60 | 4397.07 | 3349.50 | -23.82% |
| 30 minutes, sparse | 1002.88 | 789.90 | -21.24% |

All speed runs independently verified the encoded presentation grid, duration, retained
audio routes, and hardware/software pixel agreement. Peak growth includes sampled export
and finalization memory; the largest value across all three runs was 686.52 MiB:

| Fixture | `0.25x` export | `1.0x` export | `3.0x` export | Maximum memory growth |
| --- | ---: | ---: | ---: | ---: |
| 1080p60 | 1077.42 | 857.51 | 461.86 | 396.89 MiB |
| 4K60 | 4048.19 | 3349.50 | 1728.11 | 686.52 MiB |
| 30 minutes, sparse | 986.76 | 789.90 | 373.87 | 135.38 MiB |
| Separate audio tracks | 266.88 | 102.67 | 59.36 | 18.61 MiB |

The 15-second sparse selection at `0.25x` produced 60 seconds / 3600 frames at 60 FPS
in a median 12866.70 ms, with scenario memory growth at most 135.38 MiB. The initial
repeated 4K slow-export probe exceeded the ceiling; caching prepared frames removed
redundant RGBA canvases while preserving decoded color and every output FPS slot.

These are synthetic-fixture measurements, not performance guarantees for every GPU or codec.
The Windows offscreen and native crop/export, native crop controls, and WASAPI output/reset checks passed.
The macOS Audio Queue adapter and native macOS window behavior require native validation
before macOS publication.
