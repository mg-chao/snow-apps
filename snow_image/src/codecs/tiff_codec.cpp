#include "codecs/tiff_codec.h"

#include "exif_orientation.h"

#include <tiffio.h>
#include <lcms2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <vector>

namespace snow::image::internal {
namespace {

constexpr std::uint64_t kClassicLimit = std::numeric_limits<std::uint32_t>::max();

Status tiff_error(ErrorCode code, const char* message) {
    return Status::error(code, message, "LibTIFF");
}

// LibTIFF owns neither endpoint. Its offsets are relative to the beginning of
// this artifact, even when the caller has already written a prefix to the sink.
struct TiffIo final {
    const ByteSource* source = nullptr;
    ByteSink* sink = nullptr;
    std::stop_token stop;
    std::uint64_t origin = 0;
    std::uint64_t position = 0;
    std::uint64_t size = 0;
    ErrorCode failure = ErrorCode::ok;
    std::array<char, 512> message{};

    bool ready() noexcept {
        if (stop.stop_requested())
            failure = ErrorCode::cancelled;
        return failure == ErrorCode::ok;
    }
    Status error(ErrorCode fallback, const char* description) const {
        return tiff_error(failure == ErrorCode::ok ? fallback : failure,
                          message.front() == '\0' ? description : message.data());
    }
};

tmsize_t read_tiff(thandle_t opaque, void* destination, tmsize_t count) noexcept {
    auto& io = *static_cast<TiffIo*>(opaque);
    try {
        if (!io.ready() || !io.source || count < 0)
            return -1;
        const auto available = io.position < io.size ? io.size - io.position : 0;
        const auto requested = static_cast<std::size_t>(
            std::min<std::uint64_t>(static_cast<std::uint64_t>(count), available));
        auto bytes = std::span(static_cast<std::byte*>(destination), requested);
        std::size_t completed = 0;
        while (completed < requested) {
            if (!io.ready())
                return -1;
            auto read = io.source->read_at(io.position, bytes.subspan(completed));
            if (!read || read.value() == 0 || read.value() > requested - completed) {
                io.failure = read ? ErrorCode::truncated_data : read.error().code;
                return -1;
            }
            completed += read.value();
            io.position += read.value();
        }
        return static_cast<tmsize_t>(completed);
    } catch (...) {
        io.failure = ErrorCode::io_error;
        return -1;
    }
}

tmsize_t write_tiff(thandle_t opaque, void* source, tmsize_t count) noexcept {
    auto& io = *static_cast<TiffIo*>(opaque);
    try {
        if (!io.ready() || !io.sink || count < 0)
            return -1;
        const auto length = static_cast<std::uint64_t>(count);
        if (length > kClassicLimit - io.position) {
            io.failure = ErrorCode::limit_exceeded;
            return -1;
        }
        auto written = io.sink->write(
            std::span(static_cast<const std::byte*>(source), static_cast<std::size_t>(count)));
        if (!written) {
            io.failure = written.error().code;
            return -1;
        }
        io.position += length;
        io.size = std::max(io.size, io.position);
        return count;
    } catch (...) {
        io.failure = ErrorCode::io_error;
        return -1;
    }
}

toff_t seek_tiff(thandle_t opaque, toff_t offset, int whence) noexcept {
    auto& io = *static_cast<TiffIo*>(opaque);
    try {
        if (!io.ready())
            return static_cast<toff_t>(-1);
        const std::uint64_t base = whence == SEEK_SET   ? 0
                                   : whence == SEEK_CUR ? io.position
                                                        : io.size;
        if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
            io.failure = ErrorCode::io_error;
            return static_cast<toff_t>(-1);
        }
        std::uint64_t target = 0;
        // LibTIFF passes negative relative offsets through its unsigned type.
        if (whence != SEEK_SET &&
            offset > static_cast<toff_t>(std::numeric_limits<std::int64_t>::max())) {
            const std::uint64_t magnitude = std::uint64_t{0} - offset;
            if (magnitude > base) {
                io.failure = ErrorCode::io_error;
                return static_cast<toff_t>(-1);
            }
            target = base - magnitude;
        } else {
            if (offset > std::numeric_limits<std::uint64_t>::max() - base) {
                io.failure = ErrorCode::limit_exceeded;
                return static_cast<toff_t>(-1);
            }
            target = base + offset;
        }
        if (io.sink) {
            if (target > kClassicLimit ||
                target > std::numeric_limits<std::uint64_t>::max() - io.origin) {
                io.failure = ErrorCode::limit_exceeded;
                return static_cast<toff_t>(-1);
            }
            auto sought = io.sink->seek(io.origin + target);
            if (!sought) {
                io.failure = sought.error().code;
                return static_cast<toff_t>(-1);
            }
        } else if (target > io.size) {
            io.failure = ErrorCode::truncated_data;
            return static_cast<toff_t>(-1);
        }
        io.position = target;
        return target;
    } catch (...) {
        io.failure = ErrorCode::io_error;
        return static_cast<toff_t>(-1);
    }
}

int close_tiff(thandle_t) noexcept {
    return 0;
}
toff_t size_tiff(thandle_t opaque) noexcept {
    return static_cast<TiffIo*>(opaque)->size;
}
int map_tiff(thandle_t, void**, toff_t*) noexcept {
    return 0;
}
void unmap_tiff(thandle_t, void*, toff_t) noexcept {}

TIFF_ATTRIBUTE((__format__(__printf__, 4, 0)))
int error_tiff(TIFF*, void* opaque, const char*, const char* format, va_list arguments) noexcept {
    auto& io = *static_cast<TiffIo*>(opaque);
    if (io.message.front() == '\0')
        (void)std::vsnprintf(io.message.data(), io.message.size(), format, arguments);
    if (io.failure == ErrorCode::ok &&
        (std::strstr(io.message.data(), "limit") || std::strstr(io.message.data(), "Limit") ||
         std::strstr(io.message.data(), "Maximum")))
        io.failure = ErrorCode::limit_exceeded;
    return 1;
}
int warning_tiff(TIFF*, void*, const char*, const char*, va_list) noexcept {
    return 1;
}

struct TiffCloser final {
    void operator()(TIFF* handle) const noexcept {
        TIFFClose(handle);
    }
};
struct TiffSession final {
    TiffIo io;
    std::unique_ptr<TIFF, TiffCloser> handle;
};

Result<std::unique_ptr<TiffSession>> open_tiff(const Input* input, const Output* output,
                                               const DecodeLimits& limits, std::stop_token stop) {
    if (stop.stop_requested())
        return cancelled_status();
    auto session = std::make_unique<TiffSession>();
    session->io.stop = stop;
    if (input) {
        if (!input->source)
            return tiff_error(ErrorCode::invalid_argument, "TIFF input has no byte source.");
        auto size = input->source->size();
        if (!size)
            return size.error();
        if (size.value() > limits.maximum_input_bytes)
            return tiff_error(ErrorCode::limit_exceeded, "TIFF input exceeds the byte limit.");
        session->io.source = input->source.get();
        session->io.size = size.value();
    } else {
        if (!output || !output->sink)
            return tiff_error(ErrorCode::invalid_argument, "TIFF output has no byte sink.");
        if (!output->sink->seekable())
            return tiff_error(ErrorCode::unsupported_feature,
                              "TIFF encoding requires a seekable output.");
        auto origin = output->sink->position();
        if (!origin)
            return origin.error();
        session->io.sink = output->sink.get();
        session->io.origin = origin.value();
    }
    if (limits.maximum_working_bytes < 2)
        return tiff_error(ErrorCode::limit_exceeded, "TIFF working-memory limit is empty.");
    auto* options = TIFFOpenOptionsAlloc();
    if (!options)
        return tiff_error(ErrorCode::out_of_memory, "Could not allocate TIFF open options.");
    const auto memory_limit = static_cast<tmsize_t>(
        std::min<std::uint64_t>(limits.maximum_working_bytes / 2U,
                                static_cast<std::uint64_t>(std::numeric_limits<tmsize_t>::max())));
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, memory_limit);
    TIFFOpenOptionsSetMaxCumulatedMemAlloc(options, memory_limit);
    TIFFOpenOptionsSetErrorHandlerExtR(options, error_tiff, &session->io);
    TIFFOpenOptionsSetWarningHandlerExtR(options, warning_tiff, &session->io);
    session->handle.reset(TIFFClientOpenExt("Snow Image TIFF", input ? "rm" : "wl", &session->io,
                                            read_tiff, write_tiff, seek_tiff, close_tiff, size_tiff,
                                            map_tiff, unmap_tiff, options));
    TIFFOpenOptionsFree(options);
    if (!session->handle || !session->io.ready())
        return session->io.error(input ? ErrorCode::corrupt_data : ErrorCode::encode_failed,
                                 "Could not open the TIFF image.");
    return session;
}

