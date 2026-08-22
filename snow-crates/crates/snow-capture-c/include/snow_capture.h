#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowCaptureDesktopSessionImpl SnowCaptureDesktopSession;
typedef struct SnowCaptureRegionSessionImpl SnowCaptureRegionSession;
typedef struct SnowCaptureWindowSessionImpl SnowCaptureWindowSession;
typedef struct SnowCaptureSnapshotImpl SnowCaptureSnapshot;
typedef struct SnowCaptureFrameLeaseImpl SnowCaptureFrameLease;
typedef struct SnowCaptureCancellationTokenImpl SnowCaptureCancellationToken;
typedef struct SnowCaptureScreenshotResultImpl SnowCaptureScreenshotResult;
typedef struct SnowCaptureRecordingSessionImpl SnowCaptureRecordingSession;
typedef struct SnowCaptureRegionStreamImpl SnowCaptureRegionStream;
typedef struct SnowCaptureRegionStreamFrameImpl SnowCaptureRegionStreamFrame;

typedef enum SnowCaptureRegionStreamEventKind {
    SNOW_CAPTURE_REGION_STREAM_FRAME = 1,
    SNOW_CAPTURE_REGION_STREAM_ENDED = 2,
    SNOW_CAPTURE_REGION_STREAM_ERROR = 3,
    SNOW_CAPTURE_REGION_STREAM_RESOLUTION_CHANGED = 4,
} SnowCaptureRegionStreamEventKind;

typedef void (*SnowCaptureRegionStreamCallback)(
    void* context, SnowCaptureRegionStreamEventKind kind,
    SnowCaptureRegionStreamFrame* frame);

typedef enum SnowCaptureBackendKind {
    SNOW_CAPTURE_BACKEND_AUTO = 0,
    SNOW_CAPTURE_BACKEND_DXGI = 1,
    SNOW_CAPTURE_BACKEND_WGC = 2,
    SNOW_CAPTURE_BACKEND_GDI = 3,
} SnowCaptureBackendKind;

typedef enum SnowCaptureWgcUpdateMode {
    SNOW_CAPTURE_WGC_UPDATE_MODE_AUTO = 0,
    SNOW_CAPTURE_WGC_UPDATE_MODE_COMPLETE_ONLY = 1,
    SNOW_CAPTURE_WGC_UPDATE_MODE_ORDERED_INCREMENTAL = 2,
} SnowCaptureWgcUpdateMode;

typedef enum SnowCaptureDesktopAutoBackendPolicy {
    SNOW_CAPTURE_DESKTOP_AUTO_BACKEND_POLICY_DEFAULT = 0,
    /* Prefer the compositor's SDR GDI presentation for minimum snapshot
     * latency, with DXGI and WGC retained as automatic fallbacks. */
    SNOW_CAPTURE_DESKTOP_AUTO_BACKEND_POLICY_LOW_LATENCY_SDR = 1,
} SnowCaptureDesktopAutoBackendPolicy;

#define SNOW_CAPTURE_DESKTOP_AUTO_BACKEND_POLICY_RESERVED_INDEX 0u

typedef struct SnowCaptureDesktopSessionConfig {
    size_t capture_retry_count;
    uint8_t wgc_update_mode;
    uint8_t capture_backend;
    /* reserved[SNOW_CAPTURE_DESKTOP_AUTO_BACKEND_POLICY_RESERVED_INDEX] may
     * contain SnowCaptureDesktopAutoBackendPolicy when capture_backend is
     * AUTO. Zero and unrecognized values preserve the default policy for
     * forward compatibility. All other bytes must be initialized to zero. */
    uint8_t reserved[30];
} SnowCaptureDesktopSessionConfig;

typedef struct SnowCaptureDesktopSessionState {
    size_t worker_count;
    uint8_t prepared;
    uint8_t reserved0[3];
    uint32_t active_capture_access_count;
    uint64_t retained_resource_bytes;
    const char* backend_kind;
} SnowCaptureDesktopSessionState;

