#include "codecs/xbitmap_codec.h"

#include <snow/memory/pixel_array.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace snow::image::internal {
namespace {

Result<std::string> text_input(const Input& input, const DecodeOptions& options) {
    Result<std::vector<std::byte>> bytes =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!bytes)
        return bytes.error();
    if (std::find(bytes.value().begin(), bytes.value().end(), std::byte{0}) !=
        bytes.value().end()) {
        return Status::error(ErrorCode::corrupt_data, "Text image contains an embedded NUL byte.");
    }
    return std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
}

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

Result<std::uint32_t> unsigned_number(std::string_view value, std::string_view field,
                                      std::string_view codec) {
    value = trim(value);
    int base = 10;
    if (value.size() > 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
        value.remove_prefix(2);
        base = 16;
    }
    std::uint32_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result, base);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        return Status::error(ErrorCode::corrupt_data,
                             "Invalid " + std::string(field) + " in " + std::string(codec) + ".",
                             std::string(codec));
    }
    return result;
}

struct XbmData final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::optional<std::uint32_t> hot_x;
    std::optional<std::uint32_t> hot_y;
    bool short_elements = false;
    std::vector<std::uint16_t> values;
};

Result<XbmData> parse_xbm(std::string_view source, const DecodeOptions& options) {
    XbmData data;
    std::size_t line_start = 0;
    while (line_start < source.size()) {
        std::size_t line_end = source.find('\n', line_start);
        if (line_end == std::string_view::npos)
            line_end = source.size();
        std::string_view line = trim(source.substr(line_start, line_end - line_start));
        if (line.starts_with("#define")) {
            line.remove_prefix(7);
            line = trim(line);
            const std::size_t separator = line.find_first_of(" \t");
            if (separator != std::string_view::npos) {
                const std::string_view name = line.substr(0, separator);
                const std::string_view value = trim(line.substr(separator));
                const auto set_value = [&](std::string_view suffix,
                                           std::uint32_t* target) -> Result<void> {
                    if (!name.ends_with(suffix))
                        return {};
                    Result<std::uint32_t> parsed = unsigned_number(value, suffix, "XBM");
                    if (!parsed)
                        return parsed.error();
                    *target = parsed.value();
                    return {};
                };
                Result<void> status = set_value("_width", &data.width);
                if (!status)
                    return status.error();
                status = set_value("_height", &data.height);
                if (!status)
                    return status.error();
                if (name.ends_with("_x_hot")) {
                    Result<std::uint32_t> parsed = unsigned_number(value, "x hotspot", "XBM");
                    if (!parsed)
                        return parsed.error();
                    data.hot_x = parsed.value();
                }
                if (name.ends_with("_y_hot")) {
                    Result<std::uint32_t> parsed = unsigned_number(value, "y hotspot", "XBM");
                    if (!parsed)
                        return parsed.error();
                    data.hot_y = parsed.value();
                }
            }
        }
        line_start = line_end + (line_end < source.size() ? 1U : 0U);
    }
    Result<void> dimensions = validate_dimensions(data.width, data.height, options.limits);
    if (!dimensions)
        return dimensions.error();

    const std::size_t open = source.find('{');
    const std::size_t close = source.find('}', open == std::string_view::npos ? 0 : open + 1U);
    if (open == std::string_view::npos || close == std::string_view::npos) {
        return Status::error(ErrorCode::corrupt_data, "XBM pixel array is missing.", "snow XBM");
    }
    const std::string_view declaration = source.substr(0, open);
    data.short_elements = declaration.rfind("short") != std::string_view::npos;
    std::string_view values = source.substr(open + 1U, close - open - 1U);
    while (!values.empty()) {
        const std::size_t comma = values.find(',');
        std::string_view token = trim(values.substr(0, comma));
        if (!token.empty()) {
            Result<std::uint32_t> parsed = unsigned_number(token, "pixel value", "XBM");
            if (!parsed || parsed.value() > (data.short_elements ? 65535U : 255U)) {
                return parsed ? Status::error(ErrorCode::corrupt_data,
                                              "XBM pixel value is too large.", "snow XBM")
                              : parsed.error();
            }
            data.values.push_back(static_cast<std::uint16_t>(parsed.value()));
        }
        if (comma == std::string_view::npos)
            break;
        values.remove_prefix(comma + 1U);
    }
    const std::size_t row_elements = data.short_elements
                                         ? (static_cast<std::size_t>(data.width) + 15U) / 16U
                                         : (static_cast<std::size_t>(data.width) + 7U) / 8U;
    if (data.values.size() < row_elements * data.height) {
        return Status::error(ErrorCode::truncated_data, "XBM pixel array is truncated.",
                             "snow XBM");
    }
    return data;
}

