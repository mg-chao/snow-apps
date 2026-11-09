// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace snow::image::detail {

// Keep the scalar transfer and rounding as the reference when locating the first float that
// produces each byte. This preserves platform-specific pow rounding at quantization boundaries.
inline std::uint8_t scalar_linear_to_srgb8(float value) {
    value = std::max(value, 0.0F);
    const float encoded =
        value <= 0.0031308F ? value * 12.92F : 1.055F * std::pow(value, 1.0F / 2.4F) - 0.055F;
    return static_cast<std::uint8_t>(std::lround(std::clamp(encoded, 0.0F, 1.0F) * 255.0F));
}

struct Srgb8Quantization final {
    static constexpr unsigned bucket_count = 4096;
    std::array<float, 255> thresholds{};
    std::array<std::uint8_t, bucket_count> lower_bytes{};

    Srgb8Quantization() {
        static_assert(sizeof(float) == sizeof(std::uint32_t) &&
                      std::numeric_limits<float>::is_iec559);
        std::uint32_t previous = 0;
        for (unsigned byte = 1; byte <= 255; ++byte) {
            std::uint32_t first = previous;
            std::uint32_t last = std::bit_cast<std::uint32_t>(1.0F);
            while (first < last) {
                const auto middle = first + (last - first) / 2;
                if (scalar_linear_to_srgb8(std::bit_cast<float>(middle)) < byte)
                    first = middle + 1;
                else
                    last = middle;
            }
            thresholds[byte - 1] = std::bit_cast<float>(first);
            previous = first;
        }
        unsigned byte = 0;
        for (unsigned bucket = 0; bucket < bucket_count; ++bucket) {
            const float value = static_cast<float>(bucket) / bucket_count;
            while (byte < 255 && value >= thresholds[byte])
                ++byte;
            lower_bytes[bucket] = static_cast<std::uint8_t>(byte);
        }
    }

    [[nodiscard]] std::uint8_t encode(float value) const {
        if (value <= 0.0F)
            return 0;
        if (value >= 1.0F)
            return 255;
        if (!std::isfinite(value))
            return scalar_linear_to_srgb8(value);
        const auto bucket = static_cast<unsigned>(value * bucket_count);
        unsigned byte = lower_bytes[bucket];
        // The table selects a lower bound, not an approximation to the transfer function.
        // An exact threshold comparison resolves the rounding boundary within the bucket.
        while (byte < 255 && value >= thresholds[byte])
            ++byte;
        return static_cast<std::uint8_t>(byte);
    }
};

inline std::uint8_t linear_to_srgb8(float value) {
    static const Srgb8Quantization quantization;
    return quantization.encode(value);
}

} // namespace snow::image::detail
