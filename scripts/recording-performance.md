# Recording performance experiments

Use the `windows-msvc-performance` environment and Release builds. Benchmark
controls are Rust-only and do not alter the C FFI or application settings.
The workload covers the leftmost monitor and feeds synthetic observations to
recording effects; it does not inject OS mouse or keyboard input.

## Build and preserve an executable

From the repository root:

```powershell
# Detailed stage measurements.
./scripts/run-realtime-recording-perf.ps1 -BuildOnly

# Compile out stage probes for throughput confirmation.
./scripts/run-realtime-recording-perf.ps1 -BuildOnly -Metrics ''
```

Copy each executable to a unique location under `build/recording-throughput/`
before rebuilding. Preserve its SHA-256, source archive, Cargo.lock, feature
configuration, compiler versions and installed FFmpeg identity. The current
experiment archives are in `build/recording-throughput/sources/`, with frozen
executables in `build/recording-throughput/binaries/`. Do not overwrite a binary
that an earlier result identifies by hash.

Baseline and candidate must use the same harness, Release configuration and
timing features. Screen one mechanism at a time. The default benchmark path
keeps every experimental optimization disabled; switches select experiments.
Omit `-EncodeThreads` / `--encode-threads` to preserve the application policy.
Explicit zero requests the exporter's automatic worker count; 1, 2 or 4 requests that count.
The exporter resolves its automatic count from physical cores before opening
FFmpeg; it does not pass a zero thread count through to FFmpeg.
This distinction is corrected after frozen v15: older executables interpreted
zero as application policy, including its existing software 30-fps cap.
For resize, explicit zero or one requests the serial path, while omission
preserves the application's resize policy. `-AutomaticPolicies` enables
the validated application defaults in benchmark builds (which otherwise keep
automatic optimization selection disabled to support isolated comparisons).
For example:

```powershell
./scripts/run-realtime-recording-perf.ps1 `
  -Executable build/recording-throughput/binaries/prototype-v19-instrumented.exe `
  -Scenario application-default -Fps 60 -Clarity 1080p -PreferHardware `
  -OutputDirectory build/recording-throughput/example-screen
```

Run only one realtime benchmark at a time. Keep other builds, tests and desktop
activity out of the recording window. The window excludes itself from desktop
Peek; this is a workload-fixture policy. It does not modify capture behavior.
Workload publication cadence is independently measured at successful GDI
publication. It is not a measurement of physical display scanout.

## Alternating confirmation pairs

Schema 9 adds `encode_threads_override` to run metadata (`None` for omitted,
`Some(0)` for explicit automatic worker selection). The existing numeric
`encode_threads` metadata remains for compatibility. `effective_resize_threads`
reports the largest worker count selected for a composed frame; an automatic
pool whose backend/geometry rule never matches reports one. Pool size and the
eligibility rule are fixed before startup and do not retune during recording.

Write an options JSON with `common_args`, `baseline_args`, and `candidate_args`.
For synchronous versus asynchronous software H.264 at 60 fps:

```json
{
  "common_args": ["--scenario", "all-effects", "--fps", "60",
                  "--clarity", "1080p", "--workload", "continuous"],
  "baseline_args": [],
  "candidate_args": ["--async-encoder"]
}
```

```powershell
python scripts/run-recording-pairs.py `
  --baseline build/recording-throughput/binaries/prototype-v7-no-timing.exe `
  --candidate build/recording-throughput/binaries/prototype-v7-no-timing.exe `
  --config build/recording-throughput/confirm-async-sw60-config.json `
  --output build/recording-throughput/confirm-async-sw60-v7-no-timing
```

The runner fixes ten alternating pairs, five seconds of warmup and 30 seconds
of measurement for each recording. It records binary hashes and arguments and
stops on invalid workload/input/identifier measurements or competing work.
Build observations after final encoder drain are preserved separately; a
conservative one-second polling margin remains protected. Older executables
without recording boundaries protect their entire process lifetime.

Use the same command with `--resume` after addressing an interruption. It
requires the same hashes and configuration, preserves complete valid pairs,
and archives both members of an incomplete pair before repeating it. Do not
delete an invalid marker or replace a recording because its fps is low.

`comparison.json` contains paired recording-level absolute differences and
Student-t 95% confidence intervals over log ratios. Individual frames and
time windows are not independent trials. `confirmation_eligible` requires ten
valid pairs; it does not assert correctness, quality, latency, or acceptance.
Retain a throughput change only with supported fresh-fps improvement, followed
by confirmation with timing probes disabled. A stage-only improvement requires
supported lower processing time at already-satisfied cadence and no fresh-fps
regression. The asynchronous path must also satisfy the one-output-interval
capture-to-first-packet latency allowance.

## Evidence files and endurance inspection

For the final cumulative comparison, use the preserved original pipeline and
v19 executable with the matching feature configuration:

```powershell
python scripts/run-recording-pairs.py `
  --baseline build/recording-throughput/binaries/original-current-v18-instrumented.exe `
  --candidate build/recording-throughput/binaries/prototype-v19-instrumented.exe `
  --config build/recording-throughput/cumulative-sw60-final.json `
  --output build/recording-throughput/reproduce-cumulative-sw60
