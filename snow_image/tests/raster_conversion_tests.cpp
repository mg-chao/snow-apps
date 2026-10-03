#include "snow/image/raster_conversion.h"

#if defined(SNOW_IMAGE_HAS_LIBYUV)
#include <libyuv/convert_argb.h>
#endif
#if defined(SNOW_IMAGE_HAS_JPEG)
#include <turbojpeg.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace {

using namespace snow::image;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::pair<std::uint32_t, std::uint32_t> factors(ChromaSubsampling sampling) {
    switch (sampling) {
    case ChromaSubsampling::yuv444:
        return {1, 1};
    case ChromaSubsampling::yuv422:
        return {2, 1};
    case ChromaSubsampling::yuv420:
        return {2, 2};
    case ChromaSubsampling::yuv440:
        return {1, 2};
    case ChromaSubsampling::yuv411:
        return {4, 1};
    case ChromaSubsampling::yuv441:
        return {1, 4};
    case ChromaSubsampling::none:
        return {1, 1};
    }
    std::abort();
}

std::uint32_t aligned(std::uint32_t value, std::uint32_t factor) {
    return (value + factor - 1U) / factor * factor;
}

std::byte sample(std::uint32_t plane, std::uint32_t x, std::uint32_t y) {
    const std::uint32_t mixed = x * 1664525U + y * 1013904223U + plane * 4051U;
    return static_cast<std::byte>((mixed >> 11U) & 255U);
}

// Each plane accepts strictly forward reads, independently of the other planes.
// The fixture retains no full-sized source and records every staging allocation
// that the conversion exposes through its destination views.
class GeneratedRaster final : public RasterSource {
  public:
    GeneratedRaster(std::uint32_t width, std::uint32_t height,
                    ChromaSubsampling sampling = ChromaSubsampling::none,
                    ColorRange range = ColorRange::full, bool padded = false, bool alpha = false) {
        descriptor_.canvas_width = width;
        descriptor_.canvas_height = height;
        RasterFrameDescriptor frame;
        frame.width = width;
        frame.height = height;
        frame.layout.alpha = alpha ? AlphaMode::straight : AlphaMode::none;
        frame.layout.color_range = range;
        frame.layout.chroma_subsampling = sampling;
        if (sampling == ChromaSubsampling::none) {
            frame.layout.color_model = ColorModel::gray;
            frame.layout.planes = {{PlaneSemantic::gray, width, height, kGray8, 8}};
        } else {
            const auto [horizontal, vertical] = factors(sampling);
            frame.layout.color_model = ColorModel::ycbcr;
            frame.layout.planes = {
                {PlaneSemantic::luma, padded ? aligned(width, horizontal) : width,
                 padded ? aligned(height, vertical) : height, kGray8, 8},
                {PlaneSemantic::chroma_blue, (width + horizontal - 1U) / horizontal,
                 (height + vertical - 1U) / vertical, kGray8, 8},
                {PlaneSemantic::chroma_red, (width + horizontal - 1U) / horizontal,
                 (height + vertical - 1U) / vertical, kGray8, 8}};
            if (alpha)
                frame.layout.planes.push_back({PlaneSemantic::alpha, width, height, kGray8, 8});
        }
        descriptor_.frames.push_back(std::move(frame));
        require(descriptor_.validate().has_value(), "generated raster descriptor is valid");
    }

