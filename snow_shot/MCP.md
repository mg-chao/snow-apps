# Snow Shot MCP

Snow Shot exposes application controls, screenshots, background documents,
recording, pinned images, settings, history, and provider operations through the packaged Rust
`snow-shot-mcp` executable. It uses the official `rmcp` SDK over stdio. Start Snow
Shot, open **Settings → System**, and enable **MCP integration**. The feature is
disabled by default. The settings page shows the live client count, descriptor
location, and a copyable configuration for your installed executable.

## Client setup

Use the executable beside `snow_shot.exe` on Windows. Development builds stage it
in `build/windows-msvc-debug/snow_shot/Debug/`. On macOS it is inside
`Snow Shot.app/Contents/MacOS/` (the build tree bundle is named `snow_shot.app`).
Use your installation's absolute path.

Claude-style configuration:

```json
{
  "mcpServers": {
    "snow-shot": {
      "command": "D:\\Snow Shot\\bin\\snow-shot-mcp.exe",
      "args": []
    }
  }
}
```

Codex-style stdio configuration:

```toml
[mcp_servers.snow_shot]
command = 'D:\Snow Shot\bin\snow-shot-mcp.exe'
args = []
```

On macOS set `command` to, for example,
`/Applications/Snow Shot.app/Contents/MacOS/snow-shot-mcp`.
The bridge leaves application startup to you by default. Add `"--launch-app"` to
`args` to allow one launch attempt of the sibling Snow Shot executable. This does
not enable MCP integration: enable it in the application first. The bridge waits
up to ten seconds for the authenticated endpoint and never replays a mutation.
If the application is stopped or integration
is disabled, `snow_shot_status` returns `reachable: false`, `mcp_enabled: null`,
and the `unavailable` error. It cannot distinguish those two conditions without a
live authenticated endpoint. A later call discovers the endpoint again.

## Tools and workflow

1. Call `snow_shot_status` for protocol, application version, session ownership,
   and supported tool names.
2. Call `screenshot_begin` with optional `presentation` (`visible`, the default,
   or `silent`), `target` (`all_displays`, `monitor`, `current_monitor`, or
   `focused_window`), `monitor_id`, `capture_cursor`, and `smart_selection`.
3. Retain the returned `session_id` and `revision`. Every subsequent edit or
   output call requires both `session_id` and `expected_revision`. State and
   cancellation do not require a revision.
4. Use `screenshot_set_selection`, `screenshot_set_tool`, `screenshot_set_selection_style`,
   `screenshot_set_tool_style`, `screenshot_edit_elements`, `screenshot_apply_annotations`,
   `screenshot_undo`, and `screenshot_redo`.
5. Use `screenshot_scrolling` to start/stop scrolling, change axis, set automatic scrolling,
   move the selection, or trim the stitched result. Use `screenshot_scroll_once` with `up`,
   `down`, `left`, or `right` for one native wheel notch. The call returns `direction` and
   `dispatch_status: "posted"` as soon as native input is posted. Capture and stitching continue
   asynchronously; this response does not guarantee changed or settled content. Query
   `screenshot_state` for current scrolling state and use the output tools when ready to export.
6. Use `screenshot_recognize`, `screenshot_translate`, or `screenshot_auto_filter`; each returns
   an operation ID. Poll `screenshot_operation` and use `screenshot_edit_recognition` or
   `screenshot_export_recognition` for results. These operations use configured providers and
   also work in silent sessions.
7. Use `screenshot_render`, `screenshot_save`, `screenshot_copy`, or
   `screenshot_pin`. Continue with the new revision returned by each output.
8. Call `screenshot_finish` to close the capture and release ownership. Set
   `output` to `render` or `save` for a final output; absent/`none` closes directly.
   `screenshot_cancel` cancels capture or editing. With `request_id`, it cancels
   that pending operation and retains the editor, except an unfinished begin.

