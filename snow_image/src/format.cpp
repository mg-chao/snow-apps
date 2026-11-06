#include "snow/image/format.h"

#include "snow/image/result.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>

namespace snow::image {
namespace {

constexpr std::array<std::string_view, 1> kBmpExtensions{"bmp"};
constexpr std::array<std::string_view, 1> kCurExtensions{"cur"};
constexpr std::array<std::string_view, 1> kGifExtensions{"gif"};
constexpr std::array<std::string_view, 1> kIcoExtensions{"ico"};
constexpr std::array<std::string_view, 3> kJpegExtensions{"jfif", "jpeg", "jpg"};
constexpr std::array<std::string_view, 1> kPbmExtensions{"pbm"};
constexpr std::array<std::string_view, 1> kPgmExtensions{"pgm"};
constexpr std::array<std::string_view, 1> kPngExtensions{"png"};
constexpr std::array<std::string_view, 1> kPpmExtensions{"ppm"};
constexpr std::array<std::string_view, 1> kSvgExtensions{"svg"};
constexpr std::array<std::string_view, 1> kSvgzExtensions{"svgz"};
constexpr std::array<std::string_view, 1> kXbmExtensions{"xbm"};
constexpr std::array<std::string_view, 1> kXpmExtensions{"xpm"};
constexpr std::array<std::string_view, 3> kHeifExtensions{"heic", "heif", "hif"};
constexpr std::array<std::string_view, 1> kAvifExtensions{"avif"};
constexpr std::array<std::string_view, 1> kJxlExtensions{"jxl"};
constexpr std::array<std::string_view, 1> kExrExtensions{"exr"};
constexpr std::array<std::string_view, 1> kWebpExtensions{"webp"};
constexpr std::array<std::string_view, 2> kTiffExtensions{"tif", "tiff"};

std::string normalized_extension(std::string_view extension) {
    const std::size_t separator = extension.find_last_of("/\\");
    if (separator != std::string_view::npos) {
        extension.remove_prefix(separator + 1);
    }
    const std::size_t dot = extension.find_last_of('.');
    if (dot != std::string_view::npos) {
        extension.remove_prefix(dot + 1);
    }
    std::string result(extension);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return result;
}

} // namespace

std::string_view error_code_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::ok:
        return "ok";
    case ErrorCode::cancelled:
        return "cancelled";
    case ErrorCode::invalid_argument:
        return "invalid_argument";
    case ErrorCode::io_error:
        return "io_error";
    case ErrorCode::truncated_data:
        return "truncated_data";
    case ErrorCode::corrupt_data:
        return "corrupt_data";
    case ErrorCode::unsupported_format:
        return "unsupported_format";
    case ErrorCode::unsupported_feature:
        return "unsupported_feature";
    case ErrorCode::codec_unavailable:
        return "codec_unavailable";
    case ErrorCode::limit_exceeded:
        return "limit_exceeded";
    case ErrorCode::out_of_memory:
        return "out_of_memory";
    case ErrorCode::decode_failed:
        return "decode_failed";
    case ErrorCode::encode_failed:
        return "encode_failed";
    case ErrorCode::internal_error:
        return "internal_error";
    }
    return "unknown";
}

std::string_view format_name(Format format) noexcept {
    switch (format) {
    case Format::bmp:
        return "BMP";
    case Format::cur:
        return "CUR";
    case Format::gif:
        return "GIF";
    case Format::ico:
        return "ICO";
    case Format::jpeg:
        return "JPEG";
    case Format::pbm:
        return "PBM";
    case Format::pgm:
        return "PGM";
    case Format::png:
        return "PNG";
    case Format::ppm:
        return "PPM";
    case Format::svg:
        return "SVG";
    case Format::svgz:
        return "SVGZ";
    case Format::xbm:
        return "XBM";
    case Format::xpm:
        return "XPM";
    case Format::heif:
        return "HEIF";
    case Format::avif:
        return "AVIF";
    case Format::jxl:
        return "JPEG XL";
    case Format::exr:
        return "OpenEXR";
    case Format::webp:
        return "WebP";
    case Format::tiff:
        return "TIFF";
    case Format::unknown:
        return "Unknown";
    }
    return "Unknown";
}

std::span<const std::string_view> format_extensions(Format format) noexcept {
    switch (format) {
    case Format::bmp:
        return kBmpExtensions;
    case Format::cur:
        return kCurExtensions;
    case Format::gif:
        return kGifExtensions;
    case Format::ico:
        return kIcoExtensions;
    case Format::jpeg:
        return kJpegExtensions;
    case Format::pbm:
        return kPbmExtensions;
    case Format::pgm:
        return kPgmExtensions;
    case Format::png:
        return kPngExtensions;
    case Format::ppm:
        return kPpmExtensions;
    case Format::svg:
        return kSvgExtensions;
    case Format::svgz:
        return kSvgzExtensions;
    case Format::xbm:
        return kXbmExtensions;
    case Format::xpm:
        return kXpmExtensions;
    case Format::heif:
        return kHeifExtensions;
    case Format::avif:
        return kAvifExtensions;
    case Format::jxl:
        return kJxlExtensions;
    case Format::exr:
        return kExrExtensions;
    case Format::webp:
        return kWebpExtensions;
    case Format::tiff:
        return kTiffExtensions;
    case Format::unknown:
        return {};
    }
    return {};
}

Format format_from_extension(std::string_view extension) noexcept {
    try {
        const std::string normalized = normalized_extension(extension);
        constexpr std::array<Format, 19> formats{
            Format::bmp,  Format::cur, Format::gif,  Format::ico,  Format::jpeg,
            Format::pbm,  Format::pgm, Format::png,  Format::ppm,  Format::svg,
            Format::svgz, Format::xbm, Format::xpm,  Format::heif, Format::avif,
            Format::jxl,  Format::exr, Format::webp, Format::tiff};
        for (Format format : formats) {
            for (std::string_view candidate : format_extensions(format)) {
                if (candidate == normalized) {
                    return format;
                }
            }
        }
    } catch (...) {
        return Format::unknown;
    }
    return Format::unknown;
}

} // namespace snow::image
