#include "codecs/icon_codec.h"

#include "codecs/bmp_codec.h"
#if defined(SNOW_IMAGE_HAS_PNG)
#include "codecs/png_codec.h"
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace snow::image::internal {
namespace {

constexpr std::array<std::byte, 8> kPngSignature{std::byte{0x89}, std::byte{'P'},  std::byte{'N'},
                                                 std::byte{'G'},  std::byte{0x0D}, std::byte{0x0A},
                                                 std::byte{0x1A}, std::byte{0x0A}};

std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U);
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U])) << 24U;
}

std::int32_t read_i32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t bits = read_u32(bytes, offset);
    std::int32_t value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

#if defined(SNOW_IMAGE_HAS_PNG)
void write_u16(std::span<std::byte> bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::byte>(value >> 8U);
}
#endif

void write_u32(std::span<std::byte> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32U; shift += 8U) {
        bytes[offset + shift / 8U] = static_cast<std::byte>((value >> shift) & 0xFFU);
    }
}

void write_i32(std::span<std::byte> bytes, std::size_t offset, std::int32_t value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    write_u32(bytes, offset, bits);
}

struct IconEntry final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t field1 = 0;
    std::uint16_t field2 = 0;
    std::uint32_t size = 0;
    std::uint32_t offset = 0;
};

struct IconInfo final {
    bool cursor = false;
    std::vector<IconEntry> entries;
};

Result<IconInfo> parse_directory(std::span<const std::byte> bytes, bool expected_cursor,
                                 const DecodeOptions& options) {
    if (bytes.size() < 6) {
        return Status::error(ErrorCode::truncated_data, "Icon directory is truncated.",
                             expected_cursor ? "snow CUR" : "snow ICO");
    }
    const std::uint16_t type = read_u16(bytes, 2);
    const std::uint16_t count = read_u16(bytes, 4);
    if (read_u16(bytes, 0) != 0 || type != (expected_cursor ? 2U : 1U) || count == 0) {
        return Status::error(ErrorCode::corrupt_data, "Icon directory header is invalid.",
                             expected_cursor ? "snow CUR" : "snow ICO");
    }
    if (count > options.limits.maximum_frames ||
        static_cast<std::uint64_t>(6) + static_cast<std::uint64_t>(count) * 16U > bytes.size()) {
        return Status::error(count > options.limits.maximum_frames ? ErrorCode::limit_exceeded
                                                                   : ErrorCode::truncated_data,
                             "Icon directory entry count is invalid.",
                             expected_cursor ? "snow CUR" : "snow ICO");
    }
    IconInfo info;
    info.cursor = expected_cursor;
    info.entries.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        const std::size_t entry_offset = 6U + static_cast<std::size_t>(index) * 16U;
        IconEntry entry;
        entry.width = std::to_integer<std::uint8_t>(bytes[entry_offset]);
        entry.height = std::to_integer<std::uint8_t>(bytes[entry_offset + 1U]);
        if (entry.width == 0)
            entry.width = 256;
        if (entry.height == 0)
            entry.height = 256;
        entry.field1 = read_u16(bytes, entry_offset + 4U);
        entry.field2 = read_u16(bytes, entry_offset + 6U);
        entry.size = read_u32(bytes, entry_offset + 8U);
        entry.offset = read_u32(bytes, entry_offset + 12U);
        if (entry.size == 0 || entry.offset < 6U + static_cast<std::uint32_t>(count) * 16U ||
            static_cast<std::uint64_t>(entry.offset) + entry.size > bytes.size()) {
            return Status::error(ErrorCode::truncated_data,
                                 "Icon image payload is invalid or truncated.",
                                 expected_cursor ? "snow CUR" : "snow ICO");
        }
        Result<void> dimensions = validate_dimensions(entry.width, entry.height, options.limits);
        if (!dimensions)
            return dimensions.error();
        info.entries.push_back(entry);
    }
    return info;
}

