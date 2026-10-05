#include "snow/image/processing.h"
#include "snow/image/resource_estimate.h"
#include "snow/image/resource_plan.h"
#include "snow/image/service.h"

#include <libheif/heif.h>
#include <libheif/heif_sequences.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

template <typename T> T take(Result<T> result, std::string_view operation) {
    if (!result)
        throw std::runtime_error(std::string(operation) + ": " + result.error().message);
    return std::move(result).value();
}

void take(Result<void> result, std::string_view operation) {
    if (!result)
        throw std::runtime_error(std::string(operation) + ": " + result.error().message);
}

Document fixture(PixelFormat format, bool alpha, bool inverted = false) {
    constexpr std::uint32_t width = 65;
    constexpr std::uint32_t height = 17;
    MutableImage image =
        take(MutableImage::allocate(width, height, format, 64), "allocate fixture");
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool gray = format.channels == ChannelLayout::gray ||
                              format.channels == ChannelLayout::gray_alpha;
            const bool bgr =
                format.channels == ChannelLayout::bgr || format.channels == ChannelLayout::bgra;
            const auto red = static_cast<std::uint8_t>((x % 2U == 0) != inverted ? 255 : 0);
            std::array<std::uint8_t, 4> pixel{
                red, static_cast<std::uint8_t>(gray ? red : y * 15U),
                static_cast<std::uint8_t>(255U - red),
                static_cast<std::uint8_t>(alpha ? (y % 4U) * 85U : 255U)};
            if (bgr)
                std::swap(pixel[0], pixel[2]);
            if (format.channels == ChannelLayout::gray_alpha)
                pixel[1] = pixel[3];
            const std::size_t sample_bytes = format.bits_per_channel / 8U;
            const std::size_t offset =
                static_cast<std::size_t>(y) * image.row_stride() +
                static_cast<std::size_t>(x) * format.channel_count() * sample_bytes;
            for (std::uint32_t channel = 0; channel < format.channel_count(); ++channel) {
                if (sample_bytes == 1) {
                    image.pixels()[offset + channel] = static_cast<std::byte>(pixel[channel]);
                } else {
                    const std::uint16_t value = static_cast<std::uint16_t>(
                        pixel[channel] * 257U +
                        (channel < 3 && pixel[channel] > 0 && pixel[channel] < 255 ? 31U : 0U));
                    image.pixels()[offset + channel * sample_bytes] =
                        static_cast<std::byte>(format.little_endian ? value & 0xffU : value >> 8U);
                    image.pixels()[offset + channel * sample_bytes + 1] =
                        static_cast<std::byte>(format.little_endian ? value >> 8U : value & 0xffU);
                }
            }
        }
    }
    Document document;
    document.format = Format::avif;
    document.canvas_width = width;
    document.canvas_height = height;
    document.color.primaries = ColorPrimaries::display_p3;
    document.color.transfer = TransferFunction::srgb;
    Frame frame;
    frame.image = std::move(image).freeze();
    document.frames.push_back(std::move(frame));
    return document;
}

class RowSource final : public RasterSource {
  public:
    explicit RowSource(Document document, std::stop_source* cancel_after_row = nullptr)
        : document_(std::move(document)),
          descriptor_(take(describe_document(document_), "describe row source")),
          cancel_after_row_(cancel_after_row) {}

    const DocumentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    RasterAccess access() const noexcept override {
        return RasterAccess::sequential_rows;
    }
    Result<void> read_rows(std::uint32_t frame, std::uint32_t plane, std::uint32_t first,
                           std::uint32_t count, std::size_t stride,
                           std::span<std::byte> destination, std::stop_token stop) const override {
        if (stop.stop_requested())
            return Status::error(ErrorCode::cancelled, "cancelled row source");
        require(frame == 0 && plane == 0 && count == 1 && first == rows_read_,
                "AVIF must read the source once, one row at a time");
        const Image& image = document_.frames.front().image;
        const std::size_t row_bytes = static_cast<std::size_t>(image.width()) * 4U;
        require(stride >= row_bytes && destination.size() >= row_bytes, "invalid row destination");
        std::memcpy(destination.data(), image.pixels().data() + first * image.row_stride(),
                    row_bytes);
        ++rows_read_;
        if (cancel_after_row_)
            cancel_after_row_->request_stop();
        return {};
    }
    std::uint32_t rows_read() const {
        return rows_read_;
    }

