// SPDX-License-Identifier: Apache-2.0
#include "snow_draw_engine_qt/snow_canvas_image.h"

#include <snow/memory/pixel_buffer.h>

#include <limits>
#include <new>

namespace {
void releaseImageStorage(void* owner) {
    delete static_cast<snow::memory::PixelBuffer*>(owner);
}
} // namespace

QImage snowCanvasAllocateImage(const QSize& size, QImage::Format format) {
    if (size.width() <= 0 || size.height() <= 0 || format <= QImage::Format_Invalid ||
        format >= QImage::NImageFormats)
        return {};
    const quint64 depth = QImage::toPixelFormat(format).bitsPerPixel();
    const quint64 stride = ((static_cast<quint64>(size.width()) * depth + 31) / 32) * 4;
    const auto maximum = static_cast<quint64>(std::numeric_limits<qsizetype>::max());
    if (stride == 0 || stride > maximum / static_cast<quint64>(size.height()))
        return {};
    const quint64 bytes = stride * static_cast<quint64>(size.height());
    if (bytes < snow::memory::kMappedPixelBufferMinimum)
        return QImage(size, format);

    auto storage = snow::memory::allocatePixelBuffer(static_cast<std::size_t>(bytes));
    if (!storage)
        return {};
    auto owner = std::unique_ptr<snow::memory::PixelBuffer>(
        new (std::nothrow) snow::memory::PixelBuffer(std::move(storage)));
    if (!owner)
        return {};
    QImage image(owner->get(), size.width(), size.height(), static_cast<qsizetype>(stride), format,
                 releaseImageStorage, owner.get());
    if (!image.isNull())
        static_cast<void>(owner.release());
    return image;
}
