#include "snowimagecodecbridge.h"
#include "../../test-support/virtualmemory.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <vector>

namespace {
struct StreamContext final {
    explicit StreamContext(const uint8_t* source = nullptr, uint32_t imageWidth = 3,
                           uint32_t imageHeight = 2)
        : pixels(source), width(imageWidth), height(imageHeight) {}

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

SnowShotImageCodecRgba8Source bridgeSource(StreamContext* context) {
    SnowShotImageCodecRgba8Source source{};
    source.struct_size = sizeof(source);
    source.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    source.context = context;
    source.width = context->width;
    source.height = context->height;
    source.read_rows = &readRows;
    source.is_cancelled = &cancelled;
    return source;
}

SnowShotImageCodecByteSink bridgeSink(StreamContext* context) {
    SnowShotImageCodecByteSink sink{};
    sink.struct_size = sizeof(sink);
    sink.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    sink.context = context;
    sink.write = &writeBytes;
    sink.position = &position;
    sink.seek = &seek;
    sink.flush = &flush;
    sink.is_cancelled = &cancelled;
    sink.seekable = 1;
    return sink;
}

SnowShotImageCodecEncodeResult encodeResult() {
    SnowShotImageCodecEncodeResult result{};
    result.struct_size = sizeof(result);
    result.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    return result;
}

bool pngDeclaresSrgb(const uint8_t* bytes, uint64_t size) {
    for (uint64_t offset = 8; bytes != nullptr && offset <= size && size - offset >= 12;) {
        const uint32_t length = (uint32_t(bytes[offset]) << 24) |
                                (uint32_t(bytes[offset + 1]) << 16) |
                                (uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
        if (uint64_t(length) > size - offset - 12)
            return false;
        if (std::memcmp(bytes + offset + 4, "sRGB", 4) == 0)
            return length == 1 && bytes[offset + 8] <= 3;
        offset += uint64_t(length) + 12;
    }
    return false;
}

bool srgbEncodingsRetainColorDescription(const std::array<uint8_t, 3U * 2U * 4U>& pixels,
                                         SnowShotImageCodecEncodeOptions options,
                                         std::array<char, 512>* error) {
    options.format = SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG;
    options.preserve_metadata = 1;
    SnowShotImageCodecBuffer encoded{};
    const bool packed =
        snow_shot_image_codec_encode_rgba8(pixels.data(), pixels.size(), 3, 2, 12, &options,
                                           &encoded, error->data(), error->size()) != 0 &&
        pngDeclaresSrgb(encoded.data, encoded.size);
    snow_shot_image_codec_release_buffer(&encoded);
    StreamContext context{pixels.data()};
    const auto source = bridgeSource(&context);
    const auto sink = bridgeSink(&context);
    auto receipt = encodeResult();
    const bool streamed =
        snow_shot_image_codec_encode_rgba8_stream(&source, &sink, &options, &receipt, error->data(),
                                                  error->size()) != 0 &&
        pngDeclaresSrgb(context.output.data(), context.output.size());
    if (!packed || !streamed)
        std::cerr << "Packed and streamed screenshot PNGs must declare sRGB\n";
    return packed && streamed;
}

bool roundTripRequiredFormats(const std::array<uint8_t, 3U * 2U * 4U>& pixels,
                              SnowShotImageCodecEncodeOptions options,
                              std::array<char, 512>* error) {
    struct FormatCase final {
        uint32_t format;
        const char* name;
        bool lossless;
        bool exact;
    };
    constexpr std::array formats{
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG, "PNG", true, true},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_JPEG, "JPEG", false, false},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_AVIF, "AVIF (libheif/AOM)", true, false},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_JXL, "lossless JPEG XL", true, true},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_JXL, "lossy JPEG XL", false, false},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_WEBP, "lossless WebP", true, true},
        FormatCase{SNOW_SHOT_IMAGE_CODEC_FORMAT_WEBP, "lossy WebP", false, false},
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

        StreamContext streamContext{pixels.data()};
        SnowShotImageCodecRgba8Source source = bridgeSource(&streamContext);
        SnowShotImageCodecByteSink sink = bridgeSink(&streamContext);
        SnowShotImageCodecEncodeResult receipt = encodeResult();
        error->fill('\0');
        const bool receiptMatches =
            snow_shot_image_codec_encode_rgba8_stream(&source, &sink, &options, &receipt,
                                                      error->data(), error->size()) != 0 &&
            receipt.bytes_written == streamContext.output.size() &&
            receipt.encoded_format == format.format && receipt.canvas_width == 3 &&
            receipt.canvas_height == 2 && receipt.emitted_frame_count == 1 &&
            receipt.encoder_finalized_and_sink_flushed != 0 &&
            receipt.pixel_round_trip ==
                static_cast<uint8_t>(format.exact
                                         ? SNOW_SHOT_IMAGE_CODEC_PIXEL_ROUND_TRIP_EXACT
                                         : SNOW_SHOT_IMAGE_CODEC_PIXEL_ROUND_TRIP_CODEC_ARTIFACT);
        snow_shot_image_codec_release_buffer(&decoded);
        snow_shot_image_codec_release_buffer(&encoded);
        if (!inspected || !roundTripped || !receiptMatches) {
            std::cerr << format.name << " inspect/decode failed: " << error->data() << '\n';
            return false;
        }
    }
    return true;
}
} // namespace

