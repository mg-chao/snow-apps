#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct SnowDetectedRegion {
    int32_t x, y, width, height;
    uint32_t category;
} SnowDetectedRegion;
typedef struct SnowDetectedRegions SnowDetectedRegions;
// BGR8 input with explicit stride. 0 = success, 1 = invalid input, 2 = detection failure.
int32_t snow_detect_visual_regions(const uint8_t* pixels, size_t length, uint32_t width,
                                   uint32_t height, size_t stride, SnowDetectedRegions** result);
const SnowDetectedRegion* snow_detected_regions_data(const SnowDetectedRegions* result,
                                                     size_t* count);
void snow_detected_regions_release(SnowDetectedRegions* result);
#ifdef __cplusplus
}
#endif
