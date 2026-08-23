#include "snowimagecodecbridge.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <vector>

namespace {
struct StreamContext final {
    const uint8_t* pixels = nullptr;
    uint32_t width = 3;
    uint32_t height = 2;
    std::vector<uint8_t> output;
    uint64_t position = 0;
    bool cancelled = false;
    uint32_t maximumRequestedRows = 0;
    uint32_t readCount = 0;
};

int32_t SNOW_SHOT_IMAGE_CODEC_CALL readRows(void* rawContext, uint32_t firstRow, uint32_t rowCount,
                                            uint64_t destinationStride, uint8_t* destination,
                                            uint64_t destinationSize) {
    auto* context = static_cast<StreamContext*>(rawContext);
    if (context == nullptr) {
        return 0;
    }
    const uint64_t rowBytes = static_cast<uint64_t>(context->width) * 4U;
    if (context->cancelled || rowCount == 0 || firstRow + rowCount > context->height ||
        destinationStride < rowBytes ||
        destinationSize < destinationStride * (rowCount - 1U) + rowBytes) {
        return 0;
    }
    context->maximumRequestedRows = (std::max)(context->maximumRequestedRows, rowCount);
    ++context->readCount;
    for (uint32_t row = 0; row < rowCount; ++row) {
        uint8_t* output = destination + row * destinationStride;
        if (context->pixels != nullptr) {
            std::memcpy(output, context->pixels + (firstRow + row) * rowBytes, rowBytes);
            continue;
        }
        for (uint32_t column = 0; column < context->width; ++column) {
            output[column * 4U] = static_cast<uint8_t>((column * 17U + firstRow + row) & 0xffU);
            output[column * 4U + 1U] = static_cast<uint8_t>((firstRow + row) & 0xffU);
            output[column * 4U + 2U] = static_cast<uint8_t>((column * 31U) & 0xffU);
            output[column * 4U + 3U] = static_cast<uint8_t>(128U + (column & 0x7fU));
        }
    }
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL writeBytes(void* rawContext, const uint8_t* source,
                                              uint64_t sourceSize) {
    auto* context = static_cast<StreamContext*>(rawContext);
    if (context == nullptr || context->cancelled || (source == nullptr && sourceSize != 0)) {
        return 0;
    }
    const uint64_t end = context->position + sourceSize;
    if (end < context->position || end > SIZE_MAX) {
        return 0;
    }
    if (context->output.size() < end) {
        context->output.resize(static_cast<std::size_t>(end));
    }
    std::memcpy(context->output.data() + context->position, source,
                static_cast<std::size_t>(sourceSize));
    context->position = end;
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL position(void* rawContext, uint64_t* output) {
    auto* context = static_cast<StreamContext*>(rawContext);
    if (context == nullptr || output == nullptr) {
        return 0;
    }
    *output = context->position;
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL seek(void* rawContext, uint64_t value) {
    auto* context = static_cast<StreamContext*>(rawContext);
    if (context == nullptr || value > context->output.size()) {
        return 0;
    }
    context->position = value;
    return 1;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL flush(void* rawContext) {
    return rawContext != nullptr ? 1 : 0;
}

int32_t SNOW_SHOT_IMAGE_CODEC_CALL cancelled(void* rawContext) {
    const auto* context = static_cast<const StreamContext*>(rawContext);
    return context != nullptr && context->cancelled ? 1 : 0;
}

bool roundTripRequiredFormats(const std::array<uint8_t, 3U * 2U * 4U>& pixels,
                              SnowShotImageCodecEncodeOptions options,
                              std::array<char, 512>* error) {
    struct FormatCase final {
        uint32_t format;
        const char* name;
        bool lossless;
    };
    constexpr std::array formats{
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG, "PNG", true},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_JPEG, "JPEG", false},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_AVIF, "AVIF (libheif/AOM)", true},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_JXL, "JPEG XL", true},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_WEBP, "WebP", true},
    };

    for (const FormatCase& format : formats) {
        options.format = format.format;
        options.quality = format.lossless ? 100 : 90;
        options.lossless = format.lossless ? 1 : 0;
        error->fill('\0');

        SnowShotImageCodecBuffer encoded{};
        if (snow_shot_image_codec_encode_rgba8(pixels.data(), pixels.size(), 3, 2, 3U * 4U,
                                               &options, &encoded, error->data(),
                                               error->size()) == 0 ||
            encoded.data == nullptr || encoded.size == 0) {
            std::cerr << format.name << " encode failed: " << error->data() << '\n';
            snow_shot_image_codec_release_buffer(&encoded);
            return false;
        }

        SnowShotImageCodecImageInfo information{};
        error->fill('\0');
        const bool inspected =
            snow_shot_image_codec_inspect(encoded.data, encoded.size, format.format, &information,
                                          error->data(), error->size()) != 0 &&
            information.width == 3 && information.height == 2;

        SnowShotImageCodecBuffer decoded{};
        error->fill('\0');
        const bool roundTripped =
            snow_shot_image_codec_decode_rgba8(encoded.data, encoded.size, format.format, &decoded,
                                               error->data(), error->size()) != 0 &&
            decoded.data != nullptr && decoded.width == 3 && decoded.height == 2 &&
            decoded.row_stride == 3U * 4U && decoded.size == 3U * 2U * 4U;
        snow_shot_image_codec_release_buffer(&decoded);
        snow_shot_image_codec_release_buffer(&encoded);
        if (!inspected || !roundTripped) {
            std::cerr << format.name << " inspect/decode failed: " << error->data() << '\n';
            return false;
        }
    }
    return true;
}
} // namespace

