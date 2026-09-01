#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

namespace {
bool cpuSupportsAvx2() {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_cpu_supports("avx2");
#elif defined(_MSC_VER)
    int registers[4] = {};
    __cpuidex(registers, 0, 0);
    if (registers[0] < 1) return false;
    __cpuidex(registers, 1, 0);
    const bool osxsave = (registers[2] & (1 << 27)) != 0;
    const bool avx = (registers[2] & (1 << 28)) != 0;
    if (!osxsave || !avx) return false;
    const unsigned __int64 xcr0 = _xgetbv(0);
    if ((xcr0 & 0x6) != 0x6) return false;
    __cpuidex(registers, 0, 0);
    if (registers[0] < 7) return false;
    __cpuidex(registers, 7, 0);
    return (registers[1] & (1 << 5)) != 0;
#else
    return false;
#endif
}
} // namespace

bool screenshotClipboardDibAvx2Available() {
#if defined(__AVX2__) || defined(_M_AVX2)
    static const bool available = cpuSupportsAvx2();
    return available;
#else
    return false;
#endif
}

bool screenshotClipboardDibDecodeBgrxAvx2(const std::uint32_t* source,
                                          std::uint32_t* destination, int pixels) {
#if defined(__AVX2__) || defined(_M_AVX2)
    if (!screenshotClipboardDibAvx2Available() || source == nullptr || destination == nullptr ||
        pixels <= 0) {
        return false;
    }
    const __m256i alpha = _mm256_set1_epi32(static_cast<int>(0xff000000u));
    int x = 0;
    for (; x + 8 <= pixels; x += 8) {
        const __m256i values = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(source + x));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(destination + x),
                            _mm256_or_si256(values, alpha));
    }
    for (; x < pixels; ++x) destination[x] = source[x] | 0xff000000u;
    return true;
#else
    (void)source;
    (void)destination;
    (void)pixels;
    return false;
#endif
}