`screenshot_state` reports the current capture phase, region, canvas tool,
undo/redo state, display mapping, revision, and pending operation. User edits in
visible sessions increment the revision. A stale call returns `stale_revision`
and the current state in `error.details.state`; refresh and decide what to do
before sending another edit.

`screenshot_direct_capture` takes `target: current_monitor` or `focused_window`
and `output: render`, `save`, or `copy`. It uses the existing direct native
capture path, without an editor. It accepts the applicable output options and
`capture_cursor`, and releases its temporary session when output completes.
Only one MCP session can own the screenshot editor globally. Another client
receives `busy`; it cannot read or mutate another client's session.

A visible session survives client disconnect as user-owned screenshot work.
Silent sessions cancel on disconnect or when integration is disabled. Disabling
integration closes all clients and removes the descriptor. Escape and normal UI
cancellation invalidate the session and cancel any pending MCP output.

The original 28 tools retain their names and request/response contracts. New tools
use `snow_shot_<domain>_<verb>` names. `tools/list` supplies typed input schemas,
response-envelope schemas and annotations. The checked
[`mcp-capabilities.json`](mcp-capabilities.json) records exact application dispatch
coverage. Use the advertised schemas rather than guessing optional fields.

## Background documents and jobs

`snow_shot_document_open` creates an isolated, silent document from a local file,
history item, pinned item, clipboard, text, HTML, or a direct capture. Keep its
`document_id` and revision. Document tools cover selection, selection style,
annotation transactions, undo/redo, cloning, rendering, saving, copying, pinning,
recognition, recapture, drawing tools/styles, element editing (including an eraser
path), templates, original content, presentation, and closing. Recognition editing
also requires its separate `expected_recognition_revision`. Edits require
`expected_revision`; refresh document
state after `stale_revision` and decide whether to retry. Pass an
`idempotency_key` when the relevant mutation schema provides it.
Opening a pinned source preserves its rotation/flip, editable annotations and undo
history, content bounds, selection decoration, and active tool. Window opacity is a
presentation preference: it is applied by the normal pinned export, and excluded
from an isolated document source or an `original: true` pinned export.
Set `as_job: true` to open any source through an owned job. Delayed captures default
to this behavior; `as_job: false` keeps an ordinary cancelable request. A client
negotiating the modern Tasks extension receives document-open jobs as Tasks. The
completed job result contains the document ID and revision.

Recognition and standalone translation return owned job handles. Use
`snow_shot_job_get`, `snow_shot_job_list`, and `snow_shot_job_cancel`; honor the
returned `poll_interval_ms` and `ttl_ms`. Results remain available until expiry
or connection teardown. Admission and result-memory limits return explicit
errors instead of silently evicting existing results. Documents and jobs belong
to the current connection and are retired when it disconnects.
To retry an owned translation, call `snow_shot_translation_start` with
`retry_job_id` and an optional idempotency key. This creates a new job using the
retained original input and preferences; omit new text, source, language and
model fields in this mode. A fresh translation may instead use `texts` or
`source: "selection"` to capture selected text through the native application.

Large results use owned artifact handles. `snow_shot_artifact_read` returns a
base64 chunk with `offset`, `next_offset`, and `eof`; its maximum raw chunk is
256 KiB and default is 64 KiB. Continue using `next_offset` until `eof`, and
release the handle with `snow_shot_artifact_release` when finished. Artifact
metadata includes the MIME type, byte count, and lifetime. A digest is available
when computed; do not assume its presence in the initial descriptor.
Chunks travel over private IPC as binary attachments; base64 is added at the MCP
boundary. Recording files use a bounded private file snapshot so later source-file
changes do not change an artifact's content.

Current resource budgets are explicit:

