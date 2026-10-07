#include "snow/image/resource_plan.h"
#include "snow/image/service.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace snow::image;
constexpr std::uint64_t kMiB = std::uint64_t{1} << 20U;

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

template <typename T> T take(Result<T> result, std::string_view context) {
    if (!result)
        throw std::runtime_error(std::string(context) + ": " + result.error().message);
    return std::move(result).value();
}

DocumentDescriptor packedDescriptor(std::uint32_t width, std::uint32_t height,
                                    PixelFormat format = kRgba8) {
    DocumentDescriptor descriptor;
    descriptor.format = Format::webp;
    descriptor.canvas_width = width;
    descriptor.canvas_height = height;
    descriptor.color.primaries = ColorPrimaries::srgb;
    descriptor.color.transfer = TransferFunction::srgb;
    RasterFrameDescriptor frame;
    frame.width = width;
    frame.height = height;
    frame.layout.alpha = format.alpha;
    frame.layout.planes.push_back(
        {PlaneSemantic::packed, width, height, format, format.bits_per_channel});
    descriptor.frames.push_back(std::move(frame));
    require(descriptor.validate().has_value(), "packed planning descriptor is valid");
    return descriptor;
}

DocumentDescriptor planarDescriptor(std::uint32_t width, std::uint32_t height, bool alpha = true) {
    DocumentDescriptor descriptor = packedDescriptor(width, height);
    auto& layout = descriptor.frames.front().layout;
    layout.color_model = ColorModel::ycbcr;
    layout.color_range = ColorRange::limited;
    layout.alpha = alpha ? AlphaMode::straight : AlphaMode::none;
    layout.chroma_subsampling = ChromaSubsampling::yuv420;
    const auto chroma_width = width / 2U + width % 2U;
    const auto chroma_height = height / 2U + height % 2U;
    layout.planes = {{PlaneSemantic::luma, width, height, kGray8, 8},
                     {PlaneSemantic::chroma_blue, chroma_width, chroma_height, kGray8, 8},
                     {PlaneSemantic::chroma_red, chroma_width, chroma_height, kGray8, 8}};
    if (alpha)
        layout.planes.push_back({PlaneSemantic::alpha, width, height, kGray8, 8});
    require(descriptor.validate().has_value(), "planar planning descriptor is valid");
    return descriptor;
}

ResourcePlanRequest requestFor(DocumentDescriptor descriptor, bool lossless = false) {
    ResourcePlanRequest request;
    request.source = descriptor;
    request.output = std::move(descriptor);
    request.encode_options.format = Format::webp;
    request.encode_options.lossless = lossless;
    request.encode_options.preserve_metadata = false;
    request.raster_route = RasterEncodeRoute::native;
    // These fixtures describe large images without allocating any pixel storage.
    request.budgets.private_memory_bytes = std::numeric_limits<std::uint64_t>::max();
    request.gpu_transform = false;
    request.maximum_cpu_threads = 1;
    Service service;
    if (service.encoder_info(Format::webp)) {
        require(take(service.raster_encode_route(request.output, request.encode_options),
                     "query WebP raster route") == RasterEncodeRoute::native,
                "WebP planning fixture selects the native raster route");
    }
    return request;
}

std::uint64_t encodeBytes(const ResourcePlan& plan) {
    for (const auto& phase : plan.phases) {
        if (phase.phase == ResourcePhase::encode)
            return phase.footprint.private_memory_bytes;
    }
    throw std::runtime_error("resource plan omitted the encode phase");
}

std::uint64_t lossyPackedPictureBytes(std::uint32_t width, std::uint32_t height) {
    const auto pixels = std::uint64_t{width} * height;
    const auto chroma_pixels =
        ((std::uint64_t{width} + 1U) / 2U) * ((std::uint64_t{height} + 1U) / 2U);
    // ARGB remains live alongside Y, U, V and the conservative alpha plane.
    return pixels * 4U + pixels + chroma_pixels * 2U + pixels;
}

void packedLossyPlansCoverSimultaneousPictures() {
    constexpr std::uint32_t edge = 8192;
    for (const auto format : {kRgba8, kBgra8}) {
        auto request = requestFor(packedDescriptor(edge, edge, format));
        const auto native = take(plan_resources(request), "plan packed lossy WebP");
        require(encodeBytes(native) >= lossyPackedPictureBytes(edge, edge),
                "native packed WebP must budget both ARGB and YUV420 with alpha");

        auto materialized_request = request;
        materialized_request.raster_route = RasterEncodeRoute::materialized;
        const auto materialized =
            take(plan_resources(materialized_request), "plan materialized lossy WebP");
        require(encodeBytes(native) == encodeBytes(materialized),
                "packed WebP pictures require the same budget on native and materialized routes");

        request.encode_options.verified_alpha_content = AlphaContent::non_opaque;
        const auto non_opaque = take(plan_resources(request), "plan verified non-opaque WebP");
        require(encodeBytes(non_opaque) == encodeBytes(native),
                "unknown alpha must retain the same conservative picture budget as transparency");

        request.encode_options.verified_alpha_content.reset();
        request.budgets.private_memory_bytes = 400U * kMiB;
        const auto rejected = plan_resources(request);
        require(!rejected && rejected.error().code == ErrorCode::limit_exceeded,
                "a 400 MiB budget must reject 8192-square lossy packed WebP with possible alpha");
        request.encode_options.verified_alpha_content = AlphaContent::opaque;
        require(plan_resources(request).has_value(),
                "verified opaque WebP should fit without allocating a codec alpha plane");
        request.encode_options.verified_alpha_content.reset();
        request.budgets.private_memory_bytes = 512U * kMiB;
        require(plan_resources(request).has_value(),
                "a sufficient budget admits the complete lossy packed WebP picture state");
    }
}

