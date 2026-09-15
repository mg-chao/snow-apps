# macOS media implementation and acceptance

This implementation targets macOS **15.0 or newer**, with separate arm64 and
x86_64 builds. It is an implementation in progress, **not completed acceptance of
the comprehensive macOS plan**. Snow Shot's Qt application is not ported by these
changes. The remaining requirements are listed below rather than implied by a
successful compilation.

## Ownership and architecture

- `snow-media`: tagged geometry, shared-edge projection, pixel/plane/color
  descriptions, rational clock domains, immutable CoreVideo leases, SDR/HDR
  conversion primitives. It does not depend on capture or recording.
- `snow-macos`: objc2 native ownership, ScreenCaptureKit, CoreAudio/AVAudioEngine,
  public input observation, CoreText, cursor sampling, and Metal-backed Core Image
  composition. No legacy screenshot acquisition API is used.
- `snow-capture`: the common capture facade and a macOS native extension.
  Snapshots use `SCScreenshotManager` sample buffers; continuous sources use
  persistent `SCStream` instances.
- `snow-recording-runtime::macos`: fixed-size native direct recording and
  version-2 editable recording. The native capture surface is composed to BGRA or
  P010 and submitted to FFmpeg VideoToolbox without mapping capture pixels.
- `snow-recording-export`: encoding, muxing, editable transcoding and export.
- `snow-capture-c`: capture-only static library, independently buildable without
  FFmpeg. `snow-recording-c`: recording/export static library. Windows C callers
  and the aggregate Rust FFI library use the separated recording symbols.

Common capture options select `BackendTuning::Default` or explicit settings in
`tuning::windows`. macOS rejects explicit Windows tuning. Common `WindowId`
values are tagged by platform; the recording API uses the same type. Window
IDs remain session-scoped. Windows GPU recording still uses the specialized
`snow-d3d11` interface;
a single shared native-surface interface for both platforms is outstanding.

## Capture contract

Use `snow_capture::native::{DesktopConfig, DesktopSession, DesktopEvent}` on
macOS. Targets include primary display, an enumerated display, an enumerated
session-scoped window, or a tagged desktop region. Primary display resolves once;
it never changes to another monitor when the original disappears.

All macOS desktop geometry is in **points**, with a top-left origin and downward
positive Y. Frame dimensions and destination rectangles are **pixels**. Regions
default to the highest intersecting display scale. Shared projected edges avoid
independent rounding seams. Desktop gaps are transparent for screenshots and
black for recording. Display/window output defaults to native pixel resolution.
Native window capture omits shadows.
`DesktopConfig::capabilities()` resolves the current target transform, source
count, native format, available CPU formats and exclusion support without starting
a stream or acquiring pixels. It shares validation with session startup and
requires screen permission on a worker thread. `CaptureSupport::current()` queries
OS, architecture and Metal support without requesting permission. Capability
snapshots are not target reservations. The C equivalent is
`snow_capture_macos_query_capabilities`. Display filters accept excluded window IDs
and process IDs. Independent-window capture rejects display exclusions.

`Configuration` precedes a frame in the new geometry generation. Composition
retains each source timestamp; several monitors are not an atomic acquisition.
Repeated output is explicitly marked duplicate. Suspended/blank/stopped samples
invalidate in-flight observations. The first terminal native error is sticky
and takes priority over a saturated notification queue.

Native queue depth is three. Callback delivery, conversion-worker input and
latest output are bounded. The copy worker moves native frames into a four-buffer
owned GPU pool. Retaining all leases applies backpressure and drops observations
instead of retaining unlimited ScreenCaptureKit buffers. Retained leases survive
session shutdown. CPU mapping is explicit, read-only, and respects row/plane
strides. A completed GPU render precedes lease publication.
`PixelBuffer::to_cpu_format` explicitly selects BGRA8, RGBA8 or floating-point
HDR without implicit tone mapping. C callers use
`snow_capture_macos_frame_map_format`; mappings are cached for the lease lifetime.
Concurrent native and converted mappings share one CPU readback. Conversion
preserves alpha and readable plane offsets/padding; native access never maps CPU
pixels.

