# OCR and pin-export diagnostics (September 2026)

Scope: the user approved implementation of the follow-ups from the September 9
1.0.0-beta log review. Priorities are correctness, diagnosability, then responsiveness.
The existing OCR model, concurrency limits, request priorities, and wire protocol
remain compatible. No GPU speedup is claimed from the incident logs alone.

## Contract and acceptance

| Rule | Observable behavior | Verification |
| --- | --- | --- |
| OCR-1 | Submissions include operation, image dimensions, priority, queue delay, pending and running counts. | Controlled child lifecycle test reads the persisted JSON log. |
| OCR-2 | Completion/failure retains queue, worker round-trip and local render durations and a sanitized failure message. Cancellation records the stage once. | Lifecycle test covers repeated cancellation and unexpected child death. |
| OCR-3 | Pipe fragments form complete stderr lines, including split UTF-8. Structured worker events remain searchable events. | Controlled child sends a split UTF-8 error and an engine event. |
| OCR-4 | Initialization/provider-resolution/inference fallback records identify the operation, stage, backend, result and duration. | Worker build and structured-event relay test; actual driver failures require field verification. |
| OCR-5 | A cancellation observed after initialization prevents inference; cancellation also prevents starting CPU fallback work. | Rust cancellation-checkpoint test exercises skipped work, success and error propagation. |
| PIN-1 | Image materialization reports its original failure before conversion to an empty image; pin first-frame completion shares its opaque operation ID. | Artifact test reads the failure message and operation from persisted logs. |
| PIN-2 | Cancelling pending image/encoding work reports cancellation; normal destruction after completion does not imply failure. | Existing artifact cancellation tests and cancellation diagnostic coverage. |

## Reading the next logs

Join OCR events by application `session`, worker `child_pid` where present, and
`fields.operation`. Request IDs are only unique within a service instance; the PID
distinguishes child generations. `ocr.process_ready` reports the capability probe,
not proof that a model actually used the GPU. Use `ocr.engine_ready` and
`ocr.backend_fallback` for actual engine selection and recovery.

Application `ocr.finished` / `ocr.failed` timings:

- `queue_ms`: request creation through shared-memory submission, including process
  readiness and image preparation. A request never submitted reports time waiting.
- `worker_ms`: submission through receipt of completion, including child queue,
  initialization, inference and IPC. This is not pure inference time.
- `render_ms`: post-worker rendering/delivery interval. For render-only requests,
  this is the entire local task interval and the other two fields are zero.

Worker `ocr.worker_finished` has its own queue wait and worker execution duration.
Its `initialization_ms` includes initial setup and CPU fallback setup; `inference_ms`
is time in the OCR calls and result conversion, including a CPU retry if needed.
`ocr.engine_ready.duration_ms` measures initialization. Fallback stages are
`provider_resolution`, `initialization`, `cpu_initialization`, `inference`, and
`retry`; a successful CPU initialization alone does not prove successful inference.
The final worker/request result resolves the operation outcome.

`ocr.process_failed.stage` distinguishes `ready_timeout`, `protocol_frame`,
`handshake`, and `process_exit`. A subsequent OS `crashed` exit can be the parent's
forced termination after one of these failures; inspect the preceding stage event
before classifying it as a spontaneous native crash. Affected request completions
retain their worker PID, even when startup failed before submission.

Join `export.image_finished`, `export.cancelled`, and `pin.finished` by their opaque
artifact operation ID. A failed image event supplies the original materialization
error. A successful image followed by a failed `first_frame` points to presentation
or lifecycle handling. Cancellation is a distinct event; failed presentation alone
does not establish its underlying reason.

No image bytes, recognized text, model input, or document content is added to
metadata. Existing message sanitization and field allowlisting still apply.
Stderr is bounded into 8 KiB records; unusually long lines may be split. The existing
global queue and retention limits still apply.

## Delivery and remaining verification

Application-only updates provide request timings, complete stderr lines and pin
correlation even with the previous OCR runtime. Worker-stage events and cancellation
checkpoints require the rebuilt OCR executable. Package/publish an immutable new OCR
runtime version with matching hashes before distributing the next online installer;
never replace a published archive under an existing version.

The release owner should verify one normal OCR request, cancellation during model
startup, CPU recovery on the affected GPU/driver, and a failed/cancelled pin on the
next candidate. Compare the same request's queue and worker intervals before making
model or concurrency changes. Hardware-specific DirectML recovery and field latency
remain runtime validation items, not claims established by the unit tests.

The changes require no user-data migration. Rollback is the previous application
and its matching runtime manifest. Additional logs are additive; no log consumer
should infer a crash merely from a cancelled child exit or historical dump listing.

## Verification record

- Targeted Debug builds succeeded for OCR recognition, export artifacts, and pinned
  windows. Diagnostics, controlled OCR lifecycle (including unexpected child death),
  artifact diagnostics/cancellation, and asynchronous pin presentation tests passed.
- `snow-ocr-process`: six unit tests and one CLI integration test passed; targeted
  Clippy with warnings denied and Rust formatting checks passed.
- The real CPU recognition service test passed (11.39 s) with `ORT_DYLIB_PATH`
  selecting `.tools/vcpkg/installed/dynamic/x64-windows/bin/onnxruntime.dll` and that
  Release dependency directory prepended to `PATH`. This is a diagnostic test
  environment override, not a packaging change or a performance benchmark.
- The default Debug dependency set remains failing: loading ONNX Runtime 1.28.0
  triggers a CRT assertion in `libprotobufd.dll` (`__msvc_string_view.hpp:1528`,
  null pointer with nonzero size) during DLL initialization. A debugger confirmed
  the worker waiting in the assertion message box before readiness; the parent
  then reaches its five-second readiness timeout. No assertion was suppressed.
  The build/dependency owner must resolve that Debug dependency issue separately;
  it is not evidence that the same assertion occurs in the released application.
- No installer was produced or published. The release owner must complete the new
  immutable OCR runtime version and manifest/hash update, then verify the actual
  packaged candidate. DirectML driver recovery remains unverified on the affected
  user's hardware.
