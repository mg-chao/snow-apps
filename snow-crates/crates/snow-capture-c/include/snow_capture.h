#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowCaptureDesktopSessionImpl SnowCaptureDesktopSession;
typedef struct SnowCaptureRegionSessionImpl SnowCaptureRegionSession;
typedef struct SnowCaptureWindowSessionImpl SnowCaptureWindowSession;
typedef struct SnowCaptureMonitorSessionImpl SnowCaptureMonitorSession;
typedef struct SnowCaptureFrameLeaseImpl SnowCaptureFrameLease;
typedef struct SnowCaptureCancellationTokenImpl SnowCaptureCancellationToken;
typedef struct SnowCaptureScreenshotResultImpl SnowCaptureScreenshotResult;
typedef struct SnowCaptureStreamImpl SnowCaptureStream;
typedef struct SnowCaptureStreamFrameImpl SnowCaptureStreamFrame;

typedef enum SnowCaptureBackendKind {
    SNOW_CAPTURE_BACKEND_AUTO = 0,
    SNOW_CAPTURE_BACKEND_DXGI = 1,
    SNOW_CAPTURE_BACKEND_WGC = 2,
    SNOW_CAPTURE_BACKEND_GDI = 3,
    SNOW_CAPTURE_BACKEND_SCREEN_CAPTURE_KIT = 4,
} SnowCaptureBackendKind;

typedef enum SnowCaptureWgcUpdateMode {
    SNOW_CAPTURE_WGC_UPDATE_MODE_AUTO = 0,
    SNOW_CAPTURE_WGC_UPDATE_MODE_COMPLETE_ONLY = 1,
    SNOW_CAPTURE_WGC_UPDATE_MODE_ORDERED_INCREMENTAL = 2,
} SnowCaptureWgcUpdateMode;

typedef enum SnowCapturePixelFormat {
    SNOW_CAPTURE_PIXEL_FORMAT_RGBA8 = 0,
    SNOW_CAPTURE_PIXEL_FORMAT_BGRA8 = 1,
} SnowCapturePixelFormat;

/* Shared by capture and recording. Lists are copied and deduplicated during creation,
 * limited to 4096 entries each, and use macOS WindowServer IDs / process IDs.
 * Nonempty lists require non-null pointers. Filters are fixed for the session's
 * lifetime and apply to display/region captures; independent window captures
 * reject filters. Windows continues to use native display affinity. */
#ifndef SNOW_CAPTURE_EXCLUSIONS_DEFINED
#define SNOW_CAPTURE_EXCLUSIONS_DEFINED
typedef struct SnowCaptureExclusions {
    const uint32_t* windows;
    size_t window_count;
    const int32_t* processes;
    size_t process_count;
} SnowCaptureExclusions;
#endif