SDR defaults to sRGB BGRA8, with RGBA8 CPU conversion available. Explicit HDR
capture uses ScreenCaptureKit canonical HDR on Apple Silicon and floating-point
CPU output. Intel reports HDR capture unsupported. Canonical recording output is
P010 BT.2020/PQ with video range and HEVC Main10 metadata; it is converted rather
than relabeled.

## Threading, permissions and errors

Run blocking capture, recording and C entry points on a **worker thread while
the host's main run loop runs**. Blocking capture APIs reject calls from the main
thread. `snow_macos::run_loop::drive_until` is supplied for command-line harnesses;
GUI applications should retain their normal event loop.

Permission checks do not prompt. Screen, microphone and input permission requests
are explicit. A host using microphone capture must supply
`NSMicrophoneUsageDescription`; the harness plist includes usage metadata.
The C microphone permission request is asynchronous. Its callback/context must
remain valid until its one invocation, potentially on an arbitrary OS queue.
Device-enumeration visitor strings are borrowed only during the synchronous call.

Do not automatically retry permission errors. Targets that disappear are reported
unavailable. Public cursor sampling may withhold the system cursor shape; separate
cursor mode fails explicitly in that case. It never invents an arrow. Input
Monitoring and secure input are separately observed; interrupted input state is
cleared before accepting a new generation.

Synchronous snapshot stages share a deadline across enumeration and acquisition.
Capture and audio configs carry a cloneable broadcast `CancellationToken`.
Cancel wakes all waiters; it invalidates late frame/audio delivery and cannot enter
an automatic retry loop. A pre-canceled operation performs no acquisition. C
capture/recording configs clone their cancellation handles, so releasing the C
handle after create returns is safe and does not cancel the session. Keep a handle
alive throughout a concurrent cancel call. A lost completion callback is a native
failure, distinct from cancellation or timeout.

Recording cancellation is checked during finalization and serialized with final
file publication. If publication wins that ordering, a subsequent cancel does not
undo the completed result. Native waits and stream teardown have timeouts and late
callbacks retain their own state. System-audio enumeration and stream-start waits
share one cancelable deadline. Full end-to-end deadlines for synchronous Metal work and every
multi-source stream operation, plus a single serialized native control queue,
remain incomplete. Hosts must not yet treat timeout values as strict real-time
upper bounds for every call.

## Recording and export

`NativeRecordingSession` records direct MP4 and writes `<output>.snowmedia` with
color, cursor mode, geometry/destination changes and timeline discontinuities.
Source resize preserves the configured even output dimensions and aspect ratio
against black. Default cursor mode is ScreenCaptureKit embedded.

`NativeEditableSession` writes a self-contained version-2 bundle. The intermediate
uses native H.264 SDR or HEVC Main10 HDR. Frame-index writes are incremental;
audio is stored as separate PCM assets using the same pause-aware recording
clock. Only one ScreenCaptureKit system-audio source is started. System audio
means desktop playback, independent of a selected video window, excluding the
recording process. A default microphone may follow a newly selected default;
an explicitly selected UID never silently retargets. Device changes reset
conversion state and produce a source-restarted event. Required audio-source
policy is preserved in editable recording. PCM gaps and overflow silence are
reported in sample time and included in the editable media manifest. Pause/resume
alignment uses the same recording clock. Audio workers honor broadcast
cancellation, including while paused and when native packets complete late.

Editable recording supports embedded, hidden and separate cursors. Separate
cursor shapes, positions, click events (including the middle button), and trail
observations are spooled incrementally to disk. The in-memory shape cache holds
at most eight shapes. Assets retain scaled hotspots and straight alpha, and are
mapped into the fixed output canvas. Input events use the pause-aware host clock;
input loss emits an interruption instead of silently inventing observations.
Editable keyboard assets remain unsupported. Direct recording supports click,
trail and keyboard tiles composed through Metal without capture-pixel readback.

