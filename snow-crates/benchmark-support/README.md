# Memory optimization benchmarks

These examples run deterministic workloads through production APIs. They compile
against both the memory optimization branch and its pre-optimization revision. Each scenario
must run in its own process. Build both checkouts with the same Rust toolchain,
workspace release profile, third-party dependency versions, feature set, and environment.

For this branch the full comparison is `5aff3a15` (the merged main branch before
memory optimization) against `8c90efb6`. The intermediate `865805a0` already uses
page-backed raster storage; comparing against it measures only the later copy
reductions. Do not label that comparison as the complete branch improvement.
The lockfiles differ by the added local `snow-memory` crate and references to it;
all registry/Git package identities and checksums should match. Use each revision's
locked file and record both hashes rather than substituting a mismatched lockfile.

```sh
cd snow-crates
cargo build --release --locked --no-default-features \
  -p snow-capture --example memory_buffer_benchmark \
  -p snow-recording-model --example memory_decode_benchmark \
  -p snow-stitch-images --example memory_paths_benchmark --example memory_snapshot_benchmark \
  -p snow-ocr-process --example memory_transfer_benchmark \
  --features snow-ocr-process/dynamic-onnx-runtime
```

Use an explicit, separate `CARGO_TARGET_DIR` for each revision; executables are
then in its `release/examples/` directory. Invoke each executable with `SCENARIO SAMPLES`.
The default is 31 samples; three warmups precede measurement. Debug builds are
rejected. Baseline copies need the five example sources and this shared directory;
they do not require manifest changes.

| Executable | Scenario | Timed work per sample |
| --- | --- | --- |
| `memory_buffer_benchmark` | `capture-reuse` | Reuse and overwrite one 3840 × 2160 RGBA capture buffer |
| `memory_buffer_benchmark` | `capture-resize` | Shrink by 64 rows, overwrite, regrow within capacity, overwrite |
| `memory_buffer_benchmark` | `capture-cow` | Clone one 4K frame and write a byte, preserving the original |
| `memory_buffer_benchmark` | `capture-import` | Copy packed 4K pixels to Vec and import through `Frame::from_rgba8` |
| `memory_buffer_benchmark` | `capture-churn` | Release and recreate eight fully written 4K buffers |
| `memory_paths_benchmark` | `stitch-repaint` | Replace the last 128 rows of a 3840 × 21600 canvas eight times |
| `memory_paths_benchmark` | `stitch-retained` | Same replacements while retaining eight immutable snapshots |
| `memory_paths_benchmark` | `stitch-horizontal` | Replace 128 columns of a 21600 × 1920 canvas eight times |
| `memory_paths_benchmark` | `stitch-export` | Export an unaligned long snapshot in batches of 64 rows |
| `memory_paths_benchmark` | `stitch-materialize` | Materialize all pixels of the long vertical canvas |
| `memory_paths_benchmark` | `stitch-orb` | Estimate a 64-pixel upward content shift at 1920 × 1080 |
| `memory_snapshot_benchmark` | `stitch-small-snapshot` | Create and drop an unaligned 128-row snapshot; separately retain it after dropping the canvas |
| `memory_decode_benchmark` | `recording-decode` | Release and decode eight 1080p StoredFrames from unchanged bincode bytes |
| `memory_transfer_benchmark` | `ocr-transfer` | Validate a real 4K shared-memory slot and convert its RGBA pixels to BGR |

Output is JSON Lines. Sample records contain `scenario`, dimensions, zero-based
`iteration`, `elapsed_ns`, `checksum`, and `logical_bytes`. Phase records report
`empty`, `setup`, `live`, and `drop` with the following metrics:

- `rss_bytes`: current process resident memory on macOS and Linux, or null on
  other platforms. These checkpoints include mappings and allocator retention.
- `heap_live_bytes`, `heap_peak_bytes`: live and peak requested System allocator
  bytes. OS page mappings are excluded. The peak resets after warmups, before
  samples, and includes the live setup payload.
- `heap_allocated_bytes`, `heap_allocations`: total requested System allocator
  traffic during the measured samples. Reallocation counts as one allocation.
- `logical_bytes`: intended live image payload, excluding capacity headroom,
  temporary outputs, spare tiles, row staging, and metadata. The tiny snapshot adds a
  `snapshot-retained` checkpoint before materializing its validation output.

The live checkpoint precedes printing sample records. The drop checkpoint follows
release of every payload owned by the workload. Warmups deliberately condition the
allocator, so drop RSS measures retention after sustained work. Collect process
peak RSS externally as well: a live checkpoint can miss transient allocation
peaks. Heap traffic alone cannot establish a space reduction when storage moves
from the heap to OS mappings. Counting allocator atomics are enabled equally on
both revisions and add a small amount of overhead to allocation-heavy operations.