| Resource | Limit |
| --- | --- |
| Documents | 4 per connection, 16 total; 64 million source pixels |
| Source rasters | 512 MiB total |
| Retained edit history | 64 MiB per document, 256 MiB total conservative budget |
| Render/export cache | 64 MiB rendered output and 64 MiB immutable export cache |
| Artifact handles | 16 per connection, 64 total, 15-minute lifetime |
| In-memory artifacts | 64 MiB each, 256 MiB total |
| File artifacts | 2 GiB each, 4 GiB total |

Oversized or exhausted admissions fail explicitly. Release documents/artifacts as
soon as the workflow no longer needs them.

## Application, recording, and pinned workflows

Use `snow_shot_app_status` and `snow_shot_app_displays` to inspect the running
application. `snow_shot_app_action` covers application windows and lifecycle.
Settings tools expose registered fields and their types, current values, and
revision; update only intended field IDs with that revision. Provider model
metadata is readable; credentials are write-only and omitted secrets remain
unchanged. Templates, history, configuration import/export, storage maintenance,
permissions, and updates each have their own typed tools. Destructive mutations
require the revision specified by their schema.

For recording, read `snow_shot_recording_state`, start an explicit region and
options, then retain the `recording_id` and revision for controls. Stop finalizes
the file; a successful start does not mean a completed file exists. Recording
supports pause/resume, annotation editing, undo/redo, copying and closing. Client
disconnect finalizes owned recording work through the application's existing
stop path.
Finalized recordings report `artifact_status: pending|ready|unavailable`. A ready
descriptor provides the owned immutable file snapshot; inspect `artifact_error`
when snapshot admission or copying fails.

Pinned tools list bounded pages, inspect, create, replace, update geometry and
appearance, edit annotations and recognition, export, and perform window actions.
Group tools manage groups. Keep the returned per-item or group revision and
refresh after user interaction makes it stale. Recognition uses configured
providers; network/provider failures are returned as application errors.

## Discovery, subscriptions, and cancellation

Resources expose capabilities, application status, settings, owned documents,
jobs, screenshot sessions, pinned images, history metadata, and bounded artifact
reads under `snow-shot://` URIs. Mutable/private reads have zero cache lifetime;
static catalogs advertise a five-minute lifetime. No arbitrary file URI reads
are supported. Prompts provide screenshot, background-image, and recording
workflows, with completion for background-image output formats.

The stdio bridge supports legacy initialize negotiation and MCP 2026-07-28
per-request discovery. Both legacy resource subscriptions and modern
`subscriptions/listen` receive coalesced URI invalidations for subscribed
resources; notifications do not contain image pixels, recognition text, or
credentials. Subscribe to a job URI to reduce polling, then read it for state.
The current Tasks extension is offered for negotiated document opening, background recognition,
automatic filtering, translation, asynchronous settings/storage actions and update check/download jobs through
`tasks/get`, `tasks/update`, and `tasks/cancel`.
Task delivery uses polling; task-notification filters are not advertised.

MCP request cancellation maps to the owned application request through negotiated
private IPC capabilities. Control requests retain capacity when ordinary work
is saturated. Cancellation is cooperative: a file write already committed may
return `outcome_may_have_completed`; inspect the destination before retrying.
The private local IPC protocol remains `snow-shot-mcp/1`, with optional negotiated
events/cancellation capabilities for compatibility with older applications.

## Validation and platform gates

Focused Rust tests and stdio protocol tests run on Windows, including five
protocol versions, bounded payloads, saturation, cancellation, discovery and
schema/dispatch agreement. All 28 legacy requests have checked input fixtures;
these verify contracts and are not a claim of native end-to-end feature coverage.
The offscreen Qt fixture exercises actual bridge/Qt document and job operations.

Native desktop validation is a separate release gate: capture and mixed-DPI
behavior, clipboard, recording encoders/audio, permission prompts, packaging and
disconnect finalization must be exercised on Windows and macOS. Native macOS
validation is pending on this Windows development host. Run related performance
targets with `windows-msvc-performance` or the macOS performance preset only.
Bridge baseline comparisons against the same Qt fixture isolate bridge changes;
they do not measure changes to the native capture or rendering implementation.

