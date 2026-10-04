#include "snow_draw_engine_qt/snow_canvas_image.h"
#include <snow/memory/pixel_buffer.h>
#include <snow/memory/pixel_array.h>
#include "../../test-support/virtualmemory.h"

#include <QBuffer>
#include <QColorSpace>
#include <QPainter>
#include <QSemaphore>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

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
    for (int index = 1; index < QImage::NImageFormats; ++index) {
        const auto format = static_cast<QImage::Format>(index);
        const QImage row(33, 1, format);
        const qsizetype height =
            (qsizetype(snow::memory::kMappedPixelBufferMinimum) + row.bytesPerLine() - 1) /
            row.bytesPerLine();
        QImage image = snowCanvasAllocateImage(QSize(33, static_cast<int>(height)), format);
        require(!image.isNull() && image.depth() == row.depth() &&
                    image.bytesPerLine() == row.bytesPerLine(),
                "every format must use Qt storage depth rather than color channel bits");
        const auto* middle = image.constBits() + image.sizeInBytes() / 2;
        image.fill(0);
        image = {};
        require(!virtualMemoryMapped(middle),
                "every storage format reaching one megabyte must return its pages");
    }
}

void monochromeImagesPreserveDefaultPalette() {
    for (const auto format : {QImage::Format_Mono, QImage::Format_MonoLSB}) {
        for (const auto size :
             {QSize(4096, 2047), QSize(4096, 2048), QSize(4096, 2049), QSize(4097, 2048)}) {
            QImage image = snowCanvasAllocateImage(size, format);
            QImage reference(size, format);
            require(!image.isNull() && image.colorTable() == reference.colorTable(),
                    "monochrome allocation must retain Qt's default palette at every size");
            const auto* pixels = image.constBits();
            image.fill(1);
            reference.fill(1);
            image.setPixel(0, 0, 0);
            reference.setPixel(0, 0, 0);
            image.setPixel(size.width() - 1, size.height() - 1, 0);
            reference.setPixel(size.width() - 1, size.height() - 1, 0);
            require(image == reference && image.pixelColor(0, 0) == QColor(Qt::black) &&
                        image.pixelColor(1, 0) == QColor(Qt::white),
                    "both monochrome bit orders must expose the same black and white pixels");

            QByteArray encoded;
            QBuffer buffer(&encoded);
            require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
                    "allocated monochrome images must encode with their default palette");
            const QImage decoded = QImage::fromData(encoded, "PNG");
            require(decoded.size() == size && decoded.pixelColor(0, 0) == QColor(Qt::black) &&
                        decoded.pixelColor(1, 0) == QColor(Qt::white) &&
                        decoded.pixelColor(size.width() - 1, size.height() - 1) ==
                            QColor(Qt::black),
                    "monochrome PNG round trips must preserve palette colors and edge pixels");

            image.setColorTable({qRgb(255, 0, 0), qRgb(0, 0, 255)});
            const QImage fresh = snowCanvasAllocateImage(size, format);
            require(image.pixelColor(0, 0) == QColor(Qt::red) &&
                        fresh.colorTable() == reference.colorTable(),
                    "customizing one image's palette must preserve subsequent default palettes");
            {
                const QImage copied = snowCanvasCopyImage(image);
                require(copied == image && copied.colorTable() == image.colorTable(),
                        "copying a monochrome image must retain its customized palette");
            }
            require(image.constBits() == pixels,
                    "palette changes must retain unique writable pixel storage");
            if (image.sizeInBytes() >= qsizetype(snow::memory::kMappedPixelBufferMinimum)) {
                const auto* middle = pixels + image.sizeInBytes() / 2;
                image = {};
                require(!virtualMemoryMapped(middle),
                        "monochrome palette initialization must preserve final page release");
            }
        }
    }
}