DocumentInfo xbm_info(const XbmData& data) {
    DocumentInfo info;
    info.format = Format::xbm;
    info.canvas_width = data.width;
    info.canvas_height = data.height;
    std::optional<std::array<std::uint32_t, 2>> hotspot;
    if (data.hot_x && data.hot_y)
        hotspot = std::array<std::uint32_t, 2>{*data.hot_x, *data.hot_y};
    info.frames.push_back({data.width,
                           data.height,
                           0,
                           0,
                           std::chrono::nanoseconds{0},
                           kGray8,
                           false,
                           hotspot,
                           {},
                           {},
                           FrameBlend::source,
                           FrameDisposal::keep});
    return info;
}

struct Rgba final {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
};

struct XpmData final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t colors = 0;
    std::uint32_t chars_per_pixel = 0;
    std::optional<std::array<std::uint32_t, 2>> hotspot;
    std::vector<std::string> lines;
};

Result<std::vector<std::string>> quoted_strings(std::string_view source) {
    std::vector<std::string> strings;
    for (std::size_t position = 0; position < source.size();) {
        const std::size_t quote = source.find('"', position);
        if (quote == std::string_view::npos)
            break;
        std::string value;
        bool closed = false;
        for (position = quote + 1U; position < source.size(); ++position) {
            const char current = source[position];
            if (current == '"') {
                ++position;
                closed = true;
                break;
            }
            if (current == '\\' && position + 1U < source.size()) {
                const char escaped = source[++position];
                value.push_back(escaped == 'n' ? '\n' : (escaped == 't' ? '\t' : escaped));
            } else {
                value.push_back(current);
            }
        }
        if (!closed) {
            return Status::error(ErrorCode::truncated_data, "XPM string literal is truncated.",
                                 "snow XPM");
        }
        strings.push_back(std::move(value));
    }
    return strings;
}

Result<XpmData> parse_xpm(std::string_view source, const DecodeOptions& options) {
    Result<std::vector<std::string>> extracted = quoted_strings(source);
    if (!extracted)
        return extracted.error();
    std::vector<std::string> lines = std::move(extracted).value();
    if (lines.empty() && source.find("! XPM2") != std::string_view::npos) {
        std::size_t start = source.find('\n', source.find("! XPM2"));
        while (start != std::string_view::npos && start + 1U < source.size()) {
            ++start;
            std::size_t end = source.find('\n', start);
            if (end == std::string_view::npos)
                end = source.size();
            const std::string_view line = trim(source.substr(start, end - start));
            if (!line.empty())
                lines.emplace_back(line);
            start = end == source.size() ? std::string_view::npos : end;
        }
    }
    if (lines.empty()) {
        return Status::error(ErrorCode::corrupt_data, "XPM header string is missing.", "snow XPM");
    }
    XpmData data;
    data.lines = std::move(lines);
    std::istringstream header(data.lines.front());
    header >> data.width >> data.height >> data.colors >> data.chars_per_pixel;
    if (!header || data.colors == 0 || data.chars_per_pixel == 0 || data.chars_per_pixel > 16U ||
        data.colors > 1'000'000U) {
        return Status::error(ErrorCode::corrupt_data, "XPM header values are invalid.", "snow XPM");
    }
    std::uint32_t hot_x = 0;
    std::uint32_t hot_y = 0;
    if (header >> hot_x >> hot_y)
        data.hotspot = std::array<std::uint32_t, 2>{hot_x, hot_y};
    Result<void> dimensions = validate_dimensions(data.width, data.height, options.limits);
    if (!dimensions)
        return dimensions.error();
    const std::uint64_t required_lines = 1U + static_cast<std::uint64_t>(data.colors) + data.height;
    if (required_lines > data.lines.size()) {
        return Status::error(ErrorCode::truncated_data,
                             "XPM color table or pixel rows are truncated.", "snow XPM");
    }
    const std::uint64_t row_chars = static_cast<std::uint64_t>(data.width) * data.chars_per_pixel;
    if (row_chars > std::numeric_limits<std::size_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "XPM row is too large.", "snow XPM");
    }
    for (std::uint32_t y = 0; y < data.height; ++y) {
        if (data.lines[1U + data.colors + y].size() < row_chars) {
            return Status::error(ErrorCode::truncated_data, "XPM pixel row is truncated.",
                                 "snow XPM");
        }
    }
    return data;
}

