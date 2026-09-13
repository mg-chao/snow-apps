#include "snow_canvas_filter_avx2.h"

#include <algorithm>

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace snow_canvas_filter_render::detail {

// This translation unit is an explicitly selected x86 AVX2 implementation.
// NOLINTBEGIN(portability-simd-intrinsics)

bool copyRowsAvx2(ConstImageView source, ImageView destination, int beginRow, int endRow) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    if (source.width != destination.width || source.width < 8) {
        return false;
    }
    for (int y = beginRow; y < endRow; ++y) {
        const auto* input = reinterpret_cast<const std::uint32_t*>(
            source.data + static_cast<qsizetype>(y) * source.stride);
        auto* output = reinterpret_cast<std::uint32_t*>(
            destination.data + static_cast<qsizetype>(y) * destination.stride);
        int x = 0;
        for (; x + 8 <= source.width; x += 8) {
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + x),
                                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(input + x)));
        }
        for (; x < source.width; ++x) {
            output[x] = input[x];
        }
    }
    return beginRow < endRow;
#else
    Q_UNUSED(source);
    Q_UNUSED(destination);
    Q_UNUSED(beginRow);
    Q_UNUSED(endRow);
    return false;
#endif
}

bool downsampleFourTapAvx2(ConstImageView source, int sourceLeft, int sourceTop, int sourceRight,
                           int sourceBottom, ImageView destination, int factor, int beginRow,
                           int endRow) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    if (factor < 2 || destination.width < 8) {
        return false;
    }
    bool executed = false;
    const __m256i zero = _mm256_setzero_si256();
    for (int y = beginRow; y < endRow; ++y) {
        const int top = sourceTop + y * factor;
        const int bottom = std::min(sourceBottom, top + factor);
        const int y0 = top + (bottom - top) / 4;
        const int y1 = top + ((bottom - top) * 3) / 4;
        const auto* line0 =
            reinterpret_cast<const int*>(source.data + static_cast<qsizetype>(y0) * source.stride);
        const auto* line1 =
            reinterpret_cast<const int*>(source.data + static_cast<qsizetype>(y1) * source.stride);
        auto* output = reinterpret_cast<std::uint32_t*>(
            destination.data + static_cast<qsizetype>(y) * destination.stride);
        int x = 0;
        alignas(32) int firstIndices[8];
        alignas(32) int secondIndices[8];
        for (; x + 8 <= destination.width; x += 8) {
            bool full = y0 != y1;
            for (int lane = 0; lane < 8; ++lane) {
                const int left = sourceLeft + (x + lane) * factor;
                const int right = std::min(sourceRight, left + factor);
                firstIndices[lane] = left + (right - left) / 4;
                secondIndices[lane] = left + ((right - left) * 3) / 4;
                full = full && firstIndices[lane] != secondIndices[lane];
            }
            if (!full) {
                break;
            }
            const __m256i first = _mm256_load_si256(reinterpret_cast<const __m256i*>(firstIndices));
            const __m256i second =
                _mm256_load_si256(reinterpret_cast<const __m256i*>(secondIndices));
            const __m256i a = _mm256_i32gather_epi32(line0, first, 4);
            const __m256i b = _mm256_i32gather_epi32(line0, second, 4);
            const __m256i c = _mm256_i32gather_epi32(line1, first, 4);
            const __m256i d = _mm256_i32gather_epi32(line1, second, 4);
            const __m256i low = _mm256_srli_epi16(
                _mm256_add_epi16(
                    _mm256_add_epi16(_mm256_unpacklo_epi8(a, zero), _mm256_unpacklo_epi8(b, zero)),
                    _mm256_add_epi16(_mm256_unpacklo_epi8(c, zero), _mm256_unpacklo_epi8(d, zero))),
                2);
            const __m256i high = _mm256_srli_epi16(
                _mm256_add_epi16(
                    _mm256_add_epi16(_mm256_unpackhi_epi8(a, zero), _mm256_unpackhi_epi8(b, zero)),
                    _mm256_add_epi16(_mm256_unpackhi_epi8(c, zero), _mm256_unpackhi_epi8(d, zero))),
                2);
            const __m256i result = _mm256_packus_epi16(low, high);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(output + x), result);
            executed = true;
        }
        for (; x < destination.width; ++x) {
            const int left = sourceLeft + x * factor;
            const int right = std::min(sourceRight, left + factor);
            const int x0 = left + (right - left) / 4;
            const int x1 = left + ((right - left) * 3) / 4;
            const QRgb samples[] = {static_cast<QRgb>(line0[x0]), static_cast<QRgb>(line0[x1]),
                                    static_cast<QRgb>(line1[x0]), static_cast<QRgb>(line1[x1])};
            const int xCount = x0 == x1 ? 1 : 2;
            const int yCount = y0 == y1 ? 1 : 2;
            int a = 0, r = 0, g = 0, b = 0;
            for (int sy = 0; sy < yCount; ++sy) {
                for (int sx = 0; sx < xCount; ++sx) {
                    const QRgb pixel = samples[sy * 2 + sx];
                    a += qAlpha(pixel);
                    r += qRed(pixel);
                    g += qGreen(pixel);
                    b += qBlue(pixel);
                }
            }
            const int count = xCount * yCount;
            output[x] = qRgba(r / count, g / count, b / count, a / count);
        }
    }
    return executed;