void rasterOperationsPreservePixelsAndReleasePages() {
    QImage source = snowCanvasAllocateImage(QSize(1025, 513), QImage::Format_RGBA8888);
    source.setColorSpace(QColorSpace::SRgb);
    source.setDevicePixelRatio(1.5);
    source.setDotsPerMeterX(3779);
    source.setText(QStringLiteral("fixture"), QStringLiteral("pixel ownership"));
    for (int y = 0; y < source.height(); ++y) {
        uchar* row = source.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            row[x * 4] = static_cast<uchar>(x % 251);
            row[x * 4 + 1] = static_cast<uchar>(y % 239);
            row[x * 4 + 2] = static_cast<uchar>((x + y) % 227);
            row[x * 4 + 3] = static_cast<uchar>((x * 3 + y) % 256);
        }
    }
    const auto checkRelease = [](QImage& image) {
        const auto* middle = image.constBits() + image.sizeInBytes() / 2;
        require(virtualMemoryMapped(middle), "operation result must own mapped pixels");
        image = {};
        require(!virtualMemoryMapped(middle), "operation result must release its final pages");
    };
    QImage converted = snowCanvasConvertImage(source, QImage::Format_ARGB32_Premultiplied);
    require(converted == source.convertToFormat(QImage::Format_ARGB32_Premultiplied),
            "bounded format conversion must exactly match Qt including alpha");
    require(converted.devicePixelRatio() == source.devicePixelRatio() &&
                converted.colorSpace() == source.colorSpace() &&
                converted.text(QStringLiteral("fixture")) == source.text(QStringLiteral("fixture")),
            "format conversion must preserve metadata");
    checkRelease(converted);
    source.setColorSpace(QColorSpace::DisplayP3);
    QImage color = snowCanvasColorConvertedImage(source, QColorSpace::SRgb,
                                                 QImage::Format_ARGB32_Premultiplied);
    require(color == source.convertedToColorSpace(QColorSpace::SRgb,
                                                  QImage::Format_ARGB32_Premultiplied),
            "bounded color conversion must exactly match Qt");
    checkRelease(color);
    const QRect crop(-3, -2, 1030, 520);
    QImage cropped = snowCanvasCopyImage(source, crop);
    require(cropped == source.copy(crop), "mapped crop must preserve padding and pixels");
    checkRelease(cropped);
    for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation}) {
        for (int orientation = 0; orientation < 8; ++orientation) {
            QTransform transform;
            transform.rotate((orientation % 4) * 90);
            if (orientation >= 4)
                transform.scale(-1, 1);
            QImage rotated = snowCanvasTransformImage(source, transform, mode);
            if (rotated != source.transformed(transform, mode)) {
                std::cerr << "orientation=" << orientation << " mode=" << mode << '\n';
                const QImage expected = source.transformed(transform, mode);
                for (int y = 0, differences = 0; y < expected.height() && differences < 10; ++y)
                    for (int x = 0; x < expected.width() && differences < 10; ++x)
                        if (rotated.pixel(x, y) != expected.pixel(x, y)) {
                            std::cerr << "diff " << x << ',' << y << ' ' << std::hex
                                      << rotated.pixel(x, y) << " != " << expected.pixel(x, y)
                                      << std::dec << '\n';
                            ++differences;
                        }
            }
            require(rotated == source.transformed(transform, mode),
                    "quarter-turn and flip pixels must exactly match Qt");
            if (orientation != 0)
                checkRelease(rotated);
        }
    }
    QImage large = snowCanvasAllocateImage(QSize(1025, 1024), QImage::Format_RGBA8888);
    large.fill(QColor(83, 121, 217, 67));
    for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation}) {
        for (const QTransform& transform : {QTransform().scale(-1, 1), QTransform().scale(1, -1)}) {
            QImage flipped = snowCanvasTransformImage(large, transform, mode);
            require(flipped == large.transformed(transform, mode),
                    "large smooth mirrors must preserve Qt premultiplication rounding");
            checkRelease(flipped);
        }
    }
    QImage scaled = snowCanvasScaleImage(source, QSize(913, 711), Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
    require(scaled ==
                source.scaled(QSize(913, 711), Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
            "managed scaling must retain Qt resampling pixels");
    checkRelease(scaled);
    QImage wide = snowCanvasAllocateImage(QSize(70001, 5), QImage::Format_RGBA8888);
    wide.fill(QColor(103, 79, 211, 121));
    QImage wideConverted = snowCanvasConvertImage(wide, QImage::Format_ARGB32_Premultiplied);
    require(wideConverted == wide.convertToFormat(QImage::Format_ARGB32_Premultiplied),
            "very wide rows must use bounded conversion without changing pixels");
    checkRelease(wideConverted);
    QImage gray(QSize(769, 769), QImage::Format_Grayscale8);
    gray.fill(173);
    QImage expanded = snowCanvasConvertImage(gray, QImage::Format_RGBA8888);
    require(expanded == gray.convertToFormat(QImage::Format_RGBA8888),
            "a small source expanding into a large format must retain Qt pixels");
    checkRelease(expanded);
    QImage detached = source;
    require(snowCanvasDetachImage(detached), "explicit copy-on-write must succeed");
    require(detached.constBits() != source.constBits() && detached == source,
            "detached storage must preserve original pixels");
    const auto* owned = detached.constBits();
    require(snowCanvasDetachImage(detached) && detached.constBits() == owned,
            "unique owned pixels must avoid redundant copies");
    detached.setPixelColor(0, 0, Qt::red);
    require(detached.pixelColor(0, 0) != source.pixelColor(0, 0),
            "detached mutation must preserve the original");
    checkRelease(detached);
    QImage readOnly(static_cast<const uchar*>(source.constBits()), source.width(), source.height(),
                    source.bytesPerLine(), source.format());
    require(snowCanvasDetachImage(readOnly) && readOnly.constBits() != source.constBits(),
            "a unique read-only view over mapped pixels must become independently writable");
    const auto* writable = readOnly.constBits();
    readOnly.fill(Qt::blue);
    require(readOnly.constBits() == writable,
            "mutating detached read-only pixels must not heap-copy");
    checkRelease(readOnly);
}

void pixelArraysReleaseOriginalCapacity() {
    snow::memory::PixelArray<float> values(1024 * 1024, 1.5f);
    const auto* middle = values.data() + values.size() / 2;
    values.resize(10);
    auto moved = std::move(values);
    require(moved.front() == 1.5f && virtualMemoryMapped(middle),
            "shrinking and moving must retain original allocation ownership");
    moved = {};
    moved.shrink_to_fit();
    require(!virtualMemoryMapped(middle), "pixel arrays must release their original capacity");
}

QImage alphaFixture(const QSize& size) {
    QImage image = snowCanvasAllocateImage(size, QImage::Format_RGBA8888);
    require(!image.isNull(), "alpha fixture must allocate");
    constexpr std::array<uchar, 10> alpha{0, 1, 2, 3, 63, 127, 128, 253, 254, 255};
    uchar* pixels = image.bits();
    for (int y = 0; y < size.height(); ++y) {
        uchar* row = pixels + qsizetype(y) * image.bytesPerLine();
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4] = static_cast<uchar>((x * 29 + y * 37) % 256);
            row[x * 4 + 1] = static_cast<uchar>((x * 47 + y * 19) % 256);
            row[x * 4 + 2] = static_cast<uchar>((x * 13 + y * 53) % 256);
            row[x * 4 + 3] = alpha[static_cast<std::size_t>((x + y) % int(alpha.size()))];
        }
    }
    image.setColorSpace(QColorSpace::DisplayP3);
    image.setDevicePixelRatio(1.25);
    image.setDotsPerMeterX(4173);
    image.setDotsPerMeterY(3981);
    image.setOffset(QPoint(17, 29));
    image.setText(QStringLiteral("fixture"), QStringLiteral("alpha boundaries"));
    return image;
}