`RecordingArtifact::open` opens a completed bundle. `EditingSession` defaults to
the intermediate's codec. HEVC export from HDR preserves 10-bit BT.2020/PQ; H.264
and animated outputs explicitly tone-map. The tone mapper uses ST2084 absolute
luminance, a 203-nit reference/1000-nit Reinhard shoulder, linear BT.2020→BT.709
conversion, gamut clipping and sRGB transfer. This fixed rendering policy is
implemented in `snow-media::color` and tested against its scalar reference.
Editable HDR cursor/click/trail overlays blend in linear BT.2020 light at a
203-nit SDR reference white before PQ encoding or SDR tone mapping. Video resize
runs once per decoded source; repeated output frames start from an immutable
base and evaluate the retimed mouse state independently. RGB row padding is
preserved. Destination-dependent XOR/masked cursor operations fail explicitly
for HDR. Separate macOS cursor assets use alpha blending.

Hardware-only never opens a software encoder on failure. Hardware-preferred
may fall back if the software encoder accepts the required pixel depth. Native
HDR software recording/fallback accepts padded P010 with explicit HDR10 color
metadata; both macOS profiles provision Main10 x265. The encoder rejects SDR or
RGBA input submitted to that path. Software encoding performs a reported CPU
readback; native hardware recording retains its no-readback path. Live software
HDR uses one x265 frame worker, no lookahead and no B-frame reordering, with WPP
parallelism within the frame. Offline export keeps its compression settings.
Animated export and
editable transcoding currently use CPU conversion/readback. The no-readback
contract applies to the supported **native direct/intermediate recording** path.

Version-1 bundles are rejected explicitly; no compatibility decoder is provided.

## Standalone build

Run commands from the repository root. Xcode command-line tools and Rust 1.97.1
are required. Qt and OCR are not required for these standalone targets.

```sh
scripts/bootstrap-macos-media.sh arm64
export FFMPEG_DIR="$PWD/.tools/macos-media/installed/arm64-osx-snow-media/arm64-osx-snow-media"
scripts/build-macos-media.sh arm64 --recording
```

For Intel, provision `x86_64` and select the
`installed/x64-osx-snow-media/x64-osx-snow-media` profile, then build with
`scripts/build-macos-media.sh x86_64 --recording`. Each architecture has a separate
vcpkg install root; provisioning one must not prune the other. The provisioning
script pins vcpkg and verifies downloaded pkgconf/NASM archives. FFmpeg enables
VideoToolbox, H.264/HEVC, audio, GIF/APNG/WebP and the existing export containers.

Capture only:

```sh
scripts/build-macos-media.sh arm64
cmake -S snow-crates/native -B build/macos-media/capture -DCMAKE_BUILD_TYPE=Release
cmake --build build/macos-media/capture --target snow_capture_c_smoke
```

Add `-DSNOW_MEDIA_BUILD_RECORDING=ON` for the recording C target with the matching
`FFMPEG_DIR` and pkg-config on PATH. CMake selects the target architecture and
macOS 15 deployment target. It does not run Windows shader/DLL steps on macOS.
A universal binary must combine **two separately built** architecture outputs.

The build creates `SnowMediaHarness.app` and, with recording enabled,
`SnowRecordingHarness.app` under `build/macos-media/<rust-target>/`. The recording
app contains its provisioned dylib dependency closure and a relative runtime
search path; it is ad-hoc signed for local testing, not notarized for distribution.

```sh
build/macos-media/aarch64-apple-darwin/SnowMediaHarness.app/Contents/MacOS/SnowMediaHarness status
build/macos-media/aarch64-apple-darwin/SnowMediaHarness.app/Contents/MacOS/SnowMediaHarness list
build/macos-media/aarch64-apple-darwin/SnowMediaHarness.app/Contents/MacOS/SnowMediaHarness snapshot hdr
build/macos-media/aarch64-apple-darwin/SnowMediaHarness.app/Contents/MacOS/SnowMediaHarness stream
build/macos-media/aarch64-apple-darwin/SnowMediaHarness.app/Contents/MacOS/SnowMediaHarness benchmark
build/macos-media/aarch64-apple-darwin/SnowRecordingHarness.app/Contents/MacOS/SnowRecordingHarness /tmp/native.mp4 hdr system-audio pause
build/macos-media/aarch64-apple-darwin/SnowRecordingHarness.app/Contents/MacOS/SnowRecordingHarness /tmp/editable.snowrec hdr system-audio editable
```

