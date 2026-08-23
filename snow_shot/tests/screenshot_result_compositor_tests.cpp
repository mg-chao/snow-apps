#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include <QCoreApplication>
#include <QImage>
#include <QPainter>

#include <cstring>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <utility>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

struct ScreenshotClipboardPayloadTestAccess {
#if defined(Q_OS_WIN) || defined(_WIN32)
    static HGLOBAL nativeHandle(const ScreenshotClipboardPayload& payload) {
        return static_cast<HGLOBAL>(payload.m_nativeHandle);
    }

    static ScreenshotClipboardFormatMode formatMode(const ScreenshotClipboardPayload& payload) {
        return payload.m_formatMode;
    }
#endif
};

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

QImage solidContent(const QSize& size = QSize(80, 48)) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(QColor(30, 100, 210, 255));
    image.setDevicePixelRatio(2.0);
    return image;
}

void squareResultPreservesPhysicalPixels() {
    const QImage result = ScreenshotResultCompositor::compose(solidContent(), {});
    require(result.size() == QSize(80, 48), "square output dimensions changed");
    require(result.format() == QImage::Format_ARGB32_Premultiplied,
            "result is not premultiplied");
    require(result.devicePixelRatio() == 1.0, "result DPR is not normalized");
    require(result.pixelColor(0, 0) == QColor(30, 100, 210, 255),
            "square output changed a source pixel");
}

void roundedAndShadowedResultHasRealTransparency() {
    const QImage roundedOnly = ScreenshotResultCompositor::compose(
        solidContent(), ScreenshotResultStyle{16, 0, QColor(20, 30, 40, 220)});
    require(roundedOnly.pixelColor(0, 0).alpha() == 0,
            "rounded-only content corner is not transparent");

    const ScreenshotResultStyle style{16, 12, QColor(20, 30, 40, 220)};
    const QImage result = ScreenshotResultCompositor::compose(solidContent(), style);
    require(result.size() == QSize(104, 72), "effect insets were not added to output");
    require(result.pixelColor(0, 0).alpha() == 0, "outer corner is not transparent");
    require(result.pixelColor(12, 12).alpha() < 255,
            "rounded content leaked opaque pixels into the shadow");
    require(result.pixelColor(12 + 40, 12 + 24).alpha() == 255,
            "content center became transparent");
    const int shadowAlpha = result.pixelColor(11, 12 + 24).alpha();
    require(shadowAlpha > 0 && shadowAlpha < 255,
            "shadow edge is not semitransparent");
}

void layoutScalesOnlyEffectsForFractionalDpr() {
    const ScreenshotResultStyle style{10, 8, QColor()};
    for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
        const ScreenshotResultLayout layout =
            ScreenshotResultCompositor::layoutForContent(QSize(100, 50), style, dpr);
        const int effect = qRound(8.0 * dpr);
        require(layout.isValid(), "fractional-DPR layout is invalid");
        require(layout.contentRect == QRect(effect, effect, 100, 50),
                "fractional-DPR content rect is wrong");
        require(layout.outputRect.size() == QSize(100 + effect * 2, 50 + effect * 2),
                "fractional-DPR output bounds are wrong");
    }
}

void noEffectResultSharesNormalizedStorage() {
    QImage source(QSize(32, 24), QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(30, 100, 210, 255));
    const QImage result = ScreenshotResultCompositor::compose(source, {});
    require(result.constBits() == source.constBits(), "no-effect result detached source pixels");
    require(result.size() == source.size(), "no-effect result changed dimensions");
}

void liveSurfaceClipsExistingCanvasPixelsBeforeAddingShadow() {
    QImage surface(QSize(70, 60), QImage::Format_ARGB32_Premultiplied);
    surface.fill(Qt::transparent);
    {
        QPainter painter(&surface);
        painter.fillRect(QRect(6, 6, 58, 48), QColor(220, 20, 20, 255));
        ScreenshotResultCompositor::finishLiveSurface(
            painter, surface.rect(), QRectF(12, 12, 46, 36),
            ScreenshotResultStyle{10, 6, QColor(0, 0, 0, 220)}, 1.0);
    }
    require(surface.pixelColor(6, 6).alpha() == 0,
            "canvas pixels leaked outside the result and shadow bounds");
    require(surface.pixelColor(12, 12).alpha() < 255,
            "canvas pixels leaked through the rounded corner");
    require(surface.pixelColor(35, 30).alpha() == 255,
            "live result center was clipped");
    const int shadowAlpha = surface.pixelColor(11, 30).alpha();
    require(shadowAlpha > 0 && shadowAlpha < 255,
            "live surface did not add a semitransparent shadow behind content");
}