void zeroedImagesInitializePixelsAndPadding() {
    require(snowCanvasAllocateZeroedImage({}, QImage::Format_RGBA8888).isNull(),
            "empty zeroed images must fail");
    require(snowCanvasAllocateZeroedImage(QSize(1, 1), QImage::Format_Invalid).isNull(),
            "invalid zeroed image formats must fail");
    for (int index = 1; index < QImage::NImageFormats; ++index) {
        const auto format = static_cast<QImage::Format>(index);
        const QImage row(33, 1, format);
        const int largeHeight = static_cast<int>(
            (qsizetype(snow::memory::kMappedPixelBufferMinimum) + row.bytesPerLine() - 1) /
            row.bytesPerLine());
        for (const QSize& size : {QSize(33, 5), QSize(33, largeHeight)}) {
            QImage image = snowCanvasAllocateZeroedImage(size, format);
            require(!image.isNull(), "zeroed images must allocate every valid format");
            const uchar* pixels = image.constBits();
            require(std::all_of(pixels, pixels + image.sizeInBytes(),
                                [](uchar byte) { return byte == 0; }),
                    "zeroed images must initialize every byte, including row padding");
            if (image.hasAlphaChannel())
                require(image.pixelColor(0, 0).alpha() == 0,
                        "zeroed alpha images must start transparent");
            if (image.sizeInBytes() >= qsizetype(snow::memory::kMappedPixelBufferMinimum)) {
                const uchar* middle = pixels + image.sizeInBytes() / 2;
                image = {};
                require(!virtualMemoryMapped(middle),
                        "zeroed large images must preserve immediate final page release");
            }
        }
    }
}