Explicit permission commands are `request-screen`, `request-input`, and
`request-microphone`. `inputs` enumerates microphones; `microphone [UID]` runs a
three-second sample/host-timeline check without requiring a display. `input`
counts passive events for three seconds without logging keys or mouse locations.
The recording harness accepts `software` or `preferred` to select an encoding
policy; its default is hardware-only. Do not run several capture acceptance harnesses at once.
Export uses `macos_export INPUT.snowrec OUTPUT [hevc|h264|gif|apng|webp]` from the
Rust examples directory, with `DYLD_LIBRARY_PATH="$FFMPEG_DIR/lib"` when running
that unbundled executable.

## Validation record (2026-09-16)

Only affected crate tests and filtered CTest targets were run. No full workspace
or repository suite was run.

- arm64 production FFmpeg and release capture/recording harnesses build; app
  dependency closure passes code-signature verification.
- Both production FFmpeg architecture profiles and both architecture release
  library/harness builds complete. Physical Intel execution was not tested.
- Windows capture, capture-C and audio all-target cross-checks passed; Windows runtime/Qt/performance
  acceptance remains outstanding.
- Native synthetic Metal/VideoToolbox SDR H.264 and HDR HEVC round-trips passed,
  including retained-pool ownership and effect-tile pixel orientation.
- Native editable HDR resize/transcode passed for HEVC Main10 with BT.2020/PQ
  metadata, tone-mapped H.264 and GIF. HDR fidelity on a calibrated HDR display is
  not established by these metadata tests.
- Related media, capture, audio, model, recording, export and C ABI tests pass;
  added tests cover cancellation broadcast/commit ordering, late audio, C token
  lifetime, capability validation, concurrent CPU mapping, required audio sources
  and PCM gap metadata. Both filtered C ABI smoke tests pass;
  the native GPU tests are explicit/ignored in unattended runs.
- Prior native acquisition checks observed SDR/HDR 1920×1080 snapshots and a
  five-second SDR stream, with a retained frame surviving shutdown. Prior direct
  SDR/HDR-plus-system-audio samples recorded 89 frames and zero reported CPU
  readbacks.
- A previous capture attempt reported no shareable displays. On September 16,
  the refreshed harness enumerated a single 1920×1080 display. Screen, microphone
  and input permissions were authorized. The passive input tap ran for three
  seconds with Active status and zero drops; no events occurred during that
  interval. No microphone device was enumerated, so microphone samples remain
  untested on this host.

September 16 continuation checks (Mac16,10, arm64, macOS 27.0):

- Affected library tests: `snow-macos` 15, `snow-media` 15, C recording 24,
  model 7, export 75 (one ignored), runtime 95. All nine explicitly selected native/software
  integration tests passed, including padded P010, retimed cursor pixels,
  HEVC/H.264/GIF exports, and CPU/Metal opaque/translucent colors.
- The destination BT.2020 YCbCr attachment is now set before Metal rendering.
  The regression test previously observed a 12-code red-luma error on first pool
  use; opaque/translucent color channels now agree within five 10-bit codes.
- Native 1920×1080 floating-point HDR snapshot acquisition passed. Direct and
  editable HDR/system-audio recordings with pause/resume encoded 70 frames each,
  with zero reported CPU readbacks and no reported interruptions. The editable
  session included separate cursor and input-effect assets; HEVC HDR and H.264
  SDR exports completed. After the color fix, a final packaged editable HDR run
  encoded 71 frames, zero readbacks and zero interruptions; HEVC export completed.
  This does not verify audible playback or manual input-effect appearance.
- Both architecture release libraries and signed harnesses build. The packager
  refreshes the entire provisioned dylib closure on every invocation; two
  deterministic tests cover dependency refresh and missing dependencies.
- Clippy passed with and without optional timing/experimental conversion hooks.
  Intel macOS all-target checks and Windows capture/audio/model checks passed.
  Both filtered C ABI tests passed.
- License collection completed: 183 notices, 98 Rust packages, 5 vcpkg packages.

Short release measurements on the single display:

| Capture operation | Result |
| --- | --- |
| 30 snapshots | Cold 132.7 ms; warm p50 59.5 ms, p95 61.6 ms, p99 62.6 ms |
| 1080p stream, 5 seconds | 56.0 frames/s; source age p50 2.6 ms, p95 7.8 ms; 2 drops |
| 4K stream, 5 seconds | 56.5 frames/s; source age p50 4.9 ms, p95 11.1 ms; 1 drop |