  private:
    Document document_;
    DocumentDescriptor descriptor_;
    mutable std::uint32_t rows_read_ = 0;
    std::stop_source* cancel_after_row_ = nullptr;
};

class CollectingSink final : public PixelSink {
  public:
    explicit CollectingSink(bool frame_storage) : frame_storage_(frame_storage) {}
    Result<void> begin(const DocumentInfo& info) override {
        document.canvas_width = info.canvas_width;
        document.canvas_height = info.canvas_height;
        document.color = info.color;
        return {};
    }
    Result<void> begin_frame(std::uint32_t, const FrameInfo& info) override {
        image_ = take(MutableImage::allocate(info.width, info.height, info.native_format),
                      "allocate sink image");
        frame_ = {};
        frame_.color = info.color;
        frame_.duration = info.duration;
        return {};
    }
    std::span<std::byte> frame_storage(std::uint32_t, std::size_t stride,
                                       std::size_t byte_size) override {
        if (!frame_storage_)
            return {};
        require(stride == image_.row_stride() && byte_size == image_.pixels().size(),
                "sink frame storage geometry");
        return image_.pixels();
    }
    Result<void> write_rows(std::uint32_t first, std::uint32_t count, std::size_t stride,
                            std::span<const std::byte> pixels) override {
        require(first + count <= image_.height() && stride >= image_.row_stride() &&
                    pixels.size() >= (count - 1U) * stride + image_.row_stride(),
                "sink row geometry");
        for (std::uint32_t row = 0; row < count; ++row) {
            std::memcpy(image_.pixels().data() + (first + row) * image_.row_stride(),
                        pixels.data() + row * stride, image_.row_stride());
        }
        return {};
    }
    Result<void> end_frame(std::uint32_t) override {
        frame_.image = std::move(image_).freeze();
        document.frames.push_back(std::move(frame_));
        return {};
    }
    Result<void> end() override {
        return {};
    }
    Document document;

  private:
    bool frame_storage_;
    MutableImage image_;
    Frame frame_;
};

using Bytes = std::shared_ptr<std::vector<std::byte>>;

void verify_configuration(const Bytes& bytes, int depth, bool alpha) {
    const auto context = std::unique_ptr<heif_context, decltype(&heif_context_free)>(
        heif_context_alloc(), heif_context_free);
    require(context != nullptr, "allocate inspection context");
    require(heif_context_read_from_memory_without_copy(context.get(), bytes->data(), bytes->size(),
                                                       nullptr)
                    .code == heif_error_Ok,
            "parse encoded AVIF");
    heif_image_handle* raw = nullptr;
    require(heif_context_get_primary_image_handle(context.get(), &raw).code == heif_error_Ok,
            "inspect encoded AVIF handle");
    const auto handle = std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)>(
        raw, heif_image_handle_release);
    require(heif_image_handle_get_luma_bits_per_pixel(handle.get()) == depth,
            "AVIF must encode the selected significant depth");
    require((heif_image_handle_has_alpha_channel(handle.get()) != 0) == alpha,
            "AVIF must preserve alpha classification");
    heif_colorspace colorspace = heif_colorspace_undefined;
    heif_chroma chroma = heif_chroma_undefined;
    require(heif_image_handle_get_preferred_decoding_colorspace(handle.get(), &colorspace, &chroma)
                        .code == heif_error_Ok &&
                chroma == heif_chroma_444,
            "AVIF must retain full chroma resolution");
}

Document decode_rgba8(Service& service, const Bytes& bytes) {
    DecodeOptions options;
    options.output_format = kRgba8;
    return take(service.decode(memory_input(bytes), options), "decode AVIF as RGBA8");
}

