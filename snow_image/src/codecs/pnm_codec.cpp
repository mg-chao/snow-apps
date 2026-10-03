#include "codecs/pnm_codec.h"

#include <snow/memory/pixel_array.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <string>
#include <vector>

namespace snow::image::internal {
namespace {

struct PnmInfo final {
    int kind = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t maximum_value = 1;
    std::size_t pixel_offset = 0;
    bool ascii = false;
    PixelFormat pixel_format = kGray8;
    Format format = Format::unknown;
};

bool whitespace(std::byte value) {
    return std::isspace(std::to_integer<unsigned char>(value)) != 0;
}

class Tokenizer final {
  public:
    explicit Tokenizer(std::span<const std::byte> bytes) : bytes_(bytes) {}

    Result<std::string_view> next() {
        skip_space_and_comments();
        if (position_ >= bytes_.size()) {
            return Status::error(ErrorCode::truncated_data, "Netpbm header ended unexpectedly.",
                                 "snow Netpbm");
        }
        const std::size_t start = position_;
        while (position_ < bytes_.size() && !whitespace(bytes_[position_]) &&
               bytes_[position_] != std::byte{'#'}) {
            ++position_;
        }
        return std::string_view(reinterpret_cast<const char*>(bytes_.data() + start),
                                position_ - start);
    }

    void finish_header() {
        if (position_ < bytes_.size() && whitespace(bytes_[position_])) {
            if (bytes_[position_] == std::byte{'\r'} && position_ + 1 < bytes_.size() &&
                bytes_[position_ + 1] == std::byte{'\n'}) {
                position_ += 2;
            } else {
                ++position_;
            }
        }
    }

    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

  private:
    void skip_space_and_comments() {
        for (;;) {
            while (position_ < bytes_.size() && whitespace(bytes_[position_]))
                ++position_;
            if (position_ >= bytes_.size() || bytes_[position_] != std::byte{'#'})
                return;
            while (position_ < bytes_.size() && bytes_[position_] != std::byte{'\n'} &&
                   bytes_[position_] != std::byte{'\r'}) {
                ++position_;
            }
        }
    }