struct TiffInfo final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint16_t samples = 0;
    bool direct = false;
    bool alpha = false;
    bool associated_alpha = false;
    Orientation orientation = Orientation::identity;
    Metadata metadata;
    ColorEncoding color;
};

Result<TiffInfo> read_info(TiffSession& session, const DecodeOptions& options) {
    if (options.frame_index && *options.frame_index != 0)
        return tiff_error(ErrorCode::invalid_argument,
                          "TIFF import supports only the first image.");
    if (options.limits.maximum_frames == 0)
        return tiff_error(ErrorCode::limit_exceeded, "TIFF frame limit is empty.");
    if (options.output_format && *options.output_format != kRgba8 &&
        *options.output_format != kBgra8)
        return tiff_error(ErrorCode::unsupported_feature,
                          "TIFF decoding supports RGBA8 and BGRA8 output.");
    TIFF* tif = session.handle.get();
    TiffInfo info;
    if (TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &info.width) != 1 ||
        TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &info.height) != 1)
        return tiff_error(ErrorCode::corrupt_data, "TIFF image dimensions are missing.");
    auto dimensions = validate_dimensions(info.width, info.height, options.limits);
    if (!dimensions)
        return dimensions.error();
    std::uint16_t bits = 0;
    std::uint16_t photometric = 0;
    std::uint16_t planar = 0;
    std::uint16_t sample_format = 0;
    std::uint16_t orientation = ORIENTATION_TOPLEFT;
    (void)TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
    (void)TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &info.samples);
    (void)TIFFGetFieldDefaulted(tif, TIFFTAG_PLANARCONFIG, &planar);
    (void)TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &sample_format);
    (void)TIFFGetFieldDefaulted(tif, TIFFTAG_ORIENTATION, &orientation);
    if (TIFFGetField(tif, TIFFTAG_PHOTOMETRIC, &photometric) != 1)
        return tiff_error(ErrorCode::corrupt_data, "TIFF photometric interpretation is missing.");
    if (orientation < 1 || orientation > 8)
        return tiff_error(ErrorCode::corrupt_data, "TIFF orientation is invalid.");
    info.orientation = static_cast<Orientation>(orientation);
    std::uint16_t extra_count = 0;
    std::uint16_t* extra_types = nullptr;
    if (TIFFGetField(tif, TIFFTAG_EXTRASAMPLES, &extra_count, &extra_types) == 1 &&
        extra_count != 0 && extra_types) {
        info.alpha =
            extra_types[0] == EXTRASAMPLE_ASSOCALPHA || extra_types[0] == EXTRASAMPLE_UNASSALPHA;
        info.associated_alpha = extra_types[0] == EXTRASAMPLE_ASSOCALPHA;
    }
    if (photometric == PHOTOMETRIC_RGB &&
        (info.samples < 3 || extra_count > static_cast<unsigned int>(info.samples) - 3U))
        return tiff_error(ErrorCode::corrupt_data, "TIFF RGB extra samples are invalid.");
    info.direct = bits == 8 && sample_format == SAMPLEFORMAT_UINT &&
                  photometric == PHOTOMETRIC_RGB && planar == PLANARCONFIG_CONTIG &&
                  (info.samples == 3 || info.samples == 4) && extra_count <= 1;
    if (!info.direct) {
        std::array<char, 1024> error{};
        if (TIFFRGBAImageOK(tif, error.data()) != 1)
            return tiff_error(ErrorCode::unsupported_feature, error.data());
    }
    info.metadata.orientation = info.orientation;
    if (!options.preserve_metadata)
        return info;

    std::uint64_t metadata_bytes = 0;
    const auto read_blob = [&](std::uint32_t tag,
                               std::vector<std::byte>* destination) -> Result<void> {
        std::uint32_t length = 0;
        void* data = nullptr;
        if (TIFFGetField(tif, tag, &length, &data) != 1 || length == 0)
            return {};
        if (!data)
            return tiff_error(ErrorCode::corrupt_data, "TIFF metadata data is missing.");
        if (length > options.limits.maximum_metadata_bytes - metadata_bytes)
            return tiff_error(ErrorCode::limit_exceeded, "TIFF metadata exceeds the byte limit.");
        metadata_bytes += length;
        const auto* begin = static_cast<const std::byte*>(data);
        destination->assign(begin, begin + length);
        return {};
    };
    auto read = read_blob(TIFFTAG_ICCPROFILE, &info.color.icc_profile);
    if (!read)
        return read.error();
    // The RGBA fallback changes grayscale/CMYK samples to RGB. A source profile
    // for those original channels cannot describe the converted RGB samples.
    if (photometric != PHOTOMETRIC_RGB && !info.color.icc_profile.empty() &&
        (info.color.icc_profile.size() < 20 ||
         std::memcmp(info.color.icc_profile.data() + 16, "RGB ", 4) != 0))
        info.color.icc_profile.clear();
    read = read_blob(TIFFTAG_XMLPACKET, &info.metadata.xmp);
    if (!read)
        return read.error();
    char* comment = nullptr;
    if (TIFFGetField(tif, TIFFTAG_IMAGEDESCRIPTION, &comment) == 1 && comment) {
        const auto length = std::strlen(comment);
        if (length > options.limits.maximum_metadata_bytes - metadata_bytes)
            return tiff_error(ErrorCode::limit_exceeded, "TIFF comment exceeds the byte limit.");
        info.metadata.comment.assign(comment, length);
    }
    std::uint16_t unit = RESUNIT_INCH;
    float x_resolution = 0;
    float y_resolution = 0;
    (void)TIFFGetFieldDefaulted(tif, TIFFTAG_RESOLUTIONUNIT, &unit);
    const double factor = unit == RESUNIT_CENTIMETER ? 2.54 : 1.0;
    if (unit == RESUNIT_INCH || unit == RESUNIT_CENTIMETER) {
        if (TIFFGetField(tif, TIFFTAG_XRESOLUTION, &x_resolution) == 1 &&
            std::isfinite(x_resolution) && x_resolution > 0)
            info.metadata.horizontal_dpi = static_cast<double>(x_resolution) * factor;
        if (TIFFGetField(tif, TIFFTAG_YRESOLUTION, &y_resolution) == 1 &&
            std::isfinite(y_resolution) && y_resolution > 0)
            info.metadata.vertical_dpi = static_cast<double>(y_resolution) * factor;
    }
    return info;
}

