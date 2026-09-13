# Recording optimization evidence

**In progress: validated resize and restoration defaults are enabled in restricted
domains; hardware cumulative and endurance validation remains.** Final scoped
Rust and Qt integration checks passed for v19. Screening results are
configuration selection evidence, not acceptance evidence. Raw results and
frozen executable/source identities are preserved in the ignored
`build/recording-throughput/` directory. See [the protocol and commands](recording-performance.md).

## Confirmed isolated experiment

Four resize workers, software H.264, 60 fps, all effects, continuous desktop,
3840×2160 capture to 1920×1080 output, Auto capture resolving to DXGI:

| Comparison | Baseline fresh fps | Candidate fresh fps | Paired relative change, 95% CI |
|---|---:|---:|---:|
| Resize 1 → 4 workers, instrumented | 31.1519 | 34.8986 | +12.0665% [10.1973%, 13.9675%] |
| Resize 1 → 4 workers, timing disabled | 31.9452 | 34.8851 | +9.0434% [6.1712%, 11.9934%] |

All ten pairs are valid. The mean per-recording combined-resize median fell
from 9.1193 to 4.5831 ms; paired reduction 49.7387%, 95% CI
[47.9954%, 51.4236%]. Actual resize-pixel work fell from 6.5491 to
2.6212 ms. Capture-to-first-packet p95 fell from 480.0352 to 419.5284 ms,
paired difference −60.5068 ms, 95% CI [−70.8968, −50.1168] ms. CPU rose
8.4513%; private-memory difference was inconclusive. Workload publication
cadence fell by 1.9630 fps, so the recorded fresh-throughput gain is not
explained by a faster fixture. Artifacts: `confirm-resize4-sw60-v11/` and
`confirm-resize4-sw60-v12-no-timing/`. Timing-disabled confirmation also has
ten valid pairs; CPU rose 8.0826%, and private-memory difference was
inconclusive. Four workers pass the isolated acceptance gate in this domain
and become the software comparison baseline for subsequent candidates.
The v19 automatic policy selects this measured domain; final coverage and
endurance validation remain.

Hardware-preferred H.264 (opened `h264_mf`, AMD hardware MFT), otherwise the
same geometry, rate, effects and workload, also passes the isolated resize gate:

| Comparison | Baseline fresh fps | Candidate fresh fps | Paired relative change, 95% CI |
|---|---:|---:|---:|
| Resize 1 → 4 workers, instrumented | 40.5585 | 44.4813 | +9.6643% [4.7313%, 14.8297%] |
| Resize 1 → 4 workers, timing disabled | 39.5421 | 42.9212 | +8.6893% [3.7101%, 13.9075%] |

Both comparisons have ten valid pairs. Instrumented combined-resize median
means fell from 6.8040 to 3.3613 ms. Capture-to-first-packet p95 fell from
166.6779 to 146.3642 ms; paired difference −20.3137 ms, 95% CI
[−25.5872, −15.0402] ms. CPU rose about 6.0–6.1%. The instrumented peak-private
memory reduction was supported; the timing-disabled memory difference was
inconclusive. Artifacts: `confirm-resize4-hw60-v13/` and
`confirm-resize4-hw60-v14-no-timing/`.

Software H.264, 60 fps, all effects, continuous desktop, 3840×2160 capture to
1920×1080 output, Auto capture resolving to DXGI:

| Comparison | Baseline fresh fps | Candidate fresh fps | Paired relative change, 95% CI |
|---|---:|---:|---:|
| Encoder worker, instrumented | 31.5622 | 35.0550 | +11.1398% [7.6806%, 14.7101%] |
| Encoder worker, timing disabled | 33.9486 | 38.2851 | +12.8887% [6.7126%, 19.4222%] |

Each comparison contains ten valid independent alternating pairs with five
seconds warmup and 30 seconds measured recording per member. The instrumented
comparison's recording-level mean capture-to-first-packet p95 fell from
476.4318 to 376.3202 ms: paired difference −100.1116 ms, 95% CI
[−116.6711, −83.5521] ms. This satisfies the one-output-interval allowance in
this configuration. Timing-disabled CPU rose from 502.2548% to 573.8865%
(summed across cores); peak private memory changed from 673.1907 to
679.3949 MiB, with an absolute difference CI spanning zero.

Artifacts: `confirm-async-sw60/` and
`confirm-async-sw60-v7-no-timing/`. The isolated experiment passes the
throughput/latency gate. Production acceptance still requires validation
against any preceding accepted optimizations, the remaining configuration
coverage, and audio/endurance checks. These results are not cumulative gains
against the original source. The subsequent combination failed the throughput
gate, as detailed below, so the worker is excluded from production.

## Candidate status

