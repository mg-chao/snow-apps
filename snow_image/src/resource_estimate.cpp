#include "snow/image/resource_estimate.h"
#include "resize_resource_bounds.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace snow::image {
namespace {

Result<std::uint64_t> checked_add(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return Status::error(ErrorCode::limit_exceeded,
                             "The resource estimate exceeds addressable limits.");
    }
    return left + right;
}

Result<std::uint64_t> checked_multiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Status::error(ErrorCode::limit_exceeded,
                             "The resource estimate exceeds addressable limits.");
    }
    return left * right;
}

Result<std::uint64_t> frame_bytes(std::uint32_t width, std::uint32_t height, PixelFormat format) {
    auto bytes_per_pixel = format.bytes_per_pixel();
    if (!bytes_per_pixel)
        return bytes_per_pixel.error();
    auto pixels = checked_multiply(width, height);
    if (!pixels)
        return pixels.error();
    return checked_multiply(pixels.value(), bytes_per_pixel.value());
}

Result<std::uint64_t> document_bytes(const DocumentInfo& document) {
    std::uint64_t total = 0;
    for (const FrameInfo& frame : document.frames) {
        auto bytes = frame_bytes(frame.width, frame.height, frame.native_format);
        if (!bytes)
            return bytes.error();
        auto sum = checked_add(total, bytes.value());
        if (!sum)
            return sum.error();
        total = sum.value();
    }
    return total;
}

DocumentInfo document_info(const Document& document) {
    DocumentInfo info;
    info.format = document.format;
    info.canvas_width = document.canvas_width;
    info.canvas_height = document.canvas_height;
    info.loop_count = document.loop_count;
    info.color = document.color;
    info.frames.reserve(document.frames.size());
    for (const Frame& frame : document.frames) {
        info.frames.push_back({frame.image.width(), frame.image.height(), frame.x, frame.y,
                               frame.duration, frame.image.format(),
                               frame.image.format().alpha != AlphaMode::none, frame.cursor_hotspot,
                               frame.color, frame.metadata, frame.blend, frame.disposal});
    }
    return info;
}

Result<std::uint64_t> scaled(std::uint64_t bytes, std::uint64_t factor, std::uint64_t headroom) {
    auto product = checked_multiply(bytes, factor);
    if (!product)
        return product.error();
    return checked_add(product.value(), headroom);
}

} // namespace

Result<std::uint64_t> ResourceEstimate::total_bytes() const {
    auto resident = checked_add(private_memory_bytes, mapped_bytes);
    if (!resident)
        return resident.error();
    return checked_add(resident.value(), output_bytes);
}

Result<ResourceEstimate> estimate_decode_resources(const DocumentInfo& document,
                                                   const DecodeOptions& options) {
    auto output = document_bytes(document);
    if (!output)
        return output.error();
    if (output.value() > options.limits.maximum_owned_output_bytes) {
        return Status::error(ErrorCode::limit_exceeded,
                             "Decoded output exceeds the configured resource limit.");
    }
    auto private_memory = scaled(output.value(), 2, std::uint64_t{8} << 20U);
    if (!private_memory)
        return private_memory.error();
    return ResourceEstimate{private_memory.value(), 0, output.value()};
}