DocumentInfo document_info(const IconInfo& icon) {
    DocumentInfo document;
    document.format = icon.cursor ? Format::cur : Format::ico;
    for (const IconEntry& entry : icon.entries) {
        document.canvas_width = std::max(document.canvas_width, entry.width);
        document.canvas_height = std::max(document.canvas_height, entry.height);
        std::optional<std::array<std::uint32_t, 2>> hotspot;
        if (icon.cursor)
            hotspot = std::array<std::uint32_t, 2>{entry.field1, entry.field2};
        document.frames.push_back({entry.width,
                                   entry.height,
                                   0,
                                   0,
                                   std::chrono::nanoseconds{0},
                                   kRgba8,
                                   true,
                                   hotspot,
                                   {},
                                   {},
                                   FrameBlend::source,
                                   FrameDisposal::keep});
    }
    return document;
}

bool png_payload(std::span<const std::byte> bytes) {
    return bytes.size() >= kPngSignature.size() &&
           std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin());
}

Result<std::vector<std::byte>> synthetic_bmp(std::span<const std::byte> dib,
                                             std::uint32_t expected_width,
                                             std::uint32_t expected_height) {
    if (dib.size() < 40) {
        return Status::error(ErrorCode::unsupported_feature,
                             "Icon DIB header is too small for the portable BMP decoder.",
                             "snow ICO");
    }
    const std::uint32_t header_size = read_u32(dib, 0);
    const std::int32_t width = read_i32(dib, 4);
    const std::int32_t doubled_height = read_i32(dib, 8);
    const std::uint16_t bits_per_pixel = read_u16(dib, 14);
    const std::uint32_t compression = read_u32(dib, 16);
    if (header_size < 40 || header_size > dib.size() || width <= 0 || doubled_height <= 0 ||
        (doubled_height % 2) != 0 || static_cast<std::uint32_t>(width) != expected_width ||
        static_cast<std::uint32_t>(doubled_height / 2) != expected_height) {
        return Status::error(ErrorCode::corrupt_data, "Icon DIB dimensions are invalid.",
                             "snow ICO");
    }
    std::uint32_t mask_bytes = header_size == 40 && compression == 3 ? 12U : 0U;
    const std::uint32_t used_colors = read_u32(dib, 32);
    const std::uint32_t palette_entries =
        bits_per_pixel <= 8 ? (used_colors != 0 ? used_colors : 1U << bits_per_pixel) : 0U;
    const std::uint64_t pixel_offset = static_cast<std::uint64_t>(14) + header_size + mask_bytes +
                                       static_cast<std::uint64_t>(palette_entries) * 4U;
    if (pixel_offset > std::numeric_limits<std::uint32_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "Icon DIB pixel offset is too large.",
                             "snow ICO");
    }
    std::vector<std::byte> bmp(14U + dib.size(), std::byte{0});
    bmp[0] = std::byte{'B'};
    bmp[1] = std::byte{'M'};
    write_u32(bmp, 2, static_cast<std::uint32_t>(bmp.size()));
    write_u32(bmp, 10, static_cast<std::uint32_t>(pixel_offset));
    std::copy(dib.begin(), dib.end(), bmp.begin() + 14);
    write_i32(bmp, 14U + 8U, doubled_height / 2);
    return bmp;
}