Static overlay restoration passes an isolated compositor-stage gate in
`confirm-static-restoration-after-resize4-hw60-v17/`: ten valid alternating
pairs, five seconds warmup and 30 seconds measurement, four resize workers,
hardware H.264, 60 fps, all effects, 4K-to-1080p. Auto capture transitions to
WGC after the desktop stops changing; these are not DXGI-only recordings.

| Metric | Full restoration | Partial restoration | Paired result, 95% CI |
|---|---:|---:|---|
| Restoration median | 0.8002 ms | 0.0606 ms | 92.4329% reduction [91.6415%, 93.1495%] |
| Combined resize/restoration median | 0.8037 ms | 0.0629 ms | 92.1794% reduction [91.3588%, 92.9221%] |
| Output frames in 30 seconds | 1795.8 | 1798.3 | +2.5 frames [−0.5953, 5.5953] |

Every recording preserves exactly one unique source identifier. Fresh-source
fps differs only with measured wall-clock duration; its relative interval
spans zero. Output averages 59.86 and 59.94 fps, close to the requested 60-fps
cadence including startup. CPU and private-memory changes are inconclusive.
This supports static processing headroom, not increased fresh-source throughput.
The separated restoration-only prototype skips coverage allocation/drawing
when backgrounds change and leaves source-damage updates benchmark-only.
The separated implementation also passes its final isolated gate in
`confirm-restoration-only-after-resize4-hw60-v18/`, with ten valid pairs:

| Metric | Full restoration | Restoration only | Paired result, 95% CI |
|---|---:|---:|---|
| Restoration median | 0.7130 ms | 0.0526 ms | 92.6680% reduction [92.0117%, 93.2704%] |
| Combined resize/restoration median | 0.7157 ms | 0.0550 ms | 92.3638% reduction [91.6692%, 93.0005%] |
| Output frames in 30 seconds | 1798.0 | 1797.8 | −0.2 frames [−1.5822, 1.1822] |
| CPU, summed across cores | 125.5788% | 120.8053% | 3.8286% reduction [2.1281%, 5.4994%] |

Every recording retains one unique source identifier; the fresh-fps relative
interval spans zero. Memory changes are inconclusive. Deterministic exact-pixel
tests cover recycled buffers, cursor movement, effect expiry, pause/resume and
geometry changes. Timing-disabled static screens preserve source identity and
cadence (717 versus 716 frames over 12 seconds). Automatic-selection screening
reports restoration enabled in the intended hardware case. Changing-content
screens are mixed and establish no throughput claim: continuous 44.4109 →
46.6666 fps, sparse 45.6609 → 44.7450 fps.

The v19 production defaults select four resize workers only for the validated
60-fps 4K-to-1080p H.264 domains, with actual DXGI frame checks and preserved
explicit resize overrides. Restoration additionally requires hardware `h264_mf`
and configured Auto capture; each frame must use DXGI or WGC at the expected
geometry. It tracks/restores coverage only while the background is unchanged.
Software fallback, other rates and geometries retain their restoration behavior.
Final cumulative and endurance validation remains outstanding.

Four conversion workers after the accepted four-resize-worker configuration,
software H.264 at 60 fps, all effects and continuous 4K-to-1080p DXGI capture,
pass the instrumented gate (`confirm-conversion4-after-resize4-sw60-v15/`).
All ten independent pairs are valid:

| Metric | Legacy conversion | Four-worker frame API | Paired result, 95% CI |
|---|---:|---:|---|
| Fresh fps | 36.3188 | 38.4723 | +5.9295% [3.0838%, 8.8539%] |
| Total conversion median, including input copy | 8.7820 ms | 5.7378 ms | 34.6792% reduction [33.0537%, 36.2651%] |
| Capture-to-first-packet p95 | 408.3051 ms | 367.8002 ms | −40.5049 ms [−55.2643, −25.7455] |

CPU rose 7.2456%; the peak-private-memory difference was inconclusive
(+5.0669 MiB, CI [−6.0722, 16.2060]). Workload publication cadence fell from
54.8682 to 50.9353 fps, so faster fixture publication does not explain the gain.
Timing-disabled confirmation (`confirm-conversion4-after-resize4-sw60-v15-no-timing/`)
also has ten valid pairs. Fresh fps rose from 37.8250 to 39.1188, but the
preselected paired log-ratio interval includes zero: +3.4861%, 95% CI
[−0.0116%, 7.1062%]. The absolute paired delta interval is positive
(+1.2938 fps, CI [0.0469, 2.5407]), but switching the acceptance statistic after
seeing the result would weaken the protocol. The conversion experiment is
therefore **inconclusive and excluded from production builds**. Subsequent
software candidates retain four resize workers with legacy conversion as
their baseline. CPU rose 8.4471% in the timing-disabled comparison.