void requireRgbaRowsEquivalent(const QImage& source, int firstRow, int rowCount,
                               const QColorSpace& target = {},
                               Qt::ImageConversionFlags flags = Qt::AutoColor) {
    // Qt 6.11's const indexed color converter changes its source palette. Keep
    // reference metadata independent so comparison never modifies the input.
    const QImage reference = source.copy();
    const auto originalPalette = source.colorTable();
    const QImage expected =
        target.isValid() ? reference.convertedToColorSpace(target, QImage::Format_RGBA8888, flags)
                         : reference.convertToFormat(QImage::Format_RGBA8888, flags);
    require(!expected.isNull(), "row reference must convert");
    const qsizetype rowBytes = qsizetype(source.width()) * 4;
    const qsizetype stride = rowBytes + 23;
    constexpr qsizetype prefix = 17;
    constexpr qsizetype suffix = 19;
    constexpr uchar sentinel = 0xa7;
    const qsizetype capacity = stride * rowCount;
    std::vector<uchar> output(static_cast<std::size_t>(prefix + capacity + suffix), sentinel);
    const uchar* sourcePixels = source.constBits();
    require(snowCanvasCopyRgba8888Rows(source, firstRow, rowCount, output.data() + prefix, capacity,
                                       stride, target, flags),
            "borrowed RGBA rows must convert");
    for (int row = 0; row < rowCount; ++row) {
        const uchar* actual = output.data() + prefix + qsizetype(row) * stride;
        if (std::memcmp(actual, expected.constScanLine(firstRow + row),
                        static_cast<std::size_t>(rowBytes)) != 0) {
            std::cerr << "row conversion source=" << source.format() << " size=" << source.width()
                      << 'x' << source.height() << " target=" << target.isValid()
                      << " source-profile=" << source.colorSpace().description().toStdString()
                      << " equal-profile=" << (source.colorSpace() == target)
                      << " first=" << firstRow << " count=" << rowCount << " row=" << row << '\n';
            for (qsizetype byte = 0; byte < rowBytes; ++byte)
                if (actual[byte] != expected.constScanLine(firstRow + row)[byte]) {
                    std::cerr << "first differing byte=" << byte
                              << " actual=" << static_cast<int>(actual[byte]) << " expected="
                              << static_cast<int>(expected.constScanLine(firstRow + row)[byte])
                              << '\n';
                    break;
                }
        }
        require(std::memcmp(actual, expected.constScanLine(firstRow + row),
                            static_cast<std::size_t>(rowBytes)) == 0,
                "borrowed RGBA rows must exactly match Qt pixel/color/alpha rounding");
        require(std::all_of(actual + rowBytes, actual + stride,
                            [](uchar byte) { return byte == sentinel; }),
                "row conversion must leave destination padding unchanged");
    }
    require(std::all_of(output.begin(), output.begin() + prefix,
                        [](uchar byte) { return byte == sentinel; }) &&
                std::all_of(output.end() - suffix, output.end(),
                            [](uchar byte) { return byte == sentinel; }),
            "row conversion must remain within borrowed destination bounds");
    require(source.constBits() == sourcePixels && source.colorTable() == originalPalette,
            "row conversion must retain source storage, palette and lifetime");

    // The last row does not need trailing padding in the destination capacity.
    const qsizetype exactCapacity = (rowCount - 1) * stride + rowBytes;
    require(snowCanvasCopyRgba8888Rows(source, firstRow, rowCount, output.data() + prefix,
                                       exactCapacity, stride, target, flags),
            "row conversion must accept a destination ending at the last pixel");
}

void borrowedRgbaRowsMatchQt() {
    const QImage small = alphaFixture(QSize(65, 41));
    for (int index = 1; index < QImage::NImageFormats; ++index) {
        const QImage source = small.convertToFormat(static_cast<QImage::Format>(index));
        require(!source.isNull(), "row format fixture must convert");
        requireRgbaRowsEquivalent(source, 3, 31);
    }
    for (const QSize& size : {QSize(1031, 263), QSize(70001, 7), QSize(17, 20001)}) {
        const QImage fixture = alphaFixture(size);
        for (const auto format :
             {QImage::Format_RGBA8888, QImage::Format_ARGB32_Premultiplied,
              QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA16FPx4_Premultiplied,
              QImage::Format_RGBA32FPx4, QImage::Format_Grayscale16, QImage::Format_Mono,
              QImage::Format_MonoLSB, QImage::Format_Indexed8}) {
            const QImage source = fixture.convertToFormat(format);
            requireRgbaRowsEquivalent(source, 1, source.height() - 2);
            requireRgbaRowsEquivalent(source, 1, source.height() - 2, QColorSpace::SRgb);
        }
    }
    const auto dither =
        Qt::ImageConversionFlags(Qt::PreferDither | Qt::OrderedDither | Qt::OrderedAlphaDither);
    requireRgbaRowsEquivalent(small, 7, 23, {}, dither);
    requireRgbaRowsEquivalent(small, 7, 23, QColorSpace::SRgb, dither);
    const QImage indexed = small.convertToFormat(QImage::Format_Indexed8);
    requireRgbaRowsEquivalent(indexed, 7, 23, QColorSpace::SRgb, dither);

    const qsizetype paddedStride = small.bytesPerLine() + 32;
    std::vector<uchar> storage(static_cast<std::size_t>(paddedStride * small.height()), 0x5a);
    for (int y = 0; y < small.height(); ++y)
        std::memcpy(storage.data() + qsizetype(y) * paddedStride, small.constScanLine(y),
                    static_cast<std::size_t>(small.width()) * 4);
    const auto original = storage;
    QImage readOnly(static_cast<const uchar*>(storage.data()), small.width(), small.height(),
                    paddedStride, small.format());
    readOnly.setColorSpace(small.colorSpace());
    requireRgbaRowsEquivalent(readOnly, 3, 29, QColorSpace::SRgb);
    require(storage == original, "row conversion must not modify a padded read-only source");
}

