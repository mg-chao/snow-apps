#include "codecs/svg_codec.h"

#include <lunasvg/lunasvg.h>
#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace snow::image::internal {
namespace {

Status zlib_error(const z_stream& stream, ErrorCode code, std::string_view operation) {
    std::string message(operation);
    message += " failed";
    if (stream.msg && *stream.msg) {
        message += ": ";
        message += stream.msg;
    }
    message += ".";
    return Status::error(code, std::move(message), "zlib");
}

Result<std::vector<std::byte>> gunzip(std::span<const std::byte> compressed,
                                      std::uint64_t maximum_output, std::stop_token stop) {
    if (compressed.size() > std::numeric_limits<uInt>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "SVGZ input exceeds zlib limits.", "zlib");
    }
    z_stream stream{};
    if (inflateInit2(&stream, MAX_WBITS + 16) != Z_OK) {
        return zlib_error(stream, ErrorCode::decode_failed, "SVGZ initialization");
    }
    const auto finish = [&stream]() { inflateEnd(&stream); };
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data()));
    stream.avail_in = static_cast<uInt>(compressed.size());
    std::vector<std::byte> output;
    constexpr std::size_t kChunk = 64U * 1024U;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stop.stop_requested()) {
            finish();
            return cancelled_status();
        }
        if (output.size() >= maximum_output) {
            finish();
            return Status::error(ErrorCode::limit_exceeded,
                                 "Decompressed SVGZ exceeds its byte limit.", "zlib");
        }
        const std::size_t available = static_cast<std::size_t>(
            std::min<std::uint64_t>(kChunk, maximum_output - output.size()));
        const std::size_t start = output.size();
        output.resize(start + available);
        stream.next_out = reinterpret_cast<Bytef*>(output.data() + start);
        stream.avail_out = static_cast<uInt>(available);
        status = inflate(&stream, Z_NO_FLUSH);
        const std::size_t produced = available - stream.avail_out;
        output.resize(start + produced);
        if (status != Z_OK && status != Z_STREAM_END) {
            Status error = zlib_error(stream, ErrorCode::corrupt_data, "SVGZ decompression");
            finish();
            return error;
        }
        if (produced == 0 && status != Z_STREAM_END && stream.avail_in == 0) {
            finish();
            return Status::error(ErrorCode::truncated_data, "SVGZ stream ended prematurely.",
                                 "zlib");
        }
    }
    finish();
    return output;
}

Result<std::vector<std::byte>> gzip(std::span<const std::byte> source, int level,
                                    std::stop_token stop) {
    if (source.size() > std::numeric_limits<uInt>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "SVG source exceeds zlib limits.", "zlib");
    }
    z_stream stream{};
    if (deflateInit2(&stream, std::clamp(level, 0, 9), Z_DEFLATED, MAX_WBITS + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return zlib_error(stream, ErrorCode::encode_failed, "SVGZ initialization");
    }
    const auto finish = [&stream]() { deflateEnd(&stream); };
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(source.data()));
    stream.avail_in = static_cast<uInt>(source.size());
    std::vector<std::byte> output;
    constexpr std::size_t kChunk = 64U * 1024U;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stop.stop_requested()) {
            finish();
            return cancelled_status();
        }
        const std::size_t start = output.size();
        output.resize(start + kChunk);
        stream.next_out = reinterpret_cast<Bytef*>(output.data() + start);
        stream.avail_out = static_cast<uInt>(kChunk);
        status = deflate(&stream, Z_FINISH);
        const std::size_t produced = kChunk - stream.avail_out;
        output.resize(start + produced);
        if (status != Z_OK && status != Z_STREAM_END) {
            Status error = zlib_error(stream, ErrorCode::encode_failed, "SVGZ compression");
            finish();
            return error;
        }
    }
    finish();
    return output;
}

struct SvgSource final {
    std::vector<std::byte> original;
    std::vector<std::byte> xml;
};

Result<SvgSource> read_svg(const Input& input, bool compressed, const DecodeOptions& options,
                           std::stop_token stop) {
    Result<std::vector<std::byte>> original =
        read_all(*input.source, options.limits.maximum_input_bytes);
    if (!original)
        return original.error();
    SvgSource source;
    source.original = std::move(original).value();
    if (compressed) {
        Result<std::vector<std::byte>> decompressed =
            gunzip(source.original, options.limits.maximum_input_bytes, stop);
        if (!decompressed)
            return decompressed.error();
        source.xml = std::move(decompressed).value();
    } else {
        source.xml = source.original;
    }
    if (source.xml.empty() ||
        std::find(source.xml.begin(), source.xml.end(), std::byte{0}) != source.xml.end()) {
        return Status::error(ErrorCode::corrupt_data, "SVG text is empty or contains a NUL byte.",
                             "lunasvg");
    }
    return source;
}