| Mechanism | Current status | Evidence and next decision |
|---|---|---|
| Resize workers | Accepted in the restricted SW60 and HW60 domains | Four independent ten-pair comparisons above; automatic selection validated and enabled in v19. |
| Encoder thread limit | Inconclusive; unchanged | Earlier 1/2/4-worker screens did not support replacing automatic SW60 selection. Forced hardware fallback at 30 fps did not support imposing a two-thread limit. |
| Capture pacing | Inconclusive; unchanged | Earlier aligned-capture screens did not improve fresh throughput. No waiting/timer change is justified by current evidence. |
| Direct output without overlays | Inconclusive; disabled | Latest SW60 screen 35.4151 → 35.8270 fps; HW60 38.8274 → 39.2429. Small screen differences do not establish a benefit. |
| Exact 2:1 resize | Inconclusive; disabled | Latest SW60 screen 34.1658 → 35.2475 fps; HW60 43.1611 → 42.4153. Earlier attempted confirmation also had invalid recordings and an interval spanning zero. |
| Source partial updates | Inconclusive; excluded from production | Ten sparse HW60 pairs after resize4: 49.8740 → 50.5945 fps, +1.5134% CI [−1.7744%, 4.9113%]. Combined resize and restoration stage intervals also span zero. The measured DXGI region path exposes unknown damage for changed frames. |
| Unchanged-background overlay restoration | Accepted in the restricted hardware domain | Separated v18 ten-pair stage confirmation above; exact pixels and timing-disabled cadence checks pass. |
| libswscale frame conversion | Inconclusive; excluded from production | Instrumented relative gain supported; the preselected timing-disabled relative interval narrowly spans zero. Both ten-pair results are above. HW60 screening offered little throughput evidence. |
| Encoder worker | Inconclusive after resize4; excluded from production | Ten valid SW60 pairs: 37.6620 → 36.9251 fresh fps, −2.0315% CI [−4.7668%, 0.7824%]. Actual capture-to-first-packet p95 improves 393.7573 → 353.9096 ms, difference −39.8477 ms CI [−50.5827, −29.1127]. CPU rises 8.7229%. The throughput gate fails; no timing-disabled follow-up is justified. |
| Omit unused cursor attachment | Inconclusive; disabled | Latest no-effects screens show lower fresh throughput for both encoder preferences. No default change. |
| Keyboard cache | Unchanged | Existing cache is bounded and reused. Latest SW60 all-effects screen has 28 misses and 1,707 hits; combined keyboard stage p95 1.720 ms, max 7.766 ms. Cache hit/miss timing entries represent counts and must not be interpreted as rasterization times. |

The direct-output, half-resize, cursor and keyboard screen figures above use
frozen `prototype-v11-instrumented.exe`. The latest partial/worker screens are
in `screen12/` using frozen v16; corrected static fixture screens are in
`screen8/` and `screen9/` using v12 and v13.
Screens use two seconds warmup and 12 seconds measurement. They are not
independent ten-pair confirmation trials.

## Correctness and integration evidence

Final v19 affected-crate checks passed: effects 37 tests, export 61 tests,
runtime 92 instrumented tests and 86 default-feature tests; effects/export
each retain one existing ignored benchmark.
Scoped all-targets Clippy passed for instrumented and timing-disabled builds,
and production library Clippy passed. The latest fixture revision passes
scoped Clippy in both feature configurations; the eight Python analysis and
pair-runner tests pass. The v16 check passes ten Python tests. Both v16
benchmark feature configurations and normal production libraries pass scoped
Clippy. New tests cover fixed resize-domain dispatch, explicit zero encoder
overrides, dense/overlapping damage work limits, and fully initialized input
padding after FFmpeg replaces shared AVFrame storage.

Conversion validation compared 120 exact visible-plane outputs over five
geometries, eight supported pixel formats, and three worker counts. Worker
ownership/lifecycle tests cover slow encoding, replacement of waiting frames,
buffer history, cancellation and failed-worker acknowledgments. For the
worker's fixed common text/edge/motion fixture and identical admitted PTS,
inline and threaded H.264 packets are identical.

The performance Release C FFI bundle and relevant Snow Shot targets were
rebuilt for v19 (`qt-build-v19.log`). Exactly the requested controller,
effects-preview and recording-workflow CTests passed (`ctest-recording-v19.log`).
Final scoped instrumented/no-timing all-targets and production-library Clippy
checks, formatting, and ten Python tool tests passed. All 15 installed and
frozen executable FFmpeg DLL hashes match (`runtime-identity-v19.json`). No Cargo workspace
test suite or unfiltered CTest suite was run.

## Invalid measurements and fixture corrections

Competing builds invalidate a recording even when its media and identifiers
are correct. Resume retains complete valid pairs and archives both members of
an incomplete pair. Earlier obstructed/unreadable workload recordings remain
invalid; they have not been reclassified or selected by their performance.

