#include "snow/image/resource_plan.h"

#include "snow/image/processing.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <thread>

namespace snow::image {
namespace {

Status overflow() {
    return Status::error(ErrorCode::limit_exceeded, "Resource plan arithmetic overflowed.");
}

bool add(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left)
        return false;
    *result = left + right;
    return true;
}

bool multiply(std::uint64_t left, std::uint64_t right, std::uint64_t* result) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left)
        return false;
    *result = left * right;
    return true;
}

Result<std::uint64_t> raster_bytes(const DocumentDescriptor& descriptor) {
    std::uint64_t total = 0;
    for (const RasterFrameDescriptor& frame : descriptor.frames) {
        for (const PlaneDescriptor& plane : frame.layout.planes) {
            Result<std::size_t> row = plane.row_bytes();
            std::uint64_t plane_bytes = 0;
            if (!row || !multiply(row.value(), plane.height, &plane_bytes) ||
                !add(total, plane_bytes, &total))
                return row ? overflow() : row.error();
        }
    }
    return total;
}

Result<std::uint64_t> maximum_row_bytes(const DocumentDescriptor& descriptor) {
    std::uint64_t maximum = 0;
    for (const RasterFrameDescriptor& frame : descriptor.frames) {
        for (const PlaneDescriptor& plane : frame.layout.planes) {
            Result<std::size_t> row = plane.row_bytes();
            if (!row)
                return row.error();
            maximum = std::max(maximum, static_cast<std::uint64_t>(row.value()));
        }
    }
    return maximum;
}

Result<std::uint64_t> maximum_pixel_bytes(const DocumentDescriptor& descriptor) {
    std::uint64_t maximum = 0;
    for (const RasterFrameDescriptor& frame : descriptor.frames) {
        std::uint64_t frame_bytes = 0;
        for (const PlaneDescriptor& plane : frame.layout.planes) {
            Result<std::size_t> bytes = plane.format.bytes_per_pixel();
            if (!bytes)
                return bytes.error();
            frame_bytes += bytes.value();
        }
        maximum = std::max(maximum, frame_bytes);
    }
    return maximum;
}

Result<std::uint64_t> webp_picture_storage(const ResourcePlanRequest& request,
                                           std::uint64_t raster_bytes) {
    const auto& frames = request.output.frames;
    const bool animation = frames.size() > 1;
    const bool native_planar = request.raster_route == RasterEncodeRoute::native && !animation &&
                               !request.encode_options.lossless &&
                               frames.front().layout.color_model == ColorModel::ycbcr;
    std::uint64_t storage = raster_bytes;
    if (!native_planar) {
        std::uint64_t pixels = 0;
        if (!multiply(request.output.canvas_width, request.output.canvas_height, &pixels) ||
            !multiply(pixels, animation ? 8U : 6U, &storage))
            return overflow();
        if (!animation && !request.encode_options.lossless) {
            // Packed input still owns ARGB when libwebp allocates YUV420. The
            // route avoids materializing the source, not these codec buffers.
            const std::uint64_t chroma_width =
                (static_cast<std::uint64_t>(request.output.canvas_width) + 1U) / 2U;
            const std::uint64_t chroma_height =
                (static_cast<std::uint64_t>(request.output.canvas_height) + 1U) / 2U;
            std::uint64_t chroma_bytes = 0;
            std::uint64_t picture_bytes = 0;
            if (!multiply(chroma_width, chroma_height, &chroma_bytes) ||
                !multiply(chroma_bytes, 2U, &chroma_bytes) ||
                !multiply(pixels, 5U, &picture_bytes) ||
                !add(picture_bytes, chroma_bytes, &picture_bytes))
                return overflow();
            const bool alpha =
                frames.front().layout.alpha != AlphaMode::none &&
                request.encode_options.verified_alpha_content != AlphaContent::opaque;
            if (alpha && !add(picture_bytes, pixels, &picture_bytes))
                return overflow();
            // Keep the existing packed working-set allowance, including the
            // lossless path, and raise it when the live picture buffers need more.
            storage = std::max(storage, picture_bytes);
        }
    }
    if (!add(storage, std::uint64_t{8} << 20U, &storage))
        return overflow();
    return storage;
}