    std::span<const std::byte> bytes_;
    std::size_t position_ = 0;
};

Result<std::uint32_t> parse_number(std::string_view token, std::string_view field) {
    std::uint32_t value = 0;
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size()) {
        return Status::error(ErrorCode::corrupt_data, "Invalid Netpbm " + std::string(field) + ".",
                             "snow Netpbm");
    }
    return value;
}

Result<PnmInfo> parse_info(std::span<const std::byte> bytes, const DecodeLimits& limits) {
    Tokenizer tokenizer(bytes);
    Result<std::string_view> magic = tokenizer.next();
    if (!magic || magic.value().size() != 2 || magic.value()[0] != 'P' || magic.value()[1] < '1' ||
        magic.value()[1] > '6') {
        return Status::error(ErrorCode::corrupt_data, "Invalid Netpbm magic number.",
                             "snow Netpbm");
    }
    PnmInfo info;
    info.kind = magic.value()[1] - '0';
    info.ascii = info.kind <= 3;
    info.format = (info.kind == 1 || info.kind == 4)
                      ? Format::pbm
                      : ((info.kind == 2 || info.kind == 5) ? Format::pgm : Format::ppm);
    Result<std::string_view> width_token = tokenizer.next();
    Result<std::string_view> height_token = tokenizer.next();
    if (!width_token)
        return width_token.error();
    if (!height_token)
        return height_token.error();
    Result<std::uint32_t> width = parse_number(width_token.value(), "width");
    Result<std::uint32_t> height = parse_number(height_token.value(), "height");
    if (!width)
        return width.error();
    if (!height)
        return height.error();
    info.width = width.value();
    info.height = height.value();
    if (info.format != Format::pbm) {
        Result<std::string_view> maximum_token = tokenizer.next();
        if (!maximum_token)
            return maximum_token.error();
        Result<std::uint32_t> maximum = parse_number(maximum_token.value(), "maximum value");
        if (!maximum)
            return maximum.error();
        if (maximum.value() == 0 || maximum.value() > 65535U) {
            return Status::error(ErrorCode::unsupported_feature,
                                 "Netpbm maximum value must be between 1 and 65535.",
                                 "snow Netpbm");
        }
        info.maximum_value = maximum.value();
    }
    tokenizer.finish_header();
    info.pixel_offset = tokenizer.position();
    Result<void> dimensions = validate_dimensions(info.width, info.height, limits);
    if (!dimensions)
        return dimensions.error();
    const bool sixteen_bit = info.maximum_value > 255U;
    info.pixel_format =
        info.format == Format::ppm
            ? (sixteen_bit ? PixelFormat{SampleType::unsigned_integer, ChannelLayout::rgb,
                                         AlphaMode::none, 16, true}
                           : kRgb8)
            : (sixteen_bit ? kGray16 : kGray8);
    return info;
}

DocumentInfo document_info(const PnmInfo& info) {
    DocumentInfo document;
    document.format = info.format;
    document.canvas_width = info.width;
    document.canvas_height = info.height;
    document.frames.push_back({info.width,
                               info.height,
                               0,
                               0,
                               std::chrono::nanoseconds{0},
                               info.pixel_format,
                               false,
                               std::nullopt,
                               {},
                               {},
                               FrameBlend::source,
                               FrameDisposal::keep});
    return document;
}

std::uint16_t scaled_sample(std::uint32_t value, std::uint32_t maximum, std::uint32_t target) {
    return static_cast<std::uint16_t>((static_cast<std::uint64_t>(value) * target + maximum / 2U) /
                                      maximum);
}

Result<Image> decode_pixels(std::span<const std::byte> bytes, const PnmInfo& info,
                            std::stop_token stop) {
    Result<MutableImage> allocated =
        MutableImage::allocate(info.width, info.height, info.pixel_format);
    if (!allocated)
        return allocated.error();
    MutableImage image = std::move(allocated).value();
    const std::uint32_t channels = info.pixel_format.channel_count();
    const std::uint32_t sample_bytes = info.pixel_format.bits_per_channel / 8U;
    const std::uint64_t sample_count =
        static_cast<std::uint64_t>(info.width) * info.height * channels;

    if (info.ascii) {
        Tokenizer tokenizer(bytes.subspan(info.pixel_offset));
        for (std::uint64_t index = 0; index < sample_count; ++index) {
            if ((index % info.width) == 0 && stop.stop_requested())
                return cancelled_status();
            Result<std::string_view> token = tokenizer.next();
            if (!token)
                return token.error();
            Result<std::uint32_t> parsed = parse_number(token.value(), "sample");
            if (!parsed)
                return parsed.error();
            if (parsed.value() > info.maximum_value) {
                return Status::error(ErrorCode::corrupt_data,
                                     "Netpbm sample exceeds the maximum value.", "snow Netpbm");
            }
            const std::uint32_t logical =
                info.format == Format::pbm ? 1U - parsed.value() : parsed.value();
            const std::uint16_t scaled =
                scaled_sample(logical, info.maximum_value, sample_bytes == 1 ? 255U : 65535U);
            const std::size_t output = static_cast<std::size_t>(index) * sample_bytes;
            image.pixels()[output] = static_cast<std::byte>(scaled & 0xFFU);
            if (sample_bytes == 2)
                image.pixels()[output + 1U] = static_cast<std::byte>(scaled >> 8U);
        }
        return std::move(image).freeze();
    }

    if (info.format == Format::pbm) {
        const std::size_t packed_row = (static_cast<std::size_t>(info.width) + 7U) / 8U;
        const std::uint64_t required = static_cast<std::uint64_t>(packed_row) * info.height;
        if (info.pixel_offset + required > bytes.size()) {
            return Status::error(ErrorCode::truncated_data, "PBM pixel data is truncated.",
                                 "snow Netpbm");
        }
        for (std::uint32_t y = 0; y < info.height; ++y) {
            if (stop.stop_requested())
                return cancelled_status();
            for (std::uint32_t x = 0; x < info.width; ++x) {
                const std::uint8_t packed = std::to_integer<std::uint8_t>(
                    bytes[info.pixel_offset + static_cast<std::size_t>(y) * packed_row + x / 8U]);
                image.pixels()[static_cast<std::size_t>(y) * image.row_stride() + x] =
                    (packed & (0x80U >> (x % 8U))) != 0 ? std::byte{0} : std::byte{0xFF};
            }
        }
        return std::move(image).freeze();
    }

    const std::uint64_t required = sample_count * sample_bytes;
    if (info.pixel_offset + required > bytes.size()) {
        return Status::error(ErrorCode::truncated_data, "Netpbm pixel data is truncated.",
                             "snow Netpbm");
    }
    for (std::uint64_t index = 0; index < sample_count; ++index) {
        if ((index % (static_cast<std::uint64_t>(info.width) * channels)) == 0 &&
            stop.stop_requested())
            return cancelled_status();
        const std::size_t input =
            info.pixel_offset + static_cast<std::size_t>(index) * sample_bytes;
        const std::uint32_t raw =
            sample_bytes == 1
                ? std::to_integer<std::uint8_t>(bytes[input])
                : (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[input])) << 8U) |
                      std::to_integer<std::uint8_t>(bytes[input + 1U]);
        if (raw > info.maximum_value) {
            return Status::error(ErrorCode::corrupt_data,
                                 "Netpbm sample exceeds the maximum value.", "snow Netpbm");
        }
        const std::uint16_t scaled =
            scaled_sample(raw, info.maximum_value, sample_bytes == 1 ? 255U : 65535U);
        const std::size_t output = static_cast<std::size_t>(index) * sample_bytes;
        image.pixels()[output] = static_cast<std::byte>(scaled & 0xFFU);
        if (sample_bytes == 2)
            image.pixels()[output + 1U] = static_cast<std::byte>(scaled >> 8U);
    }
    return std::move(image).freeze();
}

