# Recording performance validation

## Scope and environment

Measured on 2026-09-11 from base revision
`1e5766f9ced513e96c06a3de7f15c9050bf9f135` on Windows, Ryzen 9 5950X
(12 physical / 24 logical processors exposed), Radeon RX 6750 XT, and a
3840x2160 primary capture target. All performance runs use the repository's
`windows-msvc-performance` environment, dynamic FFmpeg, and Cargo `--release`
with the repository's optimization settings. No Debug results are used.

Raw local CSVs/logs are in `build/windows-msvc-performance/recording-perf/`.
The old `snow-crates/target/perf/webp-debug` CSV does not establish a Release
baseline and was excluded.

## Independently validated changes

Times below are medians. Each A/B microbenchmark discards a warmup pair and
alternates execution order. The buffer benchmark uses identical changing
screen-like images, timestamps, dimensions, quality, and encoder settings.
It includes submission and encoder finalization but excludes input generation,
encoder initialization, and output verification.

| Change / workload | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Buffer ownership + reuse, 1080p, libx264, 60 frames, 5 samples | 846.97 ms | 713.19 ms | 15.8% |
| Buffer ownership + reuse, 4K, libx264, 60 frames, 5 samples | 3056.70 ms | 2523.74 ms | 17.4% |
| Resize 4K -> 1080p, 7 samples of 60 frames | 4.41 ms/frame | 1.37 ms/frame | 68.9% |
| Resize 1080p -> 720p, 7 samples of 60 frames | 1.90 ms/frame | 0.33 ms/frame | 82.6% |
| RGBA -> NV12, 1080p, 5 samples of 30 frames | 5.42 ms/frame | 1.59 ms/frame | 70.7% |
| RGBA -> NV12, 4K, 5 samples of 30 frames | 22.15 ms/frame | 8.81 ms/frame | 60.2% |
| RGBA -> YUV420P, 4K, 5 samples of 30 frames | 19.74 ms/frame | 8.31 ms/frame | 57.9% |
| Explicit hardware MFT, 1080p, borrowed buffers, 30 frames, 3 samples | 392.60 ms | 346.43 ms | 11.8% |
| Explicit hardware MFT, 4K, borrowed buffers, 30 frames, 3 samples | 1591.08 ms | 1296.65 ms | 18.5% |

The individual results are in `copy/streaming-copy.csv`,
`resize/recording-resize.csv`, `conversion/recording-conversion.csv`, and
`hardware-{baseline,enabled}/streaming-copy.csv`. Do not add these percentage
improvements: they affect overlapping portions of the pipeline.

The final runner (`final/`) was also executed with three samples of 30 frames,
including decoded-video validation. Against the matching original
`hardware-baseline` borrowed-buffer runs, the final owned-buffer path reduced
1080p median time from 392.60 ms to 198.78 ms (49.4%), and 4K from 1591.08 ms to
741.28 ms (53.4%). This combined comparison includes both pixel-pipeline changes
and the hardware-encoder change, with the compression caveat below.

### Buffer ownership

The compositor transfers its completed `Vec<u8>` into the streaming encoder.
The encoder returns the prior buffer for reuse. Coalescing swaps buffers instead
of copying pixels. The borrowed API retains its existing behavior. The final
copy into FFmpeg's refcounted frame remains; avoiding it safely requires a
separate frame-ownership API.

### Resizing

The source-to-output coordinate map is cached until dimensions change. The
inner pixel loop performs no divisions, and output buffers are recycled.
Sampling and alpha remain byte-for-byte identical to the old top-left
nearest-neighbor implementation. This is not a filtering/quality change.

### Color conversion

FFmpeg's old `sws_scale` entry point executes conversion on the calling thread.
The new wrapper explicitly initializes the same scaler and uses
`sws_scale_frame` to activate native slice workers. NV12/YUV420P frames of at
least 1920x1080 use up to four workers, respecting a smaller configured thread
limit or CPU count. Smaller frames and other formats retain the original path.
Tests compare visible pixels, chroma planes, alpha input, repeated calls, and
small/unaligned edge dimensions against the old conversion.

### Hardware selection

FFmpeg's `h264_mf` defaults `hw_encoding` to false. Merely selecting this encoder
previously chose `H264 Encoder MFT` (software), while the runtime called it
hardware. An explicit hardware request now sets `hw_encoding=1`; the measured
machine selects `AMDh264Encoder`. Existing software fallback on initialization
failure remains in place.

This change has a compression tradeoff. With the existing quality/bitrate
settings, the synthetic 30-frame 4K output grew from about 0.67 MB to 11.48 MB.
Hardware and software encoders do not have equivalent rate control. The
performance comparison retains the configured quality/bitrate, but does not
claim equivalent compression efficiency or decoded quality. Codec/bitrate
policy was not retuned to make benchmark numbers look better.

## Live observations and limitations

The corrected live WGC benchmark produced these 12-second 4K observations:

| Metric | Instrumented baseline | Hardware + threaded conversion |
| --- | ---: | ---: |
| Delivered frames/s | 28.89 | 35.27 |
| Encoder submission p50 | 34.01 ms | 26.97 ms |
| Stream drops | 361 | 233 |
| Capture duplicates / delivered frames | 318 / 347 | 151 / 424 |

Desktop content was not controlled between those runs. These observations
confirm that the live path operates and improves in these runs, but cannot
establish the isolated speedup. The deterministic A/B results above provide
that evidence. The live benchmark bypasses app overlays/resizing and uses the
borrowed submission API, so it does not measure the entire Qt recording flow.
4K60 has not been achieved on this machine.

The final runner's additional eight-second live checks completed without errors:
WGC delivered 33.28 fps; DXGI delivered 11.99 fps with capture p95 of 471.74 ms.
The latter is a capture-side outlier/latency issue, not evidence for changing the
default to DXGI. No capture backend implementation was changed in this work.

Capture already runs on a separate thread with a bounded queue. Another encoder
queue would not fix sustained conversion/encoding throughput and was not added
without evidence of benefit. Duplicate-frame suppression and overlay scheduling
were not changed: both need separate timeline/animation benchmarks before
changing frame durations or dropping visual updates. GPU-resident capture,
composition, and encoding remain a larger potential follow-up.

## Reproduction and verification

From the repository root:

```powershell
./scripts/run-recording-pipeline-perf.ps1
```

The runner builds only the related Release examples and runs them sequentially.
Use `-SkipLive` for deterministic workloads only. It writes CSVs and native
encoder logs to a new timestamped folder under `build/windows-msvc-performance`.
`-Samples`, `-Frames`, and `-LiveSeconds` adjust the encoder/live workloads.

The capture benchmark now excludes warmup and encoder initialization from elapsed
measurement time, discards initialization backlog, preserves capture timestamps,
reports authoritative stream-drop deltas, and separates setup/finalization and
copy/conversion/send/mux timings. Missing frame timings are errors. Instrumentation
is gated by `stage-timing` and compiles out of ordinary builds.

Regression coverage: 27 focused tests in `streaming::tests`,
`rgba_converter::tests`, `rgba_resizer::tests`, `direct::tests`, and
`media_foundation_hardware_selection_requires_a_hardware_transform` passed in
Release. The full Rust/C++ test suites were not run. The encoder benchmark also
decodes its generated files and checks dimensions/frame counts outside timing.
Targeted Clippy passed with `-D warnings` for the three affected libraries and
all four benchmark examples. Rust formatting and `git diff --check` passed.
`cargo check -p snow-capture-c --release --lib` also passed with default features,
validating the application's Rust FFI dependency chain without stage timers.
The Qt application executable was not rebuilt or driven through its UI.
