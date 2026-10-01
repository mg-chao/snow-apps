#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SnowRecordingSessionImpl SnowRecordingSession;
typedef struct SnowRecordingSourceImpl SnowRecordingSource;
typedef struct SnowRecordingRenderTaskImpl SnowRecordingRenderTask;
typedef struct SnowRecordingConfig {
    /* Desktop points on macOS; physical desktop pixels on Windows. */
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
} SnowRecordingConfig;

typedef enum SnowRecordingState {
    SNOW_RECORDING_STATE_CREATED = 0,
    SNOW_RECORDING_STATE_RUNNING = 1,
    SNOW_RECORDING_STATE_PAUSED = 2,
    SNOW_RECORDING_STATE_STOPPED = 3,
} SnowRecordingState;

typedef enum SnowRecordingResult {
    SNOW_RECORDING_RESULT_OK = 0,
    SNOW_RECORDING_RESULT_INVALID_ARGUMENT = 1,
    SNOW_RECORDING_RESULT_INVALID_STATE = 2,
    SNOW_RECORDING_RESULT_CAPTURE_ERROR = 3,
    SNOW_RECORDING_RESULT_ENCODER_ERROR = 4,
    SNOW_RECORDING_RESULT_IO_ERROR = 5,
    SNOW_RECORDING_RESULT_CANCELED = 6,
    SNOW_RECORDING_RESULT_PERMISSION_DENIED = 7,
    SNOW_RECORDING_RESULT_UNSUPPORTED = 8,
    SNOW_RECORDING_RESULT_TARGET_UNAVAILABLE = 9,
    SNOW_RECORDING_RESULT_INTERNAL_ERROR = 255,
} SnowRecordingResult;

typedef enum SnowRecordingOutputFormat {
    SNOW_RECORDING_OUTPUT_FORMAT_MP4 = 0,
    SNOW_RECORDING_OUTPUT_FORMAT_GIF = 1,
    SNOW_RECORDING_OUTPUT_FORMAT_APNG = 2,
    SNOW_RECORDING_OUTPUT_FORMAT_WEBP = 3,
} SnowRecordingOutputFormat;

typedef enum SnowRecordingExportFormat {
    SNOW_RECORDING_EXPORT_FORMAT_MP4 = 0,
    SNOW_RECORDING_EXPORT_FORMAT_GIF = 1,
    SNOW_RECORDING_EXPORT_FORMAT_APNG = 2,
    SNOW_RECORDING_EXPORT_FORMAT_WEBP = 3,
} SnowRecordingExportFormat;

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

typedef enum SnowCaptureEncoderPreference {
    SNOW_CAPTURE_ENCODER_PREFERENCE_SOFTWARE = 0,
    SNOW_CAPTURE_ENCODER_PREFERENCE_H264_HARDWARE = 1,
} SnowCaptureEncoderPreference;

#define SNOW_RECORDING_EXPORT_CONFIG_VERSION 1u

typedef struct SnowRecordingExportConfig {
    uint32_t version;
    uint32_t struct_size;
    const char* output_file_utf8;
    uint32_t format;
    uint32_t maximum_width;
    uint32_t maximum_height;
    uint32_t target_fps;
    uint32_t codec;
    uint32_t preset;
    uint32_t encoder_preference;
    uint8_t reserved[32];
} SnowRecordingExportConfig;

#ifndef SNOW_CAPTURE_EXCLUSIONS_DEFINED
#define SNOW_CAPTURE_EXCLUSIONS_DEFINED
/* macOS WindowServer IDs / process IDs. Nonempty lists require non-null pointers.
 * Each list is limited to 4096 entries, copied and deduplicated during creation. */
typedef struct SnowCaptureExclusions {
    const uint32_t* windows;
    size_t window_count;
    const int32_t* processes;
    size_t process_count;
} SnowCaptureExclusions;
#endif

#define SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION 11u

/* Strings are bounded UTF-8 key names, copied during session creation. */
typedef struct SnowCaptureKeyboardLabel {
    uint32_t key_code;
    const uint8_t* utf8;
    uint32_t utf8_len;
} SnowCaptureKeyboardLabel;