Result<void> write_text(ByteSink& sink, std::string_view text) {
    return sink.write(std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace

CodecCapability PnmCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::streaming_decode | CodecCapability::metadata_decode;
}

int PnmCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 2 && header[0] == std::byte{'P'}) {
        const char kind = static_cast<char>(std::to_integer<unsigned char>(header[1]));
        const Format detected =
            (kind == '1' || kind == '4')
                ? Format::pbm
                : ((kind == '2' || kind == '5')
                       ? Format::pgm
                       : ((kind == '3' || kind == '6') ? Format::ppm : Format::unknown));
        if (detected == format_)
            return 100;
    }
    return format_from_extension(name_hint) == format_ ? 10 : 0;
}

Result<DocumentInfo> PnmCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<PnmInfo> info = parse_info(bytes.value(), options.limits);
    if (!info)
        return info.error();
    if (info.value().format != format_) {
        return Status::error(ErrorCode::corrupt_data, "Netpbm subtype does not match the codec.",
                             "snow Netpbm");
    }
    return document_info(info.value());
}

Result<Document> PnmCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    Result<PnmInfo> info = parse_info(bytes.value(), options.limits);
    if (!info)
        return info.error();
    if (info.value().format != format_) {
        return Status::error(ErrorCode::corrupt_data, "Netpbm subtype does not match the codec.",
                             "snow Netpbm");
    }
    const Result<std::size_t> pixel_bytes = info.value().pixel_format.bytes_per_pixel();
    if (!pixel_bytes)
        return pixel_bytes.error();
    const std::uint64_t output_size =
        static_cast<std::uint64_t>(info.value().width) * info.value().height * pixel_bytes.value();
    if (output_size > options.limits.maximum_owned_output_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "Netpbm output exceeds the owning decode limit.", "snow Netpbm");
    }
    Result<Image> pixels = decode_pixels(bytes.value(), info.value(), stop);
    if (!pixels)
        return pixels.error();
    Document document;
    document.format = info.value().format;
    document.canvas_width = info.value().width;
    document.canvas_height = info.value().height;
    Frame frame;
    frame.image = std::move(pixels).value();
    document.frames.push_back(std::move(frame));
    return document;
}

