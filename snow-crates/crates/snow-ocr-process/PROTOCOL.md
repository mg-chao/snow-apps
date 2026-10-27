# OCR protocol 7

Runtime 1.2.0 uses a binary stdin/stdout command channel. Diagnostics use stderr.
The application and runtime are released together; earlier protocols are incompatible.
All integers and IEEE-754 floats are little-endian. A string is a `u32` byte
length followed by UTF-8. Frames have a 20-byte header: `SOCR`, version `u16`,
kind `u16`, operation ID `u64`, payload size `u32`. Payloads are capped at 1 MiB.

| Kind | Name | Operation ID | Payload |
| --- | --- | --- | --- |
| 1 | Hello | 0 | Empty |
| 2 | Ready | 0 | Success `u8`, provider string (`unloaded`), runtime version string, protocol `u32` |
| 3 | Submit image | Recognition token | Mapping generation `u64`, width/height/stride `u32`, transfer sequence `u64` |
| 4 | Cancel | Recognition token | Empty |
| 5 | Complete | Recognition token | Recognition result, described below |
| 6 | Shutdown | 0 | Empty |
| 7 | ShutdownAck | 0 | Empty |
| 8 | PrepareSession | Session operation | Detector resize policy `u8` (0 = max, 1 = min), text detection processing `u8` (0 = accuracy first, 1 = speed first), hardware acceleration `u8` (0 = off, 1 = prefer GPU), detector/recognizer/dictionary path strings |
| 9 | SessionReady | Session operation | Success `u8` |
| 10 | ReleaseSession | Session operation | Empty |
| 11 | SessionReleased | Session operation | Empty |
| 12 | AttachBuffer | Mapping generation | File path string, total mapped bytes `u64` |
| 13 | BufferAttached | Mapping generation | Empty |
| 14 | Recognize | Recognition token | Empty |
| 15 | ImageConsumed | Recognition token | Mapping generation `u64`, transfer sequence `u64` |
| 16 | DetachBuffer | Mapping generation | Empty |
| 17 | BufferDetached | Mapping generation | Empty |
| 18 | DiscardImage | Recognition token | Empty |
| 19 | ConfigureAcceleration | 0 | Enabled `u8` (0 or 1) |
| 20 | AccelerationReady | 0 | Empty |

`Ready` establishes only the command channel. It does not open an image buffer,
probe a model, or create an inference session. Model creation and inference run
on one executor thread, which owns the session exclusively. The command thread
can copy the next image during inference. There is at most one active inference
and one staged, child-owned image. `Recognize` consumes that staged image using
the currently acknowledged session. The parent decides dispatch order and when
an idle cycle begins; an empty child command queue alone is not an idle signal.
Commands and executor completions wake the same event channel; reporting model
readiness and inference completion does not depend on a polling timeout.

Session replacement drops the previous request's mutable storage before creating
the next one. Repeating `PrepareSession` with the same configuration may share
immutable compiled models, packed weights, dictionary and CPU context. Idle hot
start detaches the image mapping, then sends `PrepareSession` directly when the
configuration is unchanged. `ReleaseSession` always drops all model resources;
configuration changes and disabling hot start still use it. Protocol 7 workers exclusively accept this session layout. Session preparation accepts no image. CPU inference uses simd-paddle-ocr-rs with a
bounded context and local V6 models, without orientation classification.
On multi-core workers, detector and recognizer parsing/compilation overlap on
two bounded tasks. Both are joined before preparation succeeds or fails.
Text detection processing selects the recognizer input-width floor: accuracy first uses
320, while speed first uses 48 before the existing 32-pixel width alignment.
Accuracy first is the default. Protocol 7 adds the acceleration preference to
PrepareSession and the separate acceleration control/readiness messages. Older
workers are rejected before decoding their incompatible payloads.

The host sends ConfigureAcceleration immediately after Ready and when the
preference changes. Enabling starts device discovery on a separate thread;
Ready, PrepareSession and Recognize never wait for that thread. Preparation
reads a nonblocking context snapshot and selects CPU if it is not ready. The
GPU compiler receives the existing context explicitly, preventing implicit
synchronous discovery. Initialization failures are cached for the worker lifetime.
AccelerationReady allows the host to promote an idle resident session, after
queued requests have drained. A nonresident cold request may finish on CPU.
Disabling invalidates pending discovery and releases the context after active
sessions drain; discovery threads are not joined at shutdown. GPU preparation
failures select CPU; GPU execution failures retry once on CPU unless cancelled.
Input/model/I/O errors are preserved. After a GPU failure, that worker stays on
CPU. Settings never implicitly enable residency or hot start.
Before acknowledging PrepareSession or ReleaseSession, the worker releases
unused GPU transfer, command, descriptor and native compilation caches. Live
model uploads and compiled kernels survive an idle hot-start reset. An empty
Vulkan allocator releases its retained blocks without recreating the device or
the process; allocator blocks for live models use OCR-sized slabs.
A failed session preparation leaves no usable session and does not exit the
process. Only real recognition requests receive user-facing errors.

An image mapping contains one 32-byte slot header followed by tightly packed
RGBA pixels. Header offsets are sequence `u64` at 0; state `u32` at 8; width,
height, stride, byte count, and magic `u32` at 12, 16, 20, 24, and 28. Ready state
is 1 and magic is `0x544f4c53`. The parent publishes pixels before marking the
slot ready. Dimensions must be positive, total pixels must not exceed
`3840 * 2160`, and stride must equal `width * 4`.

`ImageConsumed` proves the child has copied the pixels and no longer reads that
slot. The parent may then reuse its capacity. To resize or delete the mapping,
it sends `DetachBuffer` and waits for `BufferDetached`, which proves the child
has dropped its mapping. Only then may the parent unmap/delete the file. A new
mapping receives a new generation, with transfer sequences increasing from 1.
There is no mapping during warm-up or process-only startup.

Cancellation sets only the corresponding active inference's flag; an inference call
already executing may finish. `DiscardImage` removes a canceled staged image.
The parent suppresses canceled callbacks and waits for physical inference to
drain before releasing its session. Default-mode final-request cancellation may
still terminate the child; resident-mode cancellation never does.

`Complete` starts with status `u8`: 0 failure plus error string; 2 cancellation
plus message string; or 1 success plus reserved string, line count `u32`, then
each line's text string, confidence `f32`, and eight quad-coordinate `f32`s.
The parent preserves callback submission order even when local image rendering
finishes later. It validates process generation, session operation, mapping
generation, and image sequence before applying acknowledgements.