## Coordinates and selection

All input is in canonical **canvas coordinates**, using half-open rectangles
`[x, y, width, height]`. Width and height are extents, not right/bottom endpoints.
Do not multiply coordinates by the monitor's operating-system scale factor.
Begin/state responses include each display's stable ID, name, physical/logical/
canvas bounds, image size, backing scale, backend ID, and `canvas_to_logical`
affine transform `[m11, m12, m21, m22, dx, dy]`. They also report cursor and color
restoration policy. This preserves the application's mixed-DPI mapping and
macOS point-coordinate capture behavior.

Selection takes `operation: replace|add|subtract`, `type:
rectangle|polygon|polyline|freehand`, and either `bounds` or `points: [[x,y],...]`.
Paths close to form filled regions. The whole operand is validated before the
live selection changes. Nonfinite/out-of-canvas coordinates, empty results,
paths outside 3–8192 points, and excessive accumulated operands are rejected.
Rectangle bounds align outward to captured pixels through the existing model.

## Typed annotations

Example `screenshot_apply_annotations` input:

```json
{
  "session_id": "<returned session>",
  "expected_revision": 7,
  "version": 1,
  "label": "Explain the screenshot",
  "operations": [
    {"type":"rounded_rectangle","bounds":[100,80,240,120],
     "style":{"stroke":[255,40,40,255],"stroke_width":3,"corner_radius":12}},
    {"type":"arrow","points":[[360,140],[430,200]],"style":{}},
    {"type":"text","bounds":[430,180,220,70],"text":"Check this value",
     "style":{"font_size":24,"color":[255,40,40,255]}}
  ]
}
```

Supported types: rectangle, rounded_rectangle, rectangle_highlight, arrow, line,
freehand, pen_highlight, rectangle_filter, pen_filter, text, serial_number,
watermark, spotlight, select, and delete. Shapes/text/filters use `bounds`;
linear/freehand operations use `points`; serial numbers use `center` and
`number`; watermark uses `text`; select uses `id`; delete uses `ids`.
IDs are the returned `{index,generation}` objects. Element selection/deletion
refer to existing elements; split creation and operations on returned IDs into
successive requests.

Styles use RGBA byte arrays (`stroke`, `fill`, `color`), finite dimensions and
opacity, `stroke_style: solid|dashed|dotted`, `arrow_type: straight|curve|elbow`,
and `filter: mosaic|gaussian_blur|grayscale|inversion|emboss`.
Other fields are `stroke_width`, `corner_radius`, `rotation`, `font_size`,
`font_family`, `diameter`, `strength`, and `gap`. Consult `tools/list` for the
typed schemas. Limits are 1 MiB of annotation JSON, 256 operations, 8192 points
per path, 64 KiB of text, and a 128-byte history label. Rust rejects unknown
versions, operations, fields, colors, enums, and invalid geometry/styles.
The batch commits as one normal draw-engine history transaction. Viewport
clients synchronize immediately; undo and redo retain normal editor semantics.

## Outputs and performance

Render returns a PNG MCP image plus dimensions, scale, byte count, SHA-256,
source revision, and current revision. `scale` is 0.1–4 relative to the existing
native selection renderer; the rendered image is limited to 100 million pixels
and the local response frame to 64 MiB. `source_revision` identifies the immutable
snapshot when a user edits while export is running.

Save requires an absolute `path` or explicit `automatic_path: true`. Formats are
`png` (default), `jpeg`, `webp`, `avif`, `jxl`, `bmp`, and `pdf`, subject to the platform codecs.
`quality` is 1–100; `compression_level` is `low|medium|high`. PDF accepts
`pdf_page_size: image_size|a4_portrait|a4_landscape` and a `pdf_title` up to 1024 characters.
Canonical parent validation rejects relative, URL, UNC/device,
Windows alternate-stream/reserved-device names, and symlink leaf paths. The file
service writes atomically. The response reports the actual saved path, byte
count, and digest. Copy and pin use the existing canonical clipboard and pinned
window services. MCP outputs do not apply automatic user output preferences.