#else
    Q_UNUSED(source);
    Q_UNUSED(sourceLeft);
    Q_UNUSED(sourceTop);
    Q_UNUSED(sourceRight);
    Q_UNUSED(sourceBottom);
    Q_UNUSED(destination);
    Q_UNUSED(factor);
    Q_UNUSED(beginRow);
    Q_UNUSED(endRow);
    return false;
#endif
}

int interpolateAndBlendConstantAvx2(const QRgb* first, const QRgb* second, QRgb* destination,
                                    int count, int weight, int mix) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    if (count < 8 || weight < 0 || weight > 256 || mix <= 0 || mix > 255) {
        return 0;
    }
    const __m256i zero = _mm256_setzero_si256();
    const __m256i interpolationWeight = _mm256_set1_epi16(static_cast<short>(weight));
    const __m256i interpolationInverse = _mm256_set1_epi16(static_cast<short>(256 - weight));
    const __m256i interpolationRounding = _mm256_set1_epi16(128);
    const __m256i blendWeight = _mm256_set1_epi16(static_cast<short>(mix));
    const __m256i blendInverse = _mm256_set1_epi16(static_cast<short>(255 - mix));
    const __m256i blendRounding = _mm256_set1_epi16(127);
    const __m256i one = _mm256_set1_epi16(1);
    const __m256i spreadByte = _mm256_set1_epi32(0x01010101);
    const auto interpolateHalf = [&](const __m256i from, const __m256i to) {
        return _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(from, interpolationInverse),
                                              _mm256_mullo_epi16(to, interpolationWeight)),
                             interpolationRounding),
            8);
    };
    const auto divideBy255 = [&](const __m256i value) {
        return _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(value, one), _mm256_srli_epi16(value, 8)), 8);
    };
    const auto blendHalf = [&](const __m256i current, const __m256i effect) {
        return divideBy255(
            _mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(current, blendInverse),
                                              _mm256_mullo_epi16(effect, blendWeight)),
                             blendRounding));
    };

    int processed = 0;
    for (; processed + 8 <= count; processed += 8) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(first + processed));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(second + processed));
        const __m256i effectLow =
            interpolateHalf(_mm256_unpacklo_epi8(a, zero), _mm256_unpacklo_epi8(b, zero));
        const __m256i effectHigh =
            interpolateHalf(_mm256_unpackhi_epi8(a, zero), _mm256_unpackhi_epi8(b, zero));
        __m256i result = _mm256_packus_epi16(effectLow, effectHigh);
        if (mix < 255) {
            const __m256i current =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destination + processed));
            result =
                _mm256_packus_epi16(blendHalf(_mm256_unpacklo_epi8(current, zero), effectLow),
                                    blendHalf(_mm256_unpackhi_epi8(current, zero), effectHigh));
        }
        const __m256i replicatedAlpha =
            _mm256_mullo_epi32(_mm256_srli_epi32(result, 24), spreadByte);
        result = _mm256_min_epu8(result, replicatedAlpha);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + processed), result);
    }
    return processed;
