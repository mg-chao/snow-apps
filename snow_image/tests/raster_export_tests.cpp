#include "snow/image/io.h"
#include "snow/image/processing.h"
#include "snow/image/service.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace snow::image;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename T> T take(Result<T> result, std::string_view context) {
    if (!result)
        throw std::runtime_error(std::string(context) + ": " + result.error().message);
    return std::move(result).value();
}

DocumentDescriptor descriptorFor(std::uint32_t width, std::uint32_t height,
                                 PixelFormat format = kRgba8) {
    DocumentDescriptor descriptor;
    descriptor.format = Format::png;
    descriptor.canvas_width = width;
    descriptor.canvas_height = height;
    descriptor.color.primaries = ColorPrimaries::srgb;
    descriptor.color.transfer = TransferFunction::srgb;
    RasterFrameDescriptor frame;
    frame.width = width;
    frame.height = height;
    frame.layout.color_model = ColorModel::rgb;
    frame.layout.alpha = format.alpha;
    frame.layout.planes.push_back(
        {PlaneSemantic::packed, width, height, format, format.bits_per_channel});
    descriptor.frames.push_back(std::move(frame));
    return descriptor;
}

class RowSource final : public RasterSource {
  public:
    RowSource(std::uint32_t width, std::uint32_t height, PixelFormat format = kRgba8)
        : descriptor_(descriptorFor(width, height, format)),
          pixels_(static_cast<std::size_t>(width) * height * format.channel_count()) {
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * width + x) * format.channel_count();
                pixels_[offset] = static_cast<std::byte>((x * 31U + y * 7U) & 0xffU);
                pixels_[offset + 1U] = static_cast<std::byte>((x * 3U + y * 29U) & 0xffU);
                pixels_[offset + 2U] = static_cast<std::byte>((x * 17U + y * 11U) & 0xffU);
                if (format.channel_count() == 4)
                    pixels_[offset + 3U] = static_cast<std::byte>((x + y) % 5U == 0U ? 73U : 255U);
            }
        }
    }

    const DocumentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    RasterAccess access() const noexcept override {
        return RasterAccess::sequential_rows | RasterAccess::random_rows;
    }
    Result<void> read_rows(std::uint32_t frame, std::uint32_t plane, std::uint32_t first,
                           std::uint32_t count, std::size_t stride,
                           std::span<std::byte> destination, std::stop_token stop) const override {
        const std::size_t row_bytes =
            static_cast<std::size_t>(descriptor_.canvas_width) *
            descriptor_.frames.front().layout.planes.front().format.channel_count();
        if (stop.stop_requested())
            return Status::error(ErrorCode::cancelled, "row read cancelled");
        if (frame != 0 || plane != 0 || count == 0 || first > descriptor_.canvas_height ||
            count > descriptor_.canvas_height - first || stride < row_bytes ||
            destination.size() < static_cast<std::size_t>(count - 1U) * stride + row_bytes) {
            return Status::error(ErrorCode::invalid_argument, "invalid row read");
        }
        maximum_rows_ = std::max(maximum_rows_, count);
        if (!requested_rows_.empty() && first <= requested_rows_.back())
            strictly_increasing_ = false;
        requested_rows_.push_back(first);
        for (std::uint32_t row = 0; row < count; ++row) {
            std::memcpy(destination.data() + static_cast<std::size_t>(row) * stride,
                        pixels_.data() + static_cast<std::size_t>(first + row) * row_bytes,
                        row_bytes);
        }
        return {};
    }

    Document document(Format format = Format::png) const {
        MutableImage image =
            take(MutableImage::allocate(descriptor_.canvas_width, descriptor_.canvas_height,
                                        descriptor_.frames.front().layout.planes.front().format),
                 "allocate image fixture");
        std::memcpy(image.pixels().data(), pixels_.data(), pixels_.size());
        Document document;
        document.format = format;
        document.canvas_width = descriptor_.canvas_width;
        document.canvas_height = descriptor_.canvas_height;
        document.color = descriptor_.color;
        Frame frame;
        frame.image = std::move(image).freeze();
        document.frames.push_back(std::move(frame));
        return document;
    }

    void setFormat(Format format) {
        descriptor_.format = format;
    }
    void makeOpaque() {
        if (descriptor_.frames.front().layout.planes.front().format.channel_count() != 4)
            return;
        for (std::size_t offset = 3; offset < pixels_.size(); offset += 4)
            pixels_[offset] = std::byte{255};
    }
    void setPixelFormat(PixelFormat format) {
        RasterFrameDescriptor& frame = descriptor_.frames.front();
        frame.layout.alpha = format.alpha;
        frame.layout.planes.front().format = format;
        frame.layout.planes.front().significant_bits = format.bits_per_channel;
    }
    void resetMetrics() const {
        maximum_rows_ = 0;
        requested_rows_.clear();
        strictly_increasing_ = true;
    }
    std::uint32_t maximumRows() const {
        return maximum_rows_;
    }
    std::size_t readCount() const {
        return requested_rows_.size();
    }
    bool strictlyIncreasing() const {
        return strictly_increasing_;
    }

  private:
    DocumentDescriptor descriptor_;
    std::vector<std::byte> pixels_;
    mutable std::uint32_t maximum_rows_ = 0;
    mutable std::vector<std::uint32_t> requested_rows_;
    mutable bool strictly_increasing_ = true;
};