template <typename T> void verify_dynamic_range(const T& document, DynamicRange expected) {
    require(document.color.dynamic_range == expected,
            "AVIF sample precision must not change the source SDR/HDR classification: expected=" +
                std::to_string(static_cast<int>(expected)) +
                ", document=" + std::to_string(static_cast<int>(document.color.dynamic_range)) +
                ", transfer=" + std::to_string(static_cast<int>(document.color.transfer)));
    for (const auto& frame : document.frames) {
        // An empty frame profile inherits the document profile, as in effective_color().
        const ColorEncoding& color = frame.color.icc_profile.empty() &&
                                             frame.color.primaries == ColorPrimaries::unknown &&
                                             frame.color.transfer == TransferFunction::unknown
                                         ? document.color
                                         : frame.color;
        require(color.dynamic_range == expected,
                "AVIF frames must retain the source SDR/HDR classification: expected=" +
                    std::to_string(static_cast<int>(expected)) +
                    ", frame=" + std::to_string(static_cast<int>(color.dynamic_range)) +
                    ", transfer=" + std::to_string(static_cast<int>(color.transfer)));
    }
}

void verify_rgba16(const Document& rgba8, const Document& rgba16) {
    require(rgba8.frames.size() == rgba16.frames.size(), "RGBA16 decode must retain every frame");
    verify_dynamic_range(rgba16, rgba8.color.dynamic_range);
    for (std::size_t frame = 0; frame < rgba8.frames.size(); ++frame) {
        const ImageView a = rgba8.frames[frame].image.view();
        const ImageView b = rgba16.frames[frame].image.view();
        require(b.format == kRgba16, "High-depth AVIF must decode to RGBA16 by default");
        for (std::uint32_t y = 0; y < a.height; ++y) {
            for (std::size_t x = 0; x < static_cast<std::size_t>(a.width) * 4U; ++x) {
                const std::size_t offset = y * b.row_stride + x * 2U;
                const int sample = static_cast<int>(b.pixels[offset]) +
                                   static_cast<int>(b.pixels[offset + 1]) * 256;
                require(std::abs(static_cast<int>(a.pixels[y * a.row_stride + x]) -
                                 (sample + 128) / 257) <= 1,
                        "RGBA16 decode must normalize significant bits to the full 16-bit range");
            }
        }
    }
}

void verify_fidelity(const Document& source, const Document& decoded, int tolerance) {
    require(decoded.canvas_width == source.canvas_width &&
                decoded.canvas_height == source.canvas_height,
            "AVIF must retain odd image dimensions");
    require(decoded.frames.size() == source.frames.size(), "AVIF must retain every frame");
    verify_dynamic_range(decoded, source.color.dynamic_range);
    const ColorEncoding& decoded_color = decoded.color.primaries == ColorPrimaries::unknown
                                             ? decoded.frames.front().color
                                             : decoded.color;
    require(decoded_color.primaries == source.color.primaries &&
                decoded_color.transfer == source.color.transfer,
            "AVIF must retain the source color profile: frames=" +
                std::to_string(source.frames.size()) +
                ", primaries=" + std::to_string(static_cast<int>(decoded_color.primaries)) +
                ", transfer=" + std::to_string(static_cast<int>(decoded_color.transfer)));
    for (std::size_t frame_index = 0; frame_index < source.frames.size(); ++frame_index) {
        const ImageView a = source.frames[frame_index].image.view();
        const ImageView b = decoded.frames[frame_index].image.view();
        const bool gray = a.format.channels == ChannelLayout::gray ||
                          a.format.channels == ChannelLayout::gray_alpha;
        const bool bgr =
            a.format.channels == ChannelLayout::bgr || a.format.channels == ChannelLayout::bgra;
        const std::size_t sample_bytes = a.format.bits_per_channel / 8U;
        for (std::uint32_t y = 0; y < a.height; ++y) {
            for (std::size_t x = 0; x < static_cast<std::size_t>(a.width) * 4U; ++x) {
                const std::size_t channel = x % 4U;
                int left = 255;
                if (channel != 3 || a.format.alpha != AlphaMode::none) {
                    const std::size_t source_channel = gray ? (channel == 3 ? 1U : 0U)
                                                       : bgr && channel != 3 ? 2U - channel
                                                                             : channel;
                    const std::size_t offset =
                        y * a.row_stride +
                        (x / 4U * a.format.channel_count() + source_channel) * sample_bytes;
                    left = static_cast<int>(a.pixels[offset]);
                    if (sample_bytes == 2) {
                        const int other = static_cast<int>(a.pixels[offset + 1]);
                        left = ((a.format.little_endian ? left + other * 256 : left * 256 + other) +
                                128) /
                               257;
                    }
                }
                const int right = static_cast<int>(b.pixels[y * b.row_stride + x]);
                require(std::abs(left - right) <= tolerance,
                        "AVIF must preserve sharp color edges and alpha");
            }
        }
    }
}