DocumentInfo document_info(const TiffInfo& source, const DecodeOptions& options) {
    DocumentInfo info;
    info.format = Format::tiff;
    info.canvas_width = source.width;
    info.canvas_height = source.height;
    info.metadata = source.metadata;
    info.color = source.color;
    if (options.orientation == OrientationPolicy::apply) {
        if (static_cast<std::uint8_t>(source.orientation) >= 5)
            std::swap(info.canvas_width, info.canvas_height);
        info.metadata.orientation = Orientation::identity;
    }
    if (!options.preserve_metadata) {
        info.metadata = {};
        info.color = {};
    }
    FrameInfo frame;
    frame.width = info.canvas_width;
    frame.height = info.canvas_height;
    frame.native_format = options.output_format.value_or(kRgba8);
    frame.has_alpha = source.alpha;
    frame.color = info.color;
    frame.metadata = info.metadata;
    info.frames.push_back(std::move(frame));
    return info;
}

unsigned char straight_sample(unsigned char sample, unsigned char alpha) {
    return alpha == 0 ? 0
                      : static_cast<unsigned char>(
                            std::min(255U, (static_cast<unsigned int>(sample) * 255U +
                                            static_cast<unsigned int>(alpha) / 2U) /
                                               static_cast<unsigned int>(alpha)));
}

