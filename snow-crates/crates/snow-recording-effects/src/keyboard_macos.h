#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t keycode;
    uint8_t down;
    uint8_t repeat;
    uint8_t modifiers;
    uint8_t reset;
    char label[64];
} SnowMacKeyEvent;

typedef void (*SnowMacKeyCallback)(void* context, const SnowMacKeyEvent* event);

// Create, pump and destroy on one worker thread. Destroy removes and invalidates the event
// source before returning, so no subsequent callback can access the caller-owned context.
void* snow_recording_keyboard_start(SnowMacKeyCallback callback, void* context, char* error,
                                    size_t error_size);
void snow_recording_keyboard_pump(void* handle);
void snow_recording_keyboard_stop(void* handle);

// Passing NULL pixels only measures. The caller owns a tightly packed premultiplied RGBA buffer.
int snow_recording_keycap(const char* label, float scale, const uint8_t* background,
                          const uint8_t* foreground, const uint8_t* border, uint32_t* width,
                          uint32_t* height, uint8_t* pixels, size_t capacity);
