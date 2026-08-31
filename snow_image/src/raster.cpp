#include "snow/image/raster.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <random>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace snow::image {
namespace {

constexpr std::array<std::byte, 8> kMagic{std::byte{'S'}, std::byte{'N'}, std::byte{'O'},
                                          std::byte{'W'}, std::byte{'R'}, std::byte{'S'},
                                          std::byte{0}, std::byte{0}};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kIncomplete = 0;
constexpr std::uint32_t kComplete = 1;
constexpr std::uint64_t kHeaderBytes = 64;
constexpr std::uint64_t kDataAlignment = std::uint64_t{64} << 10U;

Status invalid(std::string message) {
    return Status::error(ErrorCode::invalid_argument, std::move(message), "raster store");
}

Status corrupt(std::string message) {
    return Status::error(ErrorCode::corrupt_data, std::move(message), "raster store");
}

Status io_error(std::string message) {
    return Status::error(ErrorCode::io_error, std::move(message), "raster store");
}

Status limit(std::string message) {
    return Status::error(ErrorCode::limit_exceeded, std::move(message), "raster store");
}

Status cancelled() {
    return Status::error(ErrorCode::cancelled, "Raster operation was cancelled.", "raster store");
}

Result<void> validate_color(const ColorEncoding& color) {
    if (color.primaries > ColorPrimaries::custom || color.transfer > TransferFunction::hlg ||
        color.dynamic_range > DynamicRange::high || !std::isfinite(color.source_peak_nits) ||
        !std::isfinite(color.diffuse_white_nits))
        return invalid("Raster color metadata is invalid.");
    if (color.mastering_primaries_and_white &&
        !std::all_of(color.mastering_primaries_and_white->begin(),
                     color.mastering_primaries_and_white->end(),
                     [](float value) { return std::isfinite(value); }))
        return invalid("Raster mastering-display metadata is invalid.");
    return {};
}

Result<void> validate_metadata(const Metadata& metadata) {
    if (metadata.orientation < Orientation::identity ||
        metadata.orientation > Orientation::rotate_270)
        return invalid("Raster orientation metadata is invalid.");
    const auto valid_dpi = [](const std::optional<double>& dpi) {
        return !dpi || (std::isfinite(*dpi) && *dpi > 0.0);
    };
    if (!valid_dpi(metadata.horizontal_dpi) || !valid_dpi(metadata.vertical_dpi))
        return invalid("Raster resolution metadata is invalid.");
    return {};
}

bool checked_add(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    *result = left + right;
    return true;
}

bool checked_multiply(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    *result = left * right;
    return true;
}

bool align_up(std::uint64_t value, std::uint64_t alignment, std::uint64_t* result) {
    if (alignment == 0 || !result)
        return false;
    const std::uint64_t remainder = value % alignment;
    if (remainder == 0) {
        *result = value;
        return true;
    }
    return checked_add(value, alignment - remainder, result);
}

constexpr std::array<std::array<std::uint32_t, 256>, 8> make_crc32c_tables() {
    std::array<std::array<std::uint32_t, 256>, 8> tables{};
    for (std::uint32_t value = 0; value < 256; ++value) {
        std::uint32_t crc = value;
        for (unsigned bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0x82F63B78U & mask);
        }
        tables[0][value] = crc;
    }
    for (std::size_t table = 1; table < tables.size(); ++table) {
        for (std::size_t value = 0; value < tables[table].size(); ++value) {
            const std::uint32_t previous = tables[table - 1U][value];
            tables[table][value] = tables[0][previous & 0xFFU] ^ (previous >> 8U);
        }
    }
    return tables;
}

constexpr auto kCrc32cTables = make_crc32c_tables();

#if defined(_MSC_VER) && defined(_M_X64)
bool hardware_crc32c_available() {
    static const bool available = [] {
        int registers[4]{};
        __cpuid(registers, 1);
        return (registers[2] & (1 << 20)) != 0;
    }();
    return available;
}

std::uint32_t hardware_crc32c_update(std::uint32_t state, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    std::uint64_t crc = state;
    while (bytes.size() - offset >= sizeof(std::uint64_t)) {
        std::uint64_t block = 0;
        std::memcpy(&block, bytes.data() + offset, sizeof(block));
        crc = _mm_crc32_u64(crc, block);
        offset += sizeof(block);
    }
    state = static_cast<std::uint32_t>(crc);
    while (offset < bytes.size()) {
        state = _mm_crc32_u8(state, std::to_integer<std::uint8_t>(bytes[offset]));
        ++offset;
    }
    return state;
}
#endif

std::uint32_t crc32c_update(std::uint32_t state, std::span<const std::byte> bytes) {
#if defined(_MSC_VER) && defined(_M_X64)
    if (hardware_crc32c_available())
        return hardware_crc32c_update(state, bytes);
#endif
    std::size_t offset = 0;
    while (bytes.size() - offset >= 8) {
        std::uint64_t block = 0;
        for (unsigned index = 0; index < 8; ++index) {
            block |=
                static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index]))
                << (index * 8U);
        }
        block ^= state;
        state =
            kCrc32cTables[7][block & 0xFFU] ^ kCrc32cTables[6][(block >> 8U) & 0xFFU] ^
            kCrc32cTables[5][(block >> 16U) & 0xFFU] ^ kCrc32cTables[4][(block >> 24U) & 0xFFU] ^
            kCrc32cTables[3][(block >> 32U) & 0xFFU] ^ kCrc32cTables[2][(block >> 40U) & 0xFFU] ^
            kCrc32cTables[1][(block >> 48U) & 0xFFU] ^ kCrc32cTables[0][(block >> 56U) & 0xFFU];
        offset += 8;
    }
    while (offset < bytes.size()) {
        state = kCrc32cTables[0][(state ^ std::to_integer<std::uint8_t>(bytes[offset])) & 0xFFU] ^
                (state >> 8U);
        ++offset;
    }
    return state;
}

std::uint32_t crc32c(std::span<const std::byte> bytes) {
    return crc32c_update(0xFFFFFFFFU, bytes) ^ 0xFFFFFFFFU;
}