void unpack_rgb(const TiffInfo& info, std::span<const std::byte> source,
                std::span<std::byte> destination, std::uint32_t pixels, bool bgra) {
    for (std::uint32_t x = 0; x < pixels; ++x) {
        const auto input = source.subspan(static_cast<std::size_t>(x) * info.samples, info.samples);
        auto output = destination.subspan(static_cast<std::size_t>(x) * 4U, 4U);
        const auto alpha = info.alpha ? std::to_integer<unsigned char>(input[3]) : 255U;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            output[channel] = info.associated_alpha
                                  ? static_cast<std::byte>(straight_sample(
                                        std::to_integer<unsigned char>(input[channel]),
                                        static_cast<unsigned char>(alpha)))
                                  : input[channel];
        }
        output[3] = static_cast<std::byte>(alpha);
        if (bgra)
            std::swap(output[0], output[2]);
    }
}

using WriteRows = std::function<Result<void>(std::uint32_t, std::uint32_t, std::size_t,
                                             std::span<const std::byte>)>;

Result<void> read_pixels(TiffSession& session, const TiffInfo& info, const DecodeOptions& options,
                         const WriteRows& write_rows) {
    TIFF* tif = session.handle.get();
    const auto row_bytes = static_cast<std::size_t>(info.width) * 4U;
    // Reserve the other half for LibTIFF's bounded cumulative allocations.
    const auto memory_limit = options.limits.maximum_working_bytes / 2U;
    const bool bgra = options.output_format == kBgra8;
    if (!info.direct) {
        const auto raster_bytes = static_cast<std::uint64_t>(row_bytes) * info.height;
        if (raster_bytes > memory_limit || row_bytes > memory_limit - raster_bytes)
            return tiff_error(ErrorCode::limit_exceeded,
                              "TIFF color conversion exceeds the working-memory limit.");
        std::vector<std::uint32_t> raster(static_cast<std::size_t>(info.width) * info.height);
        std::vector<std::byte> row(row_bytes);
        // Request raw storage orientation. LibTIFF's RGBA converter mirrors but
        // cannot transpose orientations 5-8; our common transform handles all 8.
        if (TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) != 1 ||
            TIFFReadRGBAImageOriented(tif, info.width, info.height, raster.data(),
                                      ORIENTATION_TOPLEFT, 1) != 1)
            return session.io.error(ErrorCode::decode_failed, "Could not decode TIFF pixels.");
        for (std::uint32_t y = 0; y < info.height; ++y) {
            if (!session.io.ready())
                return session.io.error(ErrorCode::decode_failed, "TIFF decoding was cancelled.");
            for (std::uint32_t x = 0; x < info.width; ++x) {
                const auto pixel = raster[static_cast<std::size_t>(y) * info.width + x];
                const auto alpha = static_cast<unsigned char>(TIFFGetA(pixel));
                const auto offset = static_cast<std::size_t>(x) * 4U;
                row[offset] = static_cast<std::byte>(
                    straight_sample(static_cast<unsigned char>(TIFFGetR(pixel)), alpha));
                row[offset + 1U] = static_cast<std::byte>(
                    straight_sample(static_cast<unsigned char>(TIFFGetG(pixel)), alpha));
                row[offset + 2U] = static_cast<std::byte>(
                    straight_sample(static_cast<unsigned char>(TIFFGetB(pixel)), alpha));
                row[offset + 3U] = static_cast<std::byte>(alpha);
                if (bgra)
                    std::swap(row[offset], row[offset + 2U]);
            }
            auto written = write_rows(y, 1, row_bytes, row);
            if (!written)
                return written;
        }
        return {};
    }
    if (TIFFIsTiled(tif)) {
        std::uint32_t tile_width = 0;
        std::uint32_t tile_height = 0;
        if (TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tile_width) != 1 ||
            TIFFGetField(tif, TIFFTAG_TILELENGTH, &tile_height) != 1 || tile_width == 0 ||
            tile_height == 0)
            return tiff_error(ErrorCode::corrupt_data, "TIFF tile dimensions are invalid.");
        const auto tile_bytes = TIFFTileSize64(tif);
        const auto tile_row = TIFFTileRowSize64(tif);
        const auto band_bytes =
            static_cast<std::uint64_t>(row_bytes) * std::min(tile_height, info.height);
        if (tile_bytes == 0 || tile_row < static_cast<std::uint64_t>(tile_width) * info.samples ||
            tile_bytes > memory_limit || band_bytes > memory_limit - tile_bytes ||
            tile_bytes > static_cast<std::uint64_t>(std::numeric_limits<tmsize_t>::max()))
            return tiff_error(ErrorCode::limit_exceeded,
                              "TIFF tiles exceed the working-memory limit.");
        std::vector<std::byte> tile(static_cast<std::size_t>(tile_bytes));
        std::vector<std::byte> band(static_cast<std::size_t>(band_bytes));
        for (std::uint32_t y = 0; y < info.height;) {
            const auto rows = std::min(tile_height, info.height - y);
            for (std::uint32_t x = 0; x < info.width;) {
                if (!session.io.ready())
                    return session.io.error(ErrorCode::decode_failed,
                                            "TIFF decoding was cancelled.");
                const auto count = TIFFReadTile(tif, tile.data(), x, y, 0, 0);
                if (count < 0 || static_cast<std::uint64_t>(count) < tile_bytes)
                    return session.io.error(ErrorCode::truncated_data,
                                            "TIFF tile data is incomplete.");
                const auto columns = std::min(tile_width, info.width - x);
                for (std::uint32_t row = 0; row < rows; ++row) {
                    unpack_rgb(info,
                               std::span(tile).subspan(static_cast<std::size_t>(row * tile_row)),
                               std::span(band).subspan(static_cast<std::size_t>(row) * row_bytes +
                                                       static_cast<std::size_t>(x) * 4U),
                               columns, bgra);
                }
                x += columns;
            }
            auto written =
                write_rows(y, rows, row_bytes,
                           std::span(band).first(static_cast<std::size_t>(rows) * row_bytes));
            if (!written)
                return written;
            y += rows;
        }
        return {};
    }
    const auto scanline_bytes = TIFFScanlineSize64(tif);
    if (scanline_bytes < static_cast<std::uint64_t>(info.width) * info.samples ||
        scanline_bytes > memory_limit || row_bytes > memory_limit - scanline_bytes ||
        scanline_bytes > static_cast<std::uint64_t>(std::numeric_limits<tmsize_t>::max()))
        return tiff_error(ErrorCode::limit_exceeded, "TIFF rows exceed the working-memory limit.");
    std::vector<std::byte> raw(static_cast<std::size_t>(scanline_bytes));
    std::vector<std::byte> row(row_bytes);
    for (std::uint32_t y = 0; y < info.height; ++y) {
        if (!session.io.ready())
            return session.io.error(ErrorCode::decode_failed, "TIFF decoding was cancelled.");
        if (TIFFReadScanline(tif, raw.data(), y, 0) != 1)
            return session.io.error(ErrorCode::truncated_data, "TIFF row data is incomplete.");
        unpack_rgb(info, raw, row, info.width, bgra);
        auto written = write_rows(y, 1, row_bytes, row);
        if (!written)
            return written;
    }
    return {};
}

