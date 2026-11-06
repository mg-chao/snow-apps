#include "raster_serialization.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <arm_acle.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#elif defined(__linux__)
#include <sys/auxv.h>
#include <asm/hwcap.h>
#endif
#endif

namespace snow::image::detail::raster_serialization {
namespace {

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

#if defined(__clang__)
__attribute__((target("sse4.2")))
#endif
std::uint32_t
hardware_crc32c_update(std::uint32_t state, std::span<const std::byte> bytes) {
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
#elif defined(__aarch64__) || defined(_M_ARM64)
bool hardware_crc32c_available() {
    static const bool available = [] {
#if defined(_WIN32)
        return IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE) != FALSE;
#elif defined(__APPLE__)
        int supported = 0;
        std::size_t size = sizeof(supported);
        return sysctlbyname("hw.optional.armv8_crc32", &supported, &size, nullptr, 0) == 0 &&
               supported != 0;
#elif defined(__linux__)
        return (getauxval(AT_HWCAP) & HWCAP_CRC32) != 0;
#else
        return false;
#endif
    }();
    return available;
}

#if defined(__clang__)
__attribute__((target("crc")))
#elif defined(__GNUC__)
__attribute__((target("arch=armv8-a+crc")))
#endif
std::uint32_t
hardware_crc32c_update(std::uint32_t state, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (bytes.size() - offset >= sizeof(std::uint64_t)) {
        std::uint64_t block = 0;
        std::memcpy(&block, bytes.data() + offset, sizeof(block));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        block = __builtin_bswap64(block);
#endif
        state = __crc32cd(state, block);
        offset += sizeof(block);
    }
    while (offset < bytes.size()) {
        state = __crc32cb(state, std::to_integer<std::uint8_t>(bytes[offset++]));
    }
    return state;
}
#endif

} // namespace

bool crc32c_uses_hardware() {
#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__aarch64__) || defined(_M_ARM64)
    return hardware_crc32c_available();
#else
    return false;
#endif
}

std::uint32_t crc32c_update(std::uint32_t state, std::span<const std::byte> bytes) {
#if (defined(_MSC_VER) && defined(_M_X64)) || defined(__aarch64__) || defined(_M_ARM64)
    if (crc32c_uses_hardware())
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

void BinaryWriter::write_integer(std::uint64_t value, std::size_t size) {
    for (std::size_t index = 0; index < size; ++index) {
        bytes_.push_back(static_cast<std::byte>(value & 0xFFU));
        value >>= 8U;
    }
}

void BinaryWriter::boolean(bool value) {
    integer<std::uint8_t>(value ? 1U : 0U);
}

void BinaryWriter::floating(float value) {
    integer(std::bit_cast<std::uint32_t>(value));
}

void BinaryWriter::floating(double value) {
    integer(std::bit_cast<std::uint64_t>(value));
}

void BinaryWriter::bytes(std::span<const std::byte> value) {
    integer<std::uint64_t>(static_cast<std::uint64_t>(value.size()));
    bytes_.insert(bytes_.end(), value.begin(), value.end());
}

void BinaryWriter::string(const std::string& value) {
    bytes(std::as_bytes(std::span(value.data(), value.size())));
}

std::vector<std::byte> BinaryWriter::take() && {
    return std::move(bytes_);
}

bool BinaryReader::read_integer(std::uint64_t* value, std::size_t size) {
    if (remaining() < size)
        return false;
    std::uint64_t encoded = 0;
    for (std::size_t index = 0; index < size; ++index) {
        encoded |=
            static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes_[offset_ + index]))
            << (index * 8U);
    }
    offset_ += size;
    *value = encoded;
    return true;
}

bool BinaryReader::boolean(bool* value) {
    std::uint8_t encoded = 0;
    if (!value || !integer(&encoded) || encoded > 1)
        return false;
    *value = encoded != 0;
    return true;
}

bool BinaryReader::floating(float* value) {
    std::uint32_t encoded = 0;
    if (!value || !integer(&encoded))
        return false;
    *value = std::bit_cast<float>(encoded);
    return true;
}

bool BinaryReader::floating(double* value) {
    std::uint64_t encoded = 0;
    if (!value || !integer(&encoded))
        return false;
    *value = std::bit_cast<double>(encoded);
    return true;
}

bool BinaryReader::bytes(std::vector<std::byte>* value) {
    std::uint64_t size = 0;
    if (!value || !integer(&size) || size > remaining() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    value->assign(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                  bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size));
    offset_ += static_cast<std::size_t>(size);
    return true;
}

bool BinaryReader::string(std::string* value) {
    std::uint64_t size = 0;
    if (!value || !integer(&size) || size > remaining() ||
        size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    value->assign(reinterpret_cast<const char*>(bytes_.data() + offset_),
                  static_cast<std::size_t>(size));
    offset_ += static_cast<std::size_t>(size);
    return true;
}

std::size_t BinaryReader::remaining() const noexcept {
    return bytes_.size() - offset_;
}

namespace {

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

} // namespace

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
    if (!reader->integer(&format) || format > static_cast<std::uint8_t>(Format::tiff) ||
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

} // namespace snow::image::detail::raster_serialization