Result<ResizeResourceEstimate> estimate_resize_resources(std::uint32_t source_width,
                                                         std::uint32_t source_height,
                                                         PixelFormat source_format,
                                                         const ResizeOptions& options) {
    if (source_width == 0 || source_height == 0 || options.width == 0 || options.height == 0 ||
        source_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        source_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        options.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        options.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        std::uint64_t{options.width} * options.height > (std::uint64_t{1} << 32U))
        return Status::error(ErrorCode::invalid_argument, "Resize dimensions are invalid.");
    auto pixel_bytes = source_format.bytes_per_pixel();
    if (!pixel_bytes)
        return pixel_bytes.error();
    constexpr std::uint64_t headroom = std::uint64_t{8} << 20U;
    if (source_width == options.width && source_height == options.height)
        return ResizeResourceEstimate{0, headroom};
    const bool nearest = options.method == ResamplingMethod::nearest;
    const double support = nearest ? .5 : options.method == ResamplingMethod::linear ? 1.0 : 3.0;
    auto horizontal = detail::resize_axis_resources(source_width, options.width, support, nearest);
    auto vertical = detail::resize_axis_resources(source_height, options.height, support, nearest);
    if (!horizontal || !vertical)
        return !horizontal ? horizontal.error() : vertical.error();
    auto retained = checked_add(horizontal.value().retained_bytes, vertical.value().retained_bytes);
    if (!retained)
        return retained.error();
    std::uint64_t working =
        std::max(horizontal.value().construction_bytes, vertical.value().construction_bytes);
    auto source_row = checked_multiply(source_width, pixel_bytes.value());
    auto pixel_row = checked_multiply(options.width, sizeof(float) * 4U);
    auto ring = checked_multiply(pixel_row ? pixel_row.value() : 0, vertical.value().maximum_count);
    auto index = checked_multiply(vertical.value().maximum_count, 64U);
    if (!source_row || !pixel_row || !ring || !index)
        return !source_row  ? source_row.error()
               : !pixel_row ? pixel_row.error()
               : !ring      ? ring.error()
                            : index.error();
    auto cached = checked_add(ring.value(), index.value());
    if (!cached)
        return cached.error();
    const bool bounded =
        options.maximum_worker_cache_bytes != std::numeric_limits<std::uint64_t>::max();
    const bool possible_streaming =
        bounded && source_height > options.height &&
        (vertical.value().maximum_count > kDefaultResizeMaximumCachedRows ||
         cached.value() > options.maximum_worker_cache_bytes);
    if (possible_streaming) {
        // The implementation can choose either path near a filter-support boundary. Budget the
        // larger retained representation and both possible read peaks, rather than guessing which
        // coefficients vanish at that boundary.
        auto inverted_entries = checked_multiply(vertical.value().coefficient_count, 8U);
        auto source_offsets =
            checked_multiply(static_cast<std::uint64_t>(source_height) + 1U, sizeof(std::size_t));
        auto target_ranges = checked_multiply(options.height, sizeof(int) * 2U);
        if (!inverted_entries || !source_offsets || !target_ranges)
            return !inverted_entries ? inverted_entries.error()
                   : !source_offsets ? source_offsets.error()
                                     : target_ranges.error();
        auto inverted = checked_add(inverted_entries.value(), source_offsets.value());
        if (!inverted)
            return inverted.error();
        inverted = checked_add(inverted.value(), target_ranges.value());
        if (!inverted)
            return inverted.error();
        retained = checked_add(horizontal.value().retained_bytes,
                               std::max(vertical.value().retained_bytes, inverted.value()));
        if (!retained)
            return retained.error();
        auto inversion_scratch = checked_multiply(source_offsets.value(), 2U);
        if (!inversion_scratch)
            return inversion_scratch.error();
        inversion_scratch = checked_add(
            inversion_scratch.value(), std::min(vertical.value().retained_bytes, inverted.value()));
        if (!inversion_scratch)
            return inversion_scratch.error();
        working = std::max(working, inversion_scratch.value());

        const auto active_rows = static_cast<std::uint64_t>(std::ceil(support * 2.0 + 2.0));
        auto accumulators = checked_multiply(pixel_row.value(), active_rows + 1U);
        auto target_slots = checked_multiply(options.height, sizeof(std::size_t));
        auto free_slots = checked_multiply(active_rows, sizeof(std::size_t));
        if (!accumulators || !target_slots || !free_slots)
            return !accumulators   ? accumulators.error()
                   : !target_slots ? target_slots.error()
                                   : free_slots.error();
        auto streaming = checked_add(accumulators.value(), source_row.value());
        if (!streaming)
            return streaming.error();
        streaming = checked_add(streaming.value(), target_slots.value());
        if (!streaming)
            return streaming.error();
        streaming = checked_add(streaming.value(), free_slots.value());
        if (!streaming)
            return streaming.error();
        working = std::max(working, streaming.value());

        // A cached path admits only its ring plus the implementation's 32-byte index allowance.
        // Our 64-byte index bound additionally covers nodes, buckets, row tags and row pointers.
        auto maximum_index = checked_multiply(
            std::min(vertical.value().maximum_count, kDefaultResizeMaximumCachedRows), 64U);
        auto budgeted_cache = checked_add(options.maximum_worker_cache_bytes,
                                          maximum_index ? maximum_index.value() : 0);
        auto maximum_ring =
            checked_multiply(pixel_row.value(), std::min(vertical.value().maximum_count,
                                                         kDefaultResizeMaximumCachedRows));
        if (!maximum_index || !budgeted_cache || !maximum_ring)
            return !maximum_index    ? maximum_index.error()
                   : !budgeted_cache ? budgeted_cache.error()
                                     : maximum_ring.error();
        auto row_limited = checked_add(maximum_ring.value(), maximum_index.value());
        if (!row_limited)
            return row_limited.error();
        cached = std::min({cached.value(), budgeted_cache.value(), row_limited.value()});
    }
    auto per_worker = checked_add(cached.value(), source_row.value());
    if (!per_worker)
        return per_worker.error();
    per_worker = checked_add(per_worker.value(), pixel_row.value());
    if (!per_worker)
        return per_worker.error();
    const auto hardware = std::max(1U, std::thread::hardware_concurrency());
    const auto threads = std::clamp(
        options.maximum_threads == 0 ? hardware : std::min(hardware, options.maximum_threads), 1U,
        options.height);
    auto read_working = checked_multiply(per_worker.value(), threads);
    if (!read_working)
        return read_working.error();
    working = std::max(working, read_working.value());
    auto total_working = checked_add(working, headroom);
    if (!total_working)
        return total_working.error();
    return ResizeResourceEstimate{retained.value(), total_working.value()};
}