Result<Frame> decode_dib(std::span<const std::byte> payload, const IconEntry& entry,
                         const DecodeOptions& options, std::stop_token stop) {
    Result<std::vector<std::byte>> bmp_bytes = synthetic_bmp(payload, entry.width, entry.height);
    if (!bmp_bytes)
        return bmp_bytes.error();
    BmpCodec codec;
    Result<Document> decoded =
        codec.decode(memory_input(bmp_bytes.value(), "embedded.bmp"), options, stop);
    if (!decoded)
        return decoded.error();
    Frame frame = std::move(decoded.value().frames.front());
    Result<MutableImage> mutable_pixels = MutableImage::copy(frame.image.view());
    if (!mutable_pixels)
        return mutable_pixels.error();
    MutableImage pixels = std::move(mutable_pixels).value();

    const std::uint32_t header_size = read_u32(payload, 0);
    const std::uint16_t bits_per_pixel = read_u16(payload, 14);
    const std::uint32_t compression = read_u32(payload, 16);
    const std::uint32_t mask_bytes = header_size == 40 && compression == 3 ? 12U : 0U;
    const std::uint32_t used_colors = read_u32(payload, 32);
    const std::uint32_t palette_entries =
        bits_per_pixel <= 8 ? (used_colors != 0 ? used_colors : 1U << bits_per_pixel) : 0U;
    const std::uint64_t xor_offset = static_cast<std::uint64_t>(header_size) + mask_bytes +
                                     static_cast<std::uint64_t>(palette_entries) * 4U;
    const std::uint64_t xor_stride =
        ((static_cast<std::uint64_t>(entry.width) * bits_per_pixel + 31U) / 32U) * 4U;
    const std::uint64_t and_offset = xor_offset + xor_stride * entry.height;
    const std::uint64_t and_stride = ((static_cast<std::uint64_t>(entry.width) + 31U) / 32U) * 4U;
    const bool has_and_mask = and_offset + and_stride * entry.height <= payload.size();
    bool any_alpha = false;
    if (bits_per_pixel == 32) {
        for (std::size_t offset = 3; offset < pixels.pixels().size(); offset += 4U) {
            any_alpha |= pixels.pixels()[offset] != std::byte{0};
        }
    }
    for (std::uint32_t y = 0; y < entry.height; ++y) {
        for (std::uint32_t x = 0; x < entry.width; ++x) {
            bool masked = false;
            if (has_and_mask) {
                const std::uint32_t source_y = entry.height - 1U - y;
                const std::uint8_t value = std::to_integer<std::uint8_t>(
                    payload[static_cast<std::size_t>(and_offset + source_y * and_stride + x / 8U)]);
                masked = (value & (0x80U >> (x % 8U))) != 0;
            }
            const std::size_t alpha = static_cast<std::size_t>(y) * pixels.row_stride() +
                                      static_cast<std::size_t>(x) * 4U + 3U;
            if (masked)
                pixels.pixels()[alpha] = std::byte{0};
            else if (!any_alpha)
                pixels.pixels()[alpha] = std::byte{0xFF};
        }
    }
    frame.image = std::move(pixels).freeze();
    return frame;
}

Result<Frame> decode_entry(std::span<const std::byte> bytes, const IconEntry& entry,
                           const DecodeOptions& options, std::stop_token stop) {
    const auto payload = bytes.subspan(entry.offset, entry.size);
    if (png_payload(payload)) {
#if defined(SNOW_IMAGE_HAS_PNG)
        PngCodec codec;
        auto owned = std::make_shared<std::vector<std::byte>>(payload.begin(), payload.end());
        Result<Document> decoded =
            codec.decode(memory_input(std::move(owned), "embedded.png"), options, stop);
        if (!decoded)
            return decoded.error();
        return std::move(decoded.value().frames.front());
#else
        return Status::error(ErrorCode::codec_unavailable,
                             "This icon uses PNG data, but the PNG codec is disabled.", "snow ICO");
#endif
    }
    return decode_dib(payload, entry, options, stop);
}

} // namespace

CodecCapability IconCodec::capabilities() const noexcept {
    CodecCapability capabilities =
        CodecCapability::inspect | CodecCapability::decode | CodecCapability::multiple_images |
        CodecCapability::streaming_decode | CodecCapability::metadata_decode;
#if defined(SNOW_IMAGE_HAS_PNG)
    capabilities = capabilities | CodecCapability::encode;
#endif
    return capabilities;
}

int IconCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 6 && read_u16(header, 0) == 0 &&
        read_u16(header, 2) == (cursor_ ? 2U : 1U) && read_u16(header, 4) != 0)
        return 100;
    return format_from_extension(name_hint) == format() ? 10 : 0;
}

Result<DocumentInfo> IconCodec::inspect(const Input& input, const DecodeOptions& options,
                                        std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<IconInfo> parsed = parse_directory(bytes.value(), cursor_, options);
    if (!parsed)
        return parsed.error();
    DocumentInfo info = document_info(parsed.value());
    if (options.frame_index) {
        if (*options.frame_index >= info.frames.size())
            return Status::error(ErrorCode::invalid_argument,
                                 "Requested icon frame does not exist.", std::string(name()));
        FrameInfo selected = info.frames[*options.frame_index];
        info.frames.clear();
        info.frames.push_back(std::move(selected));
    }
    return info;
}