std::optional<Rgba> named_color(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (value == "none")
        return Rgba{0, 0, 0, 0};
    if (value == "black")
        return Rgba{0, 0, 0, 255};
    if (value == "white")
        return Rgba{255, 255, 255, 255};
    if (value == "red")
        return Rgba{255, 0, 0, 255};
    if (value == "green")
        return Rgba{0, 128, 0, 255};
    if (value == "blue")
        return Rgba{0, 0, 255, 255};
    if (value == "yellow")
        return Rgba{255, 255, 0, 255};
    if (value == "gray" || value == "grey")
        return Rgba{128, 128, 128, 255};
    return std::nullopt;
}

Result<Rgba> parse_color(std::string value) {
    if (const auto named = named_color(value))
        return *named;
    if (value.empty() || value[0] != '#' ||
        (value.size() != 4 && value.size() != 7 && value.size() != 13)) {
        return Status::error(ErrorCode::unsupported_feature,
                             "XPM color must be None, a standard basic name, or an RGB hex value.",
                             "snow XPM");
    }
    const std::size_t digits = (value.size() - 1U) / 3U;
    std::array<std::uint8_t, 3> channels{};
    for (std::size_t index = 0; index < 3; ++index) {
        std::uint32_t parsed = 0;
        const char* begin = value.data() + 1U + index * digits;
        const auto result = std::from_chars(begin, begin + digits, parsed, 16);
        if (result.ec != std::errc{}) {
            return Status::error(ErrorCode::corrupt_data, "XPM hex color is invalid.", "snow XPM");
        }
        const std::uint32_t maximum = digits == 1 ? 15U : (digits == 2 ? 255U : 65535U);
        channels[index] = static_cast<std::uint8_t>((parsed * 255U + maximum / 2U) / maximum);
    }
    return Rgba{channels[0], channels[1], channels[2], 255};
}

Result<std::unordered_map<std::string, Rgba>> xpm_palette(const XpmData& data) {
    std::unordered_map<std::string, Rgba> palette;
    palette.reserve(data.colors);
    for (std::uint32_t index = 0; index < data.colors; ++index) {
        const std::string& line = data.lines[1U + index];
        if (line.size() < data.chars_per_pixel) {
            return Status::error(ErrorCode::truncated_data, "XPM color key is truncated.",
                                 "snow XPM");
        }
        const std::string key = line.substr(0, data.chars_per_pixel);
        std::istringstream fields(line.substr(data.chars_per_pixel));
        std::string field;
        std::string color;
        while (fields >> field) {
            std::string value;
            if (!(fields >> value))
                break;
            if (field == "c" ||
                (color.empty() && (field == "g" || field == "g4" || field == "m"))) {
                color = value;
                if (field == "c")
                    break;
            }
        }
        if (color.empty()) {
            return Status::error(ErrorCode::corrupt_data, "XPM color entry has no visual color.",
                                 "snow XPM");
        }
        Result<Rgba> parsed = parse_color(color);
        if (!parsed)
            return parsed.error();
        if (!palette.emplace(key, parsed.value()).second) {
            return Status::error(ErrorCode::corrupt_data, "XPM contains a duplicate color key.",
                                 "snow XPM");
        }
    }
    return palette;
}

DocumentInfo xpm_info(const XpmData& data) {
    DocumentInfo info;
    info.format = Format::xpm;
    info.canvas_width = data.width;
    info.canvas_height = data.height;
    info.frames.push_back({data.width,
                           data.height,
                           0,
                           0,
                           std::chrono::nanoseconds{0},
                           kRgba8,
                           true,
                           data.hotspot,
                           {},
                           {},
                           FrameBlend::source,
                           FrameDisposal::keep});
    return info;
}

Result<void> write_text(ByteSink& sink, std::string_view value) {
    return sink.write(std::as_bytes(std::span(value.data(), value.size())));
}

std::uint32_t rgba_key(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
                       std::uint8_t alpha) {
    return static_cast<std::uint32_t>(red) << 24U | static_cast<std::uint32_t>(green) << 16U |
           static_cast<std::uint32_t>(blue) << 8U | alpha;
}

} // namespace