void include_peak(ResourceFootprint* peak, const ResourceFootprint& value) {
    peak->private_memory_bytes = std::max(peak->private_memory_bytes, value.private_memory_bytes);
    peak->mapped_bytes = std::max(peak->mapped_bytes, value.mapped_bytes);
    peak->gpu_bytes = std::max(peak->gpu_bytes, value.gpu_bytes);
    peak->artifact_bytes = std::max(peak->artifact_bytes, value.artifact_bytes);
    peak->temporary_disk_bytes = std::max(peak->temporary_disk_bytes, value.temporary_disk_bytes);
}

Result<std::uint64_t> tile_storage(std::uint32_t edge, std::uint32_t halo,
                                   std::uint64_t source_pixel_bytes,
                                   std::uint64_t output_pixel_bytes, std::uint32_t staging_depth) {
    std::uint64_t atlas_edge = 0;
    if (!add(edge, static_cast<std::uint64_t>(halo) * 2U, &atlas_edge))
        return overflow();
    std::uint64_t atlas_pixels = 0;
    std::uint64_t output_pixels = 0;
    std::uint64_t source_bytes = 0;
    std::uint64_t output_bytes = 0;
    if (!multiply(atlas_edge, atlas_edge, &atlas_pixels) || !multiply(edge, edge, &output_pixels) ||
        !multiply(atlas_pixels, source_pixel_bytes, &source_bytes) ||
        !multiply(output_pixels, output_pixel_bytes, &output_bytes) ||
        !multiply(output_bytes, static_cast<std::uint64_t>(staging_depth) + 2U, &output_bytes) ||
        !add(source_bytes, output_bytes, &source_bytes))
        return overflow();
    return source_bytes;
}

} // namespace

Result<void> ResourcePlan::fits(const ResourceBudgets& budgets) const {
    if (peak.private_memory_bytes > budgets.private_memory_bytes)
        return Status::error(ErrorCode::limit_exceeded,
                             "Resource plan requires " + std::to_string(peak.private_memory_bytes) +
                                 " private bytes; " + std::to_string(budgets.private_memory_bytes) +
                                 " are available.");
    if (peak.mapped_bytes > budgets.mapped_bytes)
        return Status::error(ErrorCode::limit_exceeded,
                             "Resource plan exceeds the mapped-storage budget.");
    if (peak.gpu_bytes > budgets.gpu_bytes)
        return Status::error(ErrorCode::limit_exceeded,
                             "Resource plan exceeds the GPU-memory budget.");
    if (peak.artifact_bytes > budgets.artifact_bytes)
        return Status::error(ErrorCode::limit_exceeded,
                             "Resource plan exceeds the artifact budget.");
    if (peak.temporary_disk_bytes > budgets.temporary_disk_bytes)
        return Status::error(ErrorCode::limit_exceeded,
                             "Resource plan exceeds the temporary-disk budget.");
    return {};
}

