#include "snow_draw_engine_qt/snow_transient_image.h"

#include <cstddef>
#include <limits>
#include <new>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace snow_draw_engine_qt {
namespace {
constexpr std::size_t kBytesPerPixel = 4;

void releaseImageStorage(void* storage) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (storage != nullptr) {
        static_cast<void>(VirtualFree(storage, 0, MEM_RELEASE));
    }
#else
    delete[] static_cast<uchar*>(storage);
#endif
}

uchar* allocateImageStorage(std::size_t byteCount) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return static_cast<uchar*>(
        VirtualAlloc(nullptr, byteCount, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    return new (std::nothrow) uchar[byteCount];
#endif
}

bool isSupportedFormat(QImage::Format format) {
    return format > QImage::Format_Invalid && format < QImage::NImageFormats &&
           QImage::toPixelFormat(format).bitsPerPixel() == kBytesPerPixel * 8;
}
} // namespace

QImage allocateTransientImage(const QSize& size, QImage::Format format) {
    if (size.width() <= 0 || size.height() <= 0 || !isSupportedFormat(format)) {
        return {};
    }

    const std::size_t width = static_cast<std::size_t>(size.width());
    const std::size_t height = static_cast<std::size_t>(size.height());
    constexpr std::size_t kMaximumSize = (std::numeric_limits<std::size_t>::max)();
    constexpr auto kMaximumQtSize =
        static_cast<std::size_t>((std::numeric_limits<qsizetype>::max)());
    if (width > kMaximumSize / kBytesPerPixel) {
        return {};
    }
    const std::size_t bytesPerLine = width * kBytesPerPixel;
    if (bytesPerLine > kMaximumQtSize || height > kMaximumSize / bytesPerLine) {
        return {};
    }
    const std::size_t byteCount = bytesPerLine * height;
    if (byteCount > kMaximumQtSize) {
        return {};
    }

    uchar* const storage = allocateImageStorage(byteCount);
    if (storage == nullptr) {
        return {};
    }
    return QImage(storage, size.width(), size.height(), static_cast<qsizetype>(bytesPerLine), format,
                  releaseImageStorage, storage);
}

} // namespace snow_draw_engine_qt
