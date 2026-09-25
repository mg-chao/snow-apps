# Snow Shot MCP

Snow Shot exposes its standard screenshot editor through the packaged Rust
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
The bridge never launches Snow Shot. If the application is stopped or integration
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
4. Use `screenshot_set_selection`, `screenshot_set_tool`,
   `screenshot_apply_annotations`, `screenshot_undo`, and `screenshot_redo`.
5. Use `screenshot_render`, `screenshot_save`, `screenshot_copy`, or
   `screenshot_pin`. Continue with the new revision returned by each output.
6. Call `screenshot_finish` to close the capture and release ownership. Set
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

Scrolling capture, OCR, translation, recording, document recovery JSON, and
arbitrary code execution are excluded from this surface.

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
`png` (default), `jpeg`, `webp`, `avif`, and `pdf`, subject to the platform codecs.
`quality` is 1–100. Canonical parent validation rejects relative, URL, UNC/device,
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
`SNOW_SHOT_MCP_DESCRIPTOR` can override the descriptor path for diagnostics/tests.
Never put the token in a client configuration.

Wire layout: `u32be payload_size`, `u32be json_size`, UTF-8 JSON, optional raw
attachment. The payload limit includes the JSON-size prefix, JSON, and bytes.
Requests carry no binary data and JSON is limited to 1 MiB plus envelope overhead.
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

The stdio smoke test uses an absent temporary descriptor and never captures the
desktop or changes integration settings. Native Windows/macOS capture validation
remains separate from offscreen protocol/session tests. macOS binaries must be
built and validated on each supported native architecture before release.
