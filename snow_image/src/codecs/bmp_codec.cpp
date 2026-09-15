#include "codecs/bmp_codec.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace snow::image::internal {
namespace {

constexpr std::uint32_t kBiRgb = 0;
constexpr std::uint32_t kBiBitfields = 3;

std::uint16_t read_u16(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(
            std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U));
}

std::uint32_t read_u32(std::span<const std::byte> bytes, std::size_t offset) {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2])) << 16U |
           static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3])) << 24U;
}

std::int32_t read_i32(std::span<const std::byte> bytes, std::size_t offset) {
    std::uint32_t value = read_u32(bytes, offset);
    std::int32_t result = 0;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

void append_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_u32(std::vector<std::byte>& bytes, std::uint32_t value) {
    for (unsigned int shift = 0; shift < 32U; shift += 8U) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void append_i32(std::vector<std::byte>& bytes, std::int32_t value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    append_u32(bytes, bits);
}

struct BmpInfo final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t pixel_offset = 0;
    std::uint32_t row_stride = 0;
    std::uint16_t bits_per_pixel = 0;
    std::uint32_t compression = 0;
    bool top_down = false;
};

Result<BmpInfo> parse_info(std::span<const std::byte> bytes, std::uint64_t file_size,
                           const DecodeLimits& limits) {
    if (bytes.size() < 54 || bytes[0] != std::byte{'B'} || bytes[1] != std::byte{'M'}) {
        return Status::error(ErrorCode::truncated_data, "BMP file header is missing or truncated.",
                             "snow BMP");
    }
    const std::uint32_t dib_size = read_u32(bytes, 14);
    if (dib_size < 40 || static_cast<std::uint64_t>(14) + dib_size > bytes.size()) {
        return Status::error(ErrorCode::unsupported_feature,
                             "BMP DIB header is unsupported or truncated.", "snow BMP");
    }
    const std::int32_t signed_width = read_i32(bytes, 18);
    const std::int32_t signed_height = read_i32(bytes, 22);
    if (signed_width <= 0 || signed_height == 0 ||
        signed_height == std::numeric_limits<std::int32_t>::min() || read_u16(bytes, 26) != 1) {
        return Status::error(ErrorCode::corrupt_data, "BMP dimensions or plane count are invalid.",
                             "snow BMP");
    }
    BmpInfo info;
    info.width = static_cast<std::uint32_t>(signed_width);
    info.height = static_cast<std::uint32_t>(signed_height < 0 ? -signed_height : signed_height);
    info.top_down = signed_height < 0;
    info.pixel_offset = read_u32(bytes, 10);
    info.bits_per_pixel = read_u16(bytes, 28);
    info.compression = read_u32(bytes, 30);
    Result<void> dimensions = validate_dimensions(info.width, info.height, limits);
    if (!dimensions) {
        return dimensions.error();
    }
    if ((info.bits_per_pixel != 24 && info.bits_per_pixel != 32) ||
        (info.compression != kBiRgb && info.compression != kBiBitfields)) {
        return Status::error(
            ErrorCode::unsupported_feature,
            "This build currently accepts 24-bit RGB and 32-bit RGB/bitfield BMP data.",
            "snow BMP");
    }
    const std::uint64_t row_bits = static_cast<std::uint64_t>(info.width) * info.bits_per_pixel;
    const std::uint64_t row_stride = ((row_bits + 31U) / 32U) * 4U;
    if (row_stride > std::numeric_limits<std::uint32_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "BMP row stride is too large.", "snow BMP");
    }
    info.row_stride = static_cast<std::uint32_t>(row_stride);
    const std::uint64_t pixel_end =
        static_cast<std::uint64_t>(info.pixel_offset) + row_stride * info.height;
    if (info.pixel_offset < 14U + dib_size || pixel_end > file_size) {
        return Status::error(ErrorCode::truncated_data, "BMP pixel data is truncated.", "snow BMP");
    }
    return info;
}