CodecCapability XbmCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::metadata_decode;
}

int XbmCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    const std::string_view text(reinterpret_cast<const char*>(header.data()), header.size());
    if (text.find("#define") != std::string_view::npos &&
        text.find("_width") != std::string_view::npos &&
        text.find("_height") != std::string_view::npos)
        return 90;
    return format_from_extension(name_hint) == Format::xbm ? 10 : 0;
}

Result<DocumentInfo> XbmCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::string> source = text_input(input, options);
    if (!source)
        return source.error();
    Result<XbmData> parsed = parse_xbm(source.value(), options);
    if (!parsed)
        return parsed.error();
    return xbm_info(parsed.value());
}

Result<Document> XbmCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    Result<std::string> source = text_input(input, options);
    if (!source)
        return source.error();
    Result<XbmData> parsed = parse_xbm(source.value(), options);
    if (!parsed)
        return parsed.error();
    const XbmData& data = parsed.value();
    const std::uint64_t output_size = static_cast<std::uint64_t>(data.width) * data.height;
    if (output_size > options.limits.maximum_owned_output_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "XBM output exceeds the owning decode limit.", "snow XBM");
    }
    Result<MutableImage> allocated = MutableImage::allocate(data.width, data.height, kGray8);
    if (!allocated)
        return allocated.error();
    MutableImage image = std::move(allocated).value();
    const std::size_t element_bits = data.short_elements ? 16U : 8U;
    const std::size_t row_elements =
        (static_cast<std::size_t>(data.width) + element_bits - 1U) / element_bits;
    for (std::uint32_t y = 0; y < data.height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        for (std::uint32_t x = 0; x < data.width; ++x) {
            const std::uint16_t packed =
                data.values[static_cast<std::size_t>(y) * row_elements + x / element_bits];
            image.pixels()[static_cast<std::size_t>(y) * image.row_stride() + x] =
                (packed & (1U << (x % element_bits))) != 0 ? std::byte{0} : std::byte{0xFF};
        }
    }
    Document document;
    document.format = Format::xbm;
    document.canvas_width = data.width;
    document.canvas_height = data.height;
    Frame frame;
    frame.image = std::move(image).freeze();
    if (data.hot_x && data.hot_y)
        frame.cursor_hotspot = std::array<std::uint32_t, 2>{*data.hot_x, *data.hot_y};
    document.frames.push_back(std::move(frame));
    return document;
}

Result<EncodedArtifactReceipt> XbmCodec::encode_to_sink(const Document& document,
                                                        const Output& output, const EncodeOptions&,
                                                        std::stop_token stop) const {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "XBM encoding requires one frame.",
                             "snow XBM");
    }
    const Frame& frame = document.frames.front();
    const ImageView view = frame.image.view();
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    if (view.format.sample_type != SampleType::unsigned_integer ||
        view.format.bits_per_channel != 8) {
        return Status::error(ErrorCode::unsupported_feature, "XBM encoding requires 8-bit pixels.",
                             "snow XBM");
    }
    std::ostringstream text;
    text << "#define snow_image_width " << view.width << "\n"
         << "#define snow_image_height " << view.height << "\n";
    if (frame.cursor_hotspot) {
        text << "#define snow_image_x_hot " << (*frame.cursor_hotspot)[0] << "\n"
             << "#define snow_image_y_hot " << (*frame.cursor_hotspot)[1] << "\n";
    }
    text << "static const unsigned char snow_image_bits[] = {\n  ";
    const std::size_t channels = view.format.channel_count();
    const bool bgr =
        view.format.channels == ChannelLayout::bgr || view.format.channels == ChannelLayout::bgra;
    std::size_t emitted = 0;
    for (std::uint32_t y = 0; y < view.height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        const auto row =
            view.pixels.subspan(static_cast<std::size_t>(y) * view.row_stride, view.row_stride);
        for (std::uint32_t byte_x = 0; byte_x < (view.width + 7U) / 8U; ++byte_x) {
            std::uint8_t packed = 0;
            for (std::uint32_t bit = 0; bit < 8U; ++bit) {
                const std::uint32_t x = byte_x * 8U + bit;
                if (x >= view.width)
                    break;
                const std::size_t offset = static_cast<std::size_t>(x) * channels;
                const std::uint8_t red =
                    std::to_integer<std::uint8_t>(row[offset + (bgr ? 2U : 0U)]);
                const std::uint8_t green =
                    channels == 1U ? red : std::to_integer<std::uint8_t>(row[offset + 1U]);
                const std::uint8_t blue =
                    channels == 1U ? red
                                   : std::to_integer<std::uint8_t>(row[offset + (bgr ? 0U : 2U)]);
                const std::uint32_t luminance = red * 77U + green * 150U + blue * 29U;
                if (luminance < 128U * 256U)
                    packed |= static_cast<std::uint8_t>(1U << bit);
            }
            if (emitted != 0)
                text << ", ";
            text << "0x" << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned int>(packed) << std::dec;
            if (++emitted % 12U == 0U)
                text << "\n  ";
        }
    }
    text << "\n};\n";
    Result<void> written = write_text(*output.sink, text.str());
    if (!written)
        return written.error();
    return receipt_for_document(document, format());
}