bool encoder_format(const PixelFormat& format) noexcept {
    return format.sample_type == SampleType::unsigned_integer && format.bits_per_channel == 8 &&
           format.alpha != AlphaMode::premultiplied &&
           (format.channels == ChannelLayout::rgb || format.channels == ChannelLayout::rgba ||
            format.channels == ChannelLayout::bgr || format.channels == ChannelLayout::bgra);
}

Result<void> write_metadata(TIFF* tif, const Metadata& metadata, const ColorEncoding& color) {
    if (color.icc_profile.size() > kClassicLimit || metadata.xmp.size() > kClassicLimit)
        return tiff_error(ErrorCode::limit_exceeded, "TIFF metadata is too large.");
    if (TIFFSetField(tif, TIFFTAG_ORIENTATION, static_cast<std::uint16_t>(metadata.orientation)) !=
        1)
        return tiff_error(ErrorCode::encode_failed, "Could not set TIFF orientation.");
    if (!color.icc_profile.empty() &&
        TIFFSetField(tif, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(color.icc_profile.size()),
                     color.icc_profile.data()) != 1)
        return tiff_error(ErrorCode::encode_failed, "Could not set the TIFF ICC profile.");
    if (!metadata.xmp.empty() &&
        TIFFSetField(tif, TIFFTAG_XMLPACKET, static_cast<std::uint32_t>(metadata.xmp.size()),
                     metadata.xmp.data()) != 1)
        return tiff_error(ErrorCode::encode_failed, "Could not set TIFF XMP metadata.");
    if (!metadata.comment.empty() &&
        TIFFSetField(tif, TIFFTAG_IMAGEDESCRIPTION, metadata.comment.c_str()) != 1)
        return tiff_error(ErrorCode::encode_failed, "Could not set the TIFF comment.");
    const auto set_dpi = [&](std::uint32_t tag, std::optional<double> value) {
        return !value || !std::isfinite(*value) || *value <= 0 ||
               *value > static_cast<double>(std::numeric_limits<float>::max()) ||
               TIFFSetField(tif, tag, *value) == 1;
    };
    if (TIFFSetField(tif, TIFFTAG_RESOLUTIONUNIT, RESUNIT_INCH) != 1 ||
        !set_dpi(TIFFTAG_XRESOLUTION, metadata.horizontal_dpi) ||
        !set_dpi(TIFFTAG_YRESOLUTION, metadata.vertical_dpi))
        return tiff_error(ErrorCode::encode_failed, "Could not set TIFF resolution.");
    return {};
}