The static fixture originally painted before capture startup and never
published again. Its failed warmup produced a media file without a video
stream (`screen7/static-full/`). DXGI first-presentation requirements and
pre-clock observation filtering make that setup unsuitable for testing a
static recording. The corrected fixture requests one initial presentation
after each session starts, then remains unchanged. Both v12 static screens
pass with readable identifiers and one measured workload publication. The
session report now survives offline media-decode failures.

## Remaining validation

Isolated decisions are complete, with two defaults enabled only in measured
domains and experimental mechanisms excluded from production. The original
594c7191 pipeline is frozen with the same schema-10 harness and FFmpeg identity.
Complete the hardware cumulative comparison, scenario/geometry matrix and four five-minute all-effects,
nonzero-system-audio recordings, then assess cadence windows, actual packet
latency, A/V endpoints, dropped audio and bounded memory/queue behavior.

## Cumulative comparison with the original pipeline

Software H.264, 60 fps, all effects, continuous 4K-to-1080p, ten independent
alternating pairs per build configuration:

| Comparison | Original fresh fps | Final v19 fresh fps | Paired relative gain, 95% CI |
|---|---:|---:|---:|
| Instrumented | 31.4122 | 34.5589 | 10.0322% [6.0252%, 14.1905%] |
| Timing disabled | 31.0654 | 35.1923 | 13.2786% [12.5210%, 14.0413%] |

Instrumented capture-to-first-packet p95 falls from 485.3229 to 428.7071 ms;
paired difference −56.6158 ms [−69.0137, −44.2179]. Combined-resize median
falls from 9.0753 to 4.6442 ms. Conversion is statistically unchanged
(9.1019 to 9.0080 ms). CPU increases approximately 6.61% with instrumentation
and 8.33% without it; peak-private-memory differences are inconclusive.
Artifacts: `cumulative-sw60-v19/` and `cumulative-sw60-v19-no-timing/`.
These cumulative gains are measured directly, never summed from isolated stages.

## Current policy and harness findings

The v15 automatic-resize startup probe did not select four workers in any of
four short screens (`screen10/`: software/hardware, all-effects/application-default,
60 fps). Every run reported one worker. Auto capture selects its backend lazily;
encoder initialization can complete before a frame is available. This probe is
inconclusive as an integration mechanism, regardless of the explicit-pool gains.

The replacement allocates a fixed pool before acknowledging
startup, with an immutable eligibility rule for actual DXGI frames at the measured
source/output geometry. Unsupported frames use the preceding serial path. This
does not wait for a first frame or resize the pool during recording. Production
defaults were enabled in v19 after isolated and dispatch validation. The
four v16 screens (`screen11/`) all report four effective resize workers, with
readable identifiers and the expected actual encoder/backend identities. They
validate policy dispatch; they are not paired performance confirmations.

Review also found that the benchmark's explicit `--encode-threads 0` was
indistinguishable from an omitted override in v15 and earlier. In the existing
30-fps software policy domain, it retained the application's two-thread cap
instead of requesting the exporter's automatic worker count. Corrected screening must use
an optional override: omission preserves application defaults, explicit zero
requests automatic worker selection. Earlier 30-fps thread screens must not be
used as evidence for a true automatic-versus-two comparison. The 60-fps resize
and conversion comparisons are unaffected, because the preceding application's
encoder setting in that domain was already automatic.

DXGI region capture currently returns an empty `dirty_rects` vector for changed
frames (`read_region_slot_into_output`, including its blocking variant). The
capture contract treats changed frames with empty damage as unknown damage.
The compositor therefore performs a complete resize rather than assuming the
backend's internal sparse readback data is complete damage for an external
consumer. Accumulating observed captures cannot recover unavailable damage.
Selective background restoration can still apply to repeated backgrounds and
overlay animation. A future source-damage optimization needs a verified capture
damage contract before it can improve this path. No capture metadata behavior
was changed in this experiment.

The v17 encoder builder separates software-fallback thread selection from the
initial hardware setting. It applies that selection when the actual software
codec opens, including discovery fallback, and preserves explicit Rust
overrides (including zero for the exporter's automatic physical-core policy).
The automatic fallback count remains unchanged; no unsupported two-thread
fallback default is introduced. Deterministic tests exercise forced hardware
initialization failure and preservation of explicit overrides.

Worker confirmation artifacts are in `confirm-async-after-resize4-sw60-v16/`.
The initial attempt overlapped a compiler build; a later candidate warmup
failed with disk-full error 112. Both invalid attempts are retained, and the
complete valid pairs were preserved before resuming. Completed hardware resize
and sparse-partial artifacts were moved with per-file SHA-256 verification to
`C:/Users/Magic/.codex/recording-benchmark-storage/snow-apps-594c7191/`.
Directory junctions preserve their original ignored build paths; transfer
manifests remain alongside the benchmark results.