int main() {
    constexpr std::array<uint8_t, 3U * 2U * 4U> pixels{
        255, 0,   0,   255, 0, 255, 0, 255, 0,  0,   255, 255,
        255, 255, 255, 255, 0, 0,   0, 255, 80, 100, 120, 255,
    };
    SnowShotImageCodecEncodeOptions options{};
    options.struct_size = static_cast<uint32_t>(sizeof(options));
    options.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    options.format = SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG;
    options.quality = 90;
    options.effort = 7;
    options.lossless_effort = 6;
    options.compression_level = 0;
    options.preserve_metadata = 0;

    SnowShotImageCodecBuffer output{};
    std::array<char, 512> error{};
    const int32_t succeeded = snow_shot_image_codec_encode_rgba8(
        pixels.data(), pixels.size(), 3, 2, 3U * 4U, &options, &output, error.data(), error.size());
    const bool hasPngSignature = output.size >= 8 && output.data != nullptr &&
                                 output.data[0] == 0x89 && output.data[1] == 'P' &&
                                 output.data[2] == 'N' && output.data[3] == 'G';

    uint8_t* const originalData = output.data;
    const uint64_t originalSize = output.size;
    error.fill('\0');
    const int32_t reusedWithoutRelease = snow_shot_image_codec_encode_rgba8(
        pixels.data(), pixels.size(), 3, 2, 3U * 4U, &options, &output, error.data(), error.size());
    const bool rejectedUnsafeReuse = reusedWithoutRelease == 0 && output.data == originalData &&
                                     output.size == originalSize && error[0] != '\0';

    StreamContext streamContext{pixels.data()};
    SnowShotImageCodecRgba8Source source{};
    source.struct_size = sizeof(source);
    source.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    source.context = &streamContext;
    source.width = 3;
    source.height = 2;
    source.read_rows = &readRows;
    source.is_cancelled = &cancelled;
    SnowShotImageCodecByteSink sink{};
    sink.struct_size = sizeof(sink);
    sink.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    sink.context = &streamContext;
    sink.write = &writeBytes;
    sink.position = &position;
    sink.seek = &seek;
    sink.flush = &flush;
    sink.is_cancelled = &cancelled;
    sink.seekable = 1;
    uint64_t streamBytes = 0;
    error.fill('\0');
    const int32_t streamed = snow_shot_image_codec_encode_rgba8_stream(
        &source, &sink, &options, &streamBytes, error.data(), error.size());
    const bool streamedPng = streamed != 0 && streamBytes == streamContext.output.size() &&
                             streamContext.output.size() >= 8 && streamContext.output[0] == 0x89 &&
                             streamContext.output[1] == 'P' && streamContext.output[2] == 'N' &&
                             streamContext.output[3] == 'G';

    StreamContext tallPngContext;
    tallPngContext.width = 7;
    tallPngContext.height = 20000;
    source.context = &tallPngContext;
    source.width = tallPngContext.width;
    source.height = tallPngContext.height;
    sink.context = &tallPngContext;
    streamBytes = 0;
    error.fill('\0');
    const int32_t tallPng = snow_shot_image_codec_encode_rgba8_stream(
        &source, &sink, &options, &streamBytes, error.data(), error.size());
    const bool tallPngStreamedRows = tallPng != 0 && streamBytes > 0 &&
                                     tallPngContext.maximumRequestedRows == 1 &&
                                     tallPngContext.readCount == tallPngContext.height;

    StreamContext tallJpegContext;
    tallJpegContext.width = 7;
    tallJpegContext.height = 20000;
    source.context = &tallJpegContext;
    source.width = tallJpegContext.width;
    source.height = tallJpegContext.height;
    sink.context = &tallJpegContext;
    options.format = SNOW_SHOT_IMAGE_CODEC_FORMAT_JPEG;
    streamBytes = 0;
    error.fill('\0');
    const int32_t tallJpeg = snow_shot_image_codec_encode_rgba8_stream(
        &source, &sink, &options, &streamBytes, error.data(), error.size());
    const bool tallJpegStreamedRows =
        tallJpeg != 0 && streamBytes > 2 && tallJpegContext.output[0] == 0xff &&
        tallJpegContext.output[1] == 0xd8 && tallJpegContext.maximumRequestedRows == 1 &&
        tallJpegContext.readCount == tallJpegContext.height;

    source.context = &streamContext;
    source.width = streamContext.width;
    source.height = streamContext.height;
    sink.context = &streamContext;
    options.format = SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG;
    streamContext.cancelled = true;
    streamContext.output.clear();
    streamContext.position = 0;
    streamBytes = 0;
    error.fill('\0');
    const int32_t cancelledStream = snow_shot_image_codec_encode_rgba8_stream(
        &source, &sink, &options, &streamBytes, error.data(), error.size());
    const bool propagatedCancellation =
        cancelledStream == 0 && streamBytes == 0 && error[0] != '\0';

    snow_shot_image_codec_release_buffer(&output);
    snow_shot_image_codec_release_buffer(&output);
    const bool requiredFormatsRoundTrip = roundTripRequiredFormats(pixels, options, &error);
    if (snow_shot_image_codec_abi_version() != SNOW_SHOT_IMAGE_CODEC_ABI_VERSION ||
        succeeded == 0 || !hasPngSignature || !rejectedUnsafeReuse || output.data != nullptr ||
        output.size != 0 || !streamedPng || !tallPngStreamedRows || !tallJpegStreamedRows ||
        !propagatedCancellation || !requiredFormatsRoundTrip) {
        std::cerr << (error[0] == '\0' ? "The C ABI PNG smoke test failed." : error.data()) << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