/* Separate MP4 tracks use speaker audio as the default when both sources are enabled. */
typedef enum SnowCaptureRecordingAudioMode {
    SNOW_CAPTURE_RECORDING_AUDIO_MIXED = 0,
    SNOW_CAPTURE_RECORDING_AUDIO_SEPARATE = 1
} SnowCaptureRecordingAudioMode;

/* RGBA values use 0xRRGGBBAA packing. A zero alpha disables the effect. */
typedef struct SnowCaptureDirectRecordingConfig {
    uint32_t version;
    uint32_t struct_size;
    /* Desktop points on macOS; physical desktop pixels on Windows. */
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t capture_backend;
    const char* output_file_utf8;
    uint32_t output_format;
    uint32_t capture_fps;
    uint32_t output_fps;
    uint32_t maximum_width;
    uint32_t maximum_height;
    uint32_t codec;
    uint32_t preset;
    uint32_t encoder_preference;
    uint8_t enable_microphone;
    uint8_t enable_system_audio;
    uint8_t show_cursor;
    uint8_t reserved0;
    uint32_t mouse_trail_rgba;
    uint32_t mouse_click_rgba;
    uint8_t reserved[64];
    /* Version 2 extension. Version 1 callers end before show_keyboard and remain supported. */
    uint32_t show_keyboard;
    uint32_t keyboard_background_rgba;
    uint32_t keyboard_text_rgba;
    uint32_t keyboard_border_rgba;
    const SnowCaptureKeyboardLabel* keyboard_labels;
    uint32_t keyboard_label_count;
    /* Versions 1/2 use the legacy 500 ms trail lifetime. */
    uint32_t mouse_trail_duration_ms;
    /* Version 4: keycap height in pixels (32..128). */
    uint32_t keyboard_size;
    /* Version 5: 0 plays once, 1 loops infinitely. Older versions loop infinitely. */
    uint32_t loop_animated_images;
    /* Version 6: fixed exclusion filters for this recording, including pauses. */
    SnowCaptureExclusions exclusions;
    /* Version 7: multiply highlight and independent mouse input keycaps. */
    uint32_t mouse_highlight_rgba;
    uint32_t record_mouse_clicks;
    /* Version 8: optional application font; strings are copied during configuration.
       Null family selects the system UI font. Weight uses OpenType values (1..999). */
    const char* keyboard_font_family_utf8;
    const char* keyboard_cjk_font_family_utf8;
    uint32_t keyboard_font_weight;
    /* Version 9: MP4 quality, 0..100. Older versions retain the default of 80. */
    uint32_t quality;
    /* v10: SnowCaptureRecordingAudioMode; older versions use mixed audio. */
    uint32_t audio_mode;
    /* v11: independent source gain in decibels (-24..24); older callers use 0. */
    int32_t system_audio_gain_db;
    int32_t microphone_gain_db;
} SnowCaptureDirectRecordingConfig;

#define SNOW_RECORDING_DEFERRED_OPTIONS_VERSION 1u
#define SNOW_RECORDING_RENDER_PROGRESS_VERSION 1u

typedef enum SnowRecordingPlaybackOverlay {
    SNOW_RECORDING_PLAYBACK_OVERLAY_NONE = 0,
    SNOW_RECORDING_PLAYBACK_OVERLAY_PROGRESS_BAR = 1,
    SNOW_RECORDING_PLAYBACK_OVERLAY_PLAYBACK_TIME = 2,
} SnowRecordingPlaybackOverlay;

/* The full recording configuration is copied at creation. The working directory
 * may be null to use the output directory. Rendering always reuses this snapshot. */
typedef struct SnowRecordingDeferredOptions {
    uint32_t version;
    uint32_t struct_size;
    uint32_t overlay;
    uint32_t progress_bar_rgba;
    const char* working_directory_utf8;
} SnowRecordingDeferredOptions;

typedef enum SnowRecordingRenderState {
    SNOW_RECORDING_RENDER_STATE_RUNNING = 0,
    SNOW_RECORDING_RENDER_STATE_SUCCEEDED = 1,
    SNOW_RECORDING_RENDER_STATE_CANCELED = 2,
    SNOW_RECORDING_RENDER_STATE_FAILED = 3,
} SnowRecordingRenderState;

typedef enum SnowRecordingRenderStage {
    SNOW_RECORDING_RENDER_STAGE_PREPARE = 0,
    SNOW_RECORDING_RENDER_STAGE_RENDER = 1,
    SNOW_RECORDING_RENDER_STAGE_FINALIZE = 2,
} SnowRecordingRenderStage;

