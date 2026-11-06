#include "snow/image/resource_estimate.h"
#include "snow/image/resource_plan.h"
#include "snow/image/service.h"

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace snow::image;
constexpr PixelFormat kBgr8{SampleType::unsigned_integer, ChannelLayout::bgr, AlphaMode::none, 8,
                            true};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
template <typename T> T take(Result<T> result) {
    if (!result)
        throw std::runtime_error(result.error().message);
    return std::move(result).value();
}

struct TempFile final {
    std::filesystem::path path;
    TempFile() {
        static unsigned int index = 0;
        path = std::filesystem::temp_directory_path() /
               ("snow-image-tiff-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(++index) + ".tif");
    }
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
};

using Pixel = std::array<unsigned char, 4>;
Pixel pixel(std::uint32_t x, std::uint32_t y) {
    return {static_cast<unsigned char>((x * 23U + y * 17U + 31U) % 256U),
            static_cast<unsigned char>((x * 13U + y * 37U + 7U) % 256U),
            static_cast<unsigned char>((x * 41U + y * 3U + 19U) % 256U),
            static_cast<unsigned char>((x + y) % 3U == 0   ? 0
                                       : (x + y) % 3U == 1 ? 73
                                                           : 255)};
}

Document image_document(std::uint32_t width = 19, std::uint32_t height = 18,
                        PixelFormat format = kRgba8) {
    auto image = take(MutableImage::allocate(width, height, format, 1));
    const auto channels = format.channel_count();
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            auto values = pixel(x, y);
            if (format == kBgra8 || format == kBgr8)
                std::swap(values[0], values[2]);
            std::memcpy(image.pixels().data() + static_cast<std::size_t>(y) * image.row_stride() +
                            static_cast<std::size_t>(x) * channels,
                        values.data(), channels);
        }
    }
    Document document;
    document.format = Format::tiff;
    document.canvas_width = width;
    document.canvas_height = height;
    Frame frame;
    frame.image = std::move(image).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

std::vector<std::byte> encode_document(const Document& document, int level = 6,
                                       EncodeResult* receipt = nullptr) {
    auto bytes = std::make_shared<std::vector<std::byte>>();
    EncodeOptions options;
    options.format = Format::tiff;
    options.compression_level = level;
    auto result = take(Service{}.encode(document, memory_output(bytes), options));
    require(result.bytes_written == bytes->size(), "TIFF byte count must match its final extent.");
    require(result.round_trip == PixelRoundTrip::exact, "TIFF must report exact pixel encoding.");
    require(result.receipt.encoder_finalized_and_sink_flushed &&
                result.receipt.emitted_frame_count == 1 && result.receipt.format == Format::tiff,
            "TIFF receipt must describe one finalized image.");
    if (receipt)
        *receipt = result;
    return *bytes;
}

Pixel read_pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    Pixel result{};
    std::memcpy(result.data(),
                image.pixels().data() + static_cast<std::size_t>(y) * image.row_stride() +
                    static_cast<std::size_t>(x) * 4U,
                4);
    if (image.format() == kBgra8)
        std::swap(result[0], result[2]);
    return result;
}
void check_pixels(const Image& image, bool opaque = false) {
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            auto expected = pixel(x, y);
            if (opaque)
                expected[3] = 255;
            require(read_pixel(image, x, y) == expected,
                    "TIFF pixels, including hidden RGB, must survive exactly.");
        }
    }
}

struct FixtureOptions final {
    bool tiled = false;
    bool big_endian = false;
    bool big_tiff = false;
    bool multipage = false;
    bool gray16 = false;
    bool palette = false;
    std::uint16_t orientation = 1;
};