Result<ResourcePlan> plan_resources(const ResourcePlanRequest& request) {
    Result<void> source_status = request.source.validate();
    Result<void> output_status = request.output.validate();
    if (!source_status)
        return source_status.error();
    if (!output_status)
        return output_status.error();
    if (request.minimum_tile_edge == 0 || request.preferred_tile_edge == 0 ||
        request.minimum_tile_edge > request.preferred_tile_edge || request.staging_depth == 0 ||
        !std::isfinite(request.resampling_support) || request.resampling_support < 0.0)
        return Status::error(ErrorCode::invalid_argument,
                             "Resource plan tile parameters are invalid.");
    Result<std::uint64_t> source_bytes = raster_bytes(request.source);
    Result<std::uint64_t> output_bytes = raster_bytes(request.output);
    Result<std::uint64_t> source_row = maximum_row_bytes(request.source);
    Result<std::uint64_t> output_row = maximum_row_bytes(request.output);
    Result<std::uint64_t> source_pixel = maximum_pixel_bytes(request.source);
    Result<std::uint64_t> output_pixel = maximum_pixel_bytes(request.output);
    if (!source_bytes || !output_bytes || !source_row || !output_row || !source_pixel ||
        !output_pixel)
        return overflow();

    const double scale_x =
        static_cast<double>(request.source.canvas_width) / request.output.canvas_width;
    const double scale_y =
        static_cast<double>(request.source.canvas_height) / request.output.canvas_height;
    const double exact_halo =
        std::ceil(request.resampling_support * std::max({1.0, scale_x, scale_y})) + 1.0;
    const double exact_ring_rows_double =
        std::ceil(request.resampling_support * std::max(1.0, scale_y) * 2.0 + 1.0);
    if (exact_ring_rows_double > static_cast<double>(std::numeric_limits<std::uint64_t>::max()))
        return overflow();
    const std::uint64_t exact_ring_rows = static_cast<std::uint64_t>(exact_ring_rows_double);
    std::uint64_t resize_pixel_row = 0;
    std::uint64_t cached_rows = 0;
    std::uint64_t cached_index = 0;
    if (!multiply(request.output.canvas_width, 16U, &resize_pixel_row) ||
        !multiply(resize_pixel_row, exact_ring_rows, &cached_rows) ||
        !multiply(exact_ring_rows, 32U, &cached_index) ||
        !add(cached_rows, cached_index, &cached_rows))
        return overflow();
    const bool vertical_streaming = scale_y > 1.0 && cached_rows > kDefaultResizeWorkerCacheBytes;
    const bool streaming = exact_halo > request.preferred_tile_edge || vertical_streaming;
    const std::uint32_t bounded_halo =
        streaming ? static_cast<std::uint32_t>(std::ceil(request.resampling_support * 2.0) + 1.0)
                  : static_cast<std::uint32_t>(exact_halo);

    std::uint32_t tile = std::min(request.preferred_tile_edge, request.maximum_texture_size);
    if (tile < request.minimum_tile_edge)
        return Status::error(ErrorCode::limit_exceeded,
                             "Graphics backend texture limit is smaller than the minimum tile.");
    while (tile > request.minimum_tile_edge && (tile & (tile - 1U)) != 0)
        tile &= tile - 1U;
    Result<std::uint64_t> gpu = std::uint64_t{0};
    if (request.gpu_transform) {
        gpu = tile_storage(tile, bounded_halo, source_pixel.value(), output_pixel.value(),
                           request.staging_depth);
        while ((!gpu || gpu.value() > request.budgets.gpu_bytes) &&
               tile > request.minimum_tile_edge) {
            tile /= 2U;
            gpu = tile_storage(tile, bounded_halo, source_pixel.value(), output_pixel.value(),
                               request.staging_depth);
        }
        if (!gpu || gpu.value() > request.budgets.gpu_bytes)
            return Status::error(ErrorCode::limit_exceeded,
                                 "Minimum transform tile exceeds the GPU budget.");
    }

    ResourcePlan result;
    result.tile_width = tile;
    result.tile_height = tile;
    result.staging_depth = request.staging_depth;
    result.streaming_polyphase = streaming;

    ResourceFootprint inspect;
    inspect.private_memory_bytes = std::uint64_t{2} << 20U;
    result.phases.push_back({ResourcePhase::inspect, inspect});

    ResourceFootprint decode;
    if (!multiply(source_row.value(), 4U, &decode.private_memory_bytes) ||
        !add(decode.private_memory_bytes, std::uint64_t{8} << 20U, &decode.private_memory_bytes))
        return overflow();
    decode.temporary_disk_bytes = source_bytes.value();
    result.phases.push_back({ResourcePhase::decode, decode});

    ResourceFootprint transform;
    std::uint64_t tile_pixels = 0;
    std::uint64_t row_working = 0;
    std::uint64_t gpu_staging = 0;
    if (!multiply(tile, tile, &tile_pixels) ||
        !multiply(tile_pixels, output_pixel.value(), &gpu_staging) ||
        !multiply(gpu_staging, request.staging_depth, &gpu_staging) ||
        !multiply(std::max(source_row.value(), output_row.value()), 16U, &row_working))
        return overflow();
    std::uint64_t per_thread = 0;
    if (vertical_streaming) {
        const std::uint64_t active_rows =
            static_cast<std::uint64_t>(std::ceil(request.resampling_support * 2.0 + 2.0));
        std::uint64_t coefficient_count = 0;
        std::uint64_t coefficient_peak = 0;
        std::uint64_t source_offsets = 0;
        if (!multiply(request.output.canvas_height, exact_ring_rows, &coefficient_count) ||
            !multiply(coefficient_count, 16U, &coefficient_peak) ||
            !multiply(static_cast<std::uint64_t>(request.source.canvas_height) + 1U,
                      sizeof(std::size_t), &source_offsets) ||
            !add(coefficient_peak, source_offsets, &coefficient_peak) ||
            !multiply(resize_pixel_row, active_rows + 1U, &per_thread) ||
            !add(per_thread, coefficient_peak, &per_thread))
            return overflow();
    } else {
        if (!add(cached_rows, resize_pixel_row, &per_thread))
            return overflow();
    }
    if (!add(row_working, std::uint64_t{8} << 20U, &row_working))
        return overflow();
    const std::uint32_t hardware = std::max(1U, std::thread::hardware_concurrency());
    const std::uint32_t requested_threads = request.maximum_cpu_threads == 0
                                                ? hardware
                                                : std::min(hardware, request.maximum_cpu_threads);
    result.cpu_threads = std::min(requested_threads, request.output.canvas_height);
    result.cpu_threads = std::max(1U, result.cpu_threads);
    if (vertical_streaming)
        result.cpu_threads = 1;
    for (;;) {
        std::uint64_t rings = 0;
        std::uint64_t cpu_working = 0;
        if (!multiply(per_thread, result.cpu_threads, &rings) ||
            !add(row_working, rings, &cpu_working))
            return overflow();
        transform.private_memory_bytes =
            std::max(cpu_working, request.gpu_transform ? gpu_staging : 0U);
        if (transform.private_memory_bytes <= request.budgets.private_memory_bytes ||
            result.cpu_threads == 1)
            break;
        --result.cpu_threads;
    }
    if (transform.private_memory_bytes > request.budgets.private_memory_bytes)
        return Status::error(
            ErrorCode::limit_exceeded,
            "Minimum CPU transform working set exceeds the private-memory budget.");
    transform.mapped_bytes = output_bytes.value();
    transform.gpu_bytes = request.gpu_transform ? gpu.value() : 0;
    if (!add(source_bytes.value(), output_bytes.value(), &transform.temporary_disk_bytes))
        return overflow();
    result.phases.push_back({ResourcePhase::transform, transform});

    ResourceFootprint encode;
    const std::uint64_t artifact_bytes = request.expected_artifact_bytes == 0
                                             ? output_bytes.value()
                                             : request.expected_artifact_bytes;
    if (request.output.format == Format::webp) {
        const bool animation = request.output.frames.size() > 1;
        std::uint64_t metadata_bytes = request.output.color.icc_profile.size();
        if (!add(metadata_bytes, request.output.metadata.exif.size(), &metadata_bytes) ||
            !add(metadata_bytes, request.output.metadata.xmp.size(), &metadata_bytes))
            return overflow();
        const auto picture_state = webp_picture_storage(request, output_bytes.value());
        if (!picture_state)
            return picture_state.error();
        encode.private_memory_bytes = picture_state.value();
        if (animation || (request.encode_options.preserve_metadata && metadata_bytes != 0)) {
            std::uint64_t assembly = 0;
            if (!multiply(artifact_bytes, 2U, &assembly) ||
                !add(assembly, metadata_bytes, &assembly) ||
                !add(encode.private_memory_bytes, assembly, &encode.private_memory_bytes))
                return overflow();
        }
    } else {
        const std::uint64_t encoder_rows = request.output.format == Format::jpeg ? 16U : 256U;
        if (!multiply(output_row.value(), encoder_rows, &encode.private_memory_bytes))
            return overflow();
        encode.private_memory_bytes =
            std::max(encode.private_memory_bytes,
                     request.output.format == Format::jpeg
                         ? (std::uint64_t{2} << 20U) + (std::uint64_t{256} << 10U)
                         : std::uint64_t{32} << 20U);
    }
    if (request.output.format == Format::avif || request.output.format == Format::heif) {
        std::uint64_t pixel_scaled = 0;
        if (!multiply(output_bytes.value(), 10U, &pixel_scaled) ||
            !add(pixel_scaled, std::uint64_t{64} << 20U, &pixel_scaled))
            return overflow();
        // Native row input still builds full RGB and codec YCbCr images. Match
        // estimate_encode_resources(), including high-depth AVIF conversion.
        encode.private_memory_bytes = std::max(encode.private_memory_bytes, pixel_scaled);
    }
    if (request.output.format == Format::jxl) {
        std::uint64_t pixel_scaled = 0;
        if (!multiply(output_bytes.value(), 16U, &pixel_scaled) ||
            !add(pixel_scaled, std::uint64_t{64} << 20U, &pixel_scaled))
            return overflow();
        // Keep the phase plan consistent with estimate_encode_resources(): libjxl
        // needs both a substantial fixed working set and pixel-scaled headroom.
        encode.private_memory_bytes =
            std::max({encode.private_memory_bytes, pixel_scaled, std::uint64_t{640} << 20U});
    }
    encode.mapped_bytes = output_bytes.value();
    encode.artifact_bytes = artifact_bytes;
    if (!add(source_bytes.value(), output_bytes.value(), &encode.temporary_disk_bytes) ||
        !add(encode.temporary_disk_bytes, artifact_bytes, &encode.temporary_disk_bytes))
        return overflow();
    result.phases.push_back({ResourcePhase::encode, encode});

    ResourceFootprint validate;
    validate.private_memory_bytes = request.output.format == Format::webp
                                        ? (std::uint64_t{4} << 20U)
                                        : (std::uint64_t{2} << 20U);
    validate.artifact_bytes = artifact_bytes;
    validate.temporary_disk_bytes = artifact_bytes;
    result.phases.push_back({ResourcePhase::validate, validate});

    ResourceFootprint preview;
    preview.private_memory_bytes =
        transform.private_memory_bytes / std::max<std::uint32_t>(1, request.staging_depth);
    preview.mapped_bytes = output_bytes.value();
    preview.artifact_bytes = artifact_bytes;
    preview.temporary_disk_bytes = encode.temporary_disk_bytes;
    if (request.output.format == Format::webp) {
        std::uint64_t decoded_canvas = 0;
        if (!multiply(static_cast<std::uint64_t>(request.output.canvas_width),
                      request.output.canvas_height, &decoded_canvas) ||
            !multiply(decoded_canvas, 4U, &decoded_canvas) ||
            !add(decoded_canvas, std::uint64_t{16} << 20U, &decoded_canvas))
            return overflow();
        preview.private_memory_bytes = std::max(preview.private_memory_bytes, decoded_canvas);
    }
    result.phases.push_back({ResourcePhase::preview, preview});

    for (const PhaseResourcePlan& phase : result.phases)
        include_peak(&result.peak, phase.footprint);
    Result<void> fit = result.fits(request.budgets);
    if (!fit)
        return fit.error();
    return result;
}

} // namespace snow::image
