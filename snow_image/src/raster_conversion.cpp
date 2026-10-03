#include "snow/image/raster_conversion.h"

#include <snow/memory/pixel_array.h>

#if defined(SNOW_IMAGE_HAS_LIBYUV)
#include <libyuv/convert_argb.h>
#endif
#if defined(SNOW_IMAGE_HAS_JPEG)
#include <turbojpeg.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace snow::image {
namespace {

Status invalid(const char* message) {
    return Status::error(ErrorCode::invalid_argument, message, "raster conversion");
}

Status unsupported(const char* message) {
    return Status::error(ErrorCode::unsupported_feature, message, "raster conversion");
}

Result<std::pair<std::uint32_t, std::uint32_t>> sampling_factors(ChromaSubsampling sampling) {
    switch (sampling) {
    case ChromaSubsampling::yuv444:
        return std::pair{1U, 1U};
    case ChromaSubsampling::yuv422:
        return std::pair{2U, 1U};
    case ChromaSubsampling::yuv420:
        return std::pair{2U, 2U};
    case ChromaSubsampling::yuv440:
        return std::pair{1U, 2U};
    case ChromaSubsampling::yuv411:
        return std::pair{4U, 1U};
    case ChromaSubsampling::yuv441:
        return std::pair{1U, 4U};
    case ChromaSubsampling::none:
        return unsupported("YCbCr input has no chroma subsampling geometry.");
    }
    return unsupported("YCbCr input has unknown chroma subsampling geometry.");
}

bool gray_plane(const PlaneDescriptor& plane, PlaneSemantic semantic) {
    return plane.semantic == semantic && plane.format == kGray8 && plane.significant_bits == 8;
}

// Reuse a small working set rather than faulting and releasing whole temporary
// planes. A sampling group is the minimum when a single very wide row exceeds
// the budget; storage otherwise stays independent of the requested height.
constexpr std::size_t kConversionScratchBytes = 256U * 1024U;

std::uint32_t scratch_rows(std::size_t group_bytes, std::uint32_t group_height,
                           std::uint32_t height) {
    const std::size_t groups = std::max<std::size_t>(1U, kConversionScratchBytes / group_bytes);
    return static_cast<std::uint32_t>(std::min<std::size_t>(height, groups * group_height));
}

Result<void> read_plane_region(const RasterSource& source, std::uint32_t frame_index,
                               std::uint32_t plane_index, RasterRect region, std::size_t stride,
                               std::span<std::byte> pixels, std::stop_token stop) {
    MutablePlaneView view{region.width, region.height, kGray8, stride, pixels};
    return source.read_region(frame_index, plane_index, region, view, stop);
}

int convert_ycbcr(const RasterLayout& layout, const std::byte* y, int y_stride, const std::byte* cb,
                  int cb_stride, const std::byte* cr, int cr_stride, std::byte* rgba,
                  int rgba_stride, int width, int height) {
#if defined(SNOW_IMAGE_HAS_JPEG)
    if (layout.color_range == ColorRange::full) {
        int subsampling = TJSAMP_UNKNOWN;
        switch (layout.chroma_subsampling) {
        case ChromaSubsampling::yuv444:
            subsampling = TJSAMP_444;
            break;
        case ChromaSubsampling::yuv422:
            subsampling = TJSAMP_422;
            break;
        case ChromaSubsampling::yuv420:
            subsampling = TJSAMP_420;
            break;
        case ChromaSubsampling::yuv440:
            subsampling = TJSAMP_440;
            break;
        case ChromaSubsampling::yuv411:
            subsampling = TJSAMP_411;
            break;
        case ChromaSubsampling::yuv441:
            subsampling = TJSAMP_441;
            break;
        case ChromaSubsampling::none:
            break;
        }
        struct ThreadDecoder final {
            ThreadDecoder() : handle(tj3Init(TJINIT_DECOMPRESS)) {}
            ~ThreadDecoder() {
                if (handle)
                    tj3Destroy(handle);
            }
            void* handle = nullptr;
        };
        thread_local ThreadDecoder decoder;
        if (decoder.handle && subsampling != TJSAMP_UNKNOWN &&
            tj3Set(decoder.handle, TJPARAM_SUBSAMP, subsampling) == 0) {
            const std::array<const unsigned char*, 3> planes{
                reinterpret_cast<const unsigned char*>(y),
                reinterpret_cast<const unsigned char*>(cb),
                reinterpret_cast<const unsigned char*>(cr)};
            const std::array<int, 3> strides{y_stride, cb_stride, cr_stride};
            return tj3DecodeYUVPlanes8(decoder.handle, planes.data(), strides.data(),
                                       reinterpret_cast<unsigned char*>(rgba), width, rgba_stride,
                                       height, TJPF_RGBA);
        }
    }
#endif
#if defined(SNOW_IMAGE_HAS_LIBYUV)
    const auto* y_bytes = reinterpret_cast<const std::uint8_t*>(y);
    const auto* cb_bytes = reinterpret_cast<const std::uint8_t*>(cb);
    const auto* cr_bytes = reinterpret_cast<const std::uint8_t*>(cr);
    auto* rgba_bytes = reinterpret_cast<std::uint8_t*>(rgba);
    const bool full = layout.color_range == ColorRange::full;
    switch (layout.chroma_subsampling) {
    case ChromaSubsampling::yuv444:
        return full ? libyuv::J444ToABGR(y_bytes, y_stride, cb_bytes, cb_stride, cr_bytes,
                                         cr_stride, rgba_bytes, rgba_stride, width, height)
                    : libyuv::I444ToABGR(y_bytes, y_stride, cb_bytes, cb_stride, cr_bytes,
                                         cr_stride, rgba_bytes, rgba_stride, width, height);
    case ChromaSubsampling::yuv422:
        return full ? libyuv::J422ToABGR(y_bytes, y_stride, cb_bytes, cb_stride, cr_bytes,
                                         cr_stride, rgba_bytes, rgba_stride, width, height)
                    : libyuv::I422ToABGR(y_bytes, y_stride, cb_bytes, cb_stride, cr_bytes,
                                         cr_stride, rgba_bytes, rgba_stride, width, height);
    case ChromaSubsampling::yuv420:
        return full ? libyuv::J420ToABGR(y_bytes, y_stride, cb_bytes, cb_stride, cr_bytes,
                                         cr_stride, rgba_bytes, rgba_stride, width, height)
                    : libyuv::I420ToABGR(y_bytes, y_stride, cb_bytes, cb_stride, cr_bytes,
                                         cr_stride, rgba_bytes, rgba_stride, width, height);
    case ChromaSubsampling::none:
    case ChromaSubsampling::yuv440:
    case ChromaSubsampling::yuv411:
    case ChromaSubsampling::yuv441:
        return -1;
    }
    return -1;
#else
    const auto clamp = [](int value) { return static_cast<std::byte>(std::clamp(value, 0, 255)); };
    Result<std::pair<std::uint32_t, std::uint32_t>> factors =
        sampling_factors(layout.chroma_subsampling);
    if (!factors)
        return -1;
    const auto [horizontal, vertical] = factors.value();
    for (int row = 0; row < height; ++row) {
        const std::size_t row_index = static_cast<std::size_t>(row);
        const std::size_t chroma_row = row_index / vertical;
        const auto* y_row = y + row_index * static_cast<std::size_t>(y_stride);
        const auto* cb_row = cb + chroma_row * static_cast<std::size_t>(cb_stride);
        const auto* cr_row = cr + chroma_row * static_cast<std::size_t>(cr_stride);
        auto* destination = rgba + row_index * static_cast<std::size_t>(rgba_stride);
        for (int column = 0; column < width; ++column) {
            const std::size_t column_index = static_cast<std::size_t>(column);
            const std::size_t chroma_column = column_index / horizontal;
            const int luma = std::to_integer<std::uint8_t>(y_row[column_index]);
            const int blue_difference = std::to_integer<std::uint8_t>(cb_row[chroma_column]) - 128;
            const int red_difference = std::to_integer<std::uint8_t>(cr_row[chroma_column]) - 128;
            int red = 0;
            int green = 0;
            int blue = 0;
            if (layout.color_range == ColorRange::full) {
                red = luma + ((91881 * red_difference + 32768) >> 16);
                green = luma - ((22554 * blue_difference + 46802 * red_difference + 32768) >> 16);
                blue = luma + ((116130 * blue_difference + 32768) >> 16);
            } else {
                const int scaled_luma = std::max(0, luma - 16) * 76309;
                red = (scaled_luma + 104597 * red_difference + 32768) >> 16;
                green =
                    (scaled_luma - 25675 * blue_difference - 53279 * red_difference + 32768) >> 16;
                blue = (scaled_luma + 132201 * blue_difference + 32768) >> 16;
            }
            const std::size_t offset = column_index * 4U;
            destination[offset] = clamp(red);
            destination[offset + 1U] = clamp(green);
            destination[offset + 2U] = clamp(blue);
            destination[offset + 3U] = std::byte{0xFF};
        }
    }
    return 0;
#endif
}

int convert_gray(const std::byte* gray, int gray_stride, std::byte* rgba, int rgba_stride,
                 int width, int height) {
#if defined(SNOW_IMAGE_HAS_LIBYUV)
    return libyuv::J400ToARGB(reinterpret_cast<const std::uint8_t*>(gray), gray_stride,
                              reinterpret_cast<std::uint8_t*>(rgba), rgba_stride, width, height);
#else
    for (int row = 0; row < height; ++row) {
        const std::size_t row_index = static_cast<std::size_t>(row);
        const std::byte* source = gray + row_index * static_cast<std::size_t>(gray_stride);
        std::byte* destination = rgba + row_index * static_cast<std::size_t>(rgba_stride);
        for (int column = 0; column < width; ++column) {
            const std::size_t column_index = static_cast<std::size_t>(column);
            const std::size_t offset = column_index * 4U;
            destination[offset] = source[column_index];
            destination[offset + 1U] = source[column_index];
            destination[offset + 2U] = source[column_index];
            destination[offset + 3U] = std::byte{0xFF};
        }
    }
    return 0;
#endif
}

} // namespace