void borrowedRgbaRowsRejectInvalidBoundsAndAliasing() {
    QImage image = alphaFixture(QSize(17, 11));
    constexpr qsizetype stride = 17 * 4 + 12;
    std::array<uchar, static_cast<std::size_t>(stride * 11)> output{};
    const auto before = output;
    const auto reject = [&](int first, int count, uchar* destination, qsizetype capacity,
                            qsizetype rowStride, const QColorSpace& target = {}) {
        require(!snowCanvasCopyRgba8888Rows(image, first, count, destination, capacity, rowStride,
                                            target),
                "invalid or overlapping row destinations must fail");
        require(output == before, "validation failure must leave destination untouched");
    };
    reject(-1, 1, output.data(), static_cast<qsizetype>(output.size()), stride);
    reject(0, -1, output.data(), static_cast<qsizetype>(output.size()), stride);
    reject(0, 0, output.data(), static_cast<qsizetype>(output.size()), stride);
    reject(10, 2, output.data(), static_cast<qsizetype>(output.size()), stride);
    reject(std::numeric_limits<int>::max(), 1, output.data(), static_cast<qsizetype>(output.size()),
           stride);
    reject(0, 1, nullptr, static_cast<qsizetype>(output.size()), stride);
    reject(0, 1, output.data(), -1, stride);
    reject(0, 1, output.data(), 17 * 4 - 1, stride);
    reject(0, 1, output.data(), static_cast<qsizetype>(output.size()), -stride);
    reject(0, 1, output.data(), static_cast<qsizetype>(output.size()), 17 * 4 - 1);
    reject(0, 11, output.data(), static_cast<qsizetype>(output.size()),
           std::numeric_limits<qsizetype>::max());
    reject(0, 1, image.bits(), image.sizeInBytes(), image.bytesPerLine());
    reject(0, 1, image.bits() + 1, image.sizeInBytes(), image.bytesPerLine());
    image.setColorSpace({});
    reject(0, 1, output.data(), static_cast<qsizetype>(output.size()), stride, QColorSpace::SRgb);
    require(snowCanvasCopyRgba8888Rows(image, 0, 11, output.data(),
                                       static_cast<qsizetype>(output.size()), stride),
            "unprofiled images must support format-only row transfer");
    require(!snowCanvasCopyRgba8888Rows({}, 0, 1, output.data(),
                                        static_cast<qsizetype>(output.size()), stride),
            "null row sources must fail");
}

void requireEquivalent(QImage actual, const QImage& expected, const char* message) {
    if (actual != expected) {
        std::cerr << "format=" << actual.format() << " size=" << actual.width() << 'x'
                  << actual.height() << '\n';
        for (int y = 0, differences = 0; y < expected.height() && differences < 3; ++y)
            for (int x = 0; x < expected.width() && differences < 3; ++x)
                if (actual.pixel(x, y) != expected.pixel(x, y)) {
                    std::cerr << "diff " << x << ',' << y << ' ' << std::hex << actual.pixel(x, y)
                              << " != " << expected.pixel(x, y) << std::dec << '\n';
                    ++differences;
                }
    }
    require(actual == expected && actual.format() == expected.format(), message);
    const bool metadataMatches =
        actual.colorSpace() == expected.colorSpace() &&
        actual.devicePixelRatio() == expected.devicePixelRatio() &&
        actual.dotsPerMeterX() == expected.dotsPerMeterX() &&
        actual.dotsPerMeterY() == expected.dotsPerMeterY() &&
        actual.offset() == expected.offset() &&
        actual.text(QStringLiteral("fixture")) == expected.text(QStringLiteral("fixture"));
    if (!metadataMatches)
        std::cerr << message << " format=" << actual.format()
                  << " dpr=" << actual.devicePixelRatio() << '/' << expected.devicePixelRatio()
                  << " dpm=" << actual.dotsPerMeterX() << '/' << expected.dotsPerMeterX() << ','
                  << actual.dotsPerMeterY() << '/' << expected.dotsPerMeterY()
                  << " offset=" << actual.offset().x() << ',' << actual.offset().y() << '/'
                  << expected.offset().x() << ',' << expected.offset().y() << '\n';
    require(metadataMatches, "optimized operations must preserve Qt metadata");
    if (actual.isDetached() &&
        actual.sizeInBytes() >= qsizetype(snow::memory::kMappedPixelBufferMinimum)) {
        const auto* middle = actual.constBits() + actual.sizeInBytes() / 2;
        require(virtualMemoryMapped(middle), "large optimized results must own mapped pixels");
        actual = {};
        if (virtualMemoryMapped(middle))
            std::cerr << "retained: " << message << " format=" << expected.format()
                      << " size=" << expected.width() << 'x' << expected.height() << '\n';
        require(!virtualMemoryMapped(middle), "large optimized results must release their pages");
    }
}

QImage nativeColorResult(const QImage& source, QImage::Format format,
                         Qt::ImageConversionFlags flags = Qt::AutoColor) {
    QImage expected = source.convertedToColorSpace(QColorSpace::SRgb, format, flags);
    // The managed helper promises source metadata preservation. Qt's color
    // converter copies physical resolution and text but drops placement offset.
    expected.setOffset(source.offset());
    return expected;
}

