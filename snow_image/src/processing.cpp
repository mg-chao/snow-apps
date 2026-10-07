#include "snow/image/processing.h"

#include <snow/memory/pixel_array.h>

#include "alpha_analysis.h"

#include "snow/image/codec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <numeric>
#include <mutex>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#define SNOW_IMAGE_HAS_SSE2 1
#endif

namespace snow::image {
namespace {

struct Pixel final {
    float red = 0.0F;
    float green = 0.0F;
    float blue = 0.0F;
    float alpha = 0.0F;
};

float srgb_to_linear(float value) {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

float linear_to_srgb(float value) {
    value = std::max(value, 0.0F);
    return value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
}

float clamp_unit(float value) {
    return std::clamp(value, 0.0F, 1.0F);
}

constexpr float normalized_byte(std::uint8_t value) noexcept {
    return static_cast<float>(value) / 255.0F;
}

float linear_byte(std::byte value) {
    // Packed 8-bit samples have only 256 possible transfer-function inputs.
    // Use the same float operations as the scalar path, once per input value.
    static const auto values = [] {
        std::array<float, 256> result{};
        for (std::size_t index = 0; index < result.size(); ++index)
            result[index] = srgb_to_linear(normalized_byte(static_cast<std::uint8_t>(index)));
        return result;
    }();
    return values[std::to_integer<std::uint8_t>(value)];
}

Result<void> validate_processable_format(const PixelFormat& format) {
    if ((format.sample_type != SampleType::unsigned_integer ||
         (format.bits_per_channel != 8 && format.bits_per_channel != 16)) &&
        format.sample_type != SampleType::floating_point) {
        return Status::error(ErrorCode::unsupported_feature,
                             "Image processing does not support this sample type.");
    }
    if (format.channels == ChannelLayout::cmyk || format.channels == ChannelLayout::indexed) {
        return Status::error(ErrorCode::unsupported_feature,
                             "Image processing does not support this channel layout.");
    }
    return {};
}

Result<void> validate_processable(const ImageView& view) {
    Result<void> valid = view.validate();
    return valid ? validate_processable_format(view.format) : valid;
}

Pixel read_pixel(const ImageView& view, std::uint32_t x, std::uint32_t y, bool linear_rgb) {
    const PixelFormat& format = view.format;
    const std::uint32_t channels = format.channel_count();
    const bool bgr =
        format.channels == ChannelLayout::bgr || format.channels == ChannelLayout::bgra;
    const bool gray =
        format.channels == ChannelLayout::gray || format.channels == ChannelLayout::gray_alpha;
    const bool has_alpha = format.alpha != AlphaMode::none &&
                           (format.channels == ChannelLayout::gray_alpha || channels == 4U);
    const std::size_t sample_bytes = format.bits_per_channel / 8U;
    const std::byte* source = view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride +
                              static_cast<std::size_t>(x) * channels * sample_bytes;
    const auto sample = [&](std::uint32_t channel) {
        if (format.sample_type == SampleType::floating_point && format.bits_per_channel == 32) {
            float result = 0.0F;
            std::memcpy(&result, source + static_cast<std::size_t>(channel) * 4U, sizeof(result));
            return result;
        }
        if (format.sample_type == SampleType::floating_point && format.bits_per_channel == 16) {
            std::uint16_t half = 0;
            std::memcpy(&half, source + static_cast<std::size_t>(channel) * 2U, sizeof(half));
            const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000U) << 16U;
            std::uint32_t exponent = (half >> 10U) & 0x1FU;
            std::uint32_t mantissa = half & 0x3FFU;
            std::uint32_t bits = 0;
            if (exponent == 0U) {
                if (mantissa != 0U) {
                    exponent = 1U;
                    while ((mantissa & 0x400U) == 0U) {
                        mantissa <<= 1U;
                        --exponent;
                    }
                    mantissa &= 0x3FFU;
                    bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
                } else {
                    bits = sign;
                }
            } else if (exponent == 31U) {
                bits = sign | 0x7F800000U | (mantissa << 13U);
            } else {
                bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
            }
            float result = 0.0F;
            std::memcpy(&result, &bits, sizeof(result));
            return result;
        }
        if (format.bits_per_channel == 16) {
            std::uint16_t value = 0;
            std::memcpy(&value, source + static_cast<std::size_t>(channel) * 2U, sizeof(value));
            return static_cast<float>(value) / 65535.0F;
        }
        return static_cast<float>(std::to_integer<std::uint8_t>(source[channel])) / 255.0F;
    };
    Pixel pixel;
    if (gray) {
        pixel.red = pixel.green = pixel.blue = sample(0);
    } else {
        pixel.red = sample(bgr ? 2U : 0U);
        pixel.green = sample(1U);
        pixel.blue = sample(bgr ? 0U : 2U);
    }
    pixel.alpha = has_alpha ? sample(channels - 1U) : 1.0F;
    if (format.alpha == AlphaMode::premultiplied && pixel.alpha > 0.000001F) {
        pixel.red /= pixel.alpha;
        pixel.green /= pixel.alpha;
        pixel.blue /= pixel.alpha;
    }
    if (linear_rgb && format.sample_type != SampleType::floating_point) {
        pixel.red = srgb_to_linear(clamp_unit(pixel.red));
        pixel.green = srgb_to_linear(clamp_unit(pixel.green));
        pixel.blue = srgb_to_linear(clamp_unit(pixel.blue));
    }
    return pixel;
}

Pixel read_rgba8_pixel(const ImageView& view, std::uint32_t x, std::uint32_t y, bool bgra,
                       bool linear_rgb) {
    const std::byte* source = view.pixels.data() + static_cast<std::size_t>(y) * view.row_stride +
                              static_cast<std::size_t>(x) * 4U;
    const auto sample = [source](std::size_t channel) {
        return static_cast<float>(std::to_integer<std::uint8_t>(source[channel])) / 255.0F;
    };
    Pixel pixel;
    pixel.red = linear_rgb ? linear_byte(source[bgra ? 2U : 0U]) : sample(bgra ? 2U : 0U);
    pixel.green = linear_rgb ? linear_byte(source[1U]) : sample(1U);
    pixel.blue = linear_rgb ? linear_byte(source[bgra ? 0U : 2U]) : sample(bgra ? 0U : 2U);
    pixel.alpha = sample(3U);
    return pixel;
}

Result<Image> pack_rgba8(const snow::memory::PixelArray<Pixel>& pixels, std::uint32_t width,
                         std::uint32_t height, bool linear_rgb) {
    Result<MutableImage> allocated = MutableImage::allocate(width, height, kRgba8);
    if (!allocated)
        return allocated.error();
    MutableImage image = std::move(allocated).value();
    for (std::uint32_t y = 0; y < height; ++y) {
        std::byte* row = image.pixels().data() + static_cast<std::size_t>(y) * image.row_stride();
        for (std::uint32_t x = 0; x < width; ++x) {
            Pixel pixel = pixels[static_cast<std::size_t>(y) * width + x];
            if (linear_rgb) {
                pixel.red = linear_to_srgb(pixel.red);
                pixel.green = linear_to_srgb(pixel.green);
                pixel.blue = linear_to_srgb(pixel.blue);
            }
            const std::size_t offset = static_cast<std::size_t>(x) * 4U;
            row[offset] = static_cast<std::byte>(std::lround(clamp_unit(pixel.red) * 255.0F));
            row[offset + 1U] =
                static_cast<std::byte>(std::lround(clamp_unit(pixel.green) * 255.0F));
            row[offset + 2U] = static_cast<std::byte>(std::lround(clamp_unit(pixel.blue) * 255.0F));
            row[offset + 3U] =
                static_cast<std::byte>(std::lround(clamp_unit(pixel.alpha) * 255.0F));
        }
    }
    return std::move(image).freeze();
}

std::uint16_t float_to_half(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::uint32_t mantissa = bits & 0x7FFFFFU;
    const int exponent = static_cast<int>((bits >> 23U) & 0xFFU) - 127 + 15;
    if (exponent <= 0) {
        if (exponent < -10)
            return static_cast<std::uint16_t>(sign);
        const std::uint32_t rounded = (mantissa | 0x800000U) >> static_cast<unsigned>(1 - exponent);
        return static_cast<std::uint16_t>(sign | ((rounded + 0x1000U) >> 13U));
    }
    if (exponent >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7C00U | (mantissa == 0 ? 0U : 0x0200U));
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exponent) << 10U) |
                                      ((mantissa + 0x1000U) >> 13U));
}

void write_pixel(std::byte* destination, Pixel pixel, const PixelFormat& format, bool linear_rgb) {
    if (linear_rgb && format.sample_type != SampleType::floating_point) {
        pixel.red = linear_to_srgb(pixel.red);
        pixel.green = linear_to_srgb(pixel.green);
        pixel.blue = linear_to_srgb(pixel.blue);
    }
    if (format.alpha == AlphaMode::premultiplied) {
        pixel.red *= pixel.alpha;
        pixel.green *= pixel.alpha;
        pixel.blue *= pixel.alpha;
    }
    const bool gray =
        format.channels == ChannelLayout::gray || format.channels == ChannelLayout::gray_alpha;
    const bool bgr =
        format.channels == ChannelLayout::bgr || format.channels == ChannelLayout::bgra;
    const std::uint32_t channels = format.channel_count();
    const float luminance = 0.2126F * pixel.red + 0.7152F * pixel.green + 0.0722F * pixel.blue;
    auto component = [&](std::uint32_t channel) {
        if (gray)
            return channel == 0 ? luminance : pixel.alpha;
        if (channel == 0)
            return bgr ? pixel.blue : pixel.red;
        if (channel == 1)
            return pixel.green;
        if (channel == 2)
            return bgr ? pixel.red : pixel.blue;
        return pixel.alpha;
    };
    const std::size_t sample_bytes = format.bits_per_channel / 8U;
    for (std::uint32_t channel = 0; channel < channels; ++channel) {
        std::byte* target = destination + static_cast<std::size_t>(channel) * sample_bytes;
        const float value = component(channel);
        if (format.sample_type == SampleType::floating_point && format.bits_per_channel == 32) {
            std::memcpy(target, &value, sizeof(value));
        } else if (format.sample_type == SampleType::floating_point &&
                   format.bits_per_channel == 16) {
            const std::uint16_t half = float_to_half(value);
            std::memcpy(target, &half, sizeof(half));
        } else if (format.bits_per_channel == 16) {
            const std::uint16_t encoded =
                static_cast<std::uint16_t>(std::lround(clamp_unit(value) * 65535.0F));
            std::memcpy(target, &encoded, sizeof(encoded));
        } else {
            *target = static_cast<std::byte>(std::lround(clamp_unit(value) * 255.0F));
        }
    }
}

