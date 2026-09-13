#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    double scale;
    uint8_t primary;
    char name[256];
} SnowMacDisplay;

typedef struct {
    uint32_t id;
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} SnowMacWindow;

int snow_macos_displays(SnowMacDisplay* displays, size_t capacity, size_t* count);
uint64_t snow_macos_display_generation(void);
int snow_macos_windows(SnowMacWindow* windows, size_t capacity, size_t* count);
int snow_macos_window_element(uint32_t window_id, int32_t x, int32_t y, SnowMacWindow* element);
int snow_macos_capture(uint32_t display_id, uint32_t window_id, uint32_t width, uint32_t height,
                       uint8_t bgra, uint8_t* pixels, size_t length, char* error,
                       size_t error_size);

void* snow_macos_stream_start(uint32_t display_id, uint32_t window_id, uint32_t source_x,
                              uint32_t source_y, uint32_t width, uint32_t height, char* error,
                              size_t error_size);
int snow_macos_stream_read(void* handle, uint8_t bgra, uint8_t* pixels, size_t length,
                           uint64_t* sequence, char* error, size_t error_size);
void snow_macos_stream_stop(void* handle);

// Coordinates use the same desktop/backing-pixel mapping as SnowMacWindow.
typedef struct {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t hotspot_x;
    uint32_t hotspot_y;
    uint8_t buttons;
} SnowMacCursor;
int snow_macos_cursor(SnowMacCursor* cursor, uint8_t* rgba, size_t capacity);

int snow_macos_pointer(SnowMacCursor* cursor);
void snow_macos_exclude_window(uint32_t window_id, uint8_t excluded);