void lossy_encoding(Service& service) {
    PixelFormat big_endian = kRgba16;
    big_endian.little_endian = false;
    for (PixelFormat format :
         {kGray8, kGrayAlpha8, kRgb8, kRgba8, kBgra8,
          PixelFormat{SampleType::unsigned_integer, ChannelLayout::bgr, AlphaMode::none, 8, true},
          kRgba16, big_endian}) {
        for (bool alpha : {false, true}) {
            Document document = fixture(format, alpha);
            const bool has_alpha = alpha && format.alpha != AlphaMode::none;
            EncodeOptions options;
            options.format = Format::avif;
            options.quality = 95;
            options.effort = 1;
            auto bytes = std::make_shared<std::vector<std::byte>>();
            take(service.encode(document, memory_output(bytes), options), "encode lossy AVIF");
            verify_configuration(bytes, format.bits_per_channel == 16 ? 12 : 10, has_alpha);
            const Document decoded = decode_rgba8(service, bytes);
            verify_fidelity(document, decoded, 12);
            verify_rgba16(decoded, take(service.decode(memory_input(bytes)), "decode AVIF"));
            if (format != kRgba8)
                continue;
            for (bool frame_storage : {false, true}) {
                CollectingSink sink(frame_storage);
                take(service.decode_to_sink(memory_input(bytes), sink), "decode AVIF to sink");
                verify_rgba16(decoded, sink.document);
            }
            RowSource source(document);
            options.verified_alpha_content =
                has_alpha ? AlphaContent::non_opaque : AlphaContent::opaque;
            require(take(service.raster_encode_route(source.descriptor(), options),
                         "query route") == RasterEncodeRoute::native,
                    "RGBA8 must retain the native raster route");
            auto raster_bytes = std::make_shared<std::vector<std::byte>>();
            take(service.encode(source, memory_output(raster_bytes), options), "encode row AVIF");
            require(source.rows_read() == document.canvas_height, "read every source row once");
            verify_configuration(raster_bytes, 10, has_alpha);
            verify_fidelity(decoded, decode_rgba8(service, raster_bytes), 0);
        }
    }
}

void lossless_encoding(Service& service) {
    for (bool alpha : {false, true}) {
        Document document = fixture(kRgba8, alpha);
        EncodeOptions options;
        options.format = Format::avif;
        options.lossless = true;
        options.quality = 100;
        options.effort = 1;
        options.verified_alpha_content = alpha ? AlphaContent::non_opaque : AlphaContent::opaque;
        auto bytes = std::make_shared<std::vector<std::byte>>();
        take(service.encode(document, memory_output(bytes), options), "encode lossless AVIF");
        verify_configuration(bytes, 8, alpha);
        verify_fidelity(document, decode_rgba8(service, bytes), 0);
        RowSource source(document);
        bytes = std::make_shared<std::vector<std::byte>>();
        take(service.encode(source, memory_output(bytes), options), "encode lossless row AVIF");
        verify_configuration(bytes, 8, alpha);
        verify_fidelity(document, decode_rgba8(service, bytes), 0);
    }
}

void raster_cancellation(Service& service) {
    std::stop_source stop;
    RowSource source(fixture(kRgba8, true), &stop);
    EncodeOptions options;
    options.format = Format::avif;
    options.verified_alpha_content = AlphaContent::non_opaque;
    auto bytes = std::make_shared<std::vector<std::byte>>();
    const auto result = service.encode(source, memory_output(bytes), options, stop.get_token());
    require(!result && result.error().code == ErrorCode::cancelled && source.rows_read() == 1 &&
                bytes->empty(),
            "AVIF raster conversion must stop between rows without writing an artifact");
}