void write_rgba8_pixel(std::byte* destination, Pixel pixel, bool bgra, bool linear_rgb) {
    if (linear_rgb) {
        pixel.red = linear_to_srgb(pixel.red);
        pixel.green = linear_to_srgb(pixel.green);
        pixel.blue = linear_to_srgb(pixel.blue);
    }
    destination[bgra ? 2U : 0U] =
        static_cast<std::byte>(std::lround(clamp_unit(pixel.red) * 255.0F));
    destination[1] = static_cast<std::byte>(std::lround(clamp_unit(pixel.green) * 255.0F));
    destination[bgra ? 0U : 2U] =
        static_cast<std::byte>(std::lround(clamp_unit(pixel.blue) * 255.0F));
    destination[3] = static_cast<std::byte>(std::lround(clamp_unit(pixel.alpha) * 255.0F));
}

float kernel(ResamplingMethod method, float value) {
    value = std::abs(value);
    if (method == ResamplingMethod::linear)
        return value < 1.0F ? 1.0F - value : 0.0F;
    if (method == ResamplingMethod::nearest)
        return value < 0.5F ? 1.0F : 0.0F;
    if (value >= 3.0F)
        return 0.0F;
    if (value < 0.000001F)
        return 1.0F;
    constexpr float kPi = 3.14159265358979323846F;
    const float scaled = kPi * value;
    return (std::sin(scaled) / scaled) * (std::sin(scaled / 3.0F) / (scaled / 3.0F));
}

Result<std::vector<std::pair<int, float>>> weights(int source_extent, int target_extent, int target,
                                                   ResamplingMethod method, std::stop_token stop) {
    if (stop.stop_requested()) {
        return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
    }
    if (method == ResamplingMethod::nearest) {
        const int source =
            std::clamp(static_cast<int>(std::floor((static_cast<double>(target) + 0.5) *
                                                   source_extent / target_extent)),
                       0, source_extent - 1);
        return std::vector<std::pair<int, float>>{{source, 1.0F}};
    }
    const float scale = static_cast<float>(target_extent) / static_cast<float>(source_extent);
    const float filter_scale = std::min(1.0F, scale);
    const float radius = (method == ResamplingMethod::linear ? 1.0F : 3.0F) / filter_scale;
    const float center = (static_cast<float>(target) + 0.5F) / scale - 0.5F;
    const int first = static_cast<int>(std::ceil(center - radius));
    const int last = static_cast<int>(std::floor(center + radius));
    std::vector<std::pair<int, float>> result;
    float total = 0.0F;
    for (int source = first; source <= last; ++source) {
        if (((source - first) & 1023) == 0 && stop.stop_requested()) {
            return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
        }
        const float weight = kernel(method, (center - static_cast<float>(source)) * filter_scale);
        if (weight == 0.0F)
            continue;
        const int clamped = std::clamp(source, 0, source_extent - 1);
        const auto existing =
            std::find_if(result.begin(), result.end(),
                         [clamped](const auto& item) { return item.first == clamped; });
        if (existing == result.end())
            result.emplace_back(clamped, weight);
        else
            existing->second += weight;
        total += weight;
    }
    if (std::abs(total) > 0.000001F) {
        for (auto& item : result)
            item.second /= total;
    }
    return result;
}

struct PackedWeights final {
    std::vector<std::size_t> offsets;
    std::vector<int> indices;
    std::vector<float> values;
    std::size_t maximum_count = 0;
};

Result<PackedWeights> pack_weights(int source_extent, int target_extent, ResamplingMethod method,
                                   std::stop_token stop) {
    PackedWeights packed;
    try {
        packed.offsets.reserve(static_cast<std::size_t>(target_extent) + 1U);
        packed.offsets.push_back(0);
        for (int target = 0; target < target_extent; ++target) {
            Result<std::vector<std::pair<int, float>>> coefficients =
                weights(source_extent, target_extent, target, method, stop);
            if (!coefficients)
                return coefficients.error();
            packed.maximum_count = std::max(packed.maximum_count, coefficients.value().size());
            for (const auto& [index, value] : coefficients.value()) {
                packed.indices.push_back(index);
                packed.values.push_back(value);
            }
            packed.offsets.push_back(packed.indices.size());
        }
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate resize coefficient tables.");
    }
    return packed;
}

struct SourceMajorWeights final {
    std::vector<std::size_t> offsets;
    std::vector<std::uint32_t> targets;
    std::vector<float> values;
    std::vector<int> first_sources;
    std::vector<int> last_sources;
    std::size_t maximum_active_targets = 0;
};

Result<SourceMajorWeights> invert_weights(const PackedWeights& target_major,
                                          std::uint32_t source_extent, std::uint32_t target_extent,
                                          std::stop_token stop) {
    SourceMajorWeights result;
    try {
        result.offsets.assign(static_cast<std::size_t>(source_extent) + 1U, 0);
        result.first_sources.assign(target_extent, -1);
        result.last_sources.assign(target_extent, -1);
        std::vector<std::int64_t> active_delta(static_cast<std::size_t>(source_extent) + 1U, 0);

        for (std::uint32_t target = 0; target < target_extent; ++target) {
            if ((target & 255U) == 0U && stop.stop_requested()) {
                return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
            }
            const std::size_t first = target_major.offsets[target];
            const std::size_t last = target_major.offsets[target + 1U];
            if (first == last) {
                return Status::error(ErrorCode::internal_error,
                                     "Resize filter produced an empty target row.");
            }
            result.first_sources[target] = target_major.indices[first];
            result.last_sources[target] = target_major.indices[last - 1U];
            ++active_delta[static_cast<std::size_t>(result.first_sources[target])];
            if (static_cast<std::uint32_t>(result.last_sources[target]) + 1U < source_extent) {
                --active_delta[static_cast<std::size_t>(result.last_sources[target]) + 1U];
            }
            for (std::size_t coefficient = first; coefficient < last; ++coefficient) {
                ++result.offsets[static_cast<std::size_t>(target_major.indices[coefficient]) + 1U];
            }
        }

        std::size_t active = 0;
        for (std::uint32_t source = 0; source < source_extent; ++source) {
            result.offsets[source + 1U] += result.offsets[source];
            const std::int64_t next = static_cast<std::int64_t>(active) + active_delta[source];
            if (next < 0) {
                return Status::error(ErrorCode::internal_error,
                                     "Resize activity table is inconsistent.");
            }
            active = static_cast<std::size_t>(next);
            result.maximum_active_targets = std::max(result.maximum_active_targets, active);
        }

        result.targets.resize(target_major.indices.size());
        result.values.resize(target_major.values.size());
        std::vector<std::size_t> cursors = result.offsets;
        for (std::uint32_t target = 0; target < target_extent; ++target) {
            for (std::size_t coefficient = target_major.offsets[target];
                 coefficient < target_major.offsets[target + 1U]; ++coefficient) {
                const std::size_t source =
                    static_cast<std::size_t>(target_major.indices[coefficient]);
                const std::size_t destination = cursors[source]++;
                result.targets[destination] = target;
                result.values[destination] = target_major.values[coefficient];
            }
        }
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory,
                             "Could not allocate source-major resize tables.");
    }
    return result;
}

std::uint32_t resize_thread_count(const ResizeOptions& options, std::size_t row_ring_bytes) {
    std::uint32_t available = std::thread::hardware_concurrency();
    if (available == 0)
        available = 1;
    std::uint32_t limit =
        options.maximum_threads == 0 ? available : std::min(available, options.maximum_threads);
    limit = std::clamp<std::uint32_t>(limit, 1, options.height);
    if (options.maximum_threads == 0 && row_ring_bytes != 0) {
        constexpr std::size_t kAutomaticWorkingSet = std::size_t{256} << 20U;
        limit = std::min<std::uint32_t>(limit, static_cast<std::uint32_t>(std::max<std::size_t>(
                                                   1, kAutomaticWorkingSet / row_ring_bytes)));
    }
    return limit;
}

Pixel add_weighted(Pixel total, Pixel value, float weight, bool premultiply) {
    if (premultiply) {
        total.red += value.red * value.alpha * weight;
        total.green += value.green * value.alpha * weight;
        total.blue += value.blue * value.alpha * weight;
    } else {
        total.red += value.red * weight;
        total.green += value.green * weight;
        total.blue += value.blue * weight;
    }
    total.alpha += value.alpha * weight;
    return total;
}

void finish_pixel(Pixel* pixel, bool premultiply) {
    if (premultiply && pixel->alpha > 0.000001F) {
        pixel->red /= pixel->alpha;
        pixel->green /= pixel->alpha;
        pixel->blue /= pixel->alpha;
    }
    if (pixel->alpha <= 0.000001F)
        pixel->red = pixel->green = pixel->blue = 0.0F;
}

bool filter_horizontal_row(const ImageView& source, std::uint32_t source_y,
                           const PackedWeights& horizontal, const ResizeOptions& options,
                           bool specialized_rgba8, bool bgra8, std::span<Pixel> output,
                           std::stop_token stop) {
    for (std::uint32_t x = 0; x < options.width; ++x) {
        if ((x & 255U) == 0U && stop.stop_requested())
            return false;
        Pixel value{};
        for (std::size_t index = horizontal.offsets[x]; index < horizontal.offsets[x + 1U];
             ++index) {
            const std::uint32_t source_x = static_cast<std::uint32_t>(horizontal.indices[index]);
            const Pixel sample =
                specialized_rgba8
                    ? read_rgba8_pixel(source, source_x, source_y, bgra8, options.linear_rgb)
                    : read_pixel(source, source_x, source_y, options.linear_rgb);
            value =
                add_weighted(value, sample, horizontal.values[index], options.premultiply_alpha);
        }
        finish_pixel(&value, options.premultiply_alpha);
        output[x] = value;
    }
    return true;
}