void clipboardPayloadOwnsPreparedPixels() {
    require(!ScreenshotClipboardService::prepareImage({}).isValid(),
            "null image produced a clipboard payload");

    for (QImage::Format format : {QImage::Format_ARGB32,
                                  QImage::Format_ARGB32_Premultiplied,
                                  QImage::Format_RGBA8888}) {
        QImage source(QSize(3, 2), QImage::Format_ARGB32);
        source.setPixel(0, 0, qRgba(10, 20, 30, 40));
        source.setPixel(1, 0, qRgba(50, 60, 70, 80));
        source.setPixel(2, 0, qRgba(90, 100, 110, 120));
        source.setPixel(0, 1, qRgba(130, 140, 150, 160));
        source.setPixel(1, 1, qRgba(170, 180, 190, 200));
        source.setPixel(2, 1, qRgba(210, 220, 230, 240));
        source = source.convertToFormat(format);
        const QImage expected = source.convertToFormat(QImage::Format_ARGB32);

        ScreenshotClipboardPayload prepared = ScreenshotClipboardService::prepare(
            ScreenshotClipboardPixelSource(source));
        require(prepared.isValid(), "supported image format produced no clipboard payload");
        ScreenshotClipboardPayload moved = std::move(prepared);
        require(!prepared.isValid() && moved.isValid(),
                "clipboard payload move did not transfer ownership");

#if defined(Q_OS_WIN) || defined(_WIN32)
        require(ScreenshotClipboardPayloadTestAccess::formatMode(moved) ==
                    ScreenshotClipboardFormatMode::DibV5,
                "default clipboard payload did not retain CF_DIBV5");
        const HGLOBAL handle = ScreenshotClipboardPayloadTestAccess::nativeHandle(moved);
        require(handle != nullptr, "prepared DIBV5 has no native allocation");
        const auto* header = static_cast<const BITMAPV5HEADER*>(GlobalLock(handle));
        require(header != nullptr, "prepared DIB could not be locked");
        require(header->bV5Size == sizeof(BITMAPV5HEADER) && header->bV5Width == 3 &&
                    header->bV5Height == -2 && header->bV5Planes == 1 &&
                    header->bV5BitCount == 32 && header->bV5Compression == BI_BITFIELDS &&
                    header->bV5SizeImage == static_cast<DWORD>(expected.sizeInBytes()) &&
                    header->bV5RedMask == 0x00ff0000 &&
                    header->bV5GreenMask == 0x0000ff00 &&
                    header->bV5BlueMask == 0x000000ff &&
                    header->bV5AlphaMask == 0xff000000,
                "prepared DIB header is invalid");
        const auto* pixels = reinterpret_cast<const uchar*>(header + 1);
        require(std::memcmp(pixels, expected.constBits(),
                            static_cast<std::size_t>(expected.sizeInBytes())) == 0,
                "prepared DIB pixels do not match ARGB32 source bytes");
        GlobalUnlock(handle);
#endif
    }
}

void rgbaClipboardSourceUsesWindowsChannelOrder() {
    QImage source(QSize(2, 1), QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(10, 20, 30, 40));
    source.setPixelColor(1, 0, QColor(50, 60, 70, 80));
    const QImage expected = source.convertToFormat(QImage::Format_ARGB32);

    ScreenshotClipboardPayload prepared = ScreenshotClipboardService::prepare(
        ScreenshotClipboardPixelSource(source));
    require(prepared.isValid(), "RGBA clipboard source produced no payload");
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(ScreenshotClipboardPayloadTestAccess::formatMode(prepared) ==
                ScreenshotClipboardFormatMode::DibV5,
            "RGBA clipboard payload did not retain CF_DIBV5");
    const HGLOBAL handle = ScreenshotClipboardPayloadTestAccess::nativeHandle(prepared);
    const auto* header = static_cast<const BITMAPV5HEADER*>(GlobalLock(handle));
    require(header != nullptr, "RGBA clipboard payload could not be locked");
    const auto* pixels = reinterpret_cast<const uchar*>(header + 1);
    require(std::memcmp(pixels, expected.constBits(),
                        static_cast<std::size_t>(expected.sizeInBytes())) == 0,
            "RGBA clipboard payload channel order is incorrect");
    GlobalUnlock(handle);
#endif
}