    const DocumentDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }
    RasterAccess access() const noexcept override {
        return RasterAccess::sequential_rows;
    }
    Result<void> read_rows(std::uint32_t frame, std::uint32_t plane, std::uint32_t first,
                           std::uint32_t count, std::size_t stride, std::span<std::byte> pixels,
                           std::stop_token stop) const override {
        const auto& layout = descriptor_.frames.at(frame).layout.planes.at(plane);
        return read_region(frame, plane, {0, first, layout.width, count},
                           {layout.width, count, layout.format, stride, pixels}, stop);
    }
    Result<void> read_region(std::uint32_t frame, std::uint32_t plane, RasterRect region,
                             MutablePlaneView destination, std::stop_token stop) const override {
        ++calls;
        if (stop.stop_requested())
            return Status::error(ErrorCode::cancelled, "fixture read cancelled", "fixture");
        if (fail_on_call == calls)
            return Status::error(ErrorCode::io_error, "fixture read failed", "fixture", 47);
        const auto& layout = descriptor_.frames.at(frame).layout.planes.at(plane);
        require(region.width != 0 && region.height != 0 && region.x <= layout.width &&
                    region.width <= layout.width - region.x && region.y <= layout.height &&
                    region.height <= layout.height - region.y,
                "conversion never requests source padding outside tight planes");
        require(!next_row_[plane] || *next_row_[plane] == region.y,
                "conversion reads every source row once in forward order");
        next_row_[plane] = region.y + region.height;
        require(destination.validate().has_value(), "conversion supplies valid staging views");
        maximum_plane_bytes = std::max(maximum_plane_bytes, destination.pixels.size());
        maximum_read_rows = std::max(maximum_read_rows, region.height);
        for (std::uint32_t y = 0; y < region.height; ++y) {
            auto* row =
                destination.pixels.data() + static_cast<std::size_t>(y) * destination.row_stride;
            for (std::uint32_t x = 0; x < region.width; ++x)
                row[x] = sample(plane, region.x + x, region.y + y);
        }
        if (cancel_after_call == calls && cancellation)
            cancellation->request_stop();
        return {};
    }

    mutable std::size_t calls = 0;
    mutable std::size_t maximum_plane_bytes = 0;
    mutable std::uint32_t maximum_read_rows = 0;
    std::size_t fail_on_call = 0;
    std::size_t cancel_after_call = 0;
    std::stop_source* cancellation = nullptr;

  private:
    DocumentDescriptor descriptor_;
    mutable std::array<std::optional<std::uint32_t>, 4> next_row_;
};

#if defined(SNOW_IMAGE_HAS_JPEG)
int jpeg_sampling(ChromaSubsampling sampling) {
    switch (sampling) {
    case ChromaSubsampling::yuv444:
        return TJSAMP_444;
    case ChromaSubsampling::yuv422:
        return TJSAMP_422;
    case ChromaSubsampling::yuv420:
        return TJSAMP_420;
    case ChromaSubsampling::yuv440:
        return TJSAMP_440;
    case ChromaSubsampling::yuv411:
        return TJSAMP_411;
    case ChromaSubsampling::yuv441:
        return TJSAMP_441;
    case ChromaSubsampling::none:
        return TJSAMP_UNKNOWN;
    }
    std::abort();
}
#endif