Result<Document> IconCodec::decode(const Input& input, const DecodeOptions& options,
                                   std::stop_token stop) const {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<IconInfo> parsed = parse_directory(bytes.value(), cursor_, options);
    if (!parsed)
        return parsed.error();
    const DocumentInfo info = document_info(parsed.value());
    std::uint64_t output_bytes = 0;
    Document document;
    document.format = format();
    document.canvas_width = info.canvas_width;
    document.canvas_height = info.canvas_height;
    for (std::size_t index = 0; index < parsed.value().entries.size(); ++index) {
        if (options.frame_index && *options.frame_index != index)
            continue;
        const IconEntry& entry = parsed.value().entries[index];
        output_bytes += static_cast<std::uint64_t>(entry.width) * entry.height * 4U;
        if (output_bytes > options.limits.maximum_owned_output_bytes) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "Icon images exceed the owning decode limit.",
                                 std::string(name()));
        }
        Result<Frame> frame = decode_entry(bytes.value(), entry, options, stop);
        if (!frame)
            return frame.error();
        if (cursor_) {
            frame.value().cursor_hotspot = std::array<std::uint32_t, 2>{entry.field1, entry.field2};
        }
        document.frames.push_back(std::move(frame).value());
    }
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "Requested icon frame does not exist.",
                             std::string(name()));
    }
    return document;
}

Result<void> IconCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                       const DecodeOptions& options, std::stop_token stop) const {
    if (options.output_format && *options.output_format != kRgba8)
        return Status::error(ErrorCode::unsupported_feature,
                             "ICO/CUR decoding supports RGBA8 output.", std::string(name()));
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<IconInfo> parsed = parse_directory(bytes.value(), cursor_, options);
    if (!parsed)
        return parsed.error();
    if (options.frame_index && *options.frame_index >= parsed.value().entries.size())
        return Status::error(ErrorCode::invalid_argument, "Requested icon frame does not exist.",
                             std::string(name()));
    DocumentInfo info = document_info(parsed.value());
    const std::size_t first = options.frame_index ? *options.frame_index : 0;
    const std::size_t end = options.frame_index ? first + 1 : info.frames.size();
    if (options.frame_index) {
        FrameInfo selected = info.frames[first];
        info.frames.clear();
        info.frames.push_back(std::move(selected));
    }
    for (std::size_t index = first; index < end; ++index) {
        const IconEntry& entry = parsed.value().entries[index];
        const std::uint64_t frame_bytes =
            static_cast<std::uint64_t>(entry.width) * entry.height * 4U;
        if (frame_bytes > options.limits.maximum_working_bytes)
            return Status::error(
                ErrorCode::limit_exceeded,
                "One icon image exceeds the configured streaming working-memory limit.",
                std::string(name()));
    }
    Result<void> status = sink.begin(info);
    if (!status)
        return status;
    for (std::size_t index = first; index < end; ++index) {
        if (stop.stop_requested())
            return cancelled_status();
        const IconEntry& entry = parsed.value().entries[index];
        const std::uint32_t sink_index = static_cast<std::uint32_t>(index - first);
        status = sink.begin_frame(sink_index, info.frames[sink_index]);
        if (!status)
            return status;
        DecodeOptions frame_options = options;
        frame_options.frame_index.reset();
        frame_options.output_format = kRgba8;
        const std::uint64_t frame_bytes =
            static_cast<std::uint64_t>(entry.width) * entry.height * 4U;
        frame_options.limits.maximum_owned_output_bytes =
            std::max(frame_options.limits.maximum_owned_output_bytes, frame_bytes);
        Result<Frame> frame = decode_entry(bytes.value(), entry, frame_options, stop);
        if (!frame)
            return frame.error();
        if (cursor_)
            frame.value().cursor_hotspot = std::array<std::uint32_t, 2>{entry.field1, entry.field2};
        const Image& image = frame.value().image;
        const std::size_t byte_size = image.row_stride() * static_cast<std::size_t>(image.height());
        std::span<std::byte> storage =
            sink.frame_storage(sink_index, image.row_stride(), byte_size);
        if (storage.size() == byte_size) {
            std::memcpy(storage.data(), image.pixels().data(), byte_size);
        } else {
            status = sink.write_rows(0, image.height(), image.row_stride(), image.pixels());
            if (!status)
                return status;
        }
        status = sink.end_frame(sink_index);
        if (!status)
            return status;
    }
    return sink.end();
}