void formatsAndAlphaBoundariesMatchQt() {
    const QImage small = alphaFixture(QSize(257, 129));
    const std::array formats{QImage::Format_Mono,
                             QImage::Format_MonoLSB,
                             QImage::Format_Indexed8,
                             QImage::Format_RGB16,
                             QImage::Format_RGB888,
                             QImage::Format_BGR888,
                             QImage::Format_RGB32,
                             QImage::Format_ARGB32,
                             QImage::Format_ARGB32_Premultiplied,
                             QImage::Format_RGBA8888,
                             QImage::Format_RGBA8888_Premultiplied,
                             QImage::Format_RGBX8888,
                             QImage::Format_RGB30,
                             QImage::Format_A2BGR30_Premultiplied,
                             QImage::Format_Alpha8,
                             QImage::Format_Grayscale8,
                             QImage::Format_Grayscale16,
                             QImage::Format_RGBA64,
                             QImage::Format_RGBA64_Premultiplied,
                             QImage::Format_RGBX64,
                             QImage::Format_RGBX16FPx4,
                             QImage::Format_RGBA16FPx4,
                             QImage::Format_RGBA16FPx4_Premultiplied,
                             QImage::Format_RGBX32FPx4,
                             QImage::Format_RGBA32FPx4,
                             QImage::Format_RGBA32FPx4_Premultiplied,
                             QImage::Format_CMYK8888};
    for (const auto format : formats) {
        const QImage source = small.convertToFormat(format);
        require(!source.isNull(), "format fixture must convert");
        requireEquivalent(snowCanvasCopyImage(source), source.copy(),
                          "whole-image copies must retain all pixel formats");
        const QRect crop(-3, -1, 263, 133);
        requireEquivalent(snowCanvasCopyImage(source, crop), source.copy(crop),
                          "partially covered crops must retain packed and byte formats");
        for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation}) {
            for (const auto& transform :
                 {QTransform().rotate(90), QTransform().rotate(180), QTransform().rotate(270),
                  QTransform().scale(-1, 1), QTransform().scale(1, -1),
                  QTransform().rotate(90).scale(-1, 1)}) {
                QImage actual = snowCanvasTransformImage(source, transform, mode);
                const QImage expected = source.transformed(transform, mode);
                if (actual != expected)
                    std::cerr << "mode=" << mode << " matrix=" << transform.m11() << ','
                              << transform.m12() << ',' << transform.m21() << ',' << transform.m22()
                              << '\n';
                requireEquivalent(std::move(actual), expected,
                                  "tiled transforms must match Qt for every pixel format");
            }
        }
    }
    const QImage large = alphaFixture(QSize(1025, 1024));
    for (const auto format :
         {QImage::Format_RGB32, QImage::Format_ARGB32, QImage::Format_RGBA8888,
          QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA8888_Premultiplied,
          QImage::Format_RGBA64, QImage::Format_RGBA64_Premultiplied, QImage::Format_RGBA32FPx4,
          QImage::Format_RGBA32FPx4_Premultiplied}) {
        const QImage source = large.convertToFormat(format);
        for (const auto target :
             {QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA8888_Premultiplied})
            requireEquivalent(snowCanvasConvertImage(source, target),
                              source.convertToFormat(target),
                              "premultiplication must retain all alpha edge values");
        for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation})
            for (const auto& transform : {QTransform().rotate(90), QTransform().rotate(270),
                                          QTransform().scale(-1, 1), QTransform().scale(1, -1)})
                requireEquivalent(snowCanvasTransformImage(source, transform, mode),
                                  source.transformed(transform, mode),
                                  "large mapped transforms must preserve alpha rounding");
    }
}