Result<void>
filter_horizontal_input_row(const ImageView* image_source, const RasterSource* raster_source,
                            std::uint32_t source_width, const PixelFormat& source_format,
                            std::size_t source_row_bytes, std::uint32_t source_y,
                            const PackedWeights& horizontal, const ResizeOptions& options,
                            bool specialized_rgba8, bool bgra8, std::span<Pixel> output,
                            snow::memory::PixelArray<std::byte>& scratch, std::stop_token stop) {
    if (image_source != nullptr) {
        if (!filter_horizontal_row(*image_source, source_y, horizontal, options, specialized_rgba8,
                                   bgra8, output, stop)) {
            return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
        }
        return {};
    }
    if (raster_source == nullptr) {
        return Status::error(ErrorCode::internal_error, "Resize source is unavailable.");
    }
    if (scratch.size() != source_row_bytes)
        scratch.resize(source_row_bytes);
    Result<void> read =
        raster_source->read_rows(0, 0, source_y, 1, source_row_bytes, scratch, stop);
    if (!read)
        return read;
    const ImageView row_view{source_width, 1, source_format, source_row_bytes, scratch};
    if (!filter_horizontal_row(row_view, 0, horizontal, options, specialized_rgba8, bgra8, output,
                               stop)) {
        return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
    }
    return {};
}

bool write_resize_row(std::span<std::byte> output_pixels, std::size_t output_stride,
                      std::uint32_t output_y, std::size_t bytes_per_pixel,
                      const PixelFormat& format, const ResizeOptions& options,
                      bool specialized_rgba8, bool bgra8, std::span<Pixel> row,
                      std::stop_token stop) {
    std::byte* output = output_pixels.data() + static_cast<std::size_t>(output_y) * output_stride;
    for (std::uint32_t x = 0; x < options.width; ++x) {
        if ((x & 255U) == 0U && stop.stop_requested())
            return false;
        finish_pixel(&row[x], options.premultiply_alpha);
        std::byte* destination = output + static_cast<std::size_t>(x) * bytes_per_pixel;
        if (specialized_rgba8) {
            write_rgba8_pixel(destination, row[x], bgra8, options.linear_rgb);
        } else {
            write_pixel(destination, row[x], format, options.linear_rgb);
        }
    }
    return true;
}

bool exceeds_worker_cache(std::size_t ring_bytes, std::size_t ring_rows, std::uint64_t budget) {
    if (static_cast<std::uint64_t>(ring_bytes) > budget)
        return true;
    // Account conservatively for the source-to-slot index used by the cached path.
    constexpr std::uint64_t kIndexBytesPerRow = 32U;
    const std::uint64_t remaining = budget - static_cast<std::uint64_t>(ring_bytes);
    return ring_rows > remaining / kIndexBytesPerRow;
}

Result<void> resize_packed_into(const ImageView* image_source, const RasterSource* raster_source,
                                std::uint32_t source_width, std::uint32_t source_height,
                                const PixelFormat& source_format, const ResizeOptions& options,
                                std::span<std::byte> output_pixels, std::size_t output_stride,
                                std::stop_token stop) {
    if ((image_source == nullptr) == (raster_source == nullptr)) {
        return Status::error(ErrorCode::internal_error,
                             "Resize requires exactly one pixel source.");
    }
    if (options.width == 0 || options.height == 0) {
        return Status::error(ErrorCode::invalid_argument, "Resize dimensions must be non-zero.");
    }
    const std::uint64_t count = static_cast<std::uint64_t>(options.width) * options.height;
    if (count > (std::uint64_t{1} << 32U)) {
        return Status::error(ErrorCode::limit_exceeded, "Resized image exceeds the pixel limit.");
    }
    Result<void> processable = image_source != nullptr ? validate_processable(*image_source)
                                                       : validate_processable_format(source_format);
    if (!processable)
        return processable.error();
    if (source_width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        source_height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        options.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        options.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return Status::error(ErrorCode::limit_exceeded,
                             "Resize dimensions exceed the filter index range.");
    }
    Result<std::size_t> bytes_per_pixel = source_format.bytes_per_pixel();
    if (!bytes_per_pixel)
        return bytes_per_pixel.error();
    if (options.width > std::numeric_limits<std::size_t>::max() / bytes_per_pixel.value()) {
        return Status::error(ErrorCode::limit_exceeded, "Resized image row size overflows.");
    }
    const std::size_t row_bytes = static_cast<std::size_t>(options.width) * bytes_per_pixel.value();
    if (output_stride < row_bytes || output_pixels.size() < row_bytes ||
        options.height - 1U > (output_pixels.size() - row_bytes) / output_stride) {
        return Status::error(ErrorCode::invalid_argument, "Resize output storage is too small.");
    }
    if (source_width == options.width && source_height == options.height) {
        if (image_source != nullptr) {
            for (std::uint32_t y = 0; y < options.height; ++y) {
                if (stop.stop_requested()) {
                    return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
                }
                std::memcpy(output_pixels.data() + static_cast<std::size_t>(y) * output_stride,
                            image_source->pixels.data() +
                                static_cast<std::size_t>(y) * image_source->row_stride,
                            row_bytes);
            }
            return {};
        }
        constexpr std::uint32_t kRowsPerRead = 64;
        for (std::uint32_t first = 0; first < options.height; first += kRowsPerRead) {
            const std::uint32_t row_count = std::min(kRowsPerRead, options.height - first);
            const std::size_t offset = static_cast<std::size_t>(first) * output_stride;
            const std::size_t capacity =
                static_cast<std::size_t>(row_count - 1U) * output_stride + row_bytes;
            Result<void> read =
                raster_source->read_rows(0, 0, first, row_count, output_stride,
                                         output_pixels.subspan(offset, capacity), stop);
            if (!read)
                return read;
        }
        return {};
    }
    Result<PackedWeights> horizontal = pack_weights(
        static_cast<int>(source_width), static_cast<int>(options.width), options.method, stop);
    if (!horizontal)
        return horizontal.error();
    Result<PackedWeights> vertical = pack_weights(
        static_cast<int>(source_height), static_cast<int>(options.height), options.method, stop);
    if (!vertical)
        return vertical.error();

    const std::size_t ring_rows = std::max<std::size_t>(1, vertical.value().maximum_count);
    if (options.width > std::numeric_limits<std::size_t>::max() / sizeof(Pixel) / ring_rows) {
        return Status::error(ErrorCode::limit_exceeded, "Resize row-ring size overflows.");
    }
    const std::size_t ring_bytes =
        static_cast<std::size_t>(options.width) * sizeof(Pixel) * ring_rows;
    const bool rgba8 = source_format == kRgba8;
    const bool bgra8 = source_format == kBgra8;
    const bool specialized_rgba8 = rgba8 || bgra8;

    const bool unbounded_cache =
        options.maximum_worker_cache_bytes == std::numeric_limits<std::uint64_t>::max();
    const bool source_major =
        !unbounded_cache && source_height > options.height &&
        (ring_rows > kDefaultResizeMaximumCachedRows ||
         exceeds_worker_cache(ring_bytes, ring_rows, options.maximum_worker_cache_bytes));
    if (source_major) {
        Result<SourceMajorWeights> inverted =
            invert_weights(vertical.value(), source_height, options.height, stop);
        if (!inverted)
            return inverted.error();
        vertical = PackedWeights{};

        const std::size_t active_rows = inverted.value().maximum_active_targets;
        if (active_rows == 0 ||
            options.width > std::numeric_limits<std::size_t>::max() / sizeof(Pixel) / active_rows) {
            return Status::error(ErrorCode::limit_exceeded,
                                 "Streaming resize accumulator size overflows.");
        }
        try {
            snow::memory::PixelArray<Pixel> horizontal_row(options.width);
            snow::memory::PixelArray<std::byte> source_row;
            snow::memory::PixelArray<Pixel> accumulators(active_rows * options.width);
            constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();
            std::vector<std::size_t> target_slots(options.height, kNoSlot);
            std::vector<std::size_t> free_slots;
            free_slots.reserve(active_rows);
            for (std::size_t slot = active_rows; slot > 0; --slot)
                free_slots.push_back(slot - 1U);

            for (std::uint32_t source_y = 0; source_y < source_height; ++source_y) {
                if (stop.stop_requested()) {
                    return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
                }
                const std::size_t first = inverted.value().offsets[source_y];
                const std::size_t last = inverted.value().offsets[source_y + 1U];
                if (first == last)
                    continue;
                Result<void> filtered = filter_horizontal_input_row(
                    image_source, raster_source, source_width, source_format,
                    static_cast<std::size_t>(source_width) * bytes_per_pixel.value(), source_y,
                    horizontal.value(), options, specialized_rgba8, bgra8, horizontal_row,
                    source_row, stop);
                if (!filtered)
                    return filtered;
                for (std::size_t coefficient = first; coefficient < last; ++coefficient) {
                    const std::uint32_t target = inverted.value().targets[coefficient];
                    if (inverted.value().first_sources[target] == static_cast<int>(source_y)) {
                        if (free_slots.empty()) {
                            return Status::error(
                                ErrorCode::internal_error,
                                "Streaming resize exhausted its active-row table.");
                        }
                        const std::size_t slot = free_slots.back();
                        free_slots.pop_back();
                        target_slots[target] = slot;
                        std::fill_n(accumulators.data() + slot * options.width, options.width,
                                    Pixel{});
                    }
                    const std::size_t slot = target_slots[target];
                    if (slot == kNoSlot) {
                        return Status::error(ErrorCode::internal_error,
                                             "Streaming resize target has no accumulator.");
                    }
                    Pixel* accumulator = accumulators.data() + slot * options.width;
                    const float weight = inverted.value().values[coefficient];
                    for (std::uint32_t x = 0; x < options.width; ++x) {
                        if ((x & 1023U) == 0U && stop.stop_requested()) {
                            return Status::error(ErrorCode::cancelled,
                                                 "Image operation was cancelled.");
                        }
                        accumulator[x] = add_weighted(accumulator[x], horizontal_row[x], weight,
                                                      options.premultiply_alpha);
                    }
                    if (inverted.value().last_sources[target] == static_cast<int>(source_y)) {
                        if (!write_resize_row(output_pixels, output_stride, target,
                                              bytes_per_pixel.value(), source_format, options,
                                              specialized_rgba8, bgra8,
                                              std::span<Pixel>(accumulator, options.width), stop)) {
                            return Status::error(ErrorCode::cancelled,
                                                 "Image operation was cancelled.");
                        }
                        target_slots[target] = kNoSlot;
                        free_slots.push_back(slot);
                    }
                }
            }
            if (free_slots.size() != active_rows) {
                return Status::error(ErrorCode::internal_error,
                                     "Streaming resize did not finalize every target row.");
            }
        } catch (const std::bad_alloc&) {
            return Status::error(ErrorCode::out_of_memory,
                                 "Could not allocate streaming resize accumulators.");
        }
        return {};
    }

    const std::uint32_t thread_count =
        raster_source != nullptr &&
                !has_access(raster_source->access(), RasterAccess::concurrent_reads)
            ? 1U
            : resize_thread_count(options, ring_bytes);
    std::stop_source failure_stop;
    std::stop_callback external_stop(stop, [&failure_stop] { failure_stop.request_stop(); });
    std::atomic<bool> failed = false;
    Status failure;
    std::mutex failure_mutex;

    const auto run_band = [&](std::uint32_t first_y, std::uint32_t last_y) {
        try {
            snow::memory::PixelArray<Pixel> ring(ring_rows * options.width);
            std::vector<int> ring_sources(ring_rows, -1);
            std::vector<const Pixel*> vertical_rows(ring_rows);
            snow::memory::PixelArray<std::byte> source_row;
            std::unordered_map<int, std::size_t> source_slots;
            const bool indexed_slots = ring_rows > 32U;
            if (indexed_slots)
                source_slots.reserve(ring_rows);
            std::size_t next_slot = 0;
            const auto cancelled = [&] {
                return stop.stop_requested() || failure_stop.stop_requested();
            };
            for (std::uint32_t y = first_y; y < last_y && !cancelled(); ++y) {
                const std::size_t vertical_first = vertical.value().offsets[y];
                const std::size_t vertical_last = vertical.value().offsets[y + 1U];
                for (std::size_t coefficient = vertical_first; coefficient < vertical_last;
                     ++coefficient) {
                    const int source_y = vertical.value().indices[coefficient];
                    const bool already_cached =
                        indexed_slots ? source_slots.contains(source_y)
                                      : std::find(ring_sources.begin(), ring_sources.end(),
                                                  source_y) != ring_sources.end();
                    if (already_cached) {
                        continue;
                    }
                    std::size_t slot = next_slot++ % ring_rows;
                    for (std::size_t attempt = 0; attempt < ring_rows; ++attempt) {
                        const int candidate = ring_sources[slot];
                        const auto required =
                            std::lower_bound(vertical.value().indices.begin() +
                                                 static_cast<std::ptrdiff_t>(vertical_first),
                                             vertical.value().indices.begin() +
                                                 static_cast<std::ptrdiff_t>(vertical_last),
                                             candidate);
                        if (candidate < 0 ||
                            required == vertical.value().indices.begin() +
                                            static_cast<std::ptrdiff_t>(vertical_last) ||
                            *required != candidate) {
                            break;
                        }
                        slot = (slot + 1U) % ring_rows;
                    }
                    next_slot = slot + 1U;
                    if (indexed_slots && ring_sources[slot] >= 0)
                        source_slots.erase(ring_sources[slot]);
                    ring_sources[slot] = source_y;
                    if (indexed_slots)
                        source_slots.emplace(source_y, slot);
                    Pixel* horizontal_row = ring.data() + slot * options.width;
                    Result<void> filtered = filter_horizontal_input_row(
                        image_source, raster_source, source_width, source_format,
                        static_cast<std::size_t>(source_width) * bytes_per_pixel.value(),
                        static_cast<std::uint32_t>(source_y), horizontal.value(), options,
                        specialized_rgba8, bgra8, std::span<Pixel>(horizontal_row, options.width),
                        source_row, failure_stop.get_token());
                    if (!filtered) {
                        std::lock_guard lock(failure_mutex);
                        if (!failed.exchange(true))
                            failure = filtered.error();
                        failure_stop.request_stop();
                        return;
                    }
                }
                // Row slots are invariant across every column of this output row.
                // Resolve them once, keeping the coefficient accumulation order.
                for (std::size_t coefficient = vertical_first; coefficient < vertical_last;
                     ++coefficient) {
                    const int source_y = vertical.value().indices[coefficient];
                    const std::size_t slot =
                        indexed_slots
                            ? source_slots.at(source_y)
                            : static_cast<std::size_t>(
                                  std::find(ring_sources.begin(), ring_sources.end(), source_y) -
                                  ring_sources.begin());
                    vertical_rows[coefficient - vertical_first] =
                        ring.data() + slot * options.width;
                }
                std::byte* output_row =
                    output_pixels.data() + static_cast<std::size_t>(y) * output_stride;
                for (std::uint32_t x = 0; x < options.width; ++x) {
                    if ((x & 255U) == 0U && cancelled())
                        return;
                    Pixel value{};
                    for (std::size_t coefficient = vertical_first; coefficient < vertical_last;
                         ++coefficient) {
                        value = add_weighted(value, vertical_rows[coefficient - vertical_first][x],
                                             vertical.value().values[coefficient],
                                             options.premultiply_alpha);
                    }
                    finish_pixel(&value, options.premultiply_alpha);
                    std::byte* destination =
                        output_row + static_cast<std::size_t>(x) * bytes_per_pixel.value();
                    if (specialized_rgba8) {
                        write_rgba8_pixel(destination, value, bgra8, options.linear_rgb);
                    } else {
                        write_pixel(destination, value, source_format, options.linear_rgb);
                    }
                }
            }
        } catch (const std::bad_alloc&) {
            std::lock_guard lock(failure_mutex);
            if (!failed.exchange(true)) {
                failure =
                    Status::error(ErrorCode::out_of_memory, "Could not allocate resize row rings.");
            }
            failure_stop.request_stop();
        } catch (...) {
            std::lock_guard lock(failure_mutex);
            if (!failed.exchange(true)) {
                failure = Status::error(ErrorCode::internal_error,
                                        "Parallel image resize failed unexpectedly.");
            }
            failure_stop.request_stop();
        }
    };

    std::vector<std::jthread> workers;
    try {
        workers.reserve(thread_count);
        for (std::uint32_t index = 0; index < thread_count; ++index) {
            const std::uint32_t first = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(options.height) * index / thread_count);
            const std::uint32_t last = static_cast<std::uint32_t>(
                static_cast<std::uint64_t>(options.height) * (index + 1U) / thread_count);
            workers.emplace_back(run_band, first, last);
        }
    } catch (const std::system_error&) {
        failure_stop.request_stop();
        workers.clear();
        return Status::error(ErrorCode::internal_error, "Could not start resize worker threads.");
    }
    workers.clear();
    if (failed.load())
        return failure;
    if (stop.stop_requested()) {
        return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
    }
    return {};
}