Native images remain in the capture session. Exports reuse the application
renderer and export coordinator. A retained artifact caches the rendered image,
canonical PNG, and digest metadata until canvas/selection/display/style state
or scale changes. Output lifecycle revisions alone do not force re-encoding.
Rendering, encoding, hashing, and saving stay off the GUI thread. Local IPC
transfers raw PNG bytes; base64 is introduced only in the MCP response.
Background documents use two fixed worker lanes with shared memory quotas. Local
IPC admits up to eight independent background requests per connection, while
eight reserved control slots keep cancellation and status requests responsive.
Responses expose timing measurements for capture/reconciliation, queue wait,
render, encode, and total latency, with cache-hit metadata. Existing capture and
export instrumentation provides finer native-stage measurements. No screenshot
pixels, tokens, clipboard payloads, or annotation text are written to logs.

## Architecture and protocol

The Blender references (`mcp-for-blender-main/src/blender_mcp/server.py` and
`addon.py`) establish the stdio/process boundary, bounded socket operations, and
main-thread command dispatch. Snow Shot adapts those principles to its existing
asynchronous workflows; it does not embed Python or expose arbitrary commands.

- Rust `snow-shot-mcp`: typed SDK tools, asynchronous same-user IPC, cancellation,
  request correlation, and image content assembly.
- Qt `ScreenshotMcpServer`: private descriptor/endpoint, authentication, framing,
  bounded per-client queues, and a dedicated I/O thread.
- Qt `ScreenshotMcpSession`: ownership, revision conflicts, request lifecycle,
  retained artifacts, and injected ports for deterministic tests.
- `ScreenshotController`: GUI-thread capture, selection, toolbar commands, and
  export-service integration. Silent capture suppresses presentation explicitly.
- Rust `snow_runtime_apply_annotation_json`: validated typed transactions through
  the existing document/history model, with changed-viewport synchronization.

The descriptor lives at `%LOCALAPPDATA%/SnowShot/mcp/snow-shot-mcp.json` on
Windows or `~/Library/Application Support/SnowShot/mcp/snow-shot-mcp.json` on
macOS. A lockfile establishes one owner. Atomic publication, a protected user
ACL/private directory, a random 256-bit token, and generation-aware cleanup keep
endpoint discovery scoped to the current user. Windows uses a named pipe with
`QLocalServer::UserAccessOption`; macOS uses a Unix socket. No TCP listener exists.
`SNOW_SHOT_MCP_DESCRIPTOR` can override the absolute descriptor path for both
the application and bridge during diagnostics/tests.
Never put the token in a client configuration.

Wire layout: `u32be payload_size`, `u32be json_size`, UTF-8 JSON, optional raw
attachment. The payload limit includes the JSON-size prefix, JSON, and bytes.
Requests carry no binary data and JSON is limited to 1 MiB plus envelope overhead.
The stdio input-line limit is 1 MiB plus 64 KiB for MCP framing and metadata,
which preserves the smaller local request limit. A non-reading peer may retain
at most 16 outgoing messages and 128 MiB of serialized output; exceeding either
budget or stalling a write for ten seconds closes that connection and retires
its owned application work. No mutation is replayed after this disconnect.
Responses declare `attachment_length` and MIME. Parsers validate lengths before
allocation and handle split prefixes, split UTF-8, EOF, malformed JSON, and
truncated attachments. Handshake negotiates `snow-shot-mcp/1` and authenticates
the token before any application request. Up to eight clients and eight queued
requests per client are accepted; status/cancellation can bypass pending work.

The Rust bridge retries connection establishment only. Mutations are never
replayed after a possibly partial write/disconnect. A caller may provide an
`idempotency_key`; otherwise the bridge generates one. The application keeps a
bounded replay window on that authenticated connection. Reusing a key with a
different payload returns `idempotency_conflict`. Reconnect releases ownership,
so obtain a new session rather than attempting to replay an old edit.