CodecCapability XpmCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::metadata_decode;
}

int XpmCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    const std::string_view text(reinterpret_cast<const char*>(header.data()), header.size());
    if (text.find("XPM") != std::string_view::npos &&
        (text.find("/* XPM */") != std::string_view::npos ||
         text.find("! XPM2") != std::string_view::npos))
        return 90;
    return format_from_extension(name_hint) == Format::xpm ? 10 : 0;
}

Result<DocumentInfo> XpmCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    if (stop.stop_requested())
        return cancelled_status();
    Result<std::string> source = text_input(input, options);
    if (!source)
        return source.error();
    Result<XpmData> parsed = parse_xpm(source.value(), options);
    if (!parsed)
        return parsed.error();
    return xpm_info(parsed.value());
}

Result<Document> XpmCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    Result<std::string> source = text_input(input, options);
    if (!source)
        return source.error();
    Result<XpmData> parsed = parse_xpm(source.value(), options);
    if (!parsed)
        return parsed.error();
    Result<std::unordered_map<std::string, Rgba>> palette = xpm_palette(parsed.value());
    if (!palette)
        return palette.error();
    const std::uint64_t output_size =
        static_cast<std::uint64_t>(parsed.value().width) * parsed.value().height * 4U;
    if (output_size > options.limits.maximum_owned_output_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "XPM output exceeds the owning decode limit.", "snow XPM");
    }
    Result<MutableImage> allocated =
        MutableImage::allocate(parsed.value().width, parsed.value().height, kRgba8);
    if (!allocated)
        return allocated.error();
    MutableImage image = std::move(allocated).value();
    for (std::uint32_t y = 0; y < parsed.value().height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        const std::string& row = parsed.value().lines[1U + parsed.value().colors + y];
        for (std::uint32_t x = 0; x < parsed.value().width; ++x) {
            const std::string key =
                row.substr(static_cast<std::size_t>(x) * parsed.value().chars_per_pixel,
                           parsed.value().chars_per_pixel);
            const auto color = palette.value().find(key);
            if (color == palette.value().end()) {
                return Status::error(ErrorCode::corrupt_data,
                                     "XPM pixel uses an unknown color key.", "snow XPM");
            }
            const std::size_t offset =
                static_cast<std::size_t>(y) * image.row_stride() + static_cast<std::size_t>(x) * 4U;
            image.pixels()[offset] = static_cast<std::byte>(color->second.red);
            image.pixels()[offset + 1U] = static_cast<std::byte>(color->second.green);
            image.pixels()[offset + 2U] = static_cast<std::byte>(color->second.blue);
            image.pixels()[offset + 3U] = static_cast<std::byte>(color->second.alpha);
        }
    }
    Document document;
    document.format = Format::xpm;
    document.canvas_width = parsed.value().width;
    document.canvas_height = parsed.value().height;
    Frame frame;
    frame.image = std::move(image).freeze();
    frame.cursor_hotspot = parsed.value().hotspot;
    document.frames.push_back(std::move(frame));
    return document;
}