// Convert the complete padded frame with the actual backend, independently of
// production's strip scheduling, and compare every pixel of every requested crop.
std::vector<std::byte> reference(const GeneratedRaster& source, AlphaMode alpha_mode) {
    const auto& frame = source.descriptor().frames.front();
    const auto& layout = frame.layout;
    const auto [horizontal, vertical] = factors(layout.chroma_subsampling);
    const std::uint32_t width = aligned(frame.width, horizontal);
    const std::uint32_t height = aligned(frame.height, vertical);
    std::array<std::vector<std::byte>, 3> planes;
    for (std::uint32_t plane = 0; plane < 3; ++plane) {
        const std::uint32_t plane_width = plane == 0 ? width : width / horizontal;
        const std::uint32_t plane_height = plane == 0 ? height : height / vertical;
        planes[plane].resize(static_cast<std::size_t>(plane_width) * plane_height);
        for (std::uint32_t y = 0; y < plane_height; ++y)
            for (std::uint32_t x = 0; x < plane_width; ++x)
                planes[plane][static_cast<std::size_t>(y) * plane_width + x] =
                    sample(plane, std::min(x, layout.planes[plane].width - 1U),
                           std::min(y, layout.planes[plane].height - 1U));
    }
    std::vector<std::byte> output(static_cast<std::size_t>(width) * height * 4U);
    bool converted = false;
#if defined(SNOW_IMAGE_HAS_JPEG)
    if (layout.color_range == ColorRange::full) {
        void* decoder = tj3Init(TJINIT_DECOMPRESS);
        require(decoder != nullptr, "reference TurboJPEG decoder allocates");
        require(tj3Set(decoder, TJPARAM_SUBSAMP, jpeg_sampling(layout.chroma_subsampling)) == 0,
                "reference TurboJPEG sampling is accepted");
        const std::array<const unsigned char*, 3> pointers{
            reinterpret_cast<const unsigned char*>(planes[0].data()),
            reinterpret_cast<const unsigned char*>(planes[1].data()),
            reinterpret_cast<const unsigned char*>(planes[2].data())};
        const std::array<int, 3> strides{static_cast<int>(width),
                                         static_cast<int>(width / horizontal),
                                         static_cast<int>(width / horizontal)};
        require(tj3DecodeYUVPlanes8(decoder, pointers.data(), strides.data(),
                                    reinterpret_cast<unsigned char*>(output.data()),
                                    static_cast<int>(width), static_cast<int>(width * 4U),
                                    static_cast<int>(height), TJPF_RGBA) == 0,
                "full-frame reference TurboJPEG conversion succeeds");
        tj3Destroy(decoder);
        converted = true;
    }
#endif
#if defined(SNOW_IMAGE_HAS_LIBYUV)
    if (!converted) {
        // Expansion is also the established production fallback for less common
        // limited-range subsamplings. 444 avoids any reference crop/alignment logic.
        std::vector<std::uint8_t> cb(static_cast<std::size_t>(width) * height);
        std::vector<std::uint8_t> cr(cb.size());
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t chroma =
                    static_cast<std::size_t>(y / vertical) * (width / horizontal) + x / horizontal;
                const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
                cb[pixel] = std::to_integer<std::uint8_t>(planes[1][chroma]);
                cr[pixel] = std::to_integer<std::uint8_t>(planes[2][chroma]);
            }
        }
        const auto* y = reinterpret_cast<const std::uint8_t*>(planes[0].data());
        auto* rgba = reinterpret_cast<std::uint8_t*>(output.data());
        const int result =
            layout.color_range == ColorRange::full
                ? libyuv::J444ToABGR(y, static_cast<int>(width), cb.data(), static_cast<int>(width),
                                     cr.data(), static_cast<int>(width), rgba,
                                     static_cast<int>(width * 4U), static_cast<int>(width),
                                     static_cast<int>(height))
                : libyuv::I444ToABGR(y, static_cast<int>(width), cb.data(), static_cast<int>(width),
                                     cr.data(), static_cast<int>(width), rgba,
                                     static_cast<int>(width * 4U), static_cast<int>(width),
                                     static_cast<int>(height));
        require(result == 0, "full-frame reference libyuv conversion succeeds");
        converted = true;
    }
#endif
    if (!converted) {
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
                const std::size_t chroma =
                    static_cast<std::size_t>(y / vertical) * (width / horizontal) + x / horizontal;
                const int luma = std::to_integer<int>(planes[0][pixel]);
                const int cb = std::to_integer<int>(planes[1][chroma]) - 128;
                const int cr = std::to_integer<int>(planes[2][chroma]) - 128;
                const bool full = layout.color_range == ColorRange::full;
                const int scaled = std::max(0, luma - 16) * 76309;
                const std::array<int, 3> rgb{full ? luma + ((91881 * cr + 32768) >> 16)
                                                  : (scaled + 104597 * cr + 32768) >> 16,
                                             full
                                                 ? luma - ((22554 * cb + 46802 * cr + 32768) >> 16)
                                                 : (scaled - 25675 * cb - 53279 * cr + 32768) >> 16,
                                             full ? luma + ((116130 * cb + 32768) >> 16)
                                                  : (scaled + 132201 * cb + 32768) >> 16};
                for (std::size_t channel = 0; channel < 3; ++channel)
                    output[pixel * 4U + channel] =
                        static_cast<std::byte>(std::clamp(rgb[channel], 0, 255));
                output[pixel * 4U + 3U] = std::byte{255};
            }
        }
    }
    if (layout.alpha == AlphaMode::straight) {
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            for (std::uint32_t x = 0; x < frame.width; ++x) {
                const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4U;
                const std::byte alpha = sample(3, x, y);
                output[offset + 3U] = alpha;
                if (alpha_mode == AlphaMode::premultiplied)
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        output[offset + channel] = static_cast<std::byte>(
                            (std::to_integer<std::uint32_t>(output[offset + channel]) *
                                 std::to_integer<std::uint32_t>(alpha) +
                             127U) /
                            255U);
            }
        }
    }
    return output;
}