using ReadRow = std::function<Result<std::span<const std::byte>>(std::uint32_t)>;

Result<ColorEncoding> tiff_color_encoding(const ColorEncoding& color, bool preserve_metadata) {
    if (!preserve_metadata)
        return ColorEncoding{};
    ColorEncoding encoded = color;
    if (!encoded.icc_profile.empty() || encoded.primaries != ColorPrimaries::srgb ||
        encoded.transfer != TransferFunction::srgb)
        return encoded;
    const std::unique_ptr<void, decltype(&cmsCloseProfile)> profile(cmsCreate_sRGBProfile(),
                                                                    cmsCloseProfile);
    if (!profile)
        return tiff_error(ErrorCode::out_of_memory, "Could not create the TIFF sRGB profile.");
    cmsUInt32Number size = 0;
    if (!cmsSaveProfileToMem(profile.get(), nullptr, &size) || size == 0)
        return tiff_error(ErrorCode::encode_failed, "Could not size the TIFF sRGB profile.");
    encoded.icc_profile.resize(size);
    if (!cmsSaveProfileToMem(profile.get(), encoded.icc_profile.data(), &size))
        return tiff_error(ErrorCode::encode_failed, "Could not write the TIFF sRGB profile.");
    encoded.icc_profile.resize(size);
    return encoded;
}