class BinaryWriter final {
  public:
    template <typename T> void integer(T value) {
        using U = std::make_unsigned_t<T>;
        U unsigned_value = static_cast<U>(value);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            bytes_.push_back(static_cast<std::byte>(unsigned_value & U{0xFF}));
            unsigned_value >>= 8U;
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
        integer<std::uint64_t>(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }

    void string(const std::string& value) {
        bytes(std::as_bytes(std::span(value.data(), value.size())));
    }

    std::vector<std::byte> take() && {
        return std::move(bytes_);
    }

  private:
    std::vector<std::byte> bytes_;
};

class BinaryReader final {
  public:
    explicit BinaryReader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename T> bool integer(T* value) {
        if (!value || remaining() < sizeof(T))
            return false;
        using U = std::make_unsigned_t<T>;
        std::uint64_t result = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            result |=
                static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + index]))
                << (index * 8U);
        }
        offset_ += sizeof(T);
        *value = static_cast<T>(static_cast<U>(result));
        return true;
    }

    bool boolean(bool* value) {
        std::uint8_t encoded = 0;
        if (!integer(&encoded) || encoded > 1)
            return false;
        *value = encoded != 0;
        return true;
    }

    bool floating(float* value) {
        std::uint32_t encoded = 0;
        if (!integer(&encoded))
            return false;
        *value = std::bit_cast<float>(encoded);
        return true;
    }

    bool floating(double* value) {
        std::uint64_t encoded = 0;
        if (!integer(&encoded))
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
        if (!bytes(&encoded))
            return false;
        value->assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
        return true;
    }

    [[nodiscard]] std::size_t remaining() const {
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
        std::array<float, 8> mastering{};
        for (float& value : mastering) {
            if (!reader->floating(&value))
                return false;
        }
        color->mastering_primaries_and_white = mastering;
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
    return true;
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
        double value = 0;
        if (!reader->floating(&value))
            return false;
        metadata->horizontal_dpi = value;
    }
    if (!reader->boolean(&present))
        return false;
    if (present) {
        double value = 0;
        if (!reader->floating(&value))
            return false;
        metadata->vertical_dpi = value;
    }
    std::uint64_t blocks = 0;
    if (!reader->string(&metadata->comment) || !reader->bytes(&metadata->exif) ||
        !reader->bytes(&metadata->xmp) || !reader->integer(&blocks) || blocks > reader->remaining())
        return false;
    metadata->blocks.reserve(static_cast<std::size_t>(blocks));
    for (std::uint64_t index = 0; index < blocks; ++index) {
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
    std::uint64_t frames = 0;
    if (!reader->integer(&format) || format > static_cast<std::uint8_t>(Format::webp) ||
        !reader->integer(&kind) || kind > static_cast<std::uint8_t>(DocumentKind::deep) ||
        !reader->integer(&descriptor->canvas_width) ||
        !reader->integer(&descriptor->canvas_height) || !reader->integer(&descriptor->loop_count) ||
        !read_metadata(reader, &descriptor->metadata) || !read_color(reader, &descriptor->color) ||
        !reader->integer(&frames) || frames > limits.maximum_frames)
        return false;
    descriptor->format = static_cast<Format>(format);
    descriptor->kind = static_cast<DocumentKind>(kind);
    descriptor->frames.reserve(static_cast<std::size_t>(frames));
    for (std::uint64_t frame_index = 0; frame_index < frames; ++frame_index) {
        RasterFrameDescriptor frame;
        std::int64_t duration = 0;
        std::uint8_t blend = 0;
        std::uint8_t disposal = 0;
        bool hotspot = false;
        std::uint8_t color_model = 0;
        std::uint8_t alpha = 0;
        std::uint8_t chroma = 0;
        std::uint8_t range = 0;
        std::uint64_t planes = 0;
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
            !reader->integer(&planes) || planes == 0 || planes > limits.maximum_planes_per_frame)
            return false;
        frame.layout.color_model = static_cast<ColorModel>(color_model);
        frame.layout.alpha = static_cast<AlphaMode>(alpha);
        frame.layout.chroma_subsampling = static_cast<ChromaSubsampling>(chroma);
        frame.layout.color_range = static_cast<ColorRange>(range);
        frame.layout.planes.reserve(static_cast<std::size_t>(planes));
        for (std::uint64_t plane_index = 0; plane_index < planes; ++plane_index) {
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
    std::uint64_t first_chunk = 0;
    std::uint64_t chunk_count = 0;
};

struct ChunkStorage final {
    std::uint32_t frame = 0;
    std::uint32_t plane = 0;
    std::uint32_t first_row = 0;
    std::uint32_t row_count = 0;
    std::uint64_t offset = 0;
    std::uint64_t logical_bytes = 0;
    std::uint32_t crc = 0;
    std::uint32_t crc_state = 0xFFFFFFFFU;
    std::uint32_t next_row = 0;
    bool needs_recompute = false;
};

void write_storage(BinaryWriter* writer, const std::vector<PlaneStorage>& planes,
                   const std::vector<ChunkStorage>& chunks) {
    writer->integer<std::uint64_t>(planes.size());
    for (const PlaneStorage& plane : planes) {
        writer->integer(plane.offset);
        writer->integer(plane.row_stride);
        writer->integer(plane.row_bytes);
        writer->integer(plane.byte_size);
        writer->integer(plane.first_chunk);
        writer->integer(plane.chunk_count);
    }
    writer->integer<std::uint64_t>(chunks.size());
    for (const ChunkStorage& chunk : chunks) {
        writer->integer(chunk.frame);
        writer->integer(chunk.plane);
        writer->integer(chunk.first_row);
        writer->integer(chunk.row_count);
        writer->integer(chunk.offset);
        writer->integer(chunk.logical_bytes);
        writer->integer(chunk.crc);
    }
}

bool read_storage(BinaryReader* reader, const DocumentDescriptor& descriptor,
                  const RasterStoreLimits& limits, std::uint64_t file_bytes,
                  std::vector<PlaneStorage>* planes, std::vector<ChunkStorage>* chunks) {
    std::uint64_t plane_count = 0;
    std::uint64_t expected_planes = 0;
    for (const RasterFrameDescriptor& frame : descriptor.frames)
        expected_planes += frame.layout.planes.size();
    if (!reader->integer(&plane_count) || plane_count != expected_planes ||
        plane_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    planes->reserve(static_cast<std::size_t>(plane_count));
    for (std::uint64_t index = 0; index < plane_count; ++index) {
        PlaneStorage plane;
        std::uint64_t end = 0;
        if (!reader->integer(&plane.offset) || !reader->integer(&plane.row_stride) ||
            !reader->integer(&plane.row_bytes) || !reader->integer(&plane.byte_size) ||
            !reader->integer(&plane.first_chunk) || !reader->integer(&plane.chunk_count) ||
            plane.row_bytes == 0 || plane.row_stride < plane.row_bytes ||
            !checked_add(plane.offset, plane.byte_size, &end) || end > file_bytes)
            return false;
        planes->push_back(plane);
    }
    std::uint64_t chunk_count = 0;
    if (!reader->integer(&chunk_count) || chunk_count > limits.maximum_chunks ||
        chunk_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    chunks->reserve(static_cast<std::size_t>(chunk_count));
    for (std::uint64_t index = 0; index < chunk_count; ++index) {
        ChunkStorage chunk;
        std::uint64_t end = 0;
        if (!reader->integer(&chunk.frame) || !reader->integer(&chunk.plane) ||
            !reader->integer(&chunk.first_row) || !reader->integer(&chunk.row_count) ||
            !reader->integer(&chunk.offset) || !reader->integer(&chunk.logical_bytes) ||
            !reader->integer(&chunk.crc) || chunk.row_count == 0 ||
            !checked_add(chunk.offset, chunk.logical_bytes, &end) || end > file_bytes)
            return false;
        chunks->push_back(chunk);
    }
    for (const PlaneStorage& plane : *planes) {
        std::uint64_t end = 0;
        if (!checked_add(plane.first_chunk, plane.chunk_count, &end) || end > chunks->size())
            return false;
    }
    return reader->remaining() == 0;
}

std::vector<std::byte> make_manifest(const DocumentDescriptor& descriptor,
                                     const std::vector<PlaneStorage>& planes,
                                     const std::vector<ChunkStorage>& chunks) {
    BinaryWriter writer;
    write_descriptor(&writer, descriptor);
    write_storage(&writer, planes, chunks);
    return std::move(writer).take();
}

std::array<std::byte, kHeaderBytes> make_header(std::uint32_t state, std::uint64_t manifest_bytes,
                                                std::uint64_t file_bytes,
                                                std::uint32_t manifest_crc,
                                                std::array<std::byte, 16> nonce,
                                                RasterAnalysis analysis = {}) {
    std::array<std::byte, kHeaderBytes> header{};
    std::memcpy(header.data(), kMagic.data(), kMagic.size());
    auto put32 = [&header](std::size_t offset, std::uint32_t value) {
        for (std::size_t index = 0; index < 4; ++index)
            header[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    };
    auto put64 = [&header](std::size_t offset, std::uint64_t value) {
        for (std::size_t index = 0; index < 8; ++index)
            header[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    };
    put32(8, kVersion);
    put32(12, state);
    put64(16, manifest_bytes);
    put64(24, file_bytes);
    put32(32, manifest_crc);
    std::copy(nonce.begin(), nonce.end(), header.begin() + 40);
    if (analysis.alpha_content)
        header[56] =
            static_cast<std::byte>(static_cast<std::uint8_t>(*analysis.alpha_content) + 1U);
    put32(36, crc32c(header));
    return header;
}

std::uint32_t header_u32(const std::array<std::byte, kHeaderBytes>& header, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(header[offset + index]))
                 << (index * 8U);
    return value;
}

std::uint64_t header_u64(const std::array<std::byte, kHeaderBytes>& header, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(header[offset + index]))
                 << (index * 8U);
    return value;
}

std::array<std::byte, 16> random_nonce() {
    std::array<std::byte, 16> result{};
    std::random_device random;
    for (std::byte& value : result)
        value = static_cast<std::byte>(random() & 0xFFU);
    return result;
}

bool all_zero(std::span<const std::byte> value) {
    return std::all_of(value.begin(), value.end(),
                       [](std::byte byte) { return byte == std::byte{0}; });
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

std::string path_message(const std::filesystem::path& path, std::string_view message) {
    return std::string(message) + ": " + path.string();
}

struct PlaneMapping final {
#if defined(_WIN32)
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE mapping = nullptr;
#else
    int file = -1;
#endif
    void* base = nullptr;
    std::size_t mapped_bytes = 0;

    ~PlaneMapping() {
#if defined(_WIN32)
        if (base)
            UnmapViewOfFile(base);
        if (mapping)
            CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
#else
        if (base && mapped_bytes != 0)
            munmap(base, mapped_bytes);
        if (file >= 0)
            close(file);
#endif
    }
};

} // namespace

Result<std::size_t> PlaneDescriptor::row_bytes() const {
    const Result<std::size_t> bytes_per_pixel = format.bytes_per_pixel();
    if (!bytes_per_pixel)
        return bytes_per_pixel.error();
    if (width != 0 && bytes_per_pixel.value() > std::numeric_limits<std::size_t>::max() / width)
        return limit("Plane row byte count overflows the address space.");
    return static_cast<std::size_t>(width) * bytes_per_pixel.value();
}

Result<void> PlaneDescriptor::validate() const {
    if (width == 0 || height == 0)
        return invalid("Raster planes must have non-zero dimensions.");
    const Result<std::size_t> bytes = row_bytes();
    if (!bytes || bytes.value() == 0)
        return bytes ? invalid("Raster row is empty.") : bytes.error();
    if (significant_bits == 0 || significant_bits > format.bits_per_channel)
        return invalid("Plane significant bits exceed its storage format.");
    return {};
}

Result<void> RasterLayout::validate() const {
    if (planes.empty())
        return invalid("Raster layouts require at least one plane.");
    if (planes.size() > 16)
        return limit("Raster layout has too many planes.");
    for (const PlaneDescriptor& plane : planes) {
        Result<void> status = plane.validate();
        if (!status)
            return status;
    }
    if (color_model != ColorModel::ycbcr && chroma_subsampling != ChromaSubsampling::none)
        return invalid("Chroma subsampling is only valid for YCbCr layouts.");
    return {};
}

Result<void> RasterFrameDescriptor::validate(std::uint32_t canvas_width,
                                             std::uint32_t canvas_height) const {
    if (width == 0 || height == 0 || x > canvas_width || y > canvas_height ||
        width > canvas_width - x || height > canvas_height - y)
        return invalid("Raster frame lies outside its document canvas.");
    Result<void> status = layout.validate();
    if (!status)
        return status;
    status = validate_metadata(metadata);
    if (!status)
        return status;
    status = validate_color(color);
    if (!status)
        return status;
    const PlaneDescriptor& primary = layout.planes.front();
    const bool padded_ycbcr = layout.color_model == ColorModel::ycbcr &&
                              primary.semantic == PlaneSemantic::luma && primary.width >= width &&
                              primary.height >= height;
    if (!padded_ycbcr && (primary.width != width || primary.height != height))
        return invalid("Raster frame primary plane dimensions do not match the frame.");
    return {};
}

Result<void> DocumentDescriptor::validate() const {
    if (kind != DocumentKind::raster)
        return Status::error(ErrorCode::unsupported_feature,
                             "This raster interface requires a raster document.");
    if (canvas_width == 0 || canvas_height == 0 || frames.empty())
        return invalid("Raster document is empty.");
    Result<void> status = validate_metadata(metadata);
    if (!status)
        return status;
    status = validate_color(color);
    if (!status)
        return status;
    for (const RasterFrameDescriptor& frame : frames) {
        status = frame.validate(canvas_width, canvas_height);
        if (!status)
            return status;
    }
    return {};
}

Result<void> MutablePlaneView::validate() const {
    PlaneDescriptor descriptor{PlaneSemantic::packed, width, height, format,
                               format.bits_per_channel};
    Result<std::size_t> row_bytes = descriptor.row_bytes();
    if (!row_bytes)
        return row_bytes.error();
    if (row_stride < row_bytes.value())
        return invalid("Plane view stride is too small.");
    std::uint64_t required = row_bytes.value();
    if (height > 1 &&
        (!checked_multiply(row_stride, static_cast<std::uint64_t>(height - 1), &required) ||
         !checked_add(required, row_bytes.value(), &required) || required > pixels.size()))
        return invalid("Plane view storage is too small.");
    return {};
}

Result<void> RasterSource::read_region(std::uint32_t frame_index, std::uint32_t plane_index,
                                       RasterRect region, MutablePlaneView destination,
                                       std::stop_token stop) const {
    if (frame_index >= descriptor().frames.size() ||
        plane_index >= descriptor().frames[frame_index].layout.planes.size())
        return invalid("Raster region references an unknown frame or plane.");
    const PlaneDescriptor& plane = descriptor().frames[frame_index].layout.planes[plane_index];
    if (region.width == 0 || region.height == 0 || region.x > plane.width ||
        region.y > plane.height || region.width > plane.width - region.x ||
        region.height > plane.height - region.y || destination.width != region.width ||
        destination.height != region.height || destination.format != plane.format)
        return invalid("Raster region or destination layout is invalid.");
    Result<void> destination_status = destination.validate();
    if (!destination_status)
        return destination_status;
    Result<std::size_t> row_bytes = plane.row_bytes();
    Result<std::size_t> bytes_per_pixel = plane.format.bytes_per_pixel();
    if (!row_bytes || !bytes_per_pixel)
        return row_bytes ? bytes_per_pixel.error() : row_bytes.error();
    try {
        std::vector<std::byte> row(row_bytes.value());
        const std::size_t copy_bytes =
            static_cast<std::size_t>(region.width) * bytes_per_pixel.value();
        const std::size_t source_offset =
            static_cast<std::size_t>(region.x) * bytes_per_pixel.value();
        for (std::uint32_t y = 0; y < region.height; ++y) {
            if (stop.stop_requested())
                return cancelled();
            Result<void> status =
                read_rows(frame_index, plane_index, region.y + y, 1, row_bytes.value(), row, stop);
            if (!status)
                return status;
            std::memcpy(destination.pixels.data() +
                            static_cast<std::size_t>(y) * destination.row_stride,
                        row.data() + source_offset, copy_bytes);
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate a raster row buffer.",
                             "raster store");
    }
}

Result<MappedPlane> RasterSource::map_plane(std::uint32_t, std::uint32_t) const {
    return Status::error(ErrorCode::unsupported_feature,
                         "This raster source does not expose mapped planes.", "raster store");
}

Result<MutableMappedPlane> RasterWriter::map_plane_for_write(std::uint32_t, std::uint32_t) {
    return Status::error(ErrorCode::unsupported_feature,
                         "This raster writer does not expose writable plane mappings.",
                         "raster store");
}

Result<void> RasterWriter::finish_mapped_plane(std::uint32_t, std::uint32_t,
                                               const MutableMappedPlane&) {
    return Status::error(ErrorCode::unsupported_feature,
                         "This raster writer does not expose writable plane mappings.",
                         "raster store");
}

class RasterStore::Impl final {
  public:
    std::filesystem::path path;
    DocumentDescriptor descriptor;
    RasterStoreOptions options;
    std::array<std::byte, 16> nonce{};
    std::vector<PlaneStorage> planes;
    std::vector<ChunkStorage> chunks;
    mutable std::vector<bool> verified_chunks;
    std::vector<std::vector<bool>> written_rows;
    mutable std::fstream file;
    mutable std::mutex mutex;
    std::uint64_t file_bytes = 0;
    std::uint64_t manifest_bytes = 0;
    RasterAnalysis analysis;
    bool complete = false;
    bool writable = false;
    bool analysis_writable = false;
    bool aborted = false;

    [[nodiscard]] std::size_t flat_plane(std::uint32_t frame, std::uint32_t plane) const {
        std::size_t result = plane;
        for (std::uint32_t index = 0; index < frame; ++index)
            result += descriptor.frames[index].layout.planes.size();
        return result;
    }

    Result<void> write_header_and_manifest(std::uint32_t state) {
        std::vector<std::byte> manifest = make_manifest(descriptor, planes, chunks);
        if (manifest.size() != manifest_bytes)
            return Status::error(ErrorCode::internal_error,
                                 "Raster manifest changed size during commit.", "raster store");
        const auto header =
            make_header(state, manifest.size(), file_bytes, crc32c(manifest), nonce, analysis);
        file.clear();
        file.seekp(0, std::ios::beg);
        file.write(reinterpret_cast<const char*>(header.data()),
                   static_cast<std::streamsize>(header.size()));
        file.write(reinterpret_cast<const char*>(manifest.data()),
                   static_cast<std::streamsize>(manifest.size()));
        file.flush();
        if (!file)
            return io_error(path_message(path, "Could not commit raster manifest"));
        return {};
    }

    Result<void> verify_chunk_locked(std::size_t chunk_index) const {
        if (chunk_index >= chunks.size())
            return corrupt("Raster chunk index is invalid.");
        if (verified_chunks[chunk_index])
            return {};
        const ChunkStorage& chunk = chunks[chunk_index];
        const PlaneStorage& storage = planes[flat_plane(chunk.frame, chunk.plane)];
        try {
            std::vector<std::byte> row(static_cast<std::size_t>(storage.row_bytes));
            std::uint32_t state = 0xFFFFFFFFU;
            for (std::uint32_t index = 0; index < chunk.row_count; ++index) {
                const std::uint64_t offset =
                    storage.offset +
                    static_cast<std::uint64_t>(chunk.first_row + index) * storage.row_stride;
                file.clear();
                file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                file.read(reinterpret_cast<char*>(row.data()),
                          static_cast<std::streamsize>(row.size()));
                if (!file)
                    return io_error("Could not verify raster store chunk data.");
                state = crc32c_update(state, row);
            }
            if ((state ^ 0xFFFFFFFFU) != chunk.crc)
                return corrupt("Raster store chunk checksum does not match.");
            verified_chunks[chunk_index] = true;
            return {};
        } catch (const std::bad_alloc&) {
            return Status::error(ErrorCode::out_of_memory,
                                 "Could not allocate a raster verification row.", "raster store");
        }
    }
};

RasterStore::RasterStore(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

RasterStore::~RasterStore() {
    if (impl_ && impl_->writable && !impl_->complete && !impl_->aborted)
        abort();
}

Result<std::shared_ptr<RasterStore>> RasterStore::create(const std::filesystem::path& path,
                                                         DocumentDescriptor descriptor,
                                                         const RasterStoreOptions& options) {
    Result<void> descriptor_status = descriptor.validate();
    if (!descriptor_status)
        return descriptor_status.error();
    if (options.chunk_rows == 0 || options.row_alignment == 0 ||
        (options.row_alignment & (options.row_alignment - 1U)) != 0)
        return invalid("Raster store chunk rows and power-of-two alignment must be non-zero.");
    if (descriptor.frames.size() > options.limits.maximum_frames)
        return limit("Raster store frame count exceeds its configured limit.");
    if (!compatible_analysis(descriptor, options.analysis))
        return invalid("Raster analysis contradicts the descriptor alpha layout.");
    try {
        auto impl = std::make_unique<Impl>();
        impl->path = path;
        impl->descriptor = std::move(descriptor);
        impl->options = options;
        impl->analysis = options.analysis;
        impl->nonce = all_zero(options.session_nonce) ? random_nonce() : options.session_nonce;
        std::uint64_t chunk_count = 0;
        for (std::uint32_t frame_index = 0; frame_index < impl->descriptor.frames.size();
             ++frame_index) {
            const RasterFrameDescriptor& frame = impl->descriptor.frames[frame_index];
            if (frame.layout.planes.size() > options.limits.maximum_planes_per_frame)
                return limit("Raster store plane count exceeds its configured limit.");
            for (std::uint32_t plane_index = 0; plane_index < frame.layout.planes.size();
                 ++plane_index) {
                const PlaneDescriptor& descriptor_plane = frame.layout.planes[plane_index];
                Result<std::size_t> row_bytes_result = descriptor_plane.row_bytes();
                if (!row_bytes_result)
                    return row_bytes_result.error();
                const std::uint64_t row_bytes = row_bytes_result.value();
                std::uint64_t row_stride = 0;
                if (!align_up(row_bytes, options.row_alignment, &row_stride))
                    return limit("Raster row stride overflows.");
                std::uint64_t byte_size = 0;
                if (!checked_multiply(row_stride, descriptor_plane.height, &byte_size) ||
                    byte_size > options.limits.maximum_pixel_bytes)
                    return limit("Raster plane exceeds its configured byte limit.");
                const std::uint64_t plane_chunks =
                    (static_cast<std::uint64_t>(descriptor_plane.height) + options.chunk_rows -
                     1U) /
                    options.chunk_rows;
                if (!checked_add(chunk_count, plane_chunks, &chunk_count) ||
                    chunk_count > options.limits.maximum_chunks)
                    return limit("Raster store chunk count exceeds its configured limit.");
                PlaneStorage storage;
                storage.row_stride = row_stride;
                storage.row_bytes = row_bytes;
                storage.byte_size = byte_size;
                storage.first_chunk = impl->chunks.size();
                storage.chunk_count = plane_chunks;
                impl->planes.push_back(storage);
                impl->written_rows.emplace_back(descriptor_plane.height, false);
                for (std::uint32_t first = 0; first < descriptor_plane.height;
                     first += options.chunk_rows) {
                    ChunkStorage chunk;
                    chunk.frame = frame_index;
                    chunk.plane = plane_index;
                    chunk.first_row = first;
                    chunk.row_count = std::min(options.chunk_rows, descriptor_plane.height - first);
                    chunk.next_row = first;
                    if (!checked_multiply(row_bytes, chunk.row_count, &chunk.logical_bytes))
                        return limit("Raster chunk byte count overflows.");
                    impl->chunks.push_back(chunk);
                }
            }
        }
        std::vector<std::byte> provisional =
            make_manifest(impl->descriptor, impl->planes, impl->chunks);
        if (provisional.empty() || provisional.size() > options.limits.maximum_manifest_bytes)
            return limit("Raster store manifest exceeds its configured limit.");
        impl->manifest_bytes = provisional.size();
        std::uint64_t next_offset = 0;
        if (!checked_add(kHeaderBytes, impl->manifest_bytes, &next_offset) ||
            !align_up(next_offset, kDataAlignment, &next_offset))
            return limit("Raster store data offset overflows.");
        for (PlaneStorage& plane : impl->planes) {
            plane.offset = next_offset;
            if (!checked_add(next_offset, plane.byte_size, &next_offset) ||
                !align_up(next_offset, kDataAlignment, &next_offset))
                return limit("Raster store file size overflows.");
            for (std::uint64_t index = 0; index < plane.chunk_count; ++index) {
                ChunkStorage& chunk =
                    impl->chunks[static_cast<std::size_t>(plane.first_chunk + index)];
                chunk.offset =
                    plane.offset + static_cast<std::uint64_t>(chunk.first_row) * plane.row_stride;
            }
        }
        impl->file_bytes = next_offset;
        impl->verified_chunks.assign(impl->chunks.size(), false);
        if (impl->file_bytes > options.limits.maximum_pixel_bytes +
                                   options.limits.maximum_manifest_bytes + kDataAlignment)
            return limit("Raster store file exceeds its configured limit.");
        std::error_code filesystem_error;
        if (!path.parent_path().empty())
            std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error)
            return io_error(path_message(path, "Could not create raster store directory"));
        impl->file.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
        if (!impl->file)
            return io_error(path_message(path, "Could not create raster store"));
        impl->file.seekp(static_cast<std::streamoff>(impl->file_bytes - 1U), std::ios::beg);
        impl->file.put('\0');
        if (!impl->file)
            return io_error(path_message(path, "Could not size raster store"));
        impl->writable = true;
        impl->analysis_writable = true;
        Result<void> header_status = impl->write_header_and_manifest(kIncomplete);
        if (!header_status)
            return header_status.error();
        return std::shared_ptr<RasterStore>(new RasterStore(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate raster store state.",
                             "raster store");
    }
}

Result<std::shared_ptr<RasterStore>> RasterStore::open(const std::filesystem::path& path,
                                                       const RasterStoreLimits& limits) {
    try {
        auto impl = std::make_unique<Impl>();
        impl->path = path;
        impl->options.limits = limits;
        impl->file.open(path, std::ios::binary | std::ios::in);
        if (!impl->file)
            return io_error(path_message(path, "Could not open raster store"));
        impl->file.seekg(0, std::ios::end);
        const std::streamoff size = impl->file.tellg();
        if (size < static_cast<std::streamoff>(kHeaderBytes))
            return corrupt("Raster store is truncated.");
        impl->file_bytes = static_cast<std::uint64_t>(size);
        std::array<std::byte, kHeaderBytes> header{};
        impl->file.seekg(0, std::ios::beg);
        impl->file.read(reinterpret_cast<char*>(header.data()),
                        static_cast<std::streamsize>(header.size()));
        if (!impl->file || !std::equal(kMagic.begin(), kMagic.end(), header.begin()) ||
            header_u32(header, 8) != kVersion || header_u32(header, 12) != kComplete)
            return corrupt("Raster store header is incomplete or unsupported.");
        const std::uint32_t expected_header_crc = header_u32(header, 36);
        auto checksum_header = header;
        std::fill(checksum_header.begin() + 36, checksum_header.begin() + 40, std::byte{0});
        if (crc32c(checksum_header) != expected_header_crc)
            return corrupt("Raster store header checksum does not match.");
        impl->manifest_bytes = header_u64(header, 16);
        const std::uint64_t declared_file_bytes = header_u64(header, 24);
        if (impl->manifest_bytes == 0 || impl->manifest_bytes > limits.maximum_manifest_bytes ||
            declared_file_bytes != impl->file_bytes ||
            impl->manifest_bytes > impl->file_bytes - kHeaderBytes ||
            impl->manifest_bytes >
                static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return corrupt("Raster store sizes are invalid.");
        std::copy(header.begin() + 40, header.begin() + 56, impl->nonce.begin());
        const std::uint8_t analysis_value = std::to_integer<std::uint8_t>(header[56]);
        if (analysis_value > 2)
            return corrupt("Raster store alpha analysis metadata is invalid.");
        if (analysis_value == 1)
            impl->analysis.alpha_content = AlphaContent::opaque;
        else if (analysis_value == 2)
            impl->analysis.alpha_content = AlphaContent::non_opaque;
        std::vector<std::byte> manifest(static_cast<std::size_t>(impl->manifest_bytes));
        impl->file.read(reinterpret_cast<char*>(manifest.data()),
                        static_cast<std::streamsize>(manifest.size()));
        if (!impl->file || crc32c(manifest) != header_u32(header, 32))
            return corrupt("Raster store manifest checksum does not match.");
        BinaryReader reader(manifest);
        if (!read_descriptor(&reader, limits, &impl->descriptor) ||
            !read_storage(&reader, impl->descriptor, limits, impl->file_bytes, &impl->planes,
                          &impl->chunks))
            return corrupt("Raster store manifest is invalid.");
        if (!compatible_analysis(impl->descriptor, impl->analysis))
            return corrupt("Raster store alpha analysis contradicts its descriptor.");
        impl->verified_chunks.assign(impl->chunks.size(), false);
        impl->complete = true;
        impl->analysis_writable = false;
        return std::shared_ptr<RasterStore>(new RasterStore(std::move(impl)));
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate raster store manifest.",
                             "raster store");
    }
}

const DocumentDescriptor& RasterStore::descriptor() const noexcept {
    return impl_->descriptor;
}

RasterAccess RasterStore::access() const noexcept {
    return RasterAccess::sequential_rows | RasterAccess::random_rows |
           RasterAccess::random_regions | RasterAccess::mapped_planes |
           RasterAccess::concurrent_reads;
}

Result<void> RasterStore::read_rows(std::uint32_t frame_index, std::uint32_t plane_index,
                                    std::uint32_t first_row, std::uint32_t row_count,
                                    std::size_t destination_stride,
                                    std::span<std::byte> destination, std::stop_token stop) const {
    if (!impl_->complete)
        return invalid("Raster store is not committed.");
    if (frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("Raster read references an unknown frame or plane.");
    const PlaneDescriptor& descriptor_plane =
        impl_->descriptor.frames[frame_index].layout.planes[plane_index];
    if (row_count == 0 || first_row > descriptor_plane.height ||
        row_count > descriptor_plane.height - first_row)
        return invalid("Raster row range is invalid.");
    const PlaneStorage& storage = impl_->planes[impl_->flat_plane(frame_index, plane_index)];
    if (destination_stride < storage.row_bytes)
        return invalid("Raster destination stride is too small.");
    std::uint64_t required = storage.row_bytes;
    if (row_count > 1 &&
        (!checked_multiply(destination_stride, row_count - 1U, &required) ||
         !checked_add(required, storage.row_bytes, &required) || required > destination.size()))
        return invalid("Raster destination storage is too small.");
    std::lock_guard lock(impl_->mutex);
    const std::uint32_t requested_end = first_row + row_count;
    for (std::uint64_t relative = 0; relative < storage.chunk_count; ++relative) {
        const std::size_t chunk_index = static_cast<std::size_t>(storage.first_chunk + relative);
        const ChunkStorage& chunk = impl_->chunks[chunk_index];
        if (chunk.first_row >= requested_end || chunk.first_row + chunk.row_count <= first_row)
            continue;
        Result<void> verified = impl_->verify_chunk_locked(chunk_index);
        if (!verified)
            return verified;
    }
    for (std::uint32_t row = 0; row < row_count; ++row) {
        if (stop.stop_requested())
            return cancelled();
        const std::uint64_t offset =
            storage.offset + static_cast<std::uint64_t>(first_row + row) * storage.row_stride;
        impl_->file.clear();
        impl_->file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        impl_->file.read(
            reinterpret_cast<char*>(destination.data() +
                                    static_cast<std::size_t>(row) * destination_stride),
            static_cast<std::streamsize>(storage.row_bytes));
        if (!impl_->file)
            return io_error("Could not read raster store rows.");
    }
    return {};
}

Result<void> RasterStore::read_region(std::uint32_t frame_index, std::uint32_t plane_index,
                                      RasterRect region, MutablePlaneView destination,
                                      std::stop_token stop) const {
    if (!impl_->complete)
        return invalid("Raster store is not committed.");
    if (frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("Raster region references an unknown frame or plane.");
    const PlaneDescriptor& plane = impl_->descriptor.frames[frame_index].layout.planes[plane_index];
    if (region.width == 0 || region.height == 0 || region.x > plane.width ||
        region.y > plane.height || region.width > plane.width - region.x ||
        region.height > plane.height - region.y || destination.width != region.width ||
        destination.height != region.height || destination.format != plane.format)
        return invalid("Raster region or destination layout is invalid.");
    Result<void> destination_status = destination.validate();
    if (!destination_status)
        return destination_status;
    Result<std::size_t> bytes_per_pixel = plane.format.bytes_per_pixel();
    if (!bytes_per_pixel)
        return bytes_per_pixel.error();

    const std::size_t flat_plane = impl_->flat_plane(frame_index, plane_index);
    const PlaneStorage& storage = impl_->planes[flat_plane];
    const std::uint64_t copy_bytes =
        static_cast<std::uint64_t>(region.width) * bytes_per_pixel.value();
    const std::uint64_t horizontal_offset =
        static_cast<std::uint64_t>(region.x) * bytes_per_pixel.value();
    const std::uint32_t requested_end = region.y + region.height;
    std::lock_guard lock(impl_->mutex);
    for (std::uint64_t relative = 0; relative < storage.chunk_count; ++relative) {
        const std::size_t chunk_index = static_cast<std::size_t>(storage.first_chunk + relative);
        const ChunkStorage& chunk = impl_->chunks[chunk_index];
        if (chunk.first_row >= requested_end || chunk.first_row + chunk.row_count <= region.y)
            continue;
        Result<void> verified = impl_->verify_chunk_locked(chunk_index);
        if (!verified)
            return verified;
    }
    for (std::uint32_t row = 0; row < region.height; ++row) {
        if (stop.stop_requested())
            return cancelled();
        const std::uint64_t offset =
            storage.offset + static_cast<std::uint64_t>(region.y + row) * storage.row_stride +
            horizontal_offset;
        impl_->file.clear();
        impl_->file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        impl_->file.read(
            reinterpret_cast<char*>(destination.pixels.data() +
                                    static_cast<std::size_t>(row) * destination.row_stride),
            static_cast<std::streamsize>(copy_bytes));
        if (!impl_->file)
            return io_error("Could not read a raster store region.");
    }
    return {};
}

Result<MappedPlane> RasterStore::map_plane(std::uint32_t frame_index,
                                           std::uint32_t plane_index) const {
    if (!impl_->complete)
        return invalid("Raster store is not committed.");
    if (frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("Raster mapping references an unknown frame or plane.");
    const PlaneStorage& storage = impl_->planes[impl_->flat_plane(frame_index, plane_index)];
    if (storage.byte_size > std::numeric_limits<std::size_t>::max())
        return limit("Raster plane mapping exceeds the address space.");
    try {
        auto owner = std::make_shared<PlaneMapping>();
#if defined(_WIN32)
        SYSTEM_INFO system_info{};
        GetSystemInfo(&system_info);
        const std::uint64_t granularity = system_info.dwAllocationGranularity;
        const std::uint64_t aligned_offset = storage.offset - storage.offset % granularity;
        const std::uint64_t delta = storage.offset - aligned_offset;
        std::uint64_t mapped_bytes = 0;
        if (!checked_add(delta, storage.byte_size, &mapped_bytes) ||
            mapped_bytes > std::numeric_limits<SIZE_T>::max())
            return limit("Raster plane mapping length overflows.");
        owner->file = CreateFileW(impl_->path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (owner->file == INVALID_HANDLE_VALUE)
            return io_error(path_message(impl_->path, "Could not open raster mapping"));
        owner->mapping = CreateFileMappingW(owner->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!owner->mapping)
            return io_error(path_message(impl_->path, "Could not create raster mapping"));
        owner->base = MapViewOfFile(
            owner->mapping, FILE_MAP_READ, static_cast<DWORD>(aligned_offset >> 32U),
            static_cast<DWORD>(aligned_offset & 0xFFFFFFFFU), static_cast<SIZE_T>(mapped_bytes));
        if (!owner->base)
            return io_error(path_message(impl_->path, "Could not map raster plane"));
        owner->mapped_bytes = static_cast<std::size_t>(mapped_bytes);
#else
        const long page_size = sysconf(_SC_PAGE_SIZE);
        if (page_size <= 0)
            return io_error("Could not query the mapping page size.");
        const std::uint64_t granularity = static_cast<std::uint64_t>(page_size);
        const std::uint64_t aligned_offset = storage.offset - storage.offset % granularity;
        const std::uint64_t delta = storage.offset - aligned_offset;
        std::uint64_t mapped_bytes = 0;
        if (!checked_add(delta, storage.byte_size, &mapped_bytes) ||
            mapped_bytes > std::numeric_limits<std::size_t>::max())
            return limit("Raster plane mapping length overflows.");
        owner->file = open(impl_->path.c_str(), O_RDONLY);
        if (owner->file < 0)
            return io_error(path_message(impl_->path, "Could not open raster mapping"));
        owner->base = mmap(nullptr, static_cast<std::size_t>(mapped_bytes), PROT_READ, MAP_SHARED,
                           owner->file, static_cast<off_t>(aligned_offset));
        if (owner->base == MAP_FAILED) {
            owner->base = nullptr;
            return io_error(path_message(impl_->path, "Could not map raster plane"));
        }
        owner->mapped_bytes = static_cast<std::size_t>(mapped_bytes);
#endif
        const auto* pixels = reinterpret_cast<const std::byte*>(owner->base) +
                             static_cast<std::size_t>(storage.offset - aligned_offset);
        {
            std::lock_guard lock(impl_->mutex);
            for (std::uint64_t relative = 0; relative < storage.chunk_count; ++relative) {
                const std::size_t chunk_index =
                    static_cast<std::size_t>(storage.first_chunk + relative);
                if (impl_->verified_chunks[chunk_index])
                    continue;
                const ChunkStorage& chunk = impl_->chunks[chunk_index];
                std::uint32_t state = 0xFFFFFFFFU;
                for (std::uint32_t row = 0; row < chunk.row_count; ++row) {
                    const std::size_t absolute_row = chunk.first_row + row;
                    state = crc32c_update(state, std::span<const std::byte>(
                                                     pixels + absolute_row * storage.row_stride,
                                                     static_cast<std::size_t>(storage.row_bytes)));
                }
                if ((state ^ 0xFFFFFFFFU) != chunk.crc)
                    return corrupt("Raster store mapped-plane checksum does not match.");
                impl_->verified_chunks[chunk_index] = true;
            }
        }
        return MappedPlane{
            std::static_pointer_cast<const void>(owner),
            std::span<const std::byte>(pixels, static_cast<std::size_t>(storage.byte_size)),
            static_cast<std::size_t>(storage.row_stride)};
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate raster mapping state.",
                             "raster store");
    }
}

Result<MutableMappedPlane> RasterStore::map_plane_for_write(std::uint32_t frame_index,
                                                            std::uint32_t plane_index) {
    if (!impl_->writable || impl_->complete || impl_->aborted)
        return invalid("Raster store is not writable.");
    if (frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("Raster write mapping references an unknown frame or plane.");
    const PlaneStorage& storage = impl_->planes[impl_->flat_plane(frame_index, plane_index)];
    if (storage.byte_size > std::numeric_limits<std::size_t>::max())
        return limit("Raster write mapping exceeds the address space.");
    try {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->file.flush();
            if (!impl_->file)
                return io_error("Could not flush the raster store before mapping.");
        }
        auto owner = std::make_shared<PlaneMapping>();
#if defined(_WIN32)
        SYSTEM_INFO system_info{};
        GetSystemInfo(&system_info);
        const std::uint64_t granularity = system_info.dwAllocationGranularity;
        const std::uint64_t aligned_offset = storage.offset - storage.offset % granularity;
        const std::uint64_t delta = storage.offset - aligned_offset;
        std::uint64_t mapped_bytes = 0;
        if (!checked_add(delta, storage.byte_size, &mapped_bytes) ||
            mapped_bytes > std::numeric_limits<SIZE_T>::max())
            return limit("Raster write mapping length overflows.");
        owner->file = CreateFileW(impl_->path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (owner->file == INVALID_HANDLE_VALUE)
            return io_error(path_message(impl_->path, "Could not open raster write mapping"));
        owner->mapping = CreateFileMappingW(owner->file, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        if (!owner->mapping)
            return io_error(path_message(impl_->path, "Could not create raster write mapping"));
        owner->base = MapViewOfFile(owner->mapping, FILE_MAP_READ | FILE_MAP_WRITE,
                                    static_cast<DWORD>(aligned_offset >> 32U),
                                    static_cast<DWORD>(aligned_offset & 0xFFFFFFFFU),
                                    static_cast<SIZE_T>(mapped_bytes));
        if (!owner->base)
            return io_error(path_message(impl_->path, "Could not map writable raster plane"));
        owner->mapped_bytes = static_cast<std::size_t>(mapped_bytes);
#else
        const long page_size = sysconf(_SC_PAGE_SIZE);
        if (page_size <= 0)
            return io_error("Could not query the mapping page size.");
        const std::uint64_t granularity = static_cast<std::uint64_t>(page_size);
        const std::uint64_t aligned_offset = storage.offset - storage.offset % granularity;
        const std::uint64_t delta = storage.offset - aligned_offset;
        std::uint64_t mapped_bytes = 0;
        if (!checked_add(delta, storage.byte_size, &mapped_bytes) ||
            mapped_bytes > std::numeric_limits<std::size_t>::max())
            return limit("Raster write mapping length overflows.");
        owner->file = open(impl_->path.c_str(), O_RDWR);
        if (owner->file < 0)
            return io_error(path_message(impl_->path, "Could not open raster write mapping"));
        owner->base = mmap(nullptr, static_cast<std::size_t>(mapped_bytes), PROT_READ | PROT_WRITE,
                           MAP_SHARED, owner->file, static_cast<off_t>(aligned_offset));
        if (owner->base == MAP_FAILED) {
            owner->base = nullptr;
            return io_error(path_message(impl_->path, "Could not map writable raster plane"));
        }
        owner->mapped_bytes = static_cast<std::size_t>(mapped_bytes);
#endif
        auto* pixels = reinterpret_cast<std::byte*>(owner->base) +
                       static_cast<std::size_t>(storage.offset - aligned_offset);
        return MutableMappedPlane{
            std::static_pointer_cast<void>(owner),
            std::span<std::byte>(pixels, static_cast<std::size_t>(storage.byte_size)),
            static_cast<std::size_t>(storage.row_stride)};
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate raster write mapping state.", "raster store");
    }
}

Result<void> RasterStore::finish_mapped_plane(std::uint32_t frame_index, std::uint32_t plane_index,
                                              const MutableMappedPlane& mapping) {
    if (!impl_->writable || impl_->complete || impl_->aborted)
        return invalid("Raster store is not writable.");
    if (frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("Raster write mapping references an unknown frame or plane.");
    const std::size_t flat_plane = impl_->flat_plane(frame_index, plane_index);
    const PlaneStorage& storage = impl_->planes[flat_plane];
    if (!mapping.owner || mapping.row_stride != storage.row_stride ||
        mapping.pixels.size() != storage.byte_size)
        return invalid("Raster write mapping does not match its plane.");
    std::lock_guard lock(impl_->mutex);
    std::fill(impl_->written_rows[flat_plane].begin(), impl_->written_rows[flat_plane].end(), true);
    for (std::uint64_t index = 0; index < storage.chunk_count; ++index) {
        ChunkStorage& chunk = impl_->chunks[static_cast<std::size_t>(storage.first_chunk + index)];
        chunk.crc_state = 0xFFFFFFFFU;
        for (std::uint32_t row = 0; row < chunk.row_count; ++row) {
            const std::size_t absolute_row = chunk.first_row + row;
            const std::span<const std::byte> source_row = mapping.pixels.subspan(
                absolute_row * mapping.row_stride, static_cast<std::size_t>(storage.row_bytes));
            chunk.crc_state = crc32c_update(chunk.crc_state, source_row);
        }
        chunk.next_row = chunk.first_row + chunk.row_count;
        chunk.needs_recompute = false;
    }
    return {};
}

Result<void> RasterStore::copy_plane(std::uint32_t frame_index, std::uint32_t plane_index,
                                     std::size_t source_stride, std::span<const std::byte> source,
                                     std::stop_token stop) {
    if (!impl_->writable || impl_->complete || impl_->aborted)
        return invalid("Raster store is not writable.");
    if (frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("Raster copy references an unknown frame or plane.");
    const PlaneDescriptor& descriptor_plane =
        impl_->descriptor.frames[frame_index].layout.planes[plane_index];
    const std::size_t flat_plane = impl_->flat_plane(frame_index, plane_index);
    const PlaneStorage& storage = impl_->planes[flat_plane];
    if (source_stride < storage.row_bytes)
        return invalid("Raster copy source stride is too small.");
    std::uint64_t required = storage.row_bytes;
    if (descriptor_plane.height > 1 &&
        (!checked_multiply(source_stride, descriptor_plane.height - 1U, &required) ||
         !checked_add(required, storage.row_bytes, &required) || required > source.size()))
        return invalid("Raster copy source storage is too small.");
    if (stop.stop_requested())
        return cancelled();

    std::lock_guard lock(impl_->mutex);
    impl_->file.clear();
    impl_->file.seekp(static_cast<std::streamoff>(storage.offset), std::ios::beg);
    if (!impl_->file)
        return io_error("Could not seek to the raster copy destination.");
    for (std::uint64_t index = 0; index < storage.chunk_count; ++index) {
        if (stop.stop_requested())
            return cancelled();
        ChunkStorage& chunk = impl_->chunks[static_cast<std::size_t>(storage.first_chunk + index)];
        std::uint32_t crc_state = 0xFFFFFFFFU;
        if (source_stride == storage.row_bytes && storage.row_stride == storage.row_bytes) {
            const std::span<const std::byte> chunk_source =
                source.subspan(static_cast<std::size_t>(chunk.first_row) * source_stride,
                               static_cast<std::size_t>(chunk.logical_bytes));
            if (chunk_source.size() >
                static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
                return limit("Raster copy chunk exceeds the stream write limit.");
            crc_state = crc32c_update(crc_state, chunk_source);
            impl_->file.write(reinterpret_cast<const char*>(chunk_source.data()),
                              static_cast<std::streamsize>(chunk_source.size()));
            if (!impl_->file)
                return io_error("Could not copy a contiguous raster chunk.");
        } else {
            for (std::uint32_t row = 0; row < chunk.row_count; ++row) {
                if (stop.stop_requested())
                    return cancelled();
                const std::size_t absolute_row = chunk.first_row + row;
                const std::span<const std::byte> source_row = source.subspan(
                    absolute_row * source_stride, static_cast<std::size_t>(storage.row_bytes));
                const std::uint64_t offset =
                    storage.offset + static_cast<std::uint64_t>(absolute_row) * storage.row_stride;
                impl_->file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
                impl_->file.write(reinterpret_cast<const char*>(source_row.data()),
                                  static_cast<std::streamsize>(source_row.size()));
                if (!impl_->file)
                    return io_error("Could not copy a raster row.");
                crc_state = crc32c_update(crc_state, source_row);
            }
        }
        chunk.crc_state = crc_state;
        chunk.next_row = chunk.first_row + chunk.row_count;
        chunk.needs_recompute = false;
    }
    std::fill(impl_->written_rows[flat_plane].begin(), impl_->written_rows[flat_plane].end(), true);
    return {};
}

Result<void> RasterStore::write_rows(std::uint32_t frame_index, std::uint32_t plane_index,
                                     std::uint32_t first_row, std::uint32_t row_count,
                                     std::size_t source_stride, std::span<const std::byte> source,
                                     std::stop_token stop) {
    if (!impl_->writable || impl_->complete || impl_->aborted)
        return invalid("Raster store is not writable.");
    if (frame_index >= impl_->descriptor.frames.size() ||
        plane_index >= impl_->descriptor.frames[frame_index].layout.planes.size())
        return invalid("Raster write references an unknown frame or plane.");
    const PlaneDescriptor& descriptor_plane =
        impl_->descriptor.frames[frame_index].layout.planes[plane_index];
    if (row_count == 0 || first_row > descriptor_plane.height ||
        row_count > descriptor_plane.height - first_row)
        return invalid("Raster row range is invalid.");
    const std::size_t flat_plane = impl_->flat_plane(frame_index, plane_index);
    const PlaneStorage& storage = impl_->planes[flat_plane];
    if (source_stride < storage.row_bytes)
        return invalid("Raster source stride is too small.");
    std::uint64_t required = storage.row_bytes;
    if (row_count > 1 &&
        (!checked_multiply(source_stride, row_count - 1U, &required) ||
         !checked_add(required, storage.row_bytes, &required) || required > source.size()))
        return invalid("Raster source storage is too small.");
    std::lock_guard lock(impl_->mutex);
    for (std::uint32_t row = 0; row < row_count; ++row) {
        if (stop.stop_requested())
            return cancelled();
        const std::uint32_t absolute_row = first_row + row;
        const std::span<const std::byte> source_row =
            source.subspan(static_cast<std::size_t>(row) * source_stride,
                           static_cast<std::size_t>(storage.row_bytes));
        const std::uint64_t offset =
            storage.offset + static_cast<std::uint64_t>(absolute_row) * storage.row_stride;
        impl_->file.clear();
        impl_->file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
        impl_->file.write(reinterpret_cast<const char*>(source_row.data()),
                          static_cast<std::streamsize>(source_row.size()));
        if (!impl_->file)
            return io_error("Could not write raster store rows.");
        impl_->written_rows[flat_plane][absolute_row] = true;
        const std::uint64_t relative_chunk = absolute_row / impl_->options.chunk_rows;
        ChunkStorage& chunk =
            impl_->chunks[static_cast<std::size_t>(storage.first_chunk + relative_chunk)];
        if (!chunk.needs_recompute && absolute_row == chunk.next_row) {
            chunk.crc_state = crc32c_update(chunk.crc_state, source_row);
            ++chunk.next_row;
        } else {
            chunk.needs_recompute = true;
        }
    }
    return {};
}

Result<void> RasterStore::commit() {
    if (!impl_->writable || impl_->complete || impl_->aborted)
        return invalid("Raster store cannot be committed in its current state.");
    std::lock_guard lock(impl_->mutex);
    for (const std::vector<bool>& rows : impl_->written_rows) {
        if (std::find(rows.begin(), rows.end(), false) != rows.end())
            return invalid("Raster store cannot commit with unwritten rows.");
    }
    try {
        for (ChunkStorage& chunk : impl_->chunks) {
            const PlaneStorage& storage =
                impl_->planes[impl_->flat_plane(chunk.frame, chunk.plane)];
            if (chunk.needs_recompute || chunk.next_row != chunk.first_row + chunk.row_count) {
                std::vector<std::byte> row(static_cast<std::size_t>(storage.row_bytes));
                chunk.crc_state = 0xFFFFFFFFU;
                for (std::uint32_t index = 0; index < chunk.row_count; ++index) {
                    const std::uint64_t offset =
                        storage.offset +
                        static_cast<std::uint64_t>(chunk.first_row + index) * storage.row_stride;
                    impl_->file.clear();
                    impl_->file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                    impl_->file.read(reinterpret_cast<char*>(row.data()),
                                     static_cast<std::streamsize>(row.size()));
                    if (!impl_->file)
                        return io_error("Could not checksum raster store chunk.");
                    chunk.crc_state = crc32c_update(chunk.crc_state, row);
                }
            }
            chunk.crc = chunk.crc_state ^ 0xFFFFFFFFU;
        }
        std::fill(impl_->verified_chunks.begin(), impl_->verified_chunks.end(), true);
        Result<void> status = impl_->write_header_and_manifest(kComplete);
        if (!status)
            return status;
        impl_->complete = true;
        impl_->writable = false;
        return {};
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate raster checksum buffer.",
                             "raster store");
    }
}

void RasterStore::abort() noexcept {
    if (!impl_ || impl_->aborted || impl_->complete)
        return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->file.close();
        impl_->aborted = true;
        impl_->writable = false;
    }
    std::error_code ignored;
    std::filesystem::remove(impl_->path, ignored);
}

bool RasterStore::complete() const noexcept {
    return impl_->complete;
}

const std::filesystem::path& RasterStore::path() const noexcept {
    return impl_->path;
}

std::uint64_t RasterStore::file_bytes() const noexcept {
    return impl_->file_bytes;
}

std::array<std::byte, 16> RasterStore::session_nonce() const noexcept {
    return impl_->nonce;
}

RasterAnalysis RasterStore::analysis() const noexcept {
    return impl_->analysis;
}

Result<void> RasterStore::set_analysis(RasterAnalysis analysis) {
    if (!impl_ || !impl_->analysis_writable || impl_->aborted)
        return invalid("Raster store analysis is not writable.");
    const bool alpha_capable =
        std::any_of(impl_->descriptor.frames.begin(), impl_->descriptor.frames.end(),
                    [](const RasterFrameDescriptor& frame) {
                        return std::any_of(frame.layout.planes.begin(), frame.layout.planes.end(),
                                           [](const PlaneDescriptor& plane) {
                                               return plane.semantic == PlaneSemantic::alpha ||
                                                      plane.format.alpha != AlphaMode::none;
                                           });
                    });
    if (analysis.alpha_content == AlphaContent::non_opaque && !alpha_capable)
        return invalid("Raster analysis contradicts the descriptor alpha layout.");
    std::lock_guard lock(impl_->mutex);
    impl_->analysis = analysis;
    if (impl_->complete)
        return impl_->write_header_and_manifest(kComplete);
    return impl_->write_header_and_manifest(kIncomplete);
}

Result<DocumentDescriptor> describe_document(const Document& document) {
    try {
        DocumentDescriptor descriptor;
        descriptor.format = document.format;
        descriptor.canvas_width = document.canvas_width;
        descriptor.canvas_height = document.canvas_height;
        descriptor.loop_count = document.loop_count;
        descriptor.metadata = document.metadata;
        descriptor.color = document.color;
        descriptor.frames.reserve(document.frames.size());
        for (const Frame& source : document.frames) {
            RasterFrameDescriptor frame;
            frame.width = source.image.width();
            frame.height = source.image.height();
            frame.x = source.x;
            frame.y = source.y;
            frame.duration = source.duration;
            frame.blend = source.blend;
            frame.disposal = source.disposal;
            frame.metadata = source.metadata;
            frame.color = source.color;
            frame.cursor_hotspot = source.cursor_hotspot;
            frame.layout.color_model =
                source.image.format().channels == ChannelLayout::gray ||
                        source.image.format().channels == ChannelLayout::gray_alpha
                    ? ColorModel::gray
                    : ColorModel::rgb;
            frame.layout.alpha = source.image.format().alpha;
            frame.layout.planes.push_back({PlaneSemantic::packed, source.image.width(),
                                           source.image.height(), source.image.format(),
                                           source.image.format().bits_per_channel});
            descriptor.frames.push_back(std::move(frame));
        }
        Result<void> status = descriptor.validate();
        if (!status)
            return status.error();
        return descriptor;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate a document descriptor.");
    }
}

Result<DocumentDescriptor> describe_document(const DocumentInfo& document) {
    try {
        DocumentDescriptor descriptor;
        descriptor.format = document.format;
        descriptor.kind = document.is_vector ? DocumentKind::vector : DocumentKind::raster;
        descriptor.canvas_width = document.canvas_width;
        descriptor.canvas_height = document.canvas_height;
        descriptor.loop_count = document.loop_count;
        descriptor.metadata = document.metadata;
        descriptor.color = document.color;
        if (descriptor.kind != DocumentKind::raster) {
            return Status::error(ErrorCode::unsupported_feature,
                                 "Vector documents require a vector payload source.");
        }
        descriptor.frames.reserve(document.frames.size());
        for (const FrameInfo& source : document.frames) {
            RasterFrameDescriptor frame;
            frame.width = source.width;
            frame.height = source.height;
            frame.x = source.x;
            frame.y = source.y;
            frame.duration = source.duration;
            frame.blend = source.blend;
            frame.disposal = source.disposal;
            frame.metadata = source.metadata;
            frame.color = source.color;
            frame.cursor_hotspot = source.cursor_hotspot;
            frame.layout.color_model =
                source.native_format.channels == ChannelLayout::gray ||
                        source.native_format.channels == ChannelLayout::gray_alpha
                    ? ColorModel::gray
                    : ColorModel::rgb;
            frame.layout.alpha = source.native_format.alpha;
            frame.layout.planes.push_back({PlaneSemantic::packed, source.width, source.height,
                                           source.native_format,
                                           source.native_format.bits_per_channel});
            descriptor.frames.push_back(std::move(frame));
        }
        Result<void> status = descriptor.validate();
        if (!status)
            return status.error();
        return descriptor;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate a document descriptor.");
    }
}

} // namespace snow::image