#else
    Q_UNUSED(first);
    Q_UNUSED(second);
    Q_UNUSED(destination);
    Q_UNUSED(count);
    Q_UNUSED(weight);
    Q_UNUSED(mix);
    return 0;
#endif
}

int interpolateAndBlendMaskedAvx2(const QRgb* first, const QRgb* second, QRgb* destination,
                                  const std::uint8_t* mask, int count, int weight) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    if (count < 8 || weight < 0 || weight > 256 || mask == nullptr) {
        return 0;
    }
    const __m256i zero = _mm256_setzero_si256();
    const __m256i full = _mm256_set1_epi16(255);
    const __m256i interpolationWeight = _mm256_set1_epi16(static_cast<short>(weight));
    const __m256i interpolationInverse = _mm256_set1_epi16(static_cast<short>(256 - weight));
    const __m256i interpolationRounding = _mm256_set1_epi16(128);
    const __m256i blendRounding = _mm256_set1_epi16(127);
    const __m256i one = _mm256_set1_epi16(1);
    const __m256i spreadByte = _mm256_set1_epi32(0x01010101);
    const auto interpolateHalf = [&](const __m256i from, const __m256i to) {
        return _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(from, interpolationInverse),
                                              _mm256_mullo_epi16(to, interpolationWeight)),
                             interpolationRounding),
            8);
    };
    const auto divideBy255 = [&](const __m256i value) {
        return _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(value, one), _mm256_srli_epi16(value, 8)), 8);
    };
    const auto blendHalf = [&](const __m256i current, const __m256i effect, const __m256i mix) {
        return divideBy255(_mm256_add_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(current, _mm256_sub_epi16(full, mix)),
                             _mm256_mullo_epi16(effect, mix)),
            blendRounding));
    };

    int processed = 0;
    for (; processed + 8 <= count; processed += 8) {
        const __m256i a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(first + processed));
        const __m256i b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(second + processed));
        const __m256i effectLow =
            interpolateHalf(_mm256_unpacklo_epi8(a, zero), _mm256_unpacklo_epi8(b, zero));
        const __m256i effectHigh =
            interpolateHalf(_mm256_unpackhi_epi8(a, zero), _mm256_unpackhi_epi8(b, zero));
        const __m256i current =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destination + processed));
        const __m128i maskBytes =
            _mm_loadl_epi64(reinterpret_cast<const __m128i*>(mask + processed));
        const __m256i packedMix = _mm256_mullo_epi32(_mm256_cvtepu8_epi32(maskBytes), spreadByte);
        const __m256i mixLow = _mm256_unpacklo_epi8(packedMix, zero);
        const __m256i mixHigh = _mm256_unpackhi_epi8(packedMix, zero);
        __m256i result = _mm256_packus_epi16(
            blendHalf(_mm256_unpacklo_epi8(current, zero), effectLow, mixLow),
            blendHalf(_mm256_unpackhi_epi8(current, zero), effectHigh, mixHigh));
        const __m256i replicatedAlpha =
            _mm256_mullo_epi32(_mm256_srli_epi32(result, 24), spreadByte);
        result = _mm256_min_epu8(result, replicatedAlpha);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + processed), result);
    }
    return processed;
#else
    Q_UNUSED(first);
    Q_UNUSED(second);
    Q_UNUSED(destination);
    Q_UNUSED(mask);
    Q_UNUSED(count);
    Q_UNUSED(weight);
    return 0;
#endif
}

