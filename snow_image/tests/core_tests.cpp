#include "snow/image/service.h"
#include "snow/image/processing.h"
#include "snow/image/resource_estimate.h"
#include "snow/image/resource_plan.h"
#include "snow/image/raster.h"
#include "snow/image/raster_conversion.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using snow::image::Document;
using snow::image::ErrorCode;
using snow::image::Format;
using snow::image::Frame;
using snow::image::Image;
using snow::image::ImageView;
using snow::image::PixelSink;
using snow::image::Result;
using snow::image::Service;
using snow::image::Status;

class FailingByteSink final : public snow::image::ByteSink {
  public:
    Result<void> write(std::span<const std::byte>) override {
        ++writes;
        return Status::error(ErrorCode::io_error, "forced sink failure");
    }
    Result<std::uint64_t> position() const override {
        return std::uint64_t{0};
    }
    Result<void> seek(std::uint64_t) override {
        return {};
    }
    Result<void> flush() override {
        return {};
    }
    bool seekable() const noexcept override {
        return false;
    }

    int writes = 0;
};

class CancellingByteSink final : public snow::image::ByteSink {
  public:
    explicit CancellingByteSink(std::stop_source* cancellation) : cancellation_(cancellation) {}
    Result<void> write(std::span<const std::byte>) override {
        ++writes;
        cancellation_->request_stop();
        return {};
    }
    Result<std::uint64_t> position() const override {
        return std::uint64_t{0};
    }
    Result<void> seek(std::uint64_t) override {
        return {};
    }
    Result<void> flush() override {
        return {};
    }
    bool seekable() const noexcept override {
        return false;
    }

    int writes = 0;

  private:
    std::stop_source* cancellation_ = nullptr;
};

class FlushFailingByteSink final : public snow::image::ByteSink {
  public:
    Result<void> write(std::span<const std::byte> source) override {
        bytes += source.size();
        return {};
    }
    Result<std::uint64_t> position() const override {
        return bytes;
    }
    Result<void> seek(std::uint64_t) override {
        return Status::error(ErrorCode::io_error, "flush sink is not seekable");
    }
    Result<void> flush() override {
        ++flushes;
        return Status::error(ErrorCode::io_error, "forced flush failure");
    }
    bool seekable() const noexcept override {
        return false;
    }

    std::uint64_t bytes = 0;
    int flushes = 0;
};

class BoundedCollectingSink final : public snow::image::ByteSink {
  public:
    Result<void> write(std::span<const std::byte> source) override {
        maximum_write = std::max(maximum_write, source.size());
        ++writes;
        bytes.insert(bytes.end(), source.begin(), source.end());
        return {};
    }
    Result<std::uint64_t> position() const override {
        return bytes.size();
    }
    Result<void> seek(std::uint64_t) override {
        return Status::error(ErrorCode::io_error, "bounded sink is not seekable");
    }
    Result<void> flush() override {
        return {};
    }
    bool seekable() const noexcept override {
        return false;
    }

    std::vector<std::byte> bytes;
    std::size_t maximum_write = 0;
    int writes = 0;
};

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "FAILED: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message) {
    if (!condition)
        fail(message);
}

template <typename T> T take(Result<T> result, std::string_view context) {
    if (!result) {
        std::cerr << "FAILED: " << context << ": " << result.error().message << '\n';
        std::exit(1);
    }
    return std::move(result).value();
}

class CountingSink final : public PixelSink {
  public:
    Result<void> begin(const snow::image::DocumentInfo& document) override {
        width = document.canvas_width;
        height = document.canvas_height;
        return {};
    }
    Result<void> begin_frame(std::uint32_t, const snow::image::FrameInfo&) override {
        return {};
    }
    Result<void> write_rows(std::uint32_t, std::uint32_t row_count, std::size_t,
                            std::span<const std::byte> pixels) override {
        rows += row_count;
        bytes += pixels.size();
        return {};
    }
    Result<void> end_frame(std::uint32_t) override {
        return {};
    }
    Result<void> end() override {
        return {};
    }

    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t rows = 0;
    std::size_t bytes = 0;
};

class MappedOnlySource final : public snow::image::RasterSource {
  public:
    explicit MappedOnlySource(std::shared_ptr<snow::image::RasterStore> store)
        : store_(std::move(store)) {}

    const snow::image::DocumentDescriptor& descriptor() const noexcept override {
        return store_->descriptor();
    }
    snow::image::RasterAccess access() const noexcept override {
        return snow::image::RasterAccess::mapped_planes;
    }
    Result<void> read_rows(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::size_t,
                           std::span<std::byte>, std::stop_token) const override {
        return Status::error(ErrorCode::internal_error, "row-copy fallback must not be used");
    }
    Result<snow::image::MappedPlane> map_plane(std::uint32_t frame,
                                               std::uint32_t plane) const override {
        return store_->map_plane(frame, plane);
    }

  private:
    std::shared_ptr<snow::image::RasterStore> store_;
};

class RowRasterWriter final : public snow::image::RasterWriter {
  public:
    explicit RowRasterWriter(snow::image::DocumentDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {
        for (const auto& frame : descriptor_.frames) {
            std::vector<std::vector<std::byte>> frame_planes;
            frame_planes.reserve(frame.layout.planes.size());
            for (const auto& plane : frame.layout.planes) {
                const std::size_t row_bytes = take(plane.row_bytes(), "measure row-writer plane");
                frame_planes.emplace_back(row_bytes * plane.height);
            }
            planes_.push_back(std::move(frame_planes));
        }
    }

    const snow::image::DocumentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

    Result<void> write_rows(std::uint32_t frame_index, std::uint32_t plane_index,
                            std::uint32_t first_row, std::uint32_t row_count,
                            std::size_t source_stride, std::span<const std::byte> source,
                            std::stop_token stop) override {
        if (stop.stop_requested())
            return Status::error(ErrorCode::cancelled, "row writer cancelled");
        if (frame_index >= descriptor_.frames.size() ||
            plane_index >= descriptor_.frames[frame_index].layout.planes.size())
            return Status::error(ErrorCode::invalid_argument, "row writer plane is out of range");
        const auto& plane = descriptor_.frames[frame_index].layout.planes[plane_index];
        const std::size_t row_bytes = take(plane.row_bytes(), "measure row-writer destination");
        if (first_row > plane.height || row_count > plane.height - first_row ||
            source_stride < row_bytes ||
            (row_count != 0 && source.size() < source_stride * (row_count - 1U) + row_bytes))
            return Status::error(ErrorCode::invalid_argument, "row writer input is invalid");
        auto& destination = planes_[frame_index][plane_index];
        for (std::uint32_t row = 0; row < row_count; ++row) {
            std::memcpy(destination.data() + static_cast<std::size_t>(first_row + row) * row_bytes,
                        source.data() + static_cast<std::size_t>(row) * source_stride, row_bytes);
        }
        rows_written += row_count;
        return {};
    }

    Result<void> commit() override {
        committed = true;
        return {};
    }

    void abort() noexcept override {
        aborted = true;
    }

    const std::vector<std::byte>& plane(std::size_t index) const {
        return planes_.front()[index];
    }

    std::uint32_t rows_written = 0;
    bool committed = false;
    bool aborted = false;

  private:
    snow::image::DocumentDescriptor descriptor_;
    std::vector<std::vector<std::vector<std::byte>>> planes_;
};

class StorageSink final : public PixelSink {
  public:
    Result<void> begin(const snow::image::DocumentInfo& document) override {
        info = document;
        return {};
    }
    Result<void> begin_frame(std::uint32_t, const snow::image::FrameInfo&) override {
        return {};
    }
    std::span<std::byte> frame_storage(std::uint32_t frame_index, std::size_t requested_stride,
                                       std::size_t byte_size) override {
        if (frame_index != 0)
            return {};
        ++storage_requests;
        stride = requested_stride;
        pixels.resize(byte_size);
        return pixels;
    }
    Result<void> write_rows(std::uint32_t, std::uint32_t row_count, std::size_t,
                            std::span<const std::byte>) override {
        callback_rows += row_count;
        return {};
    }
    Result<void> end_frame(std::uint32_t) override {
        return {};
    }
    Result<void> end() override {
        ended = true;
        return {};
    }

    snow::image::DocumentInfo info;
    std::vector<std::byte> pixels;
    std::size_t stride = 0;
    std::uint32_t callback_rows = 0;
    int storage_requests = 0;
    bool ended = false;
};

class AnimationStreamingSink final : public PixelSink {
  public:
    Result<void> begin(const snow::image::DocumentInfo& document) override {
        info = document;
        return {};
    }
    Result<void> begin_frame(std::uint32_t frame_index,
                             const snow::image::FrameInfo& frame) override {
        if (frame_index != frames_started)
            return Status::error(ErrorCode::invalid_argument, "animation frame order mismatch");
        next_row = 0;
        current_height = frame.height;
        ++frames_started;
        return {};
    }
    Result<void> write_rows(std::uint32_t first_row, std::uint32_t row_count, std::size_t,
                            std::span<const std::byte> pixels) override {
        if (first_row != next_row || row_count > current_height - next_row || pixels.empty())
            return Status::error(ErrorCode::invalid_argument, "animation row publication mismatch");
        next_row += row_count;
        rows += row_count;
        return {};
    }
    Result<void> end_frame(std::uint32_t frame_index) override {
        if (frame_index != frames_ended || next_row != current_height)
            return Status::error(ErrorCode::invalid_argument,
                                 "animation frame completion mismatch");
        ++frames_ended;
        return {};
    }
    Result<void> end() override {
        ended = true;
        return {};
    }