bool largeBuffersReleaseAcrossCodecBoundary() {
    constexpr uint32_t width = 1025;
    constexpr uint32_t height = 513;
    std::vector<uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    uint32_t random = 0x12345678;
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        pixels[index] = static_cast<uint8_t>(random);
    }
    SnowShotImageCodecEncodeOptions options{};
    options.struct_size = sizeof(options);
    options.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION;
    options.format = SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG;
    options.compression_level = 1;
    std::array<char, 512> error{};
    SnowShotImageCodecBuffer encoded{};
    SnowShotImageCodecBuffer decoded{};
    if (!snow_shot_image_codec_encode_rgba8(pixels.data(), pixels.size(), width, height,
                                            uint64_t(width) * 4, &options, &encoded, error.data(),
                                            error.size()) ||
        encoded.size < 1024 * 1024) {
        snow_shot_image_codec_release_buffer(&encoded);
        return false;
    }
    const bool valid = snow_shot_image_codec_decode_rgba8(
                           encoded.data, encoded.size, SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG, &decoded,
                           error.data(), error.size()) != 0 &&
                       decoded.size == pixels.size() &&
                       std::memcmp(decoded.data, pixels.data(), pixels.size()) == 0;
    const auto* encodedMiddle = encoded.data + encoded.size / 2;
    const auto* decodedMiddle = decoded.data ? decoded.data + decoded.size / 2 : nullptr;
    snow_shot_image_codec_release_buffer(&encoded);
    snow_shot_image_codec_release_buffer(&decoded);
    return valid && !snow::test_support::virtualMemoryMapped(encodedMiddle) &&
           !snow::test_support::virtualMemoryMapped(decodedMiddle) && !encoded.data &&
           !decoded.data;
}

