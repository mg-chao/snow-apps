// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "snow_shot/presentation/screenshotimagerowsource.h"

#include <QImage>
#include <QtEndian>

#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#elif defined(__aarch64__) || defined(_M_ARM64)
#if defined(_MSC_VER)
#include <arm64_neon.h>
#else
#include <arm_neon.h>
#endif
#endif

namespace snow_shot::image_codec::detail {
inline ScreenshotImageAlphaContent imageAlphaContent(const QImage& image) {
    using Alpha = ScreenshotImageAlphaContent;
    if (image.isNull())
        return Alpha::Unknown;
    if (!image.hasAlphaChannel())
        return Alpha::Opaque;
    std::uint32_t mask = 0;
    switch (image.format()) {
    case QImage::Format_ARGB32:
    case QImage::Format_ARGB32_Premultiplied:
        mask = 0xff000000U;
        break;
    case QImage::Format_RGBA8888:
    case QImage::Format_RGBA8888_Premultiplied:
        mask = qFromLittleEndian(std::uint32_t{0xff000000U});
        break;
    default:
        // Other layouts remain unknown; never convert or materialize a raster just to inspect it.
        return Alpha::Unknown;
    }
    for (int y = 0; y < image.height(); ++y) {
        const uchar* row = image.constScanLine(y);
        int x = 0;
#if defined(__SSE2__) || defined(_M_X64)
        const __m128i alpha = _mm_set1_epi32(static_cast<int>(mask));
        for (; x <= image.width() - 4; x += 4) {
            const __m128i pixels =
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + qsizetype(x) * 4));
            if (_mm_movemask_epi8(_mm_cmpeq_epi32(_mm_and_si128(pixels, alpha), alpha)) != 0xffff)
                return Alpha::NonOpaque;
        }
#elif defined(__aarch64__) || defined(_M_ARM64)
        const uint32x4_t alpha = vdupq_n_u32(mask);
        for (; x <= image.width() - 4; x += 4) {
            const auto pixels = vreinterpretq_u32_u8(vld1q_u8(row + qsizetype(x) * 4));
            if (vminvq_u32(vceqq_u32(vandq_u32(pixels, alpha), alpha)) != 0xffffffffU)
                return Alpha::NonOpaque;
        }
#endif
        for (; x < image.width(); ++x) {
            std::uint32_t pixel;
            std::memcpy(&pixel, row + qsizetype(x) * 4, sizeof(pixel));
            if ((pixel & mask) != mask)
                return Alpha::NonOpaque;
        }
    }
    return Alpha::Opaque;
}
} // namespace snow_shot::image_codec::detail