typedef struct SnowCaptureFrameInfo {
    const char* stable_id;
    const char* name;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint8_t is_primary;
    uint8_t backend_kind;
    uint8_t reserved0[2];
    uint32_t stride_bytes;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowCaptureFrameInfo;

typedef struct SnowCaptureRegionSessionConfig {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    size_t capture_retry_count;
    uint8_t wgc_update_mode;
    uint8_t capture_backend;
    uint8_t reserved[30];
} SnowCaptureRegionSessionConfig;

typedef struct SnowCaptureRegionFrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint8_t is_duplicate;
    uint8_t reserved0[3];
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowCaptureRegionFrameInfo;

#define SNOW_CAPTURE_REGION_STREAM_CONFIG_VERSION 1u

typedef struct SnowCaptureRegionStreamConfig {
    uint32_t version;
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    size_t capture_retry_count;
    uint8_t capture_backend;
    uint8_t reserved[31];
} SnowCaptureRegionStreamConfig;

typedef struct SnowCaptureRegionStreamFrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint8_t is_duplicate;
    uint8_t reserved0[3];
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowCaptureRegionStreamFrameInfo;

#define SNOW_CAPTURE_REGION_STREAM_STITCH_FEEDBACK_VERSION 1u

/* Reports complete serialized stitch-worker service time. Duplicate fast-path
 * completions set representative to zero; their pending/replacement pressure
 * is still used for emergency capture backoff. */
typedef struct SnowCaptureRegionStreamStitchFeedback {
    uint32_t version;
    uint32_t struct_size;
    uint64_t service_time_ns;
    uint32_t pending_depth;
    uint32_t replaced_frames;
    uint8_t representative;
    uint8_t reserved[7];
} SnowCaptureRegionStreamStitchFeedback;

/* A native top-level window capture backed by Windows Graphics Capture when
 * the platform supports it. The returned pixel pointer remains valid until
 * the next capture or destroy. */
typedef struct SnowCaptureWindowSessionConfig {
    intptr_t hwnd;
    size_t capture_retry_count;
    uint8_t wgc_update_mode;
    uint8_t capture_backend;
    uint8_t reserved[30];
} SnowCaptureWindowSessionConfig;

typedef struct SnowCaptureWindowFrameInfo {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowCaptureWindowFrameInfo;

#define SNOW_CAPTURE_WINDOW_FRAME_INFO_VERSION 1u

typedef struct SnowCaptureWindowFrameInfoV1 {
    uint32_t version;
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
    uint8_t backend_kind;
    uint8_t reserved[7];
} SnowCaptureWindowFrameInfoV1;

#define SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION 1u
#define SNOW_CAPTURE_SCREENSHOT_REQUEST_REFRESH_LAYOUT (1u << 0)

typedef struct SnowCaptureScreenshotRequestV1 {
    uint32_t version;
    uint32_t struct_size;
    uint32_t flags;
    uint32_t reserved0;
    intptr_t focused_window;
    const SnowCaptureCancellationToken* cancellation_token;
    uint8_t reserved[32];
} SnowCaptureScreenshotRequestV1;

typedef struct SnowCaptureRecordingConfig {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint8_t enable_microphone;
    uint8_t enable_system_audio;
    uint8_t capture_backend;
    uint8_t reserved0;
    const char* working_directory_utf8;
    uint8_t reserved[32];
} SnowCaptureRecordingConfig;

typedef enum SnowCaptureRecordingState {
    SNOW_CAPTURE_RECORDING_STATE_CREATED = 0,
    SNOW_CAPTURE_RECORDING_STATE_RUNNING = 1,
    SNOW_CAPTURE_RECORDING_STATE_PAUSED = 2,
    SNOW_CAPTURE_RECORDING_STATE_STOPPED = 3,
} SnowCaptureRecordingState;

typedef enum SnowCaptureRecordingExportFormat {
    SNOW_CAPTURE_RECORDING_EXPORT_FORMAT_MP4 = 0,
    SNOW_CAPTURE_RECORDING_EXPORT_FORMAT_GIF = 1,
    SNOW_CAPTURE_RECORDING_EXPORT_FORMAT_APNG = 2,
    SNOW_CAPTURE_RECORDING_EXPORT_FORMAT_WEBP = 3,
} SnowCaptureRecordingExportFormat;

typedef enum SnowCaptureVideoCodec {
    SNOW_CAPTURE_VIDEO_CODEC_H264 = 0,
    SNOW_CAPTURE_VIDEO_CODEC_H265 = 1,
} SnowCaptureVideoCodec;

typedef enum SnowCaptureVideoEncodingPreset {
    SNOW_CAPTURE_VIDEO_ENCODING_PRESET_ULTRAFAST = 0,
    SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYFAST = 1,
    SNOW_CAPTURE_VIDEO_ENCODING_PRESET_MEDIUM = 2,
    SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYSLOW = 3,
    SNOW_CAPTURE_VIDEO_ENCODING_PRESET_PLACEBO = 4,
} SnowCaptureVideoEncodingPreset;

#define SNOW_CAPTURE_RECORDING_EXPORT_CONFIG_VERSION 1u

typedef struct SnowCaptureRecordingExportConfig {
    uint32_t version;
    uint32_t struct_size;
    const char* output_file_utf8;
    uint32_t format;
    uint32_t maximum_width;
    uint32_t maximum_height;
    uint32_t target_fps;
    uint32_t codec;
    uint32_t preset;
    uint8_t reserved[32];
} SnowCaptureRecordingExportConfig;

/* Releases the process-wide pixel-conversion worker pool. Conversions already
 * in progress retain the pool until they finish. Stop and join capture work
 * first when the worker threads must exit promptly. */
void snow_capture_release_conversion_pool(void);

SnowCaptureDesktopSession*
snow_capture_desktop_session_create(const SnowCaptureDesktopSessionConfig* config);
void snow_capture_desktop_session_destroy(SnowCaptureDesktopSession* session);

uint8_t snow_capture_desktop_session_prepare(SnowCaptureDesktopSession* session);
uint8_t snow_capture_desktop_session_state(SnowCaptureDesktopSession* session,
                                           SnowCaptureDesktopSessionState* out_state);
uint8_t snow_capture_desktop_session_refresh_layout(SnowCaptureDesktopSession* session);
uint8_t snow_capture_desktop_session_release_idle_resources(SnowCaptureDesktopSession* session);
/* Initializes the configured backend for an imminent screenshot while
 * leaving active frame access closed. The environment remains available to
 * the following capture until idle resources are released. */
uint8_t snow_capture_desktop_session_prepare_capture_environment(SnowCaptureDesktopSession* session,
                                                                 uint8_t refresh_layout);
SnowCaptureSnapshot* snow_capture_desktop_session_capture_all(SnowCaptureDesktopSession* session);
/* Captures every display and, when focused_window is nonzero, the requested
 * window as one all-or-nothing transaction. The returned result owns its
 * frame buffers and must be destroyed by the caller. */
SnowCaptureScreenshotResult*
snow_capture_desktop_session_capture_v1(SnowCaptureDesktopSession* session,
                                        const SnowCaptureScreenshotRequestV1* request);

/* Cancellation may be signaled from another thread. The token must remain
 * alive until every capture call that references it has returned. */
SnowCaptureCancellationToken* snow_capture_cancellation_token_create(void);
void snow_capture_cancellation_token_cancel(SnowCaptureCancellationToken* token);
uint8_t snow_capture_cancellation_token_is_canceled(const SnowCaptureCancellationToken* token);
void snow_capture_cancellation_token_destroy(SnowCaptureCancellationToken* token);

/* Pointers returned through frame-info structures remain valid until the
 * result is destroyed. Retaining the corresponding frame lease extends the
 * pixel-buffer lifetime beyond result destruction. */
size_t snow_capture_screenshot_result_display_count(const SnowCaptureScreenshotResult* result);
uint8_t snow_capture_screenshot_result_display_info(const SnowCaptureScreenshotResult* result,
                                                    size_t index, SnowCaptureFrameInfo* out_info);
SnowCaptureFrameLease*
snow_capture_screenshot_result_display_retain(const SnowCaptureScreenshotResult* result,
                                              size_t index);
uint8_t
snow_capture_screenshot_result_focused_window_info_v1(const SnowCaptureScreenshotResult* result,
                                                      SnowCaptureWindowFrameInfoV1* out_info);
SnowCaptureFrameLease*
snow_capture_screenshot_result_focused_window_retain(const SnowCaptureScreenshotResult* result);
void snow_capture_screenshot_result_destroy(SnowCaptureScreenshotResult* result);

SnowCaptureRegionSession*
snow_capture_region_session_create(const SnowCaptureRegionSessionConfig* config);
void snow_capture_region_session_destroy(SnowCaptureRegionSession* session);
uint8_t snow_capture_region_session_prepare(SnowCaptureRegionSession* session);
/* The returned pixel pointer remains valid until the next capture or destroy. */
uint8_t snow_capture_region_session_capture(SnowCaptureRegionSession* session,
                                            SnowCaptureRegionFrameInfo* out_info);

/* Starts a persistent, bounded region stream. The callback runs on the native
 * delivery thread and receives ownership of each non-duplicate frame. It must
 * release the frame with snow_capture_region_stream_frame_destroy after the
 * pixels are no longer needed. Destroying the stream stops and joins both the
 * capture and delivery threads before returning. The callback context must
 * remain valid until destroy returns. Do not destroy the stream from inside
 * its callback. On an ERROR event, snow_capture_last_error_message is readable
 * from that callback invocation. */
SnowCaptureRegionStream* snow_capture_region_stream_create(
    const SnowCaptureRegionStreamConfig* config,
    SnowCaptureRegionStreamCallback callback,
    void* context);
void snow_capture_region_stream_destroy(SnowCaptureRegionStream* stream);
/* Returns nonzero when feedback was accepted. On failure,
 * snow_capture_last_error_message() describes the validation error. */
uint8_t snow_capture_region_stream_report_stitch_feedback(
    SnowCaptureRegionStream* stream,
    const SnowCaptureRegionStreamStitchFeedback* feedback);
uint8_t snow_capture_region_stream_frame_info(
    const SnowCaptureRegionStreamFrame* frame,
    SnowCaptureRegionStreamFrameInfo* out_info);
void snow_capture_region_stream_frame_destroy(SnowCaptureRegionStreamFrame* frame);

SnowCaptureWindowSession*
snow_capture_window_session_create(const SnowCaptureWindowSessionConfig* config);
void snow_capture_window_session_destroy(SnowCaptureWindowSession* session);
uint8_t snow_capture_window_session_prepare(SnowCaptureWindowSession* session);
uint8_t snow_capture_window_session_capture(SnowCaptureWindowSession* session,
                                            SnowCaptureWindowFrameInfo* out_info);
SnowCaptureFrameLease*
snow_capture_window_session_frame_retain(const SnowCaptureWindowSession* session);

size_t snow_capture_snapshot_count(const SnowCaptureSnapshot* snapshot);
uint8_t snow_capture_snapshot_frame_info(const SnowCaptureSnapshot* snapshot, size_t index,
                                         SnowCaptureFrameInfo* out_info);
SnowCaptureFrameLease* snow_capture_snapshot_frame_retain(const SnowCaptureSnapshot* snapshot,
                                                          size_t index);
void snow_capture_frame_lease_release(SnowCaptureFrameLease* lease);
void snow_capture_snapshot_destroy(SnowCaptureSnapshot* snapshot);

SnowCaptureRecordingSession*
snow_capture_recording_session_create(const SnowCaptureRecordingConfig* config);
void snow_capture_recording_session_destroy(SnowCaptureRecordingSession* session);
uint8_t snow_capture_recording_session_start(SnowCaptureRecordingSession* session);
uint8_t snow_capture_recording_session_pause(SnowCaptureRecordingSession* session);
uint8_t snow_capture_recording_session_resume(SnowCaptureRecordingSession* session);
uint8_t snow_capture_recording_session_state(const SnowCaptureRecordingSession* session,
                                             SnowCaptureRecordingState* out_state);
uint8_t snow_capture_recording_session_stop_and_export(SnowCaptureRecordingSession* session,
                                                       const char* output_file_utf8,
                                                       uint8_t export_gif);
uint8_t
snow_capture_recording_session_stop_and_export_v1(SnowCaptureRecordingSession* session,
                                                  const SnowCaptureRecordingExportConfig* config);

const char* snow_capture_last_error_message(void);

#ifdef __cplusplus
}
#endif