typedef struct SnowRecordingRenderProgress {
    uint32_t version;
    uint32_t struct_size;
    uint32_t state;
    uint32_t stage;
    float percent;
    uint64_t completed_pts;
    uint64_t total_pts;
    uint64_t duration_ms;
} SnowRecordingRenderProgress;

SnowRecordingResult
snow_recording_session_create_deferred(const SnowCaptureDirectRecordingConfig* config,
                                       const SnowRecordingDeferredOptions* options,
                                       SnowRecordingSession** out_session);
/* Blocking capture teardown: call on a worker thread. Source ownership is separate
 * from the session and from each render attempt. Failed rendering never deletes it. */
SnowRecordingResult snow_recording_session_finalize_deferred(SnowRecordingSession* session,
                                                             SnowRecordingSource** out_source);
SnowRecordingResult snow_recording_source_render_start(SnowRecordingSource* source,
                                                       SnowRecordingRenderTask** out_task);
/* String getters return required bytes INCLUDING the terminator. A null buffer
 * with capacity zero queries size; a short buffer is terminated and not overrun. */
size_t snow_recording_source_path(const SnowRecordingSource* source, char* buffer, size_t capacity);
SnowRecordingResult snow_recording_source_discard(SnowRecordingSource* source);
/* Destroy preserves files. Only explicit discard or successful publication
 * removes source media. Do not discard a source until its render task has ended. */
void snow_recording_source_destroy(SnowRecordingSource* source);
/* Initialize version/struct_size before polling. Poll is nonblocking and returns
 * the latest snapshot, without building an unbounded telemetry queue. */
SnowRecordingResult snow_recording_render_task_poll(const SnowRecordingRenderTask* task,
                                                    SnowRecordingRenderProgress* progress);
/* Cancellation synchronizes with publication and can wait for filesystem work.
 * Request it on a worker thread; continue polling asynchronously for teardown. */
SnowRecordingResult snow_recording_render_task_cancel(SnowRecordingRenderTask* task);
size_t snow_recording_render_task_error(const SnowRecordingRenderTask* task, char* buffer,
                                        size_t capacity);
/* Empty before success. The completed output path belongs to the task snapshot. */
size_t snow_recording_render_task_output_path(const SnowRecordingRenderTask* task, char* buffer,
                                              size_t capacity);
/* Cancels and joins any remaining worker: dispose on a worker thread, or only
 * after observing terminal state. The GUI thread must never wait for rendering. */
void snow_recording_render_task_destroy(SnowRecordingRenderTask* task);

/* Worker-thread query. macOS region coordinates are points; output is pixels.
 * Uses the same display transform and sizing policy as native recording startup. */
int32_t snow_recording_region_output_dimensions(int32_t x, int32_t y, uint32_t width,
                                                uint32_t height, uint32_t maximum_width,
                                                uint32_t maximum_height, uint32_t format,
                                                uint32_t* output_width, uint32_t* output_height);

SnowRecordingSession* snow_recording_session_create(const SnowRecordingConfig* config);
/* Pure output sizing shared by recording and effects preview. Zero maximums mean uncapped. */
int32_t snow_recording_output_dimensions(uint32_t width, uint32_t height, uint32_t maximum_width,
                                         uint32_t maximum_height, uint32_t format,
                                         uint32_t* output_width, uint32_t* output_height);

SnowRecordingResult
snow_recording_session_create_direct(const SnowCaptureDirectRecordingConfig* config,
                                     SnowRecordingSession** out_session);
void snow_recording_session_destroy(SnowRecordingSession* session);
uint8_t snow_recording_session_start(SnowRecordingSession* session);
uint8_t snow_recording_session_pause(SnowRecordingSession* session);
uint8_t snow_recording_session_resume(SnowRecordingSession* session);
uint8_t snow_recording_session_state(const SnowRecordingSession* session,
                                     SnowRecordingState* out_state);
uint8_t snow_recording_session_stop_and_export(SnowRecordingSession* session,
                                               const SnowRecordingExportConfig* config);
/* Nonblocking Stop admission. Freeze the media endpoint on the caller thread,
 * then call stop/finalize_deferred on a worker to wait for teardown. Idempotent. */