Result<void> resize_image_into(const Image& source, const ResizeOptions& options,
                               std::span<std::byte> output_pixels, std::size_t output_stride,
                               std::stop_token stop) {
    const ImageView source_view = source.view();
    return resize_packed_into(&source_view, nullptr, source.width(), source.height(),
                              source.format(), options, output_pixels, output_stride, stop);
}

Result<Image> resize_image(const Image& source, const ResizeOptions& options,
                           std::stop_token stop) {
    if (options.width == 0 || options.height == 0) {
        return Status::error(ErrorCode::invalid_argument, "Resize dimensions must be non-zero.");
    }
    if (source.width() == options.width && source.height() == options.height)
        return source;
    Result<MutableImage> allocated =
        MutableImage::allocate(options.width, options.height, source.format());
    if (!allocated)
        return allocated.error();
    MutableImage output = std::move(allocated).value();
    Result<void> resized =
        resize_image_into(source, options, output.pixels(), output.row_stride(), stop);
    if (!resized)
        return resized.error();
    return std::move(output).freeze();
}

struct QuantizedColor final {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;
    std::uint8_t alpha = 255;
};

int color_distance(const QuantizedColor& left, const QuantizedColor& right) {
    const int red = static_cast<int>(left.red) - right.red;
    const int green = static_cast<int>(left.green) - right.green;
    const int blue = static_cast<int>(left.blue) - right.blue;
    const int alpha = static_cast<int>(left.alpha) - right.alpha;
    return red * red + green * green + blue * blue + alpha * alpha * 2;
}

