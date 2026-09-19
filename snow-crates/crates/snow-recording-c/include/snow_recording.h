#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SnowRecordingSessionImpl SnowRecordingSession;
typedef struct SnowRecordingConfig {
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

#define SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION 6u

/* Strings are bounded UTF-8 key names, copied during session creation. */
typedef struct SnowCaptureKeyboardLabel {
    uint32_t key_code;
    const uint8_t* utf8;
    uint32_t utf8_len;
} SnowCaptureKeyboardLabel;

/* RGBA values use 0xRRGGBBAA packing. A zero alpha disables the effect. */
typedef struct SnowCaptureDirectRecordingConfig {
    uint32_t version;
    uint32_t struct_size;
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
} SnowCaptureDirectRecordingConfig;

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

const char* snow_recording_last_error_message(void);
#ifdef __cplusplus
}
#endif