namespace {

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
inline QRgb grayscalePixel(QRgb pixel) {
    const int alpha = qAlpha(pixel);
    const int luminance =
        qMin(alpha, (qRed(pixel) * 54 + qGreen(pixel) * 183 + qBlue(pixel) * 19 + 128) >> 8);
    return qRgba(luminance, luminance, luminance, alpha);
}

inline QRgb inversionPixel(QRgb pixel) {
    const int alpha = qAlpha(pixel);
    return qRgba(alpha - qRed(pixel), alpha - qGreen(pixel), alpha - qBlue(pixel), alpha);
}

inline QRgb blendPixel(QRgb current, QRgb effect, int mix) {
    const std::uint64_t inverse = static_cast<std::uint64_t>(255 - mix);
    const auto blendPair = [inverse, mix](std::uint32_t first, std::uint32_t second) {
        std::uint64_t value = static_cast<std::uint64_t>(first) * inverse +
                              static_cast<std::uint64_t>(second) * static_cast<std::uint64_t>(mix) +
                              0x007f007full;
        value += 0x00010001ull + ((value >> 8) & 0x00ff00ffull);
        return static_cast<std::uint32_t>((value >> 8) & 0x00ff00ffull);
    };
    const std::uint32_t redBlue = blendPair(current & 0x00ff00ffu, effect & 0x00ff00ffu);
    const std::uint32_t alphaGreen =
        blendPair((current >> 8) & 0x00ff00ffu, (effect >> 8) & 0x00ff00ffu);
    return redBlue | (alphaGreen << 8);
}

__m256i grayscalePixels(__m256i pixels) {
    const __m256i byteMask = _mm256_set1_epi32(0xff);
    const __m256i red = _mm256_and_si256(_mm256_srli_epi32(pixels, 16), byteMask);
    const __m256i green = _mm256_and_si256(_mm256_srli_epi32(pixels, 8), byteMask);
    const __m256i blue = _mm256_and_si256(pixels, byteMask);
    __m256i luminance = _mm256_add_epi32(
        _mm256_add_epi32(_mm256_mullo_epi32(red, _mm256_set1_epi32(54)),
                         _mm256_mullo_epi32(green, _mm256_set1_epi32(183))),
        _mm256_add_epi32(_mm256_mullo_epi32(blue, _mm256_set1_epi32(19)), _mm256_set1_epi32(128)));
    luminance = _mm256_srli_epi32(luminance, 8);
    const __m256i alpha = _mm256_srli_epi32(pixels, 24);
    luminance = _mm256_min_epu32(luminance, alpha);
    const __m256i gray = _mm256_mullo_epi32(luminance, _mm256_set1_epi32(0x00010101));
    return _mm256_or_si256(gray, _mm256_slli_epi32(alpha, 24));
}

__m256i inversionPixels(__m256i pixels) {
    const __m256i alpha = _mm256_srli_epi32(pixels, 24);
    const __m256i replicated = _mm256_mullo_epi32(alpha, _mm256_set1_epi32(0x01010101));
    const __m256i inverted = _mm256_subs_epu8(replicated, pixels);
    return _mm256_or_si256(
        _mm256_and_si256(inverted, _mm256_set1_epi32(0x00ffffff)),
        _mm256_and_si256(pixels, _mm256_set1_epi32(static_cast<int>(0xff000000u))));
}

__m256i blendConstant(__m256i current, __m256i effect, int mix) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i weight = _mm256_set1_epi16(static_cast<short>(mix));
    const __m256i inverse = _mm256_set1_epi16(static_cast<short>(255 - mix));
    const __m256i rounding = _mm256_set1_epi16(127);
    const __m256i one = _mm256_set1_epi16(1);
    const auto blendHalf = [&](const __m256i first, const __m256i second) {
        const __m256i value = _mm256_add_epi16(_mm256_add_epi16(_mm256_mullo_epi16(first, inverse),
                                                                _mm256_mullo_epi16(second, weight)),
                                               rounding);
        return _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(value, one), _mm256_srli_epi16(value, 8)), 8);
    };
    return _mm256_packus_epi16(
        blendHalf(_mm256_unpacklo_epi8(current, zero), _mm256_unpacklo_epi8(effect, zero)),
        blendHalf(_mm256_unpackhi_epi8(current, zero), _mm256_unpackhi_epi8(effect, zero)));
}