Result<ResourceEstimate> estimate_transform_resources(const DocumentInfo& document,
                                                      const TransformOptions& options) {
    std::uint64_t output = 0;
    std::uint64_t row_working = 0;
    const std::size_t frame_count = options.animation_policy == AnimationPolicy::first_frame
                                        ? std::min<std::size_t>(1, document.frames.size())
                                        : document.frames.size();
    for (std::size_t index = 0; index < frame_count; ++index) {
        const FrameInfo& frame = document.frames[index];
        const std::uint32_t width = options.resize ? options.resize->width : frame.width;
        const std::uint32_t height = options.resize ? options.resize->height : frame.height;
        auto bytes = frame_bytes(width, height, frame.native_format);
        if (!bytes)
            return bytes.error();
        auto sum = checked_add(output, bytes.value());
        if (!sum)
            return sum.error();
        output = sum.value();
        auto bytes_per_pixel = frame.native_format.bytes_per_pixel();
        if (!bytes_per_pixel)
            return bytes_per_pixel.error();
        auto row = checked_multiply(std::max(frame.width, width), bytes_per_pixel.value() * 16U);
        if (!row)
            return row.error();
        std::uint64_t working = row.value();
        if (options.resize) {
            auto resize = estimate_resize_resources(frame.width, frame.height, frame.native_format,
                                                    *options.resize);
            if (!resize)
                return resize.error();
            auto combined =
                checked_add(resize.value().retained_bytes, resize.value().working_bytes);
            if (!combined)
                return combined.error();
            // The resize estimate already includes its private-memory headroom.
            working = combined.value() - (std::uint64_t{8} << 20U);
        }
        row_working = std::max(row_working, working);
    }
    auto private_memory = checked_add(row_working, std::uint64_t{8} << 20U);
    if (!private_memory)
        return private_memory.error();
    return ResourceEstimate{private_memory.value(), output, 0};
}

Result<ResourceEstimate> estimate_export_resources(const DocumentInfo& document, Format format,
                                                   bool reduce_palette) {
    auto raw = document_bytes(document);
    if (!raw)
        return raw.error();
    const bool high_precision = format == Format::avif || format == Format::jxl;
    const std::uint64_t conversion_factor = high_precision ? 2U : 1U;
    auto mapped = checked_multiply(raw.value(), conversion_factor);
    if (!mapped)
        return mapped.error();
    auto private_memory = scaled(mapped.value(), reduce_palette ? 2U : 1U, std::uint64_t{8} << 20U);
    if (!private_memory)
        return private_memory.error();
    return ResourceEstimate{private_memory.value(), mapped.value(), 0};
}

Result<ResourceEstimate> estimate_preview_decode_resources(const DocumentInfo& document) {
    auto raw = document_bytes(document);
    if (!raw)
        return raw.error();
    auto private_memory = scaled(raw.value(), 2, std::uint64_t{8} << 20U);
    if (!private_memory)
        return private_memory.error();
    return ResourceEstimate{private_memory.value(), raw.value(), 0};
}