Result<void> read_exact(const ByteSource& source, std::uint64_t offset,
                        std::span<std::byte> destination) {
    std::size_t completed = 0;
    while (completed < destination.size()) {
        Result<std::size_t> count =
            source.read_at(offset + completed, destination.subspan(completed));
        if (!count)
            return count.error();
        if (count.value() == 0) {
            return Status::error(ErrorCode::truncated_data, "BMP input ended unexpectedly.",
                                 "snow BMP");
        }
        completed += count.value();
    }
    return {};
}

Result<BmpInfo> read_info(const Input& input, const DecodeOptions& options) {
    Result<std::uint64_t> size = input.source->size();
    if (!size)
        return size.error();
    if (size.value() > options.limits.maximum_input_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "BMP input exceeds the configured byte limit.", "snow BMP");
    }
    std::array<std::byte, 18> prefix{};
    Result<void> status = read_exact(*input.source, 0, prefix);
    if (!status)
        return status.error();
    if (prefix[0] != std::byte{'B'} || prefix[1] != std::byte{'M'}) {
        return Status::error(ErrorCode::corrupt_data, "BMP signature is invalid.", "snow BMP");
    }
    const std::uint32_t dib_size = read_u32(prefix, 14);
    constexpr std::uint32_t kMaximumSupportedHeaderBytes = 1U << 20U;
    if (dib_size < 40 || dib_size > kMaximumSupportedHeaderBytes - 14U ||
        static_cast<std::uint64_t>(14U) + dib_size > size.value()) {
        return Status::error(ErrorCode::unsupported_feature,
                             "BMP DIB header is unsupported or truncated.", "snow BMP");
    }
    std::vector<std::byte> header(static_cast<std::size_t>(14U + dib_size));
    status = read_exact(*input.source, 0, header);
    if (!status)
        return status.error();
    return parse_info(header, size.value(), options.limits);
}

DocumentInfo document_info(const BmpInfo& info) {
    DocumentInfo document;
    document.format = Format::bmp;
    document.canvas_width = info.width;
    document.canvas_height = info.height;
    document.frames.push_back({info.width,
                               info.height,
                               0,
                               0,
                               std::chrono::nanoseconds{0},
                               kRgba8,
                               true,
                               std::nullopt,
                               {},
                               {},
                               FrameBlend::source,
                               FrameDisposal::keep});
    return document;
}

void convert_row(const BmpInfo& info, std::span<const std::byte> source,
                 std::span<std::byte> destination) {
    const std::size_t source_pixel_bytes = info.bits_per_pixel / 8U;
    for (std::uint32_t x = 0; x < info.width; ++x) {
        const std::size_t source_offset = static_cast<std::size_t>(x) * source_pixel_bytes;
        const std::size_t destination_offset = static_cast<std::size_t>(x) * 4U;
        destination[destination_offset] = source[source_offset + 2U];
        destination[destination_offset + 1U] = source[source_offset + 1U];
        destination[destination_offset + 2U] = source[source_offset];
        destination[destination_offset + 3U] =
            info.bits_per_pixel == 32 ? source[source_offset + 3U] : std::byte{0xFF};
    }
}

Result<ImageView> first_frame_view(const Document& document) {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "BMP encoding requires one frame.");
    }
    const ImageView view = document.frames.front().image.view();
    Result<void> valid = view.validate();
    if (!valid) {
        return valid.error();
    }
    if (view.format.bits_per_channel != 8 || (view.format.channels != ChannelLayout::rgb &&
                                              view.format.channels != ChannelLayout::rgba &&
                                              view.format.channels != ChannelLayout::bgr &&
                                              view.format.channels != ChannelLayout::bgra)) {
        return Status::error(ErrorCode::unsupported_feature,
                             "BMP encoding requires packed 8-bit RGB, BGR, RGBA, or BGRA pixels.",
                             "snow BMP");
    }
    return view;
}

} // namespace