Errors have stable codes and optional field/state details. Relevant codes include
`unavailable`, `disabled`, `unauthorized`, `protocol_error`, `invalid_parameters`,
`permission_required`, `capture_unavailable`, `busy`, `queue_full`,
`session_not_found`, `revision_required`, `stale_revision`, `idempotency_conflict`,
`timeout`, `canceled`, `output_failed`, `output_too_large`, `clipboard_failed`, and
`pin_failed`. MCP tool failures set `isError`; status remains a successful query
when reporting that the application is unavailable.

## Focused verification

```powershell
cd snow_shot/rust/snow-shot-mcp
cargo test -p snow-shot-mcp
cd ../../../snow_draw_engine_qt
cargo test -p snow-draw-engine annotation_
cargo test -p snow-draw-engine-c annotation_ffi
cd ..
cmake --build --preset build-windows-msvc-debug --target snow_shot snow-shot-mcp-tests
ctest --preset test-windows-msvc-debug -R '^snow-shot-mcp(-stdio)?-tests$' --output-on-failure
```

`tests/check_mcp_capabilities.py` checks both dispatch tables against the checked
capability matrix, plus reviewed settings/canvas catalogs and actual recording
option adapters. Adding a UI enum or option requires an explicit coverage update.
Passing `--execution-reports <report.json> ...` additionally requires runtime
evidence for every advertised tool and resource entry/template; catalog agreement
alone does not satisfy that gate.
`tests/mcp_fixture_tests.py <bridge> <Qt-document-test-executable>` runs actual
stdio/private-IPC workflows with isolated deterministic providers, resource
notifications, Tasks, and four concurrent clients.
`tests/mcp_launch_tests.py <bridge>` verifies adjacent launch using a private Rust
sentinel executable. It never launches the installed application and requires
the repository's Rust toolchain.

For measurements, build the Qt fixture with the performance preset. Supply
`--benchmark --samples 100 --output <report.json>`; optionally pass
`--baseline-bridge <original-HEAD-bridge>` built with the same Rust profile,
target, CRT and linker flags. The report includes binary hashes, latency
percentiles, output bytes, Windows process CPU/working-set counters, 1080p/4K
incompressible-image exports, cache checks, repeated cleanup cycles and a blocked
stdout peer while a separate client queries status. GUI thread kernel CPU counters
measure thread CPU usage; heartbeat lateness separately measures responsiveness.
The optional `--legacy-fixture` and `--baseline-legacy-fixture` compare original/current
Qt session and transport code through the same synthetic ports. Copy/pin acknowledgments
and synthetic capture do not measure native desktop behavior. Process memory retained
after cleanup can include allocator caches and alone does not establish a leak.

The stdio smoke test uses an absent temporary descriptor and never captures the
desktop or changes integration settings. Native capture validation remains separate
from synthetic Qt ports. Tests are offscreen-capable, but this host's static Qt kit
uses the repository Windows QPA fallback. This iteration validates Windows only;
macOS native validation was deferred by the user. See `MCP_VALIDATION.md` for the
specific completed and outstanding gates.

On Windows, the manual live probe starts an isolated Snow Shot instance and calls
all 28 legacy routes through the bridge. It captures the current desktop, retains
and restores the clipboard, and briefly creates a pinned image. Its report separates
successful native operations from ownership guards on provider/scrolling routes.
It uses the test-build `--mcp-fixture` startup with isolated application storage.
Run it only in an interactive
desktop session:

```powershell
python snow_shot/tests/mcp_live_tests.py `
  build/windows-msvc-debug/snow_shot/Debug/snow_shot.exe `
  build/windows-msvc-debug/cargo/x86_64-pc-windows-msvc/debug/snow-shot-mcp.exe
```