struct SvgLayout final {
    std::unique_ptr<lunasvg::Document> document;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

Result<SvgLayout> layout_svg(std::span<const std::byte> xml, const DecodeOptions& options) {
    auto document =
        lunasvg::Document::loadFromData(reinterpret_cast<const char*>(xml.data()), xml.size());
    if (!document) {
        return Status::error(ErrorCode::corrupt_data, "lunasvg could not parse the SVG document.",
                             "lunasvg");
    }
    const float intrinsic_width = document->width();
    const float intrinsic_height = document->height();
    double width = std::isfinite(intrinsic_width) && intrinsic_width > 0.0F
                       ? static_cast<double>(intrinsic_width)
                       : 300.0;
    double height = std::isfinite(intrinsic_height) && intrinsic_height > 0.0F
                        ? static_cast<double>(intrinsic_height)
                        : 150.0;
    if (options.maximum_extent && *options.maximum_extent != 0 &&
        (width > *options.maximum_extent || height > *options.maximum_extent)) {
        const double scale =
            std::min(*options.maximum_extent / width, *options.maximum_extent / height);
        width *= scale;
        height *= scale;
    }
    if (width > std::numeric_limits<std::uint32_t>::max() ||
        height > std::numeric_limits<std::uint32_t>::max()) {
        return Status::error(ErrorCode::limit_exceeded, "SVG intrinsic dimensions are too large.",
                             "lunasvg");
    }
    SvgLayout layout;
    layout.document = std::move(document);
    layout.width = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(width)));
    layout.height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::ceil(height)));
    Result<void> dimensions = validate_dimensions(layout.width, layout.height, options.limits);
    if (!dimensions)
        return dimensions.error();
    return layout;
}

DocumentInfo svg_info(Format format, std::uint32_t width, std::uint32_t height) {
    DocumentInfo info;
    info.format = format;
    info.canvas_width = width;
    info.canvas_height = height;
    info.is_vector = true;
    info.frames.push_back(
        {width, height, 0, 0, std::chrono::nanoseconds{0}, kRgba8, true, std::nullopt});
    return info;
}

} // namespace

CodecCapability SvgCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::metadata_decode | CodecCapability::vector;
}

int SvgCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (gzip_) {
        if (header.size() >= 2 && header[0] == std::byte{0x1F} && header[1] == std::byte{0x8B})
            return 80;
    } else {
        const std::string_view text(reinterpret_cast<const char*>(header.data()), header.size());
        if (text.find("<svg") != std::string_view::npos)
            return 90;
    }
    return format_from_extension(name_hint) == format() ? 10 : 0;
}

Result<DocumentInfo> SvgCodec::inspect(const Input& input, const DecodeOptions& options,
                                       std::stop_token stop) const {
    Result<SvgSource> source = read_svg(input, gzip_, options, stop);
    if (!source)
        return source.error();
    Result<SvgLayout> layout = layout_svg(source.value().xml, options);
    if (!layout)
        return layout.error();
    return svg_info(format(), layout.value().width, layout.value().height);
}

Result<Document> SvgCodec::decode(const Input& input, const DecodeOptions& options,
                                  std::stop_token stop) const {
    Result<SvgSource> source = read_svg(input, gzip_, options, stop);
    if (!source)
        return source.error();
    Result<SvgLayout> layout = layout_svg(source.value().xml, options);
    if (!layout)
        return layout.error();
    const std::uint64_t output_bytes =
        static_cast<std::uint64_t>(layout.value().width) * layout.value().height * 4U;
    if (output_bytes > options.limits.maximum_owned_output_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "SVG raster exceeds the owning decode limit.", "lunasvg");
    }
    if (stop.stop_requested())
        return cancelled_status();
    lunasvg::Bitmap bitmap = layout.value().document->renderToBitmap(
        static_cast<int>(layout.value().width), static_cast<int>(layout.value().height),
        0x00000000);
    if (bitmap.isNull() || bitmap.data() == nullptr || bitmap.stride() <= 0) {
        return Status::error(ErrorCode::decode_failed, "lunasvg could not rasterize the document.",
                             "lunasvg");
    }
    bitmap.convertToRGBA();
    Result<MutableImage> allocated =
        MutableImage::allocate(layout.value().width, layout.value().height, kRgba8);
    if (!allocated)
        return allocated.error();
    MutableImage pixels = std::move(allocated).value();
    for (std::uint32_t y = 0; y < pixels.height(); ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        std::memcpy(pixels.pixels().data() + static_cast<std::size_t>(y) * pixels.row_stride(),
                    bitmap.data() +
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(bitmap.stride()),
                    static_cast<std::size_t>(pixels.width()) * 4U);
    }
    Document document;
    document.format = format();
    document.canvas_width = pixels.width();
    document.canvas_height = pixels.height();
    document.vector = VectorDocument{std::move(source.value().original), gzip_};
    Frame frame;
    frame.image = std::move(pixels).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

Result<EncodedArtifactReceipt> SvgCodec::encode_to_sink(const Document& document,
                                                        const Output& output,
                                                        const EncodeOptions& options,
                                                        std::stop_token stop) const {
    if (!document.vector || document.vector->source.empty()) {
        return Status::error(
            ErrorCode::unsupported_feature,
            "SVG encoding requires preserved vector source; raster-to-vector is unsupported.",
            std::string(name()));
    }
    std::vector<std::byte> xml;
    if (document.vector->gzip_compressed) {
        Result<std::vector<std::byte>> decompressed =
            gunzip(document.vector->source, std::uint64_t{8} << 30U, stop);
        if (!decompressed)
            return decompressed.error();
        xml = std::move(decompressed).value();
    } else {
        xml = document.vector->source;
    }
    auto validated =
        lunasvg::Document::loadFromData(reinterpret_cast<const char*>(xml.data()), xml.size());
    if (!validated) {
        return Status::error(ErrorCode::corrupt_data, "Vector source is not a valid SVG document.",
                             "lunasvg");
    }
    std::vector<std::byte> encoded;
    if (gzip_) {
        Result<std::vector<std::byte>> compressed = gzip(xml, options.compression_level, stop);
        if (!compressed)
            return compressed.error();
        encoded = std::move(compressed).value();
    } else {
        encoded = std::move(xml);
    }
    Result<void> written = output.sink->write(encoded);
    if (!written)
        return written.error();
    return receipt_for_document(document, format());
}

} // namespace snow::image::internal