    snow::image::DocumentInfo info;
    std::uint32_t frames_started = 0;
    std::uint32_t frames_ended = 0;
    std::uint32_t rows = 0;
    std::uint32_t next_row = 0;
    std::uint32_t current_height = 0;
    bool ended = false;
};

Document sample_document() {
    snow::image::MutableImage image = take(
        snow::image::MutableImage::allocate(2, 2, snow::image::kRgba8), "allocate sample image");
    constexpr std::array<std::uint8_t, 16> values{255, 0, 0,   255, 0,   255, 0,   128,
                                                  0,   0, 255, 64,  255, 255, 255, 0};
    for (std::size_t index = 0; index < values.size(); ++index) {
        image.pixels()[index] = static_cast<std::byte>(values[index]);
    }
    Document document;
    document.format = Format::bmp;
    document.canvas_width = 2;
    document.canvas_height = 2;
    Frame frame;
    frame.image = std::move(image).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

snow::image::ExrChannel float_channel(std::string name, std::initializer_list<float> values) {
    snow::image::ExrChannel channel;
    channel.name = std::move(name);
    channel.sample_type = snow::image::SampleType::floating_point;
    channel.bits_per_sample = 32;
    channel.samples.resize(values.size() * sizeof(float));
    std::memcpy(channel.samples.data(), values.begin(), channel.samples.size());
    return channel;
}

float first_float(const snow::image::ExrChannel& channel, std::size_t index = 0) {
    float value = 0.0F;
    std::memcpy(&value, channel.samples.data() + index * sizeof(float), sizeof(value));
    return value;
}

void test_format_mapping() {
    require(snow::image::format_from_extension("photo.JPEG") == Format::jpeg,
            "JPEG extension alias");
    require(snow::image::format_from_extension("cursor.cur") == Format::cur,
            "CUR extension mapping");
    require(snow::image::format_from_extension("without-extension") == Format::unknown,
            "unknown extension mapping");
}

void test_bmp_round_trip(Service& service) {
    auto encoded = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::bmp;
    Result<snow::image::EncodeResult> encode = service.encode(
        sample_document(), snow::image::memory_output(encoded, "sample.bmp"), options);
    require(encode.has_value(), "BMP encode succeeds");
    require(encoded->size() > 138, "BMP output has header and pixels");

    const auto input = snow::image::memory_input(encoded, "misleading.png");
    require(take(service.detect(input), "detect BMP") == Format::bmp,
            "content detection wins over extension");
    Document decoded = take(service.decode(input), "decode BMP");
    require(decoded.frames.size() == 1, "BMP has one frame");
    const ImageView pixels = decoded.frames.front().image.view();
    require(pixels.width == 2 && pixels.height == 2, "BMP dimensions survive round trip");
    require(pixels.pixels[0] == std::byte{0xFF} && pixels.pixels[1] == std::byte{0},
            "BMP red pixel survives round trip");
    require(pixels.pixels[7] == std::byte{0x80}, "BMP alpha survives round trip");

    CountingSink sink;
    Result<void> streamed = service.decode_to_sink(input, sink);
    require(streamed.has_value(), "streamed BMP decode succeeds");
    require(sink.width == 2 && sink.height == 2 && sink.rows == 2 && sink.bytes == 16,
            "streamed BMP decode emits complete rows");
}

void test_netpbm(Service& service) {
    constexpr char ppm[] = "P6\n# generated fixture\n2 1\n255\n\xFF\x00\x00\x00\x80\xFF";
    const auto bytes = std::as_bytes(std::span(ppm, sizeof(ppm) - 1U));
    const auto input = snow::image::memory_input(bytes, "sample.data");
    require(take(service.detect(input), "detect PPM") == Format::ppm, "PPM detected by content");
    Document decoded = take(service.decode(input), "decode PPM");
    const ImageView pixels = decoded.frames.front().image.view();
    require(pixels.format == snow::image::kRgb8, "PPM native layout is RGB8");
    require(pixels.pixels[0] == std::byte{0xFF} && pixels.pixels[4] == std::byte{0x80} &&
                pixels.pixels[5] == std::byte{0xFF},
            "PPM pixels decoded exactly");

    auto encoded = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::pgm;
    Result<snow::image::EncodeResult> status =
        service.encode(decoded, snow::image::memory_output(encoded), options);
    require(status.has_value(), "PGM encode from RGB succeeds");
    require(encoded->size() > 10 && (*encoded)[0] == std::byte{'P'} &&
                (*encoded)[1] == std::byte{'5'},
            "PGM binary header emitted");
}

void test_limits(Service& service) {
    constexpr std::string_view ppm = "P6\n2 2\n255\nabcdefghijkl";
    snow::image::DecodeOptions options;
    options.limits.maximum_pixels = 3;
    Result<Document> decoded = service.decode(
        snow::image::memory_input(std::as_bytes(std::span(ppm.data(), ppm.size()))), options);
    require(!decoded && decoded.error().code == ErrorCode::limit_exceeded,
            "decode rejects configured pixel limit");
}

bool supports(const Service& service, Format format, snow::image::CodecCapability capability) {
    return std::ranges::any_of(
        service.formats(), [format, capability](const snow::image::FormatCapability& entry) {
            return entry.format == format &&
                   snow::image::has_capability(entry.capabilities, capability);
        });
}

void test_png_round_trip(Service& service) {
    if (!supports(service, Format::png, snow::image::CodecCapability::encode))
        return;
    require(snow::image::compression_backend_version(Format::png).find("zlib-ng") !=
                std::string_view::npos,
            "PNG uses the zlib-ng compatibility backend");
    auto encoded = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::png;
    Result<snow::image::EncodeResult> status = service.encode(
        sample_document(), snow::image::memory_output(encoded, "sample.png"), options);
    require(status.has_value(), "PNG encode succeeds");
    require(status.value().bytes_written == encoded->size() &&
                status.value().round_trip == snow::image::PixelRoundTrip::exact,
            "PNG encode reports its exact byte count and sample round trip");
    require(take(service.detect(snow::image::memory_input(encoded)), "detect PNG") == Format::png,
            "PNG detected by signature");
    Document decoded = take(service.decode(snow::image::memory_input(encoded)), "decode PNG");
    require(decoded.frames.front().image.pixels()[7] == std::byte{0x80},
            "PNG alpha survives lossless round trip");
    require(decoded.frames.front().image.pixels()[10] == std::byte{0xFF},
            "PNG color survives lossless round trip");
    CountingSink sink;
    snow::image::DecodeOptions streamed_options;
    streamed_options.limits.maximum_owned_output_bytes = 1;
    Result<void> streamed =
        service.decode_to_sink(snow::image::memory_input(encoded), sink, streamed_options);
    require(streamed.has_value() && sink.rows == 2 && sink.bytes == 16,
            "PNG row streaming bypasses the owning output limit");

    const snow::image::EncoderInfo* info = service.encoder_info(Format::png);
    require(info &&
                snow::image::has_feature(info->features, snow::image::EncoderFeature::pixel_exact),
            "PNG advertises exact preservation of accepted input samples");
    auto failing = std::make_shared<FailingByteSink>();
    status = service.encode(sample_document(), snow::image::Output{failing, "failed.png"}, options);
    require(!status && status.error().code == ErrorCode::io_error && failing->writes == 1,
            "libpng propagates direct ByteSink failures");
    std::stop_source cancellation;
    auto cancelling = std::make_shared<CancellingByteSink>(&cancellation);
    status = service.encode(sample_document(), snow::image::Output{cancelling, "cancelled.png"},
                            options, cancellation.get_token());
    require(!status && status.error().code == ErrorCode::cancelled && cancelling->writes == 1,
            "libpng observes cancellation during streamed row encoding");

    snow::image::MutableImage sixteen =
        take(snow::image::MutableImage::allocate(2, 1, snow::image::kRgba16),
             "allocate 16-bit PNG fixture");
    const std::array<std::uint16_t, 8> sixteen_values{0x0000U, 0x1234U, 0x8000U, 0xFFFFU,
                                                      0xFFFFU, 0xABCDU, 0x0001U, 0x4567U};
    std::memcpy(sixteen.pixels().data(), sixteen_values.data(), sizeof(sixteen_values));
    Document sixteen_document;
    sixteen_document.format = Format::png;
    sixteen_document.canvas_width = 2;
    sixteen_document.canvas_height = 1;
    sixteen_document.metadata.horizontal_dpi = 300.0;
    sixteen_document.metadata.vertical_dpi = 144.0;
    sixteen_document.metadata.comment = "snow PNG metadata";
    sixteen_document.metadata.xmp = {std::byte{'<'}, std::byte{'x'}, std::byte{'/'},
                                     std::byte{'>'}};
    sixteen_document.color.primaries = snow::image::ColorPrimaries::srgb;
    sixteen_document.color.transfer = snow::image::TransferFunction::srgb;
    Frame sixteen_frame;
    sixteen_frame.image = std::move(sixteen).freeze();
    sixteen_document.frames.push_back(std::move(sixteen_frame));
    auto sixteen_encoded = std::make_shared<std::vector<std::byte>>();
    options.interlaced = false;
    options.preserve_metadata = true;
    status = service.encode(sixteen_document, snow::image::memory_output(sixteen_encoded), options);
    require(status.has_value(), "16-bit metadata PNG encodes");
    const auto sixteen_info = take(service.inspect(snow::image::memory_input(sixteen_encoded)),
                                   "inspect 16-bit metadata PNG");
    const bool metadata_ok = sixteen_info.frames.front().native_format == snow::image::kRgba16 &&
                             sixteen_info.metadata.horizontal_dpi &&
                             std::abs(*sixteen_info.metadata.horizontal_dpi - 300.0) < 0.02 &&
                             sixteen_info.metadata.vertical_dpi &&
                             std::abs(*sixteen_info.metadata.vertical_dpi - 144.0) < 0.02 &&
                             sixteen_info.metadata.comment == "snow PNG metadata" &&
                             sixteen_info.metadata.xmp == sixteen_document.metadata.xmp &&
                             sixteen_info.color.primaries == snow::image::ColorPrimaries::srgb;
    if (!metadata_ok) {
        std::cerr << "PNG metadata mismatch: format="
                  << static_cast<int>(sixteen_info.frames.front().native_format.bits_per_channel)
                  << " dpi=" << sixteen_info.metadata.horizontal_dpi.value_or(0.0) << ','
                  << sixteen_info.metadata.vertical_dpi.value_or(0.0) << " comment='"
                  << sixteen_info.metadata.comment << "' xmp=" << sixteen_info.metadata.xmp.size()
                  << " primaries=" << static_cast<int>(sixteen_info.color.primaries) << '\n';
    }
    require(metadata_ok, "PNG inspection preserves native depth and contract metadata");
    const std::filesystem::path sixteen_path =
        std::filesystem::temp_directory_path() / "snow-image-rgba16-test.srs";
    std::error_code sixteen_ignored;
    std::filesystem::remove(sixteen_path, sixteen_ignored);
    auto sixteen_store =
        take(service.decode_to_store(snow::image::memory_input(sixteen_encoded), sixteen_path),
             "stream 16-bit PNG into raster store");
    std::array<std::byte, sizeof(sixteen_values)> sixteen_decoded{};
    const bool sixteen_read =
        sixteen_store->read_rows(0, 0, 0, 1, sizeof(sixteen_values), sixteen_decoded).has_value();
    const bool sixteen_equal =
        sixteen_read && std::equal(sixteen_decoded.begin(), sixteen_decoded.end(),
                                   reinterpret_cast<const std::byte*>(sixteen_values.data()));
    if (!sixteen_equal) {
        std::cerr << "16-bit PNG mismatch:";
        for (std::size_t index = 0; index < sixteen_values.size(); ++index) {
            std::uint16_t value = 0;
            std::memcpy(&value, sixteen_decoded.data() + index * sizeof(value), sizeof(value));
            std::cerr << ' ' << std::hex << value;
        }
        std::cerr << std::dec << '\n';
    }
    require(sixteen_equal, "PNG raster decode preserves every 16-bit sample");
    sixteen_store.reset();
    std::filesystem::remove(sixteen_path, sixteen_ignored);

    const std::array<snow::image::PixelFormat, 2> opaqueAlphaFormats{
        snow::image::PixelFormat{snow::image::SampleType::unsigned_integer,
                                 snow::image::ChannelLayout::gray_alpha,
                                 snow::image::AlphaMode::straight, 8, true},
        snow::image::kBgra8};
    for (const snow::image::PixelFormat& format : opaqueAlphaFormats) {
        const std::uint32_t fixtureWidth =
            format.channels == snow::image::ChannelLayout::bgra ? 300U : 7U;
        const std::uint32_t fixtureHeight =
            format.channels == snow::image::ChannelLayout::bgra ? 2U : 5U;
        snow::image::MutableImage pixels =
            take(snow::image::MutableImage::allocate(fixtureWidth, fixtureHeight, format),
                 "allocate opaque PNG write-transform fixture");
        for (std::uint32_t y = 0; y < pixels.height(); ++y) {
            for (std::uint32_t x = 0; x < pixels.width(); ++x) {
                std::byte* pixel = pixels.pixels().data() +
                                   static_cast<std::size_t>(y) * pixels.row_stride() +
                                   static_cast<std::size_t>(x) * format.channel_count();
                if (format.channels == snow::image::ChannelLayout::gray_alpha) {
                    pixel[0] = static_cast<std::byte>((x * 23U + y * 11U) & 0xFFU);
                    pixel[1] = std::byte{0xFF};
                } else {
                    pixel[0] = static_cast<std::byte>((x + y * 101U + 3U) & 0xFFU);
                    pixel[1] = static_cast<std::byte>(((x >> 8U) + 9U) & 0xFFU);
                    pixel[2] = static_cast<std::byte>((x + 17U) & 0xFFU);
                    pixel[3] = std::byte{0xFF};
                }
            }
        }
        Document fixture;
        fixture.canvas_width = pixels.width();
        fixture.canvas_height = pixels.height();
        Frame frame;
        frame.image = std::move(pixels).freeze();
        fixture.frames.push_back(std::move(frame));
        auto transformed = std::make_shared<std::vector<std::byte>>();
        options.interlaced = true;
        options.verified_alpha_content = snow::image::AlphaContent::opaque;
        status = service.encode(fixture, snow::image::memory_output(transformed), options);
        require(status.has_value(),
                "opaque alpha PNG encodes through libpng's write-side filler transform");
        const auto inspected = take(service.inspect(snow::image::memory_input(transformed)),
                                    "inspect opaque interlaced PNG");
        require(!inspected.frames.empty() && !inspected.frames.front().has_alpha,
                "opaque gray-alpha and BGRA PNG omit alpha without row repacking");
        Document roundTrip = take(service.decode(snow::image::memory_input(transformed)),
                                  "decode opaque interlaced PNG");
        require(roundTrip.frames.front().image.width() == fixtureWidth &&
                    roundTrip.frames.front().image.height() == fixtureHeight,
                "opaque filler-transformed Adam7 PNG round-trips its dimensions");
        const Image& decodedImage = roundTrip.frames.front().image;
        if (format.channels == snow::image::ChannelLayout::gray_alpha) {
            require(decodedImage.format() == snow::image::kRgba8 &&
                        decodedImage.pixels()[4] == std::byte{23} &&
                        decodedImage.pixels()[5] == std::byte{23} &&
                        decodedImage.pixels()[6] == std::byte{23} &&
                        decodedImage.pixels()[7] == std::byte{0xFF},
                    "gray-alpha filler removal preserves gray samples");
        } else {
            require(decodedImage.format() == snow::image::kRgba8 &&
                        decodedImage.pixels()[4] == std::byte{18} &&
                        decodedImage.pixels()[5] == std::byte{9} &&
                        decodedImage.pixels()[6] == std::byte{4} &&
                        decodedImage.pixels()[7] == std::byte{0xFF},
                    "BGRA filler removal and BGR transform preserve RGB samples");
        }
        options.verified_alpha_content.reset();
    }
}

void test_svg_vector_round_trip(Service& service) {
    if (!supports(service, Format::svg, snow::image::CodecCapability::encode))
        return;
    constexpr std::string_view svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="3"><rect width="4" height="3" fill="#ff0000"/></svg>)";
    const auto input =
        snow::image::memory_input(std::as_bytes(std::span(svg.data(), svg.size())), "vector.svg");
    require(take(service.detect(input), "detect SVG") == Format::svg, "SVG detected from markup");
    Document decoded = take(service.decode(input), "decode SVG");
    require(decoded.canvas_width == 4 && decoded.canvas_height == 3,
            "SVG intrinsic dimensions are preserved");
    require(decoded.vector.has_value() && !decoded.vector->gzip_compressed &&
                decoded.vector->source.size() == svg.size(),
            "SVG source is preserved for vector encoding");
    const ImageView pixels = decoded.frames.front().image.view();
    require(pixels.pixels[0] == std::byte{0xFF} && pixels.pixels[1] == std::byte{0} &&
                pixels.pixels[2] == std::byte{0} && pixels.pixels[3] == std::byte{0xFF},
            "SVG rasterization produces the expected opaque red pixel");

    auto svgz = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::svgz;
    Result<snow::image::EncodeResult> status =
        service.encode(decoded, snow::image::memory_output(svgz), options);
    require(status.has_value(), "SVGZ encode from preserved vector succeeds");
    const auto svgz_input = snow::image::memory_input(svgz, "misleading.svg");
    require(take(service.detect(svgz_input), "detect SVGZ") == Format::svgz,
            "SVGZ gzip signature wins over extension");
    Document decoded_svgz = take(service.decode(svgz_input), "decode SVGZ");
    require(decoded_svgz.vector.has_value() && decoded_svgz.vector->gzip_compressed,
            "SVGZ compressed source is preserved");

    auto transcoded = std::make_shared<std::vector<std::byte>>();
    options.format = Format::svg;
    status = service.encode(decoded_svgz, snow::image::memory_output(transcoded), options);
    require(status.has_value() && transcoded->size() == svg.size(),
            "SVGZ transcodes back to plain SVG");
    require(std::equal(transcoded->begin(), transcoded->end(),
                       reinterpret_cast<const std::byte*>(svg.data())),
            "SVGZ transcode preserves the original XML bytes");

    options.format = Format::svg;
    status = service.encode(sample_document(), snow::image::memory_output(transcoded), options);
    require(!status && status.error().code == ErrorCode::unsupported_feature,
            "SVG raster-to-vector encoding is rejected");
}

void test_heif_family(Service& service) {
    if (!supports(service, Format::avif, snow::image::CodecCapability::encode))
        return;
    Document still = sample_document();
    still.format = Format::avif;
    still.metadata.xmp.assign({std::byte{'<'}, std::byte{'x'}, std::byte{'/'}, std::byte{'>'}});
    still.color.primaries = snow::image::ColorPrimaries::display_p3;
    still.color.transfer = snow::image::TransferFunction::srgb;
    auto encoded = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::avif;
    options.lossless = true;
    options.quality = 100;
    Result<snow::image::EncodeResult> status =
        service.encode(still, snow::image::memory_output(encoded), options);
    require(status.has_value(), "AVIF lossless encode succeeds");
    const auto input = snow::image::memory_input(encoded, "misleading.heic");
    require(take(service.detect(input), "detect AVIF") == Format::avif,
            "AVIF brand wins over HEIC extension");
    Document decoded = take(service.decode(input), "decode AVIF");
    require(decoded.frames.size() == 1 && decoded.canvas_width == 2 && decoded.canvas_height == 2,
            "AVIF still dimensions survive round trip");
    require(decoded.metadata.xmp == still.metadata.xmp, "AVIF XMP metadata is preserved");
    require(decoded.color.primaries == snow::image::ColorPrimaries::display_p3,
            "AVIF NCLX color primaries are preserved");
    require(decoded.frames.front().image.pixels()[7] == std::byte{0x80},
            "AVIF alpha survives lossless round trip");
    require(std::equal(decoded.frames.front().image.pixels().begin(),
                       decoded.frames.front().image.pixels().end(),
                       still.frames.front().image.pixels().begin()),
            "AVIF lossless encoding preserves RGB values without chroma or matrix rounding");
    StorageSink avif_sink;
    require(service.decode_to_sink(input, avif_sink).has_value() && avif_sink.ended &&
                avif_sink.storage_requests == 1 && avif_sink.callback_rows == 0 &&
                avif_sink.pixels.size() == 16,
            "AVIF stills copy directly from the codec plane into sink storage");

    snow::image::EncodeOptions native_encode = options;
    native_encode.lossless = false;
    native_encode.quality = 92;
    auto native_bytes = std::make_shared<std::vector<std::byte>>();
    require(
        service
            .encode(still, snow::image::memory_output(native_bytes, "native.avif"), native_encode)
            .has_value(),
        "lossy AVIF native-plane fixture encodes");
    snow::image::DecodeOptions native_options;
    native_options.raster_layout = snow::image::RasterLayoutPolicy::native;
    const auto native_descriptor =
        take(service.inspect_raster(snow::image::memory_input(native_bytes), native_options),
             "inspect native AVIF raster");
    const auto& native_frame = native_descriptor.frames.front();
    require(
        native_frame.layout.color_model == snow::image::ColorModel::ycbcr &&
            native_frame.layout.color_range == snow::image::ColorRange::full &&
            (native_frame.layout.chroma_subsampling == snow::image::ChromaSubsampling::yuv420 ||
             native_frame.layout.chroma_subsampling == snow::image::ChromaSubsampling::yuv422 ||
             native_frame.layout.chroma_subsampling == snow::image::ChromaSubsampling::yuv444) &&
            native_frame.layout.planes.size() == 4 &&
            native_frame.layout.planes[0].semantic == snow::image::PlaneSemantic::luma &&
            native_frame.layout.planes[3].semantic == snow::image::PlaneSemantic::alpha,
        "native AVIF inspection exposes codec-preferred SDR YCbCr and alpha planes");
    const std::filesystem::path native_path =
        std::filesystem::temp_directory_path() /
        ("snow-image-native-avif-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".srs");
    std::error_code native_ignored;
    std::filesystem::remove(native_path, native_ignored);
    auto native_store = take(service.decode_to_store(snow::image::memory_input(native_bytes),
                                                     native_path, native_options),
                             "decode native AVIF to raster store");
    require(native_store->complete() &&
                native_store->descriptor().frames.front().layout == native_frame.layout,
            "native AVIF publishes directly into mapped planar storage");
    RowRasterWriter native_rows(native_descriptor);
    require(
        service.decode_into(snow::image::memory_input(native_bytes), native_rows, native_options)
                .has_value() &&
            native_rows.committed && !native_rows.aborted,
        "native AVIF publishes codec planes to row-only writers");
    std::vector<std::byte> native_rgba(16U);
    snow::image::MutablePlaneView native_view{2, 2, snow::image::kRgba8, 8U, native_rgba};
    require(snow::image::read_rgba8_region(*native_store, 0, {0, 0, 2, 2}, native_view).has_value(),
            "native AVIF planes convert to a bounded RGBA region");
    native_store.reset();
    std::filesystem::remove(native_path, native_ignored);

    Document collection = sample_document();
    collection.format = Format::avif;
    snow::image::MutableImage second =
        take(snow::image::MutableImage::allocate(1, 1, snow::image::kRgba8),
             "allocate AVIF collection image");
    second.pixels()[0] = std::byte{0};
    second.pixels()[1] = std::byte{0};
    second.pixels()[2] = std::byte{0xFF};
    second.pixels()[3] = std::byte{0xFF};
    Frame collection_frame;
    collection_frame.image = std::move(second).freeze();
    collection.frames.push_back(std::move(collection_frame));
    auto collection_bytes = std::make_shared<std::vector<std::byte>>();
    status = service.encode(collection, snow::image::memory_output(collection_bytes), options);
    require(status.has_value(), "AVIF image collection encode succeeds");
    const snow::image::DocumentInfo collection_info = take(
        service.inspect(snow::image::memory_input(collection_bytes)), "inspect AVIF collection");
    require(collection_info.frames.size() == 2,
            "AVIF collection inspection exposes every top-level image");
    Document decoded_collection =
        take(service.decode(snow::image::memory_input(collection_bytes)), "decode AVIF collection");
    require(decoded_collection.frames.size() == 2 &&
                decoded_collection.frames[1].image.width() == 1,
            "AVIF collection decode preserves distinct image sizes");
    AnimationStreamingSink collection_sink;
    require(service.decode_to_sink(snow::image::memory_input(collection_bytes), collection_sink)
                    .has_value() &&
                collection_sink.ended && collection_sink.frames_started == 2 &&
                collection_sink.frames_ended == 2 && collection_sink.rows == 3 &&
                collection_sink.info.frames.size() == 2 &&
                collection_sink.info.frames[0].width == 2 &&
                collection_sink.info.frames[1].width == 1,
            "AVIF collections publish one bounded image at a time");

    Document animation = sample_document();
    animation.format = Format::avif;
    animation.loop_count = 0;
    animation.frames[0].duration = std::chrono::milliseconds(40);
    Frame animation_frame;
    snow::image::MutableImage animation_pixels =
        take(snow::image::MutableImage::allocate(2, 2, snow::image::kRgba8),
             "allocate AVIF animation frame");
    std::fill(animation_pixels.pixels().begin(), animation_pixels.pixels().end(), std::byte{0xFF});
    animation_frame.image = std::move(animation_pixels).freeze();
    animation_frame.duration = std::chrono::milliseconds(70);
    animation.frames.push_back(std::move(animation_frame));
    auto animation_bytes = std::make_shared<std::vector<std::byte>>();
    status = service.encode(animation, snow::image::memory_output(animation_bytes), options);
    require(status.has_value(), "AVIF sequence encode succeeds");
    AnimationStreamingSink animation_sink;
    require(service.decode_to_sink(snow::image::memory_input(animation_bytes), animation_sink)
                    .has_value() &&
                animation_sink.ended && animation_sink.frames_started == 2 &&
                animation_sink.frames_ended == 2 && animation_sink.rows == 4 &&
                animation_sink.info.loop_count == 0 && animation_sink.info.frames.size() == 2 &&
                animation_sink.info.frames[0].duration == std::chrono::milliseconds(40) &&
                animation_sink.info.frames[1].duration == std::chrono::milliseconds(70),
            "AVIF sequences decode once and publish one bounded frame at a time");
    Document bounded_animation;
    bounded_animation.format = Format::avif;
    bounded_animation.canvas_width = 64;
    bounded_animation.canvas_height = 64;
    for (int index = 0; index < 2; ++index) {
        snow::image::MutableImage bounded_pixels =
            take(snow::image::MutableImage::allocate(64, 64, snow::image::kRgba8),
                 "allocate bounded AVIF sequence frame");
        std::fill(bounded_pixels.pixels().begin(), bounded_pixels.pixels().end(),
                  static_cast<std::byte>(index == 0 ? 0x44 : 0xBB));
        Frame bounded_frame;
        bounded_frame.image = std::move(bounded_pixels).freeze();
        bounded_frame.duration = std::chrono::milliseconds(50);
        bounded_animation.frames.push_back(std::move(bounded_frame));
    }
    auto bounded_animation_bytes = std::make_shared<std::vector<std::byte>>();
    require(
        service
            .encode(bounded_animation, snow::image::memory_output(bounded_animation_bytes), options)
            .has_value(),
        "bounded AVIF sequence fixture encodes");
    snow::image::DecodeOptions animation_limited;
    animation_limited.limits.maximum_working_bytes = 64U * 64U * 4U - 1U;
    AnimationStreamingSink animation_limited_sink;
    const Result<void> animation_limited_status =
        service.decode_to_sink(snow::image::memory_input(bounded_animation_bytes),
                               animation_limited_sink, animation_limited);
    require(!animation_limited_status &&
                animation_limited_status.error().code == ErrorCode::limit_exceeded &&
                !animation_limited_sink.ended,
            "AVIF sequence streaming enforces its one-frame working-memory limit");
    Document decoded_animation =
        take(service.decode(snow::image::memory_input(animation_bytes)), "decode AVIF sequence");
    require(decoded_animation.frames.size() == 2, "AVIF sequence preserves frame count");
    require(decoded_animation.frames[0].duration == std::chrono::milliseconds(40) &&
                decoded_animation.frames[1].duration == std::chrono::milliseconds(70),
            "AVIF sequence preserves frame durations");

    if (supports(service, Format::heif, snow::image::CodecCapability::encode)) {
        options.format = Format::heif;
        auto heic = std::make_shared<std::vector<std::byte>>();
        status = service.encode(sample_document(), snow::image::memory_output(heic), options);
        require(status.has_value(), "HEIC encode succeeds when the HEVC backend is available");
        require(take(service.detect(snow::image::memory_input(heic)), "detect HEIC") ==
                    Format::heif,
                "HEIC is detected as the HEIF family");
        Document decoded_heic =
            take(service.decode(snow::image::memory_input(heic)), "decode HEIC");
        require(decoded_heic.canvas_width == 2 && decoded_heic.canvas_height == 2,
                "HEIC dimensions survive round trip");
        StorageSink heic_sink;
        require(service.decode_to_sink(snow::image::memory_input(heic), heic_sink).has_value() &&
                    heic_sink.ended && heic_sink.storage_requests == 1 &&
                    heic_sink.callback_rows == 0,
                "HEIC stills copy directly from the codec plane into sink storage");
    }
}

void test_jpeg_xl(Service& service) {
    if (!supports(service, Format::jxl, snow::image::CodecCapability::encode))
        return;
    Document still = sample_document();
    still.format = Format::jxl;
    still.metadata.xmp.assign({std::byte{'<'}, std::byte{'j'}, std::byte{'x'}, std::byte{'l'},
                               std::byte{'/'}, std::byte{'>'}});
    auto encoded = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::jxl;
    options.lossless = true;
    options.progressive = true;
    Result<snow::image::EncodeResult> status =
        service.encode(still, snow::image::memory_output(encoded), options);
    require(status.has_value(), "JPEG XL lossless encode succeeds");
    const auto input = snow::image::memory_input(encoded, "misleading.png");
    require(take(service.detect(input), "detect JPEG XL") == Format::jxl,
            "JPEG XL container signature wins over extension");
    const snow::image::DocumentInfo info = take(service.inspect(input), "inspect JPEG XL");
    require(info.canvas_width == 2 && info.canvas_height == 2 && info.frames.size() == 1,
            "JPEG XL inspection reports dimensions and frame count");
    require(info.metadata.xmp == still.metadata.xmp,
            "JPEG XL inspection preserves XMP box metadata");
    Document decoded = take(service.decode(input), "decode JPEG XL");
    require(decoded.frames.front().image.pixels().size() ==
                    still.frames.front().image.pixels().size() &&
                std::equal(decoded.frames.front().image.pixels().begin(),
                           decoded.frames.front().image.pixels().end(),
                           still.frames.front().image.pixels().begin()),
            "JPEG XL preserves RGBA pixels losslessly");
    require(decoded.metadata.xmp == still.metadata.xmp,
            "JPEG XL decode preserves XMP box metadata");
    StorageSink jxl_sink;
    require(service.decode_to_sink(input, jxl_sink).has_value() && jxl_sink.ended &&
                jxl_sink.storage_requests == 1 && jxl_sink.callback_rows == 0 &&
                jxl_sink.pixels ==
                    std::vector<std::byte>(still.frames.front().image.pixels().begin(),
                                           still.frames.front().image.pixels().end()),
            "JPEG XL decodes directly into sink-owned frame storage");

    snow::image::DecodeOptions scaled_options;
    scaled_options.maximum_extent = 1;
    Document scaled = take(service.decode(input, scaled_options), "decode scaled JPEG XL");
    require(scaled.canvas_width == 1 && scaled.canvas_height == 1 &&
                scaled.frames.front().image.width() == 1,
            "JPEG XL maximum extent produces a bounded raster");

    Document animation = sample_document();
    animation.format = Format::jxl;
    animation.loop_count = 3;
    animation.frames[0].duration = std::chrono::milliseconds(25);
    snow::image::MutableImage second =
        take(snow::image::MutableImage::allocate(2, 2, snow::image::kRgba8),
             "allocate JPEG XL animation frame");
    std::fill(second.pixels().begin(), second.pixels().end(), std::byte{0xCC});
    Frame second_frame;
    second_frame.image = std::move(second).freeze();
    second_frame.duration = std::chrono::milliseconds(65);
    animation.frames.push_back(std::move(second_frame));
    auto animation_bytes = std::make_shared<std::vector<std::byte>>();
    status = service.encode(animation, snow::image::memory_output(animation_bytes), options);
    require(status.has_value(), "JPEG XL animation encode succeeds");
    AnimationStreamingSink jxl_animation_sink;
    require(service.decode_to_sink(snow::image::memory_input(animation_bytes), jxl_animation_sink)
                    .has_value() &&
                jxl_animation_sink.ended && jxl_animation_sink.frames_started == 2 &&
                jxl_animation_sink.frames_ended == 2 && jxl_animation_sink.rows == 4 &&
                jxl_animation_sink.info.frames.size() == 2 &&
                jxl_animation_sink.info.frames[0].duration == std::chrono::milliseconds(25) &&
                jxl_animation_sink.info.frames[1].duration == std::chrono::milliseconds(65),
            "JPEG XL animation publishes one bounded frame at a time");
    snow::image::DecodeOptions jxl_limited;
    jxl_limited.limits.maximum_working_bytes = 15;
    AnimationStreamingSink jxl_limited_sink;
    const Result<void> jxl_limited_status = service.decode_to_sink(
        snow::image::memory_input(animation_bytes), jxl_limited_sink, jxl_limited);
    require(!jxl_limited_status && jxl_limited_status.error().code == ErrorCode::limit_exceeded &&
                !jxl_limited_sink.ended,
            "JPEG XL animation enforces its one-frame working-memory limit");
    Document decoded_animation = take(service.decode(snow::image::memory_input(animation_bytes)),
                                      "decode JPEG XL animation");
    require(decoded_animation.frames.size() == 2 && decoded_animation.loop_count == 3,
            "JPEG XL animation preserves frame count and loop count");
    require(decoded_animation.frames[0].duration == std::chrono::milliseconds(25) &&
                decoded_animation.frames[1].duration == std::chrono::milliseconds(65),
            "JPEG XL animation preserves exact frame durations");
    require(decoded_animation.frames[1].image.pixels()[0] == std::byte{0xCC},
            "JPEG XL animation preserves the second frame pixels");
}

void test_openexr(Service& service) {
    if (!supports(service, Format::exr, snow::image::CodecCapability::encode))
        return;

    auto raster_bytes = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::exr;
    Result<snow::image::EncodeResult> status =
        service.encode(sample_document(), snow::image::memory_output(raster_bytes), options);
    require(status.has_value(), "OpenEXR raster fallback encode succeeds");
    require(take(service.detect(snow::image::memory_input(raster_bytes)), "detect OpenEXR") ==
                Format::exr,
            "OpenEXR signature is detected");
    Document raster =
        take(service.decode(snow::image::memory_input(raster_bytes)), "decode raster OpenEXR");
    require(raster.frames.size() == 1 &&
                raster.frames[0].image.format() == snow::image::kRgba32Float,
            "OpenEXR raster fallback decodes to linear RGBA32F");
    float raster_red = 0.0F;
    float raster_alpha = 0.0F;
    std::memcpy(&raster_red, raster.frames[0].image.pixels().data(), sizeof(float));
    std::memcpy(&raster_alpha, raster.frames[0].image.pixels().data() + 7U * sizeof(float),
                sizeof(float));
    require(std::abs(raster_red - 1.0F) < 0.0001F &&
                std::abs(raster_alpha - 128.0F / 255.0F) < 0.0001F,
            "OpenEXR raster fallback preserves color and alpha values");

    Document source;
    source.format = Format::exr;

    snow::image::ExrPart layered;
    layered.name = "layered";
    layered.type = snow::image::ExrPartType::scanline;
    layered.data_window = {10, 20, 11, 20};
    layered.display_window = {0, 0, 31, 31};
    layered.channels.push_back(float_channel("beauty.R", {0.25F, 0.75F}));
    layered.channels.push_back(float_channel("beauty.G", {0.5F, 0.25F}));
    layered.channels.push_back(float_channel("beauty.B", {0.75F, 0.5F}));
    layered.channels.push_back(float_channel("beauty.A", {1.0F, 0.5F}));
    snow::image::MetadataBlock opaque;
    opaque.type = "snowCustom";
    opaque.content_type = "snowOpaqueTest";
    opaque.data = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    layered.attributes.push_back(opaque);
    source.exr_parts.push_back(std::move(layered));

    snow::image::ExrPart tiled;
    tiled.name = "mipmap";
    tiled.type = snow::image::ExrPartType::tiled;
    tiled.data_window = {0, 0, 3, 3};
    tiled.display_window = {0, 0, 31, 31};
    tiled.tile_width = 2;
    tiled.tile_height = 2;
    tiled.level_mode = snow::image::ExrLevelMode::mipmap;
    tiled.channels.push_back(float_channel("R", {0.0F, 0.1F, 0.2F, 0.3F, 0.4F, 0.5F, 0.6F, 0.7F,
                                                 0.8F, 0.9F, 1.0F, 0.9F, 0.8F, 0.7F, 0.6F, 0.5F}));
    snow::image::ExrLevel mip1;
    mip1.x_level = 1;
    mip1.y_level = 1;
    mip1.data_window = {0, 0, 1, 1};
    mip1.channels.push_back(float_channel("R", {0.2F, 0.4F, 0.6F, 0.8F}));
    tiled.levels.push_back(std::move(mip1));
    snow::image::ExrLevel mip2;
    mip2.x_level = 2;
    mip2.y_level = 2;
    mip2.data_window = {0, 0, 0, 0};
    mip2.channels.push_back(float_channel("R", {0.5F}));
    tiled.levels.push_back(std::move(mip2));
    source.exr_parts.push_back(std::move(tiled));

    snow::image::ExrPart deep;
    deep.name = "deep";
    deep.type = snow::image::ExrPartType::deep_scanline;
    deep.data_window = {0, 0, 0, 0};
    deep.display_window = {0, 0, 31, 31};
    snow::image::DeepSamples samples;
    samples.counts = {2};
    samples.channels.push_back(float_channel("R", {1.0F, 0.0F}));
    samples.channels.push_back(float_channel("G", {0.0F, 0.0F}));
    samples.channels.push_back(float_channel("B", {0.0F, 1.0F}));
    samples.channels.push_back(float_channel("A", {0.5F, 1.0F}));
    samples.channels.push_back(float_channel("Z", {1.0F, 2.0F}));
    deep.deep_samples = std::move(samples);
    source.exr_parts.push_back(std::move(deep));

    snow::image::ExrPart ripmap;
    ripmap.name = "ripmap";
    ripmap.type = snow::image::ExrPartType::tiled;
    ripmap.data_window = {0, 0, 1, 1};
    ripmap.display_window = {0, 0, 31, 31};
    ripmap.tile_width = 1;
    ripmap.tile_height = 1;
    ripmap.level_mode = snow::image::ExrLevelMode::ripmap;
    ripmap.channels.push_back(float_channel("R", {0.1F, 0.2F, 0.3F, 0.4F}));
    snow::image::ExrLevel rip_x;
    rip_x.x_level = 1;
    rip_x.y_level = 0;
    rip_x.data_window = {0, 0, 0, 1};
    rip_x.channels.push_back(float_channel("R", {0.15F, 0.35F}));
    ripmap.levels.push_back(std::move(rip_x));
    snow::image::ExrLevel rip_y;
    rip_y.x_level = 0;
    rip_y.y_level = 1;
    rip_y.data_window = {0, 0, 1, 0};
    rip_y.channels.push_back(float_channel("R", {0.2F, 0.3F}));
    ripmap.levels.push_back(std::move(rip_y));
    snow::image::ExrLevel rip_xy;
    rip_xy.x_level = 1;
    rip_xy.y_level = 1;
    rip_xy.data_window = {0, 0, 0, 0};
    rip_xy.channels.push_back(float_channel("R", {0.25F}));
    ripmap.levels.push_back(std::move(rip_xy));
    source.exr_parts.push_back(std::move(ripmap));

    snow::image::ExrPart deep_tiled;
    deep_tiled.name = "deep-tiled";
    deep_tiled.type = snow::image::ExrPartType::deep_tiled;
    deep_tiled.data_window = {0, 0, 0, 0};
    deep_tiled.display_window = {0, 0, 31, 31};
    deep_tiled.tile_width = 1;
    deep_tiled.tile_height = 1;
    snow::image::DeepSamples tiled_samples;
    tiled_samples.counts = {1};
    tiled_samples.channels.push_back(float_channel("R", {0.75F}));
    tiled_samples.channels.push_back(float_channel("G", {0.5F}));
    tiled_samples.channels.push_back(float_channel("B", {0.25F}));
    tiled_samples.channels.push_back(float_channel("A", {1.0F}));
    deep_tiled.deep_samples = std::move(tiled_samples);
    source.exr_parts.push_back(std::move(deep_tiled));

    auto multipart_bytes = std::make_shared<std::vector<std::byte>>();
    status = service.encode(source, snow::image::memory_output(multipart_bytes), options);
    if (!status) {
        std::cerr << "FAILED: multipart, tiled, and deep OpenEXR encode succeeds: "
                  << status.error().message << '\n';
        std::exit(1);
    }
    const snow::image::DocumentInfo info = take(
        service.inspect(snow::image::memory_input(multipart_bytes)), "inspect multipart OpenEXR");
    require(info.exr_parts.size() == 5 &&
                info.exr_parts[1].level_mode == snow::image::ExrLevelMode::mipmap,
            "OpenEXR inspection exposes multipart and tiled semantics");
    Document decoded = take(service.decode(snow::image::memory_input(multipart_bytes)),
                            "decode multipart OpenEXR");
    require(decoded.exr_parts.size() == 5 && decoded.frames.size() == 5,
            "OpenEXR decode preserves every part and display raster");
    require(decoded.exr_parts[0].name == "layered" && decoded.exr_parts[0].attributes.size() == 1 &&
                decoded.exr_parts[0].attributes[0].data == opaque.data,
            "OpenEXR preserves part names and opaque attributes");
    require(decoded.exr_parts[1].levels.size() == 2 &&
                std::abs(first_float(decoded.exr_parts[1].levels[1].channels[0]) - 0.5F) < 0.0001F,
            "OpenEXR preserves all mip levels and arbitrary channels");
    const auto& compact_deep_samples = decoded.exr_parts[2].deep_samples;
    require(compact_deep_samples.has_value() &&
                compact_deep_samples->counts == std::vector<std::uint32_t>{2},
            "OpenEXR preserves compact deep sample counts");
    require(decoded.exr_parts[3].level_mode == snow::image::ExrLevelMode::ripmap &&
                decoded.exr_parts[3].levels.size() == 3,
            "OpenEXR preserves every ripmap level");
    const auto& tiled_deep_samples = decoded.exr_parts[4].deep_samples;
    require(decoded.exr_parts[4].type == snow::image::ExrPartType::deep_tiled &&
                tiled_deep_samples.has_value() &&
                tiled_deep_samples->counts == std::vector<std::uint32_t>{1},
            "OpenEXR preserves deep tiled samples");
    const auto deep_pixels = decoded.frames[2].image.pixels();
    float deep_red = 0.0F;
    float deep_blue = 0.0F;
    float deep_alpha = 0.0F;
    std::memcpy(&deep_red, deep_pixels.data(), sizeof(float));
    std::memcpy(&deep_blue, deep_pixels.data() + 2U * sizeof(float), sizeof(float));
    std::memcpy(&deep_alpha, deep_pixels.data() + 3U * sizeof(float), sizeof(float));
    require(std::abs(deep_red - 0.5F) < 0.0001F && std::abs(deep_blue - 0.5F) < 0.0001F &&
                std::abs(deep_alpha - 1.0F) < 0.0001F,
            "OpenEXR deep samples flatten in depth order");

    auto reencoded_bytes = std::make_shared<std::vector<std::byte>>();
    status = service.encode(decoded, snow::image::memory_output(reencoded_bytes), options);
    require(status.has_value(), "decoded OpenEXR document re-encodes without semantic loss");
    Document redecoded = take(service.decode(snow::image::memory_input(reencoded_bytes)),
                              "decode re-encoded OpenEXR");
    require(redecoded.exr_parts.size() == 5 && redecoded.exr_parts[3].levels.size() == 3 &&
                redecoded.exr_parts[4].deep_samples.has_value(),
            "OpenEXR re-encode preserves multipart, ripmap, and deep tiled structures");

    snow::image::DecodeOptions limited;
    limited.limits.maximum_deep_samples = 1;
    Result<Document> rejected = service.decode(snow::image::memory_input(multipart_bytes), limited);
    require(!rejected && rejected.error().code == ErrorCode::limit_exceeded,
            "OpenEXR deep decode enforces configured sample limits");
}

void test_jpeg_round_trip(Service& service) {
    if (!supports(service, Format::jpeg, snow::image::CodecCapability::encode))
        return;
    snow::image::MutableImage source_image =
        take(snow::image::MutableImage::allocate(7, 5, snow::image::kRgba8),
             "allocate odd-sized JPEG source");
    for (std::uint32_t y = 0; y < source_image.height(); ++y) {
        for (std::uint32_t x = 0; x < source_image.width(); ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * source_image.row_stride() + x * 4U;
            source_image.pixels()[offset] = static_cast<std::byte>((x * 29U + y * 7U) & 0xFFU);
            source_image.pixels()[offset + 1U] = static_cast<std::byte>((x * 3U + y * 43U) & 0xFFU);
            source_image.pixels()[offset + 2U] =
                static_cast<std::byte>((x * 17U + y * 13U) & 0xFFU);
            source_image.pixels()[offset + 3U] = std::byte{0xFF};
        }
    }
    Document source;
    source.canvas_width = source_image.width();
    source.canvas_height = source_image.height();
    Frame source_frame;
    source_frame.image = std::move(source_image).freeze();
    source.frames.push_back(std::move(source_frame));

    auto encoded = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::jpeg;
    options.quality = 95;
    options.progressive = true;
    options.chroma_subsampling = snow::image::ChromaSubsampling::yuv420;
    Result<snow::image::EncodeResult> status =
        service.encode(source, snow::image::memory_output(encoded, "sample.jpg"), options);
    require(status.has_value(), "JPEG encode succeeds");
    require(status.value().receipt.jpeg_chroma_subsampling ==
                snow::image::ChromaSubsampling::yuv420,
            "JPEG receipt reports manually selected 4:2:0 sampling");
    const std::array<std::byte, 2> progressiveMarker{std::byte{0xFF}, std::byte{0xC2}};
    require(std::search(encoded->begin(), encoded->end(), progressiveMarker.begin(),
                        progressiveMarker.end()) != encoded->end(),
            "progressive JPEG output uses a progressive frame marker");
    const auto input = snow::image::memory_input(encoded);
    require(take(service.detect(input), "detect JPEG") == Format::jpeg,
            "JPEG detected by signature");
    const snow::image::DocumentInfo info = take(service.inspect(input), "inspect JPEG");
    require(info.canvas_width == 7 && info.canvas_height == 5, "JPEG dimensions inspected");
    Document decoded = take(service.decode(input), "decode JPEG");
    require(decoded.frames.front().image.format() == snow::image::kRgb8,
            "JPEG RGB source decodes as native RGB8");
    StorageSink sink;
    require(service.decode_to_sink(input, sink).has_value() && sink.ended &&
                sink.storage_requests == 1 && sink.callback_rows == 0 &&
                sink.pixels.size() == 7U * 5U * 4U,
            "JPEG decodes directly into RGBA storage-backed sinks");

    snow::image::DecodeOptions native_options;
    native_options.raster_layout = snow::image::RasterLayoutPolicy::native;
    const snow::image::DocumentDescriptor native_descriptor =
        take(service.inspect_raster(input, native_options), "inspect native JPEG raster");
    require(native_descriptor.frames.size() == 1 && native_descriptor.canvas_width == 7 &&
                native_descriptor.canvas_height == 5,
            "native JPEG inspection preserves odd canvas dimensions");
    const auto& native_frame = native_descriptor.frames.front();
    require(native_frame.layout.color_model == snow::image::ColorModel::ycbcr &&
                native_frame.layout.alpha == snow::image::AlphaMode::none &&
                native_frame.layout.chroma_subsampling == snow::image::ChromaSubsampling::yuv420 &&
                native_frame.layout.color_range == snow::image::ColorRange::full &&
                native_frame.layout.planes.size() == 3,
            "native JPEG inspection exposes its YCbCr 4:2:0 layout");
    const auto& y_plane = native_frame.layout.planes[0];
    const auto& cb_plane = native_frame.layout.planes[1];
    const auto& cr_plane = native_frame.layout.planes[2];
    require(y_plane.semantic == snow::image::PlaneSemantic::luma &&
                cb_plane.semantic == snow::image::PlaneSemantic::chroma_blue &&
                cr_plane.semantic == snow::image::PlaneSemantic::chroma_red && y_plane.width == 8 &&
                y_plane.height == 6 && cb_plane.width == 4 && cb_plane.height == 3 &&
                cr_plane.width == 4 && cr_plane.height == 3 &&
                y_plane.format == snow::image::kGray8 && cb_plane.format == snow::image::kGray8 &&
                cr_plane.format == snow::image::kGray8,
            "native JPEG plane semantics and padded chroma geometry are exact");

    const std::filesystem::path store_path =
        std::filesystem::temp_directory_path() /
        ("snow-image-native-jpeg-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".srs");
    const std::filesystem::path packed_store_path = store_path.string() + ".packed";
    std::error_code ignored;
    std::filesystem::remove(store_path, ignored);
    std::filesystem::remove(packed_store_path, ignored);
    auto packed_store = take(service.decode_to_store(input, packed_store_path),
                             "decode packed JPEG directly to raster store");
    require(packed_store->complete() &&
                packed_store->descriptor().frames.front().layout.planes.size() == 1 &&
                packed_store->descriptor().frames.front().layout.planes.front().format ==
                    snow::image::kRgba8,
            "packed JPEG raster inspection matches its streaming RGBA decoder");
    auto store = take(service.decode_to_store(input, store_path, native_options),
                      "decode native JPEG directly to raster store");
    require(store->complete() && store->descriptor().frames.front().layout == native_frame.layout,
            "native JPEG decode commits the inspected planar layout");

    std::vector<std::byte> full_rgba(7U * 5U * 4U);
    snow::image::MutablePlaneView full_view{7, 5, snow::image::kRgba8, 7U * 4U, full_rgba};
    require(snow::image::read_rgba8_region(*store, 0, {0, 0, 7, 5}, full_view).has_value(),
            "native JPEG raster converts to a bounded RGBA region");
    std::vector<std::byte> odd_rgba(5U * 3U * 4U);
    snow::image::MutablePlaneView odd_view{5, 3, snow::image::kRgba8, 5U * 4U, odd_rgba};
    require(snow::image::read_rgba8_region(*store, 0, {1, 1, 5, 3}, odd_view).has_value(),
            "odd native JPEG region converts with a chroma-aligned halo");
    for (std::uint32_t row = 0; row < 3; ++row) {
        const auto row_offset = static_cast<std::ptrdiff_t>(row) * 20;
        const auto next_row_offset = (static_cast<std::ptrdiff_t>(row) + 1) * 20;
        const auto full_offset = ((static_cast<std::ptrdiff_t>(row) + 1) * 7 + 1) * 4;
        require(std::equal(odd_rgba.begin() + row_offset, odd_rgba.begin() + next_row_offset,
                           full_rgba.begin() + full_offset),
                "odd native JPEG tile pixels match the full-frame conversion crop");
    }

    RowRasterWriter row_writer(native_descriptor);
    require(service.decode_into(input, row_writer, native_options).has_value() &&
                row_writer.committed && !row_writer.aborted && row_writer.rows_written == 12,
            "native JPEG decode falls back to bounded planar row writes");
    for (std::uint32_t plane_index = 0; plane_index < 3; ++plane_index) {
        const auto mapping = take(store->map_plane(0, plane_index), "map native JPEG plane");
        const auto& plane = native_frame.layout.planes[plane_index];
        const std::size_t row_bytes = take(plane.row_bytes(), "measure native JPEG plane row");
        for (std::uint32_t row = 0; row < plane.height; ++row) {
            require(std::equal(row_writer.plane(plane_index).begin() +
                                   static_cast<std::ptrdiff_t>(row * row_bytes),
                               row_writer.plane(plane_index).begin() +
                                   static_cast<std::ptrdiff_t>((row + 1U) * row_bytes),
                               mapping.pixels.begin() +
                                   static_cast<std::ptrdiff_t>(row * mapping.row_stride)),
                    "mapped and row-write native JPEG decode paths agree");
        }
    }

    snow::image::DecodeOptions limited_native = native_options;
    limited_native.limits.maximum_working_bytes = 71;
    RowRasterWriter limited_writer(native_descriptor);
    const Result<void> limited_status = service.decode_into(input, limited_writer, limited_native);
    require(!limited_status && limited_status.error().code == ErrorCode::limit_exceeded &&
                limited_writer.aborted && !limited_writer.committed,
            "native JPEG row fallback enforces its aggregate working-memory limit");

    auto planar_encoded = std::make_shared<std::vector<std::byte>>();
    require(
        service.encode(*store, snow::image::memory_output(planar_encoded, "planar.jpg"), options)
                .has_value() &&
            !planar_encoded->empty(),
        "JPEG encoder consumes native mapped YCbCr planes directly");
    const auto reencoded_descriptor =
        take(service.inspect_raster(snow::image::memory_input(planar_encoded), native_options),
             "inspect re-encoded native JPEG");
    require(reencoded_descriptor.frames.front().layout.chroma_subsampling ==
                    snow::image::ChromaSubsampling::yuv420 &&
                reencoded_descriptor.frames.front().layout.planes[1].width == 4 &&
                reencoded_descriptor.frames.front().layout.planes[1].height == 3,
            "native JPEG re-encode preserves subsampling and chroma geometry");

    snow::image::DecodeOptions forced_packed = native_options;
    forced_packed.output_format = snow::image::kRgba8;
    const auto packed_descriptor =
        take(service.inspect_raster(input, forced_packed), "inspect forced packed JPEG raster");
    require(packed_descriptor.frames.front().layout.planes.size() == 1 &&
                packed_descriptor.frames.front().layout.planes.front().semantic ==
                    snow::image::PlaneSemantic::packed &&
                packed_descriptor.frames.front().layout.planes.front().format ==
                    snow::image::kRgba8,
            "an explicit JPEG output format keeps the packed compatibility path");

    snow::image::DecodeOptions scaled_options;
    scaled_options.maximum_extent = 3;
    scaled_options.output_format = snow::image::kRgba8;
    const auto scaled_descriptor =
        take(service.inspect_raster(input, scaled_options), "inspect scaled JPEG preview");
    Document scaled_preview =
        take(service.decode(input, scaled_options), "decode scaled JPEG preview");
    require(scaled_descriptor.canvas_width <= 3 && scaled_descriptor.canvas_height <= 3 &&
                scaled_preview.canvas_width == scaled_descriptor.canvas_width &&
                scaled_preview.canvas_height == scaled_descriptor.canvas_height &&
                scaled_preview.frames.front().image.width() == scaled_descriptor.canvas_width &&
                scaled_preview.frames.front().image.height() == scaled_descriptor.canvas_height,
            "JPEG maximum_extent uses native scaled IDCT dimensions consistently");

    snow::image::MutableImage gray_image =
        take(snow::image::MutableImage::allocate(5, 3, snow::image::kGray8),
             "allocate grayscale JPEG source");
    for (std::size_t index = 0; index < gray_image.pixels().size(); ++index)
        gray_image.pixels()[index] = static_cast<std::byte>((index * 19U) & 0xFFU);
    Document gray_document;
    gray_document.canvas_width = gray_image.width();
    gray_document.canvas_height = gray_image.height();
    Frame gray_frame;
    gray_frame.image = std::move(gray_image).freeze();
    gray_document.frames.push_back(std::move(gray_frame));
    auto gray_encoded = std::make_shared<std::vector<std::byte>>();
    auto gray_status = service.encode(
        gray_document, snow::image::memory_output(gray_encoded, "gray.jpg"), options);
    require(gray_status.has_value(), "grayscale JPEG encode succeeds");
    require(gray_status.value().receipt.jpeg_chroma_subsampling ==
                snow::image::ChromaSubsampling::none,
            "grayscale JPEG receipt resolves to grayscale sampling");
    const auto gray_descriptor =
        take(service.inspect_raster(snow::image::memory_input(gray_encoded), native_options),
             "inspect native grayscale JPEG");
    require(gray_descriptor.frames.front().layout.color_model == snow::image::ColorModel::gray &&
                gray_descriptor.frames.front().layout.planes.size() == 1 &&
                gray_descriptor.frames.front().layout.planes.front().semantic ==
                    snow::image::PlaneSemantic::gray &&
                gray_descriptor.frames.front().layout.planes.front().width == 5 &&
                gray_descriptor.frames.front().layout.planes.front().height == 3,
            "native grayscale JPEG uses one full-resolution plane");

    for (const auto& [quality, expected] :
         std::array<std::pair<int, snow::image::ChromaSubsampling>, 6>{
             std::pair{1, snow::image::ChromaSubsampling::yuv420},
             std::pair{79, snow::image::ChromaSubsampling::yuv420},
             std::pair{80, snow::image::ChromaSubsampling::yuv422},
             std::pair{89, snow::image::ChromaSubsampling::yuv422},
             std::pair{90, snow::image::ChromaSubsampling::yuv444},
             std::pair{100, snow::image::ChromaSubsampling::yuv444}}) {
        snow::image::EncodeOptions automatic = options;
        automatic.quality = quality;
        automatic.chroma_subsampling.reset();
        auto bytes = std::make_shared<std::vector<std::byte>>();
        auto automatic_status =
            service.encode(source, snow::image::memory_output(bytes, "auto.jpg"), automatic);
        require(automatic_status.has_value() &&
                    automatic_status.value().receipt.jpeg_chroma_subsampling == expected,
                "JPEG Auto sampling follows the quality thresholds");
        const auto automatic_descriptor =
            take(service.inspect_raster(snow::image::memory_input(bytes), native_options),
                 "inspect automatic JPEG sampling");
        require(automatic_descriptor.frames.front().layout.chroma_subsampling == expected,
                "JPEG descriptor agrees with Auto receipt sampling");
    }
    for (const auto manual :
         {snow::image::ChromaSubsampling::yuv444, snow::image::ChromaSubsampling::yuv422,
          snow::image::ChromaSubsampling::yuv420}) {
        snow::image::EncodeOptions selected = options;
        selected.quality = 75;
        selected.chroma_subsampling = manual;
        auto bytes = std::make_shared<std::vector<std::byte>>();
        auto selected_status =
            service.encode(source, snow::image::memory_output(bytes, "manual.jpg"), selected);
        require(selected_status.has_value() &&
                    selected_status.value().receipt.jpeg_chroma_subsampling == manual,
                "JPEG manual sampling is reported by the receipt");
    }

    snow::image::MutableImage noisy =
        take(snow::image::MutableImage::allocate(1024, 1024, snow::image::kRgb8),
             "allocate streaming JPEG fixture");
    std::uint32_t random = 0x12345678U;
    for (std::byte& value : noisy.pixels()) {
        random = random * 1664525U + 1013904223U;
        value = static_cast<std::byte>(random >> 24U);
    }
    Document noisy_document;
    noisy_document.canvas_width = noisy.width();
    noisy_document.canvas_height = noisy.height();
    Frame noisy_frame;
    noisy_frame.image = std::move(noisy).freeze();
    noisy_document.frames.push_back(std::move(noisy_frame));
    snow::image::EncodeOptions streaming_options;
    streaming_options.format = Format::jpeg;
    streaming_options.quality = 100;
    streaming_options.chroma_subsampling = snow::image::ChromaSubsampling::yuv444;
    auto bounded = std::make_shared<BoundedCollectingSink>();
    auto bounded_status = service.encode(
        noisy_document, snow::image::Output{bounded, "bounded.jpg"}, streaming_options);
    require(bounded_status.has_value() && bounded->writes > 1 &&
                bounded->maximum_write <= (std::size_t{256} << 10U),
            "streaming JPEG output never exceeds the 256 KiB destination bound");

    auto failing = std::make_shared<FailingByteSink>();
    auto sink_failure = service.encode(noisy_document, snow::image::Output{failing, "failed.jpg"},
                                       streaming_options);
    require(!sink_failure && sink_failure.error().code == ErrorCode::io_error &&
                failing->writes == 1,
            "JPEG destination manager propagates incremental sink failures");

    std::stop_source packed_stop;
    auto cancelling = std::make_shared<CancellingByteSink>(&packed_stop);
    auto cancelled =
        service.encode(noisy_document, snow::image::Output{cancelling, "cancelled.jpg"},
                       streaming_options, packed_stop.get_token());
    require(!cancelled && cancelled.error().code == ErrorCode::cancelled && cancelling->writes == 1,
            "packed JPEG scanline encoding cooperatively observes cancellation");

    const std::filesystem::path planar_cancel_path = store_path.string() + ".cancel-planar";
    std::filesystem::remove(planar_cancel_path, ignored);
    auto noisy_planar =
        take(service.decode_to_store(snow::image::memory_input(
                                         std::make_shared<std::vector<std::byte>>(bounded->bytes)),
                                     planar_cancel_path, native_options),
             "decode native JPEG cancellation fixture");
    std::stop_source planar_stop;
    auto planar_cancelling = std::make_shared<CancellingByteSink>(&planar_stop);
    auto planar_cancelled = service.encode(
        *noisy_planar, snow::image::Output{planar_cancelling, "cancelled-planar.jpg"},
        streaming_options, planar_stop.get_token());
    require(!planar_cancelled && planar_cancelled.error().code == ErrorCode::cancelled &&
                planar_cancelling->writes == 1,
            "native JPEG iMCU encoding cooperatively observes cancellation");
    noisy_planar.reset();
    std::filesystem::remove(planar_cancel_path, ignored);

    store.reset();
    packed_store.reset();
    std::filesystem::remove(store_path, ignored);
    std::filesystem::remove(packed_store_path, ignored);
}

void test_gif_animation(Service& service) {
    if (!supports(service, Format::gif, snow::image::CodecCapability::encode))
        return;
    Document document = sample_document();
    document.format = Format::gif;
    document.loop_count = 0;
    document.frames.front().duration = std::chrono::milliseconds(120);
    document.frames.front().disposal = snow::image::FrameDisposal::background;

    snow::image::MutableImage second =
        take(snow::image::MutableImage::allocate(1, 1, snow::image::kRgba8), "allocate GIF frame");
    second.pixels()[0] = std::byte{0};
    second.pixels()[1] = std::byte{0};
    second.pixels()[2] = std::byte{0xFF};
    second.pixels()[3] = std::byte{0xFF};
    Frame frame;
    frame.image = std::move(second).freeze();
    frame.x = 1;
    frame.y = 1;
    frame.duration = std::chrono::milliseconds(30);
    frame.blend = snow::image::FrameBlend::over;
    frame.disposal = snow::image::FrameDisposal::previous;
    document.frames.push_back(std::move(frame));

    auto encoded = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::gif;
    Result<snow::image::EncodeResult> status =
        service.encode(document, snow::image::memory_output(encoded), options);
    require(status.has_value(), "animated GIF encode succeeds");
    Document decoded =
        take(service.decode(snow::image::memory_input(encoded)), "decode animated GIF");
    require(decoded.frames.size() == 2, "GIF preserves frame count");
    require(decoded.loop_count == 0, "GIF preserves infinite loop count");
    require(decoded.frames[0].duration == std::chrono::milliseconds(120),
            "GIF preserves first frame duration");
    require(decoded.frames[1].x == 1 && decoded.frames[1].y == 1, "GIF preserves frame rectangle");
    require(decoded.frames[1].disposal == snow::image::FrameDisposal::previous,
            "GIF preserves disposal mode");
    AnimationStreamingSink sink;
    require(service.decode_to_sink(snow::image::memory_input(encoded), sink).has_value() &&
                sink.ended && sink.frames_started == 2 && sink.frames_ended == 2 &&
                sink.rows == 3 && sink.info.frames.size() == 2 && sink.info.loop_count == 0 &&
                sink.info.frames[0].duration == std::chrono::milliseconds(120) &&
                sink.info.frames[1].disposal == snow::image::FrameDisposal::previous,
            "GIF publishes rows from one bounded frame at a time");

    snow::image::DecodeOptions selected_options;
    selected_options.frame_index = 1;
    const snow::image::DocumentInfo selected_info =
        take(service.inspect(snow::image::memory_input(encoded), selected_options),
             "inspect selected GIF frame");
    Document selected = take(service.decode(snow::image::memory_input(encoded), selected_options),
                             "decode selected GIF frame");
    require(selected_info.frames.size() == 1 && selected.frames.size() == 1 &&
                selected.frames.front().x == 1 && selected.frames.front().image.width() == 1,
            "GIF frame selection agrees across inspection and owning decode");

    snow::image::DecodeOptions limited_options;
    limited_options.limits.maximum_working_bytes = 9;
    AnimationStreamingSink limited_sink;
    const Result<void> limited_status =
        service.decode_to_sink(snow::image::memory_input(encoded), limited_sink, limited_options);
    require(!limited_status && limited_status.error().code == ErrorCode::limit_exceeded &&
                !limited_sink.ended,
            "GIF streaming enforces its bounded scanline working set");

    Document interlaced_document;
    interlaced_document.format = Format::gif;
    interlaced_document.canvas_width = 3;
    interlaced_document.canvas_height = 7;
    snow::image::MutableImage interlaced_pixels =
        take(snow::image::MutableImage::allocate(3, 7, snow::image::kRgba8),
             "allocate interlaced GIF frame");
    for (std::uint32_t y = 0; y < 7; ++y) {
        for (std::uint32_t x = 0; x < 3; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * interlaced_pixels.row_stride() +
                static_cast<std::size_t>(x) * 4U;
            interlaced_pixels.pixels()[offset] =
                static_cast<std::byte>((y & 1U) != 0 ? 0xFF : 0x00);
            interlaced_pixels.pixels()[offset + 1U] =
                static_cast<std::byte>((y & 2U) != 0 ? 0xFF : 0x00);
            interlaced_pixels.pixels()[offset + 2U] =
                static_cast<std::byte>((y & 4U) != 0 ? 0xFF : 0x00);
            interlaced_pixels.pixels()[offset + 3U] = std::byte{0xFF};
        }
    }
    Frame interlaced_frame;
    interlaced_frame.image = std::move(interlaced_pixels).freeze();
    interlaced_document.frames.push_back(std::move(interlaced_frame));
    auto interlaced_bytes = std::make_shared<std::vector<std::byte>>();
    options.interlaced = true;
    require(
        service.encode(interlaced_document, snow::image::memory_output(interlaced_bytes), options)
            .has_value(),
        "interlaced GIF encode succeeds");
    Document interlaced_decoded =
        take(service.decode(snow::image::memory_input(interlaced_bytes)), "decode interlaced GIF");
    StorageSink interlaced_storage_sink;
    require(
        service.decode_to_sink(snow::image::memory_input(interlaced_bytes), interlaced_storage_sink)
                .has_value() &&
            interlaced_storage_sink.ended &&
            interlaced_storage_sink.pixels ==
                std::vector<std::byte>(interlaced_decoded.frames.front().image.pixels().begin(),
                                       interlaced_decoded.frames.front().image.pixels().end()),
        "interlaced GIF deinterlaces directly into sink-owned storage");
    AnimationStreamingSink interlaced_row_sink;
    require(service.decode_to_sink(snow::image::memory_input(interlaced_bytes), interlaced_row_sink)
                    .has_value() &&
                interlaced_row_sink.ended && interlaced_row_sink.rows == 7,
            "interlaced GIF publishes ordered rows with one bounded index frame");
}

void test_icon_containers(Service& service) {
    if (!supports(service, Format::ico, snow::image::CodecCapability::encode))
        return;
    Document icon = sample_document();
    icon.format = Format::ico;
    auto encoded_icon = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions options;
    options.format = Format::ico;
    Result<snow::image::EncodeResult> status =
        service.encode(icon, snow::image::memory_output(encoded_icon), options);
    require(status.has_value(), "ICO encode succeeds");
    Document decoded_icon =
        take(service.decode(snow::image::memory_input(encoded_icon)), "decode ICO");
    require(decoded_icon.frames.size() == 1 && decoded_icon.frames[0].image.width() == 2,
            "ICO preserves embedded image");
    require(decoded_icon.frames[0].image.pixels()[7] == std::byte{0x80},
            "ICO PNG payload preserves alpha");

    Document multi_icon = sample_document();
    multi_icon.format = Format::ico;
    snow::image::MutableImage small_icon =
        take(snow::image::MutableImage::allocate(1, 1, snow::image::kRgba8),
             "allocate second ICO image");
    std::fill(small_icon.pixels().begin(), small_icon.pixels().end(), std::byte{0x7F});
    Frame small_icon_frame;
    small_icon_frame.image = std::move(small_icon).freeze();
    multi_icon.frames.push_back(std::move(small_icon_frame));
    auto multi_icon_bytes = std::make_shared<std::vector<std::byte>>();
    require(service.encode(multi_icon, snow::image::memory_output(multi_icon_bytes), options)
                .has_value(),
            "multi-image ICO encode succeeds");
    AnimationStreamingSink icon_sink;
    require(service.decode_to_sink(snow::image::memory_input(multi_icon_bytes), icon_sink)
                    .has_value() &&
                icon_sink.ended && icon_sink.frames_started == 2 && icon_sink.frames_ended == 2 &&
                icon_sink.rows == 3 && icon_sink.info.frames.size() == 2,
            "ICO publishes one bounded embedded image at a time");
    snow::image::DecodeOptions selected_icon_options;
    selected_icon_options.frame_index = 1;
    const snow::image::DocumentInfo selected_icon_info =
        take(service.inspect(snow::image::memory_input(multi_icon_bytes), selected_icon_options),
             "inspect selected ICO image");
    require(selected_icon_info.frames.size() == 1 && selected_icon_info.frames.front().width == 1,
            "ICO frame selection is reflected during inspection");
    snow::image::DecodeOptions limited_icon_options;
    limited_icon_options.limits.maximum_working_bytes = 15;
    AnimationStreamingSink limited_icon_sink;
    const Result<void> limited_icon_status = service.decode_to_sink(
        snow::image::memory_input(multi_icon_bytes), limited_icon_sink, limited_icon_options);
    require(!limited_icon_status && limited_icon_status.error().code == ErrorCode::limit_exceeded &&
                !limited_icon_sink.ended,
            "ICO streaming enforces its one-image working-memory limit");

    Document cursor = sample_document();
    cursor.format = Format::cur;
    cursor.frames[0].cursor_hotspot = std::array<std::uint32_t, 2>{1, 1};
    auto encoded_cursor = std::make_shared<std::vector<std::byte>>();
    options.format = Format::cur;
    status = service.encode(cursor, snow::image::memory_output(encoded_cursor), options);
    require(status.has_value(), "CUR encode succeeds");
    Document decoded_cursor =
        take(service.decode(snow::image::memory_input(encoded_cursor)), "decode CUR");
    const auto& cursor_hotspot = decoded_cursor.frames[0].cursor_hotspot;
    require(cursor_hotspot.has_value() &&
                cursor_hotspot.value() == std::array<std::uint32_t, 2>{1, 1},
            "CUR preserves hotspot");
}

void test_xbitmap_formats(Service& service) {
    if (supports(service, Format::xbm, snow::image::CodecCapability::encode)) {
        auto encoded = std::make_shared<std::vector<std::byte>>();
        snow::image::EncodeOptions options;
        options.format = Format::xbm;
        Result<snow::image::EncodeResult> status =
            service.encode(sample_document(), snow::image::memory_output(encoded), options);
        require(status.has_value(), "XBM encode succeeds");
        Document decoded = take(service.decode(snow::image::memory_input(encoded)), "decode XBM");
        require(decoded.frames[0].image.format() == snow::image::kGray8, "XBM decodes to Gray8");
        require(decoded.frames[0].image.pixels()[0] == std::byte{0},
                "XBM dark pixel survives threshold encoding");
    }
    if (supports(service, Format::xpm, snow::image::CodecCapability::encode)) {
        auto encoded = std::make_shared<std::vector<std::byte>>();
        snow::image::EncodeOptions options;
        options.format = Format::xpm;
        Result<snow::image::EncodeResult> status =
            service.encode(sample_document(), snow::image::memory_output(encoded), options);
        require(status.has_value(), "XPM encode succeeds");
        Document decoded = take(service.decode(snow::image::memory_input(encoded)), "decode XPM");
        require(decoded.frames[0].image.pixels()[0] == std::byte{0xFF}, "XPM preserves opaque red");
        require(decoded.frames[0].image.pixels()[7] == std::byte{0xFF},
                "XPM alpha threshold keeps alpha 128 opaque");
        require(decoded.frames[0].image.pixels()[11] == std::byte{0},
                "XPM encodes low alpha as None");
    }
}

void test_processing() {
    snow::image::TransformOptions options;
    options.resize =
        snow::image::ResizeOptions{7, 5, snow::image::ResamplingMethod::lanczos3, true, true};
    options.palette = snow::image::PaletteOptions{2, 0.0F};
    Document transformed =
        take(snow::image::transform(sample_document(), options), "resize and reduce palette");
    require(transformed.canvas_width == 7 && transformed.canvas_height == 5,
            "processing updates canvas dimensions");
    require(transformed.frames.size() == 1 && transformed.frames[0].image.width() == 7 &&
                transformed.frames[0].image.height() == 5,
            "processing updates frame dimensions");
    std::vector<std::array<std::byte, 4>> colors;
    const Image& image = transformed.frames[0].image;
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        const std::byte* row =
            image.pixels().data() + static_cast<std::size_t>(y) * image.row_stride();
        for (std::uint32_t x = 0; x < image.width(); ++x) {
            std::array<std::byte, 4> color{};
            std::memcpy(color.data(), row + static_cast<std::size_t>(x) * 4U, 4U);
            if (std::find(colors.begin(), colors.end(), color) == colors.end())
                colors.push_back(color);
        }
    }
    require(colors.size() <= 2, "palette reduction respects the requested color count");

    snow::image::MutableImage skewed_pixels =
        take(snow::image::MutableImage::allocate(3, 1, snow::image::kRgba8),
             "allocate skewed palette fixture");
    const std::array<std::byte, 12> skewed_values{
        std::byte{0x00}, std::byte{0x00}, std::byte{0xFF}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF},
        std::byte{0xFF}, std::byte{0x00}, std::byte{0x00}, std::byte{0xFF}};
    std::copy(skewed_values.begin(), skewed_values.end(), skewed_pixels.pixels().begin());
    Document skewed = sample_document();
    skewed.canvas_width = 3;
    skewed.canvas_height = 1;
    skewed.frames.resize(1);
    skewed.frames.front().image = std::move(skewed_pixels).freeze();
    skewed.frames.front().x = 0;
    skewed.frames.front().y = 0;
    snow::image::TransformOptions palette_only;
    palette_only.palette = snow::image::PaletteOptions{2, 0.0F};
    const Document skewed_transformed =
        take(snow::image::transform(skewed, palette_only),
             "reduce a palette with a dominant final histogram bin");
    require(skewed_transformed.frames.front().image.width() == 3 &&
                skewed_transformed.frames.front().image.height() == 1 &&
                !std::equal(skewed_transformed.frames.front().image.pixels().begin(),
                            skewed_transformed.frames.front().image.pixels().begin() + 4,
                            skewed_transformed.frames.front().image.pixels().begin() + 4),
            "palette reduction keeps both weighted split partitions non-empty");
    StorageSink sink;
    require(snow::image::transform_to_sink(sample_document(), options, sink).has_value() &&
                sink.ended && sink.storage_requests == 1 && sink.callback_rows == 0 &&
                sink.info.canvas_width == transformed.canvas_width &&
                sink.info.canvas_height == transformed.canvas_height &&
                sink.pixels.size() == transformed.frames.front().image.pixels().size() &&
                std::equal(sink.pixels.begin(), sink.pixels.end(),
                           transformed.frames.front().image.pixels().begin()),
            "sink transform is pixel-identical to the owning transform");

    snow::image::TransformOptions direct_options = options;
    direct_options.palette.reset();
    const Document direct_owned = take(snow::image::transform(sample_document(), direct_options),
                                       "owning direct-storage resize reference");
    StorageSink direct_sink;
    require(snow::image::transform_to_sink(sample_document(), direct_options, direct_sink)
                    .has_value() &&
                direct_sink.storage_requests == 1 && direct_sink.callback_rows == 0 &&
                std::equal(direct_sink.pixels.begin(), direct_sink.pixels.end(),
                           direct_owned.frames.front().image.pixels().begin()),
            "direct-storage resize is pixel-identical to the owning transform");

    Document animation = sample_document();
    animation.loop_count = 7;
    animation.frames.front().duration = std::chrono::milliseconds(40);
    Frame second = animation.frames.front();
    second.duration = std::chrono::milliseconds(70);
    animation.frames.push_back(std::move(second));
    snow::image::TransformOptions first_frame;
    first_frame.animation_policy = snow::image::AnimationPolicy::first_frame;
    StorageSink first_sink;
    require(snow::image::transform_to_sink(animation, first_frame, first_sink).has_value() &&
                first_sink.info.frames.size() == 1 && first_sink.info.loop_count == 1 &&
                first_sink.info.frames.front().duration.count() == 0,
            "first-frame sink transforms emit one static composed frame");
    Service service;
    if (service.encoder_info(Format::png)) {
        auto encoded = std::make_shared<std::vector<std::byte>>();
        snow::image::EncodeOptions png_options;
        png_options.format = Format::png;
        require(service.encode(transformed, snow::image::memory_output(encoded), png_options)
                    .has_value(),
                "palette-reduced PNG encode succeeds");
        require(encoded->size() > 26U && (*encoded)[25] == std::byte{3},
                "palette-reduced PNG uses indexed color encoding");
    }

    snow::image::TransformOptions invalid;
    invalid.resize = snow::image::ResizeOptions{};
    Result<Document> failed = snow::image::transform(sample_document(), invalid);
    require(!failed && failed.error().code == ErrorCode::invalid_argument,
            "zero resize dimensions are rejected");
}

void test_webp(Service& service) {
    const snow::image::EncoderInfo* info = service.encoder_info(Format::webp);
    if (!info)
        return;
    require(snow::image::has_feature(info->features, snow::image::EncoderFeature::animation) &&
                snow::image::has_feature(info->features, snow::image::EncoderFeature::alpha) &&
                snow::image::has_feature(info->features, snow::image::EncoderFeature::metadata) &&
                info->limits.maximum_width == 16'383 && info->limits.maximum_height == 16'383 &&
                info->lossless_effort.minimum == 0 && info->lossless_effort.maximum == 9 &&
                info->lossless_effort.default_value == 6,
            "WebP encoder advertises metadata, limits, and lossless effort");
    snow::image::EncodeOptions normalization;
    normalization.format = Format::webp;
    normalization.lossless = true;
    normalization.quality = 91;
    normalization.effort = 5;
    normalization.lossless_effort = 12;
    auto normalized = take(snow::image::normalize_encode_options(*info, normalization),
                           "normalize lossless WebP options");
    require(normalized.quality == 0 && normalized.effort == 0 && normalized.lossless_effort == 9,
            "lossless WebP normalization removes lossy options and clamps compression");
    normalization.lossless = false;
    normalized = take(snow::image::normalize_encode_options(*info, normalization),
                      "normalize lossy WebP options");
    require(normalized.quality == 91 && normalized.effort == 5 && normalized.lossless_effort == 0,
            "lossy WebP normalization removes lossless compression effort");
    Document animation = sample_document();
    animation.format = Format::webp;
    animation.loop_count = 4;
    animation.frames[0].duration = std::chrono::milliseconds(40);
    Frame second;
    snow::image::MutableImage second_pixels =
        take(snow::image::MutableImage::allocate(2, 2, snow::image::kRgba8),
             "allocate WebP second frame");
    std::fill(second_pixels.pixels().begin(), second_pixels.pixels().end(), std::byte{0x60});
    second.image = std::move(second_pixels).freeze();
    second.duration = std::chrono::milliseconds(70);
    animation.frames.push_back(std::move(second));
    auto encoded = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions encode;
    encode.format = Format::webp;
    encode.lossless = true;
    encode.quality = 100;
    encode.effort = 4;
    auto still_bytes = std::make_shared<std::vector<std::byte>>();
    require(service
                .encode(sample_document(), snow::image::memory_output(still_bytes, "still.webp"),
                        encode)
                .has_value(),
            "static WebP encode succeeds");
    snow::image::DecodeOptions native_options;
    native_options.raster_layout = snow::image::RasterLayoutPolicy::native;
    const auto lossless_descriptor =
        take(service.inspect_raster(snow::image::memory_input(still_bytes), native_options),
             "inspect lossless WebP raster");
    require(lossless_descriptor.frames.front().layout.planes.size() == 1 &&
                lossless_descriptor.frames.front().layout.planes.front().semantic ==
                    snow::image::PlaneSemantic::packed,
            "lossless WebP remains packed under the native raster policy");
    StorageSink still_sink;
    require(
        service.decode_to_sink(snow::image::memory_input(still_bytes), still_sink).has_value() &&
            still_sink.ended && still_sink.storage_requests == 1 && still_sink.callback_rows == 0 &&
            still_sink.pixels.size() == 16,
        "static WebP decodes directly into storage-backed sinks");

    snow::image::MutableImage exact_pixels =
        take(snow::image::MutableImage::allocate(3, 1, snow::image::kRgba8),
             "allocate exact transparent WebP fixture");
    const std::array<std::byte, 12> exact_values{std::byte{17},  std::byte{29},  std::byte{43},
                                                 std::byte{0},   std::byte{71},  std::byte{83},
                                                 std::byte{97},  std::byte{1},   std::byte{101},
                                                 std::byte{113}, std::byte{127}, std::byte{255}};
    std::copy(exact_values.begin(), exact_values.end(), exact_pixels.pixels().begin());
    Document exact_document;
    exact_document.format = Format::webp;
    exact_document.canvas_width = 3;
    exact_document.canvas_height = 1;
    Frame exact_frame;
    exact_frame.image = std::move(exact_pixels).freeze();
    exact_document.frames.push_back(std::move(exact_frame));
    for (const int preset : {0, 6, 9}) {
        auto preset_bytes = std::make_shared<std::vector<std::byte>>();
        snow::image::EncodeOptions preset_options;
        preset_options.format = Format::webp;
        preset_options.lossless = true;
        preset_options.lossless_effort = preset;
        const auto preset_encoded = service.encode(
            exact_document, snow::image::memory_output(preset_bytes), preset_options);
        require(preset_encoded &&
                    preset_encoded.value().round_trip == snow::image::PixelRoundTrip::exact,
                "every tested WebP lossless preset reports an exact round trip");
        const Document preset_decoded =
            take(service.decode(snow::image::memory_input(preset_bytes)),
                 "decode exact transparent WebP fixture");
        require(std::equal(exact_values.begin(), exact_values.end(),
                           preset_decoded.frames.front().image.pixels().begin()),
                "lossless WebP preserves RGB values beneath transparent alpha");
    }

    snow::image::MutableImage four_k_pixels =
        take(snow::image::MutableImage::allocate(3840, 2160, snow::image::kRgba8),
             "allocate 4K WebP fixture");
    Document four_k;
    four_k.format = Format::webp;
    four_k.canvas_width = four_k_pixels.width();
    four_k.canvas_height = four_k_pixels.height();
    Frame four_k_frame;
    four_k_frame.image = std::move(four_k_pixels).freeze();
    four_k.frames.push_back(std::move(four_k_frame));
    auto four_k_lossless = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions four_k_lossless_options;
    four_k_lossless_options.format = Format::webp;
    four_k_lossless_options.lossless = true;
    four_k_lossless_options.lossless_effort = 6;
    require(
        service.encode(four_k, snow::image::memory_output(four_k_lossless), four_k_lossless_options)
                .has_value() &&
            !four_k_lossless->empty(),
        "4K packed static lossless WebP encode succeeds");
    const auto four_k_descriptor =
        take(snow::image::describe_document(four_k), "describe 4K WebP fixture");
    const auto four_k_store_bytes =
        take(snow::image::RasterBufferStore::required_bytes(four_k_descriptor),
             "size mapped 4K WebP fixture");
    std::vector<std::byte> four_k_storage(static_cast<std::size_t>(four_k_store_bytes));
    auto four_k_store =
        take(snow::image::RasterBufferStore::create(four_k_storage, four_k_descriptor),
             "create mapped 4K WebP fixture");
    require(four_k_store
                    ->write_rows(0, 0, 0, four_k.canvas_height,
                                 four_k.frames.front().image.row_stride(),
                                 four_k.frames.front().image.pixels())
                    .has_value() &&
                four_k_store->commit().has_value(),
            "populate mapped 4K WebP fixture");
    auto four_k_lossy = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions four_k_lossy_options;
    four_k_lossy_options.format = Format::webp;
    four_k_lossy_options.quality = 75;
    four_k_lossy_options.effort = 4;
    require(service.encode(*four_k_store, snow::image::memory_output(four_k_lossy),
                           four_k_lossy_options)
                    .has_value() &&
                !four_k_lossy->empty(),
            "4K mapped static lossy WebP encode succeeds without animation state");

    const auto boundary_document = [](std::uint32_t width, std::uint32_t height) {
        snow::image::MutableImage pixels =
            take(snow::image::MutableImage::allocate(width, height, snow::image::kRgba8),
                 "allocate WebP dimension boundary fixture");
        Document document;
        document.format = Format::webp;
        document.canvas_width = width;
        document.canvas_height = height;
        Frame frame;
        frame.image = std::move(pixels).freeze();
        document.frames.push_back(std::move(frame));
        return document;
    };
    for (const auto& [width, height] : {std::pair{16'383U, 1U}, std::pair{1U, 16'383U}}) {
        auto boundary_bytes = std::make_shared<std::vector<std::byte>>();
        require(service
                    .encode(boundary_document(width, height),
                            snow::image::memory_output(boundary_bytes), four_k_lossless_options)
                    .has_value(),
                "WebP accepts dimensions at the 16,383 boundary");
    }
    auto invalid_bytes = std::make_shared<std::vector<std::byte>>();
    const Document invalid_document = boundary_document(16'384, 1);
    auto invalid_status = service.encode(
        invalid_document, snow::image::memory_output(invalid_bytes), four_k_lossless_options);
    require(!invalid_status && invalid_status.error().code == ErrorCode::limit_exceeded &&
                invalid_bytes->empty(),
            "document WebP encode rejects width 16,384 before writing");
    const Document invalid_raster_document = boundary_document(1, 16'384);
    const auto invalid_descriptor = take(snow::image::describe_document(invalid_raster_document),
                                         "describe invalid WebP raster boundary");
    const auto invalid_store_bytes =
        take(snow::image::RasterBufferStore::required_bytes(invalid_descriptor),
             "size invalid WebP raster boundary");
    std::vector<std::byte> invalid_storage(static_cast<std::size_t>(invalid_store_bytes));
    auto invalid_store =
        take(snow::image::RasterBufferStore::create(invalid_storage, invalid_descriptor),
             "create invalid WebP raster boundary");
    invalid_status = service.encode(*invalid_store, snow::image::memory_output(invalid_bytes),
                                    four_k_lossless_options);
    require(!invalid_status && invalid_status.error().code == ErrorCode::limit_exceeded,
            "raster WebP encode rejects height 16,384 before reading pixels");

    snow::image::MutableImage odd_pixels =
        take(snow::image::MutableImage::allocate(7, 5, snow::image::kRgba8),
             "allocate odd alpha WebP source");
    for (std::uint32_t y = 0; y < odd_pixels.height(); ++y) {
        std::byte* row =
            odd_pixels.pixels().data() + static_cast<std::size_t>(y) * odd_pixels.row_stride();
        for (std::uint32_t x = 0; x < odd_pixels.width(); ++x) {
            const std::size_t offset = static_cast<std::size_t>(x) * 4U;
            row[offset] = static_cast<std::byte>((x * 31U + y * 7U) & 0xFFU);
            row[offset + 1U] = static_cast<std::byte>((x * 11U + y * 43U) & 0xFFU);
            row[offset + 2U] = static_cast<std::byte>((x * 53U + y * 17U) & 0xFFU);
            row[offset + 3U] = static_cast<std::byte>((x * 37U + y * 29U + 13U) & 0xFFU);
        }
    }
    Document odd_document;
    odd_document.format = Format::webp;
    odd_document.canvas_width = odd_pixels.width();
    odd_document.canvas_height = odd_pixels.height();
    Frame odd_frame;
    odd_frame.image = std::move(odd_pixels).freeze();
    odd_document.frames.push_back(std::move(odd_frame));
    snow::image::EncodeOptions lossy;
    lossy.format = Format::webp;
    lossy.lossless = false;
    lossy.quality = 92;
    lossy.effort = 4;
    auto lossy_bytes = std::make_shared<std::vector<std::byte>>();
    require(
        service
            .encode(odd_document, snow::image::memory_output(lossy_bytes, "odd-alpha.webp"), lossy)
            .has_value(),
        "static lossy alpha WebP encode succeeds");
    const auto native_descriptor =
        take(service.inspect_raster(snow::image::memory_input(lossy_bytes), native_options),
             "inspect native lossy WebP raster");
    const auto& native_frame = native_descriptor.frames.front();
    require(
        native_descriptor.canvas_width == 7 && native_descriptor.canvas_height == 5 &&
            native_frame.layout.color_model == snow::image::ColorModel::ycbcr &&
            native_frame.layout.chroma_subsampling == snow::image::ChromaSubsampling::yuv420 &&
            native_frame.layout.color_range == snow::image::ColorRange::limited &&
            native_frame.layout.alpha == snow::image::AlphaMode::straight &&
            native_frame.layout.planes.size() == 4 && native_frame.layout.planes[0].width == 7 &&
            native_frame.layout.planes[0].height == 5 && native_frame.layout.planes[1].width == 4 &&
            native_frame.layout.planes[1].height == 3 && native_frame.layout.planes[2].width == 4 &&
            native_frame.layout.planes[2].height == 3 &&
            native_frame.layout.planes[3].semantic == snow::image::PlaneSemantic::alpha &&
            native_frame.layout.planes[3].width == 7 && native_frame.layout.planes[3].height == 5,
        "native lossy WebP exposes tight odd-sized YUVA420 planes");

    const std::filesystem::path store_path =
        std::filesystem::temp_directory_path() /
        ("snow-image-native-webp-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".srs");
    const std::filesystem::path packed_path = store_path.string() + ".packed";
    std::error_code ignored;
    std::filesystem::remove(store_path, ignored);
    std::filesystem::remove(packed_path, ignored);
    auto native_store = take(
        service.decode_to_store(snow::image::memory_input(lossy_bytes), store_path, native_options),
        "decode native WebP to raster store");
    require(native_store->complete() &&
                native_store->descriptor().frames.front().layout == native_frame.layout,
            "native WebP decode writes directly to mapped planar storage");
    require(native_store
                ->set_analysis(snow::image::RasterAnalysis{snow::image::AlphaContent::non_opaque})
                .has_value(),
            "native YUVA raster stores accept verified separate-plane alpha analysis");
    auto packed_store =
        take(service.decode_to_store(snow::image::memory_input(lossy_bytes), packed_path),
             "decode packed WebP to raster store");
    require(packed_store->complete() &&
                packed_store->descriptor().frames.front().layout.planes.size() == 1 &&
                packed_store->descriptor().frames.front().layout.planes.front().format ==
                    snow::image::kRgba8,
            "packed WebP inspection agrees with direct external-buffer decode");
    const auto native_route = take(service.raster_encode_route(native_store->descriptor(), lossy),
                                   "query native WebP raster route");
    snow::image::EncodeOptions route_lossless = lossy;
    route_lossless.lossless = true;
    const auto lossless_route =
        take(service.raster_encode_route(native_store->descriptor(), route_lossless),
             "query lossless WebP raster route");
    const auto packed_route = take(service.raster_encode_route(packed_store->descriptor(), lossy),
                                   "query packed WebP raster route");
    require(native_route == snow::image::RasterEncodeRoute::native &&
                lossless_route == snow::image::RasterEncodeRoute::materialized &&
                packed_route == snow::image::RasterEncodeRoute::materialized,
            "WebP route query selects native YUVA only for compatible lossy input");

    RowRasterWriter row_writer(native_descriptor);
    require(service.decode_into(snow::image::memory_input(lossy_bytes), row_writer, native_options)
                    .has_value() &&
                row_writer.committed && !row_writer.aborted && row_writer.rows_written == 16,
            "native WebP decode supports bounded row-only raster writers");
    snow::image::DecodeOptions limited = native_options;
    limited.limits.maximum_working_bytes = 93;
    RowRasterWriter limited_writer(native_descriptor);
    const Result<void> limited_status =
        service.decode_into(snow::image::memory_input(lossy_bytes), limited_writer, limited);
    require(!limited_status && limited_status.error().code == ErrorCode::limit_exceeded &&
                limited_writer.aborted && !limited_writer.committed,
            "native WebP row fallback enforces aggregate working memory");

    std::vector<std::byte> full_rgba(7U * 5U * 4U);
    snow::image::MutablePlaneView full_view{7, 5, snow::image::kRgba8, 7U * 4U, full_rgba};
    require(snow::image::read_rgba8_region(*native_store, 0, {0, 0, 7, 5}, full_view).has_value(),
            "tight native WebP planes convert to straight RGBA");
    std::vector<std::byte> odd_rgba(5U * 3U * 4U);
    snow::image::MutablePlaneView odd_view{5, 3, snow::image::kRgba8, 5U * 4U, odd_rgba};
    require(snow::image::read_rgba8_region(*native_store, 0, {1, 1, 5, 3}, odd_view).has_value(),
            "odd WebP regions convert with a bounded chroma halo");
    for (std::uint32_t row = 0; row < 3; ++row) {
        const auto row_offset = static_cast<std::ptrdiff_t>(row) * 20;
        const auto next_row_offset = (static_cast<std::ptrdiff_t>(row) + 1) * 20;
        const auto full_offset = ((static_cast<std::ptrdiff_t>(row) + 1) * 7 + 1) * 4;
        require(std::equal(odd_rgba.begin() + row_offset, odd_rgba.begin() + next_row_offset,
                           full_rgba.begin() + full_offset),
                "odd WebP region pixels match the full conversion crop");
    }
    std::vector<std::byte> premultiplied(odd_rgba.size());
    snow::image::MutablePlaneView premultiplied_view{5, 3, snow::image::kRgba8, 5U * 4U,
                                                     premultiplied};
    snow::image::RasterConversionOptions conversion;
    conversion.output_alpha = snow::image::AlphaMode::premultiplied;
    require(snow::image::read_rgba8_region(*native_store, 0, {1, 1, 5, 3}, premultiplied_view,
                                           conversion)
                .has_value(),
            "native WebP alpha converts explicitly to premultiplied RGBA");
    for (std::size_t pixel = 0; pixel < odd_rgba.size(); pixel += 4U) {
        const std::uint32_t alpha = std::to_integer<std::uint8_t>(odd_rgba[pixel + 3U]);
        require(premultiplied[pixel + 3U] == odd_rgba[pixel + 3U],
                "WebP premultiplication preserves alpha");
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const std::uint32_t straight = std::to_integer<std::uint8_t>(odd_rgba[pixel + channel]);
            require(std::to_integer<std::uint8_t>(premultiplied[pixel + channel]) ==
                        (straight * alpha + 127U) / 255U,
                    "WebP premultiplication scales each color channel exactly");
        }
    }

    snow::image::DecodeOptions scaled = native_options;
    scaled.maximum_extent = 3;
    const auto scaled_descriptor =
        take(service.inspect_raster(snow::image::memory_input(lossy_bytes), scaled),
             "inspect scaled native WebP preview");
    Document scaled_document = take(service.decode(snow::image::memory_input(lossy_bytes), scaled),
                                    "decode scaled packed WebP preview");
    require(scaled_descriptor.canvas_width == 3 && scaled_descriptor.canvas_height == 2 &&
                scaled_descriptor.frames.front().layout.planes[1].width == 2 &&
                scaled_descriptor.frames.front().layout.planes[1].height == 1 &&
                scaled_document.canvas_width == scaled_descriptor.canvas_width &&
                scaled_document.canvas_height == scaled_descriptor.canvas_height,
            "WebP scaled inspection and codec decode dimensions agree");

    MappedOnlySource mapped_source(native_store);
    auto planar_encoded = std::make_shared<std::vector<std::byte>>();
    require(service.encode(mapped_source,
                           snow::image::memory_output(planar_encoded, "native-planar.webp"), lossy)
                    .has_value() &&
                !planar_encoded->empty(),
            "WebP encoder consumes mapped native YUVA planes directly");
    const auto planar_descriptor =
        take(service.inspect_raster(snow::image::memory_input(planar_encoded), native_options),
             "inspect planar re-encoded WebP");
    require(planar_descriptor.frames.front().layout == native_frame.layout,
            "native WebP re-encode preserves tight YUVA420 geometry");

    const auto exif_orientation = [](std::uint16_t orientation) {
        return std::vector<std::byte>{std::byte{'I'},
                                      std::byte{'I'},
                                      std::byte{42},
                                      std::byte{0},
                                      std::byte{8},
                                      std::byte{0},
                                      std::byte{0},
                                      std::byte{0},
                                      std::byte{1},
                                      std::byte{0},
                                      std::byte{0x12},
                                      std::byte{0x01},
                                      std::byte{3},
                                      std::byte{0},
                                      std::byte{1},
                                      std::byte{0},
                                      std::byte{0},
                                      std::byte{0},
                                      static_cast<std::byte>(orientation & 0xffU),
                                      static_cast<std::byte>(orientation >> 8U),
                                      std::byte{0},
                                      std::byte{0},
                                      std::byte{0},
                                      std::byte{0},
                                      std::byte{0},
                                      std::byte{0}};
    };
    exact_document.color.icc_profile = {std::byte{'i'}, std::byte{'c'}, std::byte{'c'}};
    exact_document.metadata.exif = exif_orientation(6);
    exact_document.metadata.xmp = {std::byte{'<'}, std::byte{'x'}, std::byte{'/'}, std::byte{'>'}};
    auto metadata_bytes = std::make_shared<std::vector<std::byte>>();
    snow::image::EncodeOptions metadata_options;
    metadata_options.format = Format::webp;
    metadata_options.lossless = true;
    metadata_options.preserve_metadata = true;
    require(
        service.encode(exact_document, snow::image::memory_output(metadata_bytes), metadata_options)
            .has_value(),
        "static WebP with ICC, EXIF, and XMP metadata encodes");
    Document metadata_preserved = take(service.decode(snow::image::memory_input(metadata_bytes)),
                                       "decode preserved WebP metadata");
    require(metadata_preserved.color.icc_profile == exact_document.color.icc_profile &&
                metadata_preserved.metadata.exif == exact_document.metadata.exif &&
                metadata_preserved.metadata.xmp == exact_document.metadata.xmp &&
                metadata_preserved.metadata.orientation == snow::image::Orientation::rotate_90,
            "static WebP round-trips ICC, EXIF, XMP, and orientation");
    snow::image::DecodeOptions apply_orientation_options;
    apply_orientation_options.orientation = snow::image::OrientationPolicy::apply;
    Document oriented =
        take(service.decode(snow::image::memory_input(metadata_bytes), apply_orientation_options),
             "apply WebP EXIF orientation");
    require(oriented.canvas_width == 1 && oriented.canvas_height == 3 &&
                oriented.metadata.orientation == snow::image::Orientation::identity &&
                oriented.metadata.exif != exact_document.metadata.exif,
            "WebP orientation application swaps dimensions and normalizes retained EXIF");
    auto stripped_bytes = std::make_shared<std::vector<std::byte>>();
    metadata_options.preserve_metadata = false;
    require(
        service.encode(exact_document, snow::image::memory_output(stripped_bytes), metadata_options)
            .has_value(),
        "static WebP metadata stripping encode succeeds");
    const Document stripped = take(service.decode(snow::image::memory_input(stripped_bytes)),
                                   "decode metadata-stripped WebP");
    require(stripped.color.icc_profile.empty() && stripped.metadata.exif.empty() &&
                stripped.metadata.xmp.empty(),
            "WebP metadata stripping removes ICC, EXIF, and XMP completely");

    animation.color.icc_profile = exact_document.color.icc_profile;
    animation.metadata.exif = exif_orientation(1);
    animation.metadata.xmp = exact_document.metadata.xmp;

    require(service.encode(animation, snow::image::memory_output(encoded, "animation.webp"), encode)
                .has_value(),
            "animated WebP encode succeeds");
    AnimationStreamingSink animation_sink;
    require(
        service.decode_to_sink(snow::image::memory_input(encoded), animation_sink).has_value() &&
            animation_sink.ended && animation_sink.frames_started == 2 &&
            animation_sink.frames_ended == 2 && animation_sink.rows == 4 &&
            animation_sink.info.frames.size() == 2 &&
            animation_sink.info.frames[0].duration == std::chrono::milliseconds(40) &&
            animation_sink.info.frames[1].duration == std::chrono::milliseconds(70),
        "animated WebP publishes one composited frame at a time with durations");
    snow::image::DecodeOptions animation_limited;
    animation_limited.limits.maximum_working_bytes = 15;
    AnimationStreamingSink limited_animation_sink;
    const Result<void> animation_limited_status = service.decode_to_sink(
        snow::image::memory_input(encoded), limited_animation_sink, animation_limited);
    require(!animation_limited_status &&
                animation_limited_status.error().code == ErrorCode::limit_exceeded &&
                !limited_animation_sink.ended,
            "animated WebP enforces its single-canvas working-memory limit");
    const auto animation_descriptor =
        take(service.inspect_raster(snow::image::memory_input(encoded), native_options),
             "inspect animated WebP raster");
    require(animation_descriptor.frames.size() == 2 &&
                animation_descriptor.frames.front().layout.planes.size() == 1 &&
                animation_descriptor.frames.front().layout.planes.front().semantic ==
                    snow::image::PlaneSemantic::packed,
            "animated WebP remains packed under the native raster policy");
    require(take(service.detect(snow::image::memory_input(encoded)), "detect WebP") == Format::webp,
            "WebP content detection succeeds");
    Document decoded =
        take(service.decode(snow::image::memory_input(encoded)), "decode animated WebP");
    require(decoded.frames.size() == 2 && decoded.loop_count == 4,
            "WebP animation frame count and loop survive round trip");
    require(decoded.color.icc_profile == animation.color.icc_profile &&
                decoded.metadata.exif == animation.metadata.exif &&
                decoded.metadata.xmp == animation.metadata.xmp,
            "animated WebP round-trips ICC, EXIF, and XMP metadata");
    require(
        std::chrono::duration_cast<std::chrono::milliseconds>(decoded.frames[0].duration).count() ==
                40 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(decoded.frames[1].duration)
                    .count() == 70,
        "WebP frame durations survive round trip");
    Document overflowing_animation = animation;
    overflowing_animation.frames[0].duration =
        std::chrono::milliseconds(std::numeric_limits<int>::max());
    overflowing_animation.frames[1].duration = std::chrono::milliseconds(1);
    auto overflow_bytes = std::make_shared<std::vector<std::byte>>();
    const auto overflow_status =
        service.encode(overflowing_animation, snow::image::memory_output(overflow_bytes), encode);
    require(!overflow_status && overflow_status.error().code == ErrorCode::limit_exceeded &&
                overflow_bytes->empty(),
            "WebP rejects cumulative animation timestamp overflow before allocation");
    native_store.reset();
    packed_store.reset();
    std::filesystem::remove(store_path, ignored);
    std::filesystem::remove(packed_path, ignored);
}

Document opaque_document(std::uint32_t width, std::uint32_t height);

void test_resource_estimates() {
    snow::image::DocumentInfo info;
    info.canvas_width = 3840;
    info.canvas_height = 2160;
    info.frames.push_back({3840,
                           2160,
                           0,
                           0,
                           {},
                           snow::image::kRgba8,
                           true,
                           {},
                           {},
                           {},
                           snow::image::FrameBlend::source,
                           snow::image::FrameDisposal::keep});
    snow::image::TransformOptions transform;
    transform.resize = snow::image::ResizeOptions{1920, 1080};
    const auto transformed = snow::image::estimate_transform_resources(info, transform);
    require(transformed && transformed.value().mapped_bytes == 1920ULL * 1080ULL * 4ULL &&
                transformed.value().private_memory_bytes > 0,
            "transform estimates separate mapped output and private row memory");
    const auto preview = snow::image::estimate_preview_decode_resources(info);
    require(preview && preview.value().mapped_bytes == 3840ULL * 2160ULL * 4ULL &&
                preview.value().total_bytes().has_value(),
            "preview decode estimates include mapped raster storage");

    snow::image::ResourceEstimate overflowing;
    overflowing.private_memory_bytes = std::numeric_limits<std::uint64_t>::max();
    overflowing.mapped_bytes = 1;
    require(!overflowing.total_bytes(), "resource estimate totals reject overflow");
    snow::image::DocumentInfo impossible;
    impossible.canvas_width = std::numeric_limits<std::uint32_t>::max();
    impossible.canvas_height = std::numeric_limits<std::uint32_t>::max();
    impossible.frames.push_back({impossible.canvas_width,
                                 impossible.canvas_height,
                                 0,
                                 0,
                                 {},
                                 snow::image::kRgba32Float,
                                 true,
                                 {},
                                 {},
                                 {},
                                 snow::image::FrameBlend::source,
                                 snow::image::FrameDisposal::keep});
    require(!snow::image::estimate_preview_decode_resources(impossible),
            "preview estimates reject frame-size overflow");

    Document jxl = sample_document();
    snow::image::EncodeOptions jxl_options;
    jxl_options.format = Format::jxl;
    const auto jxl_estimate = take(snow::image::estimate_encode_resources(jxl, jxl_options),
                                   "estimate JPEG XL encoding resources");
    const std::uint64_t jxl_raw_bytes = jxl.frames.front().image.pixels().size();
    require(jxl_estimate.private_memory_bytes >= jxl_raw_bytes * 16U + (std::uint64_t{64} << 20U) &&
                jxl_estimate.private_memory_bytes >= (std::uint64_t{640} << 20U),
            "JPEG XL estimate covers its fixed and pixel-scaled encoder working set");
}

void test_phase_resource_plan() {
    const Document source_document = opaque_document(3840, 2160);
    snow::image::DocumentDescriptor source =
        take(snow::image::describe_document(source_document), "describe resource-plan source");
    snow::image::DocumentDescriptor output = source;
    output.canvas_width = 1920;
    output.canvas_height = 1080;
    output.frames.front().width = 1920;
    output.frames.front().height = 1080;
    output.frames.front().layout.planes.front().width = 1920;
    output.frames.front().layout.planes.front().height = 1080;
    snow::image::ResourcePlanRequest request;
    request.source = source;
    request.output = output;
    request.expected_artifact_bytes = 4U * 1024U * 1024U;
    auto plan = take(snow::image::plan_resources(request), "plan phase-aware edit resources");
    std::uint64_t summed_private = 0;
    for (const snow::image::PhaseResourcePlan& phase : plan.phases)
        summed_private += phase.footprint.private_memory_bytes;
    require(plan.tile_width >= 256 && plan.staging_depth == 3 && plan.cpu_threads >= 1 &&
                plan.peak.private_memory_bytes < summed_private &&
                plan.peak.temporary_disk_bytes >= request.expected_artifact_bytes,
            "resource plan uses phase peaks instead of summing non-overlapping allocations");

    request.output.format = Format::jxl;
    request.output.frames.front().layout.planes.front().format = snow::image::kRgba16Float;
    request.output.frames.front().layout.planes.front().significant_bits = 16;
    request.budgets.private_memory_bytes = std::uint64_t{2} << 30U;
    plan = take(snow::image::plan_resources(request), "plan JPEG XL edit resources");
    const auto jxl_encode = std::find_if(
        plan.phases.begin(), plan.phases.end(), [](const snow::image::PhaseResourcePlan& phase) {
            return phase.phase == snow::image::ResourcePhase::encode;
        });
    const std::uint64_t jxl_raster_bytes = 1920ULL * 1080ULL * 8ULL;
    require(jxl_encode != plan.phases.end() &&
                jxl_encode->footprint.private_memory_bytes >=
                    jxl_raster_bytes * 16U + (std::uint64_t{64} << 20U) &&
                jxl_encode->footprint.private_memory_bytes >= (std::uint64_t{640} << 20U),
            "JPEG XL phase plan carries the encoder working-set estimate");

    request.output = source;
    request.output.format = Format::webp;
    request.output.frames.resize(12, request.output.frames.front());
    for (auto& frame : request.output.frames)
        frame.duration = std::chrono::milliseconds(40);
    request.encode_options = {};
    request.encode_options.format = Format::webp;
    request.expected_artifact_bytes = std::uint64_t{256} << 20U;
    request.budgets.private_memory_bytes = std::uint64_t{128} << 20U;
    const auto rejected_animation = snow::image::plan_resources(request);
    require(!rejected_animation && rejected_animation.error().code == ErrorCode::limit_exceeded &&
                rejected_animation.error().message.find("requires") != std::string::npos &&
                rejected_animation.error().message.find("available") != std::string::npos,
            "large WebP animation plans fail before dispatch with required and available bytes");

    source.canvas_width = 43'200;
    source.canvas_height = 21'600;
    source.frames.front().width = source.canvas_width;
    source.frames.front().height = source.canvas_height;
    source.frames.front().layout.planes.front().width = source.canvas_width;
    source.frames.front().layout.planes.front().height = source.canvas_height;
    output.canvas_width = 1;
    output.canvas_height = 1;
    output.frames.front().width = 1;
    output.frames.front().height = 1;
    output.frames.front().layout.planes.front().width = 1;
    output.frames.front().layout.planes.front().height = 1;
    request.source = source;
    request.output = output;
    request.expected_artifact_bytes = 4U * 1024U * 1024U;
    request.budgets.private_memory_bytes = std::uint64_t{512} << 20U;
    request.budgets.temporary_disk_bytes = std::uint64_t{16} << 30U;
    plan = take(snow::image::plan_resources(request), "plan extreme downscale resources");
    require(plan.streaming_polyphase && plan.cpu_threads >= 1 &&
                plan.peak.private_memory_bytes < request.budgets.private_memory_bytes,
            "extreme downscale selects bounded polyphase accumulation");
}

void test_shared_image_ownership() {
    snow::image::MutableImage mutable_image = take(
        snow::image::MutableImage::allocate(4, 3, snow::image::kRgba8), "allocate shared image");
    std::byte* allocation = mutable_image.pixels().data();
    mutable_image.pixels().front() = std::byte{0x5A};
    Image frozen = std::move(mutable_image).freeze();
    require(frozen.pixels().data() == allocation && frozen.pixels().front() == std::byte{0x5A},
            "freezing a mutable image preserves the allocation without copying");
    // This copy is the subject of the following shared-ownership assertion.
    // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
    Image copied = frozen;
    require(copied.pixels().data() == frozen.pixels().data() &&
                copied.storage().owner() == frozen.storage().owner(),
            "immutable image copies share their pixel allocation");

    auto external = std::make_shared<std::vector<std::byte>>(16U, std::byte{0x33});
    std::weak_ptr<std::vector<std::byte>> lifetime = external;
    auto buffer =
        take(snow::image::SharedPixelBuffer::adopt(std::static_pointer_cast<const void>(external),
                                                   std::span<const std::byte>(*external)),
             "adopt external pixels");
    Image adopted = take(Image::adopt(2, 2, snow::image::kRgba8, 8, std::move(buffer)),
                         "construct adopted image");
    external.reset();
    require(!lifetime.expired() && adopted.pixels().front() == std::byte{0x33},
            "an adopted image retains its external owner");
}

void test_transform_storage_and_precision() {
    Document source = sample_document();
    const std::byte* original = source.frames.front().image.pixels().data();
    snow::image::TransformOptions identity;
    identity.resize =
        snow::image::ResizeOptions{2, 2, snow::image::ResamplingMethod::lanczos3, true, true};
    Document transformed = take(snow::image::transform(source, identity), "identity transform");
    require(transformed.frames.front().image.pixels().data() == original,
            "identity transform shares immutable source storage");

    snow::image::MutableImage hdr =
        take(snow::image::MutableImage::allocate(2, 2, snow::image::kRgba16Float),
             "allocate HDR transform input");
    std::fill(hdr.pixels().begin(), hdr.pixels().end(), std::byte{0});
    Document hdr_document;
    hdr_document.canvas_width = 2;
    hdr_document.canvas_height = 2;
    Frame hdr_frame;
    hdr_frame.image = std::move(hdr).freeze();
    hdr_document.frames.push_back(std::move(hdr_frame));
    snow::image::TransformOptions resize;
    resize.resize =
        snow::image::ResizeOptions{1, 1, snow::image::ResamplingMethod::linear, true, true};
    Document resized = take(snow::image::transform(hdr_document, resize), "resize HDR pixels");
    require(resized.frames.front().image.format() == snow::image::kRgba16Float,
            "resize preserves floating-point HDR precision");
}

Document opaque_document(std::uint32_t width = 8, std::uint32_t height = 6) {
    snow::image::MutableImage image =
        take(snow::image::MutableImage::allocate(width, height, snow::image::kRgba8),
             "allocate opaque image");
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y) * image.row_stride() + x * 4U;
            image.pixels()[offset] = static_cast<std::byte>((x * 31U + y * 7U) & 0xFFU);
            image.pixels()[offset + 1U] = static_cast<std::byte>((x * 3U + y * 47U) & 0xFFU);
            image.pixels()[offset + 2U] = static_cast<std::byte>((x * 19U + y * 11U) & 0xFFU);
            image.pixels()[offset + 3U] = std::byte{0xFF};
        }
    }
    Document document;
    document.canvas_width = width;
    document.canvas_height = height;
    Frame frame;
    frame.image = std::move(image).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

void test_alpha_classification_and_composite() {
    Document opaque = opaque_document();
    require(take(snow::image::classify_alpha(opaque.frames[0].image), "classify opaque RGBA8") ==
                snow::image::AlphaContent::opaque,
            "opaque RGBA8 alpha is classified without conversion");
    const Image sharedCopy = opaque.frames[0].image;
    require(take(snow::image::classify_alpha(sharedCopy), "classify shared opaque image") ==
                snow::image::AlphaContent::opaque,
            "copied immutable images share cached alpha classification");
    const std::byte* storage = opaque.frames[0].image.pixels().data();
    Document composited =
        take(snow::image::composite_alpha(opaque, 0, 0, 0), "composite opaque document");
    require(composited.frames[0].image.pixels().data() == storage &&
                composited.frames[0].image.storage().owner() ==
                    opaque.frames[0].image.storage().owner(),
            "opaque alpha compositing preserves shared pixel storage");

    Document transparent = opaque;
    snow::image::MutableImage changed =
        take(snow::image::MutableImage::copy(transparent.frames[0].image.view()),
             "copy transparent image");
    changed.pixels()[3] = std::byte{0x80};
    transparent.frames[0].image = std::move(changed).freeze();
    require(take(snow::image::classify_alpha(transparent.frames[0].image),
                 "classify transparent RGBA8") == snow::image::AlphaContent::non_opaque,
            "non-opaque RGBA8 alpha is detected");
    Document flattened =
        take(snow::image::composite_alpha(transparent, 0, 0, 0), "composite transparent document");
    require(flattened.frames[0].image.pixels().data() !=
                    transparent.frames[0].image.pixels().data() &&
                flattened.frames[0].image.pixels()[3] == std::byte{0xFF},
            "transparent alpha still uses the black composite path");

    const std::array<snow::image::PixelFormat, 4> formats{
        snow::image::PixelFormat{snow::image::SampleType::unsigned_integer,
                                 snow::image::ChannelLayout::gray_alpha,
                                 snow::image::AlphaMode::straight, 8, true},
        snow::image::kBgra8, snow::image::kRgba16, snow::image::kRgba32Float};
    for (const auto& format : formats) {
        snow::image::MutableImage image = take(snow::image::MutableImage::allocate(2, 1, format),
                                               "allocate alpha-layout fixture");
        std::fill(image.pixels().begin(), image.pixels().end(), std::byte{0});
        const std::size_t sample_bytes = format.bits_per_channel / 8U;
        const std::size_t pixel_bytes = format.channel_count() * sample_bytes;
        for (std::size_t pixel = 0; pixel < 2; ++pixel) {
            std::byte* alpha =
                image.pixels().data() + pixel * pixel_bytes + pixel_bytes - sample_bytes;
            if (format.sample_type == snow::image::SampleType::floating_point) {
                const float one = 1.0F;
                std::memcpy(alpha, &one, sizeof(one));
            } else {
                std::fill_n(alpha, sample_bytes, std::byte{0xFF});
            }
        }
        require(take(snow::image::classify_alpha(image.view()), "classify alpha layout") ==
                    snow::image::AlphaContent::opaque,
                "supported alpha layouts classify full-scale alpha as opaque");
    }
    Document retryable = opaque_document(2048, 2048);
    std::stop_source cancelled;
    cancelled.request_stop();
    Result<snow::image::AlphaContent> stopped =
        snow::image::classify_alpha(retryable.frames[0].image, cancelled.get_token());
    require(!stopped && stopped.error().code == ErrorCode::cancelled,
            "alpha classification honors cancellation");
    require(take(snow::image::classify_alpha(retryable.frames[0].image),
                 "retry alpha classification") == snow::image::AlphaContent::opaque,
            "cancelled cached alpha classification remains retryable");

    Document concurrent = opaque_document(1024, 1024);
    auto classify = [&concurrent] {
        return snow::image::classify_alpha(concurrent.frames[0].image);
    };
    auto first = std::async(std::launch::async, classify);
    auto second = std::async(std::launch::async, classify);
    require(take(first.get(), "first concurrent alpha classification") ==
                    snow::image::AlphaContent::opaque &&
                take(second.get(), "second concurrent alpha classification") ==
                    snow::image::AlphaContent::opaque,
            "concurrent immutable-image alpha callers share a successful result");
}

void test_persistent_file_input() {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("snow-image-input-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".bin");
    std::vector<std::byte> expected(8192);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        expected[index] = static_cast<std::byte>((index * 37U) & 0xFFU);
    }
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(expected.data()),
                     static_cast<std::streamsize>(expected.size()));
        require(output.good(), "write persistent file-input fixture");
    }
    auto input = take(snow::image::file_input(path), "open persistent file input");
    require(take(input.source->size(), "persistent file size") == expected.size(),
            "file input caches its opened size");
    const auto readRange = [&input, &expected](std::size_t offset, std::size_t count) {
        std::vector<std::byte> actual(count);
        const std::size_t read =
            take(input.source->read_at(offset, actual), "persistent random read");
        return read == count && std::equal(actual.begin(), actual.end(),
                                           expected.begin() + static_cast<std::ptrdiff_t>(offset));
    };
    require(
        readRange(17, 311) && readRange(4096, 1024) &&
            take(input.source->read_at(expected.size(), std::span<std::byte>(expected).first(1)),
                 "persistent EOF") == 0,
        "persistent file input supports repeated random reads and exact EOF");
    auto first = std::async(std::launch::async, readRange, 100U, 500U);
    auto second = std::async(std::launch::async, readRange, 6000U, 700U);
    require(first.get() && second.get(),
            "persistent file input serializes concurrent seek and read operations");
    input = {};
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

