#include "snow/image/image.h"

#include "alpha_analysis.h"
#include <snow/memory/pixel_buffer.h>

#include <algorithm>
#include <limits>
#include <new>
#include <vector>

namespace snow::image {
namespace {

Result<std::size_t> checked_multiply(std::size_t left, std::size_t right,
                                     std::string_view description) {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return Status::error(ErrorCode::limit_exceeded,
                             std::string(description) + " exceeds the addressable size.");
    }
    return left * right;
}

Result<void> validate_view(std::uint32_t width, std::uint32_t height, const PixelFormat& format,
                           std::size_t row_stride, std::size_t buffer_size) {
    if (width == 0 || height == 0) {
        return Status::error(ErrorCode::invalid_argument, "Image dimensions must be non-zero.");
    }
    const Result<std::size_t> pixel_bytes = format.bytes_per_pixel();
    if (!pixel_bytes) {
        return pixel_bytes.error();
    }
    const Result<std::size_t> minimum_row =
        checked_multiply(static_cast<std::size_t>(width), pixel_bytes.value(), "Image row size");
    if (!minimum_row) {
        return minimum_row.error();
    }
    if (row_stride < minimum_row.value()) {
        return Status::error(ErrorCode::invalid_argument,
                             "Image row stride is smaller than one packed row.");
    }
    const Result<std::size_t> required =
        checked_multiply(row_stride, static_cast<std::size_t>(height), "Image buffer size");
    if (!required) {
        return required.error();
    }
    if (buffer_size < required.value()) {
        return Status::error(ErrorCode::invalid_argument,
                             "Image buffer is smaller than its declared dimensions and stride.");
    }
    return {};
}

std::shared_ptr<detail::AlphaAnalysisState> make_alpha_analysis(const PixelFormat& format) {
    auto analysis = std::make_shared<detail::AlphaAnalysisState>();
    const bool alpha_layout = format.channels == ChannelLayout::gray_alpha ||
                              format.channels == ChannelLayout::rgba ||
                              format.channels == ChannelLayout::bgra;
    if (format.alpha == AlphaMode::none || !alpha_layout) {
        analysis->value.store(detail::AlphaAnalysisValue::opaque, std::memory_order_relaxed);
    }
    return analysis;
}

} // namespace

std::uint32_t PixelFormat::channel_count() const noexcept {
    switch (channels) {
    case ChannelLayout::gray:
    case ChannelLayout::indexed:
        return 1;
    case ChannelLayout::gray_alpha:
        return 2;
    case ChannelLayout::rgb:
    case ChannelLayout::bgr:
        return 3;
    case ChannelLayout::rgba:
    case ChannelLayout::bgra:
    case ChannelLayout::cmyk:
        return 4;
    }
    return 0;
}

Result<std::size_t> PixelFormat::bytes_per_pixel() const {
    if (bits_per_channel == 0 || bits_per_channel % 8 != 0) {
        return Status::error(ErrorCode::unsupported_feature,
                             "Packed image buffers require whole-byte channel samples.");
    }
    const std::uint32_t channels_count = channel_count();
    if (channels_count == 0) {
        return Status::error(ErrorCode::invalid_argument, "Pixel format has no channels.");
    }
    return static_cast<std::size_t>(channels_count) * (bits_per_channel / 8U);
}

Result<void> ImageView::validate() const {
    return validate_view(width, height, format, row_stride, pixels.size());
}

Result<void> MutableImageView::validate() const {
    return validate_view(width, height, format, row_stride, pixels.size());
}

ImageView MutableImageView::as_const() const noexcept {
    return {width, height, format, row_stride, pixels};
}

SharedPixelBuffer::SharedPixelBuffer(std::shared_ptr<const void> owner, const std::byte* data,
                                     std::size_t size) noexcept
    : owner_(std::move(owner)), data_(data), size_(size) {}

Result<SharedPixelBuffer> SharedPixelBuffer::adopt(std::shared_ptr<const void> owner,
                                                   std::span<const std::byte> pixels) {
    if (pixels.empty())
        return SharedPixelBuffer{};
    if (!owner || !pixels.data()) {
        return Status::error(ErrorCode::invalid_argument,
                             "An adopted pixel buffer requires an owner and data.");
    }
    return SharedPixelBuffer(std::move(owner), pixels.data(), pixels.size());
}

