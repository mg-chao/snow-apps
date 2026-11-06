# Recording build profiles

Standalone `snow-recording-c`, `snow-screen-recorder`, and `snow-recording-runtime`
builds enable the `legacy-capture` and `hdr-encoding` Cargo features by default
for compatibility.
It includes `RecordingSession`, `snow_recording_session_create`,
`snow_recording_session_stop_and_export`, and the synchronous macOS C interface.
Use `default-features = false` to omit those capture APIs and HDR encoding.
The Windows Snow Shot FFI archive does this and retains SDR direct/deferred
sessions, editing, trim, playback, HDR decoding/tone mapping, and old recording
bundle readers and exporters. Re-encoding HDR sources produces tone-mapped SDR;
an unchanged whole-file copy still preserves the original bytes. Explicit P010
HDR encoder input fails before output/staging files are created. The macOS FFI
archive explicitly enables `hdr-encoding`, preserving its native HDR workflows.
Standalone users can enable either capability independently.

On Windows, `ffmpeg[snow-shot-minimal]` provides the application's MP4, GIF,
APNG, and WebP outputs, including 8-bit SDR H.265 and audio. It retains Matroska
input for old `.snowrec` bundles. Standalone users that create old Matroska
intermediates or export AVI should use `ffmpeg[snow-recording-compat]`, which
extends that profile with Main10 encoding, Matroska/AVI muxing, MPEG4/MP3 encoding,
and MP3 parsing and decoding. HDR output also requires the `hdr-encoding` Cargo
feature. The macOS media profile continues to include its native harness
capabilities.