void verifyResizeParity(std::uint32_t source_width, std::uint32_t source_height,
                        std::uint32_t output_width, std::uint32_t output_height,
                        ResamplingMethod method) {
    RowSource source(source_width, source_height);
    ResizeOptions resize{output_width, output_height, method};
    resize.maximum_threads = 1;
    std::vector<std::byte> raster_pixels(static_cast<std::size_t>(output_width) * output_height *
                                         4U);
    MutablePlaneView destination{output_width, output_height, kRgba8,
                                 static_cast<std::size_t>(output_width) * 4U, raster_pixels};
    Result<void> raster_status = resize_raster_into(source, resize, destination);
    require(raster_status.has_value(), "row-source resize failed");

    TransformOptions transform_options;
    transform_options.resize = resize;
    Document transformed =
        take(transform(source.document(), transform_options), "image-backed resize failed");
    const ImageView expected = transformed.frames.front().image.view();
    require(expected.pixels.size() == raster_pixels.size() &&
                std::equal(expected.pixels.begin(), expected.pixels.end(), raster_pixels.begin()),
            "row-source resize differs from image-backed resize");
}

void resizeParityAndValidation() {
    for (ResamplingMethod method : {ResamplingMethod::linear, ResamplingMethod::lanczos3}) {
        verifyResizeParity(7, 5, 19, 13, method);
        verifyResizeParity(19, 13, 4, 3, method);
        verifyResizeParity(1, 7, 9, 1, method);
        verifyResizeParity(9, 1, 1, 7, method);
    }

    RowSource tall(4, 5000);
    ResizeOptions bounded{2, 3, ResamplingMethod::lanczos3};
    bounded.maximum_threads = 2;
    bounded.maximum_worker_cache_bytes = 1;
    std::vector<std::byte> output(2U * 3U * 4U);
    MutablePlaneView destination{2, 3, kRgba8, 8, output};
    require(resize_raster_into(tall, bounded, destination).has_value(),
            "bounded tall resize failed");
    require(tall.maximumRows() == 1 && tall.readCount() <= 5000 && tall.strictlyIncreasing(),
            "source-major resize did not use one bounded sequential pass");

    std::stop_source stopped;
    stopped.request_stop();
    Result<void> cancelled = resize_raster_into(tall, bounded, destination, stopped.get_token());
    require(!cancelled && cancelled.error().code == ErrorCode::cancelled,
            "raster resize ignored cancellation");

    MutablePlaneView wrong_format{2, 3, kBgra8, 8, output};
    require(!resize_raster_into(tall, bounded, wrong_format),
            "raster resize accepted a mismatched destination format");

    RowSource padded_source(3, 2);
    ResizeOptions padded_options{2, 2, ResamplingMethod::linear};
    constexpr std::size_t padded_stride = 12;
    std::vector<std::byte> padded_output(padded_stride + 8U);
    MutablePlaneView padded_destination{2, 2, kRgba8, padded_stride, padded_output};
    require(resize_raster_into(padded_source, padded_options, padded_destination).has_value(),
            "raster resize rejected a valid destination without trailing row padding");

    DocumentDescriptor invalid = tall.descriptor();
    invalid.frames.push_back(invalid.frames.front());
    class InvalidSource final : public RasterSource {
      public:
        explicit InvalidSource(DocumentDescriptor descriptor)
            : descriptor_(std::move(descriptor)) {}
        const DocumentDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }
        RasterAccess access() const noexcept override {
            return RasterAccess::random_rows;
        }
        Result<void> read_rows(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                               std::size_t, std::span<std::byte>, std::stop_token) const override {
            return Status::error(ErrorCode::internal_error, "unexpected invalid-source read");
        }

      private:
        DocumentDescriptor descriptor_;
    } invalid_source(std::move(invalid));
    require(!resize_raster_into(invalid_source, bounded, destination),
            "raster resize accepted multiple frames");
}