void verify_sequence_configuration(const Bytes& bytes, int depth, bool alpha) {
    verify_configuration(bytes, depth, alpha);
    const auto context = std::unique_ptr<heif_context, decltype(&heif_context_free)>(
        heif_context_alloc(), heif_context_free);
    require(context != nullptr, "allocate sequence inspection context");
    require(heif_context_read_from_memory_without_copy(context.get(), bytes->data(), bytes->size(),
                                                       nullptr)
                    .code == heif_error_Ok,
            "parse AVIF sequence");
    heif_track* raw_track = heif_context_get_track(context.get(), 0);
    require(raw_track != nullptr, "read AVIF sequence track");
    const auto track =
        std::unique_ptr<heif_track, decltype(&heif_track_release)>(raw_track, heif_track_release);
    const auto decoding =
        std::unique_ptr<heif_decoding_options, decltype(&heif_decoding_options_free)>(
            heif_decoding_options_alloc(), heif_decoding_options_free);
    const auto inherit_profile =
        std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)>(
            heif_nclx_color_profile_alloc(), heif_nclx_color_profile_free);
    require(decoding && inherit_profile, "allocate native sequence decoding options");
    inherit_profile->color_primaries = heif_color_primaries_unspecified;
    inherit_profile->transfer_characteristics = heif_transfer_characteristic_unspecified;
    inherit_profile->matrix_coefficients = heif_matrix_coefficients_unspecified;
    inherit_profile->full_range_flag = 1;
    // Unspecified output CICP inherits the bitstream rather than converting to libheif's default
    // sRGB.
    decoding->output_image_nclx_profile = inherit_profile.get();
    heif_image* raw_image = nullptr;
    require(heif_track_decode_next_image(track.get(), &raw_image, heif_colorspace_undefined,
                                         heif_chroma_undefined, decoding.get())
                    .code == heif_error_Ok,
            "decode native AVIF sequence sample");
    const auto image =
        std::unique_ptr<heif_image, decltype(&heif_image_release)>(raw_image, heif_image_release);
    heif_color_profile_nclx* raw_profile = nullptr;
    const auto profile_error = heif_image_get_nclx_color_profile(image.get(), &raw_profile);
    const auto profile =
        std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)>(
            raw_profile, heif_nclx_color_profile_free);
    require(profile_error.code == heif_error_Ok &&
                profile->color_primaries == heif_color_primaries_SMPTE_EG_432_1 &&
                profile->transfer_characteristics == heif_transfer_characteristic_IEC_61966_2_1,
            "AVIF sequence must encode the source color profile: error=" +
                std::to_string(profile_error.code) +
                ", primaries=" + std::to_string(profile ? profile->color_primaries : -1) +
                ", transfer=" + std::to_string(profile ? profile->transfer_characteristics : -1));
    require(heif_image_get_chroma_format(image.get()) == heif_chroma_444 &&
                heif_image_get_bits_per_pixel_range(image.get(), heif_channel_Y) == depth &&
                (heif_image_has_channel(image.get(), heif_channel_Alpha) != 0) == alpha,
            "AVIF sequence samples must use the same depth, chroma and alpha as still images");
}

void sequence_encoding(Service& service) {
    for (bool lossless : {false, true}) {
        for (bool alpha : {false, true}) {
            Document document = fixture(kRgba8, alpha);
            document.frames.front().duration = std::chrono::milliseconds(40);
            document.frames.push_back(fixture(kRgba8, alpha, true).frames.front());
            document.frames.back().duration = std::chrono::milliseconds(80);
            EncodeOptions options;
            options.format = Format::avif;
            options.lossless = lossless;
            options.quality = lossless ? 100 : 95;
            options.effort = 1;
            auto bytes = std::make_shared<std::vector<std::byte>>();
            take(service.encode(document, memory_output(bytes), options), "encode AVIF sequence");
            verify_sequence_configuration(bytes, lossless ? 8 : 10, alpha);
            const Document decoded = decode_rgba8(service, bytes);
            verify_fidelity(document, decoded, lossless ? 0 : 12);
            for (std::size_t frame = 0; frame < document.frames.size(); ++frame) {
                require(decoded.frames[frame].duration == document.frames[frame].duration,
                        "AVIF must preserve every sequence duration");
            }
            DecodeOptions decoding;
            decoding.output_format = kRgba16;
            verify_rgba16(decoded, take(service.decode(memory_input(bytes), decoding),
                                        "decode AVIF sequence as RGBA16"));
            for (bool frame_storage : {false, true}) {
                CollectingSink sink(frame_storage);
                take(service.decode_to_sink(memory_input(bytes), sink, decoding),
                     "decode AVIF sequence to sink");
                verify_rgba16(decoded, sink.document);
            }
        }
    }
}