Result<EncodedArtifactReceipt> PnmCodec::encode_to_sink(const Document& document,
                                                        const Output& output, const EncodeOptions&,
                                                        std::stop_token stop) const {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "Netpbm encoding requires one frame.",
                             "snow Netpbm");
    }
    const ImageView view = document.frames.front().image.view();
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    if (view.format.sample_type != SampleType::unsigned_integer ||
        (view.format.bits_per_channel != 8 && view.format.bits_per_channel != 16)) {
        return Status::error(ErrorCode::unsupported_feature,
                             "Netpbm encoding requires 8-bit or 16-bit unsigned pixels.",
                             "snow Netpbm");
    }
    const bool source_gray = view.format.channels == ChannelLayout::gray;
    const bool source_rgb =
        view.format.channels == ChannelLayout::rgb || view.format.channels == ChannelLayout::rgba ||
        view.format.channels == ChannelLayout::bgr || view.format.channels == ChannelLayout::bgra;
    if (!source_gray && !source_rgb) {
        return Status::error(ErrorCode::unsupported_feature, "Unsupported Netpbm source layout.",
                             "snow Netpbm");
    }
    const int kind = format_ == Format::pbm ? 4 : (format_ == Format::pgm ? 5 : 6);
    const std::uint32_t maximum =
        format_ == Format::pbm ? 1U : (view.format.bits_per_channel == 16 ? 65535U : 255U);
    std::string header = "P" + std::to_string(kind) + "\n" + std::to_string(view.width) + " " +
                         std::to_string(view.height) + "\n";
    if (format_ != Format::pbm)
        header += std::to_string(maximum) + "\n";
    Result<void> status = write_text(*output.sink, header);
    if (!status)
        return status.error();

    const std::size_t source_channels = view.format.channel_count();
    const std::size_t source_sample_bytes = view.format.bits_per_channel / 8U;
    const std::size_t output_channels = format_ == Format::ppm ? 3U : 1U;
    const std::size_t output_sample_bytes = format_ == Format::pbm ? 0U : source_sample_bytes;
    const std::size_t output_row_bytes =
        format_ == Format::pbm
            ? (static_cast<std::size_t>(view.width) + 7U) / 8U
            : static_cast<std::size_t>(view.width) * output_channels * output_sample_bytes;
    snow::memory::PixelArray<std::byte> row(output_row_bytes);
    for (std::uint32_t y = 0; y < view.height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        std::fill(row.begin(), row.end(), std::byte{0});
        const auto source =
            view.pixels.subspan(static_cast<std::size_t>(y) * view.row_stride, view.row_stride);
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const std::size_t source_pixel =
                static_cast<std::size_t>(x) * source_channels * source_sample_bytes;
            const auto sample = [&](std::size_t channel) {
                const std::size_t offset = source_pixel + channel * source_sample_bytes;
                return source_sample_bytes == 1
                           ? static_cast<std::uint16_t>(
                                 std::to_integer<std::uint8_t>(source[offset]))
                           : static_cast<std::uint16_t>(
                                 std::to_integer<std::uint8_t>(source[offset]) |
                                 (std::to_integer<std::uint8_t>(source[offset + 1U]) << 8U));
            };
            const bool bgr = view.format.channels == ChannelLayout::bgr ||
                             view.format.channels == ChannelLayout::bgra;
            const std::uint16_t red = source_gray ? sample(0) : sample(bgr ? 2U : 0U);
            const std::uint16_t green = source_gray ? red : sample(1);
            const std::uint16_t blue = source_gray ? red : sample(bgr ? 0U : 2U);
            if (format_ == Format::pbm) {
                const std::uint32_t threshold = source_sample_bytes == 1 ? 128U : 32768U;
                const std::uint32_t luminance = (static_cast<std::uint32_t>(red) * 77U +
                                                 static_cast<std::uint32_t>(green) * 150U +
                                                 static_cast<std::uint32_t>(blue) * 29U) >>
                                                8U;
                if (luminance < threshold) {
                    row[x / 8U] |= static_cast<std::byte>(0x80U >> (x % 8U));
                }
                continue;
            }
            const std::array<std::uint16_t, 3> values{red, green, blue};
            for (std::size_t channel = 0; channel < output_channels; ++channel) {
                const std::uint16_t value =
                    output_channels == 1
                        ? static_cast<std::uint16_t>((static_cast<std::uint32_t>(red) * 77U +
                                                      static_cast<std::uint32_t>(green) * 150U +
                                                      static_cast<std::uint32_t>(blue) * 29U) >>
                                                     8U)
                        : values[channel];
                const std::size_t offset =
                    (static_cast<std::size_t>(x) * output_channels + channel) * output_sample_bytes;
                if (output_sample_bytes == 1) {
                    row[offset] = static_cast<std::byte>(value);
                } else {
                    row[offset] = static_cast<std::byte>(value >> 8U);
                    row[offset + 1U] = static_cast<std::byte>(value & 0xFFU);
                }
            }
        }
        status = output.sink->write(row);
        if (!status)
            return status.error();
    }
    return receipt_for_document(document, format());
}

} // namespace snow::image::internal
