#ifndef SNOW_OCR_C_H
#define SNOW_OCR_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnowOcrEngine SnowOcrEngine;
typedef struct SnowOcrResult SnowOcrResult;

typedef struct SnowOcrEngineConfig {
    uint32_t struct_size;
    uint32_t intra_threads;
    uint32_t inter_threads;
    uint32_t rayon_threads;
    uint8_t enable_cpu_mem_arena;
    uint8_t use_directml;
    uint8_t reserved[2];
    const char* model_store_dir_utf8;
} SnowOcrEngineConfig;

typedef struct SnowOcrRuntimeInfo {
    uint32_t struct_size;
    uint32_t physical_core_count;
} SnowOcrRuntimeInfo;

typedef struct SnowOcrResourceCounts {
    uint32_t struct_size;
    size_t engines;
    size_t results;
} SnowOcrResourceCounts;

typedef struct SnowOcrRequest {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    const uint8_t* rgba_bytes;
    size_t rgba_len;
} SnowOcrRequest;

typedef struct SnowOcrQuad {
    float points[8];
} SnowOcrQuad;

typedef struct SnowOcrLineInfo {
    uint32_t struct_size;
    const uint8_t* text_utf8;
    size_t text_len;
    float confidence;
    SnowOcrQuad quad;
} SnowOcrLineInfo;

SnowOcrEngine* snow_ocr_engine_create(void);
SnowOcrEngine* snow_ocr_engine_create_with_directml(uint8_t enabled);
SnowOcrEngine* snow_ocr_engine_create_with_config(
    const SnowOcrEngineConfig* config
);
uint8_t snow_ocr_directml_is_available(void);
uint8_t snow_ocr_engine_uses_directml(const SnowOcrEngine* engine);
uint8_t snow_ocr_runtime_info(SnowOcrRuntimeInfo* out_info);
uint8_t snow_ocr_resource_counts(SnowOcrResourceCounts* out_counts);
void snow_ocr_engine_destroy(SnowOcrEngine* engine);
SnowOcrResult* snow_ocr_engine_recognize_rgba(
    SnowOcrEngine* engine,
    const SnowOcrRequest* request
);
void snow_ocr_result_destroy(SnowOcrResult* result);
size_t snow_ocr_result_line_count(const SnowOcrResult* result);
uint8_t snow_ocr_result_line(
    const SnowOcrResult* result,
    size_t line_index,
    SnowOcrLineInfo* out_line
);
const char* snow_ocr_last_error_message(void);

#ifdef __cplusplus
}
#endif

#endif
