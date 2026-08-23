#ifndef SNOW_OCR_C_H
#define SNOW_OCR_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowOcrEngine SnowOcrEngine;
typedef struct SnowOcrResult SnowOcrResult;
typedef struct SnowOcrOwnedImage SnowOcrOwnedImage;

typedef struct SnowOcrEngineConfigV2 {
    uint32_t struct_size;
    uint32_t intra_threads;
    uint32_t inter_threads;
    uint32_t rayon_threads;
    uint8_t enable_cpu_mem_arena;
    uint8_t use_directml;
    uint8_t reserved[2];
} SnowOcrEngineConfigV2;

typedef struct SnowOcrRuntimeInfoV1 {
    uint32_t struct_size;
    uint32_t physical_core_count;
} SnowOcrRuntimeInfoV1;

typedef struct SnowOcrResourceCountsV1 {
    uint32_t struct_size;
    size_t engines;
    size_t results;
    size_t owned_images;
} SnowOcrResourceCountsV1;

typedef struct SnowOcrRequestV1 {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowOcrRequestV1;

typedef struct SnowOcrImageInfoV1 {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowOcrImageInfoV1;

typedef struct SnowOcrQuad {
    float points[8];
} SnowOcrQuad;

typedef struct SnowOcrColor {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    uint8_t alpha;
} SnowOcrColor;

typedef struct SnowOcrLineInfoV1 {
    uint32_t struct_size;
    const uint8_t* text_utf8;
    size_t text_len;
    float confidence;
    SnowOcrQuad quad;
    SnowOcrColor foreground;
} SnowOcrLineInfoV1;

SnowOcrEngine* snow_ocr_engine_create(void);
SnowOcrEngine* snow_ocr_engine_create_with_directml(uint8_t enabled);
SnowOcrEngine* snow_ocr_engine_create_with_config_v2(
    const SnowOcrEngineConfigV2* config
);
uint8_t snow_ocr_directml_is_available(void);
uint8_t snow_ocr_engine_uses_directml(const SnowOcrEngine* engine);
uint8_t snow_ocr_runtime_info_v1(SnowOcrRuntimeInfoV1* out_info);
uint8_t snow_ocr_resource_counts_v1(SnowOcrResourceCountsV1* out_counts);
void snow_ocr_engine_destroy(SnowOcrEngine* engine);
SnowOcrResult* snow_ocr_engine_recognize_rgba(
    SnowOcrEngine* engine,
    const SnowOcrRequestV1* request
);
void snow_ocr_result_destroy(SnowOcrResult* result);
SnowOcrOwnedImage* snow_ocr_result_take_image(SnowOcrResult* result);
void snow_ocr_owned_image_destroy(SnowOcrOwnedImage* image);
uint8_t snow_ocr_owned_image_info(
    const SnowOcrOwnedImage* image,
    SnowOcrImageInfoV1* out_image
);
uint8_t snow_ocr_result_image(
    const SnowOcrResult* result,
    SnowOcrImageInfoV1* out_image
);
size_t snow_ocr_result_line_count(const SnowOcrResult* result);
uint8_t snow_ocr_result_line(
    const SnowOcrResult* result,
    size_t line_index,
    SnowOcrLineInfoV1* out_line
);
const char* snow_ocr_last_error_message(void);

#ifdef __cplusplus
}
#endif

#endif