__m256i blendVariable(__m256i current, __m256i effect, __m256i mix32) {
    const __m256i zero = _mm256_setzero_si256();
    const __m256i full = _mm256_set1_epi16(255);
    const __m256i rounding = _mm256_set1_epi16(127);
    const __m256i one = _mm256_set1_epi16(1);
    const __m256i packedMix = _mm256_mullo_epi32(mix32, _mm256_set1_epi32(0x01010101));
    const __m256i mixLow = _mm256_unpacklo_epi8(packedMix, zero);
    const __m256i mixHigh = _mm256_unpackhi_epi8(packedMix, zero);
    const auto blendHalf = [&](const __m256i first, const __m256i second, const __m256i mix) {
        const __m256i value = _mm256_add_epi16(
            _mm256_add_epi16(_mm256_mullo_epi16(first, _mm256_sub_epi16(full, mix)),
                             _mm256_mullo_epi16(second, mix)),
            rounding);
        return _mm256_srli_epi16(
            _mm256_add_epi16(_mm256_add_epi16(value, one), _mm256_srli_epi16(value, 8)), 8);
    };
    return _mm256_packus_epi16(
        blendHalf(_mm256_unpacklo_epi8(current, zero), _mm256_unpacklo_epi8(effect, zero), mixLow),
        blendHalf(_mm256_unpackhi_epi8(current, zero), _mm256_unpackhi_epi8(effect, zero),
                  mixHigh));
}

__m256i combineMaskStrength(__m256i mask32, int strengthMix) {
    if (strengthMix >= 255) {
        return mask32;
    }
    __m256i value = _mm256_add_epi32(_mm256_mullo_epi32(mask32, _mm256_set1_epi32(strengthMix)),
                                     _mm256_set1_epi32(127));
    return _mm256_srli_epi32(_mm256_add_epi32(_mm256_add_epi32(value, _mm256_set1_epi32(1)),
                                              _mm256_srli_epi32(value, 8)),
                             8);
}

template <typename VectorTransform, typename ScalarTransform>
bool colorRectAvx2(ConstImageView source, ImageView destination, int left, int top, int right,
                   int bottom, int mix, VectorTransform vectorTransform,
                   ScalarTransform scalarTransform) {
    if (right - left < 8 || mix <= 0) {
        return false;
    }
    for (int y = top; y < bottom; ++y) {
        const auto* sourceLine = reinterpret_cast<const std::uint32_t*>(
            source.data + static_cast<qsizetype>(y) * source.stride);
        auto* destinationLine = reinterpret_cast<std::uint32_t*>(
            destination.data + static_cast<qsizetype>(y) * destination.stride);
        int x = left;
        for (; x + 8 <= right; x += 8) {
            const __m256i from =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(sourceLine + x));
            __m256i result = vectorTransform(from);
            if (mix < 255) {
                const __m256i current =
                    _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destinationLine + x));
                result = blendConstant(current, result, mix);
            }
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(destinationLine + x), result);
        }
        for (; x < right; ++x) {
            const QRgb effect = scalarTransform(sourceLine[x]);
            destinationLine[x] = mix == 255 ? effect : blendPixel(destinationLine[x], effect, mix);
        }
    }
    return true;
}

template <typename VectorTransform, typename ScalarTransform>
bool colorMaskedAvx2(ConstImageView source, ImageView destination, AlphaView mask, int maskOriginX,
                     int maskOriginY, int left, int top, int right, int bottom, int strengthMix,
                     VectorTransform vectorTransform, ScalarTransform scalarTransform) {
    if (right - left < 8 || strengthMix <= 0) {
        return false;
    }
    const __m256i full32 = _mm256_set1_epi32(255);
    for (int y = top; y < bottom; ++y) {
        const auto* sourceLine = reinterpret_cast<const std::uint32_t*>(
            source.data + static_cast<qsizetype>(y) * source.stride);
        auto* destinationLine = reinterpret_cast<std::uint32_t*>(
            destination.data + static_cast<qsizetype>(y) * destination.stride);
        const auto* alphaLine = mask.data + static_cast<qsizetype>(y - maskOriginY) * mask.stride;
        int x = left;
        for (; x + 8 <= right; x += 8) {
            const __m128i maskBytes =
                _mm_loadl_epi64(reinterpret_cast<const __m128i*>(alphaLine + x - maskOriginX));
            const __m256i mix32 = combineMaskStrength(_mm256_cvtepu8_epi32(maskBytes), strengthMix);
            if (_mm256_testz_si256(mix32, mix32)) {
                continue;
            }
            const __m256i from =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(sourceLine + x));
            const __m256i effect = vectorTransform(from);
            __m256i result = effect;
            const __m256i fullCoverage = _mm256_cmpeq_epi32(mix32, full32);
            if (_mm256_movemask_epi8(fullCoverage) != -1) {
                const __m256i current =
                    _mm256_loadu_si256(reinterpret_cast<const __m256i*>(destinationLine + x));
                result = blendVariable(current, effect, mix32);
            }
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(destinationLine + x), result);
        }
        for (; x < right; ++x) {
            const int mix = (alphaLine[x - maskOriginX] * strengthMix + 127) / 255;
            if (mix == 0) {
                continue;
            }
            const QRgb effect = scalarTransform(sourceLine[x]);
            destinationLine[x] = mix == 255 ? effect : blendPixel(destinationLine[x], effect, mix);
        }
    }
    return true;
}