void resizePackedTransferParity() {
    for (const PixelFormat packed : {kRgba8, kBgra8}) {
        RowSource source(256, 17, packed);
        for (const ResamplingMethod method :
             {ResamplingMethod::linear, ResamplingMethod::lanczos3}) {
            for (const auto size : {std::array<std::uint32_t, 2>{128, 9}, {173, 23}, {1, 1}}) {
                for (const bool linear : {false, true}) {
                    ResizeOptions options{size[0], size[1], method};
                    options.linear_rgb = linear;
                    options.maximum_threads = 1;
                    TransformOptions transform_options;
                    transform_options.resize = options;
                    source.setPixelFormat(packed);
                    const auto specialized = take(transform(source.document(), transform_options),
                                                  "resize packed transfer fixture");
                    // Byte order is immaterial for 8-bit samples. A different
                    // descriptor selects the generic scalar transfer-function path.
                    PixelFormat scalar = packed;
                    scalar.little_endian = !packed.little_endian;
                    source.setPixelFormat(scalar);
                    const auto reference = take(transform(source.document(), transform_options),
                                                "resize scalar transfer reference");
                    const auto left = specialized.frames.front().image.view();
                    const auto right = reference.frames.front().image.view();
                    require(left.pixels.size() == right.pixels.size() &&
                                std::equal(left.pixels.begin(), left.pixels.end(),
                                           right.pixels.begin()),
                            "packed resize changed scalar transfer-function rounding");
                }
            }
        }
    }
}

