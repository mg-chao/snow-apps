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

## GPU zero-copy end-to-end benchmark (`recording_gpu_benchmark`)

`scripts/run-recording-gpu-perf.ps1` (repository root) drives a real
`DirectRecordingSession` over the primary monitor twice with one harness:
once on the GPU zero-copy lane (WGC textures -> video processor ->
Media Foundation encoder) and once on the CPU pipeline (`--cpu`), then
prints effective encoded fps, decoded verification, and per-stage GPU
timings (`blt`, `mft_input`, `mft_output`) alongside mux timing. Stage
timers require the `recording-benchmark` feature, which the script sets.
```powershell
./scripts/run-recording-gpu-perf.ps1 -Seconds 12 -Fps 60
```
Pass `-SkipCpu` to measure only the GPU lane. Artifacts land under
`build/windows-msvc-performance/recording-gpu-<timestamp>/`.