void color_range_round_trip(Service& service) {
    for (PixelFormat format : {kRgba8, kRgba16}) {
        for (TransferFunction transfer :
             {TransferFunction::srgb, TransferFunction::pq, TransferFunction::hlg}) {
            const DynamicRange range =
                transfer == TransferFunction::srgb ? DynamicRange::standard : DynamicRange::high;
            for (bool sequence : {false, true}) {
                Document document = fixture(format, false);
                document.color.primaries = transfer == TransferFunction::srgb
                                               ? ColorPrimaries::srgb
                                               : ColorPrimaries::rec2020;
                document.color.transfer = transfer;
                document.color.dynamic_range = range;
                if (sequence) {
                    document.frames.front().duration = std::chrono::milliseconds(40);
                    document.frames.push_back(fixture(format, false, true).frames.front());
                    document.frames.back().duration = std::chrono::milliseconds(80);
                }
                EncodeOptions options;
                options.format = Format::avif;
                options.quality = 95;
                options.effort = 1;
                auto bytes = std::make_shared<std::vector<std::byte>>();
                take(service.encode(document, memory_output(bytes), options),
                     "encode AVIF color range");
                verify_configuration(bytes, format.bits_per_channel == 16 ? 12 : 10, false);
                verify_dynamic_range(
                    take(service.inspect(memory_input(bytes)), "inspect AVIF color range"), range);
                DecodeOptions decoding;
                decoding.raster_layout = RasterLayoutPolicy::native;
                verify_dynamic_range(take(service.inspect_raster(memory_input(bytes), decoding),
                                          "inspect AVIF raster color range"),
                                     range);
                const Document decoded =
                    take(service.decode(memory_input(bytes)), "decode AVIF color range");
                verify_dynamic_range(decoded, range);
                require(decoded.color.primaries == document.color.primaries &&
                            decoded.color.transfer == transfer,
                        "AVIF must preserve SDR and HDR color profiles");
                for (bool frame_storage : {false, true}) {
                    CollectingSink sink(frame_storage);
                    take(service.decode_to_sink(memory_input(bytes), sink),
                         "decode AVIF color range to sink");
                    verify_dynamic_range(sink.document, range);
                }
                if (range == DynamicRange::standard) {
                    const Document exported =
                        take(convert_to_sdr_srgb(decoded), "prepare SDR export");
                    const Document without_tone_mapping =
                        take(convert_to_sdr_srgb(decoded, false), "prepare untone-mapped export");
                    for (std::size_t frame = 0; frame < decoded.frames.size(); ++frame) {
                        const auto a = exported.frames[frame].image.pixels();
                        const auto b = without_tone_mapping.frames[frame].image.pixels();
                        require(std::equal(a.begin(), a.end(), b.begin(), b.end()),
                                "SDR AVIF re-export must not apply HDR tone mapping");
                    }
                }
            }
        }
    }
}

void resource_plan() {
    Document document = fixture(kRgba8, false);
    ResourcePlanRequest request;
    request.source = take(describe_document(document), "describe resource source");
    request.output = request.source;
    for (Format format : {Format::avif, Format::heif}) {
        for (RasterEncodeRoute route :
             {RasterEncodeRoute::native, RasterEncodeRoute::materialized}) {
            request.encode_options.format = format;
            request.raster_route = route;
            const ResourcePlan plan = take(plan_resources(request), "plan AVIF resources");
            const ResourceEstimate estimate =
                take(estimate_encode_resources(document, request.encode_options),
                     "estimate AVIF resources");
            const auto encode =
                std::find_if(plan.phases.begin(), plan.phases.end(), [](const auto& phase) {
                    return phase.phase == ResourcePhase::encode;
                });
            require(encode != plan.phases.end() &&
                        encode->footprint.private_memory_bytes >= estimate.private_memory_bytes,
                    "AVIF phase plan must admit the encoder's full-frame working set");
        }
    }
}
} // namespace

int main() {
    try {
        Service service;
        if (service.encoder_info(Format::avif) == nullptr)
            return 0;
        lossy_encoding(service);
        lossless_encoding(service);
        raster_cancellation(service);
        sequence_encoding(service);
        color_range_round_trip(service);
        resource_plan();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