void packedCodecStreamingAndOpacity() {
    Service service;
    for (const Format format :
         {Format::png, Format::jpeg, Format::bmp, Format::webp, Format::jxl, Format::avif}) {
        if (!service.encoder_info(format))
            continue;
        for (const bool opaque : {false, true}) {
            if (!opaque && format == Format::jpeg)
                continue;
            RowSource source(257, 129);
            if (opaque)
                source.makeOpaque();
            source.setFormat(format);
            EncodeOptions options;
            options.format = format;
            options.lossless = true;
            options.quality = 100;
            options.verified_alpha_content =
                opaque ? AlphaContent::opaque : AlphaContent::non_opaque;
            if (format != Format::png)
                require(take(service.raster_encode_route(source.descriptor(), options),
                             "query packed streaming route") == RasterEncodeRoute::native,
                        "packed encoding did not select its native route");
            auto rows = std::make_shared<std::vector<std::byte>>();
            require(service.encode(source, memory_output(rows), options).has_value(),
                    "packed streaming encoder failed");
            require(source.maximumRows() <= 64 && source.strictlyIncreasing(),
                    "packed encoder materialized or reread its source raster");
            auto image = std::make_shared<std::vector<std::byte>>();
            require(
                service.encode(source.document(format), memory_output(image), options).has_value(),
                "packed document reference encoding failed");
            DecodeOptions decode;
            decode.output_format = kRgba8;
            const auto result =
                take(service.decode(memory_input(rows), decode), "decode packed rows");
            const auto expected =
                take(service.decode(memory_input(image), decode), "decode packed reference");
            const auto left = result.frames.front().image.view();
            const auto right = expected.frames.front().image.view();
            require(left.pixels.size() == right.pixels.size() &&
                        std::equal(left.pixels.begin(), left.pixels.end(), right.pixels.begin()),
                    "packed streaming encoder changed document pixels");
            if (opaque &&
                (format == Format::png || format == Format::jxl || format == Format::avif)) {
                const auto info = take(service.inspect(memory_input(rows)), "inspect opaque rows");
                require(!info.frames.front().has_alpha, "opaque row encoding retained alpha");
            }
            source.resetMetrics();
            std::stop_source stopped;
            stopped.request_stop();
            auto cancelled = std::make_shared<std::vector<std::byte>>();
            const auto status =
                service.encode(source, memory_output(cancelled), options, stopped.get_token());
            require(!status && status.error().code == ErrorCode::cancelled &&
                        source.readCount() == 0,
                    "packed row encoding ignored pre-cancellation");
        }
    }
}

void nativeCodecRoutesAndParity() {
    Service service;
    for (Format format : {Format::jxl, Format::avif}) {
        if (service.encoder_info(format) == nullptr)
            continue;
        RowSource source(8, 6);
        source.setFormat(format);
        EncodeOptions options;
        options.format = format;
        options.lossless = true;
        options.quality = 100;
        options.verified_alpha_content = AlphaContent::non_opaque;
        const RasterEncodeRoute route =
            take(service.raster_encode_route(source.descriptor(), options), "query raster route");
        require(route == RasterEncodeRoute::native,
                "compatible raster did not select native route");

        auto raster_bytes = std::make_shared<std::vector<std::byte>>();
        Result<EncodeResult> raster_encoded =
            service.encode(source, memory_output(raster_bytes), options);
        require(raster_encoded.has_value() && !raster_bytes->empty() && source.readCount() > 0,
                "native row-only raster encoding failed");

        auto document_bytes = std::make_shared<std::vector<std::byte>>();
        Result<EncodeResult> document_encoded =
            service.encode(source.document(format), memory_output(document_bytes), options);
        require(document_encoded.has_value(), "document parity encoding failed");
        DecodeOptions decode_options;
        decode_options.output_format = kRgba8;
        const Document raster_decoded = take(
            service.decode(memory_input(raster_bytes), decode_options), "decode raster output");
        const Document document_decoded = take(
            service.decode(memory_input(document_bytes), decode_options), "decode document output");
        const ImageView left = raster_decoded.frames.front().image.view();
        const ImageView right = document_decoded.frames.front().image.view();
        require(left.width == right.width && left.height == right.height &&
                    left.pixels.size() == right.pixels.size() &&
                    std::equal(left.pixels.begin(), left.pixels.end(), right.pixels.begin()),
                "native raster codec output differs from document encoding");

        DocumentDescriptor unsupported = source.descriptor();
        unsupported.frames.front().layout.planes.front().format = kBgra8;
        require(take(service.raster_encode_route(unsupported, options),
                     "query unsupported raster route") == RasterEncodeRoute::materialized,
                "unsupported packed descriptor did not select materialized fallback");

        source.setPixelFormat(kBgra8);
        source.resetMetrics();
        auto fallback_bytes = std::make_shared<std::vector<std::byte>>();
        Result<EncodeResult> fallback =
            service.encode(source, memory_output(fallback_bytes), options);
        require(fallback.has_value() && !fallback_bytes->empty() && source.readCount() > 0,
                "unsupported packed descriptor did not encode through the materialized fallback");
    }
}