Checksums sample each 4096th byte and the last byte. Workloads additionally assert
dimensions, sentinel pixels, clone isolation, and motion detection. Compare exact
paired checksums before interpreting timing changes. The ORB fixture uses four-pixel
texture blocks; equally precise offsets can tie within two pixels of -64, so its
checksum also includes the actual offset, keypoint counts, and match count.

The tests cover CPU buffer ownership, tile reuse and snapshot leases, snapshot
export, the complete motion estimator, recording model deserialization, and OCR
staging. They do not measure native screen capture, ONNX inference, hardware
encoding, FFmpeg export, or recording overlay compositing. Those paths require
separate platform fixtures and should not inherit conclusions from these results.

The materialize workload replaces a retained previous result. Its transient peak
includes the canvas, old result, and new result, and its timing includes destruction
of the old result. This models repeated result replacement; it does not measure
the minimum memory for a single export.

## Complete comparison including copy reductions

The additional fixtures measure the later production copy reductions and can
also run against the full pre-optimization baseline.
Copy the same sources and shared support directory into the comparison checkout.
The reference example also needs its `[[example]]` entry with
`required-features = ["perf-instrumentation"]` in that checkout's Cargo manifest.

```sh
CARGO_TARGET_DIR=/absolute/path/to/output cargo build --release --locked -j4 \
  --no-default-features \
  -p snow-capture --example memory_buffer_benchmark --example memory_overwrite_benchmark \
  -p snow-stitch-images --example memory_paths_benchmark \
    --example memory_snapshot_benchmark --example memory_reference_benchmark \
  -p snow-stitch-images-c --example memory_png_benchmark \
  -p snow-recording-model --example memory_decode_benchmark \
  -p snow-ocr-process --example memory_transfer_benchmark \
  --features snow-stitch-images/perf-instrumentation,snow-ocr-process/dynamic-onnx-runtime
```

Copy all eight example sources and `benchmark-support/memory.rs` unchanged into
the baseline checkout. The pre-optimization stitch crate already has the same
`perf-instrumentation` feature and production timing scopes; only the example
registration is needed. Save the copied-source hashes, baseline-only manifest
patch, build commands, compiler version, deployment target, lockfile hashes, and
binary hashes with the results. Keep the production source at its exact revision.
On macOS use the same `MACOSX_DEPLOYMENT_TARGET=15.0` for both builds and the same
`RAYON_NUM_THREADS=4` when running the paired processes. Run the preserving capture
mode as a control alongside the overwrite mode.

Prefer separate target directories for comparison checkouts. When sharing a target
directory, freeze each version's executables and force the changed packages to
rebuild with scoped `cargo clean --release -p <package>` when switching checkouts;
do not rely on source timestamps alone. Confirm the capture metadata says
`compatibility_fallback: true` before and `false` after the overwrite fix.

| Executable | Arguments | Measured operation |
| --- | --- | --- |
| `memory_overwrite_benchmark` | `overwrite 20` | Clone a shared 4K frame, prepare a complete replacement, and copy all source bytes; the earlier API uses preserving preparation through a compatibility trait |
| `memory_overwrite_benchmark` | `preserve 20` | Same full replacement through the preserving API, as an unchanged control |
| `memory_reference_benchmark` | `stitch-reference-vertical 20` or `stitch-reference-horizontal 20` | Clone incoming input and call real `Stitcher::push` while moving within a bounded canvas; separately report inclusive production scopes |
| `memory_png_benchmark` | `scrolling-png-export 7` | Actual asynchronous C ABI export of a uniform 3840×21600 RGBA snapshot using Fast compression, including polling, file read/checksum and durable commit |

The reference fixture verifies the complete final canvas outside timing and checks
motion decisions/keypoint diagnostics in every sample. Scope times overlap and
must not be added together. The PNG fixture validates every decoded RGBA byte
row by row outside timing. It samples RSS once during each export, including
warmups; querying and printing that checkpoint are inside its end-to-end scope.
The sampling and 1 ms polling interval are identical before and after.

PNG setup can dominate whole-process peak RSS. To compare the resident storage
observed during encoding, invoke the summarizer with
`--sampled-memory-phase encoding`. It reports `encoding_max`: the maximum sampled
checkpoint within each process, followed by the median across process maxima.
This is a sampled encoding checkpoint, not a guaranteed transient encoding peak.
Ordinary duplicate memory checkpoints remain errors. The opt-in and aggregation
semantics are saved in summary provenance; raw records are retained unchanged.