Result<EncodedArtifactReceipt> XpmCodec::encode_to_sink(const Document& document,
                                                        const Output& output, const EncodeOptions&,
                                                        std::stop_token stop) const {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "XPM encoding requires one frame.",
                             "snow XPM");
    }
    const ImageView view = document.frames.front().image.view();
    Result<void> valid = view.validate();
    if (!valid)
        return valid.error();
    if (view.format.sample_type != SampleType::unsigned_integer ||
        view.format.bits_per_channel != 8 ||
        (view.format.channels != ChannelLayout::gray &&
         view.format.channels != ChannelLayout::rgb &&
         view.format.channels != ChannelLayout::rgba &&
         view.format.channels != ChannelLayout::bgr &&
         view.format.channels != ChannelLayout::bgra)) {
        return Status::error(ErrorCode::unsupported_feature,
                             "XPM encoding requires packed 8-bit gray or RGB-family pixels.",
                             "snow XPM");
    }
    std::map<std::uint32_t, std::size_t> colors;
    snow::memory::PixelArray<std::uint32_t> pixels(static_cast<std::size_t>(view.width) *
                                                   view.height);
    const std::size_t channels = view.format.channel_count();
    const bool bgr =
        view.format.channels == ChannelLayout::bgr || view.format.channels == ChannelLayout::bgra;
    for (std::uint32_t y = 0; y < view.height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        const auto row =
            view.pixels.subspan(static_cast<std::size_t>(y) * view.row_stride, view.row_stride);
        for (std::uint32_t x = 0; x < view.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * channels;
            const std::uint8_t red = std::to_integer<std::uint8_t>(row[offset + (bgr ? 2U : 0U)]);
            const std::uint8_t green =
                channels == 1U ? red : std::to_integer<std::uint8_t>(row[offset + 1U]);
            const std::uint8_t blue =
                channels == 1U ? red : std::to_integer<std::uint8_t>(row[offset + (bgr ? 0U : 2U)]);
            const std::uint8_t alpha =
                channels == 4U && std::to_integer<std::uint8_t>(row[offset + 3U]) < 128U ? 0U
                                                                                         : 255U;
            const std::uint32_t key = rgba_key(red, green, blue, alpha);
            pixels[static_cast<std::size_t>(y) * view.width + x] = key;
            colors.emplace(key, 0);
            if (colors.size() > 65536U) {
                return Status::error(ErrorCode::limit_exceeded,
                                     "XPM source has more than 65536 colors; quantize it first.",
                                     "snow XPM");
            }
        }
    }
    constexpr std::string_view alphabet =
        ".+@#$%&*=-;>,')!~{]^/"
        "(_:<[}|0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::size_t chars_per_pixel = 1;
    std::size_t capacity = alphabet.size();
    while (capacity < colors.size()) {
        ++chars_per_pixel;
        if (capacity > std::numeric_limits<std::size_t>::max() / alphabet.size()) {
            return Status::error(ErrorCode::limit_exceeded, "XPM palette key space overflowed.",
                                 "snow XPM");
        }
        capacity *= alphabet.size();
    }
    const auto key_for = [&](std::size_t index) {
        std::string key(chars_per_pixel, alphabet.front());
        for (std::size_t position = 0; position < chars_per_pixel; ++position) {
            key[chars_per_pixel - 1U - position] = alphabet[index % alphabet.size()];
            index /= alphabet.size();
        }
        return key;
    };
    std::size_t color_index = 0;
    for (auto& entry : colors)
        entry.second = color_index++;

    std::ostringstream text;
    text << "/* XPM */\nstatic const char *snow_image_xpm[] = {\n\"" << view.width << " "
         << view.height << " " << colors.size() << " " << chars_per_pixel << "\",\n";
    for (const auto& [color, index] : colors) {
        const std::uint8_t red = static_cast<std::uint8_t>(color >> 24U);
        const std::uint8_t green = static_cast<std::uint8_t>(color >> 16U);
        const std::uint8_t blue = static_cast<std::uint8_t>(color >> 8U);
        const std::uint8_t alpha = static_cast<std::uint8_t>(color);
        text << '"' << key_for(index) << " c ";
        if (alpha == 0)
            text << "None";
        else
            text << '#' << std::hex << std::setw(2) << std::setfill('0')
                 << static_cast<unsigned int>(red) << std::setw(2)
                 << static_cast<unsigned int>(green) << std::setw(2)
                 << static_cast<unsigned int>(blue) << std::dec;
        text << "\",\n";
    }
    for (std::uint32_t y = 0; y < view.height; ++y) {
        text << '"';
        for (std::uint32_t x = 0; x < view.width; ++x) {
            text << key_for(colors[pixels[static_cast<std::size_t>(y) * view.width + x]]);
        }
        text << '"' << (y + 1U == view.height ? "\n" : ",\n");
    }
    text << "};\n";
    Result<void> written = write_text(*output.sink, text.str());
    if (!written)
        return written.error();
    return receipt_for_document(document, format());
}

} // namespace snow::image::internal