```

Use `cumulative-hw60-final.json` for hardware preference. For the timing-disabled
confirmation, replace both executable suffixes with `-no-timing.exe` and use a
new output directory. These configs use all-effects, continuous content, 60 fps,
1080p output, with `--automatic-policies` only on the final candidate. Frozen
source ZIPs and manifests are under `sources/`; original-source preparation and
adaptation records are under `cumulative-original-v18-harness/`.

Prepared final coverage and endurance commands, run separately from builds and
offline analysis:

```powershell
python -u build/recording-throughput/run-coverage-resumable.py `
  build/recording-throughput/final-matrix-v19-jobs.json
python -u build/recording-throughput/run-audio-endurance.py `
  prototype-v19-instrumented.exe endurance-v19
```

The coverage manifest contains 120 short recordings, including original/final
comparisons, 30/60 fps, both encoder preferences, four effect scenarios, three
content patterns, and additional geometries. These are coverage screens, not
ten-pair confirmation evidence. The endurance helper plays a known PCM tone
through the current default output without changing device or volume settings,
checks nonzero loopback audio first, and stops playback in `finally`.

Each recording preserves media, decoded source identifiers, workload
publication times, process memory samples, summary metrics and failure reasons.
Instrumented builds additionally preserve stage summaries, raw stage samples,
source/PTS traces, encoder admission/submission/packet events, and per-PTS
capture-to-first-packet latency. Handoff latency retains its earlier meaning
and must not be substituted for packet latency.

The current harness also writes `*-composition.csv`, joining composition
start/end to output PTS. Its signed nanosecond offsets use the same first
encoder-admission origin as `*-packets.csv`; negative offsets preserve
composition that began before that admission. Join these events to
`*-sources.csv` by output PTS. Older frozen binaries without this additive file
still preserve the earlier source, stage, packet and latency observations.

Schema 8 distinguishes `conversion_backend`, `configured_conversion_threads`
and `effective_conversion_threads`. The last field reads the initialized
FFmpeg context's thread setting and stays empty for an unresolved automatic
setting. In older schema 7 files, `effective_conversion_threads=0` was the
legacy-backend selector, not evidence of zero conversion workers.

Schema 10 adds `restoration_only` for the experiment selected by
`--restoration-only`. It restores recycled-buffer overlay coverage only when
the pristine background is unchanged, with full refresh and ordinary drawing
on changed backgrounds. It does not enable source-damage updates. Static
Auto-capture runs can transition from DXGI to WGC; `capture_backend` is the
last observed backend, and `effective_resize_threads` is the largest count
used during the recording. Do not interpret those two summary values as
evidence that one backend or worker count was used for every frame.

After timed runs finish, inspect one recording directory:

```powershell
python scripts/analyze-recording-run.py `
  build/recording-throughput/confirm-async-sw60/baseline-1 `
  --executable build/recording-throughput/binaries/prototype-v19-instrumented.exe `
  --window-seconds 30
```

The script saves raw media/audio inspection output and
`recording-diagnostics.json`. It reports cadence by media-time window, packet
latency when available, memory trends excluding initialization/teardown,
replacements, dropped audio, A/V endpoints and decoded audio signal/continuity.
Static content should not be judged against a moving-content fresh-fps target.
An audio track with packets but zero signal is insufficient evidence of
nonzero audio continuity. Audio inspection uses Windows Media Foundation's
installed AAC decoder because the deployed FFmpeg has no AAC decoder; this
does not affect the recording pipeline. Provide a known continuous system-audio source for
the five-minute endurance recordings.

Run the focused Python checks without starting a recording:

```powershell
python -m unittest scripts/test_compare_recording_results.py `
  scripts/test_run_recording_pairs.py scripts/test_analyze_recording_run.py
```

Current experiment status and interruptions are recorded in
`build/recording-throughput/STATE.md`. Prototypes and screening results do not
by themselves authorize new production defaults.
