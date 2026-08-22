#pragma once

#include <stdint.h>

#if defined(_WIN32) && !defined(SNOW_SHOT_IMAGE_CODEC_BACKEND_STATIC)
#if defined(SNOW_SHOT_IMAGE_CODEC_BACKEND_BUILD)
#define SNOW_SHOT_IMAGE_CODEC_API __declspec(dllexport)
#else
#define SNOW_SHOT_IMAGE_CODEC_API __declspec(dllimport)
#endif
#define SNOW_SHOT_IMAGE_CODEC_CALL __cdecl
#elif defined(_WIN32)
#define SNOW_SHOT_IMAGE_CODEC_API
#define SNOW_SHOT_IMAGE_CODEC_CALL __cdecl
#elif defined(__GNUC__) || defined(__clang__)
#define SNOW_SHOT_IMAGE_CODEC_API __attribute__((visibility("default")))
#define SNOW_SHOT_IMAGE_CODEC_CALL
#else
#define SNOW_SHOT_IMAGE_CODEC_API
#define SNOW_SHOT_IMAGE_CODEC_CALL
#endif

#define SNOW_SHOT_IMAGE_CODEC_ABI_VERSION 2U

#ifdef __cplusplus
extern "C" {
#endif

enum SnowShotImageCodecFormat {
    SNOW_SHOT_IMAGE_CODEC_FORMAT_UNKNOWN = 0,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_BMP = 1,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_CUR = 2,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_GIF = 3,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_ICO = 4,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_JPEG = 5,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_PBM = 6,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_PGM = 7,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG = 8,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_PPM = 9,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_SVG = 10,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_SVGZ = 11,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_XBM = 12,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_XPM = 13,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_HEIF = 14,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_AVIF = 15,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_JXL = 16,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_EXR = 17,
    SNOW_SHOT_IMAGE_CODEC_FORMAT_WEBP = 18,
};

enum SnowShotImageCodecChromaSubsampling {
    SNOW_SHOT_IMAGE_CODEC_CHROMA_NONE = 0,
    SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV444 = 1,
    SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV422 = 2,
    SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV420 = 3,
    SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV440 = 4,
    SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV411 = 5,
    SNOW_SHOT_IMAGE_CODEC_CHROMA_YUV441 = 6,
};

enum SnowShotImageCodecAlphaContent {
    SNOW_SHOT_IMAGE_CODEC_ALPHA_OPAQUE = 0,
    SNOW_SHOT_IMAGE_CODEC_ALPHA_NON_OPAQUE = 1,
};

typedef struct SnowShotImageCodecEncodeOptions {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t format;
    int32_t quality;
    int32_t effort;
    int32_t lossless_effort;
    int32_t compression_level;
    uint8_t lossless;
    uint8_t preserve_metadata;
    uint8_t progressive;
    uint8_t interlaced;
    uint8_t has_chroma_subsampling;
    uint8_t chroma_subsampling;
    uint8_t has_verified_alpha_content;
    uint8_t verified_alpha_content;
} SnowShotImageCodecEncodeOptions;

typedef struct SnowShotImageCodecBuffer {
    uint8_t* data;
    uint64_t size;
    uint32_t width;
    uint32_t height;
    uint64_t row_stride;
} SnowShotImageCodecBuffer;

typedef struct SnowShotImageCodecImageInfo {
    uint32_t width;
    uint32_t height;
} SnowShotImageCodecImageInfo;

typedef int32_t(SNOW_SHOT_IMAGE_CODEC_CALL* SnowShotImageCodecReadRowsCallback)(
    void* context, uint32_t first_row, uint32_t row_count, uint64_t destination_stride,
    uint8_t* destination, uint64_t destination_size);
typedef int32_t(SNOW_SHOT_IMAGE_CODEC_CALL* SnowShotImageCodecWriteCallback)(void* context,
                                                                             const uint8_t* source,
                                                                             uint64_t source_size);
typedef int32_t(SNOW_SHOT_IMAGE_CODEC_CALL* SnowShotImageCodecPositionCallback)(void* context,
                                                                                uint64_t* position);
typedef int32_t(SNOW_SHOT_IMAGE_CODEC_CALL* SnowShotImageCodecSeekCallback)(void* context,
                                                                            uint64_t position);
typedef int32_t(SNOW_SHOT_IMAGE_CODEC_CALL* SnowShotImageCodecFlushCallback)(void* context);
typedef int32_t(SNOW_SHOT_IMAGE_CODEC_CALL* SnowShotImageCodecCancelCallback)(void* context);

typedef struct SnowShotImageCodecRgba8Source {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    uint32_t width;
    uint32_t height;
    SnowShotImageCodecReadRowsCallback read_rows;
    SnowShotImageCodecCancelCallback is_cancelled;
} SnowShotImageCodecRgba8Source;

typedef struct SnowShotImageCodecByteSink {
    uint32_t struct_size;
    uint32_t abi_version;
    void* context;
    SnowShotImageCodecWriteCallback write;
    SnowShotImageCodecPositionCallback position;
    SnowShotImageCodecSeekCallback seek;
    SnowShotImageCodecFlushCallback flush;
    SnowShotImageCodecCancelCallback is_cancelled;
    uint8_t seekable;
    uint8_t reserved[7];
} SnowShotImageCodecByteSink;

SNOW_SHOT_IMAGE_CODEC_API uint32_t SNOW_SHOT_IMAGE_CODEC_CALL
snow_shot_image_codec_abi_version(void);

// Output buffers must be zero-initialized and released before being reused.
SNOW_SHOT_IMAGE_CODEC_API int32_t SNOW_SHOT_IMAGE_CODEC_CALL snow_shot_image_codec_encode_rgba8(
    const uint8_t* pixels, uint64_t pixels_size, uint32_t width, uint32_t height,
    uint64_t row_stride, const SnowShotImageCodecEncodeOptions* options,
    SnowShotImageCodecBuffer* output, char* error, uint64_t error_capacity);

SNOW_SHOT_IMAGE_CODEC_API int32_t SNOW_SHOT_IMAGE_CODEC_CALL
snow_shot_image_codec_encode_rgba8_stream(const SnowShotImageCodecRgba8Source* source,
                                          const SnowShotImageCodecByteSink* sink,
                                          const SnowShotImageCodecEncodeOptions* options,
                                          uint64_t* bytes_written, char* error,
                                          uint64_t error_capacity);

SNOW_SHOT_IMAGE_CODEC_API int32_t SNOW_SHOT_IMAGE_CODEC_CALL snow_shot_image_codec_decode_rgba8(
    const uint8_t* encoded, uint64_t encoded_size, uint32_t expected_format,
    SnowShotImageCodecBuffer* output, char* error, uint64_t error_capacity);

SNOW_SHOT_IMAGE_CODEC_API int32_t SNOW_SHOT_IMAGE_CODEC_CALL snow_shot_image_codec_inspect(
    const uint8_t* encoded, uint64_t encoded_size, uint32_t expected_format,
    SnowShotImageCodecImageInfo* output, char* error, uint64_t error_capacity);

SNOW_SHOT_IMAGE_CODEC_API void SNOW_SHOT_IMAGE_CODEC_CALL
snow_shot_image_codec_release_buffer(SnowShotImageCodecBuffer* buffer);

#ifdef __cplusplus
}
#endif