Result<ResourceEstimate> estimate_encode_resources(const Document& document,
                                                   const EncodeOptions& options) {
    const DocumentInfo info = document_info(document);
    auto raw = document_bytes(info);
    if (!raw)
        return raw.error();
    std::uint64_t private_factor = 4;
    std::uint64_t runtime_headroom = std::uint64_t{16} << 20U;
    std::uint64_t private_floor = 0;
    switch (options.format) {
    case Format::jpeg:
        private_factor = 0;
        runtime_headroom = (std::uint64_t{2} << 20U) + (std::uint64_t{256} << 10U);
        break;
    case Format::png:
        private_factor = 2;
        break;
    case Format::tiff:
        private_factor = 0;
        runtime_headroom = std::uint64_t{16} << 20U;
        if (!document.frames.empty()) {
            const auto& image = document.frames.front().image;
            auto rows = checked_multiply(image.width(), image.format().channel_count() * 2U);
            if (!rows)
                return rows.error();
            auto total = checked_add(runtime_headroom, rows.value());
            if (!total)
                return total.error();
            runtime_headroom = total.value();
        }
        break;
    case Format::webp:
        private_factor = 6;
        runtime_headroom = std::uint64_t{32} << 20U;
        break;
    case Format::heif:
    case Format::avif:
        private_factor = 10;
        runtime_headroom = std::uint64_t{64} << 20U;
        break;
    case Format::jxl:
        private_factor = 16;
        runtime_headroom = std::uint64_t{64} << 20U;
        // libjxl has a substantial fixed encoder working set, especially in Debug
        // builds. A purely pixel-scaled estimate under-admits small images.
        private_floor = std::uint64_t{640} << 20U;
        break;
    default:
        break;
    }
    auto private_memory = scaled(raw.value(), private_factor, runtime_headroom);
    if (!private_memory)
        return private_memory.error();
    if (options.format == Format::png && !document.frames.empty()) {
        const PixelFormat& format = document.frames.front().image.format();
        const bool palette_eligible =
            format.sample_type == SampleType::unsigned_integer && format.bits_per_channel == 8 &&
            (format.channels == ChannelLayout::rgb || format.channels == ChannelLayout::rgba ||
             format.channels == ChannelLayout::bgr || format.channels == ChannelLayout::bgra);
        if (palette_eligible) {
            auto pixels = checked_multiply(document.frames.front().image.width(),
                                           document.frames.front().image.height());
            if (!pixels)
                return pixels.error();
            // The automatic palette probe owns one byte per candidate pixel and a
            // fixed lookup table, even when the 257th color rejects the conversion.
            // PaletteSlot contains a 32-bit key and two one-byte fields.  Its
            // alignment rounds the slot to eight bytes on the supported ABIs; use
            // that padded size so the plan covers the actual fixed table.
            auto table = checked_multiply(512U, 8U);
            if (!table)
                return table.error();
            auto palette = checked_multiply(256U, sizeof(std::uint32_t));
            if (!palette)
                return palette.error();
            auto extra = checked_add(pixels.value(), table.value());
            if (!extra)
                return extra.error();
            extra = checked_add(extra.value(), palette.value());
            if (!extra)
                return extra.error();
            private_memory = checked_add(private_memory.value(), extra.value());
            if (!private_memory)
                return private_memory.error();
        }
    }
    // A raw-size bound plus container overhead is conservative for the editor codecs.
    auto output = checked_add(raw.value(), std::uint64_t{1} << 20U);
    if (!output)
        return output.error();
    if (options.format == Format::tiff) {
        std::uint64_t metadata = 0;
        if (options.preserve_metadata) {
            auto total =
                checked_add(document.color.icc_profile.size(), document.metadata.xmp.size());
            if (!total)
                return total.error();
            total = checked_add(total.value(), document.metadata.comment.size());
            if (!total)
                return total.error();
            metadata = total.value();
        }
        auto artifact = checked_add(output.value(), raw.value() / 100U);
        if (!artifact)
            return artifact.error();
        artifact = checked_add(artifact.value(), metadata);
        if (!artifact)
            return artifact.error();
        output = artifact.value();
        auto metadata_working = checked_multiply(metadata, 2U);
        if (!metadata_working)
            return metadata_working.error();
        private_memory = checked_add(private_memory.value(), metadata_working.value());
        if (!private_memory)
            return private_memory.error();
    }
    return ResourceEstimate{std::max(private_memory.value(), private_floor), 0, output.value()};
}

} // namespace snow::image