CodecCapability BmpCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::streaming_decode | CodecCapability::metadata_decode;
}

int BmpCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 2 && header[0] == std::byte{'B'} && header[1] == std::byte{'M'}) {
        return 100;
    }
    return format_from_extension(name_hint) == Format::bmp ? 10 : 0;
}

Result<DocumentInfo> BmpCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    if (stop.stop_requested()) {
        return cancelled_status();
    }
    Result<BmpInfo> info = read_info(input, options);
    if (!info) {
        return info.error();
    }
    return document_info(info.value());
}

Result<Document> BmpCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes) {
        return bytes.error();
    }
    Result<BmpInfo> parsed = parse_info(bytes.value(), bytes.value().size(), options.limits);
    if (!parsed) {
        return parsed.error();
    }
    const BmpInfo& info = parsed.value();
    const std::uint64_t output_bytes = static_cast<std::uint64_t>(info.width) * info.height * 4U;
    if (output_bytes > options.limits.maximum_owned_output_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "BMP output exceeds the owning decode limit; use decode_to_sink().",
                             "snow BMP");
    }
    Result<MutableImage> allocated = MutableImage::allocate(info.width, info.height, kRgba8);
    if (!allocated) {
        return allocated.error();
    }
    MutableImage image = std::move(allocated).value();
    for (std::uint32_t y = 0; y < info.height; ++y) {
        if (stop.stop_requested()) {
            return cancelled_status();
        }
        const std::uint32_t source_y = info.top_down ? y : info.height - 1U - y;
        const std::size_t source_offset = static_cast<std::size_t>(info.pixel_offset) +
                                          static_cast<std::size_t>(source_y) * info.row_stride;
        const std::size_t destination_offset = static_cast<std::size_t>(y) * image.row_stride();
        convert_row(info, std::span(bytes.value()).subspan(source_offset, info.row_stride),
                    image.pixels().subspan(destination_offset, image.row_stride()));
    }
    Document document;
    document.format = Format::bmp;
    document.canvas_width = info.width;
    document.canvas_height = info.height;
    Frame frame;
    frame.image = std::move(image).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

Result<void> BmpCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                      const DecodeOptions& options, std::stop_token stop) const {
    Result<BmpInfo> parsed = read_info(input, options);
    if (!parsed) {
        return parsed.error();
    }
    const BmpInfo& info = parsed.value();
    const DocumentInfo metadata = document_info(info);
    Result<void> status = sink.begin(metadata);
    if (!status)
        return status;
    status = sink.begin_frame(0, metadata.frames.front());
    if (!status)
        return status;
    const std::size_t output_row_bytes = static_cast<std::size_t>(info.width) * 4U;
    const std::uint64_t per_row_working =
        static_cast<std::uint64_t>(info.row_stride) + output_row_bytes;
    if (per_row_working > options.limits.maximum_working_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "BMP row exceeds the configured working-memory limit.", "snow BMP");
    }
    constexpr std::uint32_t kPreferredStripeRows = 64;
    const std::uint64_t rows_by_limit = options.limits.maximum_working_bytes / per_row_working;
    const std::uint32_t stripe_rows = static_cast<std::uint32_t>(
        std::max<std::uint64_t>(1, std::min<std::uint64_t>(kPreferredStripeRows, rows_by_limit)));
    std::vector<std::byte> source_rows(static_cast<std::size_t>(stripe_rows) * info.row_stride);
    std::vector<std::byte> row(output_row_bytes);
    for (std::uint32_t y = 0; y < info.height;) {
        if (stop.stop_requested())
            return cancelled_status();
        const std::uint32_t count = std::min(stripe_rows, info.height - y);
        const std::uint32_t first_source_row = info.top_down ? y : info.height - y - count;
        const std::size_t source_bytes = static_cast<std::size_t>(count) * info.row_stride;
        status = read_exact(*input.source,
                            static_cast<std::uint64_t>(info.pixel_offset) +
                                static_cast<std::uint64_t>(first_source_row) * info.row_stride,
                            std::span(source_rows).first(source_bytes));
        if (!status)
            return status;
        for (std::uint32_t relative = 0; relative < count; ++relative) {
            const std::uint32_t source_relative = info.top_down ? relative : count - 1U - relative;
            convert_row(info,
                        std::span(source_rows)
                            .subspan(static_cast<std::size_t>(source_relative) * info.row_stride,
                                     info.row_stride),
                        row);
            status = sink.write_rows(y + relative, 1, row.size(), row);
            if (!status)
                return status;
        }
        y += count;
    }
    status = sink.end_frame(0);
    if (!status)
        return status;
    return sink.end();
}