void compatibleDibPayloadUsesBitmapInfoHeader() {
    QImage source(QSize(2, 2), QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(10, 20, 30, 40));
    source.setPixelColor(1, 0, QColor(50, 60, 70, 80));
    source.setPixelColor(0, 1, QColor(90, 100, 110, 120));
    source.setPixelColor(1, 1, QColor(130, 140, 150, 160));
    const QImage expected = source.convertToFormat(QImage::Format_ARGB32);

    ScreenshotClipboardPayload prepared = ScreenshotClipboardService::prepare(
        ScreenshotClipboardPixelSource(source), ScreenshotClipboardFormatMode::CompatibleDib);
    require(prepared.isValid(), "compatible DIB source produced no payload");
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(ScreenshotClipboardPayloadTestAccess::formatMode(prepared) ==
                ScreenshotClipboardFormatMode::CompatibleDib,
            "compatible DIB payload selected the wrong native format");
    const HGLOBAL handle = ScreenshotClipboardPayloadTestAccess::nativeHandle(prepared);
    require(handle != nullptr, "compatible DIB payload has no native allocation");
    const auto* header = static_cast<const BITMAPINFOHEADER*>(GlobalLock(handle));
    require(header != nullptr, "compatible DIB payload could not be locked");
    require(header->biSize == sizeof(BITMAPINFOHEADER) && header->biWidth == 2 &&
                header->biHeight == 2 && header->biPlanes == 1 && header->biBitCount == 32 &&
                header->biCompression == BI_RGB &&
                header->biSizeImage == static_cast<DWORD>(expected.sizeInBytes()),
            "compatible DIB header is invalid");
    const auto* pixels = reinterpret_cast<const uchar*>(header + 1);
    const std::size_t rowBytes = static_cast<std::size_t>(expected.bytesPerLine());
    require(std::memcmp(pixels, expected.constScanLine(1), rowBytes) == 0 &&
                std::memcmp(pixels + rowBytes, expected.constScanLine(0), rowBytes) == 0,
            "compatible DIB pixels are not bottom-up ARGB32 source bytes");
    GlobalUnlock(handle);
#endif
}

void scrollingRowSourceProducesCompatibleDib() {
    QImage source(QSize(2, 2), QImage::Format_RGBA8888);
    source.setPixelColor(0, 0, QColor(15, 25, 35, 255));
    source.setPixelColor(1, 0, QColor(45, 55, 65, 255));
    source.setPixelColor(0, 1, QColor(75, 85, 95, 255));
    source.setPixelColor(1, 1, QColor(105, 115, 125, 255));
    const QImage expected = source.convertToFormat(QImage::Format_ARGB32);

    ScreenshotImageRowSource rows;
    rows.size = source.size();
    rows.readRows = [source](int firstRow, int rowCount, qsizetype destinationStride,
                             uchar* destination, qsizetype destinationSize) {
        if (firstRow < 0 || rowCount < 0 || firstRow + rowCount > source.height() ||
            destination == nullptr || destinationStride < source.bytesPerLine() ||
            destinationSize < destinationStride * rowCount) {
            return false;
        }
        for (int row = 0; row < rowCount; ++row) {
            std::memcpy(destination + static_cast<qsizetype>(row) * destinationStride,
                        source.constScanLine(firstRow + row),
                        static_cast<std::size_t>(source.bytesPerLine()));
        }
        return true;
    };

    ScreenshotClipboardPayload prepared = ScreenshotClipboardService::prepare(
        rows, ScreenshotClipboardFormatMode::CompatibleDib);
    require(prepared.isValid(), "scrolling row source produced no compatible DIB payload");
#if defined(Q_OS_WIN) || defined(_WIN32)
    require(ScreenshotClipboardPayloadTestAccess::formatMode(prepared) ==
                ScreenshotClipboardFormatMode::CompatibleDib,
            "scrolling row payload selected the wrong native format");
    const HGLOBAL handle = ScreenshotClipboardPayloadTestAccess::nativeHandle(prepared);
    const auto* header = static_cast<const BITMAPINFOHEADER*>(GlobalLock(handle));
    require(header != nullptr && header->biHeight == expected.height() &&
                header->biCompression == BI_RGB,
            "scrolling row payload has an invalid compatible DIB header");
    const auto* pixels = reinterpret_cast<const uchar*>(header + 1);
    const std::size_t rowBytes = static_cast<std::size_t>(expected.bytesPerLine());
    require(std::memcmp(pixels, expected.constScanLine(1), rowBytes) == 0 &&
                std::memcmp(pixels + rowBytes, expected.constScanLine(0), rowBytes) == 0,
            "scrolling row payload pixels are not bottom-up BGRA data");
    GlobalUnlock(handle);
#endif
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        squareResultPreservesPhysicalPixels();
        roundedAndShadowedResultHasRealTransparency();
        layoutScalesOnlyEffectsForFractionalDpr();
        noEffectResultSharesNormalizedStorage();
        liveSurfaceClipsExistingCanvasPixelsBeforeAddingShadow();
        clipboardPayloadOwnsPreparedPixels();
        rgbaClipboardSourceUsesWindowsChannelOrder();
        compatibleDibPayloadUsesBitmapInfoHeader();
        scrollingRowSourceProducesCompatibleDib();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