void verify_gray_pixels(const GeneratedRaster& source, RasterRect region) {
    const std::size_t stride = static_cast<std::size_t>(region.width) * 4U + 13U;
    std::vector<std::byte> output(stride * region.height, std::byte{0x6D});
    require(
        read_rgba8_region(source, 0, region, {region.width, region.height, kRgba8, stride, output})
            .has_value(),
        "bounded gray conversion succeeds");
    for (std::uint32_t y = 0; y < region.height; ++y) {
        for (std::uint32_t x = 0; x < region.width; ++x) {
            const std::size_t offset = static_cast<std::size_t>(y) * stride + x * 4U;
            const std::byte gray = sample(0, region.x + x, region.y + y);
            require(output[offset] == gray && output[offset + 1U] == gray &&
                        output[offset + 2U] == gray && output[offset + 3U] == std::byte{255},
                    "gray stripe boundaries and odd regions preserve exact pixels");
        }
        for (std::size_t x = static_cast<std::size_t>(region.width) * 4U; x < stride; ++x)
            require(output[static_cast<std::size_t>(y) * stride + x] == std::byte{0x6D},
                    "gray conversion preserves destination row padding");
    }
}

void test_gray() {
    constexpr std::uint32_t width = 259;
    constexpr std::uint32_t height = 2115;
    GeneratedRaster source(width, height);
    verify_gray_pixels(source, {1, 3, width - 2U, height - 4U});
    require(source.calls > 1 && source.maximum_plane_bytes <= 256U * 1024U,
            "gray staging is bounded independently of image height");
}

void test_gray_wide_row() {
    constexpr std::uint32_t row_bytes = 256U * 1024U + 19U;
    for (const std::uint32_t height : {6U, 17U}) {
        GeneratedRaster source(row_bytes + 2U, height);
        const RasterRect region{1, 1, row_bytes, height - 2U};
        verify_gray_pixels(source, region);
        require(source.maximum_read_rows == 1U && source.calls == region.height,
                "gray rows wider than the budget use the one-row minimum");
        require(source.maximum_plane_bytes == row_bytes,
                "wide gray staging owns exactly one row independently of image height");
    }
}

