#ifndef SNOW_RECORDING_EFFECTS_H
#define SNOW_RECORDING_EFFECTS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowRecordingEffects SnowRecordingEffects;
typedef struct SnowRecordingEffectsFrame SnowRecordingEffectsFrame;
typedef void (*SnowRecordingEffectsNotify)(void* context);
typedef struct SnowRecordingEffectsKeyLabel {
    uint32_t virtual_key;
    const char* label_utf8;
} SnowRecordingEffectsKeyLabel;

#define SNOW_RECORDING_EFFECTS_CONFIG_VERSION 3u
typedef struct SnowRecordingEffectsConfig {
    uint32_t version;
    uint32_t struct_size;
    int32_t x, y;
    uint32_t width, height;
    uint32_t output_width, output_height;
    uint32_t trail_rgba, click_rgba;
    uint32_t show_keyboard;
    uint32_t keyboard_background_rgba, keyboard_text_rgba, keyboard_border_rgba;
    const SnowRecordingEffectsKeyLabel* labels;
    uint32_t label_count;
    /* Version 1 reserved this field as zero (500 ms lifetime). */
    uint32_t trail_duration_ms;
    uint64_t generation;
    /* Version 3: keycap height in pixels (32..128). */
    uint32_t keyboard_size;
} SnowRecordingEffectsConfig;

typedef struct SnowRecordingEffectsTile {
    uint32_t x, y;
    uint32_t width, height;
    uint32_t stride;
    const uint8_t* rgba_premultiplied;
} SnowRecordingEffectsTile;

typedef struct SnowRecordingEffectsFrameInfo {
    uint64_t generation, revision;
    uint32_t width, height;
    const SnowRecordingEffectsTile* tiles;
    uint32_t tile_count;
    const char* error_utf8;
} SnowRecordingEffectsFrameInfo;

// Config strings are copied. Control calls belong to one owner thread.
// Notification runs on the worker; enqueue work and return without calling control APIs.
SnowRecordingEffects* snow_recording_effects_create(const SnowRecordingEffectsConfig* config,
                                                    SnowRecordingEffectsNotify notify,
                                                    void* context);
int32_t snow_recording_effects_configure(SnowRecordingEffects* effects,
                                         const SnowRecordingEffectsConfig* config);
int32_t snow_recording_effects_set_active(SnowRecordingEffects* effects, int32_t active);
// Stops and joins all observers before returning. No subsequent notification can occur.
void snow_recording_effects_stop(SnowRecordingEffects* effects);
void snow_recording_effects_destroy(SnowRecordingEffects* effects);
// A complete immutable snapshot. All tile pointers remain valid until release, including after
// stop.
SnowRecordingEffectsFrame* snow_recording_effects_acquire_frame(SnowRecordingEffects* effects);
int32_t snow_recording_effects_frame_info(const SnowRecordingEffectsFrame* frame,
                                          SnowRecordingEffectsFrameInfo* info);
// Keyboard layer in physical capture pixels, drawn above the mouse layer returned by frame_info.
// This is a complete snapshot, including an empty tile array when keys expire. Same frame lease.
int32_t snow_recording_effects_frame_keyboard_info(const SnowRecordingEffectsFrame* frame,
                                                   SnowRecordingEffectsFrameInfo* info);
void snow_recording_effects_release_frame(SnowRecordingEffectsFrame* frame);
const char* snow_recording_effects_last_error(void);
// Live handles created and not yet destroyed; for leak diagnostics in tests.
size_t snow_recording_effects_live_handle_count(void);
#ifdef __cplusplus
}
#endif
#endif
