#pragma once

#include "snow/image/export.h"
#include "snow/image/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>

namespace snow::image {

namespace detail {
struct AlphaAnalysisState;
}

enum class SampleType : std::uint8_t {
    unsigned_integer,
    signed_integer,
    floating_point,
};

enum class ChannelLayout : std::uint8_t {
    gray,
    gray_alpha,
    rgb,
    rgba,
    bgr,
    bgra,
    cmyk,
    indexed,
};

enum class AlphaMode : std::uint8_t {
    none,
    straight,
    premultiplied,
};

struct PixelFormat final {
    SampleType sample_type = SampleType::unsigned_integer;
    ChannelLayout channels = ChannelLayout::rgba;
    AlphaMode alpha = AlphaMode::straight;
    std::uint8_t bits_per_channel = 8;
    bool little_endian = true;

    [[nodiscard]] std::uint32_t channel_count() const noexcept;
    [[nodiscard]] Result<std::size_t> bytes_per_pixel() const;

    friend bool operator==(const PixelFormat&, const PixelFormat&) = default;
};

struct ImageView final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format;
    std::size_t row_stride = 0;
    std::span<const std::byte> pixels;

    [[nodiscard]] Result<void> validate() const;
};

struct MutableImageView final {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format;
    std::size_t row_stride = 0;
    std::span<std::byte> pixels;

    [[nodiscard]] Result<void> validate() const;
    [[nodiscard]] ImageView as_const() const noexcept;
};

class MutableImage;

class SNOW_IMAGE_API SharedPixelBuffer final {
  public:
    SharedPixelBuffer() = default;

    [[nodiscard]] static Result<SharedPixelBuffer> adopt(std::shared_ptr<const void> owner,
                                                         std::span<const std::byte> pixels);

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] const std::byte* data() const noexcept {
        return data_;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        return {data_, size_};
    }
    [[nodiscard]] const std::shared_ptr<const void>& owner() const noexcept {
        return owner_;
    }

  private:
    SharedPixelBuffer(std::shared_ptr<const void> owner, const std::byte* data,
                      std::size_t size) noexcept;

    std::shared_ptr<const void> owner_;
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;

    friend class MutableImage;
};

enum class AlphaContent : std::uint8_t { opaque, non_opaque };

class SNOW_IMAGE_API Image;
[[nodiscard]] SNOW_IMAGE_API Result<AlphaContent> classify_alpha(const Image& image,
                                                                 std::stop_token stop);

class SNOW_IMAGE_API Image final {
  public:
    Image() = default;

    [[nodiscard]] static Result<Image> adopt(std::uint32_t width, std::uint32_t height,
                                             PixelFormat format, std::size_t row_stride,
                                             SharedPixelBuffer pixels);

    [[nodiscard]] bool empty() const noexcept {
        return pixels_.empty();
    }
    [[nodiscard]] std::uint32_t width() const noexcept {
        return width_;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return height_;
    }
    [[nodiscard]] const PixelFormat& format() const noexcept {
        return format_;
    }
    [[nodiscard]] std::size_t row_stride() const noexcept {
        return row_stride_;
    }
    [[nodiscard]] std::span<const std::byte> pixels() const noexcept {
        return pixels_.span();
    }
    [[nodiscard]] const SharedPixelBuffer& storage() const noexcept {
        return pixels_;
    }
    [[nodiscard]] ImageView view() const noexcept;

  private:
    Image(std::uint32_t width, std::uint32_t height, PixelFormat format, std::size_t row_stride,
          SharedPixelBuffer pixels,
          std::shared_ptr<detail::AlphaAnalysisState> alpha_analysis) noexcept;

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    PixelFormat format_;
    std::size_t row_stride_ = 0;
    SharedPixelBuffer pixels_;
    std::shared_ptr<detail::AlphaAnalysisState> alpha_analysis_;

    friend class MutableImage;
    friend SNOW_IMAGE_API Result<AlphaContent> classify_alpha(const Image&, std::stop_token);
};

class SNOW_IMAGE_API MutableImage final {
  public:
    MutableImage() = default;
    MutableImage(MutableImage&&) noexcept = default;
    MutableImage& operator=(MutableImage&&) noexcept = default;
    MutableImage(const MutableImage&) = delete;
    MutableImage& operator=(const MutableImage&) = delete;

    [[nodiscard]] static Result<MutableImage> allocate(std::uint32_t width, std::uint32_t height,
                                                       PixelFormat format,
                                                       std::size_t row_alignment = 1);
    [[nodiscard]] static Result<MutableImage> copy(ImageView source, std::size_t row_alignment = 1);

    [[nodiscard]] bool empty() const noexcept {
        return pixels_.empty();
    }
    [[nodiscard]] std::uint32_t width() const noexcept {
        return width_;
    }
    [[nodiscard]] std::uint32_t height() const noexcept {
        return height_;
    }
    [[nodiscard]] const PixelFormat& format() const noexcept {
        return format_;
    }
    [[nodiscard]] std::size_t row_stride() const noexcept {
        return row_stride_;
    }
    [[nodiscard]] std::span<const std::byte> pixels() const noexcept {
        return pixels_.span();
    }
    [[nodiscard]] std::span<std::byte> pixels() noexcept {
        return {writable_, pixels_.size()};
    }
    [[nodiscard]] ImageView view() const noexcept;
    [[nodiscard]] MutableImageView mutable_view() noexcept;
    [[nodiscard]] Image freeze() && noexcept;

  private:
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    PixelFormat format_;
    std::size_t row_stride_ = 0;
    SharedPixelBuffer pixels_;
    std::byte* writable_ = nullptr;
    std::shared_ptr<detail::AlphaAnalysisState> alpha_analysis_;
};

inline constexpr PixelFormat kGray8{SampleType::unsigned_integer, ChannelLayout::gray,
                                    AlphaMode::none, 8, true};
inline constexpr PixelFormat kGray16{SampleType::unsigned_integer, ChannelLayout::gray,
                                     AlphaMode::none, 16, true};
inline constexpr PixelFormat kGrayAlpha8{SampleType::unsigned_integer, ChannelLayout::gray_alpha,
                                         AlphaMode::straight, 8, true};
inline constexpr PixelFormat kGrayAlpha16{SampleType::unsigned_integer, ChannelLayout::gray_alpha,
                                          AlphaMode::straight, 16, true};
inline constexpr PixelFormat kRgb8{SampleType::unsigned_integer, ChannelLayout::rgb,
                                   AlphaMode::none, 8, true};
inline constexpr PixelFormat kRgb16{SampleType::unsigned_integer, ChannelLayout::rgb,
                                    AlphaMode::none, 16, true};
inline constexpr PixelFormat kRgba8{SampleType::unsigned_integer, ChannelLayout::rgba,
                                    AlphaMode::straight, 8, true};
inline constexpr PixelFormat kBgra8{SampleType::unsigned_integer, ChannelLayout::bgra,
                                    AlphaMode::straight, 8, true};
inline constexpr PixelFormat kRgba16{SampleType::unsigned_integer, ChannelLayout::rgba,
                                     AlphaMode::straight, 16, true};
inline constexpr PixelFormat kRgba16Float{SampleType::floating_point, ChannelLayout::rgba,
                                          AlphaMode::straight, 16, true};
inline constexpr PixelFormat kRgba32Float{SampleType::floating_point, ChannelLayout::rgba,
                                          AlphaMode::straight, 32, true};

} // namespace snow::image