Result<void> write_pixels(const ImageView& view, const Metadata& metadata,
                          const ColorEncoding& color, const Output& output,
                          const EncodeOptions& options, const ReadRow& read_row,
                          std::stop_token stop) {
    if (!encoder_format(view.format))
        return tiff_error(ErrorCode::unsupported_feature,
                          "TIFF encoding supports straight 8-bit RGB and RGBA.");
    if (view.width == 0 || view.height == 0)
        return tiff_error(ErrorCode::invalid_argument, "TIFF image dimensions are empty.");
    const auto channels = view.format.channel_count();
    const auto row_bytes = static_cast<std::uint64_t>(view.width) * channels;
    if (row_bytes > kClassicLimit || view.height > kClassicLimit / row_bytes)
        return tiff_error(ErrorCode::limit_exceeded, "TIFF output exceeds the classic TIFF limit.");
    const auto raw_bytes = row_bytes * view.height;
    auto encoded_color = tiff_color_encoding(color, options.preserve_metadata);
    if (!encoded_color)
        return encoded_color.error();
    const auto metadata_bytes =
        options.preserve_metadata
            ? static_cast<std::uint64_t>(encoded_color.value().icc_profile.size()) +
                  metadata.xmp.size() + metadata.comment.size()
            : 0;
    if (raw_bytes > kClassicLimit || metadata_bytes > kClassicLimit ||
        raw_bytes + raw_bytes / 100U + metadata_bytes + (std::uint64_t{1} << 20U) > kClassicLimit)
        return tiff_error(ErrorCode::limit_exceeded, "TIFF output exceeds the classic TIFF limit.");
    auto opened = open_tiff(nullptr, &output, DecodeLimits{}, stop);
    if (!opened)
        return opened.error();
    auto& session = *opened.value();
    TIFF* tif = session.handle.get();
    const auto rows_per_strip = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(view.height, std::max<std::uint64_t>(1, 8192U / row_bytes)));
    if (TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, view.width) != 1 ||
        TIFFSetField(tif, TIFFTAG_IMAGELENGTH, view.height) != 1 ||
        TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, static_cast<std::uint16_t>(channels)) != 1 ||
        TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8) != 1 ||
        TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT) != 1 ||
        TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) != 1 ||
        TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) != 1 ||
        TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, rows_per_strip) != 1 ||
        TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE) != 1 ||
        TIFFSetField(tif, TIFFTAG_ZIPQUALITY, std::clamp(options.compression_level, 0, 9)) != 1 ||
        TIFFSetField(tif, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL) != 1)
        return session.io.error(ErrorCode::encode_failed, "Could not configure TIFF encoding.");
    if (channels == 4) {
        std::uint16_t alpha = EXTRASAMPLE_UNASSALPHA;
        if (TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &alpha) != 1)
            return session.io.error(ErrorCode::encode_failed, "Could not configure TIFF alpha.");
    }
    if (options.preserve_metadata) {
        auto written = write_metadata(tif, metadata, encoded_color.value());
        if (!written)
            return written;
    }
    std::vector<std::byte> row(static_cast<std::size_t>(row_bytes));
    const bool bgr =
        view.format.channels == ChannelLayout::bgr || view.format.channels == ChannelLayout::bgra;
    for (std::uint32_t y = 0; y < view.height; ++y) {
        if (stop.stop_requested())
            return cancelled_status();
        auto read = read_row(y);
        if (!read)
            return read.error();
        if (read.value().size() < row.size())
            return tiff_error(ErrorCode::invalid_argument, "TIFF source row is incomplete.");
        std::copy_n(read.value().begin(), row.size(), row.begin());
        if (bgr) {
            for (std::uint32_t x = 0; x < view.width; ++x)
                std::swap(row[static_cast<std::size_t>(x) * channels],
                          row[static_cast<std::size_t>(x) * channels + 2U]);
        }
        // LibTIFF may modify its input while applying predictors/byte swaps.
        if (TIFFWriteScanline(tif, row.data(), y, 0) != 1)
            return session.io.error(ErrorCode::encode_failed, "Could not encode TIFF pixels.");
    }
    if (TIFFWriteDirectory(tif) != 1 || TIFFFlush(tif) != 1)
        return session.io.error(ErrorCode::encode_failed, "Could not finalize the TIFF image.");
    session.handle.reset();
    if (!session.io.ready())
        return session.io.error(ErrorCode::encode_failed, "Could not finalize TIFF output.");
    // Header/IFD backpatches may leave the cursor before the end. The common
    // encode wrapper reports artifact length from this final sink position.
    return output.sink->seek(session.io.origin + session.io.size);
}

} // namespace

CodecCapability TiffCodec::capabilities() const noexcept {
    return CodecCapability::inspect | CodecCapability::decode | CodecCapability::encode |
           CodecCapability::streaming_decode | CodecCapability::metadata_decode;
}

int TiffCodec::probe(std::span<const std::byte> header, std::string_view name_hint) const noexcept {
    if (header.size() >= 4) {
        const bool little = header[0] == std::byte{'I'} && header[1] == std::byte{'I'} &&
                            (header[2] == std::byte{42} || header[2] == std::byte{43}) &&
                            header[3] == std::byte{0};
        const bool big = header[0] == std::byte{'M'} && header[1] == std::byte{'M'} &&
                         header[2] == std::byte{0} &&
                         (header[3] == std::byte{42} || header[3] == std::byte{43});
        if (little || big)
            return 100;
    }
    return format_from_extension(name_hint) == Format::tiff ? 10 : 0;
}

Result<DocumentInfo> TiffCodec::inspect(const Input& input, const DecodeOptions& options,
                                        std::stop_token stop) const {
    auto opened = open_tiff(&input, nullptr, options.limits, stop);
    if (!opened)
        return opened.error();
    auto info = read_info(*opened.value(), options);
    if (!info)
        return info.error();
    return document_info(info.value(), options);
}

Result<Document> TiffCodec::decode(const Input& input, const DecodeOptions& options,
                                   std::stop_token stop) const {
    auto opened = open_tiff(&input, nullptr, options.limits, stop);
    if (!opened)
        return opened.error();
    auto inspected = read_info(*opened.value(), options);
    if (!inspected)
        return inspected.error();
    const auto& info = inspected.value();
    const auto output_bytes = static_cast<std::uint64_t>(info.width) * info.height * 4U;
    // Applying orientation owns a second complete raster during the transform.
    const auto allocation_factor =
        options.orientation == OrientationPolicy::apply && info.orientation != Orientation::identity
            ? 2U
            : 1U;
    if (output_bytes > options.limits.maximum_owned_output_bytes / allocation_factor)
        return tiff_error(ErrorCode::limit_exceeded,
                          "TIFF output exceeds the owning decode limit.");
    auto allocated =
        MutableImage::allocate(info.width, info.height, options.output_format.value_or(kRgba8), 1);
    if (!allocated)
        return allocated.error();
    auto image = std::move(allocated).value();
    auto read =
        read_pixels(*opened.value(), info, options,
                    [&](std::uint32_t y, std::uint32_t count, std::size_t stride,
                        std::span<const std::byte> bytes) -> Result<void> {
                        for (std::uint32_t row = 0; row < count; ++row) {
                            std::memcpy(image.pixels().data() +
                                            static_cast<std::size_t>(y + row) * image.row_stride(),
                                        bytes.data() + static_cast<std::size_t>(row) * stride,
                                        static_cast<std::size_t>(info.width) * 4U);
                        }
                        return {};
                    });
    if (!read)
        return read.error();
    Document document;
    document.format = Format::tiff;
    document.canvas_width = info.width;
    document.canvas_height = info.height;
    document.metadata = info.metadata;
    document.color = info.color;
    Frame frame;
    frame.image = std::move(image).freeze();
    frame.metadata = info.metadata;
    frame.color = info.color;
    document.frames.push_back(std::move(frame));
    if (options.orientation == OrientationPolicy::apply) {
        auto oriented = apply_orientation(&document, info.orientation, stop);
        if (!oriented)
            return oriented.error();
    }
    if (!options.preserve_metadata) {
        document.metadata = {};
        document.color = {};
        document.frames.front().metadata = {};
        document.frames.front().color = {};
    }
    return document;
}

