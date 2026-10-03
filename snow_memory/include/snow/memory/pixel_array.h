// SPDX-License-Identifier: Apache-2.0
#ifndef SNOW_MEMORY_PIXEL_ARRAY_H
#define SNOW_MEMORY_PIXEL_ARRAY_H

#include <snow/memory/pixel_buffer.h>

#include <new>
#include <type_traits>
#include <vector>

namespace snow::memory {

// Vector supplies object lifetimes and capacity management; its allocator owns
// the original capacity, so shrinking a logical raster cannot truncate release.
template <typename T> class PixelBufferAllocator {
    static_assert(std::is_trivially_copyable_v<T>, "pixel storage requires trivial elements");
    static_assert(alignof(T) <= alignof(std::max_align_t), "unsupported pixel alignment");

  public:
    using value_type = T;
    using is_always_equal = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;

    PixelBufferAllocator() noexcept = default;
    template <typename U> PixelBufferAllocator(const PixelBufferAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count >
            static_cast<std::size_t>((std::numeric_limits<std::ptrdiff_t>::max)()) / sizeof(T))
            throw std::bad_array_new_length();
        auto storage = allocatePixelBuffer(count * sizeof(T));
        if (!storage && count != 0)
            throw std::bad_alloc();
        return reinterpret_cast<T*>(storage.release());
    }

    void deallocate(T* data, std::size_t count) noexcept {
        releasePixelBuffer(reinterpret_cast<std::uint8_t*>(data), count * sizeof(T));
    }
};

template <typename T, typename U>
constexpr bool operator==(const PixelBufferAllocator<T>&, const PixelBufferAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
constexpr bool operator!=(const PixelBufferAllocator<T>&, const PixelBufferAllocator<U>&) noexcept {
    return false;
}

template <typename T> using PixelArray = std::vector<T, PixelBufferAllocator<T>>;

} // namespace snow::memory
#endif
