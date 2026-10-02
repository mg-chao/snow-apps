#include "snow_draw_engine_qt/snow_canvas_image.h"
#include <snow/memory/pixel_buffer.h>
#include "../../test-support/virtualmemory.h"

#include <QColorSpace>
#include <QPainter>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

using snow::test_support::virtualMemoryMapped;

void rawBuffersReturnPages() {
    using namespace snow::memory;
    require(!allocatePixelBuffer(0), "zero-byte allocation must be empty");
    require(!allocatePixelBuffer(std::numeric_limits<std::size_t>::max()),
            "unaddressable allocations must fail");
    for (const std::size_t size :
         {std::size_t{37}, kMappedPixelBufferMinimum, kMappedPixelBufferMinimum + 1}) {
        auto storage = allocatePixelBuffer(size);
        require(storage != nullptr, "pixel allocation must succeed");
        require(storage[0] == 0 && storage[size - 1] == 0, "pixel storage must start zeroed");
        storage[0] = 51;
        storage[size - 1] = 85;
        const auto* middle = storage.get() + size / 2;
        auto moved = std::move(storage);
        require(!storage && moved[0] == 51 && moved[size - 1] == 85,
                "ownership transfer must preserve the complete buffer");
        if (size >= kMappedPixelBufferMinimum) {
            require(virtualMemoryMapped(middle), "live pixel storage must remain mapped");
            moved.reset();
            require(!virtualMemoryMapped(middle),
                    "large pixels must unmap immediately on final release");
        }
    }
}

void imageSharingAndPaintingPreserveOwnership() {
    QImage image = snowCanvasAllocateImage(QSize(1025, 1025), QImage::Format_ARGB32_Premultiplied);
    require(!image.isNull(), "large image must allocate");
    const auto* data = image.constBits();
    const auto* middle = data + image.sizeInBytes() / 2;
    image.setColorSpace(QColorSpace::SRgb);
    image.setDevicePixelRatio(2);
    image.fill(Qt::red);
    {
        QPainter painter(&image);
        painter.fillRect(QRect(0, 0, 10, 10), Qt::blue);
    }
    require(image.constBits() == data, "painting and metadata must retain unique external storage");
    require(image.pixelColor(0, 0) == QColor(Qt::blue) &&
                image.pixelColor(1024, 1024) == QColor(Qt::red),
            "external image must paint correctly");
    QImage shared = image;
    image = {};
    require(virtualMemoryMapped(middle) && shared.constBits() == data,
            "shallow copies must retain pixel ownership");
    QImage detached = shared;
    detached.setPixelColor(0, 0, Qt::green);
    require(shared.pixelColor(0, 0) == QColor(Qt::blue) &&
                detached.pixelColor(0, 0) == QColor(Qt::green),
            "Qt copy-on-write must keep other owners unchanged");
    // Cross-thread export callbacks may destroy the last sharing image.
    std::thread release([last = std::move(shared)]() mutable { last = {}; });
    release.join();
    require(!virtualMemoryMapped(middle), "last image release must unmap its original storage");
}

void imageLayoutAndInvalidSizes() {
    require(snowCanvasAllocateImage({}, QImage::Format_ARGB32).isNull(), "empty size must fail");
    require(snowCanvasAllocateImage(QSize(1, 1), QImage::Format_Invalid).isNull(),
            "invalid format must fail");
    require(snowCanvasAllocateImage(
                QSize(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()),
                QImage::Format_RGBA64)
                .isNull(),
            "overflowing layout must fail before allocation");
    for (const auto format : {QImage::Format_RGB888, QImage::Format_Alpha8,
                              QImage::Format_Grayscale16, QImage::Format_RGBA64}) {
        QImage image = snowCanvasAllocateImage(QSize(1025, 1025), format);
        QImage reference(QSize(1025, 1), format);
        require(!image.isNull() && image.bytesPerLine() == reference.bytesPerLine(),
                "odd-width rows must match Qt alignment");
        image.fill(0);
        require(image.constBits()[image.sizeInBytes() - 1] == 0,
                "last padded row must be writable");
    }
}
} // namespace

int main() {
    rawBuffersReturnPages();
    imageSharingAndPaintingPreserveOwnership();
    imageLayoutAndInvalidSizes();
    return 0;
}