void directRgba64ExpansionMatchesQt() {
    const std::array<Qt::ImageConversionFlags, 3> flags{
        Qt::AutoColor, Qt::PreferDither | Qt::OrderedDither, Qt::AvoidDither | Qt::ThresholdDither};
    for (const QSize& size :
         {QSize(512, 256), QSize(513, 257), QSize(514, 257), QSize(1031, 517), QSize(70001, 3)}) {
        const QImage fixture = alphaFixture(size);
        for (const auto format : {QImage::Format_RGBA8888, QImage::Format_ARGB32}) {
            const QImage source = fixture.convertToFormat(format);
            const QImage original = source.copy();
            const uchar* pixels = source.constBits();
            for (const auto conversionFlags : flags)
                requireEquivalent(
                    snowCanvasConvertImage(source, QImage::Format_RGBA64, conversionFlags),
                    source.convertToFormat(QImage::Format_RGBA64, conversionFlags),
                    "direct RGBA64 expansion must preserve every channel and metadata");
            require(source == original && source.constBits() == pixels,
                    "direct RGBA64 expansion must leave its source pixels and storage unchanged");

            const qsizetype stride = source.bytesPerLine() + 32;
            constexpr qsizetype offset = 4;
            auto storage = snow::memory::allocatePixelBuffer(
                static_cast<std::size_t>(stride * source.height() + offset));
            require(storage != nullptr, "padded conversion fixture must allocate");
            uchar* paddedPixels = storage.get() + offset;
            for (int y = 0; y < source.height(); ++y)
                std::memcpy(paddedPixels + qsizetype(y) * stride, source.constScanLine(y),
                            static_cast<std::size_t>(source.width()) * 4);
            QImage readOnly(static_cast<const uchar*>(paddedPixels), source.width(),
                            source.height(), stride, format);
            readOnly.setColorSpace(source.colorSpace());
            readOnly.setDevicePixelRatio(source.devicePixelRatio());
            readOnly.setDotsPerMeterX(source.dotsPerMeterX());
            readOnly.setDotsPerMeterY(source.dotsPerMeterY());
            readOnly.setOffset(source.offset());
            readOnly.setText(QStringLiteral("fixture"), source.text(QStringLiteral("fixture")));
            requireEquivalent(snowCanvasConvertImage(readOnly, QImage::Format_RGBA64),
                              readOnly.convertToFormat(QImage::Format_RGBA64),
                              "direct RGBA64 expansion must respect padded read-only source rows");
            require(readOnly.constBits() == paddedPixels && readOnly == original,
                    "direct RGBA64 expansion must preserve a read-only lease");
        }
    }
}

void arbitraryRotationsMatchQt() {
    const QImage fixture = alphaFixture(QSize(1031, 517));
    for (const auto format :
         {QImage::Format_RGB32, QImage::Format_ARGB32, QImage::Format_ARGB32_Premultiplied,
          QImage::Format_RGBA8888, QImage::Format_RGBA8888_Premultiplied}) {
        QImage source = fixture.convertToFormat(format);
        source.detach();
        const QImage original = source.copy();
        const uchar* pixels = source.constBits();
        for (const qreal dpr : {qreal{1}, qreal{1.25}}) {
            source.setDevicePixelRatio(dpr);
            for (const auto& transform : {QTransform().rotate(17), QTransform().rotate(89.5),
                                          QTransform().translate(-0.25, 0.5).rotate(-33.25)})
                for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation})
                    requireEquivalent(
                        snowCanvasTransformImage(source, transform, mode),
                        source.transformed(transform, mode),
                        "arbitrary rotations must retain Qt bounds, alpha and metadata");
        }
        require(source == original && source.constBits() == pixels,
                "arbitrary rotation must leave source pixels and storage unchanged");
    }
    const QImage small = alphaFixture(QSize(257, 129));
    for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation})
        requireEquivalent(snowCanvasTransformImage(small, QTransform().rotate(-17), mode),
                          small.transformed(QTransform().rotate(-17), mode),
                          "small arbitrary rotations must initialize transparent corners");

    QImage large = snowCanvasAllocateImage(QSize(3840, 2160), QImage::Format_RGBA8888);
    large.fill(QColor(113, 79, 211, 127));
    large.setDevicePixelRatio(1.5);
    large.setColorSpace(QColorSpace::DisplayP3);
    const QTransform rotation = QTransform().rotate(17);
    const QImage expected = large.transformed(rotation, Qt::SmoothTransformation);
    require(expected.size() == QSize(4305, 3189),
            "Qt's aligned polygon bounds must include the fractional rotation edge");
    requireEquivalent(snowCanvasTransformImage(large, rotation, Qt::SmoothTransformation), expected,
                      "4K arbitrary rotation must preserve the entire aligned output");

    const qsizetype stride = fixture.bytesPerLine() + 32;
    auto storage =
        snow::memory::allocatePixelBuffer(static_cast<std::size_t>(stride * fixture.height()));
    require(storage != nullptr, "padded rotation fixture must allocate");
    for (int y = 0; y < fixture.height(); ++y)
        std::memcpy(storage.get() + qsizetype(y) * stride, fixture.constScanLine(y),
                    static_cast<std::size_t>(fixture.width()) * 4);
    QImage readOnly(static_cast<const uchar*>(storage.get()), fixture.width(), fixture.height(),
                    stride, fixture.format());
    readOnly.setColorSpace(fixture.colorSpace());
    for (const qreal dpr : {qreal{1}, qreal{1.25}}) {
        readOnly.setDevicePixelRatio(dpr);
        for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation})
            requireEquivalent(
                snowCanvasTransformImage(readOnly, rotation, mode),
                readOnly.transformed(rotation, mode),
                "padded read-only rotation sources must retain native Qt row semantics");
    }
    require(readOnly.constBits() == storage.get() && readOnly == fixture,
            "rotation must preserve a read-only source lease");
    for (const auto format :
         {QImage::Format_RGBA64, QImage::Format_RGBA64_Premultiplied, QImage::Format_Grayscale16}) {
        const QImage source = fixture.convertToFormat(format);
        for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation})
            requireEquivalent(snowCanvasTransformImage(source, rotation, mode),
                              source.transformed(rotation, mode),
                              "uncommon rotation formats must retain their native Qt fallback");
    }
}