#endif

} // namespace

bool grayscaleRectAvx2(ConstImageView source, ImageView destination, int left, int top, int right,
                       int bottom, int mix) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    return colorRectAvx2(source, destination, left, top, right, bottom, mix, grayscalePixels,
                         grayscalePixel);
#else
    Q_UNUSED(source);
    Q_UNUSED(destination);
    Q_UNUSED(left);
    Q_UNUSED(top);
    Q_UNUSED(right);
    Q_UNUSED(bottom);
    Q_UNUSED(mix);
    return false;
#endif
}

bool invertRectAvx2(ConstImageView source, ImageView destination, int left, int top, int right,
                    int bottom, int mix) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    return colorRectAvx2(source, destination, left, top, right, bottom, mix, inversionPixels,
                         inversionPixel);
#else
    Q_UNUSED(source);
    Q_UNUSED(destination);
    Q_UNUSED(left);
    Q_UNUSED(top);
    Q_UNUSED(right);
    Q_UNUSED(bottom);
    Q_UNUSED(mix);
    return false;
#endif
}

bool grayscaleAvx2(ImageView image, int beginRow, int endRow, int mix) {
    return grayscaleRectAvx2({image.data, image.width, image.height, image.stride}, image, 0,
                             beginRow, image.width, endRow, mix);
}

bool invertAvx2(ImageView image, int beginRow, int endRow, int mix) {
    return invertRectAvx2({image.data, image.width, image.height, image.stride}, image, 0, beginRow,
                          image.width, endRow, mix);
}

bool grayscaleMaskedAvx2(ConstImageView source, ImageView destination, AlphaView mask,
                         int maskOriginX, int maskOriginY, int left, int top, int right, int bottom,
                         int strengthMix) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    return colorMaskedAvx2(source, destination, mask, maskOriginX, maskOriginY, left, top, right,
                           bottom, strengthMix, grayscalePixels, grayscalePixel);
#else
    Q_UNUSED(source);
    Q_UNUSED(destination);
    Q_UNUSED(mask);
    Q_UNUSED(maskOriginX);
    Q_UNUSED(maskOriginY);
    Q_UNUSED(left);
    Q_UNUSED(top);
    Q_UNUSED(right);
    Q_UNUSED(bottom);
    Q_UNUSED(strengthMix);
    return false;
#endif
}

bool invertMaskedAvx2(ConstImageView source, ImageView destination, AlphaView mask, int maskOriginX,
                      int maskOriginY, int left, int top, int right, int bottom, int strengthMix) {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
    return colorMaskedAvx2(source, destination, mask, maskOriginX, maskOriginY, left, top, right,
                           bottom, strengthMix, inversionPixels, inversionPixel);
#else
    Q_UNUSED(source);
    Q_UNUSED(destination);
    Q_UNUSED(mask);
    Q_UNUSED(maskOriginX);
    Q_UNUSED(maskOriginY);
    Q_UNUSED(left);
    Q_UNUSED(top);
    Q_UNUSED(right);
    Q_UNUSED(bottom);
    Q_UNUSED(strengthMix);
    return false;
#endif
}

// NOLINTEND(portability-simd-intrinsics)

} // namespace snow_canvas_filter_render::detail