Result<void> read_rgba8_region(const RasterSource& source, std::uint32_t frame_index,
                               RasterRect region, MutablePlaneView destination,
                               std::stop_token stop) {
    return read_rgba8_region(source, frame_index, region, destination, RasterConversionOptions{},
                             stop);
}

Result<void> read_rgba8_region(const RasterSource& source, std::uint32_t frame_index,
                               RasterRect region, MutablePlaneView destination,
                               const RasterConversionOptions& options, std::stop_token stop) {
    Result<void> destination_status = destination.validate();
    if (!destination_status)
        return destination_status.error();
    if (destination.format != kRgba8 || destination.width != region.width ||
        destination.height != region.height)
        return invalid("Raster conversion requires a matching RGBA8 destination.");
    const DocumentDescriptor& descriptor = source.descriptor();
    if (frame_index >= descriptor.frames.size())
        return invalid("Raster conversion references an unknown frame.");
    const RasterFrameDescriptor& frame = descriptor.frames[frame_index];
    if (region.width == 0 || region.height == 0 || region.x > frame.width ||
        region.y > frame.height || region.width > frame.width - region.x ||
        region.height > frame.height - region.y)
        return invalid("Raster conversion region is outside the source frame.");
    if (region.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        region.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        destination.row_stride > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return Status::error(ErrorCode::limit_exceeded,
                             "Raster conversion dimensions exceed backend limits.",
                             "raster conversion");
    if (stop.stop_requested())
        return Status::error(ErrorCode::cancelled, "Raster conversion was cancelled.",
                             "raster conversion");
    if (options.output_alpha != AlphaMode::straight &&
        options.output_alpha != AlphaMode::premultiplied)
        return invalid("RGBA8 output alpha must be straight or premultiplied.");

    const RasterLayout& layout = frame.layout;
    if (layout.planes.size() == 1 && layout.planes.front().semantic == PlaneSemantic::packed &&
        layout.planes.front().format == kRgba8) {
        return source.read_region(frame_index, 0, region, destination, stop);
    }
    if (layout.color_model == ColorModel::gray && layout.planes.size() == 1 &&
        gray_plane(layout.planes.front(), PlaneSemantic::gray)) {
        try {
            const std::uint32_t rows = scratch_rows(region.width, 1U, region.height);
            snow::memory::PixelArray<std::byte> gray(static_cast<std::size_t>(region.width) * rows);
            for (std::uint32_t first = 0; first < region.height; first += rows) {
                if (stop.stop_requested())
                    return Status::error(ErrorCode::cancelled, "Raster conversion was cancelled.",
                                         "raster conversion");
                const std::uint32_t count = std::min(rows, region.height - first);
                Result<void> read = read_plane_region(
                    source, frame_index, 0, {region.x, region.y + first, region.width, count},
                    region.width, gray, stop);
                if (!read)
                    return read.error();
                if (stop.stop_requested())
                    return Status::error(ErrorCode::cancelled, "Raster conversion was cancelled.",
                                         "raster conversion");
                const int converted =
                    convert_gray(gray.data(), static_cast<int>(region.width),
                                 destination.pixels.data() +
                                     static_cast<std::size_t>(first) * destination.row_stride,
                                 static_cast<int>(destination.row_stride),
                                 static_cast<int>(region.width), static_cast<int>(count));
                if (converted != 0)
                    return Status::error(ErrorCode::decode_failed,
                                         "Grayscale raster conversion failed.", "libyuv");
            }
            return {};
        } catch (const std::bad_alloc&) {
            return Status::error(ErrorCode::out_of_memory,
                                 "Could not allocate bounded raster conversion planes.",
                                 "raster conversion");
        }
    }
    const bool has_alpha = layout.alpha == AlphaMode::straight;
    if (layout.color_model != ColorModel::ycbcr ||
        (layout.alpha != AlphaMode::none && !has_alpha) ||
        layout.planes.size() != (has_alpha ? 4U : 3U) ||
        !gray_plane(layout.planes[0], PlaneSemantic::luma) ||
        !gray_plane(layout.planes[1], PlaneSemantic::chroma_blue) ||
        !gray_plane(layout.planes[2], PlaneSemantic::chroma_red) ||
        (has_alpha && !gray_plane(layout.planes[3], PlaneSemantic::alpha)))
        return unsupported("This raster layout cannot be converted to RGBA8.");

    Result<std::pair<std::uint32_t, std::uint32_t>> factors =
        sampling_factors(layout.chroma_subsampling);
    if (!factors)
        return factors.error();
    const auto [horizontal, vertical] = factors.value();
    const auto aligned_limit = [](std::uint32_t value, std::uint32_t factor, std::uint32_t limit) {
        const std::uint64_t aligned =
            ((static_cast<std::uint64_t>(value) + factor - 1U) / factor) * factor;
        return static_cast<std::uint32_t>(std::min<std::uint64_t>(aligned, limit));
    };
    const PlaneDescriptor& luma_plane = layout.planes[0];
    const std::uint32_t padded_luma_width =
        aligned_limit(frame.width, horizontal, std::numeric_limits<std::uint32_t>::max());
    const std::uint32_t padded_luma_height =
        aligned_limit(frame.height, vertical, std::numeric_limits<std::uint32_t>::max());
    const std::uint32_t tight_chroma_width = (frame.width + horizontal - 1U) / horizontal;
    const std::uint32_t tight_chroma_height = (frame.height + vertical - 1U) / vertical;
    const bool tight = luma_plane.width == frame.width && luma_plane.height == frame.height &&
                       layout.planes[1].width == tight_chroma_width &&
                       layout.planes[2].width == tight_chroma_width &&
                       layout.planes[1].height == tight_chroma_height &&
                       layout.planes[2].height == tight_chroma_height;
    const bool padded = luma_plane.width == padded_luma_width &&
                        luma_plane.height == padded_luma_height &&
                        layout.planes[1].width == padded_luma_width / horizontal &&
                        layout.planes[2].width == padded_luma_width / horizontal &&
                        layout.planes[1].height == padded_luma_height / vertical &&
                        layout.planes[2].height == padded_luma_height / vertical;
    if ((!tight && !padded) || (has_alpha && (layout.planes[3].width != frame.width ||
                                              layout.planes[3].height != frame.height)))
        return invalid("YCbCr plane dimensions do not match their subsampling.");

    const std::uint32_t left = region.x - region.x % horizontal;
    const std::uint32_t top = region.y - region.y % vertical;
    const std::uint32_t region_right = region.x + region.width;
    const std::uint32_t region_bottom = region.y + region.height;
    const std::uint32_t right = aligned_limit(region_right, horizontal, luma_plane.width);
    const std::uint32_t bottom = aligned_limit(region_bottom, vertical, luma_plane.height);
    const RasterRect luma_region{left, top, right - left, bottom - top};
    const RasterRect chroma_region{left / horizontal, top / vertical,
                                   (right + horizontal - 1U) / horizontal - left / horizontal,
                                   (bottom + vertical - 1U) / vertical - top / vertical};
    // TurboJPEG's planar conversion uses box upsampling. Libyuv and the
    // scalar fallback also use one chroma sample per sampling group, so aligned
    // stripes preserve the full conversion's pixels without rereading halos.
    const bool direct = left == region.x && top == region.y && luma_region.width == region.width &&
                        luma_region.height == region.height;
    const bool uncommon = layout.chroma_subsampling != ChromaSubsampling::yuv444 &&
                          layout.chroma_subsampling != ChromaSubsampling::yuv422 &&
                          layout.chroma_subsampling != ChromaSubsampling::yuv420;
    const std::size_t y_stride =
        (static_cast<std::size_t>(luma_region.width) + horizontal - 1U) / horizontal * horizontal;
    const std::size_t rgba_stride = static_cast<std::size_t>(luma_region.width) * 4U;
    if (y_stride > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        rgba_stride > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return Status::error(ErrorCode::limit_exceeded,
                             "Raster conversion aligned rows exceed backend limits.",
                             "raster conversion");
    const std::size_t group_bytes =
        y_stride * vertical + static_cast<std::size_t>(chroma_region.width) * 2U +
        (direct ? 0U : rgba_stride * vertical) +
        (has_alpha ? static_cast<std::size_t>(region.width) * vertical : 0U) +
        (uncommon ? static_cast<std::size_t>(luma_region.width) * vertical * 2U : 0U);
    const std::uint32_t rows = scratch_rows(group_bytes, vertical, luma_region.height);
    const std::uint32_t padded_rows = (rows + vertical - 1U) / vertical * vertical;
    try {
        snow::memory::PixelArray<std::byte> y(y_stride * padded_rows);
        snow::memory::PixelArray<std::byte> cb(static_cast<std::size_t>(chroma_region.width) *
                                               (padded_rows / vertical));
        snow::memory::PixelArray<std::byte> cr(cb.size());
        snow::memory::PixelArray<std::byte> alpha;
        snow::memory::PixelArray<std::byte> rgba;
        snow::memory::PixelArray<std::byte> expanded_cb;
        snow::memory::PixelArray<std::byte> expanded_cr;
        if (has_alpha)
            alpha.resize(static_cast<std::size_t>(region.width) * rows);
        if (!direct)
            rgba.resize(rgba_stride * rows);

        for (std::uint32_t first = 0; first < luma_region.height; first += rows) {
            if (stop.stop_requested())
                return Status::error(ErrorCode::cancelled, "Raster conversion was cancelled.",
                                     "raster conversion");
            const std::uint32_t count = std::min(rows, luma_region.height - first);
            const RasterRect y_region{left, top + first, luma_region.width, count};
            const RasterRect uv_region{chroma_region.x, (top + first) / vertical,
                                       chroma_region.width, (count + vertical - 1U) / vertical};
            Result<void> read =
                read_plane_region(source, frame_index, 0, y_region, y_stride, y, stop);
            if (!read)
                return read.error();
            read =
                read_plane_region(source, frame_index, 1, uv_region, chroma_region.width, cb, stop);
            if (!read)
                return read.error();
            read =
                read_plane_region(source, frame_index, 2, uv_region, chroma_region.width, cr, stop);
            if (!read)
                return read.error();

            // Tight odd planes still need the padding required by TurboJPEG's
            // YUV API. Replicate edges without reading outside source geometry.
            for (std::uint32_t row = 0; row < count; ++row) {
                std::byte* y_row = y.data() + static_cast<std::size_t>(row) * y_stride;
                std::fill(y_row + luma_region.width, y_row + y_stride,
                          y_row[luma_region.width - 1U]);
            }
            const std::uint32_t aligned_count = (count + vertical - 1U) / vertical * vertical;
            for (std::uint32_t row = count; row < aligned_count; ++row)
                std::memcpy(y.data() + static_cast<std::size_t>(row) * y_stride,
                            y.data() + static_cast<std::size_t>(count - 1U) * y_stride, y_stride);

            const std::uint32_t output_top = std::max(y_region.y, region.y);
            const std::uint32_t output_bottom = std::min(y_region.y + count, region_bottom);
            const std::uint32_t output_rows = output_bottom - output_top;
            const std::uint32_t destination_first = output_top - region.y;
            if (has_alpha) {
                read = read_plane_region(source, frame_index, 3,
                                         {region.x, output_top, region.width, output_rows},
                                         region.width, alpha, stop);
                if (!read)
                    return read.error();
            }
            if (stop.stop_requested())
                return Status::error(ErrorCode::cancelled, "Raster conversion was cancelled.",
                                     "raster conversion");

            std::byte* output = direct
                                    ? destination.pixels.data() +
                                          static_cast<std::size_t>(first) * destination.row_stride
                                    : rgba.data();
            const int output_stride =
                static_cast<int>(direct ? destination.row_stride : rgba_stride);
            int converted =
                convert_ycbcr(layout, y.data(), static_cast<int>(y_stride), cb.data(),
                              static_cast<int>(chroma_region.width), cr.data(),
                              static_cast<int>(chroma_region.width), output, output_stride,
                              static_cast<int>(luma_region.width), static_cast<int>(count));
            if (converted != 0 && uncommon) {
                expanded_cb.resize(static_cast<std::size_t>(luma_region.width) * count);
                expanded_cr.resize(expanded_cb.size());
                for (std::uint32_t row = 0; row < count; ++row) {
                    if (stop.stop_requested())
                        return Status::error(ErrorCode::cancelled,
                                             "Raster conversion was cancelled.",
                                             "raster conversion");
                    const std::size_t chroma_row = row / vertical;
                    for (std::uint32_t column = 0; column < luma_region.width; ++column) {
                        const std::size_t chroma_offset =
                            chroma_row * chroma_region.width + column / horizontal;
                        const std::size_t offset =
                            static_cast<std::size_t>(row) * luma_region.width + column;
                        expanded_cb[offset] = cb[chroma_offset];
                        expanded_cr[offset] = cr[chroma_offset];
                    }
                }
                RasterLayout expanded_layout = layout;
                expanded_layout.chroma_subsampling = ChromaSubsampling::yuv444;
                converted = convert_ycbcr(
                    expanded_layout, y.data(), static_cast<int>(y_stride), expanded_cb.data(),
                    static_cast<int>(luma_region.width), expanded_cr.data(),
                    static_cast<int>(luma_region.width), output, output_stride,
                    static_cast<int>(luma_region.width), static_cast<int>(count));
            }
            if (converted != 0)
                return Status::error(ErrorCode::decode_failed, "YCbCr raster conversion failed.",
                                     "libyuv");
            if (!direct) {
                const std::size_t source_x = static_cast<std::size_t>(region.x - left) * 4U;
                const std::size_t source_y = output_top - y_region.y;
                const std::size_t row_bytes = static_cast<std::size_t>(region.width) * 4U;
                for (std::uint32_t row = 0; row < output_rows; ++row)
                    std::memcpy(destination.pixels.data() +
                                    static_cast<std::size_t>(destination_first + row) *
                                        destination.row_stride,
                                rgba.data() + (source_y + row) * rgba_stride + source_x, row_bytes);
            }
            if (has_alpha) {
                for (std::uint32_t row = 0; row < output_rows; ++row) {
                    std::byte* destination_row =
                        destination.pixels.data() +
                        static_cast<std::size_t>(destination_first + row) * destination.row_stride;
                    const std::byte* alpha_row =
                        alpha.data() + static_cast<std::size_t>(row) * region.width;
                    for (std::uint32_t column = 0; column < region.width; ++column) {
                        const std::uint8_t value = std::to_integer<std::uint8_t>(alpha_row[column]);
                        const std::size_t offset = static_cast<std::size_t>(column) * 4U;
                        destination_row[offset + 3U] = alpha_row[column];
                        if (options.output_alpha == AlphaMode::premultiplied) {
                            for (std::size_t channel = 0; channel < 3; ++channel) {
                                const std::uint32_t sample = std::to_integer<std::uint8_t>(
                                    destination_row[offset + channel]);
                                destination_row[offset + channel] =
                                    static_cast<std::byte>((sample * value + 127U) / 255U);
                            }
                        }
                    }
                }
            }
        }
        return {};
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate bounded RGBA conversion storage.",
                             "raster conversion");
    }
}

} // namespace snow::image