Result<Image> reduce_palette(const Image& source, const PaletteOptions& options,
                             std::stop_token stop) {
    if (options.maximum_colors < 2U || options.maximum_colors > 256U ||
        !std::isfinite(options.dithering) || options.dithering < 0.0F || options.dithering > 1.0F) {
        return Status::error(ErrorCode::invalid_argument,
                             "Palette colors or dithering are outside the supported range.");
    }
    const ImageView source_view = source.view();
    Result<void> processable = validate_processable(source_view);
    if (!processable)
        return processable.error();
    std::array<std::uint32_t, 32768> histogram{};
    bool has_transparent = false;
    for (std::uint32_t y = 0; y < source_view.height; ++y) {
        if (stop.stop_requested()) {
            return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
        }
        for (std::uint32_t x = 0; x < source_view.width; ++x) {
            const Pixel pixel = read_pixel(source_view, x, y, false);
            const auto red = static_cast<std::uint32_t>(std::lround(clamp_unit(pixel.red) * 31.0F));
            const auto green =
                static_cast<std::uint32_t>(std::lround(clamp_unit(pixel.green) * 31.0F));
            const auto blue =
                static_cast<std::uint32_t>(std::lround(clamp_unit(pixel.blue) * 31.0F));
            ++histogram[(red << 10U) | (green << 5U) | blue];
            has_transparent = has_transparent || pixel.alpha < 0.5F;
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> bins;
    for (std::uint32_t index = 0; index < histogram.size(); ++index) {
        if (histogram[index] != 0U)
            bins.emplace_back(index, histogram[index]);
    }
    if (bins.empty())
        return source;

    struct Box final {
        std::vector<std::size_t> entries;
    };
    std::vector<Box> boxes(1);
    boxes.front().entries.resize(bins.size());
    std::iota(boxes.front().entries.begin(), boxes.front().entries.end(), 0U);
    auto component = [&](std::size_t entry, int channel) {
        const std::uint32_t code = bins[entry].first;
        return channel == 0   ? static_cast<int>((code >> 10U) & 31U)
               : channel == 1 ? static_cast<int>((code >> 5U) & 31U)
                              : static_cast<int>(code & 31U);
    };
    while (boxes.size() < options.maximum_colors) {
        std::size_t selected = boxes.size();
        int selected_range = -1;
        int selected_channel = 0;
        for (std::size_t index = 0; index < boxes.size(); ++index) {
            if (boxes[index].entries.size() < 2U)
                continue;
            for (int channel = 0; channel < 3; ++channel) {
                int minimum = 31;
                int maximum = 0;
                for (std::size_t entry : boxes[index].entries) {
                    minimum = std::min(minimum, component(entry, channel));
                    maximum = std::max(maximum, component(entry, channel));
                }
                if (maximum - minimum > selected_range) {
                    selected = index;
                    selected_range = maximum - minimum;
                    selected_channel = channel;
                }
            }
        }
        if (selected == boxes.size())
            break;
        auto& entries = boxes[selected].entries;
        std::stable_sort(entries.begin(), entries.end(), [&](std::size_t left, std::size_t right) {
            return component(left, selected_channel) < component(right, selected_channel);
        });
        std::uint64_t total = 0;
        for (std::size_t entry : entries)
            total += bins[entry].second;
        std::uint64_t accumulated = 0;
        std::size_t split = 1;
        for (; split < entries.size(); ++split) {
            accumulated += bins[entries[split - 1U]].second;
            if (accumulated * 2U >= total)
                break;
        }
        split = std::min(split, entries.size() - 1U);
        Box second;
        second.entries.assign(entries.begin() + static_cast<std::ptrdiff_t>(split), entries.end());
        entries.erase(entries.begin() + static_cast<std::ptrdiff_t>(split), entries.end());
        boxes.push_back(std::move(second));
    }

    std::vector<QuantizedColor> palette;
    palette.reserve(boxes.size());
    for (const Box& box : boxes) {
        std::uint64_t red = 0, green = 0, blue = 0, count = 0;
        for (std::size_t entry : box.entries) {
            const std::uint32_t code = bins[entry].first;
            const std::uint64_t weight = bins[entry].second;
            red += ((code >> 10U) & 31U) * 255U * weight / 31U;
            green += ((code >> 5U) & 31U) * 255U * weight / 31U;
            blue += (code & 31U) * 255U * weight / 31U;
            count += weight;
        }
        palette.push_back({static_cast<std::uint8_t>(red / count),
                           static_cast<std::uint8_t>(green / count),
                           static_cast<std::uint8_t>(blue / count), 255U});
    }
    if (has_transparent) {
        if (palette.size() == options.maximum_colors)
            palette.back() = {0, 0, 0, 0};
        else
            palette.push_back({0, 0, 0, 0});
    }

    const std::uint32_t width = source.width();
    const std::uint32_t height = source.height();
    Result<MutableImage> allocated = MutableImage::allocate(width, height, kRgba8);
    if (!allocated)
        return allocated.error();
    MutableImage output = std::move(allocated).value();
    snow::memory::PixelArray<std::array<float, 4>> next_error(width + 2U);
    snow::memory::PixelArray<std::array<float, 4>> current_error(width + 2U);
    for (std::uint32_t y = 0; y < height; ++y) {
        if (stop.stop_requested()) {
            return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
        }
        std::fill(next_error.begin(), next_error.end(), std::array<float, 4>{});
        const bool reverse = (y & 1U) != 0U;
        std::byte* output_row =
            output.pixels().data() + static_cast<std::size_t>(y) * output.row_stride();
        for (std::uint32_t iteration = 0; iteration < width; ++iteration) {
            const std::uint32_t x = reverse ? width - 1U - iteration : iteration;
            Pixel value = read_pixel(source_view, x, y, false);
            const auto& error = current_error[x + 1U];
            value.red = clamp_unit(value.red + error[0] * options.dithering);
            value.green = clamp_unit(value.green + error[1] * options.dithering);
            value.blue = clamp_unit(value.blue + error[2] * options.dithering);
            value.alpha = clamp_unit(value.alpha + error[3] * options.dithering);
            QuantizedColor desired{static_cast<std::uint8_t>(std::lround(value.red * 255.0F)),
                                   static_cast<std::uint8_t>(std::lround(value.green * 255.0F)),
                                   static_cast<std::uint8_t>(std::lround(value.blue * 255.0F)),
                                   static_cast<std::uint8_t>(std::lround(value.alpha * 255.0F))};
            const auto nearest = std::min_element(
                palette.cbegin(), palette.cend(), [&](const auto& left, const auto& right) {
                    return color_distance(desired, left) < color_distance(desired, right);
                });
            Pixel quantized{normalized_byte(nearest->red), normalized_byte(nearest->green),
                            normalized_byte(nearest->blue), normalized_byte(nearest->alpha)};
            write_pixel(output_row + static_cast<std::size_t>(x) * 4U, quantized, kRgba8, false);
            const std::array<float, 4> quantization_error{
                value.red - quantized.red, value.green - quantized.green,
                value.blue - quantized.blue, value.alpha - quantized.alpha};
            const int direction = reverse ? -1 : 1;
            auto diffuse = [&](snow::memory::PixelArray<std::array<float, 4>>& row, int position,
                               float factor) {
                if (position < 0 || position >= static_cast<int>(row.size()))
                    return;
                for (std::size_t channel = 0; channel < 4U; ++channel) {
                    row[static_cast<std::size_t>(position)][channel] +=
                        quantization_error[channel] * factor;
                }
            };
            diffuse(current_error, static_cast<int>(x + 1U) + direction, 7.0F / 16.0F);
            diffuse(next_error, static_cast<int>(x + 1U) - direction, 3.0F / 16.0F);
            diffuse(next_error, static_cast<int>(x + 1U), 5.0F / 16.0F);
            diffuse(next_error, static_cast<int>(x + 1U) + direction, 1.0F / 16.0F);
        }
        current_error.swap(next_error);
    }
    return std::move(output).freeze();
}

Pixel over(Pixel source, Pixel destination) {
    const float alpha = source.alpha + destination.alpha * (1.0F - source.alpha);
    if (alpha <= 0.000001F)
        return {};
    return {
        (source.red * source.alpha + destination.red * destination.alpha * (1.0F - source.alpha)) /
            alpha,
        (source.green * source.alpha +
         destination.green * destination.alpha * (1.0F - source.alpha)) /
            alpha,
        (source.blue * source.alpha +
         destination.blue * destination.alpha * (1.0F - source.alpha)) /
            alpha,
        alpha};
}

struct FrameComposition {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    snow::memory::PixelArray<Pixel> previous;
};

Result<FrameComposition> blend_frame(snow::memory::PixelArray<Pixel>& canvas, std::uint32_t width,
                                     std::uint32_t height, const Frame& frame, bool another_frame,
                                     std::stop_token stop) {
    const ImageView source = frame.image.view();
    Result<void> valid = validate_processable(source);
    if (!valid)
        return valid.error();
    FrameComposition region;
    region.width = frame.x < width ? std::min(source.width, width - frame.x) : 0U;
    region.height =
        region.width != 0 && frame.y < height ? std::min(source.height, height - frame.y) : 0U;
    if (frame.disposal == FrameDisposal::previous && another_frame) {
        // Only the clipped frame rectangle is modified. Preserve its pixels
        // without initializing or retaining another full float canvas.
        region.previous.reserve(static_cast<std::size_t>(region.width) * region.height);
        for (std::uint32_t y = 0; y < region.height; ++y) {
            if (stop.stop_requested())
                return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
            const Pixel* row =
                canvas.data() + static_cast<std::size_t>(frame.y + y) * width + frame.x;
            region.previous.insert(region.previous.end(), row, row + region.width);
        }
    }
    const bool rgba8 = source.format == kRgba8;
    const bool bgra8 = source.format == kBgra8;
    const bool replace = frame.blend == FrameBlend::source;
    // Keep conversion and alpha blending in separate loops. Their vectorizable
    // pixel traffic stays in a small reusable chunk instead of a full float plane.
    constexpr std::uint32_t kPixelChunk = 4096;
    snow::memory::PixelArray<Pixel> converted;
    if (!replace)
        converted.resize(std::min(region.width, kPixelChunk));
    for (std::uint32_t y = 0; y < region.height; ++y) {
        if (stop.stop_requested())
            return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
        Pixel* row = canvas.data() + static_cast<std::size_t>(frame.y + y) * width + frame.x;
        if (replace) {
            for (std::uint32_t x = 0; x < region.width; ++x)
                row[x] = rgba8 || bgra8 ? read_rgba8_pixel(source, x, y, bgra8, false)
                                        : read_pixel(source, x, y, false);
        } else {
            for (std::uint32_t first = 0; first < region.width;) {
                const std::uint32_t count = std::min(kPixelChunk, region.width - first);
                for (std::uint32_t x = 0; x < count; ++x)
                    converted[x] = rgba8 || bgra8
                                       ? read_rgba8_pixel(source, first + x, y, bgra8, false)
                                       : read_pixel(source, first + x, y, false);
                for (std::uint32_t x = 0; x < count; ++x)
                    row[first + x] = over(converted[x], row[first + x]);
                first += count;
            }
        }
    }
    return region;
}

void dispose_frame(snow::memory::PixelArray<Pixel>& canvas, std::uint32_t width, const Frame& frame,
                   const FrameComposition& region) {
    for (std::uint32_t y = 0; y < region.height; ++y) {
        Pixel* row = canvas.data() + static_cast<std::size_t>(frame.y + y) * width + frame.x;
        if (frame.disposal == FrameDisposal::background)
            std::fill_n(row, region.width, Pixel{});
        else if (!region.previous.empty())
            std::copy_n(region.previous.data() + static_cast<std::size_t>(y) * region.width,
                        region.width, row);
    }
}

Result<Document>
compose_frames(const Document& document, std::stop_token stop,
               std::size_t maximum_frames = std::numeric_limits<std::size_t>::max()) {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "Document has no raster frames.");
    }
    const std::uint32_t width = document.canvas_width;
    const std::uint32_t height = document.canvas_height;
    if (width == 0 || height == 0) {
        return Status::error(ErrorCode::invalid_argument, "Document canvas is empty.");
    }
    if (stop.stop_requested()) {
        return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
    }
    const std::size_t frame_count = std::min(maximum_frames, document.frames.size());
    const Frame& first = document.frames.front();
    // A full source replacement is already the composed first frame. Immutable
    // pixel ownership can cross this boundary without a float canvas or a copy.
    if (frame_count == 1 && first.x == 0 && first.y == 0 && first.image.width() == width &&
        first.image.height() == height && first.blend == FrameBlend::source &&
        first.image.format() == kRgba8 &&
        first.image.row_stride() == static_cast<std::size_t>(width) * 4U &&
        first.image.pixels().size() == first.image.row_stride() * height) {
        Result<void> valid = first.image.view().validate();
        if (!valid)
            return valid.error();
        Document result = document;
        result.frames.resize(1);
        result.frames.front().disposal = FrameDisposal::keep;
        return result;
    }
    snow::memory::PixelArray<Pixel> canvas(static_cast<std::size_t>(width) * height);
    Document result = document;
    result.frames.clear();
    for (const Frame& frame : document.frames) {
        if (stop.stop_requested()) {
            return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
        }
        const bool another_frame = result.frames.size() + 1U < frame_count;
        Result<FrameComposition> region =
            blend_frame(canvas, width, height, frame, another_frame, stop);
        if (!region)
            return region.error();
        Result<Image> packed = pack_rgba8(canvas, width, height, false);
        if (!packed)
            return packed.error();
        Frame composed = frame;
        composed.image = std::move(packed).value();
        composed.x = 0;
        composed.y = 0;
        composed.blend = FrameBlend::source;
        composed.disposal = FrameDisposal::keep;
        result.frames.push_back(std::move(composed));
        if (result.frames.size() >= maximum_frames)
            break;
        if (another_frame)
            dispose_frame(canvas, width, frame, region.value());
    }
    return result;
}