void test_ycbcr() {
    constexpr std::uint32_t width = 257;
    constexpr std::uint32_t height = 2113;
    constexpr std::array samplings{ChromaSubsampling::yuv444, ChromaSubsampling::yuv422,
                                   ChromaSubsampling::yuv420, ChromaSubsampling::yuv440,
                                   ChromaSubsampling::yuv411, ChromaSubsampling::yuv441};
    constexpr std::array regions{
        RasterRect{0, 0, width, height}, RasterRect{1, 3, width - 2U, height - 4U},
        RasterRect{1, 1, width - 1U, height - 1U}, RasterRect{width - 1U, height - 1U, 1, 1}};
    for (const auto sampling : samplings) {
        const auto [horizontal, vertical] = factors(sampling);
        (void)vertical;
        for (const auto range : {ColorRange::full, ColorRange::limited}) {
            for (const bool padded : {false, true}) {
                for (const bool has_alpha : {false, true}) {
                    for (const auto alpha_mode : {AlphaMode::straight, AlphaMode::premultiplied}) {
                        GeneratedRaster reference_source(width, height, sampling, range, padded,
                                                         has_alpha);
                        const auto expected = reference(reference_source, alpha_mode);
                        for (const auto region : regions) {
                            GeneratedRaster source(width, height, sampling, range, padded,
                                                   has_alpha);
                            const std::size_t stride =
                                static_cast<std::size_t>(region.width) * 4U + 13U;
                            std::vector<std::byte> output(stride * region.height + 31U,
                                                          std::byte{0x6D});
                            RasterConversionOptions options;
                            options.output_alpha = alpha_mode;
                            require(read_rgba8_region(
                                        source, 0, region,
                                        {region.width, region.height, kRgba8, stride, output},
                                        options)
                                        .has_value(),
                                    "bounded YCbCr conversion succeeds for every "
                                    "sampling/range/layout");
                            require(source.maximum_plane_bytes <= 256U * 1024U,
                                    "YCbCr staging is bounded independently of image height");
                            for (std::uint32_t y = 0; y < region.height; ++y) {
                                const std::size_t expected_offset =
                                    (static_cast<std::size_t>(region.y + y) *
                                         aligned(width, horizontal) +
                                     region.x) *
                                    4U;
                                require(
                                    std::equal(output.begin() +
                                                   static_cast<std::ptrdiff_t>(y * stride),
                                               output.begin() + static_cast<std::ptrdiff_t>(
                                                                    y * stride + region.width * 4U),
                                               expected.begin() +
                                                   static_cast<std::ptrdiff_t>(expected_offset)),
                                    "every stripe/crop/alpha pixel matches the full-frame backend "
                                    "oracle");
                                for (std::size_t x = static_cast<std::size_t>(region.width) * 4U;
                                     x < stride; ++x)
                                    require(output[static_cast<std::size_t>(y) * stride + x] ==
                                                std::byte{0x6D},
                                            "YCbCr conversion preserves destination row padding");
                            }
                            require(std::all_of(
                                        output.end() - 31, output.end(),
                                        [](std::byte value) { return value == std::byte{0x6D}; }),
                                    "odd tight bottom rows do not write beyond the output");
                        }
                    }
                }
            }
        }
    }
}

void test_errors_and_cancellation() {
    for (const auto sampling : {ChromaSubsampling::none, ChromaSubsampling::yuv420}) {
        GeneratedRaster source(257, 2113, sampling);
        std::vector<std::byte> output(257U * 2113U * 4U, std::byte{0x6D});
        const MutablePlaneView destination{257, 2113, kRgba8, 257U * 4U, output};
        const RasterRect region{0, 0, 257, 2113};
        source.fail_on_call = sampling == ChromaSubsampling::none ? 2 : 4;
        const auto failed = read_rgba8_region(source, 0, region, destination);
        require(!failed && failed.error().code == ErrorCode::io_error &&
                    failed.error().codec == "fixture" && failed.error().native_code == 47 &&
                    source.calls == source.fail_on_call,
                "later stripe source failures preserve the original status and stop reading");

        GeneratedRaster cancelling(257, 2113, sampling);
        std::stop_source stop;
        cancelling.cancellation = &stop;
        cancelling.cancel_after_call = sampling == ChromaSubsampling::none ? 1 : 3;
        std::fill(output.begin(), output.end(), std::byte{0x6D});
        const auto cancelled =
            read_rgba8_region(cancelling, 0, region, destination, stop.get_token());
        require(!cancelled && cancelled.error().code == ErrorCode::cancelled &&
                    cancelling.calls == cancelling.cancel_after_call &&
                    std::all_of(output.begin(), output.end(),
                                [](std::byte value) { return value == std::byte{0x6D}; }),
                "cancellation during the source read prevents conversion of that stripe");

        GeneratedRaster already_cancelled(257, 2113, sampling);
        const auto before_read =
            read_rgba8_region(already_cancelled, 0, region, destination, stop.get_token());
        require(!before_read && before_read.error().code == ErrorCode::cancelled &&
                    already_cancelled.calls == 0,
                "pre-requested cancellation reads no source rows");
    }
}

} // namespace

int main() {
    test_gray();
    test_gray_wide_row();
    test_ycbcr();
    test_errors_and_cancellation();
    std::cout << "raster conversion tests passed\n";
    return EXIT_SUCCESS;
}