void packedBgrAndRgbCodecParity() {
    Service service;
    PixelFormat bgr = kRgb8;
    bgr.channels = ChannelLayout::bgr;
    for (const Format format : {Format::png, Format::jpeg, Format::bmp, Format::webp}) {
        if (!service.encoder_info(format))
            continue;
        for (const PixelFormat pixels : {kBgra8, kRgb8, bgr}) {
            if (format != Format::bmp && pixels != kBgra8)
                continue;
            RowSource source(37, 65, pixels);
            source.makeOpaque();
            source.setFormat(format);
            EncodeOptions options;
            options.format = format;
            options.lossless = format != Format::webp;
            options.verified_alpha_content = AlphaContent::opaque;
            auto encoded_rows = std::make_shared<std::vector<std::byte>>();
            auto encoded_image = std::make_shared<std::vector<std::byte>>();
            require(
                service.encode(source, memory_output(encoded_rows), options).has_value() &&
                    service.encode(source.document(format), memory_output(encoded_image), options)
                        .has_value(),
                "packed BGR or RGB codec parity encoding failed");
            DecodeOptions decode;
            decode.output_format = kRgba8;
            const auto result =
                take(service.decode(memory_input(encoded_rows), decode), "decode BGR rows");
            const auto reference =
                take(service.decode(memory_input(encoded_image), decode), "decode BGR reference");
            const auto left = result.frames.front().image.view();
            const auto right = reference.frames.front().image.view();
            require(left.pixels.size() == right.pixels.size() &&
                        std::equal(left.pixels.begin(), left.pixels.end(), right.pixels.begin()),
                    "packed BGR or RGB row encoding changed document pixels");
        }
    }
}

void mappedOnlyCodecParity() {
    class MappedSource final : public RasterSource {
      public:
        explicit MappedSource(const RowSource& rows)
            : descriptor_(rows.descriptor()),
              document_(std::make_shared<Document>(rows.document())) {}
        const DocumentDescriptor& descriptor() const noexcept override {
            return descriptor_;
        }
        RasterAccess access() const noexcept override {
            return RasterAccess::mapped_planes;
        }
        Result<void> read_rows(std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
                               std::size_t, std::span<std::byte>, std::stop_token) const override {
            return Status::error(ErrorCode::internal_error, "mapped source must not copy rows");
        }
        Result<MappedPlane> map_plane(std::uint32_t, std::uint32_t) const override {
            const auto& image = document_->frames.front().image;
            return MappedPlane{document_, image.pixels(), image.row_stride()};
        }

      private:
        DocumentDescriptor descriptor_;
        std::shared_ptr<const Document> document_;
    };
    Service service;
    for (const Format format : {Format::bmp, Format::webp}) {
        if (!service.encoder_info(format))
            continue;
        RowSource rows(37, 65);
        rows.setFormat(format);
        MappedSource mapped(rows);
        EncodeOptions options;
        options.format = format;
        options.lossless = true;
        auto mapped_bytes = std::make_shared<std::vector<std::byte>>();
        auto row_bytes = std::make_shared<std::vector<std::byte>>();
        require(service.encode(mapped, memory_output(mapped_bytes), options).has_value() &&
                    service.encode(rows, memory_output(row_bytes), options).has_value(),
                "mapped-only native packed encoding failed");
        require(*mapped_bytes == *row_bytes, "mapped-only packed encoding changed encoded bytes");
    }
}
} // namespace

int main() {
    try {
        resizeParityAndValidation();
        resizePackedTransferParity();
        packedCodecStreamingAndOpacity();
        packedBgrAndRgbCodecParity();
        mappedOnlyCodecParity();
        nativeCodecRoutesAndParity();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