Result<void> TiffCodec::decode_to_sink(const Input& input, PixelSink& sink,
                                       const DecodeOptions& options, std::stop_token stop) const {
    auto opened = open_tiff(&input, nullptr, options.limits, stop);
    if (!opened)
        return opened.error();
    auto inspected = read_info(*opened.value(), options);
    if (!inspected)
        return inspected.error();
    const auto& info = inspected.value();
    if (options.orientation == OrientationPolicy::apply &&
        info.orientation != Orientation::identity)
        return Codec::decode_to_sink(input, sink, options, stop);
    const auto metadata = document_info(info, options);
    auto begun = sink.begin(metadata);
    if (!begun)
        return begun;
    begun = sink.begin_frame(0, metadata.frames.front());
    if (!begun)
        return begun;
    auto read = read_pixels(
        *opened.value(), info, options,
        [&](std::uint32_t y, std::uint32_t rows, std::size_t stride,
            std::span<const std::byte> bytes) { return sink.write_rows(y, rows, stride, bytes); });
    if (!read)
        return read;
    auto ended = sink.end_frame(0);
    return ended ? sink.end() : ended;
}

Result<EncodedArtifactReceipt> TiffCodec::encode_to_sink(const Document& document,
                                                         const Output& output,
                                                         const EncodeOptions& options,
                                                         std::stop_token stop) const {
    if (document.frames.empty())
        return tiff_error(ErrorCode::invalid_argument, "TIFF output has no image frame.");
    const auto view = document.frames.front().image.view();
    auto valid = view.validate();
    if (!valid)
        return valid.error();
    auto written = write_pixels(
        view, document.metadata, document.color, output, options,
        [&](std::uint32_t y) -> Result<std::span<const std::byte>> {
            return view.pixels.subspan(static_cast<std::size_t>(y) * view.row_stride,
                                       static_cast<std::size_t>(view.width) *
                                           view.format.channel_count());
        },
        stop);
    if (!written)
        return written.error();
    return receipt_for_document(document, format());
}

RasterEncodeRoute TiffCodec::raster_encode_route(const DocumentDescriptor& descriptor,
                                                 const EncodeOptions&) const noexcept {
    if (descriptor.frames.empty())
        return RasterEncodeRoute::materialized;
    const auto& frame = descriptor.frames.front();
    return frame.layout.planes.size() == 1 &&
                   frame.layout.planes.front().semantic == PlaneSemantic::packed &&
                   encoder_format(frame.layout.planes.front().format)
               ? RasterEncodeRoute::native
               : RasterEncodeRoute::materialized;
}

Result<EncodedArtifactReceipt> TiffCodec::encode_raster_to_sink(const RasterSource& source,
                                                                const Output& output,
                                                                const EncodeOptions& options,
                                                                std::stop_token stop) const {
    const auto& descriptor = source.descriptor();
    if (raster_encode_route(descriptor, options) != RasterEncodeRoute::native)
        return Codec::encode_raster_to_sink(source, output, options, stop);
    const auto& frame = descriptor.frames.front();
    const auto& pixel_format = frame.layout.planes.front().format;
    const auto row_bytes = static_cast<std::size_t>(frame.width) * pixel_format.channel_count();
    std::vector<std::byte> row;
    const ImageView view{frame.width, frame.height, pixel_format, row_bytes, {}};
    auto written = write_pixels(
        view, descriptor.metadata, descriptor.color, output, options,
        [&](std::uint32_t y) -> Result<std::span<const std::byte>> {
            if (row.empty())
                row.resize(row_bytes);
            auto read = source.read_rows(0, 0, y, 1, row_bytes, row, stop);
            if (!read)
                return read.error();
            return std::span<const std::byte>(row);
        },
        stop);
    if (!written)
        return written.error();
    return receipt_for_descriptor(descriptor, format());
}

} // namespace snow::image::internal
