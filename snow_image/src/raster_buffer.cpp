#include "snow/image/raster.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <random>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace snow::image {
namespace {

constexpr std::array<std::byte, 8> kMagic{std::byte{'S'}, std::byte{'N'}, std::byte{'O'},
                                          std::byte{'W'}, std::byte{'R'}, std::byte{'B'},
                                          std::byte{0}, std::byte{0}};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kWriting = 0;
constexpr std::uint32_t kSealed = 1;
constexpr std::uint64_t kHeaderBytes = 96;
constexpr std::uint64_t kHeaderCrcOffset = 44;
constexpr std::uint64_t kManifestOffset = kHeaderBytes;

Status invalid(std::string message) {
    return Status::error(ErrorCode::invalid_argument, std::move(message), "raster buffer store");
}

Status corrupt(std::string message) {
    return Status::error(ErrorCode::corrupt_data, std::move(message), "raster buffer store");
}

Status limit(std::string message) {
    return Status::error(ErrorCode::limit_exceeded, std::move(message), "raster buffer store");
}

Status cancelled() {
    return Status::error(ErrorCode::cancelled, "Raster buffer operation was cancelled.",
                         "raster buffer store");
}

bool checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (!result || right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    *result = left + right;
    return true;
}

bool checked_multiply(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (!result || (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left))
        return false;
    *result = left * right;
    return true;
}

bool align_up(std::uint64_t value, std::uint64_t alignment, std::uint64_t* result) {
    if (!result || alignment == 0)
        return false;
    const std::uint64_t remainder = value % alignment;
    return remainder == 0 ? (*result = value, true)
                          : checked_add(value, alignment - remainder, result);
}

std::uint32_t crc32c(std::span<const std::byte> bytes) {
    std::uint32_t state = 0xFFFFFFFFU;
    for (const std::byte byte : bytes) {
        state ^= std::to_integer<std::uint8_t>(byte);
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (state & 1U);
            state = (state >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return state ^ 0xFFFFFFFFU;
}

class BinaryWriter final {
  public:
    template <typename T> void integer(T value) {
        using U = std::make_unsigned_t<T>;
        U encoded = static_cast<U>(value);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            bytes_.push_back(static_cast<std::byte>(encoded & U{0xFF}));
            encoded >>= 8U;
        }
    }

    void boolean(bool value) {
        integer<std::uint8_t>(value ? 1U : 0U);
    }
    void floating(float value) {
        integer(std::bit_cast<std::uint32_t>(value));
    }
    void floating(double value) {
        integer(std::bit_cast<std::uint64_t>(value));
    }

    void bytes(std::span<const std::byte> value) {
        integer<std::uint64_t>(static_cast<std::uint64_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void string(const std::string& value) {
        append_bytes(std::as_bytes(std::span(value.data(), value.size())));
    }

    std::vector<std::byte> take() && {
        return std::move(bytes_);
    }

  private:
    // Keep the name of the operation distinct from the member for MSVC's
    // dependent-name diagnostics.
    void append_bytes(std::span<const std::byte> value) {
        integer<std::uint64_t>(static_cast<std::uint64_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    std::vector<std::byte> bytes_;
};

class BinaryReader final {
  public:
    explicit BinaryReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename T> bool integer(T* value) {
        if (!value || remaining() < sizeof(T))
            return false;
        std::uint64_t encoded = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            encoded |=
                static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + index]))
                << (index * 8U);
        }
        offset_ += sizeof(T);
        using U = std::make_unsigned_t<T>;
        *value = static_cast<T>(static_cast<U>(encoded));
        return true;
    }

    bool boolean(bool* value) {
        std::uint8_t encoded = 0;
        if (!value || !integer(&encoded) || encoded > 1)
            return false;
        *value = encoded != 0;
        return true;
    }

    bool floating(float* value) {
        std::uint32_t encoded = 0;
        if (!value || !integer(&encoded))
            return false;
        *value = std::bit_cast<float>(encoded);
        return true;
    }

    bool floating(double* value) {
        std::uint64_t encoded = 0;
        if (!value || !integer(&encoded))
            return false;
        *value = std::bit_cast<double>(encoded);
        return true;
    }

    bool bytes(std::vector<std::byte>* value) {
        std::uint64_t size = 0;
        if (!value || !integer(&size) || size > remaining() ||
            size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return false;
        value->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                      bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
        offset_ += static_cast<std::size_t>(size);
        return true;
    }

    bool string(std::string* value) {
        std::vector<std::byte> encoded;
        if (!value || !bytes(&encoded))
            return false;
        value->assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }

  private:
    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

void write_color(BinaryWriter* writer, const ColorEncoding& color) {
    writer->integer(color.primaries);
    writer->integer(color.transfer);
    writer->integer(color.dynamic_range);
    writer->bytes(color.icc_profile);
    writer->floating(color.source_peak_nits);
    writer->floating(color.diffuse_white_nits);
    writer->boolean(color.mastering_primaries_and_white.has_value());
    if (color.mastering_primaries_and_white) {
        for (const float value : *color.mastering_primaries_and_white)
            writer->floating(value);
    }
    writer->boolean(color.max_content_light_level.has_value());
    if (color.max_content_light_level)
        writer->integer(*color.max_content_light_level);
    writer->boolean(color.max_frame_average_light_level.has_value());
    if (color.max_frame_average_light_level)
        writer->integer(*color.max_frame_average_light_level);
}

bool read_color(BinaryReader* reader, ColorEncoding* color) {
    std::uint8_t primaries = 0;
    std::uint8_t transfer = 0;
    std::uint8_t range = 0;
    bool present = false;
    if (!reader->integer(&primaries) ||
        primaries > static_cast<std::uint8_t>(ColorPrimaries::custom) ||
        !reader->integer(&transfer) ||
        transfer > static_cast<std::uint8_t>(TransferFunction::hlg) || !reader->integer(&range) ||
        range > static_cast<std::uint8_t>(DynamicRange::high) ||
        !reader->bytes(&color->icc_profile) || !reader->floating(&color->source_peak_nits) ||
        !reader->floating(&color->diffuse_white_nits) || !reader->boolean(&present))
        return false;
    color->primaries = static_cast<ColorPrimaries>(primaries);
    color->transfer = static_cast<TransferFunction>(transfer);
    color->dynamic_range = static_cast<DynamicRange>(range);
    if (present) {
        std::array<float, 8> values{};
        for (float& value : values)
            if (!reader->floating(&value))
                return false;
        color->mastering_primaries_and_white = values;
    }
    if (!reader->boolean(&present))
        return false;
    if (present) {
        std::uint32_t value = 0;
        if (!reader->integer(&value))
            return false;
        color->max_content_light_level = value;
    }
    if (!reader->boolean(&present))
        return false;
    if (present) {
        std::uint32_t value = 0;
        if (!reader->integer(&value))
            return false;
        color->max_frame_average_light_level = value;
    }
    return std::isfinite(color->source_peak_nits) && std::isfinite(color->diffuse_white_nits);
}

void write_metadata(BinaryWriter* writer, const Metadata& metadata) {
    writer->integer(metadata.orientation);
    writer->boolean(metadata.horizontal_dpi.has_value());
    if (metadata.horizontal_dpi)
        writer->floating(*metadata.horizontal_dpi);
    writer->boolean(metadata.vertical_dpi.has_value());
    if (metadata.vertical_dpi)
        writer->floating(*metadata.vertical_dpi);
    writer->string(metadata.comment);
    writer->bytes(metadata.exif);
    writer->bytes(metadata.xmp);
    writer->integer<std::uint64_t>(metadata.blocks.size());
    for (const MetadataBlock& block : metadata.blocks) {
        writer->string(block.type);
        writer->string(block.content_type);
        writer->bytes(block.data);
        writer->boolean(block.safe_to_copy);
    }
}

bool read_metadata(BinaryReader* reader, Metadata* metadata) {
    std::uint8_t orientation = 0;
    bool present = false;
    if (!reader->integer(&orientation) ||
        orientation < static_cast<std::uint8_t>(Orientation::identity) ||
        orientation > static_cast<std::uint8_t>(Orientation::rotate_270) ||
        !reader->boolean(&present))
        return false;
    metadata->orientation = static_cast<Orientation>(orientation);
    if (present) {
        double value = 0.0;
        if (!reader->floating(&value) || !std::isfinite(value))
            return false;
        metadata->horizontal_dpi = value;
    }
    if (!reader->boolean(&present))
        return false;
    if (present) {
        double value = 0.0;
        if (!reader->floating(&value) || !std::isfinite(value))
            return false;
        metadata->vertical_dpi = value;
    }
    std::uint64_t block_count = 0;
    if (!reader->string(&metadata->comment) || !reader->bytes(&metadata->exif) ||
        !reader->bytes(&metadata->xmp) || !reader->integer(&block_count) ||
        block_count > reader->remaining())
        return false;
    metadata->blocks.reserve(static_cast<std::size_t>(block_count));
    for (std::uint64_t index = 0; index < block_count; ++index) {
        MetadataBlock block;
        if (!reader->string(&block.type) || !reader->string(&block.content_type) ||
            !reader->bytes(&block.data) || !reader->boolean(&block.safe_to_copy))
            return false;
        metadata->blocks.push_back(std::move(block));
    }
    return true;
}

void write_pixel_format(BinaryWriter* writer, const PixelFormat& format) {
    writer->integer(format.sample_type);
    writer->integer(format.channels);
    writer->integer(format.alpha);
    writer->integer(format.bits_per_channel);
    writer->boolean(format.little_endian);
}

bool read_pixel_format(BinaryReader* reader, PixelFormat* format) {
    std::uint8_t sample = 0;
    std::uint8_t channels = 0;
    std::uint8_t alpha = 0;
    if (!reader->integer(&sample) ||
        sample > static_cast<std::uint8_t>(SampleType::floating_point) ||
        !reader->integer(&channels) ||
        channels > static_cast<std::uint8_t>(ChannelLayout::indexed) || !reader->integer(&alpha) ||
        alpha > static_cast<std::uint8_t>(AlphaMode::premultiplied) ||
        !reader->integer(&format->bits_per_channel) || !reader->boolean(&format->little_endian))
        return false;
    format->sample_type = static_cast<SampleType>(sample);
    format->channels = static_cast<ChannelLayout>(channels);
    format->alpha = static_cast<AlphaMode>(alpha);
    return format->bytes_per_pixel().has_value();
}

void write_descriptor(BinaryWriter* writer, const DocumentDescriptor& descriptor) {
    writer->integer(descriptor.format);
    writer->integer(descriptor.kind);
    writer->integer(descriptor.canvas_width);
    writer->integer(descriptor.canvas_height);
    writer->integer(descriptor.loop_count);
    write_metadata(writer, descriptor.metadata);
    write_color(writer, descriptor.color);
    writer->integer<std::uint64_t>(descriptor.frames.size());
    for (const RasterFrameDescriptor& frame : descriptor.frames) {
        writer->integer(frame.width);
        writer->integer(frame.height);
        writer->integer(frame.x);
        writer->integer(frame.y);
        writer->integer(frame.duration.count());
        writer->integer(frame.blend);
        writer->integer(frame.disposal);
        write_metadata(writer, frame.metadata);
        write_color(writer, frame.color);
        writer->boolean(frame.cursor_hotspot.has_value());
        if (frame.cursor_hotspot) {
            writer->integer((*frame.cursor_hotspot)[0]);
            writer->integer((*frame.cursor_hotspot)[1]);
        }
        writer->integer(frame.layout.color_model);
        writer->integer(frame.layout.alpha);
        writer->integer(frame.layout.chroma_subsampling);
        writer->integer(frame.layout.color_range);
        writer->integer<std::uint64_t>(frame.layout.planes.size());
        for (const PlaneDescriptor& plane : frame.layout.planes) {
            writer->integer(plane.semantic);
            writer->integer(plane.width);
            writer->integer(plane.height);
            write_pixel_format(writer, plane.format);
            writer->integer(plane.significant_bits);
        }
    }
}

bool read_descriptor(BinaryReader* reader, const RasterStoreLimits& limits,
                     DocumentDescriptor* descriptor) {
    std::uint8_t format = 0;
    std::uint8_t kind = 0;
    std::uint64_t frame_count = 0;
    if (!reader->integer(&format) || format > static_cast<std::uint8_t>(Format::webp) ||
        !reader->integer(&kind) || kind > static_cast<std::uint8_t>(DocumentKind::deep) ||
        !reader->integer(&descriptor->canvas_width) ||
        !reader->integer(&descriptor->canvas_height) || !reader->integer(&descriptor->loop_count) ||
        !read_metadata(reader, &descriptor->metadata) || !read_color(reader, &descriptor->color) ||
        !reader->integer(&frame_count) || frame_count == 0 || frame_count > limits.maximum_frames ||
        frame_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    descriptor->format = static_cast<Format>(format);
    descriptor->kind = static_cast<DocumentKind>(kind);
    descriptor->frames.reserve(static_cast<std::size_t>(frame_count));
    for (std::uint64_t index = 0; index < frame_count; ++index) {
        RasterFrameDescriptor frame;
        std::int64_t duration = 0;
        std::uint8_t blend = 0;
        std::uint8_t disposal = 0;
        bool hotspot = false;
        std::uint8_t color_model = 0;
        std::uint8_t alpha = 0;
        std::uint8_t chroma = 0;
        std::uint8_t range = 0;
        std::uint64_t plane_count = 0;
        if (!reader->integer(&frame.width) || !reader->integer(&frame.height) ||
            !reader->integer(&frame.x) || !reader->integer(&frame.y) ||
            !reader->integer(&duration) || !reader->integer(&blend) ||
            blend > static_cast<std::uint8_t>(FrameBlend::over) || !reader->integer(&disposal) ||
            disposal > static_cast<std::uint8_t>(FrameDisposal::previous) ||
            !read_metadata(reader, &frame.metadata) || !read_color(reader, &frame.color) ||
            !reader->boolean(&hotspot))
            return false;
        frame.duration = std::chrono::nanoseconds(duration);
        frame.blend = static_cast<FrameBlend>(blend);
        frame.disposal = static_cast<FrameDisposal>(disposal);
        if (hotspot) {
            std::array<std::uint32_t, 2> value{};
            if (!reader->integer(value.data()) || !reader->integer(value.data() + 1))
                return false;
            frame.cursor_hotspot = value;
        }
        if (!reader->integer(&color_model) ||
            color_model > static_cast<std::uint8_t>(ColorModel::indexed) ||
            !reader->integer(&alpha) ||
            alpha > static_cast<std::uint8_t>(AlphaMode::premultiplied) ||
            !reader->integer(&chroma) ||
            chroma > static_cast<std::uint8_t>(ChromaSubsampling::yuv441) ||
            !reader->integer(&range) || range > static_cast<std::uint8_t>(ColorRange::limited) ||
            !reader->integer(&plane_count) || plane_count == 0 ||
            plane_count > limits.maximum_planes_per_frame ||
            plane_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return false;
        frame.layout.color_model = static_cast<ColorModel>(color_model);
        frame.layout.alpha = static_cast<AlphaMode>(alpha);
        frame.layout.chroma_subsampling = static_cast<ChromaSubsampling>(chroma);
        frame.layout.color_range = static_cast<ColorRange>(range);
        frame.layout.planes.reserve(static_cast<std::size_t>(plane_count));
        for (std::uint64_t plane_index = 0; plane_index < plane_count; ++plane_index) {
            PlaneDescriptor plane;
            std::uint8_t semantic = 0;
            if (!reader->integer(&semantic) ||
                semantic > static_cast<std::uint8_t>(PlaneSemantic::index) ||
                !reader->integer(&plane.width) || !reader->integer(&plane.height) ||
                !read_pixel_format(reader, &plane.format) ||
                !reader->integer(&plane.significant_bits))
                return false;
            plane.semantic = static_cast<PlaneSemantic>(semantic);
            frame.layout.planes.push_back(plane);
        }
        descriptor->frames.push_back(std::move(frame));
    }
    return descriptor->validate().has_value();
}

struct PlaneStorage final {
    std::uint64_t offset = 0;
    std::uint64_t row_stride = 0;
    std::uint64_t row_bytes = 0;
    std::uint64_t byte_size = 0;
    std::uint32_t frame = 0;
    std::uint32_t plane = 0;
};

void write_manifest(BinaryWriter* writer, const DocumentDescriptor& descriptor,
                    const std::vector<PlaneStorage>& planes, RasterAnalysis analysis) {
    write_descriptor(writer, descriptor);
    writer->integer<std::uint64_t>(planes.size());
    for (const PlaneStorage& plane : planes) {
        writer->integer(plane.frame);
        writer->integer(plane.plane);
        writer->integer(plane.offset);
        writer->integer(plane.row_stride);
        writer->integer(plane.row_bytes);
        writer->integer(plane.byte_size);
    }
    const std::uint8_t alpha =
        analysis.alpha_content
            ? static_cast<std::uint8_t>(static_cast<std::uint8_t>(*analysis.alpha_content) + 1U)
            : 0;
    writer->integer(alpha);
}

bool read_manifest(BinaryReader* reader, const RasterStoreLimits& limits,
                   DocumentDescriptor* descriptor, std::vector<PlaneStorage>* planes,
                   RasterAnalysis* analysis) {
    if (!read_descriptor(reader, limits, descriptor))
        return false;
    std::uint64_t plane_count = 0;
    if (!reader->integer(&plane_count) ||
        plane_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    std::size_t expected = 0;
    for (const auto& frame : descriptor->frames) {
        if (frame.layout.planes.size() > limits.maximum_planes_per_frame ||
            frame.layout.planes.size() > std::numeric_limits<std::size_t>::max() - expected)
            return false;
        expected += frame.layout.planes.size();
    }
    if (plane_count != expected || plane_count > limits.maximum_chunks)
        return false;
    planes->reserve(static_cast<std::size_t>(plane_count));
    std::vector<bool> seen(expected, false);
    for (std::uint64_t index = 0; index < plane_count; ++index) {
        PlaneStorage plane;
        if (!reader->integer(&plane.frame) || !reader->integer(&plane.plane) ||
            !reader->integer(&plane.offset) || !reader->integer(&plane.row_stride) ||
            !reader->integer(&plane.row_bytes) || !reader->integer(&plane.byte_size) ||
            plane.frame >= descriptor->frames.size() ||
            plane.plane >= descriptor->frames[plane.frame].layout.planes.size())
            return false;
        std::size_t flat = plane.plane;
        for (std::uint32_t frame = 0; frame < plane.frame; ++frame) {
            if (descriptor->frames[frame].layout.planes.size() >
                std::numeric_limits<std::size_t>::max() - flat)
                return false;
            flat += descriptor->frames[frame].layout.planes.size();
        }
        if (flat >= seen.size() || seen[flat])
            return false;
        seen[flat] = true;
        const auto& descriptor_plane = descriptor->frames[plane.frame].layout.planes[plane.plane];
        const auto expected_row_bytes = descriptor_plane.row_bytes();
        std::uint64_t expected_byte_size = 0;
        if (!expected_row_bytes || plane.row_bytes != expected_row_bytes.value() ||
            plane.row_stride < plane.row_bytes ||
            !checked_multiply(plane.row_stride, descriptor_plane.height, &expected_byte_size) ||
            plane.byte_size != expected_byte_size)
            return false;
        planes->push_back(plane);
    }
    std::uint8_t alpha = 0;
    if (!reader->integer(&alpha) || alpha > 2 || reader->remaining() != 0)
        return false;
    if (alpha == 1)
        analysis->alpha_content = AlphaContent::opaque;
    else if (alpha == 2)
        analysis->alpha_content = AlphaContent::non_opaque;
    return true;
}

std::array<std::byte, 16> random_nonce() {
    std::array<std::byte, 16> nonce{};
    std::random_device random;
    for (std::byte& byte : nonce)
        byte = static_cast<std::byte>(random() & 0xFFU);
    return nonce;
}

bool all_zero(std::span<const std::byte> bytes) {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::byte value) { return value == std::byte{0}; });
}

bool compatible_analysis(const DocumentDescriptor& descriptor, RasterAnalysis analysis) {
    if (analysis.alpha_content != AlphaContent::non_opaque)
        return true;
    return std::any_of(
        descriptor.frames.begin(), descriptor.frames.end(), [](const RasterFrameDescriptor& frame) {
            return std::any_of(frame.layout.planes.begin(), frame.layout.planes.end(),
                               [](const PlaneDescriptor& plane) {
                                   return plane.semantic == PlaneSemantic::alpha ||
                                          plane.format.alpha != AlphaMode::none;
                               });
        });
}

Result<void> validate_storage_limits(const DocumentDescriptor& descriptor,
                                     const RasterStoreLimits& limits) {
    if (descriptor.frames.size() > limits.maximum_frames)
        return limit("Raster buffer frame count exceeds its configured limit.");
    std::uint64_t plane_count = 0;
    for (const RasterFrameDescriptor& frame : descriptor.frames) {
        if (frame.layout.planes.size() > limits.maximum_planes_per_frame)
            return limit("Raster buffer plane count exceeds its configured limit.");
        if (!checked_add(plane_count, frame.layout.planes.size(), &plane_count) ||
            plane_count > limits.maximum_chunks)
            return limit("Raster buffer plane count exceeds its configured limit.");
    }
    return {};
}

Result<std::uint64_t> maximum_storage_bytes(const RasterStoreLimits& limits) {
    std::uint64_t maximum = 0;
    if (!checked_add(limits.maximum_pixel_bytes, kHeaderBytes, &maximum) ||
        !checked_add(maximum, limits.maximum_manifest_bytes, &maximum))
        return limit("Raster buffer storage limits overflow.");
    return maximum;
}

void put32(std::span<std::byte> destination, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index)
        destination[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
}

void put64(std::span<std::byte> destination, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index)
        destination[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
}

std::uint32_t get32(std::span<const std::byte> source, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(source[offset + index]))
                 << (index * 8U);
    return value;
}

std::uint64_t get64(std::span<const std::byte> source, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(source[offset + index]))
                 << (index * 8U);
    return value;
}

std::array<std::byte, kHeaderBytes> make_header(std::uint32_t state, std::uint64_t total_bytes,
                                                std::uint64_t manifest_bytes,
                                                std::uint32_t manifest_crc,
                                                std::array<std::byte, 16> nonce) {
    std::array<std::byte, kHeaderBytes> header{};
    std::copy(kMagic.begin(), kMagic.end(), header.begin());
    put32(header, 8, kVersion);
    put32(header, 12, state);
    put64(header, 16, total_bytes);
    put64(header, 24, kManifestOffset);
    put64(header, 32, manifest_bytes);
    put32(header, 40, manifest_crc);
    std::copy(nonce.begin(), nonce.end(), header.begin() + 48);
    put32(header, kHeaderCrcOffset, 0);
    put32(header, kHeaderCrcOffset, crc32c(header));
    return header;
}

bool validate_ranges(const DocumentDescriptor& descriptor, const std::vector<PlaneStorage>& planes,
                     std::uint64_t data_start, std::uint64_t total_bytes,
                     const RasterStoreLimits& limits) {
    if (planes.empty() || planes.size() > limits.maximum_chunks)
        return false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    ranges.reserve(planes.size());
    std::uint64_t pixel_bytes = 0;
    for (const PlaneStorage& plane : planes) {
        if (plane.frame >= descriptor.frames.size() ||
            plane.plane >= descriptor.frames[plane.frame].layout.planes.size())
            return false;
        std::uint64_t end = 0;
        if (plane.offset < data_start || plane.byte_size == 0 ||
            !checked_add(plane.offset, plane.byte_size, &end) || end > total_bytes ||
            !checked_add(pixel_bytes, plane.byte_size, &pixel_bytes))
            return false;
        ranges.emplace_back(plane.offset, end);
    }
    if (pixel_bytes > limits.maximum_pixel_bytes)
        return false;
    std::sort(ranges.begin(), ranges.end());
    for (std::size_t index = 1; index < ranges.size(); ++index)
        if (ranges[index - 1].second > ranges[index].first)
            return false;
    return true;
}

std::size_t flat_plane_index(const DocumentDescriptor& descriptor, std::uint32_t frame,
                             std::uint32_t plane) {
    std::size_t index = 0;
    for (std::uint32_t current = 0; current < frame; ++current)
        index += descriptor.frames[current].layout.planes.size();
    return index + plane;
}

} // namespace

class RasterBufferStore::Impl final {
  public:
    std::span<std::byte> writable;
    std::span<const std::byte> readable;
    DocumentDescriptor descriptor;
    std::vector<PlaneStorage> planes;
    std::vector<std::uint8_t> written_rows;
    RasterAnalysis analysis;
    std::array<std::byte, 16> nonce{};
    std::uint64_t byte_size = 0;
    std::uint64_t manifest_bytes = 0;
    std::uint32_t manifest_crc = 0;
    bool writable_state = false;
    bool complete = false;
};

RasterBufferStore::RasterBufferStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RasterBufferStore::~RasterBufferStore() = default;

Result<std::uint64_t> RasterBufferStore::required_bytes(DocumentDescriptor descriptor,
                                                        const RasterBufferStoreOptions& options) {
    if (options.row_alignment == 0 || (options.row_alignment & (options.row_alignment - 1U)) != 0)
        return invalid("Raster buffer row alignment must be a power of two.");
    Result<void> valid = descriptor.validate();
    if (!valid)
        return valid.error();
    valid = validate_storage_limits(descriptor, options.limits);
    if (!valid)
        return valid.error();
    if (!compatible_analysis(descriptor, options.analysis))
        return invalid("Raster analysis contradicts the descriptor alpha layout.");
    try {
        std::vector<PlaneStorage> planes;
        for (std::uint32_t frame_index = 0; frame_index < descriptor.frames.size(); ++frame_index) {
            const auto& frame = descriptor.frames[frame_index];
            for (std::uint32_t plane_index = 0; plane_index < frame.layout.planes.size();
                 ++plane_index) {
                const auto& descriptor_plane = frame.layout.planes[plane_index];
                Result<std::size_t> row_bytes = descriptor_plane.row_bytes();
                if (!row_bytes)
                    return row_bytes.error();
                std::uint64_t row_stride = 0;
                std::uint64_t byte_size = 0;
                if (!align_up(row_bytes.value(), options.row_alignment, &row_stride) ||
                    !checked_multiply(row_stride, descriptor_plane.height, &byte_size))
                    return limit("Raster buffer plane size overflows.");
                planes.push_back(
                    {0, row_stride, row_bytes.value(), byte_size, frame_index, plane_index});
            }
        }
        BinaryWriter writer;
        write_manifest(&writer, descriptor, planes, options.analysis);
        const std::vector<std::byte> manifest = std::move(writer).take();
        if (manifest.empty() || manifest.size() > options.limits.maximum_manifest_bytes)
            return limit("Raster buffer manifest exceeds its configured limit.");
        std::uint64_t total = 0;
        if (!checked_add(kHeaderBytes, manifest.size(), &total) ||
            !align_up(total, options.row_alignment, &total))
            return limit("Raster buffer data offset overflows.");
        std::uint64_t pixel_bytes = 0;
        for (PlaneStorage& plane : planes) {
            if (!align_up(total, options.row_alignment, &plane.offset) ||
                !checked_add(plane.offset, plane.byte_size, &total) ||
                !checked_add(pixel_bytes, plane.byte_size, &pixel_bytes))
                return limit("Raster buffer storage layout overflows.");
        }
        if (pixel_bytes > options.limits.maximum_pixel_bytes)
            return limit("Raster buffer pixels exceed their configured limit.");
        return total;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not calculate the raster buffer layout.",
                             "raster buffer store");
    }
}

Result<std::shared_ptr<RasterBufferStore>>
RasterBufferStore::create(std::span<std::byte> storage, DocumentDescriptor descriptor,
                          const RasterBufferStoreOptions& options) {
    if (!storage.data() || storage.size() < kHeaderBytes)
        return limit("The raster buffer is too small for its header.");
    if (options.row_alignment == 0 || (options.row_alignment & (options.row_alignment - 1U)) != 0)
        return invalid("Raster buffer row alignment must be a power of two.");
    Result<void> valid = descriptor.validate();
    if (!valid)
        return valid.error();
    valid = validate_storage_limits(descriptor, options.limits);
    if (!valid)
        return valid.error();
    if (!compatible_analysis(descriptor, options.analysis))
        return invalid("Raster analysis contradicts the descriptor alpha layout.");

    std::vector<PlaneStorage> planes;
    try {
        for (std::uint32_t frame_index = 0; frame_index < descriptor.frames.size(); ++frame_index) {
            const auto& frame = descriptor.frames[frame_index];
            for (std::uint32_t plane_index = 0; plane_index < frame.layout.planes.size();
                 ++plane_index) {
                const auto& descriptor_plane = frame.layout.planes[plane_index];
                Result<std::size_t> row_bytes = descriptor_plane.row_bytes();
                if (!row_bytes)
                    return row_bytes.error();
                std::uint64_t row_stride = 0;
                if (!align_up(row_bytes.value(), options.row_alignment, &row_stride))
                    return limit("Raster buffer row stride overflows.");
                std::uint64_t byte_size = 0;
                if (!checked_multiply(row_stride, descriptor_plane.height, &byte_size))
                    return limit("Raster buffer plane size overflows.");
                planes.push_back(
                    {0, row_stride, row_bytes.value(), byte_size, frame_index, plane_index});
            }
        }
        BinaryWriter initial;
        write_manifest(&initial, descriptor, planes, options.analysis);
        std::vector<std::byte> manifest = std::move(initial).take();
        if (manifest.size() > options.limits.maximum_manifest_bytes)
            return limit("Raster buffer manifest exceeds its configured limit.");
        std::uint64_t data_offset = 0;
        if (!checked_add(kHeaderBytes, manifest.size(), &data_offset) ||
            !align_up(data_offset, options.row_alignment, &data_offset))
            return limit("Raster buffer data offset overflows.");
        std::uint64_t total_bytes = data_offset;
        for (PlaneStorage& plane : planes) {
            if (!align_up(total_bytes, options.row_alignment, &plane.offset) ||
                !checked_add(plane.offset, plane.byte_size, &total_bytes))
                return limit("Raster buffer storage layout overflows.");
        }
        const auto maximum = maximum_storage_bytes(options.limits);
        if (!maximum)
            return maximum.error();
        if (total_bytes != storage.size() || total_bytes > maximum.value())
            return limit("The supplied raster buffer has an invalid size.");
        if (!validate_ranges(descriptor, planes, data_offset, total_bytes, options.limits))
            return limit("Raster buffer plane ranges are invalid.");

        BinaryWriter final_manifest_writer;
        write_manifest(&final_manifest_writer, descriptor, planes, options.analysis);
        manifest = std::move(final_manifest_writer).take();
        if (manifest.size() > options.limits.maximum_manifest_bytes)
            return limit("Raster buffer manifest exceeds its configured limit.");
        const std::uint32_t manifest_crc = crc32c(manifest);
        const auto nonce = all_zero(options.session_nonce) ? random_nonce() : options.session_nonce;
        const auto header =
            make_header(kWriting, total_bytes, manifest.size(), manifest_crc, nonce);
        std::copy(header.begin(), header.end(), storage.begin());
        std::copy(manifest.begin(), manifest.end(),
                  storage.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes));
        for (const PlaneStorage& plane : planes) {
            std::fill(storage.begin() + static_cast<std::ptrdiff_t>(plane.offset),
                      storage.begin() + static_cast<std::ptrdiff_t>(plane.offset + plane.byte_size),
                      std::byte{0});
        }

        auto impl = std::make_unique<Impl>();
        impl->writable = storage;
        impl->readable = storage;
        impl->descriptor = std::move(descriptor);
        impl->planes = std::move(planes);
        impl->analysis = options.analysis;
        impl->nonce = nonce;
        impl->byte_size = total_bytes;
        impl->manifest_bytes = manifest.size();
        impl->manifest_crc = manifest_crc;
        impl->writable_state = true;
        std::size_t row_count = 0;
        for (const auto& plane : impl->descriptor.frames)
            for (const auto& descriptor_plane : plane.layout.planes)
                row_count += descriptor_plane.height;
        impl->written_rows.assign(row_count, 0);
        return std::shared_ptr<RasterBufferStore>(new RasterBufferStore(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate the raster buffer manifest.",
                             "raster buffer store");
    }
}

Result<std::shared_ptr<RasterBufferStore>>
RasterBufferStore::open(std::span<const std::byte> storage, const RasterStoreLimits& limits,
                        std::optional<std::array<std::byte, 16>> expected_nonce,
                        std::optional<std::uint64_t> expected_size) {
    if (!storage.data() || storage.size() < kHeaderBytes)
        return corrupt("The raster buffer does not contain a complete header.");
    if (!std::equal(kMagic.begin(), kMagic.end(), storage.begin()) ||
        get32(storage, 8) != kVersion || get32(storage, 12) != kSealed)
        return corrupt("The raster buffer header is unsupported or unsealed.");
    const std::uint32_t saved_header_crc = get32(storage, kHeaderCrcOffset);
    std::array<std::byte, kHeaderBytes> header{};
    std::copy_n(storage.begin(), kHeaderBytes, header.begin());
    put32(header, kHeaderCrcOffset, 0);
    if (crc32c(header) != saved_header_crc)
        return corrupt("The raster buffer header checksum does not match.");
    const std::uint64_t total_bytes = get64(storage, 16);
    const std::uint64_t manifest_offset = get64(storage, 24);
    const std::uint64_t manifest_bytes = get64(storage, 32);
    const std::uint32_t manifest_crc = get32(storage, 40);
    // Native shared-memory allocations may be rounded up by the operating
    // system. When the physical segment size is authenticated by the caller,
    // allow trailing allocation padding while keeping package ranges bounded
    // by the sealed logical size.
    if (total_bytes > storage.size() || (expected_size && *expected_size != storage.size()) ||
        (!expected_size && total_bytes != storage.size()) || manifest_offset != kManifestOffset ||
        manifest_bytes == 0 || manifest_bytes > limits.maximum_manifest_bytes ||
        manifest_offset > total_bytes || manifest_bytes > total_bytes - manifest_offset)
        return corrupt("The raster buffer header size fields are invalid.");
    const auto maximum = maximum_storage_bytes(limits);
    if (!maximum || total_bytes > maximum.value())
        return corrupt("The raster buffer exceeds its configured storage limit.");
    std::array<std::byte, 16> nonce{};
    std::copy_n(storage.begin() + 48, nonce.size(), nonce.begin());
    if (all_zero(nonce) || (expected_nonce && *expected_nonce != nonce))
        return corrupt("The raster buffer session nonce is invalid.");
    const auto manifest = storage.subspan(static_cast<std::size_t>(manifest_offset),
                                          static_cast<std::size_t>(manifest_bytes));
    if (crc32c(manifest) != manifest_crc)
        return corrupt("The raster buffer manifest checksum does not match.");

    try {
        DocumentDescriptor descriptor;
        std::vector<PlaneStorage> planes;
        RasterAnalysis analysis;
        BinaryReader reader(manifest);
        if (!read_manifest(&reader, limits, &descriptor, &planes, &analysis))
            return corrupt("The raster buffer manifest is malformed.");
        std::uint64_t data_start = 0;
        if (!checked_add(manifest_offset, manifest_bytes, &data_start) ||
            !validate_ranges(descriptor, planes, data_start, total_bytes, limits))
            return corrupt("The raster buffer plane table is invalid.");
        const bool alpha_capable =
            std::any_of(planes.begin(), planes.end(), [&](const PlaneStorage& plane) {
                const PlaneDescriptor& descriptor_plane =
                    descriptor.frames[plane.frame].layout.planes[plane.plane];
                return descriptor_plane.semantic == PlaneSemantic::alpha ||
                       descriptor_plane.format.alpha != AlphaMode::none;
            });
        if (analysis.alpha_content == AlphaContent::non_opaque && !alpha_capable)
            return corrupt("The raster buffer has contradictory alpha metadata.");

        auto impl = std::make_unique<Impl>();
        impl->readable = storage.first(static_cast<std::size_t>(total_bytes));
        impl->descriptor = std::move(descriptor);
        impl->planes = std::move(planes);
        impl->analysis = analysis;
        impl->nonce = nonce;
        impl->byte_size = total_bytes;
        impl->manifest_bytes = manifest_bytes;
        impl->manifest_crc = manifest_crc;
        impl->complete = true;
        return std::shared_ptr<RasterBufferStore>(new RasterBufferStore(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate the raster buffer descriptor.",
                             "raster buffer store");
    }
}

const DocumentDescriptor& RasterBufferStore::descriptor() const noexcept {
    return impl_->descriptor;
}

RasterAccess RasterBufferStore::access() const noexcept {
    return RasterAccess::sequential_rows | RasterAccess::random_rows |
           RasterAccess::random_regions | RasterAccess::mapped_planes |
           RasterAccess::concurrent_reads;
}

Result<void> RasterBufferStore::read_rows(std::uint32_t frame_index, std::uint32_t plane_index,
                                          std::uint32_t first_row, std::uint32_t row_count,
                                          std::size_t destination_stride,
                                          std::span<std::byte> destination,
                                          std::stop_token stop) const {
    if (!impl_->complete)
        return invalid("The raster buffer is not sealed.");
    if (frame_index >= impl_->descriptor.frames.size() || row_count == 0 ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("The raster buffer row request is invalid.");
    const auto& plane =
        impl_->planes[flat_plane_index(impl_->descriptor, frame_index, plane_index)];
    const auto& descriptor_plane = impl_->descriptor.frames[frame_index].layout.planes[plane_index];
    if (first_row > descriptor_plane.height || row_count > descriptor_plane.height - first_row ||
        destination_stride < plane.row_bytes ||
        (row_count > 1 &&
         destination_stride >
             (std::numeric_limits<std::size_t>::max() - plane.row_bytes) / (row_count - 1U)) ||
        destination.size() < destination_stride * (row_count - 1U) + plane.row_bytes)
        return invalid("The raster buffer destination is too small.");
    const std::byte* source = impl_->readable.data() + plane.offset +
                              static_cast<std::size_t>(first_row) * plane.row_stride;
    for (std::uint32_t row = 0; row < row_count; ++row) {
        if (stop.stop_requested())
            return cancelled();
        std::memcpy(destination.data() + static_cast<std::size_t>(row) * destination_stride,
                    source + static_cast<std::size_t>(row) * plane.row_stride,
                    static_cast<std::size_t>(plane.row_bytes));
    }
    return {};
}

Result<void> RasterBufferStore::read_region(std::uint32_t frame_index, std::uint32_t plane_index,
                                            RasterRect region, MutablePlaneView destination,
                                            std::stop_token stop) const {
    return RasterSource::read_region(frame_index, plane_index, region, destination, stop);
}

Result<MappedPlane> RasterBufferStore::map_plane(std::uint32_t frame_index,
                                                 std::uint32_t plane_index) const {
    if (!impl_->complete || frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("The raster buffer mapping request is invalid.");
    const PlaneStorage& plane =
        impl_->planes[flat_plane_index(impl_->descriptor, frame_index, plane_index)];
    auto owner = std::shared_ptr<const void>(shared_from_this(), static_cast<const void*>(this));
    return MappedPlane{std::move(owner),
                       impl_->readable.subspan(static_cast<std::size_t>(plane.offset),
                                               static_cast<std::size_t>(plane.byte_size)),
                       static_cast<std::size_t>(plane.row_stride)};
}

Result<MutableMappedPlane> RasterBufferStore::map_plane_for_write(std::uint32_t frame_index,
                                                                  std::uint32_t plane_index) {
    if (!impl_->writable_state || impl_->complete || impl_->writable.empty() ||
        frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("The raster buffer is not writable.");
    const PlaneStorage& plane =
        impl_->planes[flat_plane_index(impl_->descriptor, frame_index, plane_index)];
    auto owner = std::shared_ptr<void>(shared_from_this(), static_cast<void*>(this));
    return MutableMappedPlane{std::move(owner),
                              impl_->writable.subspan(static_cast<std::size_t>(plane.offset),
                                                      static_cast<std::size_t>(plane.byte_size)),
                              static_cast<std::size_t>(plane.row_stride)};
}

Result<void> RasterBufferStore::finish_mapped_plane(std::uint32_t frame_index,
                                                    std::uint32_t plane_index,
                                                    const MutableMappedPlane& mapping) {
    if (!impl_->writable_state || impl_->complete || !mapping.owner ||
        frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("The raster buffer mapping cannot be finalized.");
    const auto& descriptor_plane = impl_->descriptor.frames[frame_index].layout.planes[plane_index];
    const PlaneStorage& plane =
        impl_->planes[flat_plane_index(impl_->descriptor, frame_index, plane_index)];
    if (mapping.pixels.data() != impl_->writable.data() + plane.offset ||
        mapping.pixels.size() != plane.byte_size || mapping.row_stride != plane.row_stride)
        return invalid("The raster buffer mapping does not belong to this plane.");
    std::size_t row_base = 0;
    for (std::size_t index = 0;
         index < flat_plane_index(impl_->descriptor, frame_index, plane_index); ++index) {
        const auto& prior = impl_->planes[index];
        row_base += impl_->descriptor.frames[prior.frame].layout.planes[prior.plane].height;
    }
    std::fill(impl_->written_rows.begin() + static_cast<std::ptrdiff_t>(row_base),
              impl_->written_rows.begin() +
                  static_cast<std::ptrdiff_t>(row_base + descriptor_plane.height),
              std::uint8_t{1});
    return {};
}

Result<void> RasterBufferStore::write_rows(std::uint32_t frame_index, std::uint32_t plane_index,
                                           std::uint32_t first_row, std::uint32_t row_count,
                                           std::size_t source_stride,
                                           std::span<const std::byte> source,
                                           std::stop_token stop) {
    if (!impl_->writable_state || impl_->complete ||
        frame_index >= impl_->descriptor.frames.size() || row_count == 0 ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("The raster buffer row write is invalid.");
    const auto& descriptor_plane = impl_->descriptor.frames[frame_index].layout.planes[plane_index];
    const PlaneStorage& plane =
        impl_->planes[flat_plane_index(impl_->descriptor, frame_index, plane_index)];
    if (first_row > descriptor_plane.height || row_count > descriptor_plane.height - first_row ||
        source_stride < plane.row_bytes ||
        (row_count > 1 &&
         source_stride >
             (std::numeric_limits<std::size_t>::max() - plane.row_bytes) / (row_count - 1U)) ||
        source.size() < source_stride * (row_count - 1U) + plane.row_bytes)
        return invalid("The raster buffer source rows are too small.");
    std::byte* destination = impl_->writable.data() + plane.offset +
                             static_cast<std::size_t>(first_row) * plane.row_stride;
    std::size_t row_base = 0;
    const std::size_t flat = flat_plane_index(impl_->descriptor, frame_index, plane_index);
    for (std::size_t index = 0; index < flat; ++index) {
        const auto& prior = impl_->planes[index];
        row_base += impl_->descriptor.frames[prior.frame].layout.planes[prior.plane].height;
    }
    for (std::uint32_t row = 0; row < row_count; ++row) {
        if (stop.stop_requested())
            return cancelled();
        std::memcpy(destination + static_cast<std::size_t>(row) * plane.row_stride,
                    source.data() + static_cast<std::size_t>(row) * source_stride,
                    static_cast<std::size_t>(plane.row_bytes));
        impl_->written_rows[row_base + first_row + row] = 1;
    }
    return {};
}

Result<void> RasterBufferStore::commit() {
    if (!impl_->writable_state || impl_->complete)
        return invalid("The raster buffer cannot be sealed in its current state.");
    if (std::any_of(impl_->written_rows.begin(), impl_->written_rows.end(),
                    [](std::uint8_t written) { return written == 0; }))
        return invalid("The raster buffer cannot be sealed with unwritten rows.");
    const auto header = make_header(kSealed, impl_->byte_size, impl_->manifest_bytes,
                                    impl_->manifest_crc, impl_->nonce);
    std::copy(header.begin(), header.end(), impl_->writable.begin());
    impl_->readable = impl_->writable;
    impl_->writable_state = false;
    impl_->complete = true;
    return {};
}

void RasterBufferStore::abort() noexcept {
    if (!impl_)
        return;
    impl_->writable_state = false;
    impl_->complete = false;
}

bool RasterBufferStore::complete() const noexcept {
    return impl_->complete;
}

std::uint64_t RasterBufferStore::byte_size() const noexcept {
    return impl_->byte_size;
}

std::array<std::byte, 16> RasterBufferStore::session_nonce() const noexcept {
    return impl_->nonce;
}

RasterAnalysis RasterBufferStore::analysis() const noexcept {
    return impl_->analysis;
}

} // namespace snow::image