Result<EncodedArtifactReceipt> BmpCodec::encode_to_sink(const Document& document,
                                                        const Output& output, const EncodeOptions&,
                                                        std::stop_token stop) const {
    Result<ImageView> selected = first_frame_view(document);
    if (!selected) {
        return selected.error();
    }
    const ImageView view = selected.value();
    const std::uint64_t row_bytes64 = static_cast<std::uint64_t>(view.width) * 4U;
    const std::uint64_t image_bytes64 = row_bytes64 * view.height;
    constexpr std::uint32_t kHeaderBytes = 14U + 124U;
    if (image_bytes64 > std::numeric_limits<std::uint32_t>::max() - kHeaderBytes) {
        return Status::error(ErrorCode::limit_exceeded, "BMP output exceeds the format size limit.",
                             "snow BMP");
    }
    std::vector<std::byte> header;
    header.reserve(kHeaderBytes);
    header.push_back(std::byte{'B'});
    header.push_back(std::byte{'M'});
    append_u32(header, kHeaderBytes + static_cast<std::uint32_t>(image_bytes64));
    append_u16(header, 0);
    append_u16(header, 0);
    append_u32(header, kHeaderBytes);
    append_u32(header, 124);
    append_i32(header, static_cast<std::int32_t>(view.width));
    append_i32(header, -static_cast<std::int32_t>(view.height));
    append_u16(header, 1);
    append_u16(header, 32);
    append_u32(header, kBiBitfields);
    append_u32(header, static_cast<std::uint32_t>(image_bytes64));
    append_i32(header, 2835);
    append_i32(header, 2835);
    append_u32(header, 0);
    append_u32(header, 0);
    append_u32(header, 0x00FF0000U);
    append_u32(header, 0x0000FF00U);
    append_u32(header, 0x000000FFU);
    append_u32(header, 0xFF000000U);
    append_u32(header, 0x73524742U);
    header.resize(kHeaderBytes, std::byte{0});
    Result<void> status = output.sink->write(header);
    if (!status)
        return status.error();

    std::vector<std::byte> row(static_cast<std::size_t>(row_bytes64));
    const std::size_t channel_count = view.format.channel_count();
    for (std::uint32_t y = 0; y < view.height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        const auto source =
            view.pixels.subspan(static_cast<std::size_t>(y) * view.row_stride, view.row_stride);
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const std::size_t source_offset = static_cast<std::size_t>(x) * channel_count;
            const std::size_t destination_offset = static_cast<std::size_t>(x) * 4U;
            const bool source_bgr = view.format.channels == ChannelLayout::bgr ||
                                    view.format.channels == ChannelLayout::bgra;
            row[destination_offset] = source[source_offset + (source_bgr ? 0U : 2U)];
            row[destination_offset + 1U] = source[source_offset + 1U];
            row[destination_offset + 2U] = source[source_offset + (source_bgr ? 2U : 0U)];
            row[destination_offset + 3U] =
                channel_count == 4U ? source[source_offset + 3U] : std::byte{0xFF};
        }
        status = output.sink->write(row);
        if (!status)
            return status.error();
    }
    return receipt_for_document(document, format());
}

} // namespace snow::image::internal