Pixel convert_primaries_to_srgb(Pixel pixel, ColorPrimaries primaries) {
    const float red = pixel.red;
    const float green = pixel.green;
    const float blue = pixel.blue;
    if (primaries == ColorPrimaries::display_p3) {
        pixel.red = 1.224745F * red - 0.224904F * green - 0.000000F * blue;
        pixel.green = -0.042058F * red + 1.042081F * green + 0.000000F * blue;
        pixel.blue = -0.019642F * red - 0.078655F * green + 1.098537F * blue;
    } else if (primaries == ColorPrimaries::rec2020) {
        pixel.red = 1.660491F * red - 0.587641F * green - 0.072850F * blue;
        pixel.green = -0.124550F * red + 1.132900F * green - 0.008349F * blue;
        pixel.blue = -0.018151F * red - 0.100579F * green + 1.118730F * blue;
    } else if (primaries == ColorPrimaries::adobe_rgb) {
        pixel.red = 1.398283F * red - 0.398283F * green;
        pixel.green = -0.000010F * red + 1.000010F * green;
        pixel.blue = -0.042939F * red - 0.072945F * green + 1.115884F * blue;
    }
    return pixel;
}

float aces_fitted(float value) {
    constexpr float a = 2.51F;
    constexpr float b = 0.03F;
    constexpr float c = 2.43F;
    constexpr float d = 0.59F;
    constexpr float e = 0.14F;
    value = std::max(0.0F, value);
    return clamp_unit((value * (a * value + b)) / (value * (c * value + d) + e));
}

float pq_encode(float linear_sc_rgb) {
    constexpr float m1 = 2610.0F / 16384.0F;
    constexpr float m2 = 2523.0F / 32.0F;
    constexpr float c1 = 3424.0F / 4096.0F;
    constexpr float c2 = 2413.0F / 128.0F;
    constexpr float c3 = 2392.0F / 128.0F;
    const float normalized = std::max(0.0F, linear_sc_rgb) * (80.0F / 10000.0F);
    const float powered = std::pow(normalized, m1);
    return std::pow((c1 + c2 * powered) / (1.0F + c3 * powered), m2);
}

Pixel linear_srgb_to_rec2020_pq(Pixel pixel) {
    const float red = pixel.red;
    const float green = pixel.green;
    const float blue = pixel.blue;
    pixel.red = pq_encode(0.627404F * red + 0.329283F * green + 0.043313F * blue);
    pixel.green = pq_encode(0.069097F * red + 0.919540F * green + 0.011362F * blue);
    pixel.blue = pq_encode(0.016391F * red + 0.088013F * green + 0.895595F * blue);
    return pixel;
}

bool frames_are_full_canvas_replacements(const Document& document) {
    return !document.frames.empty() &&
           std::all_of(document.frames.begin(), document.frames.end(), [&](const Frame& frame) {
               return frame.x == 0 && frame.y == 0 &&
                      frame.image.width() == document.canvas_width &&
                      frame.image.height() == document.canvas_height &&
                      frame.blend == FrameBlend::source;
           });
}

Result<Document> transform_document(const Document& document, const TransformOptions& options,
                                    std::stop_token stop) {
    if (document.frames.empty()) {
        return Status::error(ErrorCode::invalid_argument, "Document has no raster frames.");
    }
    if (stop.stop_requested()) {
        return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
    }
    const bool first_frame = options.animation_policy == AnimationPolicy::first_frame;
    Document result;
    if (frames_are_full_canvas_replacements(document)) {
        result = document;
        if (first_frame)
            result.frames.resize(1);
    } else {
        Result<Document> composed =
            compose_frames(document, stop, first_frame ? 1U : document.frames.size());
        if (!composed)
            return composed.error();
        result = std::move(composed).value();
    }
    for (Frame& frame : result.frames) {
        if (options.resize) {
            Result<Image> resized = resize_image(frame.image, *options.resize, stop);
            if (!resized)
                return resized.error();
            frame.image = std::move(resized).value();
        }
        if (options.palette) {
            Result<Image> reduced = reduce_palette(frame.image, *options.palette, stop);
            if (!reduced)
                return reduced.error();
            frame.image = std::move(reduced).value();
        }
    }
    if (options.resize) {
        result.canvas_width = options.resize->width;
        result.canvas_height = options.resize->height;
    }
    if (first_frame) {
        result.loop_count = 1;
        result.frames.front().duration = {};
    }
    result.vector.reset();
    return result;
}

} // namespace

Result<void> resize_raster_into(const RasterSource& source, const ResizeOptions& options,
                                MutablePlaneView destination, std::stop_token stop) {
    try {
        Result<void> descriptor_status = source.descriptor().validate();
        if (!descriptor_status)
            return descriptor_status;
        Result<void> destination_status = destination.validate();
        if (!destination_status)
            return destination_status;

        const DocumentDescriptor& document = source.descriptor();
        if (document.kind != DocumentKind::raster || document.frames.size() != 1U) {
            return Status::error(ErrorCode::unsupported_feature,
                                 "Resize requires exactly one raster frame.");
        }
        const RasterFrameDescriptor& frame = document.frames.front();
        if (frame.x != 0 || frame.y != 0 || frame.width != document.canvas_width ||
            frame.height != document.canvas_height || frame.layout.planes.size() != 1U) {
            return Status::error(ErrorCode::unsupported_feature,
                                 "Resize requires one full-canvas packed plane.");
        }
        const PlaneDescriptor& plane = frame.layout.planes.front();
        if (plane.semantic != PlaneSemantic::packed || plane.width != frame.width ||
            plane.height != frame.height) {
            return Status::error(ErrorCode::unsupported_feature,
                                 "Resize requires one full-canvas packed plane.");
        }
        if (destination.width != options.width || destination.height != options.height ||
            destination.format != plane.format) {
            return Status::error(
                ErrorCode::invalid_argument,
                "Resize destination dimensions and format must match the requested output.");
        }
        return resize_packed_into(nullptr, &source, frame.width, frame.height, plane.format,
                                  options, destination.pixels, destination.row_stride, stop);
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Raster resize ran out of memory.");
    } catch (...) {
        return Status::error(ErrorCode::internal_error, "Raster resize failed unexpectedly.");
    }
}

Result<Document> transform(const Document& document, const TransformOptions& options,
                           std::stop_token stop) {
    try {
        return transform_document(document, options, stop);
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Image processing ran out of memory.");
    } catch (...) {
        return Status::error(ErrorCode::internal_error, "Image processing failed unexpectedly.");
    }
}