std::vector<std::byte> fixture(FixtureOptions options = {}) {
    TempFile file;
    std::string mode = options.big_tiff ? "w8" : "w";
    mode += options.big_endian ? "b" : "l";
#if defined(_WIN32)
    TIFF* tif = TIFFOpenW(file.path.c_str(), mode.c_str());
#else
    TIFF* tif = TIFFOpen(file.path.c_str(), mode.c_str());
#endif
    require(tif != nullptr, "TIFF fixture must open.");
    const std::uint32_t width = options.tiled ? 19U : 3U;
    const std::uint32_t height = options.tiled ? 18U : 2U;
    const auto pages = options.multipage ? 2 : 1;
    for (int page = 0; page < pages; ++page) {
        const bool gray = options.gray16 || options.palette;
        require(TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, width) == 1 &&
                    TIFFSetField(tif, TIFFTAG_IMAGELENGTH, height) == 1 &&
                    TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, gray ? 1 : 4) == 1 &&
                    TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, options.gray16 ? 16 : 8) == 1 &&
                    TIFFSetField(tif, TIFFTAG_PHOTOMETRIC,
                                 options.palette  ? PHOTOMETRIC_PALETTE
                                 : options.gray16 ? PHOTOMETRIC_MINISBLACK
                                                  : PHOTOMETRIC_RGB) == 1 &&
                    TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) == 1 &&
                    TIFFSetField(tif, TIFFTAG_COMPRESSION, COMPRESSION_LZW) == 1 &&
                    TIFFSetField(tif, TIFFTAG_ORIENTATION, options.orientation) == 1,
                "TIFF fixture tags must be written.");
        std::uint16_t alpha = EXTRASAMPLE_UNASSALPHA;
        if (!gray)
            require(TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &alpha) == 1,
                    "TIFF fixture must declare straight alpha.");
        std::array<std::uint16_t, 256> red{};
        std::array<std::uint16_t, 256> green{};
        std::array<std::uint16_t, 256> blue{};
        if (options.palette) {
            red[1] = 65535;
            green[2] = 65535;
            blue[3] = 65535;
            require(TIFFSetField(tif, TIFFTAG_COLORMAP, red.data(), green.data(), blue.data()) == 1,
                    "TIFF palette must be set.");
        }
        if (options.tiled) {
            require(TIFFSetField(tif, TIFFTAG_TILEWIDTH, 16) == 1 &&
                        TIFFSetField(tif, TIFFTAG_TILELENGTH, 16) == 1,
                    "TIFF tiles must be configured.");
            std::array<unsigned char, 16 * 16 * 4> tile{};
            for (std::uint32_t y = 0; y < height; y += 16) {
                for (std::uint32_t x = 0; x < width; x += 16) {
                    for (std::uint32_t row = 0; row < 16; ++row) {
                        for (std::uint32_t column = 0; column < 16; ++column) {
                            const auto values = pixel(x + column, y + row);
                            std::copy(values.begin(), values.end(),
                                      tile.begin() +
                                          static_cast<std::ptrdiff_t>((row * 16U + column) * 4U));
                        }
                    }
                    require(TIFFWriteTile(tif, tile.data(), x, y, 0, 0) > 0,
                            "TIFF tile must be written.");
                }
            }
        } else {
            require(TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, 1) == 1,
                    "TIFF strips must be configured.");
            for (std::uint32_t y = 0; y < height; ++y) {
                std::array<unsigned char, 12> row{};
                std::array<std::uint16_t, 3> wide_row{0, 32768, 65535};
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto values = pixel(x, y);
                    if (options.palette)
                        row[x] = static_cast<unsigned char>(x + 1U);
                    else
                        std::copy(values.begin(), values.end(), row.begin() + x * 4U);
                }
                require(TIFFWriteScanline(tif,
                                          options.gray16 ? static_cast<void*>(wide_row.data())
                                                         : static_cast<void*>(row.data()),
                                          y, 0) == 1,
                        "TIFF row must be written.");
            }
        }
        require(TIFFWriteDirectory(tif) == 1, "TIFF fixture directory must finalize.");
    }
    TIFFClose(tif);
    auto input = take(file_input(file.path));
    return take(read_all(*input.source, 1024 * 1024));
}