Result<EncodedArtifactReceipt> IconCodec::encode_to_sink(const Document& document,
                                                         const Output& output,
                                                         const EncodeOptions& options,
                                                         std::stop_token stop) const {
#if !defined(SNOW_IMAGE_HAS_PNG)
    (void)document;
    (void)output;
    (void)options;
    (void)stop;
    return Status::error(ErrorCode::codec_unavailable, "ICO/CUR encoding requires the PNG codec.",
                         std::string(name()));
#else
    if (document.frames.empty() || document.frames.size() > 65535U) {
        return Status::error(ErrorCode::invalid_argument, "ICO/CUR frame count is invalid.",
                             std::string(name()));
    }
    std::vector<std::vector<std::byte>> payloads;
    payloads.reserve(document.frames.size());
    PngCodec png;
    for (const Frame& frame : document.frames) {
        if (stop.stop_requested())
            return cancelled_status();
        if (frame.image.width() == 0 || frame.image.height() == 0 || frame.image.width() > 256U ||
            frame.image.height() > 256U) {
            return Status::error(ErrorCode::invalid_argument,
                                 "ICO/CUR entries must be between 1 and 256 pixels.",
                                 std::string(name()));
        }
        auto payload = std::make_shared<std::vector<std::byte>>();
        Document single;
        single.format = Format::png;
        single.canvas_width = frame.image.width();
        single.canvas_height = frame.image.height();
        Frame copied;
        copied.image = frame.image;
        single.frames.push_back(std::move(copied));
        EncodeOptions png_options = options;
        png_options.format = Format::png;
        Result<EncodeResult> encoded =
            png.encode(single, memory_output(payload, "entry.png"), png_options, stop);
        if (!encoded)
            return encoded.error();
        payloads.push_back(std::move(*payload));
    }

    const std::size_t directory_size = 6U + document.frames.size() * 16U;
    std::uint64_t total_size = directory_size;
    for (const auto& payload : payloads)
        total_size += payload.size();
    if (total_size > std::numeric_limits<std::uint32_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "ICO/CUR output exceeds 4 GiB.",
                             std::string(name()));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(total_size), std::byte{0});
    write_u16(bytes, 2, cursor_ ? 2 : 1);
    write_u16(bytes, 4, static_cast<std::uint16_t>(document.frames.size()));
    std::uint32_t payload_offset = static_cast<std::uint32_t>(directory_size);
    for (std::size_t index = 0; index < document.frames.size(); ++index) {
        const Frame& frame = document.frames[index];
        const std::size_t entry_offset = 6U + index * 16U;
        bytes[entry_offset] =
            frame.image.width() == 256 ? std::byte{0} : static_cast<std::byte>(frame.image.width());
        bytes[entry_offset + 1U] = frame.image.height() == 256
                                       ? std::byte{0}
                                       : static_cast<std::byte>(frame.image.height());
        if (cursor_) {
            const auto hotspot = frame.cursor_hotspot.value_or(std::array<std::uint32_t, 2>{0, 0});
            if (hotspot[0] >= frame.image.width() || hotspot[1] >= frame.image.height() ||
                hotspot[0] > 65535U || hotspot[1] > 65535U) {
                return Status::error(ErrorCode::invalid_argument,
                                     "CUR hotspot is outside the image.", std::string(name()));
            }
            write_u16(bytes, entry_offset + 4U, static_cast<std::uint16_t>(hotspot[0]));
            write_u16(bytes, entry_offset + 6U, static_cast<std::uint16_t>(hotspot[1]));
        } else {
            write_u16(bytes, entry_offset + 4U, 1);
            write_u16(bytes, entry_offset + 6U, 32);
        }
        write_u32(bytes, entry_offset + 8U, static_cast<std::uint32_t>(payloads[index].size()));
        write_u32(bytes, entry_offset + 12U, payload_offset);
        std::copy(payloads[index].begin(), payloads[index].end(), bytes.begin() + payload_offset);
        payload_offset += static_cast<std::uint32_t>(payloads[index].size());
    }
    Result<void> written = output.sink->write(bytes);
    if (!written)
        return written.error();
    return receipt_for_document(document, format());
#endif
}

} // namespace snow::image::internal