These capture paths performed no CPU mapping. Stream source age measures callback
to consumer delay, not full capture-to-display latency. Software HEVC HDR at
1280×720 encoded 88 frames with 88 reported readbacks in the initial three-second
run. After live-encoder tuning, a short run encoded 84 frames with 84 readbacks;
peak RSS fell from 702 MiB to 275 MiB (process footprint 771 MiB to 345 MiB).
Scene content and brief concurrent synthetic-test activity were uncontrolled;
these are diagnostic observations, not sustained throughput/regression acceptance.
CPU/GPU utilization, full allocation/readback stage timing, long-term memory,
physical Intel/macOS 15, multi-display and Windows comparisons remain outstanding.

New logs under `build/macos-media/` include `sept16-final-tests.log`,
`sept16-native-tests.log`, `sept16-color-agreement.log`, `sept16-clippy.log`,
`sept16-timing-clippy.log`, `sept16-clippy-final.log`, `sept16-intel-check.log`,
`sept16-windows-check.log`, `sept16-capture-benchmark.log`,
`sept16-software-hdr-native.log`, `sept16-software-hdr-tuned.log`, and
`sept16-native-editable-export.log`.

Current continuation logs are under `build/macos-media/`: `continuation-tests.log`,
`audio-cancellation-tests.log`, `cpu-mapping-tests.log`,
`continuation-native-tests.log`, `continuation-windows-check.log`,
`continuation-arm64-release.log`, and `continuation-intel-release.log`.

Relevant commands:

```sh
cargo test --manifest-path snow-crates/Cargo.toml -p snow-media
cargo test --manifest-path snow-crates/Cargo.toml -p snow-macos
# Set FFMPEG_DIR and DYLD_LIBRARY_PATH for recording tests.
cargo test --manifest-path snow-crates/Cargo.toml -p snow-recording-export --lib
cargo test --manifest-path snow-crates/Cargo.toml -p snow-recording-runtime --lib
cargo test --manifest-path snow-crates/Cargo.toml -p snow-recording-c --lib
cargo test --manifest-path snow-crates/Cargo.toml -p snow-recording-export --test macos_native -- --ignored --test-threads=1
ctest --test-dir build/macos-media/cmake-native -R '^(snow-capture-c|snow-recording-c)-macos-abi$' --output-on-failure
```

## Outstanding acceptance and implementation

1. Complete the common cross-platform API/native-surface migration, including
   unified display identity/geometry throughout shared callers and full
   per-target/OS/hardware/format capability queries.
2. Complete serialized native control and strict end-to-end startup/reconfigure/
   stop/cancel deadlines. Associate window geometry with the exact frame, and
   validate all sleep/wake, lock, display removal, and permission-revocation paths.
3. Persist editable keyboard assets and implement GPU editable-export composition.
   Validate prolonged cursor/input-asset recording, real input interruptions,
   and CPU/Metal agreement across more displays, formats and effect overlaps.
4. Validate audio device reconfiguration, pause/resume, drift, overflow silence
   and interruption reporting over long recordings, including interruptions at
   stream termination before another timestamped packet arrives.
5. Measure acquisition, queue delay, conversion, composition, allocation,
   readback and retained resources independently with optional timing compiled
   out. Run prolonged start/stop and resource-retention tests.
6. Run physical Apple Silicon and Intel/macOS 15 acceptance, mixed Retina and
   rotated/negative-origin display setups, hotplug, system/microphone audio,
   input effects and cursor availability. Verify HDR content and playback.
7. Record optimized latency percentiles, throughput, CPU/GPU utilization, memory,
   allocations, readbacks and drops for snapshots, small regions, 1080p/4K,
   multi-display composition and direct recording. Compare against Windows
   pre-refactor behavior and performance on Windows hardware.

## Licensing

The macOS FFmpeg profile includes GPL x264/x265. The existing license collector
supports `-StandaloneMedia` without requiring Qt and collects Cargo and vcpkg
notices. Upstream objc2-family license sources are retained under `licenses/cargo`.
Redistribution must include the generated notices and the applicable source/license
obligations; local harness signing does not change the libraries' licenses.