class SequentialSink final : public ByteSink {
  public:
    Result<void> write(std::span<const std::byte> source) override {
        written += source.size();
        return {};
    }
    Result<std::uint64_t> position() const override {
        return written;
    }
    Result<void> seek(std::uint64_t) override {
        return Status::error(ErrorCode::io_error, "Cannot seek.");
    }
    Result<void> flush() override {
        return {};
    }
    bool seekable() const noexcept override {
        return false;
    }
    std::uint64_t written = 0;
};

class FailingSink final : public ByteSink {
  public:
    FailingSink()
        : bytes(std::make_shared<std::vector<std::byte>>()), delegate(memory_output(bytes)) {}
    Result<void> write(std::span<const std::byte> source) override {
        if (fail_write)
            return Status::error(ErrorCode::io_error, "Injected TIFF write failure.");
        return delegate.sink->write(source);
    }
    Result<std::uint64_t> position() const override {
        return delegate.sink->position();
    }
    Result<void> seek(std::uint64_t offset) override {
        if (fail_seek)
            return Status::error(ErrorCode::io_error, "Injected TIFF seek failure.");
        return delegate.sink->seek(offset);
    }
    Result<void> flush() override {
        return fail_flush ? Result<void>(Status::error(ErrorCode::io_error, "Flush failure."))
                          : Result<void>{};
    }
    bool seekable() const noexcept override {
        return true;
    }
    std::shared_ptr<std::vector<std::byte>> bytes;
    Output delegate;
    bool fail_write = false;
    bool fail_seek = false;
    bool fail_flush = false;
};

class SourceRows final : public RasterSource {
  public:
    explicit SourceRows(Document document) : document_(std::move(document)) {
        DocumentInfo info;
        info.format = Format::tiff;
        info.canvas_width = document_.canvas_width;
        info.canvas_height = document_.canvas_height;
        FrameInfo frame;
        frame.width = info.canvas_width;
        frame.height = info.canvas_height;
        frame.native_format = document_.frames.front().image.format();
        frame.has_alpha = true;
        info.frames.push_back(frame);
        descriptor_ = take(describe_document(info));
    }
    const DocumentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    RasterAccess access() const noexcept override {
        return RasterAccess::sequential_rows;
    }
    Result<void> read_rows(std::uint32_t frame, std::uint32_t plane, std::uint32_t first,
                           std::uint32_t count, std::size_t stride,
                           std::span<std::byte> destination, std::stop_token) const override {
        require(frame == 0 && plane == 0 && count == 1 && first == next_row++,
                "TIFF native export must read first-frame rows sequentially.");
        const auto& image = document_.frames.front().image;
        require(stride >= image.width() * 4U && destination.size() >= image.width() * 4U,
                "TIFF row buffer must fit the source.");
        std::memcpy(destination.data(), image.pixels().data() + first * image.row_stride(),
                    image.width() * 4U);
        return {};
    }
    DocumentDescriptor descriptor_;
    mutable std::uint32_t next_row = 0;
    Document document_;
};

class RowSink final : public PixelSink {
  public:
    Result<void> begin(const DocumentInfo& info) override {
        format = info.frames.front().native_format;
        width = info.canvas_width;
        height = info.canvas_height;
        pixels.resize(static_cast<std::size_t>(width) * height * 4U);
        return {};
    }
    Result<void> begin_frame(std::uint32_t index, const FrameInfo&) override {
        require(index == 0, "TIFF must deliver only one frame.");
        return {};
    }
    Result<void> write_rows(std::uint32_t first, std::uint32_t count, std::size_t stride,
                            std::span<const std::byte> source) override {
        require(first == next, "TIFF streaming rows must be sequential.");
        for (std::uint32_t row = 0; row < count; ++row)
            std::memcpy(pixels.data() + static_cast<std::size_t>(first + row) * width * 4U,
                        source.data() + static_cast<std::size_t>(row) * stride, width * 4U);
        next += count;
        return {};
    }
    Result<void> end_frame(std::uint32_t) override {
        return {};
    }
    Result<void> end() override {
        require(next == height, "TIFF sink must receive every row.");
        ended = true;
        return {};
    }
    PixelFormat format;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t next = 0;
    bool ended = false;
    std::vector<std::byte> pixels;
};