Result<void> transform_to_sink(const Document& document, const TransformOptions& options,
                               PixelSink& sink, std::stop_token stop) {
    try {
        if (!document.frames.empty() && !options.palette &&
            frames_are_full_canvas_replacements(document)) {
            const bool first_frame = options.animation_policy == AnimationPolicy::first_frame;
            const std::size_t frame_count = first_frame ? 1U : document.frames.size();
            DocumentInfo info;
            info.format = document.format;
            info.canvas_width = options.resize ? options.resize->width : document.canvas_width;
            info.canvas_height = options.resize ? options.resize->height : document.canvas_height;
            if (info.canvas_width == 0 || info.canvas_height == 0) {
                return Status::error(ErrorCode::invalid_argument,
                                     "Resize dimensions must be non-zero.");
            }
            info.loop_count = first_frame ? 1U : document.loop_count;
            info.metadata = document.metadata;
            info.color = document.color;
            info.frames.reserve(frame_count);
            for (std::size_t index = 0; index < frame_count; ++index) {
                const Frame& frame = document.frames[index];
                info.frames.push_back({info.canvas_width, info.canvas_height, 0, 0,
                                       first_frame ? std::chrono::nanoseconds{} : frame.duration,
                                       frame.image.format(),
                                       frame.image.format().alpha != AlphaMode::none,
                                       frame.cursor_hotspot, frame.color, frame.metadata,
                                       frame.blend, frame.disposal});
            }
            Result<void> status = sink.begin(info);
            if (!status)
                return status;
            for (std::uint32_t index = 0; index < frame_count; ++index) {
                if (stop.stop_requested()) {
                    return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
                }
                const Frame& frame = document.frames[index];
                status = sink.begin_frame(index, info.frames[index]);
                if (!status)
                    return status;
                Result<std::size_t> bytes_per_pixel = frame.image.format().bytes_per_pixel();
                if (!bytes_per_pixel)
                    return bytes_per_pixel.error();
                if (info.canvas_width >
                    std::numeric_limits<std::size_t>::max() / bytes_per_pixel.value()) {
                    return Status::error(ErrorCode::limit_exceeded,
                                         "Transform output row size overflows.");
                }
                const std::size_t stride =
                    static_cast<std::size_t>(info.canvas_width) * bytes_per_pixel.value();
                if (info.canvas_height > std::numeric_limits<std::size_t>::max() / stride) {
                    return Status::error(ErrorCode::limit_exceeded,
                                         "Transform output size overflows.");
                }
                const std::size_t byte_size = stride * info.canvas_height;
                std::span<std::byte> storage = sink.frame_storage(index, stride, byte_size);
                ResizeOptions resize =
                    options.resize.value_or(ResizeOptions{frame.image.width(), frame.image.height(),
                                                          ResamplingMethod::nearest, true, true});
                if (storage.size() == byte_size) {
                    status = resize_image_into(frame.image, resize, storage, stride, stop);
                } else {
                    Result<MutableImage> allocated = MutableImage::allocate(
                        info.canvas_width, info.canvas_height, frame.image.format());
                    if (!allocated)
                        return allocated.error();
                    MutableImage output = std::move(allocated).value();
                    status = resize_image_into(frame.image, resize, output.pixels(),
                                               output.row_stride(), stop);
                    if (status) {
                        status = sink.write_rows(0, output.height(), output.row_stride(),
                                                 output.pixels());
                    }
                }
                if (!status)
                    return status;
                status = sink.end_frame(index);
                if (!status)
                    return status;
            }
            return sink.end();
        }
        if (document.frames.empty() || document.canvas_width == 0 || document.canvas_height == 0) {
            return Status::error(ErrorCode::invalid_argument, "Document has no raster frames.");
        }
        const bool first_frame = options.animation_policy == AnimationPolicy::first_frame;
        const std::size_t frame_count = first_frame ? 1U : document.frames.size();
        const bool compose = !frames_are_full_canvas_replacements(document);
        DocumentInfo info;
        info.format = document.format;
        info.canvas_width = options.resize ? options.resize->width : document.canvas_width;
        info.canvas_height = options.resize ? options.resize->height : document.canvas_height;
        if (info.canvas_width == 0 || info.canvas_height == 0) {
            return Status::error(ErrorCode::invalid_argument,
                                 "Resize dimensions must be non-zero.");
        }
        info.loop_count = first_frame ? 1U : document.loop_count;
        info.metadata = document.metadata;
        info.color = document.color;
        info.frames.reserve(frame_count);
        for (std::size_t index = 0; index < frame_count; ++index) {
            const Frame& frame = document.frames[index];
            const PixelFormat format = options.palette || compose ? kRgba8 : frame.image.format();
            info.frames.push_back({info.canvas_width, info.canvas_height, 0, 0,
                                   first_frame ? std::chrono::nanoseconds{} : frame.duration,
                                   format, format.alpha != AlphaMode::none, frame.cursor_hotspot,
                                   frame.color, frame.metadata,
                                   compose ? FrameBlend::source : frame.blend,
                                   compose ? FrameDisposal::keep : frame.disposal});
        }
        Result<void> status = sink.begin(info);
        if (!status)
            return status;
        snow::memory::PixelArray<Pixel> canvas;
        if (compose) {
            const std::uint64_t canvas_pixels =
                static_cast<std::uint64_t>(document.canvas_width) * document.canvas_height;
            if (canvas_pixels > std::numeric_limits<std::size_t>::max()) {
                return Status::error(ErrorCode::limit_exceeded, "Animation canvas size overflows.");
            }
            canvas.resize(static_cast<std::size_t>(canvas_pixels));
        }
        for (std::uint32_t index = 0; index < frame_count; ++index) {
            if (stop.stop_requested()) {
                return Status::error(ErrorCode::cancelled, "Image operation was cancelled.");
            }
            const Frame& frame = document.frames[index];
            FrameComposition region;
            Image working = frame.image;
            if (compose) {
                Result<FrameComposition> blended =
                    blend_frame(canvas, document.canvas_width, document.canvas_height, frame,
                                index + 1U < frame_count, stop);
                if (!blended)
                    return blended.error();
                region = std::move(blended).value();
                Result<Image> packed =
                    pack_rgba8(canvas, document.canvas_width, document.canvas_height, false);
                if (!packed)
                    return packed.error();
                working = std::move(packed).value();
            }
            if (options.resize) {
                Result<Image> resized = resize_image(working, *options.resize, stop);
                if (!resized)
                    return resized.error();
                working = std::move(resized).value();
            }
            if (options.palette) {
                Result<Image> reduced = reduce_palette(working, *options.palette, stop);
                if (!reduced)
                    return reduced.error();
                working = std::move(reduced).value();
            }
            status = sink.begin_frame(index, info.frames[index]);
            if (!status)
                return status;
            const std::size_t byte_size = working.pixels().size();
            std::span<std::byte> storage =
                sink.frame_storage(index, working.row_stride(), byte_size);
            if (storage.size() == byte_size) {
                std::memcpy(storage.data(), working.pixels().data(), byte_size);
            } else {
                status =
                    sink.write_rows(0, working.height(), working.row_stride(), working.pixels());
                if (!status)
                    return status;
            }
            status = sink.end_frame(index);
            if (!status)
                return status;
            if (compose && index + 1U < frame_count)
                dispose_frame(canvas, document.canvas_width, frame, region);
        }
        return sink.end();
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Image processing ran out of memory.");
    } catch (...) {
        return Status::error(ErrorCode::internal_error, "Image processing failed unexpectedly.");
    }
}

Result<Document> flatten_animation(const Document& document, std::stop_token stop) {
    Result<Document> composed = compose_frames(document, stop, 1U);
    if (!composed)
        return composed.error();
    Document result = std::move(composed).value();
    result.frames.resize(1);
    result.loop_count = 1;
    result.frames.front().duration = {};
    return result;
}

Result<AlphaContent> classify_alpha(ImageView image, std::stop_token stop) {
    Result<void> valid = image.validate();
    if (!valid)
        return valid.error();
    const PixelFormat& format = image.format;
    const bool alpha_layout = format.channels == ChannelLayout::gray_alpha ||
                              format.channels == ChannelLayout::rgba ||
                              format.channels == ChannelLayout::bgra;
    if (format.alpha == AlphaMode::none || !alpha_layout) {
        return AlphaContent::opaque;
    }
    if ((format.sample_type == SampleType::unsigned_integer && format.bits_per_channel != 8 &&
         format.bits_per_channel != 16) ||
        (format.sample_type == SampleType::floating_point && format.bits_per_channel != 16 &&
         format.bits_per_channel != 32) ||
        format.sample_type == SampleType::signed_integer) {
        return Status::error(ErrorCode::unsupported_feature,
                             "Alpha classification does not support this sample type.");
    }
    const std::size_t sample_bytes = format.bits_per_channel / 8U;
    const std::size_t pixel_bytes = static_cast<std::size_t>(format.channel_count()) * sample_bytes;
    const std::size_t alpha_offset = pixel_bytes - sample_bytes;
    const auto read_u16 = [&](const std::byte* sample) {
        const auto first = std::to_integer<std::uint16_t>(sample[0]);
        const auto second = std::to_integer<std::uint16_t>(sample[1]);
        return static_cast<std::uint16_t>(format.little_endian ? first | (second << 8U)
                                                               : (first << 8U) | second);
    };
    const auto half_to_float = [](std::uint16_t half) {
        const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000U) << 16U;
        std::uint32_t exponent = (half >> 10U) & 0x1FU;
        std::uint32_t mantissa = half & 0x3FFU;
        std::uint32_t bits = 0;
        if (exponent == 0U) {
            if (mantissa == 0U) {
                bits = sign;
            } else {
                exponent = 1U;
                while ((mantissa & 0x400U) == 0U) {
                    mantissa <<= 1U;
                    --exponent;
                }
                bits = sign | ((exponent + 112U) << 23U) | ((mantissa & 0x3FFU) << 13U);
            }
        } else if (exponent == 31U) {
            bits = sign | 0x7F800000U | (mantissa << 13U);
        } else {
            bits = sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
        }
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    };

    for (std::uint32_t y = 0; y < image.height; ++y) {
        if (stop.stop_requested()) {
            return Status::error(ErrorCode::cancelled, "Alpha classification was cancelled.");
        }
        const std::byte* row = image.pixels.data() + static_cast<std::size_t>(y) * image.row_stride;
        std::uint32_t scalar_start = 0;
#if defined(SNOW_IMAGE_HAS_SSE2)
        if (format.sample_type == SampleType::unsigned_integer &&
            (sample_bytes == 1 || sample_bytes == 2) && pixel_bytes <= 16 &&
            16 % pixel_bytes == 0) {
            const std::uint32_t pixels_per_vector = static_cast<std::uint32_t>(16 / pixel_bytes);
            unsigned expected_mask = 0;
            for (std::size_t pixel = 0; pixel < 16; pixel += pixel_bytes) {
                for (std::size_t byte = 0; byte < sample_bytes; ++byte)
                    expected_mask |= 1U << (pixel + alpha_offset + byte);
            }
            const __m128i opaque_bytes = _mm_set1_epi8(static_cast<char>(0xFF));
            while (image.width - scalar_start >= pixels_per_vector) {
                const auto* packed = reinterpret_cast<const __m128i*>(
                    row + static_cast<std::size_t>(scalar_start) * pixel_bytes);
                const __m128i samples = _mm_loadu_si128(packed);
                const unsigned equal_mask =
                    static_cast<unsigned>(_mm_movemask_epi8(_mm_cmpeq_epi8(samples, opaque_bytes)));
                if ((equal_mask & expected_mask) != expected_mask)
                    return AlphaContent::non_opaque;
                scalar_start += pixels_per_vector;
            }
        }
#endif
        for (std::uint32_t x = scalar_start; x < image.width; ++x) {
            const std::byte* alpha = row + static_cast<std::size_t>(x) * pixel_bytes + alpha_offset;
            bool opaque = false;
            if (format.sample_type == SampleType::unsigned_integer) {
                opaque = format.bits_per_channel == 8
                             ? alpha[0] == std::byte{0xFF}
                             : read_u16(alpha) == std::numeric_limits<std::uint16_t>::max();
            } else if (format.bits_per_channel == 16) {
                opaque = half_to_float(read_u16(alpha)) >= 1.0F;
            } else {
                std::array<std::byte, 4> native{};
                if (format.little_endian) {
                    std::copy_n(alpha, 4, native.begin());
                } else {
                    std::reverse_copy(alpha, alpha + 4, native.begin());
                }
                float value = 0.0F;
                std::memcpy(&value, native.data(), sizeof(value));
                opaque = value >= 1.0F;
            }
            if (!opaque)
                return AlphaContent::non_opaque;
        }
    }
    return AlphaContent::opaque;
}