void oddDimensionsRoundBothChromaPlanesUp() {
    const auto even =
        take(plan_resources(requestFor(packedDescriptor(8192, 8192))), "plan even WebP dimensions");
    for (const auto size :
         {std::pair{8193U, 8192U}, std::pair{8192U, 8193U}, std::pair{8193U, 8193U}}) {
        const auto odd = take(plan_resources(requestFor(packedDescriptor(size.first, size.second))),
                              "plan odd WebP dimensions");
        const auto additional_picture_bytes =
            lossyPackedPictureBytes(size.first, size.second) - lossyPackedPictureBytes(8192, 8192);
        require(encodeBytes(odd) - encodeBytes(even) >= additional_picture_bytes,
                "odd WebP dimensions must include the rounded U and V plane extents");
    }
}

void losslessAndPlanarRoutesKeepTheirDistinctBudgets() {
    constexpr std::uint32_t edge = 8192;
    const auto packed_lossy =
        take(plan_resources(requestFor(packedDescriptor(edge, edge))), "plan packed lossy WebP");
    auto lossless_request = requestFor(packedDescriptor(edge, edge), true);
    const auto lossless = take(plan_resources(lossless_request), "plan packed lossless WebP");
    require(encodeBytes(lossless) >= std::uint64_t{edge} * edge * 6U &&
                encodeBytes(lossless) < encodeBytes(packed_lossy),
            "packed lossless WebP retains its established budget without lossy alpha conversion");
    lossless_request.raster_route = RasterEncodeRoute::materialized;
    require(encodeBytes(take(plan_resources(lossless_request),
                             "plan materialized lossless WebP")) == encodeBytes(lossless),
            "native packed lossless WebP must not reduce the established picture budget");
    lossless_request.budgets.private_memory_bytes = 400U * kMiB;
    require(plan_resources(lossless_request).has_value(),
            "lossless packed WebP remains admissible within its established 400 MiB budget");

    auto planar_request = requestFor(planarDescriptor(edge, edge));
    const auto planar = take(plan_resources(planar_request), "plan planar native WebP");
    const auto planar_pixel_bytes = std::uint64_t{edge} * edge * 5U / 2U;
    require(encodeBytes(planar) >= planar_pixel_bytes &&
                encodeBytes(planar) < std::uint64_t{edge} * edge * 4U,
            "native planar WebP should retain its YUV and alpha planes without an ARGB canvas");
    planar_request.budgets.private_memory_bytes = 300U * kMiB;
    require(plan_resources(planar_request).has_value(),
            "native planar WebP must retain admission under a smaller picture budget");
    const auto planar_without_alpha =
        take(plan_resources(requestFor(planarDescriptor(edge, edge, false))),
             "plan planar WebP without alpha");
    require(encodeBytes(planar_without_alpha) >= std::uint64_t{edge} * edge * 3U / 2U &&
                encodeBytes(planar) - encodeBytes(planar_without_alpha) >=
                    std::uint64_t{edge} * edge,
            "native planar WebP without alpha omits the separate codec alpha plane");
}

void metadataAssemblyRemainsBudgeted() {
    auto request = requestFor(packedDescriptor(257, 129));
    request.expected_artifact_bytes = 4U * kMiB;
    request.output.color.icc_profile.resize(4096);
    request.output.metadata.exif.resize(2048);
    request.output.metadata.xmp.resize(1024);
    const auto stripped = take(plan_resources(request), "plan stripped WebP");
    request.encode_options.preserve_metadata = true;
    const auto preserved = take(plan_resources(request), "plan WebP metadata assembly");
    const auto metadata_bytes = request.output.color.icc_profile.size() +
                                request.output.metadata.exif.size() +
                                request.output.metadata.xmp.size();
    require(encodeBytes(preserved) >=
                encodeBytes(stripped) + request.expected_artifact_bytes * 2U + metadata_bytes,
            "metadata assembly must budget the encoded input and assembled output concurrently");
    request.budgets.private_memory_bytes = stripped.peak.private_memory_bytes;
    const auto rejected = plan_resources(request);
    require(!rejected && rejected.error().code == ErrorCode::limit_exceeded,
            "preserving metadata cannot reuse a budget that covers only the picture state");
}

void overflowingRequestsFailBeforeAllocation() {
    ResourcePlanRequest request;
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    request.source = packedDescriptor(maximum, maximum);
    request.output = request.source;
    request.encode_options.format = Format::webp;
    request.raster_route = RasterEncodeRoute::native;
    request.gpu_transform = false;
    request.budgets.private_memory_bytes = std::numeric_limits<std::uint64_t>::max();
    const auto oversized = plan_resources(request);
    require(!oversized && oversized.error().code == ErrorCode::limit_exceeded,
            "overflowing WebP raster sizes must be rejected instead of wrapping their budget");

    request = requestFor(packedDescriptor(257, 129));
    request.encode_options.preserve_metadata = true;
    request.output.metadata.exif.resize(1);
    request.expected_artifact_bytes = std::numeric_limits<std::uint64_t>::max() / 2U + 1U;
    const auto assembly = plan_resources(request);
    require(!assembly && assembly.error().code == ErrorCode::limit_exceeded,
            "overflowing WebP metadata assembly must be rejected before allocation");
}

} // namespace

int main() {
    try {
        packedLossyPlansCoverSimultaneousPictures();
        oddDimensionsRoundBothChromaPlanesUp();
        losslessAndPlanarRoutesKeepTheirDistinctBudgets();
        metadataAssemblyRemainsBudgeted();
        overflowingRequestsFailBeforeAllocation();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