void aliases_roundtrips_and_capabilities() {
    Service service;
    require(format_from_extension("picture.TIF") == Format::tiff &&
                format_from_extension(".tiff") == Format::tiff &&
                format_name(Format::tiff) == "TIFF",
            "Both TIFF aliases must be case insensitive.");
    const auto* encoder = service.encoder_info(Format::tiff);
    require(encoder && has_feature(encoder->features, EncoderFeature::pixel_exact) &&
                has_feature(encoder->features, EncoderFeature::alpha) &&
                !has_feature(encoder->features, EncoderFeature::animation) &&
                encoder->compression_level.minimum == 0 &&
                encoder->compression_level.default_value == 6 &&
                encoder->compression_level.maximum == 9,
            "TIFF encoder must advertise lossless still-image compression controls.");
    for (const auto format : {kRgba8, kBgra8, kRgb8, kBgr8}) {
        for (const int level : {0, 6, 9}) {
            auto document = image_document(19, 18, format);
            const auto bytes = encode_document(document, level);
            require(take(service.detect(memory_input(bytes, "wrong.jpg"))) == Format::tiff,
                    "TIFF magic must take precedence over a misleading extension.");
            for (const auto output : {kRgba8, kBgra8}) {
                DecodeOptions options;
                options.output_format = output;
                auto decoded = take(service.decode(memory_input(bytes), options));
                require(decoded.frames.front().image.format() == output,
                        "TIFF must honor requested RGBA and BGRA output.");
                check_pixels(decoded.frames.front().image, format.channel_count() == 3);
            }
        }
    }
}

void external_tiled_and_fallback_images() {
    Service service;
    for (const bool endian : {false, true}) {
        for (const bool big_tiff : {false, true}) {
            for (const bool tiled : {false, true}) {
                const auto bytes =
                    fixture({.tiled = tiled, .big_endian = endian, .big_tiff = big_tiff});
                auto decoded = take(service.decode(memory_input(bytes, "image.bin")));
                check_pixels(decoded.frames.front().image);
                DecodeOptions options;
                options.output_format = kBgra8;
                options.limits.maximum_owned_output_bytes = 0;
                RowSink sink;
                require(service.decode_to_sink(memory_input(bytes), sink, options).has_value() &&
                            sink.ended && sink.format == kBgra8,
                        "TIFF must stream strips/tiles without owning the output frame.");
                const auto values = pixel(0, 0);
                require(sink.pixels[0] == static_cast<std::byte>(values[2]) &&
                            sink.pixels[2] == static_cast<std::byte>(values[0]),
                        "TIFF streaming BGRA must preserve hidden RGB.");
            }
        }
    }
    auto gray = take(service.decode(memory_input(fixture({.gray16 = true}))));
    require(read_pixel(gray.frames.front().image, 0, 0) == Pixel{0, 0, 0, 255} &&
                read_pixel(gray.frames.front().image, 2, 0) == Pixel{255, 255, 255, 255},
            "16-bit grayscale import must convert to RGBA8.");
    auto palette = take(service.decode(memory_input(fixture({.palette = true}))));
    require(read_pixel(palette.frames.front().image, 0, 0) == Pixel{255, 0, 0, 255} &&
                read_pixel(palette.frames.front().image, 1, 0) == Pixel{0, 255, 0, 255} &&
                read_pixel(palette.frames.front().image, 2, 0) == Pixel{0, 0, 255, 255},
            "Palette TIFF import must expand its color map.");
}