Image::Image(std::uint32_t width, std::uint32_t height, PixelFormat format, std::size_t row_stride,
             SharedPixelBuffer pixels,
             std::shared_ptr<detail::AlphaAnalysisState> alpha_analysis) noexcept
    : width_(width), height_(height), format_(format), row_stride_(row_stride),
      pixels_(std::move(pixels)), alpha_analysis_(std::move(alpha_analysis)) {}

Result<Image> Image::adopt(std::uint32_t width, std::uint32_t height, PixelFormat format,
                           std::size_t row_stride, SharedPixelBuffer pixels) {
    Result<void> valid = validate_view(width, height, format, row_stride, pixels.size());
    if (!valid)
        return valid.error();
    try {
        return Image(width, height, format, row_stride, std::move(pixels),
                     make_alpha_analysis(format));
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate image analysis state.");
    }
}

Result<MutableImage> MutableImage::allocate(std::uint32_t width, std::uint32_t height,
                                            PixelFormat format, std::size_t row_alignment) {
    if (width == 0 || height == 0 || row_alignment == 0 ||
        (row_alignment & (row_alignment - 1U)) != 0) {
        return Status::error(ErrorCode::invalid_argument,
                             "Image dimensions and power-of-two row alignment must be valid.");
    }
    const Result<std::size_t> pixel_bytes = format.bytes_per_pixel();
    if (!pixel_bytes) {
        return pixel_bytes.error();
    }
    const Result<std::size_t> packed_row =
        checked_multiply(static_cast<std::size_t>(width), pixel_bytes.value(), "Image row size");
    if (!packed_row) {
        return packed_row.error();
    }
    if (packed_row.value() > std::numeric_limits<std::size_t>::max() - (row_alignment - 1U)) {
        return Status::error(ErrorCode::limit_exceeded, "Aligned image row size overflows.");
    }
    const std::size_t row_stride =
        (packed_row.value() + row_alignment - 1U) & ~(row_alignment - 1U);
    const Result<std::size_t> allocation_size =
        checked_multiply(row_stride, static_cast<std::size_t>(height), "Image allocation");
    if (!allocation_size) {
        return allocation_size.error();
    }

    try {
        auto allocation = snow::memory::allocatePixelBuffer(allocation_size.value());
        if (!allocation)
            return Status::error(ErrorCode::out_of_memory, "Could not allocate the image buffer.");
        MutableImage image;
        image.width_ = width;
        image.height_ = height;
        image.format_ = format;
        image.row_stride_ = row_stride;
        image.writable_ = reinterpret_cast<std::byte*>(allocation.get());
        image.pixels_ = SharedPixelBuffer(std::shared_ptr<const void>(std::move(allocation)),
                                          image.writable_, allocation_size.value());
        image.alpha_analysis_ = make_alpha_analysis(format);
        return image;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Could not allocate the image buffer.");
    }
}

ImageView Image::view() const noexcept {
    return {width_, height_, format_, row_stride_, pixels_.span()};
}

Result<MutableImage> MutableImage::copy(ImageView source, std::size_t row_alignment) {
    Result<void> valid = source.validate();
    if (!valid)
        return valid.error();
    Result<MutableImage> allocated =
        allocate(source.width, source.height, source.format, row_alignment);
    if (!allocated)
        return allocated.error();
    MutableImage image = std::move(allocated).value();
    Result<std::size_t> pixel_bytes = source.format.bytes_per_pixel();
    if (!pixel_bytes)
        return pixel_bytes.error();
    const std::size_t row_bytes = static_cast<std::size_t>(source.width) * pixel_bytes.value();
    for (std::uint32_t row = 0; row < source.height; ++row) {
        std::copy_n(source.pixels.data() + static_cast<std::size_t>(row) * source.row_stride,
                    row_bytes,
                    image.pixels().data() + static_cast<std::size_t>(row) * image.row_stride());
    }
    return image;
}

ImageView MutableImage::view() const noexcept {
    return {width_, height_, format_, row_stride_, pixels_.span()};
}

MutableImageView MutableImage::mutable_view() noexcept {
    return {width_, height_, format_, row_stride_, pixels()};
}

Image MutableImage::freeze() && noexcept {
    Image image(width_, height_, format_, row_stride_, std::move(pixels_),
                std::move(alpha_analysis_));
    width_ = 0;
    height_ = 0;
    row_stride_ = 0;
    writable_ = nullptr;
    return image;
}

} // namespace snow::image
