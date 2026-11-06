#pragma once

#include "snow/image/export.h"

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

namespace snow::image {

enum class Format : std::uint8_t {
    unknown = 0,
    bmp,
    cur,
    gif,
    ico,
    jpeg,
    pbm,
    pgm,
    png,
    ppm,
    svg,
    svgz,
    xbm,
    xpm,
    heif,
    avif,
    jxl,
    exr,
    webp,
    tiff,
};

enum class EncoderFeature : std::uint32_t {
    none = 0,
    alpha = 1U << 0U,
    animation = 1U << 1U,
    indexed = 1U << 2U,
    quality = 1U << 3U,
    lossless = 1U << 4U,
    effort = 1U << 5U,
    progressive = 1U << 6U,
    interlaced = 1U << 7U,
    compression_level = 1U << 8U,
    // Accepted input samples survive an encode/decode round trip unchanged.
    pixel_exact = 1U << 9U,
    chroma_subsampling = 1U << 10U,
    metadata = 1U << 11U,
};

constexpr EncoderFeature operator|(EncoderFeature left, EncoderFeature right) noexcept {
    // Bitmask combinations do not require a matching named enumerator.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<EncoderFeature>(static_cast<std::uint32_t>(left) |
                                       static_cast<std::uint32_t>(right));
}
constexpr EncoderFeature operator&(EncoderFeature left, EncoderFeature right) noexcept {
    return static_cast<EncoderFeature>(static_cast<std::uint32_t>(left) &
                                       static_cast<std::uint32_t>(right));
}
constexpr bool has_feature(EncoderFeature set, EncoderFeature value) noexcept {
    return (set & value) == value;
}

struct EncoderOptionRange final {
    int minimum = 0;
    int maximum = 0;
    int default_value = 0;
};

struct EncoderLimits final {
    std::uint32_t maximum_width = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_height = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t maximum_frames = std::numeric_limits<std::uint32_t>::max();
};

enum class CodecCancellation : std::uint8_t {
    cooperative,
    terminate_worker,
};

struct EncoderInfo final {
    Format format = Format::unknown;
    EncoderFeature features = EncoderFeature::none;
    EncoderOptionRange quality;
    EncoderOptionRange effort;
    EncoderOptionRange lossless_effort;
    EncoderOptionRange compression_level;
    EncoderLimits limits;
    CodecCancellation cancellation = CodecCancellation::terminate_worker;
};

enum class CodecCapability : std::uint32_t {
    none = 0,
    inspect = 1U << 0U,
    decode = 1U << 1U,
    encode = 1U << 2U,
    animation = 1U << 3U,
    multiple_images = 1U << 4U,
    streaming_decode = 1U << 5U,
    metadata_decode = 1U << 6U,
    hdr = 1U << 7U,
    deep_data = 1U << 8U,
    vector = 1U << 9U,
};

constexpr CodecCapability operator|(CodecCapability left, CodecCapability right) noexcept {
    // Bitmask combinations do not require a matching named enumerator.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    return static_cast<CodecCapability>(static_cast<std::uint32_t>(left) |
                                        static_cast<std::uint32_t>(right));
}
constexpr CodecCapability operator&(CodecCapability left, CodecCapability right) noexcept {
    return static_cast<CodecCapability>(static_cast<std::uint32_t>(left) &
                                        static_cast<std::uint32_t>(right));
}
constexpr bool has_capability(CodecCapability set, CodecCapability value) noexcept {
    return (set & value) == value;
}

struct FormatCapability final {
    Format format = Format::unknown;
    CodecCapability capabilities = CodecCapability::none;
    std::string_view codec_name;
    std::span<const std::string_view> extensions;
};

SNOW_IMAGE_API std::string_view format_name(Format format) noexcept;
SNOW_IMAGE_API Format format_from_extension(std::string_view extension) noexcept;
SNOW_IMAGE_API std::span<const std::string_view> format_extensions(Format format) noexcept;

} // namespace snow::image