void orientation_and_metadata() {
    constexpr std::array<std::array<unsigned int, 6>, 8> orders{{{0, 1, 2, 3, 4, 5},
                                                                 {2, 1, 0, 5, 4, 3},
                                                                 {5, 4, 3, 2, 1, 0},
                                                                 {3, 4, 5, 0, 1, 2},
                                                                 {0, 3, 1, 4, 2, 5},
                                                                 {3, 0, 4, 1, 5, 2},
                                                                 {5, 2, 4, 1, 3, 0},
                                                                 {2, 5, 1, 4, 0, 3}}};
    Service service;
    for (std::uint16_t orientation = 1; orientation <= 8; ++orientation) {
        const auto bytes = fixture({.orientation = orientation});
        DecodeOptions options;
        options.orientation = OrientationPolicy::apply;
        options.output_format = kBgra8;
        options.preserve_metadata = false;
        const auto info = take(service.inspect(memory_input(bytes), options));
        const auto decoded = take(service.decode(memory_input(bytes), options));
        const auto& image = decoded.frames.front().image;
        require(image.width() == (orientation >= 5 ? 2U : 3U) &&
                    image.height() == (orientation >= 5 ? 3U : 2U) &&
                    info.canvas_width == image.width() && info.canvas_height == image.height() &&
                    image.format() == kBgra8 &&
                    decoded.metadata.orientation == Orientation::identity,
                "All TIFF orientations must agree between inspection and decode.");
        for (std::uint32_t index = 0; index < 6; ++index) {
            const auto source = orders[orientation - 1U][index];
            require(read_pixel(image, index % image.width(), index / image.width()) ==
                        pixel(source % 3U, source / 3U),
                    "TIFF orientation must transform pixels exactly once.");
        }
    }
    auto document = image_document();
    document.metadata.horizontal_dpi = 144.0;
    document.metadata.vertical_dpi = 96.0;
    document.metadata.comment = "TIFF metadata fixture";
    document.metadata.xmp = {std::byte{'x'}, std::byte{'m'}, std::byte{'p'}};
    document.color.icc_profile = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    const auto bytes = encode_document(document);
    auto decoded = take(service.decode(memory_input(bytes)));
    require(decoded.metadata.comment == document.metadata.comment &&
                decoded.metadata.xmp == document.metadata.xmp &&
                decoded.color.icc_profile == document.color.icc_profile &&
                decoded.metadata.horizontal_dpi && decoded.metadata.vertical_dpi &&
                std::abs(*decoded.metadata.horizontal_dpi - 144.0) < 0.001 &&
                std::abs(*decoded.metadata.vertical_dpi - 96.0) < 0.001,
            "TIFF must preserve understood metadata.");
    DecodeOptions options;
    options.preserve_metadata = false;
    decoded = take(service.decode(memory_input(bytes), options));
    require(decoded.metadata.comment.empty() && decoded.metadata.xmp.empty() &&
                decoded.color.icc_profile.empty() && !decoded.metadata.horizontal_dpi,
            "Disabling TIFF metadata preservation must clear metadata.");
    options.preserve_metadata = true;
    options.limits.maximum_metadata_bytes = 2;
    const auto limited = service.decode(memory_input(bytes), options);
    require(!limited && limited.error().code == ErrorCode::limit_exceeded,
            "TIFF metadata limits must be enforced.");
    document = image_document();
    document.color.primaries = ColorPrimaries::srgb;
    document.color.transfer = TransferFunction::srgb;
    decoded = take(service.decode(memory_input(encode_document(document))));
    require(decoded.color.icc_profile.size() >= 128 &&
                std::memcmp(decoded.color.icc_profile.data() + 16, "RGB ", 4) == 0 &&
                decoded.frames.front().color.icc_profile == decoded.color.icc_profile,
            "Named sRGB TIFF exports must carry an RGB ICC profile.");
}