int main() {
    if (!largeBuffersReleaseAcrossCodecBoundary()) {
        std::cerr << "Large codec buffers must preserve pixels and unmap on release\n";
        return EXIT_FAILURE;
    }
    constexpr std::array<uint8_t, 12> resizePixels{0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    std::array<uint8_t, 20> resized{};
    std::array<char, 512> resizeError{};
    StreamContext resizeContext{resizePixels.data(), 3, 1};
    SnowShotImageCodecRgba8Source resizeSource = bridgeSource(&resizeContext);
    SnowShotImageCodecRgba8Source invalidResizeSource = resizeSource;
    invalidResizeSource.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION + 1U;
    const bool rejectedInvalidResizeSource =
        snow_shot_image_codec_resize_rgba8(&invalidResizeSource, resized.data(), 5U * 4U,
                                           resized.size(), 5, 1, resizeError.data(),
                                           resizeError.size()) == 0 &&
        resizeError[0] != '\0';
    resizeError.fill('\0');
    if (!snow_shot_image_codec_resize_rgba8(&resizeSource, resized.data(), 5U * 4U, resized.size(),
                                            5, 1, resizeError.data(), resizeError.size()) ||
        resized[0] != 0 || resized[8] != 255 || resized[16] != 0 ||
        std::abs(int(resized[4]) - 170) > 1 || std::abs(int(resized[12]) - 170) > 1) {
        std::cerr << "Export resizing must use linear interpolation in linear RGB: "
                  << resizeError.data() << '\n';
        return EXIT_FAILURE;
    }
    StreamContext tallResizeContext;
    tallResizeContext.width = 7;
    tallResizeContext.height = 5000;
    SnowShotImageCodecRgba8Source tallResizeSource = bridgeSource(&tallResizeContext);
    std::array<uint8_t, 2U * 3U * 4U> tallResized{};
    resizeError.fill('\0');
    const bool tallResizeStreamed =
        snow_shot_image_codec_resize_rgba8(&tallResizeSource, tallResized.data(), 2U * 4U,
                                           tallResized.size(), 2, 3, resizeError.data(),
                                           resizeError.size()) != 0 &&
        tallResizeContext.maximumRequestedRows == 1 &&
        tallResizeContext.readCount <= tallResizeContext.height;
    constexpr std::array<uint8_t, 12> alphaPixels{255, 0, 0, 255, 0, 0, 255, 0, 255, 0, 0, 255};
    resizeContext.pixels = alphaPixels.data();
    if (!snow_shot_image_codec_resize_rgba8(&resizeSource, resized.data(), 5U * 4U, resized.size(),
                                            5, 1, resizeError.data(), resizeError.size()) ||
        resized[4] != 255 || resized[5] != 0 || resized[6] != 0 || resized[7] != 153 ||
        resized[11] != 0) {
        std::cerr << "Linear export resizing must premultiply alpha: " << resizeError.data()
                  << '\n';
        return EXIT_FAILURE;
    }
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

    SnowShotImageCodecBuffer bgra{};
    error.fill('\0');
    const int32_t decodedBgra = snow_shot_image_codec_decode_bgra8(
        output.data, output.size, SNOW_SHOT_IMAGE_CODEC_FORMAT_PNG, &bgra, error.data(),
        error.size());
    const bool hasBgraPixels = decodedBgra != 0 && bgra.data != nullptr && bgra.width == 3 &&
                               bgra.height == 2 && bgra.row_stride == 3U * 4U &&
                               bgra.size == 3U * 2U * 4U && bgra.data[0] == 0 &&
                               bgra.data[1] == 0 && bgra.data[2] == 255 && bgra.data[3] == 255;
    snow_shot_image_codec_release_buffer(&bgra);

    uint8_t* const originalData = output.data;
    const uint64_t originalSize = output.size;
    error.fill('\0');
    const int32_t reusedWithoutRelease = snow_shot_image_codec_encode_rgba8(
        pixels.data(), pixels.size(), 3, 2, 3U * 4U, &options, &output, error.data(), error.size());
    const bool rejectedUnsafeReuse = reusedWithoutRelease == 0 && output.data == originalData &&
                                     output.size == originalSize && error[0] != '\0';

    StreamContext streamContext{pixels.data()};
    SnowShotImageCodecRgba8Source source = bridgeSource(&streamContext);
    SnowShotImageCodecByteSink sink = bridgeSink(&streamContext);
    SnowShotImageCodecEncodeResult streamResult = encodeResult();
    SnowShotImageCodecEncodeResult invalidResult = encodeResult();
    invalidResult.abi_version = SNOW_SHOT_IMAGE_CODEC_ABI_VERSION + 1U;
    error.fill('\0');
    const bool rejectedInvalidResult =
        snow_shot_image_codec_encode_rgba8_stream(&source, &sink, &options, &invalidResult,
                                                  error.data(), error.size()) == 0 &&
        streamContext.output.empty() && error[0] != '\0';

    SnowShotImageCodecRgba8Source invalidSource = source;
    invalidSource.struct_size = sizeof(invalidSource) - 1U;
    streamResult = encodeResult();
    error.fill('\0');
    const bool rejectedInvalidSource =
        snow_shot_image_codec_encode_rgba8_stream(&invalidSource, &sink, &options, &streamResult,
                                                  error.data(), error.size()) == 0 &&
        streamResult.bytes_written == 0 && streamContext.output.empty() && error[0] != '\0';

    streamResult = encodeResult();
    error.fill('\0');
    const int32_t streamed = snow_shot_image_codec_encode_rgba8_stream(
        &source, &sink, &options, &streamResult, error.data(), error.size());
    const bool streamedPng = streamed != 0 &&
                             streamResult.bytes_written == streamContext.output.size() &&
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
    streamResult = encodeResult();
    error.fill('\0');
    const int32_t tallPng = snow_shot_image_codec_encode_rgba8_stream(
        &source, &sink, &options, &streamResult, error.data(), error.size());
    const bool tallPngStreamedRows = tallPng != 0 && streamResult.bytes_written > 0 &&
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
    streamResult = encodeResult();
    error.fill('\0');
    const int32_t tallJpeg = snow_shot_image_codec_encode_rgba8_stream(
        &source, &sink, &options, &streamResult, error.data(), error.size());
    const bool tallJpegStreamedRows =
        tallJpeg != 0 && streamResult.bytes_written > 2 && tallJpegContext.output[0] == 0xff &&
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
    streamResult = encodeResult();
    error.fill('\0');
    const int32_t cancelledStream = snow_shot_image_codec_encode_rgba8_stream(
        &source, &sink, &options, &streamResult, error.data(), error.size());
    const bool propagatedCancellation =
        cancelledStream == 0 && streamResult.bytes_written == 0 && error[0] != '\0';

    snow_shot_image_codec_release_buffer(&output);
    snow_shot_image_codec_release_buffer(&output);
    const bool requiredFormatsRoundTrip = roundTripRequiredFormats(pixels, options, &error);
    const bool retainedSrgb = srgbEncodingsRetainColorDescription(pixels, options, &error);
    if (snow_shot_image_codec_abi_version() != SNOW_SHOT_IMAGE_CODEC_ABI_VERSION ||
        succeeded == 0 || !hasPngSignature || !rejectedUnsafeReuse || output.data != nullptr ||
        output.size != 0 || !rejectedInvalidResult || !rejectedInvalidSource ||
        !rejectedInvalidResizeSource || !tallResizeStreamed || !streamedPng ||
        !tallPngStreamedRows || !tallJpegStreamedRows || !propagatedCancellation ||
        !requiredFormatsRoundTrip || !hasBgraPixels || !retainedSrgb) {
        std::cerr << (error[0] == '\0' ? "The C ABI PNG smoke test failed." : error.data()) << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