void test_raster_store() {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("snow-image-raster-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".srs");
    const std::filesystem::path incomplete_path = path.string() + ".partial";
    const std::filesystem::path decoded_path = path.string() + ".decoded";
    const std::filesystem::path mapped_write_path = path.string() + ".mapped-write";
    const std::filesystem::path bulk_copy_path = path.string() + ".bulk-copy";
    const std::filesystem::path cancelled_copy_path = path.string() + ".cancelled-copy";
    const std::filesystem::path corrupt_pixel_path = path.string() + ".corrupt-pixel";
    const std::filesystem::path chroma_path = path.string() + ".chroma";
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(incomplete_path, ignored);
    std::filesystem::remove(decoded_path, ignored);
    std::filesystem::remove(mapped_write_path, ignored);
    std::filesystem::remove(bulk_copy_path, ignored);
    std::filesystem::remove(cancelled_copy_path, ignored);
    std::filesystem::remove(corrupt_pixel_path, ignored);
    std::filesystem::remove(chroma_path, ignored);

    snow::image::DocumentDescriptor chroma_descriptor;
    chroma_descriptor.format = Format::jpeg;
    chroma_descriptor.canvas_width = 1;
    chroma_descriptor.canvas_height = 1;
    constexpr std::array chroma_layouts{
        std::pair{snow::image::ChromaSubsampling::yuv440, std::pair{1U, 2U}},
        std::pair{snow::image::ChromaSubsampling::yuv411, std::pair{4U, 1U}},
        std::pair{snow::image::ChromaSubsampling::yuv441, std::pair{1U, 4U}}};
    for (const auto& [sampling, luma_size] : chroma_layouts) {
        snow::image::RasterFrameDescriptor frame;
        frame.width = 1;
        frame.height = 1;
        frame.layout.color_model = snow::image::ColorModel::ycbcr;
        frame.layout.alpha = snow::image::AlphaMode::none;
        frame.layout.chroma_subsampling = sampling;
        frame.layout.planes = {
            {snow::image::PlaneSemantic::luma, luma_size.first, luma_size.second,
             snow::image::kGray8, 8},
            {snow::image::PlaneSemantic::chroma_blue, 1, 1, snow::image::kGray8, 8},
            {snow::image::PlaneSemantic::chroma_red, 1, 1, snow::image::kGray8, 8}};
        chroma_descriptor.frames.push_back(std::move(frame));
    }
    auto chroma_store = take(snow::image::RasterStore::create(chroma_path, chroma_descriptor),
                             "create uncommon chroma raster store");
    for (std::uint32_t frame = 0;
         frame < static_cast<std::uint32_t>(chroma_descriptor.frames.size()); ++frame) {
        const auto& planes = chroma_descriptor.frames[frame].layout.planes;
        for (std::uint32_t plane = 0; plane < static_cast<std::uint32_t>(planes.size()); ++plane) {
            std::vector<std::byte> zeros(static_cast<std::size_t>(planes[plane].width) *
                                         planes[plane].height);
            require(
                chroma_store
                    ->write_rows(frame, plane, 0, planes[plane].height, planes[plane].width, zeros)
                    .has_value(),
                "write uncommon chroma raster plane");
        }
    }
    require(chroma_store->commit().has_value(), "commit uncommon chroma raster store");
    auto reopened_chroma =
        take(snow::image::RasterStore::open(chroma_path), "reopen uncommon chroma raster store");
    for (std::size_t index = 0; index < chroma_layouts.size(); ++index) {
        require(reopened_chroma->descriptor().frames[index].layout.chroma_subsampling ==
                    chroma_layouts[index].first,
                "raster store serializes every JPEG chroma subsampling enum");
    }

    Document source = sample_document();
    snow::image::DocumentDescriptor descriptor =
        take(snow::image::describe_document(source), "describe sample raster document");
    descriptor.metadata.comment = "binary raster metadata";
    descriptor.color.icc_profile = {std::byte{0x00}, std::byte{0x7f}, std::byte{0xff}};
    descriptor.frames.front().metadata.xmp = {std::byte{'x'}, std::byte{'m'}, std::byte{'p'}};
    snow::image::RasterStoreOptions options;
    options.chunk_rows = 1;
    auto store =
        take(snow::image::RasterStore::create(path, descriptor, options), "create raster store");
    const Image& image = source.frames.front().image;
    require(
        store->write_rows(0, 0, 0, image.height(), image.row_stride(), image.pixels()).has_value(),
        "raster store accepts complete row writes");
    require(store->commit().has_value() && store->complete(),
            "raster store commits complete content");

    snow::image::RasterStoreOptions mapped_write_options;
    mapped_write_options.row_alignment = 1;
    auto mapped_write_store =
        take(snow::image::RasterStore::create(mapped_write_path, descriptor, mapped_write_options),
             "create writable-mapped raster store");
    snow::image::MutableMappedPlane mapped_write =
        take(mapped_write_store->map_plane_for_write(0, 0), "map writable raster plane");
    require(mapped_write.row_stride == image.row_stride() &&
                mapped_write.pixels.size() == image.pixels().size(),
            "writable raster mapping exposes the requested packed layout");
    std::copy(image.pixels().begin(), image.pixels().end(), mapped_write.pixels.begin());
    require(mapped_write_store->finish_mapped_plane(0, 0, mapped_write).has_value(),
            "writable raster mapping finalizes verified chunks");
    mapped_write.pixels = {};
    mapped_write.owner.reset();
    require(mapped_write_store->commit().has_value(),
            "writable raster mapping publishes through verified commit");
    auto mapped_write_reopened = take(snow::image::RasterStore::open(mapped_write_path),
                                      "reopen writable-mapped raster store");
    std::array<std::byte, 16> mapped_write_rows{};
    require(
        mapped_write_reopened->read_rows(0, 0, 0, 2, 8, mapped_write_rows).has_value() &&
            std::equal(mapped_write_rows.begin(), mapped_write_rows.end(), image.pixels().begin()),
        "writable raster mapping retains exact committed rows");

    auto bulk_copy_store =
        take(snow::image::RasterStore::create(bulk_copy_path, descriptor, mapped_write_options),
             "create bulk-copy raster store");
    require(bulk_copy_store->copy_plane(0, 0, image.row_stride(), image.pixels()).has_value() &&
                bulk_copy_store->commit().has_value(),
            "bulk plane copy publishes exact checksummed content");
    auto bulk_copy_reopened =
        take(snow::image::RasterStore::open(bulk_copy_path), "reopen bulk-copy raster store");
    std::array<std::byte, 16> bulk_copy_rows{};
    require(bulk_copy_reopened->read_rows(0, 0, 0, 2, 8, bulk_copy_rows).has_value() &&
                std::equal(bulk_copy_rows.begin(), bulk_copy_rows.end(), image.pixels().begin()),
            "bulk plane copy retains exact rows and valid chunk checksums");

    auto cancelled_copy_store = take(
        snow::image::RasterStore::create(cancelled_copy_path, descriptor, mapped_write_options),
        "create cancelled bulk-copy raster store");
    std::stop_source cancelled_copy;
    cancelled_copy.request_stop();
    const auto cancelled_status = cancelled_copy_store->copy_plane(
        0, 0, image.row_stride(), image.pixels(), cancelled_copy.get_token());
    require(!cancelled_status && cancelled_status.error().code == ErrorCode::cancelled &&
                !cancelled_copy_store->commit(),
            "cancelled bulk plane copy cannot publish partial content");
    cancelled_copy_store->abort();

    auto reopened = take(snow::image::RasterStore::open(path), "reopen raster store");
    require(reopened->descriptor().metadata.comment == "binary raster metadata" &&
                reopened->descriptor().color.icc_profile == descriptor.color.icc_profile &&
                reopened->descriptor().frames.front().metadata.xmp ==
                    descriptor.frames.front().metadata.xmp,
            "raster store preserves binary document and frame metadata");
    std::array<std::byte, 16> rows{};
    require(reopened->read_rows(0, 0, 0, 2, 8, rows).has_value() &&
                std::equal(rows.begin(), rows.end(), image.pixels().begin()),
            "raster store returns exact rows");
    std::array<std::byte, 4> region{};
    snow::image::MutablePlaneView region_view{1, 1, snow::image::kRgba8, 4, region};
    require(reopened->read_region(0, 0, {1, 0, 1, 1}, region_view).has_value() &&
                std::equal(region.begin(), region.end(), image.pixels().begin() + 4),
            "raster store reads bounded regions without owning the frame");

    require(std::filesystem::copy_file(path, corrupt_pixel_path,
                                       std::filesystem::copy_options::overwrite_existing, ignored),
            "copy raster store for pixel corruption test");
    {
        std::fstream corrupt_file(corrupt_pixel_path,
                                  std::ios::binary | std::ios::in | std::ios::out);
        require(corrupt_file.seekg(static_cast<std::streamoff>(64U << 10U), std::ios::beg).good(),
                "locate raster store pixel payload");
        char pixel = 0;
        corrupt_file.read(&pixel, 1);
        pixel = static_cast<char>(pixel ^ 0x5a);
        corrupt_file.seekp(static_cast<std::streamoff>(64U << 10U), std::ios::beg);
        corrupt_file.write(&pixel, 1);
        require(corrupt_file.good(), "corrupt raster store pixel payload");
    }
    auto corrupt_store = take(snow::image::RasterStore::open(corrupt_pixel_path),
                              "open pixel-corrupted raster store manifest");
    auto corrupt_mapping = corrupt_store->map_plane(0, 0);
    require(!corrupt_mapping && corrupt_mapping.error().code == ErrorCode::corrupt_data,
            "raster store rejects mapped pixel corruption by chunk checksum");
    corrupt_store.reset();

    Service service;
    snow::image::EncodeOptions encode_options;
    encode_options.format = Format::bmp;
    auto encoded = std::make_shared<std::vector<std::byte>>();
    require(
        service.encode(*reopened, snow::image::memory_output(encoded, "store.bmp"), encode_options)
                .has_value() &&
            !encoded->empty(),
        "encoder session consumes a raster source");
    auto session_bytes = std::make_shared<std::vector<std::byte>>();
    snow::image::EncoderSession session =
        take(service.create_encoder(reopened->descriptor(),
                                    snow::image::memory_output(session_bytes, "session.bmp"),
                                    encode_options),
             "create mapped encoder session");
    {
        MappedOnlySource mapped_only(reopened);
        require(session.encode_frame(0, mapped_only).has_value(),
                "encoder session adopts mapped frames without row copies");
    }
    require(session.finish().has_value() && !session_bytes->empty(),
            "mapped encoder session retains plane ownership through finish");
    if (service.encoder_info(Format::png)) {
        snow::image::EncodeOptions png_options;
        png_options.format = Format::png;
        png_options.preserve_metadata = false;
        auto png_encoded = std::make_shared<std::vector<std::byte>>();
        MappedOnlySource mapped_only(reopened);
        require(service.encode(mapped_only, snow::image::memory_output(png_encoded, "store.png"),
                               png_options)
                        .has_value() &&
                    !png_encoded->empty(),
                "PNG encoder consumes mapped planes without row-copy materialization");
    }
    auto decoded_store =
        take(service.decode_to_store(snow::image::memory_input(encoded, "store.bmp"), decoded_path),
             "decode directly into raster store");
    std::array<std::byte, 16> decoded_rows{};
    require(decoded_store->read_rows(0, 0, 0, 2, 8, decoded_rows).has_value() &&
                std::equal(decoded_rows.begin(), decoded_rows.end(), image.pixels().begin()),
            "decoder streams directly into a raster writer");

    auto incomplete = take(snow::image::RasterStore::create(incomplete_path, descriptor, options),
                           "create incomplete raster store");
    auto rejected = snow::image::RasterStore::open(incomplete_path);
    require(!rejected && rejected.error().code == ErrorCode::corrupt_data,
            "raster store rejects incomplete publication");
    incomplete->abort();
    require(!std::filesystem::exists(incomplete_path),
            "aborted raster store removes its partial file");

    snow::image::MappedPlane mapped = take(reopened->map_plane(0, 0), "map committed raster plane");
    require(mapped.owner && mapped.row_stride >= image.row_stride() &&
                mapped.pixels.size() >= mapped.row_stride * image.height(),
            "raster store exposes a complete row-aligned plane mapping");
    require(std::equal(image.pixels().begin(),
                       image.pixels().begin() + static_cast<std::ptrdiff_t>(image.row_stride()),
                       mapped.pixels.begin()) &&
                std::equal(image.pixels().begin() + static_cast<std::ptrdiff_t>(image.row_stride()),
                           image.pixels().end(),
                           mapped.pixels.begin() + static_cast<std::ptrdiff_t>(mapped.row_stride)),
            "mapped raster rows retain exact pixel content");
    reopened.reset();
    store.reset();
    require(std::equal(image.pixels().begin(),
                       image.pixels().begin() + static_cast<std::ptrdiff_t>(image.row_stride()),
                       mapped.pixels.begin()),
            "mapped raster plane remains valid after store destruction");
    decoded_store.reset();
    mapped_write_reopened.reset();
    mapped_write_store.reset();
    reopened_chroma.reset();
    chroma_store.reset();
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(decoded_path, ignored);
    std::filesystem::remove(mapped_write_path, ignored);
    std::filesystem::remove(bulk_copy_path, ignored);
    std::filesystem::remove(cancelled_copy_path, ignored);
    std::filesystem::remove(corrupt_pixel_path, ignored);
    std::filesystem::remove(chroma_path, ignored);
}

void test_raster_buffer_store() {
    snow::image::DocumentDescriptor descriptor;
    descriptor.format = Format::png;
    descriptor.canvas_width = 4;
    descriptor.canvas_height = 3;
    snow::image::RasterFrameDescriptor frame;
    frame.width = 4;
    frame.height = 3;
    frame.layout.color_model = snow::image::ColorModel::rgb;
    frame.layout.alpha = snow::image::AlphaMode::straight;
    frame.layout.planes = {{snow::image::PlaneSemantic::packed, 4, 3, snow::image::kRgba8, 8},
                           {snow::image::PlaneSemantic::index, 4, 3, snow::image::kGray8, 8}};
    descriptor.frames.push_back(frame);

    snow::image::RasterBufferStoreOptions options;
    options.row_alignment = 16;
    for (std::size_t index = 0; index < options.session_nonce.size(); ++index)
        options.session_nonce[index] = static_cast<std::byte>(index + 1U);
    options.analysis.alpha_content = snow::image::AlphaContent::non_opaque;
    const std::uint64_t required =
        take(snow::image::RasterBufferStore::required_bytes(descriptor, options),
             "calculate raster buffer size");
    require(required > 96U, "raster buffer includes header and manifest storage");
    std::vector<std::byte> storage(static_cast<std::size_t>(required));
    std::vector<std::byte> unsealed = storage;
    auto writable = take(snow::image::RasterBufferStore::create(storage, descriptor, options),
                         "create writable raster buffer");
    unsealed = storage;
    require(!snow::image::RasterBufferStore::open(unsealed),
            "unsealed raster buffer cannot be reopened");

    const std::size_t packedRowBytes = 16;
    const std::size_t packedSourceStride = 20;
    std::vector<std::byte> packedSource(packedSourceStride * 3U, std::byte{0});
    for (std::uint32_t row = 0; row < 3; ++row) {
        for (std::size_t index = 0; index < packedRowBytes; ++index) {
            packedSource[static_cast<std::size_t>(row) * packedSourceStride + index] =
                static_cast<std::byte>((row * packedRowBytes + index) & 0xFFU);
        }
    }
    auto mapped = take(writable->map_plane_for_write(0, 0), "map raster buffer plane for writing");
    require(mapped.row_stride == 16 && mapped.pixels.size() == 16U * 3U,
            "raster buffer mapping honors row alignment");
    for (std::uint32_t row = 0; row < 3; ++row) {
        std::memcpy(mapped.pixels.data() + static_cast<std::size_t>(row) * mapped.row_stride,
                    packedSource.data() + static_cast<std::size_t>(row) * packedSourceStride,
                    packedRowBytes);
    }
    require(writable->finish_mapped_plane(0, 0, mapped).has_value(),
            "raster buffer mapped plane write is finalized");
    mapped.owner.reset();

    std::array<std::byte, 12> indexRows{};
    for (std::size_t index = 0; index < indexRows.size(); ++index)
        indexRows[index] = static_cast<std::byte>(index + 40U);
    require(writable->write_rows(0, 1, 0, 3, 4, indexRows).has_value(),
            "raster buffer accepts direct row writes for a second plane");
    require(writable->commit().has_value() && writable->complete(),
            "raster buffer seals after all plane rows are written");
    require(writable->analysis().alpha_content == snow::image::AlphaContent::non_opaque,
            "raster buffer retains alpha analysis metadata");

    auto reopened = take(snow::image::RasterBufferStore::open(std::span<const std::byte>(storage),
                                                              {}, options.session_nonce, required),
                         "reopen sealed raster buffer");
    const auto& reopenedFrame = reopened->descriptor().frames.front();
    const auto& expectedFrame = descriptor.frames.front();
    require(reopened->descriptor().format == descriptor.format &&
                reopened->descriptor().canvas_width == descriptor.canvas_width &&
                reopened->descriptor().canvas_height == descriptor.canvas_height &&
                reopenedFrame.width == expectedFrame.width &&
                reopenedFrame.height == expectedFrame.height &&
                reopenedFrame.layout == expectedFrame.layout && reopened->byte_size() == required &&
                reopened->session_nonce() == options.session_nonce,
            "sealed raster buffer round-trips descriptor, size, and nonce");
    require(!reopened->map_plane_for_write(0, 0),
            "read-only raster buffer rejects writable mappings");
    std::array<std::byte, 36> copiedRows{};
    require(reopened->read_rows(0, 0, 0, 2, packedSourceStride, copiedRows).has_value() &&
                std::equal(copiedRows.begin(), copiedRows.begin() + packedRowBytes,
                           packedSource.begin()) &&
                std::equal(copiedRows.begin() + packedSourceStride,
                           copiedRows.begin() + packedSourceStride + packedRowBytes,
                           packedSource.begin() + packedSourceStride),
            "sealed raster buffer returns exact mapped rows");
    std::array<std::byte, 12> copiedIndex{};
    require(reopened->read_rows(0, 1, 0, 3, 4, copiedIndex).has_value() && copiedIndex == indexRows,
            "sealed raster buffer returns exact multi-plane row writes");

    auto wrongNonce = options.session_nonce;
    wrongNonce[0] = std::byte{0xFF};
    require(!snow::image::RasterBufferStore::open(std::span<const std::byte>(storage), {},
                                                  wrongNonce, required),
            "raster buffer rejects a mismatched session nonce");
    require(!snow::image::RasterBufferStore::open(std::span<const std::byte>(storage), {},
                                                  options.session_nonce, required + 1U),
            "raster buffer rejects a mismatched expected size");

    // Native shared-memory segments may be larger than the logical package due
    // to page-size rounding. The physical size must be supplied explicitly for
    // that padded span to be accepted.
    std::vector<std::byte> padded(static_cast<std::size_t>(required + 4096U));
    std::copy(storage.begin(), storage.end(), padded.begin());
    require(!snow::image::RasterBufferStore::open(std::span<const std::byte>(padded)),
            "padded raster storage requires an authenticated physical size");
    auto paddedReopened =
        take(snow::image::RasterBufferStore::open(std::span<const std::byte>(padded), {},
                                                  options.session_nonce, padded.size()),
             "reopen raster buffer with shared-memory padding");
    require(paddedReopened->byte_size() == required,
            "padded shared-memory storage preserves its logical package size");

    std::vector<std::byte> badHeader = storage;
    badHeader[48] ^= std::byte{0x01};
    require(!snow::image::RasterBufferStore::open(badHeader),
            "raster buffer rejects a corrupted header checksum");
    std::vector<std::byte> badManifest = storage;
    badManifest[96] ^= std::byte{0x01};
    require(!snow::image::RasterBufferStore::open(badManifest),
            "raster buffer rejects a corrupted manifest checksum");
}

void test_encode_receipts(Service& service) {
    const std::array formats{Format::bmp, Format::png,  Format::jpeg, Format::pbm,
                             Format::pgm, Format::ppm,  Format::gif,  Format::ico,
                             Format::cur, Format::webp, Format::xbm,  Format::xpm};
    for (const Format format : formats) {
        if (!supports(service, format, snow::image::CodecCapability::encode) ||
            !supports(service, format, snow::image::CodecCapability::inspect))
            continue;
        Document document = sample_document();
        document.format = format;
        snow::image::EncodeOptions options;
        options.format = format;
        options.lossless = format == Format::png || format == Format::webp;
        auto bytes = std::make_shared<std::vector<std::byte>>();
        const auto encoded = service.encode(document, snow::image::memory_output(bytes), options);
        require(encoded.has_value() && !bytes->empty(),
                "receipt fixture encodes for every enabled simple codec");
        const auto& receipt = encoded.value().receipt;
        require(receipt.format == format &&
                    receipt.document_kind == snow::image::DocumentKind::raster &&
                    receipt.encoder_finalized_and_sink_flushed &&
                    receipt.emitted_frame_count == receipt.emitted_frame_extents.size() &&
                    encoded.value().bytes_written == bytes->size(),
                "codec receipt reports finalized bytes and format");
        const auto inspected = take(service.inspect(snow::image::memory_input(bytes)),
                                    "independently inspect receipt fixture");
        require(receipt.canvas_width == inspected.canvas_width &&
                    receipt.canvas_height == inspected.canvas_height &&
                    receipt.emitted_frame_extents.size() == inspected.frames.size(),
                "codec receipt canvas and frame count agree with independent inspection");
        for (std::size_t index = 0; index < inspected.frames.size(); ++index) {
            const auto& extent = receipt.emitted_frame_extents[index];
            const auto& frame = inspected.frames[index];
            require(extent.x == frame.x && extent.y == frame.y && extent.width == frame.width &&
                        extent.height == frame.height,
                    "codec receipt extents agree with independent inspection");
        }
    }

    if (supports(service, Format::png, snow::image::CodecCapability::encode)) {
        const Document document = sample_document();
        const auto descriptor =
            take(snow::image::describe_document(document), "describe encoder-session fixture");
        const auto path =
            std::filesystem::temp_directory_path() /
            ("snow-image-receipt-session-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".srs");
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        auto store = take(snow::image::RasterStore::create(path, descriptor),
                          "create encoder-session source");
        require(store->copy_plane(0, 0, document.frames.front().image.row_stride(),
                                  document.frames.front().image.pixels())
                        .has_value() &&
                    store->commit().has_value(),
                "commit encoder-session source");
        auto bytes = std::make_shared<std::vector<std::byte>>();
        snow::image::EncodeOptions options;
        options.format = Format::png;
        auto mutableSession =
            take(service.create_encoder(descriptor, snow::image::memory_output(bytes), options),
                 "create receipt encoder session");
        MappedOnlySource source(store);
        require(mutableSession.encode_frame(0, source).has_value(),
                "encoder session accepts its raster frame");
        const auto finished = mutableSession.finish();
        require(finished.has_value() && finished.value().receipt.format == Format::png &&
                    finished.value().receipt.emitted_frame_count == 1 &&
                    finished.value().receipt.encoder_finalized_and_sink_flushed,
                "encoder session finish returns a validated artifact receipt");
        store.reset();
        std::filesystem::remove(path, ignored);
    }

    if (supports(service, Format::svg, snow::image::CodecCapability::encode)) {
        const std::string svg =
            "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"7\" height=\"5\"></svg>";
        Document vector;
        vector.format = Format::svg;
        vector.canvas_width = 7;
        vector.canvas_height = 5;
        vector.vector = snow::image::VectorDocument{
            std::vector<std::byte>(reinterpret_cast<const std::byte*>(svg.data()),
                                   reinterpret_cast<const std::byte*>(svg.data()) + svg.size()),
            false};
        snow::image::EncodeOptions options;
        options.format = Format::svg;
        auto bytes = std::make_shared<std::vector<std::byte>>();
        const auto encoded = service.encode(vector, snow::image::memory_output(bytes), options);
        require(encoded.has_value() &&
                    encoded.value().receipt.document_kind == snow::image::DocumentKind::vector &&
                    encoded.value().receipt.canvas_width == 7 &&
                    encoded.value().receipt.canvas_height == 5 &&
                    encoded.value().receipt.emitted_frame_count == 0,
                "SVG receipt reports the outer vector artifact");
    }

    if (supports(service, Format::png, snow::image::CodecCapability::encode)) {
        auto sink = std::make_shared<FlushFailingByteSink>();
        snow::image::EncodeOptions options;
        options.format = Format::png;
        const auto failed = service.encode(sample_document(),
                                           snow::image::Output{sink, "flush-failure.png"}, options);
        require(!failed && failed.error().code == ErrorCode::io_error && sink->flushes == 1 &&
                    sink->bytes != 0,
                "codec wrapper reports a sink flush failure after native finalization");
    }
}

Document png_palette_document(snow::image::PixelFormat format, std::uint32_t color_count) {
    snow::image::MutableImage image =
        take(snow::image::MutableImage::allocate(color_count, 1, format),
             "allocate PNG palette fixture");
    const std::size_t pixelBytes = take(format.bytes_per_pixel(), "measure PNG palette pixel");
    const bool bgr = format.channels == snow::image::ChannelLayout::bgr ||
                     format.channels == snow::image::ChannelLayout::bgra;
    const bool alpha = format.channels == snow::image::ChannelLayout::rgba ||
                       format.channels == snow::image::ChannelLayout::bgra;
    for (std::uint32_t x = 0; x < color_count; ++x) {
        std::byte* pixel = image.pixels().data() + static_cast<std::size_t>(x) * pixelBytes;
        const std::byte red = static_cast<std::byte>(x & 0xFFU);
        const std::byte green = static_cast<std::byte>((x >> 8U) & 0xFFU);
        const std::byte blue = static_cast<std::byte>((x * 73U) & 0xFFU);
        pixel[bgr ? 2U : 0U] = red;
        pixel[1] = green;
        pixel[bgr ? 0U : 2U] = blue;
        if (alpha)
            pixel[3] = static_cast<std::byte>(x == 0 ? 0 : 0xFF);
    }
    Document document;
    document.format = Format::png;
    document.canvas_width = color_count;
    document.canvas_height = 1;
    Frame frame;
    frame.image = std::move(image).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

void test_png_palette_behavior(Service& service) {
    if (!supports(service, Format::png, snow::image::CodecCapability::encode))
        return;
    snow::image::EncodeOptions options;
    options.format = Format::png;
    const std::array<std::uint32_t, 4> counts{1, 2, 255, 256};
    for (const std::uint32_t count : counts) {
        auto bytes = std::make_shared<std::vector<std::byte>>();
        const auto encoded = service.encode(png_palette_document(snow::image::kRgb8, count),
                                            snow::image::memory_output(bytes), options);
        require(encoded.has_value() && bytes->size() > 25U && (*bytes)[25] == std::byte{3},
                "eligible PNG color counts use deterministic indexed encoding");
    }
    auto photographic = std::make_shared<std::vector<std::byte>>();
    require(service.encode(png_palette_document(snow::image::kRgb8, 257),
                           snow::image::memory_output(photographic), options)
                    .has_value() &&
                photographic->size() > 25U && (*photographic)[25] == std::byte{2},
            "257-color PNG input falls back to direct RGB encoding");

    const snow::image::PixelFormat bgr8{snow::image::SampleType::unsigned_integer,
                                        snow::image::ChannelLayout::bgr,
                                        snow::image::AlphaMode::none, 8, true};
    const std::array<snow::image::PixelFormat, 4> channelFormats{
        snow::image::kRgb8, snow::image::kRgba8, bgr8, snow::image::kBgra8};
    for (const snow::image::PixelFormat format : channelFormats) {
        Document document = png_palette_document(format, 2);
        auto first = std::make_shared<std::vector<std::byte>>();
        auto second = std::make_shared<std::vector<std::byte>>();
        require(
            service.encode(document, snow::image::memory_output(first), options).has_value() &&
                service.encode(document, snow::image::memory_output(second), options).has_value() &&
                *first == *second,
            "PNG palette ordering is deterministic across channel layouts");
        const Document decoded = take(service.decode(snow::image::memory_input(first)),
                                      "decode channel-order PNG palette fixture");
        require(decoded.frames.front().image.pixels()[0] == std::byte{0} &&
                    decoded.frames.front().image.pixels()[1] == std::byte{0} &&
                    decoded.frames.front().image.pixels()[2] == std::byte{0} &&
                    decoded.frames.front().image.pixels()[4] == std::byte{1} &&
                    decoded.frames.front().image.pixels()[5] == std::byte{0} &&
                    decoded.frames.front().image.pixels()[6] == std::byte{73},
                "PNG palette preserves RGB/BGR channel ordering");
    }

    auto transparent = std::make_shared<std::vector<std::byte>>();
    require(service
                .encode(png_palette_document(snow::image::kRgba8, 2),
                        snow::image::memory_output(transparent), options)
                .has_value(),
            "transparent PNG palette fixture encodes");
    const std::array<std::byte, 4> trns{std::byte{'t'}, std::byte{'R'}, std::byte{'N'},
                                        std::byte{'S'}};
    require(std::search(transparent->begin(), transparent->end(), trns.begin(), trns.end()) !=
                transparent->end(),
            "transparent PNG palette emits a tRNS chunk");
    const auto decodedTransparent = take(service.decode(snow::image::memory_input(transparent)),
                                         "decode transparent PNG palette fixture");
    require(decodedTransparent.frames.front().image.pixels()[3] == std::byte{0},
            "transparent PNG palette retains alpha samples");

    std::stop_source cancelled;
    cancelled.request_stop();
    const auto stopped =
        service.encode(png_palette_document(snow::image::kRgb8, 256),
                       snow::image::memory_output(std::make_shared<std::vector<std::byte>>()),
                       options, cancelled.get_token());
    require(!stopped && stopped.error().code == ErrorCode::cancelled,
            "PNG palette discovery honors pre-cancelled encoding");
}

void test_parallel_resize_determinism() {
    Document source = opaque_document(37, 29);
    snow::image::TransformOptions single;
    single.resize =
        snow::image::ResizeOptions{19, 11, snow::image::ResamplingMethod::lanczos3, true, true, 1};
    snow::image::TransformOptions parallel = single;
    parallel.resize->maximum_threads = 4;
    Document one = take(snow::image::transform(source, single), "single-threaded Lanczos resize");
    Document many = take(snow::image::transform(source, parallel), "parallel Lanczos resize");
    require(one.frames[0].image.pixels().size() == many.frames[0].image.pixels().size() &&
                std::equal(one.frames[0].image.pixels().begin(), one.frames[0].image.pixels().end(),
                           many.frames[0].image.pixels().begin()),
            "Lanczos output is bit-identical across thread limits");

    snow::image::MutableImage bgra =
        take(snow::image::MutableImage::allocate(37, 29, snow::image::kBgra8),
             "allocate BGRA resize fixture");
    const auto rgbaPixels = source.frames[0].image.pixels();
    for (std::size_t offset = 0; offset < rgbaPixels.size(); offset += 4U) {
        bgra.pixels()[offset] = rgbaPixels[offset + 2U];
        bgra.pixels()[offset + 1U] = rgbaPixels[offset + 1U];
        bgra.pixels()[offset + 2U] = rgbaPixels[offset];
        bgra.pixels()[offset + 3U] = rgbaPixels[offset + 3U];
    }
    Document bgraSource = source;
    bgraSource.frames[0].image = std::move(bgra).freeze();
    Document bgraOne =
        take(snow::image::transform(bgraSource, single), "single-threaded BGRA Lanczos resize");
    Document bgraMany =
        take(snow::image::transform(bgraSource, parallel), "parallel BGRA Lanczos resize");
    require(std::equal(bgraOne.frames[0].image.pixels().begin(),
                       bgraOne.frames[0].image.pixels().end(),
                       bgraMany.frames[0].image.pixels().begin()),
            "BGRA8 Lanczos fast path is bit-identical across thread limits");

    snow::image::DocumentInfo info;
    info.canvas_width = 37;
    info.canvas_height = 29;
    info.frames.push_back({37,
                           29,
                           0,
                           0,
                           {},
                           snow::image::kRgba8,
                           true,
                           {},
                           {},
                           {},
                           snow::image::FrameBlend::source,
                           snow::image::FrameDisposal::keep});
    auto one_estimate = take(snow::image::estimate_transform_resources(info, single),
                             "single-thread resize estimate");
    auto many_estimate =
        take(snow::image::estimate_transform_resources(info, parallel), "parallel resize estimate");
    require(many_estimate.private_memory_bytes > one_estimate.private_memory_bytes,
            "resize estimates include per-thread row-ring storage");

    std::stop_source cancellation;
    cancellation.request_stop();
    Result<Document> stopped = snow::image::transform(source, parallel, cancellation.get_token());
    require(!stopped && stopped.error().code == ErrorCode::cancelled,
            "parallel resize honors cancellation");
}

Document patterned_resize_document(std::uint32_t width, std::uint32_t height,
                                   snow::image::PixelFormat format) {
    snow::image::MutableImage image =
        take(snow::image::MutableImage::allocate(width, height, format),
             "allocate streaming resize fixture");
    const bool floating = format.sample_type == snow::image::SampleType::floating_point;
    const bool bgr = format.channels == snow::image::ChannelLayout::bgra;
    const std::size_t pixel_bytes = take(format.bytes_per_pixel(), "get resize fixture pixel size");
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const float alpha = static_cast<float>(17U + ((x * 29U + y * 13U) % 239U)) / 255.0F;
            float red = static_cast<float>((x * 41U + y * 7U) & 0xFFU) / 255.0F;
            float green = static_cast<float>((x * 11U + y * 31U) & 0xFFU) / 255.0F;
            float blue = static_cast<float>((x * 3U + y * 47U) & 0xFFU) / 255.0F;
            if (format.alpha == snow::image::AlphaMode::premultiplied) {
                red *= alpha;
                green *= alpha;
                blue *= alpha;
            }
            std::byte* pixel = image.pixels().data() +
                               static_cast<std::size_t>(y) * image.row_stride() +
                               static_cast<std::size_t>(x) * pixel_bytes;
            if (floating) {
                const std::array<float, 4> values{bgr ? blue : red, green, bgr ? red : blue, alpha};
                std::memcpy(pixel, values.data(), sizeof(values));
            } else {
                pixel[0] = static_cast<std::byte>(std::lround((bgr ? blue : red) * 255.0F));
                pixel[1] = static_cast<std::byte>(std::lround(green * 255.0F));
                pixel[2] = static_cast<std::byte>(std::lround((bgr ? red : blue) * 255.0F));
                pixel[3] = static_cast<std::byte>(std::lround(alpha * 255.0F));
            }
        }
    }
    Document document;
    document.canvas_width = width;
    document.canvas_height = height;
    Frame frame;
    frame.image = std::move(image).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

void test_streaming_resize() {
    snow::image::PixelFormat premultiplied_rgba = snow::image::kRgba8;
    premultiplied_rgba.alpha = snow::image::AlphaMode::premultiplied;
    const std::array<snow::image::PixelFormat, 4> formats{
        snow::image::kRgba8, snow::image::kBgra8, premultiplied_rgba, snow::image::kRgba32Float};
    for (const snow::image::PixelFormat& format : formats) {
        const Document source = patterned_resize_document(23, 257, format);
        for (snow::image::ResamplingMethod method :
             {snow::image::ResamplingMethod::linear, snow::image::ResamplingMethod::lanczos3}) {
            for (bool premultiply : {false, true}) {
                snow::image::TransformOptions cached;
                cached.resize = snow::image::ResizeOptions{
                    17, 3, method, premultiply, true, 1, std::numeric_limits<std::uint64_t>::max()};
                snow::image::TransformOptions streaming = cached;
                streaming.resize->maximum_worker_cache_bytes = 1;
                const Document reference = take(snow::image::transform(source, cached),
                                                "cached polyphase resize reference");
                const Document bounded = take(snow::image::transform(source, streaming),
                                              "source-major polyphase resize");
                require(reference.frames.front().image.pixels().size() ==
                                bounded.frames.front().image.pixels().size() &&
                            std::equal(reference.frames.front().image.pixels().begin(),
                                       reference.frames.front().image.pixels().end(),
                                       bounded.frames.front().image.pixels().begin()),
                        "source-major resize is bit-identical to cached polyphase");
            }
        }
    }

    const Document tall = patterned_resize_document(64, 32'768, snow::image::kRgba8);
    snow::image::TransformOptions bounded_options;
    bounded_options.resize =
        snow::image::ResizeOptions{64, 1, snow::image::ResamplingMethod::lanczos3, true, true, 1};
    const Document reduced =
        take(snow::image::transform(tall, bounded_options), "bounded extreme-aspect-ratio resize");
    require(reduced.canvas_width == 64 && reduced.canvas_height == 1 &&
                reduced.frames.front().image.pixels().size() == 64U * 4U,
            "extreme vertical downscale completes with bounded accumulators");

    snow::image::DocumentInfo info;
    info.canvas_width = 64;
    info.canvas_height = 32'768;
    info.frames.push_back({64,
                           32'768,
                           0,
                           0,
                           {},
                           snow::image::kRgba8,
                           true,
                           {},
                           {},
                           {},
                           snow::image::FrameBlend::source,
                           snow::image::FrameDisposal::keep});
    const auto bounded_estimate =
        take(snow::image::estimate_transform_resources(info, bounded_options),
             "estimate bounded extreme resize");
    snow::image::TransformOptions cached_options = bounded_options;
    cached_options.resize->maximum_worker_cache_bytes = std::numeric_limits<std::uint64_t>::max();
    const auto cached_estimate =
        take(snow::image::estimate_transform_resources(info, cached_options),
             "estimate unbounded-cache extreme resize");
    require(bounded_estimate.private_memory_bytes < cached_estimate.private_memory_bytes &&
                bounded_estimate.private_memory_bytes < (std::uint64_t{32} << 20U),
            "streaming resize estimates reflect the bounded working set");

    snow::image::DocumentDescriptor source_descriptor =
        take(snow::image::describe_document(tall), "describe extreme resize source");
    snow::image::DocumentDescriptor output_descriptor = source_descriptor;
    output_descriptor.canvas_height = 1;
    output_descriptor.frames.front().height = 1;
    output_descriptor.frames.front().layout.planes.front().height = 1;
    snow::image::ResourcePlanRequest request;
    request.source = source_descriptor;
    request.output = output_descriptor;
    request.gpu_transform = false;
    const snow::image::ResourcePlan plan =
        take(snow::image::plan_resources(request), "plan extreme streaming resize");
    require(plan.streaming_polyphase && plan.cpu_threads == 1,
            "resource planner agrees with source-major resize admission");

    std::stop_source pre_cancelled;
    pre_cancelled.request_stop();
    Result<Document> stopped =
        snow::image::transform(tall, bounded_options, pre_cancelled.get_token());
    require(!stopped && stopped.error().code == ErrorCode::cancelled,
            "streaming resize honors pre-cancellation");

    std::stop_source in_flight;
    std::promise<void> entered;
    std::future<void> entered_future = entered.get_future();
    auto operation = std::async(std::launch::async, [&] {
        entered.set_value();
        return snow::image::transform(tall, bounded_options, in_flight.get_token());
    });
    entered_future.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    in_flight.request_stop();
    stopped = operation.get();
    require(!stopped && stopped.error().code == ErrorCode::cancelled,
            "streaming resize honors in-flight cancellation");
}

void test_opaque_codec_alpha_omission(Service& service) {
    for (Format format : {Format::png, Format::heif, Format::avif, Format::jxl}) {
        if (!service.encoder_info(format))
            continue;
        Document document = opaque_document(32, 32);
        document.format = format;
        snow::image::EncodeOptions options;
        options.format = format;
        options.lossless = format == Format::png || format == Format::jxl;
        auto bytes = std::make_shared<std::vector<std::byte>>();
        Result<snow::image::EncodeResult> encoded =
            service.encode(document, snow::image::memory_output(bytes), options);
        require(encoded.has_value(), "opaque alpha-omission fixture encodes");
        auto info = take(service.inspect(snow::image::memory_input(bytes)),
                         "inspect opaque alpha-omission fixture");
        if (info.frames.empty() || info.frames.front().has_alpha) {
            std::cerr << "opaque alpha omission failed for format " << static_cast<int>(format)
                      << '\n';
        }
        require(!info.frames.empty() && !info.frames.front().has_alpha,
                "opaque alpha-capable codec omits its alpha channel");
    }
}

void test_jxl_opaque_progressive_preview(Service& service) {
    if (!service.encoder_info(Format::jxl))
        return;
    Document document = opaque_document(128, 64);
    document.format = Format::jxl;
    document.frames.front().duration = std::chrono::milliseconds(40);
    Frame second = document.frames.front();
    second.duration = std::chrono::milliseconds(60);
    second.blend = snow::image::FrameBlend::over;
    document.frames.push_back(std::move(second));
    snow::image::EncodeOptions options;
    options.format = Format::jxl;
    options.quality = 75;
    options.effort = 7;
    options.progressive = true;
    auto bytes = std::make_shared<std::vector<std::byte>>();
    Result<snow::image::EncodeResult> encoded =
        service.encode(document, snow::image::memory_output(bytes), options);
    if (!encoded) {
        std::cerr << "opaque progressive JPEG XL encode failed: " << encoded.error().message
                  << '\n';
    }
    require(encoded.has_value(),
            "opaque blended JPEG XL animation encodes with the preview defaults");
    const auto info = take(service.inspect(snow::image::memory_input(bytes)),
                           "inspect opaque progressive JPEG XL");
    require(info.frames.size() == 2 && !info.frames.front().has_alpha,
            "opaque blended JPEG XL animation omits its alpha channel");

    snow::image::MutableImage float_image =
        take(snow::image::MutableImage::allocate(128, 64, snow::image::kRgba16Float),
             "allocate opaque JPEG XL GPU preview fixture");
    const std::array<std::uint16_t, 4> half_pixel{0x3800U, 0x3400U, 0x3000U, 0x3C00U};
    for (std::size_t offset = 0; offset < float_image.pixels().size();
         offset += sizeof(half_pixel)) {
        std::memcpy(float_image.pixels().data() + offset, half_pixel.data(), sizeof(half_pixel));
    }
    Document float_document;
    float_document.canvas_width = float_image.width();
    float_document.canvas_height = float_image.height();
    float_document.color.primaries = snow::image::ColorPrimaries::srgb;
    float_document.color.transfer = snow::image::TransferFunction::linear;
    Frame float_frame;
    float_frame.image = std::move(float_image).freeze();
    float_document.frames.push_back(std::move(float_frame));
    bytes = std::make_shared<std::vector<std::byte>>();
    encoded = service.encode(float_document, snow::image::memory_output(bytes), options);
    if (!encoded) {
        std::cerr << "opaque float16 progressive JPEG XL encode failed: " << encoded.error().message
                  << '\n';
    }
    require(encoded.has_value(), "opaque RGBA16F JPEG XL encodes with the GPU preview defaults");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--jpeg-only") {
        Service service;
        test_jpeg_round_trip(service);
        std::cout << "snow_image JPEG tests passed\n";
        return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--heif-only") {
        Service service;
        test_heif_family(service);
        std::cout << "snow_image HEIF tests passed\n";
        return 0;
    }
    test_shared_image_ownership();
    test_alpha_classification_and_composite();
    test_persistent_file_input();
    test_raster_store();
    test_raster_buffer_store();
    test_format_mapping();
    Service service;
    test_bmp_round_trip(service);
    test_netpbm(service);
    test_limits(service);
    test_png_round_trip(service);
    test_png_palette_behavior(service);
    test_encode_receipts(service);
    test_jpeg_round_trip(service);
    test_gif_animation(service);
    test_icon_containers(service);
    test_xbitmap_formats(service);
    test_svg_vector_round_trip(service);
    test_heif_family(service);
    test_jpeg_xl(service);
    test_openexr(service);
    test_processing();
    test_transform_storage_and_precision();
    test_parallel_resize_determinism();
    test_streaming_resize();
    test_opaque_codec_alpha_omission(service);
    test_jxl_opaque_progressive_preview(service);
    test_webp(service);
    test_resource_estimates();
    test_phase_resource_plan();
    std::cout << "snow_image core tests passed\n";
    return 0;
}