void first_page_streaming_and_receipts() {
    Service service;
    const auto bytes = fixture({.multipage = true});
    const auto info = take(service.inspect(memory_input(bytes)));
    require(info.frames.size() == 1, "TIFF import must expose only its first page.");
    DecodeOptions options;
    options.frame_index = 0;
    auto decoded = take(service.decode(memory_input(bytes), options));
    require(decoded.frames.size() == 1, "Selecting TIFF page zero must work.");
    options.frame_index = 1;
    const auto rejected = service.decode(memory_input(bytes), options);
    require(!rejected && rejected.error().code == ErrorCode::invalid_argument,
            "Selecting later TIFF pages must fail clearly.");
    auto document = image_document();
    document.frames.push_back(document.frames.front());
    const auto exported = encode_document(document);
    require(take(service.decode(memory_input(exported))).frames.size() == 1,
            "TIFF export must write one page even for multiframe input.");
    SourceRows source(image_document());
    EncodeOptions encode;
    encode.format = Format::tiff;
    require(take(service.raster_encode_route(source.descriptor(), encode)) ==
                RasterEncodeRoute::native,
            "TIFF row sources must use native encoding.");
    auto output = std::make_shared<std::vector<std::byte>>();
    const auto receipt = take(service.encode(source, memory_output(output), encode));
    require(source.next_row == 18 && receipt.bytes_written == output->size() &&
                receipt.receipt.encoder_finalized_and_sink_flushed &&
                receipt.round_trip == PixelRoundTrip::exact,
            "TIFF native row encoding must finalize with correct accounting.");
    check_pixels(take(service.decode(memory_input(*output))).frames.front().image);
    auto prefixed = std::make_shared<std::vector<std::byte>>();
    auto sink = memory_output(prefixed);
    const std::array<std::byte, 3> prefix{std::byte{1}, std::byte{2}, std::byte{3}};
    require(sink.sink->write(prefix).has_value(), "Output prefix must be written.");
    const auto appended = take(service.encode(image_document(), sink, encode));
    require(appended.bytes_written == prefixed->size() - prefix.size() &&
                (*prefixed)[0] == prefix[0],
            "TIFF offsets and byte counts must be relative to its artifact origin.");
    check_pixels(take(service.decode(memory_input(std::span(*prefixed).subspan(prefix.size()))))
                     .frames.front()
                     .image);

    auto descriptor = source.descriptor();
    auto storage = std::vector<std::byte>(
        static_cast<std::size_t>(take(RasterBufferStore::required_bytes(descriptor))));
    auto store = take(RasterBufferStore::create(storage, descriptor));
    const auto& image = document.frames.front().image;
    require(store->write_rows(0, 0, 0, image.height(), image.row_stride(), image.pixels())
                    .has_value() &&
                store->commit().has_value(),
            "TIFF raster transport fixture must seal.");
    store.reset();
    const auto reopened = take(RasterBufferStore::open(storage));
    require(reopened->descriptor().format == Format::tiff,
            "TIFF format identifiers must survive raster transport serialization.");
}

std::uint32_t little_u32(const std::vector<std::byte>& bytes, std::size_t offset) {
    std::uint32_t result = 0;
    for (unsigned int shift = 0; shift < 32; shift += 8)
        result |=
            static_cast<std::uint32_t>(std::to_integer<unsigned char>(bytes[offset + shift / 8U]))
            << shift;
    return result;
}
void malformed_directory_and_later_pages() {
    Service service;
    auto bytes = fixture();
    const auto directory = static_cast<std::size_t>(little_u32(bytes, 4));
    const auto entries =
        static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[directory])) |
        (static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[directory + 1])) << 8U);
    bool patched = false;
    for (unsigned int index = 0; index < entries; ++index) {
        const auto entry = directory + 2U + static_cast<std::size_t>(index) * 12U;
        const auto tag =
            static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[entry])) |
            (static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[entry + 1])) << 8U);
        if (tag == TIFFTAG_SAMPLESPERPIXEL) {
            bytes[entry + 8U] = std::byte{3};
            patched = true;
        }
    }
    require(patched && !service.decode(memory_input(bytes)),
            "RGB TIFF with too few color samples for its declared alpha must fail safely.");
    bytes = fixture({.multipage = true});
    const auto first = static_cast<std::size_t>(little_u32(bytes, 4));
    const auto count =
        static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[first])) |
        (static_cast<unsigned int>(std::to_integer<unsigned char>(bytes[first + 1])) << 8U);
    const auto next = first + 2U + static_cast<std::size_t>(count) * 12U;
    for (std::size_t index = 0; index < 4; ++index)
        bytes[next + index] = std::byte{0xff};
    check_pixels(take(service.decode(memory_input(bytes))).frames.front().image);
}

