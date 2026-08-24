#include "snow_draw_engine_qt/snow_transient_image.h"

#include <QColor>
#include <QPainter>

#include <cstdlib>
#include <iostream>
#include <limits>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void supportedFormatsHaveCheckedContiguousStorage() {
    for (const QImage::Format format : {QImage::Format_ARGB32_Premultiplied,
                                        QImage::Format_RGBA8888}) {
        const QImage image = snow_draw_engine_qt::allocateTransientImage(QSize(37, 19), format);
        require(!image.isNull(), "a supported transient image should be valid");
        require(image.format() == format, "a transient image should retain its requested format");
        require(image.bytesPerLine() == 37 * 4,
                "a transient image should use a checked 32-bit stride");
        require(image.sizeInBytes() == 37 * 4 * 19,
                "a transient image should expose the complete contiguous allocation");
    }

    require(snow_draw_engine_qt::allocateTransientImage(QSize(), QImage::Format_RGBA8888).isNull(),
            "an empty transient image should be rejected");
    require(snow_draw_engine_qt::allocateTransientImage(QSize(8, 8), QImage::Format_Grayscale8)
                .isNull(),
            "a non-32-bit transient image should be rejected");
    require(snow_draw_engine_qt::allocateTransientImage(
                QSize((std::numeric_limits<int>::max)(),
                      (std::numeric_limits<int>::max)()),
                QImage::Format_RGBA8888)
                .isNull(),
            "overflowing transient image dimensions should be rejected");
}

void painterWritesAndImplicitSharingDetachSafely() {
    QImage original = snow_draw_engine_qt::allocateTransientImage(
        QSize(32, 24), QImage::Format_ARGB32_Premultiplied);
    require(!original.isNull(), "the painter target should be allocated");
    original.fill(Qt::transparent);
    {
        QPainter painter(&original);
        require(painter.isActive(), "QPainter should activate on transient storage");
        painter.fillRect(QRect(2, 3, 10, 8), QColor(12, 34, 56, 255));
    }
    require(original.pixelColor(4, 5) == QColor(12, 34, 56, 255),
            "QPainter should write the expected transient pixels");

    QImage copy = original;
    require(copy.constBits() == original.constBits(),
            "a shallow transient image copy should share its backing storage");
    copy.setPixelColor(4, 5, QColor(200, 100, 50, 255));
    require(copy.constBits() != original.constBits(),
            "writing a shared transient image should detach the writer");
    require(original.pixelColor(4, 5) == QColor(12, 34, 56, 255),
            "detaching a writer should preserve the original pixels");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
bool isCommitted(const void* address) {
    MEMORY_BASIC_INFORMATION information{};
    return VirtualQuery(address, &information, sizeof(information)) == sizeof(information) &&
           information.State == MEM_COMMIT;
}

void finalShallowOwnerReleasesVirtualAllocation() {
    const uchar* storage = nullptr;
    QImage copy;
    {
        QImage image = snow_draw_engine_qt::allocateTransientImage(
            QSize(256, 256), QImage::Format_ARGB32_Premultiplied);
        require(!image.isNull(), "the release-test image should be allocated");
        storage = image.constBits();
        require(isCommitted(storage), "transient pixels should occupy committed virtual memory");
        copy = image;
    }
    require(isCommitted(storage), "a shallow owner should retain transient virtual memory");
    copy = {};
    require(!isCommitted(storage),
            "destroying the final shallow owner should release transient virtual memory");
}
#endif
} // namespace

int main() {
    supportedFormatsHaveCheckedContiguousStorage();
    painterWritesAndImplicitSharingDetachSafely();
#if defined(Q_OS_WIN) || defined(_WIN32)
    finalShallowOwnerReleasesVirtualAllocation();
#endif
    return 0;
}