SnowRecordingResult snow_recording_session_request_stop(SnowRecordingSession* session);
SnowRecordingResult snow_recording_session_stop(SnowRecordingSession* session);
/* Disposable one-second native-GPU diagnostic using the regular direct recording
 * path. Publishes to the supplied path without overwriting, and returns OK only
 * if the complete GPU pipeline produced video. Set recover to 1 to disconnect
 * GPU capture and require successful software recovery; otherwise use 0.
 * Existing config layouts are unchanged. */
SnowRecordingResult snow_recording_gpu_probe(const SnowCaptureDirectRecordingConfig* config,
                                             uint32_t recover);
/* Live recording sessions created and not yet destroyed; for leak diagnostics in tests. */
size_t snow_recording_session_live_count(void);

typedef enum SnowRecordingAudioSource {
    SNOW_RECORDING_AUDIO_SYSTEM = 0,
    SNOW_RECORDING_AUDIO_MICROPHONE = 1
} SnowRecordingAudioSource;
typedef enum SnowRecordingAudioSourceStatus {
    SNOW_RECORDING_AUDIO_DISABLED = 0,
    SNOW_RECORDING_AUDIO_STARTING = 1,
    SNOW_RECORDING_AUDIO_READY = 2,
    SNOW_RECORDING_AUDIO_RECONNECTING = 3,
    SNOW_RECORDING_AUDIO_UNAVAILABLE = 4,
    SNOW_RECORDING_AUDIO_PERMISSION_DENIED = 5,
    SNOW_RECORDING_AUDIO_STOPPED = 6
} SnowRecordingAudioSourceStatus;
typedef struct SnowRecordingAudioLevel {
    float peak;
    uint32_t clipped;
    uint32_t status;
    uint64_t age_ms;
} SnowRecordingAudioLevel;
typedef struct SnowRecordingAudioLevels {
    SnowRecordingAudioLevel system_audio;
    SnowRecordingAudioLevel microphone;
} SnowRecordingAudioLevels;
uint8_t snow_recording_session_set_audio_gain(SnowRecordingSession* session, uint32_t source,
                                              int32_t gain_db);
/* Bit 0 enables the system meter; bit 1 enables the microphone meter. */
uint8_t snow_recording_session_set_audio_metering(SnowRecordingSession* session,
                                                  uint32_t source_mask);
uint8_t snow_recording_session_take_audio_levels(const SnowRecordingSession* session,
                                                 SnowRecordingAudioLevels* levels);

typedef struct SnowRecordingAudioMonitorImpl SnowRecordingAudioMonitor;
/* Returns before native acquisition finishes. Inspect source status for startup errors. */
SnowRecordingResult snow_recording_audio_monitor_create(uint32_t source, int32_t gain_db,
                                                        SnowRecordingAudioMonitor** monitor);
void snow_recording_audio_monitor_cancel(SnowRecordingAudioMonitor* monitor);
/* Joins acquisition/teardown; call on a worker thread. */
void snow_recording_audio_monitor_destroy(SnowRecordingAudioMonitor* monitor);
uint8_t snow_recording_audio_monitor_set_gain(SnowRecordingAudioMonitor* monitor, int32_t gain_db);
uint8_t snow_recording_audio_monitor_set_metering(SnowRecordingAudioMonitor* monitor,
                                                  uint8_t enabled);
uint8_t snow_recording_audio_monitor_take_levels(const SnowRecordingAudioMonitor* monitor,
                                                 SnowRecordingAudioLevels* levels);

typedef struct SnowRecordingExclusionStatus {
    uint64_t requested_generation;
    uint64_t applied_generation;
    /* 0: ready; 1: pending; 2: failed. */
    uint32_t status;
} SnowRecordingExclusionStatus;
/* Copies all IDs immediately. Required windows must appear in the native content snapshot. */
uint8_t snow_recording_session_request_exclusions(SnowRecordingSession* session,
                                                  const SnowCaptureExclusions* exclusions,
                                                  const uint32_t* required_windows,
                                                  uint32_t required_count, uint64_t* generation);
uint8_t snow_recording_session_exclusion_status(const SnowRecordingSession* session,
                                                SnowRecordingExclusionStatus* status);

const char* snow_recording_last_error_message(void);
#ifdef __cplusplus
}
#endif