void errors_and_resource_limits() {
    Service service;
    const auto bytes = encode_document(image_document());
    auto truncated = bytes;
    truncated.resize(12);
    require(!service.decode(memory_input(truncated)), "Truncated TIFF must fail.");
    auto garbage = bytes;
    garbage[0] = std::byte{0};
    require(!service.decode(memory_input(garbage, "fixture.tiff")), "Corrupt TIFF must fail.");
    DecodeOptions options;
    options.limits.maximum_width = 1;
    require(!service.decode(memory_input(bytes), options), "TIFF dimensions must respect limits.");
    options = {};
    options.limits.maximum_input_bytes = 4;
    require(!service.decode(memory_input(bytes), options), "TIFF input bytes must respect limits.");
    options = {};
    options.limits.maximum_owned_output_bytes = 1;
    require(!service.decode(memory_input(bytes), options), "TIFF owned bytes must respect limits.");
    options = {};
    options.limits.maximum_working_bytes = 0;
    require(!service.decode(memory_input(bytes), options),
            "TIFF working memory must respect limits.");
    options = {};
    options.limits.maximum_frames = 0;
    require(!service.decode(memory_input(bytes), options), "TIFF frame limit must be checked.");
    std::stop_source stopped;
    stopped.request_stop();
    const auto cancelled = service.decode(memory_input(bytes), {}, stopped.get_token());
    require(!cancelled && cancelled.error().code == ErrorCode::cancelled,
            "TIFF cancellation must remain observable.");
    EncodeOptions encode;
    encode.format = Format::tiff;
    auto sequential = std::make_shared<SequentialSink>();
    const auto unsupported = service.encode(image_document(), Output{sequential, {}}, encode);
    require(!unsupported && unsupported.error().code == ErrorCode::unsupported_feature &&
                sequential->written == 0,
            "TIFF must reject sequential sinks before writing.");
    for (int failure = 0; failure < 3; ++failure) {
        auto failed = std::make_shared<FailingSink>();
        failed->fail_write = failure == 0;
        failed->fail_seek = failure == 1;
        failed->fail_flush = failure == 2;
        const auto result = service.encode(image_document(), Output{failed, {}}, encode);
        require(!result && result.error().code == ErrorCode::io_error,
                "TIFF write, seek, and flush failures must not yield success receipts.");
    }
    SourceRows oversized(image_document());
    auto& huge = oversized.descriptor_;
    huge.canvas_width = std::numeric_limits<std::uint32_t>::max();
    huge.canvas_height = std::numeric_limits<std::uint32_t>::max();
    huge.frames.front().width = huge.canvas_width;
    huge.frames.front().height = huge.canvas_height;
    huge.frames.front().layout.planes.front().width = huge.canvas_width;
    huge.frames.front().layout.planes.front().height = huge.canvas_height;
    auto huge_output = std::make_shared<std::vector<std::byte>>();
    const auto huge_result = service.encode(oversized, memory_output(huge_output), encode);
    require(
        !huge_result && huge_result.error().code == ErrorCode::limit_exceeded &&
            huge_output->empty() && oversized.next_row == 0,
        "TIFF classic size limits must reject overflowing row sources before allocation or reads.");
    auto document = image_document();
    const auto estimate = take(estimate_encode_resources(document, encode));
    require(estimate.output_bytes >= bytes.size(), "TIFF estimates must cover the final artifact.");
    SourceRows source(document);
    ResourcePlanRequest request;
    request.source = source.descriptor();
    request.output = source.descriptor();
    request.encode_options = encode;
    request.raster_route = RasterEncodeRoute::native;
    request.gpu_transform = false;
    const auto plan = take(plan_resources(request));
    require(plan.peak.artifact_bytes >= bytes.size(),
            "TIFF resource plans must include container overhead.");
}

} // namespace

int main() {
    try {
        aliases_roundtrips_and_capabilities();
        external_tiled_and_fallback_images();
        orientation_and_metadata();
        first_page_streaming_and_receipts();
        malformed_directory_and_later_pages();
        errors_and_resource_limits();
        std::cout << "snow_image TIFF tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