void chunkBoundariesAndConcurrencyMatchQt() {
    for (const QSize& size : {QSize(70001, 17), QSize(17, 20001)}) {
        const QImage source = alphaFixture(size);
        for (const auto target :
             {QImage::Format_ARGB32_Premultiplied, QImage::Format_RGBA64, QImage::Format_RGB16}) {
            requireEquivalent(snowCanvasConvertImage(source, target),
                              source.convertToFormat(target),
                              "wide and tall conversions must preserve every pixel");
            requireEquivalent(snowCanvasColorConvertedImage(source, QColorSpace::SRgb, target),
                              nativeColorResult(source, target),
                              "wide and tall color conversions must preserve every pixel");
        }
        for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation})
            for (const auto& transform : {QTransform().scale(-1, 1), QTransform().scale(1, -1)}) {
                QImage actual = snowCanvasTransformImage(source, transform, mode);
                const QImage expected = source.transformed(transform, mode);
                if (actual != expected)
                    std::cerr << "wide mirror mode=" << mode << " x=" << transform.m11()
                              << " y=" << transform.m22() << '\n';
                requireEquivalent(std::move(actual), expected,
                                  "wide and tall mirrors must preserve SIMD tail rounding");
            }
    }
    const QImage source = alphaFixture(QSize(1031, 517));
    const auto flags = Qt::PreferDither | Qt::OrderedDither;
    for (const auto format :
         {QImage::Format_RGB16, QImage::Format_RGB444, QImage::Format_A2BGR30_Premultiplied}) {
        requireEquivalent(snowCanvasConvertImage(source, format, flags),
                          source.convertToFormat(format, flags),
                          "chunked conversion must preserve global ordered-dither phase");
        requireEquivalent(snowCanvasColorConvertedImage(source, QColorSpace::SRgb, format, flags),
                          nativeColorResult(source, format, flags),
                          "chunked color conversion must preserve global ordered-dither phase");
    }
    for (const QSize& size : {QSize(9, 40001), QSize(70001, 5)}) {
        const QImage narrowOrWide = alphaFixture(size);
        for (const auto format : {QImage::Format_RGB16, QImage::Format_A2BGR30_Premultiplied})
            requireEquivalent(snowCanvasConvertImage(narrowOrWide, format, flags),
                              narrowOrWide.convertToFormat(format, flags),
                              "narrow and wide tiles must preserve global dither phase");
    }
    for (const auto& transform : {QTransform().shear(1, 0), QTransform().shear(0, -1),
                                  QTransform().translate(13, -7).shear(1, 0),
                                  QTransform().translate(-11, 5).rotate(90).shear(0, 1),
                                  QTransform().translate(0.5, 0.25).rotate(90),
                                  QTransform().translate(-0.25, 0.5).scale(-1, 1),
                                  QTransform().translate(0.75, -0.25).scale(1, -1)})
        for (const auto mode : {Qt::FastTransformation, Qt::SmoothTransformation})
            requireEquivalent(snowCanvasTransformImage(source, transform, mode),
                              source.transformed(transform, mode),
                              "noncanonical transforms must retain Qt resampling and metadata");

    QThreadPool* pool = QThreadPool::globalInstance();
    const int maximumThreads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore complete;
    pool->start([&] {
        requireEquivalent(snowCanvasConvertImage(source, QImage::Format_RGBA64),
                          source.convertToFormat(QImage::Format_RGBA64),
                          "direct RGBA64 expansion on a saturated pool must complete correctly");
        requireEquivalent(snowCanvasColorConvertedImage(source, QColorSpace::SRgb,
                                                        QImage::Format_ARGB32_Premultiplied),
                          nativeColorResult(source, QImage::Format_ARGB32_Premultiplied),
                          "conversion on a saturated worker pool must complete correctly");
        complete.release();
    });
    require(complete.tryAcquire(1, 10000), "saturated worker conversion must not deadlock");
    pool->waitForDone();
    pool->setMaxThreadCount(maximumThreads);
}
} // namespace

int main() {
    rawBuffersReturnPages();
    imageSharingAndPaintingPreserveOwnership();
    imageLayoutAndInvalidSizes();
    monochromeImagesPreserveDefaultPalette();
    rasterOperationsPreservePixelsAndReleasePages();
    pixelArraysReleaseOriginalCapacity();
    zeroedImagesInitializePixelsAndPadding();
    borrowedRgbaRowsMatchQt();
    borrowedRgbaRowsRejectInvalidBoundsAndAliasing();
    formatsAndAlphaBoundariesMatchQt();
    directRgba64ExpansionMatchesQt();
    arbitraryRotationsMatchQt();
    chunkBoundariesAndConcurrencyMatchQt();
    return 0;
}
