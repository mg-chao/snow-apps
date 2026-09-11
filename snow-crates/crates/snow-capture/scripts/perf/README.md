# Recording/export benchmark

Install the repository's dynamic FFmpeg dependencies first, then run from this directory:

```powershell
./run_recording_export_benchmark.ps1 -Backend wgc -Seconds 30
```

The script initializes the Windows performance environment and builds Release
with `stage-timing`. It captures the primary monitor and feeds frames to
`snow-recording-export::StreamingEncoder`. Warmup and encoder initialization are
excluded from measured elapsed time; pre-measurement frames are discarded and
real capture timestamps are retained. CSV output includes submission p50/p95,
queue pressure, stream drops, actual elapsed seconds, encoder setup/finalization,
and the selected encoder. A `.stages.csv` file separates frame copy, conversion,
encoder send, and packet drain/mux time. Stage timers compile out of normal builds.

Use `-Software` to select libx264 explicitly. Hardware-preferred runs can fall
back to libx264; inspect the encoder and FFmpeg transform name in the log.
Desktop content, GPU load, disk activity, and monitor size affect live results.

From the repository root, run all related A/B benchmarks sequentially:

```powershell
./scripts/run-recording-pipeline-perf.ps1
```

`-SkipLive` runs only deterministic pixel/encode workloads. `-Samples` and
`-Frames` control the streaming encoder comparisons. Each A/B benchmark warms
up and alternates execution order. Resizing and conversion require exact pixel
equality; the encoder benchmark decodes output and verifies frame count and
dimensions outside timed regions. Generated results remain under `build/`.
