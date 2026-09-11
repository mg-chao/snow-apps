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
int snow_macos_windows(SnowMacWindow* windows, size_t capacity, size_t* count);
int snow_macos_capture(uint32_t display_id, uint32_t window_id, uint32_t width, uint32_t height,
                       uint8_t bgra, uint8_t* pixels, size_t length, char* error,
                       size_t error_size);
