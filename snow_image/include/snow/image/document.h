#pragma once

#include "snow/image/format.h"
#include "snow/image/image.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace snow::image {

enum class DynamicRange : std::uint8_t { standard, high };
enum class TransferFunction : std::uint8_t { unknown, linear, srgb, gamma, pq, hlg };
enum class ColorPrimaries : std::uint8_t { unknown, srgb, display_p3, adobe_rgb, rec2020, custom };
enum class Orientation : std::uint8_t {
    identity = 1,
    mirror_horizontal = 2,
    rotate_180 = 3,
    mirror_vertical = 4,
    mirror_horizontal_rotate_270 = 5,
    rotate_90 = 6,
    mirror_horizontal_rotate_90 = 7,
    rotate_270 = 8,
};

struct ColorEncoding final {
    ColorPrimaries primaries = ColorPrimaries::unknown;
    TransferFunction transfer = TransferFunction::unknown;
    DynamicRange dynamic_range = DynamicRange::standard;
    std::vector<std::byte> icc_profile;
    float source_peak_nits = 0.0F;
    float diffuse_white_nits = 80.0F;
    std::optional<std::array<float, 8>> mastering_primaries_and_white;
    std::optional<std::uint32_t> max_content_light_level;
    std::optional<std::uint32_t> max_frame_average_light_level;
};

struct MetadataBlock final {
    std::string type;
    std::string content_type;
    std::vector<std::byte> data;
    bool safe_to_copy = false;
};

struct Metadata final {
    Orientation orientation = Orientation::identity;
    std::optional<double> horizontal_dpi;
    std::optional<double> vertical_dpi;
    std::string comment;
    std::vector<std::byte> exif;
    std::vector<std::byte> xmp;
    std::vector<MetadataBlock> blocks;
};

enum class FrameBlend : std::uint8_t { source, over };
enum class FrameDisposal : std::uint8_t { keep, background, previous };

struct Frame final {
    Image image;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::chrono::nanoseconds duration{0};
    FrameBlend blend = FrameBlend::source;
    FrameDisposal disposal = FrameDisposal::keep;
    Metadata metadata;
    ColorEncoding color;
    std::optional<std::array<std::uint32_t, 2>> cursor_hotspot;
};

enum class ExrPartType : std::uint8_t { scanline, tiled, deep_scanline, deep_tiled };
enum class ExrLevelMode : std::uint8_t { one_level, mipmap, ripmap };

struct ExrChannel final {
    std::string name;
    SampleType sample_type = SampleType::floating_point;
    std::uint8_t bits_per_sample = 16;
    std::uint32_t x_sampling = 1;
    std::uint32_t y_sampling = 1;
    std::vector<std::byte> samples;
};

struct DeepSamples final {
    std::vector<std::uint32_t> counts;
    std::vector<ExrChannel> channels;
};

struct ExrLevel final {
    std::int32_t x_level = 0;
    std::int32_t y_level = 0;
    std::array<std::int32_t, 4> data_window{};
    std::vector<ExrChannel> channels;
    std::optional<DeepSamples> deep_samples;
};

struct ExrPart final {
    std::string name;
    ExrPartType type = ExrPartType::scanline;
    std::array<std::int32_t, 4> data_window{};
    std::array<std::int32_t, 4> display_window{};
    std::uint32_t tile_width = 0;
    std::uint32_t tile_height = 0;
    ExrLevelMode level_mode = ExrLevelMode::one_level;
    std::vector<ExrChannel> channels;
    std::optional<DeepSamples> deep_samples;
    // Additional tiled levels. The base level (0,0) remains in channels/deep_samples.
    std::vector<ExrLevel> levels;
    std::vector<MetadataBlock> attributes;
};

struct VectorDocument final {
    std::vector<std::byte> source;
    bool gzip_compressed = false;
};

struct Document final {
    Format format = Format::unknown;
    std::uint32_t canvas_width = 0;
    std::uint32_t canvas_height = 0;
    std::uint32_t loop_count = 1;
    Metadata metadata;
    ColorEncoding color;
    std::vector<Frame> frames;
    std::vector<ExrPart> exr_parts;
    std::optional<VectorDocument> vector;
};

struct FrameInfo final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::chrono::nanoseconds duration{0};
    PixelFormat native_format;
    bool has_alpha = false;
    std::optional<std::array<std::uint32_t, 2>> cursor_hotspot;
    ColorEncoding color{};
    Metadata metadata{};
    FrameBlend blend = FrameBlend::source;
    FrameDisposal disposal = FrameDisposal::keep;
};

struct DocumentInfo final {
    Format format = Format::unknown;
    std::uint32_t canvas_width = 0;
    std::uint32_t canvas_height = 0;
    std::uint32_t loop_count = 1;
    Metadata metadata;
    ColorEncoding color;
    std::vector<FrameInfo> frames;
    std::vector<ExrPart> exr_parts;
    bool is_vector = false;
};

} // namespace snow::image