typedef struct SnowCaptureDesktopSessionConfig {
    size_t capture_retry_count;
    uint8_t wgc_update_mode;
    /* Preferred backend; remaining backends are tried in default order on eligible failures.
       AUTO retains the default priority for each capture target. */
    uint8_t capture_backend;
    uint8_t pixel_format;
    uint8_t reserved[29];
    SnowCaptureExclusions exclusions;
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
    uint8_t pixel_format;
    uint8_t reserved0;
    uint32_t stride_bytes;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowCaptureFrameInfo;

/* A single monitor identified by its native device name (for example \\.\DISPLAY1).
 * Uses DXGI, WGC, then GDI on eligible failures, independently of desktop sessions. */
typedef struct SnowCaptureMonitorSessionConfig {
    const char* device_name_utf8;
    size_t capture_retry_count;
    uint8_t pixel_format;
    uint8_t reserved[31];
    SnowCaptureExclusions exclusions;
} SnowCaptureMonitorSessionConfig;

SnowCaptureMonitorSession*
snow_capture_monitor_session_create(const SnowCaptureMonitorSessionConfig* config);
void snow_capture_monitor_session_destroy(SnowCaptureMonitorSession* session);
uint8_t snow_capture_monitor_session_capture(SnowCaptureMonitorSession* session,
                                             SnowCaptureFrameInfo* out_info);
/* Retained pixels survive the next capture and session destruction. */
SnowCaptureFrameLease*
snow_capture_monitor_session_frame_retain(const SnowCaptureMonitorSession* session);

typedef struct SnowCaptureRegionSessionConfig {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    size_t capture_retry_count;
    uint8_t wgc_update_mode;
    uint8_t capture_backend;
    uint8_t pixel_format;
    uint8_t reserved[29];
    SnowCaptureExclusions exclusions;
} SnowCaptureRegionSessionConfig;

typedef struct SnowCaptureRegionFrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint8_t is_duplicate;
    uint8_t pixel_format;
    uint8_t reserved0[2];
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowCaptureRegionFrameInfo;

#define SNOW_CAPTURE_STREAM_CONFIG_VERSION 2u
#define SNOW_CAPTURE_STREAM_FRAME_INFO_VERSION 1u

typedef enum SnowCaptureStreamEventKind {
    SNOW_CAPTURE_STREAM_EVENT_TIMEOUT = 0,
    SNOW_CAPTURE_STREAM_EVENT_FRAME = 1,
    SNOW_CAPTURE_STREAM_EVENT_FRAMES_DROPPED = 2,
    SNOW_CAPTURE_STREAM_EVENT_RESOLUTION_CHANGED = 3,
    SNOW_CAPTURE_STREAM_EVENT_PAUSED = 4,
    SNOW_CAPTURE_STREAM_EVENT_RESUMED = 5,
    SNOW_CAPTURE_STREAM_EVENT_ENDED = 6,
    SNOW_CAPTURE_STREAM_EVENT_ERROR = 7,
} SnowCaptureStreamEventKind;

typedef struct SnowCaptureStreamConfig {
    uint32_t version;
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t target_fps;
    uint32_t min_fps;
    uint32_t buffer_depth;
    uint32_t max_consecutive_errors;
    size_t capture_retry_count;
    uint8_t wgc_update_mode;
    uint8_t capture_backend;
    uint8_t pixel_format;
    uint8_t adaptive_fps;
    /* Set to zero for scrolling captures to avoid cursor compositing work. */
    uint8_t include_cursor;
    /* Opt in to reversal of supported full-screen Magnifier effects.
       The effect is sampled once when the stream is created. */
    uint8_t restore_original_colors;
    uint8_t reserved[26];
    SnowCaptureExclusions exclusions;
} SnowCaptureStreamConfig;

typedef struct SnowCaptureStreamEvent {
    SnowCaptureStreamEventKind kind;
    SnowCaptureStreamFrame* frame;
    uint64_t dropped_count;
    uint32_t old_width;
    uint32_t old_height;
    uint32_t new_width;
    uint32_t new_height;
    uint8_t reserved[32];
} SnowCaptureStreamEvent;

typedef struct SnowCaptureStreamFrameInfo {
    uint32_t version;
    uint32_t struct_size;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint8_t is_duplicate;
    uint8_t pixel_format;
    /* Actual frame backend; consumes a reserved byte without changing ABI layout. */
    uint8_t backend_kind;
    uint8_t reserved0[1];
    uint64_t sequence;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowCaptureStreamFrameInfo;

typedef struct SnowCaptureStreamStats {
    uint64_t frames_captured;
    uint64_t frames_dropped;
    uint64_t errors_recovered;
    double current_fps;
    uint32_t target_fps;
    uint32_t buffer_fill;
    uint64_t capture_latency_ns;
} SnowCaptureStreamStats;

/* A native top-level window capture backed by Windows Graphics Capture when
 * the platform supports it. The returned pixel pointer remains valid until
 * the next capture, explicit frame release, or destroy. */
typedef struct SnowCaptureWindowSessionConfig {
    intptr_t hwnd;
    size_t capture_retry_count;
    uint8_t wgc_update_mode;
    uint8_t capture_backend;
    uint8_t pixel_format;
    uint8_t reserved[29];
} SnowCaptureWindowSessionConfig;

#define SNOW_CAPTURE_WINDOW_FRAME_INFO_VERSION 1u

typedef struct SnowCaptureWindowFrameInfo {
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
    uint8_t pixel_format;
    uint8_t reserved[6];
} SnowCaptureWindowFrameInfo;

#define SNOW_CAPTURE_SCREENSHOT_REQUEST_VERSION 1u
#define SNOW_CAPTURE_SCREENSHOT_REQUEST_REFRESH_LAYOUT (1u << 0)
#define SNOW_CAPTURE_SCREENSHOT_REQUEST_RESTORE_ORIGINAL_COLORS (1u << 1)
#define SNOW_CAPTURE_SCREENSHOT_REQUEST_INCLUDE_CURSOR (1u << 2)

typedef struct SnowCaptureScreenshotRequest {
    uint32_t version;
    uint32_t struct_size;
    uint32_t flags;
    uint32_t reserved0;
    intptr_t focused_window;
    const SnowCaptureCancellationToken* cancellation_token;
    uint8_t reserved[32];
} SnowCaptureScreenshotRequest;

SnowCaptureDesktopSession*
snow_capture_desktop_session_create(const SnowCaptureDesktopSessionConfig* config);
void snow_capture_desktop_session_destroy(SnowCaptureDesktopSession* session);

uint8_t snow_capture_desktop_session_prepare(SnowCaptureDesktopSession* session);
uint8_t snow_capture_desktop_session_state(SnowCaptureDesktopSession* session,
                                           SnowCaptureDesktopSessionState* out_state);
uint8_t snow_capture_desktop_session_refresh_layout(SnowCaptureDesktopSession* session);
uint8_t snow_capture_desktop_session_reset_to_prepared(SnowCaptureDesktopSession* session);
/* Captures every display and, when focused_window is nonzero, the requested
 * window as one all-or-nothing transaction. The returned result owns its
 * frame buffers and must be destroyed by the caller. */
SnowCaptureScreenshotResult*
snow_capture_desktop_session_capture(SnowCaptureDesktopSession* session,
                                     const SnowCaptureScreenshotRequest* request);

/* Cancellation may be signaled from another thread. The token must remain
 * alive until every capture call that references it has returned. */
SnowCaptureCancellationToken* snow_capture_cancellation_token_create(void);
void snow_capture_cancellation_token_cancel(SnowCaptureCancellationToken* token);
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
snow_capture_screenshot_result_focused_window_info(const SnowCaptureScreenshotResult* result,
                                                   SnowCaptureWindowFrameInfo* out_info);
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

/* Starts a continuous region stream. Frames are delivered as explicit leases;
 * a frame event must be released with snow_capture_stream_frame_release(). */
SnowCaptureStream* snow_capture_stream_create_region(const SnowCaptureStreamConfig* config);
void snow_capture_stream_destroy(SnowCaptureStream* stream);
uint8_t snow_capture_stream_stop(SnowCaptureStream* stream);
uint8_t snow_capture_stream_set_target_fps(SnowCaptureStream* stream, uint32_t target_fps);
/* A timeout is a successful receive with kind TIMEOUT. The function only
 * returns zero for invalid arguments or an internal API error. */
uint8_t snow_capture_stream_receive(SnowCaptureStream* stream, uint32_t timeout_ms,
                                    SnowCaptureStreamEvent* out_event);
uint8_t snow_capture_stream_frame_info(const SnowCaptureStreamFrame* frame,
                                       SnowCaptureStreamFrameInfo* out_info);
void snow_capture_stream_frame_release(SnowCaptureStreamFrame* frame);
uint8_t snow_capture_stream_stats(const SnowCaptureStream* stream,
                                  SnowCaptureStreamStats* out_stats);

SnowCaptureWindowSession*
snow_capture_window_session_create(const SnowCaptureWindowSessionConfig* config);
void snow_capture_window_session_destroy(SnowCaptureWindowSession* session);
uint8_t snow_capture_window_session_prepare(SnowCaptureWindowSession* session);
uint8_t snow_capture_window_session_capture(SnowCaptureWindowSession* session,
                                            SnowCaptureWindowFrameInfo* out_info);
SnowCaptureFrameLease*
snow_capture_window_session_frame_retain(const SnowCaptureWindowSession* session);
/* Clears the session-owned frame and restores the prepared state. Retained
 * frame leases remain valid; pointers obtained from frame-info calls are
 * invalid after this function returns. */
uint8_t snow_capture_window_session_release_frame(SnowCaptureWindowSession* session);

void snow_capture_frame_lease_release(SnowCaptureFrameLease* lease);

const char* snow_capture_last_error_message(void);

#ifdef __cplusplus
}
#endif