Result<AlphaContent> classify_alpha(const Image& image, std::stop_token stop) {
    if (!image.alpha_analysis_)
        return classify_alpha(image.view(), stop);
    detail::AlphaAnalysisState& analysis = *image.alpha_analysis_;
    for (;;) {
        const detail::AlphaAnalysisValue value = analysis.value.load(std::memory_order_acquire);
        if (value == detail::AlphaAnalysisValue::opaque)
            return AlphaContent::opaque;
        if (value == detail::AlphaAnalysisValue::non_opaque) {
            return AlphaContent::non_opaque;
        }
        if (value == detail::AlphaAnalysisValue::unknown) {
            detail::AlphaAnalysisValue expected = detail::AlphaAnalysisValue::unknown;
            if (analysis.value.compare_exchange_strong(
                    expected, detail::AlphaAnalysisValue::analyzing, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                Result<AlphaContent> result = classify_alpha(image.view(), stop);
                analysis.value.store(result ? result.value() == AlphaContent::opaque
                                                  ? detail::AlphaAnalysisValue::opaque
                                                  : detail::AlphaAnalysisValue::non_opaque
                                            : detail::AlphaAnalysisValue::unknown,
                                     std::memory_order_release);
                analysis.changed.notify_all();
                return result;
            }
            continue;
        }

        std::unique_lock lock(analysis.mutex);
        const bool changed = analysis.changed.wait(lock, stop, [&analysis] {
            return analysis.value.load(std::memory_order_acquire) !=
                   detail::AlphaAnalysisValue::analyzing;
        });
        if (!changed) {
            return Status::error(ErrorCode::cancelled, "Alpha classification was cancelled.");
        }
    }
}

Result<Document> composite_alpha(const Document& document, std::uint8_t red, std::uint8_t green,
                                 std::uint8_t blue, bool linear_rgb, std::stop_token stop) {
    try {
        bool opaque = true;
        for (const Frame& frame : document.frames) {
            Result<AlphaContent> content = classify_alpha(frame.image, stop);
            if (!content)
                return content.error();
            if (content.value() == AlphaContent::non_opaque) {
                opaque = false;
                break;
            }
        }
        if (opaque)
            return document;
        Document result = document;
        for (Frame& frame : result.frames) {
            const ImageView source_view = frame.image.view();
            Result<void> processable = validate_processable(source_view);
            if (!processable)
                return processable.error();
            Result<MutableImage> allocated =
                MutableImage::allocate(frame.image.width(), frame.image.height(), kRgba8);
            if (!allocated)
                return allocated.error();
            MutableImage output = std::move(allocated).value();
            Pixel background{normalized_byte(red), normalized_byte(green), normalized_byte(blue),
                             1.0F};
            if (linear_rgb) {
                background.red = srgb_to_linear(background.red);
                background.green = srgb_to_linear(background.green);
                background.blue = srgb_to_linear(background.blue);
            }
            for (std::uint32_t y = 0; y < output.height(); ++y) {
                if (stop.stop_requested()) {
                    return Status::error(ErrorCode::cancelled, "Alpha compositing was cancelled.");
                }
                std::byte* output_row =
                    output.pixels().data() + static_cast<std::size_t>(y) * output.row_stride();
                for (std::uint32_t x = 0; x < output.width(); ++x) {
                    Pixel pixel = read_pixel(source_view, x, y, linear_rgb);
                    pixel.red = pixel.red * pixel.alpha + background.red * (1.0F - pixel.alpha);
                    pixel.green =
                        pixel.green * pixel.alpha + background.green * (1.0F - pixel.alpha);
                    pixel.blue = pixel.blue * pixel.alpha + background.blue * (1.0F - pixel.alpha);
                    pixel.alpha = 1.0F;
                    write_pixel(output_row + static_cast<std::size_t>(x) * 4U, pixel, kRgba8,
                                linear_rgb);
                }
            }
            frame.image = std::move(output).freeze();
        }
        return result;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "Alpha compositing ran out of memory.");
    }
}

Result<Document> convert_to_sdr_srgb(const Document& document, const SdrConversionOptions& options,
                                     std::stop_token stop) {
    try {
        const Result<std::size_t> output_pixel_bytes = options.output_format.bytes_per_pixel();
        if (!output_pixel_bytes ||
            options.output_format.sample_type != SampleType::unsigned_integer ||
            options.output_format.bits_per_channel != 8 ||
            options.output_format.channels == ChannelLayout::indexed ||
            options.output_format.channels == ChannelLayout::cmyk ||
            options.output_format.channels == ChannelLayout::gray_alpha) {
            return Status::error(
                ErrorCode::invalid_argument,
                "Structured SDR conversion requires packed Gray/RGB/BGR/RGBA/BGRA 8-bit output.");
        }
        if (!options.background && options.verified_alpha_content == AlphaContent::non_opaque &&
            options.output_format.alpha == AlphaMode::none) {
            return Status::error(
                ErrorCode::invalid_argument,
                "Non-opaque SDR conversion requires alpha output or a background.");
        }
        const bool already_sdr_srgb =
            document.color.dynamic_range == DynamicRange::standard &&
            (document.color.primaries == ColorPrimaries::unknown ||
             document.color.primaries == ColorPrimaries::srgb) &&
            (document.color.transfer == TransferFunction::unknown ||
             document.color.transfer == TransferFunction::srgb) &&
            std::all_of(document.frames.begin(), document.frames.end(), [&](const Frame& frame) {
                return frame.image.format().sample_type == SampleType::unsigned_integer &&
                       frame.image.format().bits_per_channel == 8 &&
                       frame.image.format() == options.output_format &&
                       frame.color.dynamic_range == DynamicRange::standard &&
                       (frame.color.primaries == ColorPrimaries::unknown ||
                        frame.color.primaries == ColorPrimaries::srgb) &&
                       (frame.color.transfer == TransferFunction::unknown ||
                        frame.color.transfer == TransferFunction::srgb);
            });
        if (already_sdr_srgb &&
            (!options.background || options.verified_alpha_content == AlphaContent::opaque))
            return document;
        Document result = document;
        Pixel background{};
        if (options.background) {
            background = {normalized_byte((*options.background)[0]),
                          normalized_byte((*options.background)[1]),
                          normalized_byte((*options.background)[2]), 1.0F};
            background.red = srgb_to_linear(background.red);
            background.green = srgb_to_linear(background.green);
            background.blue = srgb_to_linear(background.blue);
        }
        for (Frame& frame : result.frames) {
            const ImageView source_view = frame.image.view();
            Result<void> processable = validate_processable(source_view);
            if (!processable)
                return processable.error();
            Result<MutableImage> allocated = MutableImage::allocate(
                frame.image.width(), frame.image.height(), options.output_format);
            if (!allocated)
                return allocated.error();
            MutableImage output = std::move(allocated).value();
            const ColorPrimaries primaries = frame.color.primaries != ColorPrimaries::unknown
                                                 ? frame.color.primaries
                                                 : document.color.primaries;
            const bool hdr = frame.color.dynamic_range == DynamicRange::high ||
                             document.color.dynamic_range == DynamicRange::high ||
                             frame.image.format().sample_type == SampleType::floating_point;
            for (std::uint32_t y = 0; y < output.height(); ++y) {
                if (stop.stop_requested()) {
                    return Status::error(ErrorCode::cancelled, "SDR conversion was cancelled.");
                }
                std::byte* output_row =
                    output.pixels().data() + static_cast<std::size_t>(y) * output.row_stride();
                for (std::uint32_t x = 0; x < output.width(); ++x) {
                    Pixel pixel =
                        convert_primaries_to_srgb(read_pixel(source_view, x, y, true), primaries);
                    if (hdr && options.tone_map_hdr) {
                        pixel.red = aces_fitted(pixel.red);
                        pixel.green = aces_fitted(pixel.green);
                        pixel.blue = aces_fitted(pixel.blue);
                    }
                    if (options.background &&
                        options.verified_alpha_content == AlphaContent::non_opaque) {
                        pixel.red = pixel.red * pixel.alpha + background.red * (1.0F - pixel.alpha);
                        pixel.green =
                            pixel.green * pixel.alpha + background.green * (1.0F - pixel.alpha);
                        pixel.blue =
                            pixel.blue * pixel.alpha + background.blue * (1.0F - pixel.alpha);
                        pixel.alpha = 1.0F;
                    }
                    write_pixel(output_row +
                                    static_cast<std::size_t>(x) * output_pixel_bytes.value(),
                                pixel, options.output_format, true);
                }
            }
            frame.image = std::move(output).freeze();
            frame.color.primaries = ColorPrimaries::srgb;
            frame.color.transfer = TransferFunction::srgb;
            frame.color.dynamic_range = DynamicRange::standard;
            frame.color.icc_profile.clear();
        }
        result.color.primaries = ColorPrimaries::srgb;
        result.color.transfer = TransferFunction::srgb;
        result.color.dynamic_range = DynamicRange::standard;
        result.color.icc_profile.clear();
        return result;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "SDR conversion ran out of memory.");
    }
}

Result<Document> convert_to_sdr_srgb(const Document& document, bool tone_map_hdr,
                                     std::stop_token stop) {
    const bool already_sdr_srgb =
        document.color.dynamic_range == DynamicRange::standard &&
        (document.color.primaries == ColorPrimaries::unknown ||
         document.color.primaries == ColorPrimaries::srgb) &&
        (document.color.transfer == TransferFunction::unknown ||
         document.color.transfer == TransferFunction::srgb) &&
        std::all_of(document.frames.begin(), document.frames.end(), [](const Frame& frame) {
            return frame.image.format().sample_type == SampleType::unsigned_integer &&
                   frame.image.format().bits_per_channel == 8 &&
                   frame.color.dynamic_range == DynamicRange::standard &&
                   (frame.color.primaries == ColorPrimaries::unknown ||
                    frame.color.primaries == ColorPrimaries::srgb) &&
                   (frame.color.transfer == TransferFunction::unknown ||
                    frame.color.transfer == TransferFunction::srgb);
        });
    if (already_sdr_srgb)
        return document;
    SdrConversionOptions options;
    options.tone_map_hdr = tone_map_hdr;
    options.verified_alpha_content = AlphaContent::non_opaque;
    options.output_format = kRgba8;
    return convert_to_sdr_srgb(document, options, stop);
}

Result<Document> convert_to_hdr_rec2020_pq16(const Document& document, std::stop_token stop) {
    try {
        Document result = document;
        for (Frame& frame : result.frames) {
            const ImageView source_view = frame.image.view();
            Result<void> processable = validate_processable(source_view);
            if (!processable)
                return processable.error();
            Result<MutableImage> allocated =
                MutableImage::allocate(frame.image.width(), frame.image.height(), kRgba16);
            if (!allocated)
                return allocated.error();
            MutableImage output = std::move(allocated).value();
            for (std::uint32_t y = 0; y < output.height(); ++y) {
                if (stop.stop_requested()) {
                    return Status::error(ErrorCode::cancelled, "HDR conversion was cancelled.");
                }
                std::byte* row =
                    output.pixels().data() + static_cast<std::size_t>(y) * output.row_stride();
                for (std::uint32_t x = 0; x < output.width(); ++x) {
                    Pixel value = linear_srgb_to_rec2020_pq(read_pixel(source_view, x, y, true));
                    write_pixel(row + static_cast<std::size_t>(x) * 8U, value, kRgba16, false);
                }
            }
            frame.image = std::move(output).freeze();
            frame.color.primaries = ColorPrimaries::rec2020;
            frame.color.transfer = TransferFunction::pq;
            frame.color.dynamic_range = DynamicRange::high;
            frame.color.icc_profile.clear();
        }
        result.color.primaries = ColorPrimaries::rec2020;
        result.color.transfer = TransferFunction::pq;
        result.color.dynamic_range = DynamicRange::high;
        result.color.icc_profile.clear();
        return result;
    } catch (const std::bad_alloc&) {
        return Status::error(ErrorCode::out_of_memory, "HDR conversion ran out of memory.");
    }
}

} // namespace snow::image
