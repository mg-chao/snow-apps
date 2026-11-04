#ifndef SNOW_RECORDING_EFFECTS_H
#define SNOW_RECORDING_EFFECTS_H
#include <stddef.h>
#include <stdint.h>
#include "snow_webcam.h"
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

#define SNOW_RECORDING_EFFECTS_CONFIG_VERSION 12u
typedef struct SnowRecordingEffectsConfig {
    uint32_t version;
    uint32_t struct_size;
    int32_t x, y;
    uint32_t width, height;
    /* Export dimensions, independent of the preview canvas. */
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
    uint32_t reserved_v3;
    /* Version 4: live alpha approximation of the saved multiply highlight. */
    uint32_t highlight_rgba;
    uint32_t record_mouse_clicks;
    /* Version 5: optional application font; strings are copied during configuration.
       Null family selects the system UI font. Weight uses OpenType values (1..999). */
    const char* keyboard_font_family_utf8;
    const char* keyboard_cjk_font_family_utf8;
    uint32_t keyboard_font_weight;
    /* Version 6: physical display pixels per native desktop unit.
       Use the display DPR for macOS points and 1 for Windows physical pixels. */
    double canvas_scale;
    /* Version 7: highlight radius in physical display pixels (8..128); older callers use 41. */
    uint32_t highlight_radius;
    /* v8: retained camera source; null disables webcam. Shape: 0 square, 1 circle. */
    const SnowWebcam* webcam_source;
    uint32_t webcam_size;
    uint32_t webcam_shape;
    /* Preferred pixel distances from the region's left and bottom edges. */
    uint32_t webcam_offset_left;
    uint32_t webcam_offset_bottom;
    /* Version 10: vertical scroll labels, independent of clicks and keyboard. */
    uint32_t record_mouse_scrolling;
    /* Version 11: clockwise quarter-turns in bits 0..1; reflect horizontally before
       rotation in bit 2. Values 0..7; older configurations use identity (0). */
    uint32_t webcam_orientation;
    /* Version 12: keystroke category filters. Each is 0 or 1; older callers enable all.
       Shortcut preservation is effective only while record_control_keys is enabled. */
    uint32_t record_number_keys;
    uint32_t record_letters_and_symbols;
    uint32_t record_control_keys;
    uint32_t preserve_keyboard_shortcuts;
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
// Static zero-second playback overlay on the same physical display canvas as mouse effects.
// Width/height use native desktop units; canvas_scale matches SnowRecordingEffectsConfig.
// No observers are started.
// overlay: 1 = progress bar, 2 = playback time. Release with release_frame.
// Null font family selects the system UI font.
SnowRecordingEffectsFrame* snow_recording_effects_playback_preview(
    uint32_t width, uint32_t height, double canvas_scale, uint32_t overlay, uint32_t progress_rgba,
    const char* font_family_utf8, const char* cjk_font_family_utf8, uint32_t font_weight);
int32_t snow_recording_effects_frame_info(const SnowRecordingEffectsFrame* frame,
                                          SnowRecordingEffectsFrameInfo* info);
// Both layers use physical display pixels and fixed pixel styles. FrameInfo width/height
// describe that coordinate space independently of the configured export dimensions.
// Keyboard layer, drawn above the mouse layer returned by
// frame_info. This is a complete snapshot, including an empty tile array when keys expire. Same
// frame lease.
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
