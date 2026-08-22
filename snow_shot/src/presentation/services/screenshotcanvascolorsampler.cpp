#include "snow_shot/presentation/screenshotcanvascolorsampler.h"

#include <QPixmap>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace {
int rasterCoordinateForPhysicalPixel(int physicalCoordinate, int physicalStart, int physicalExtent,
                                     int rasterExtent) {
    if (physicalExtent <= 0 || rasterExtent <= 0) {
        return 0;
    }
    const qint64 offset = std::clamp<qint64>(
        static_cast<qint64>(physicalCoordinate) - physicalStart, 0, physicalExtent - 1);
    const qint64 centeredNumerator = (offset * 2 + 1) * rasterExtent;
    const qint64 centeredDenominator = static_cast<qint64>(physicalExtent) * 2;
    return std::clamp(static_cast<int>(centeredNumerator / centeredDenominator), 0,
                      rasterExtent - 1);
}
} // namespace

void ScreenshotCanvasColorSampler::reset() {
    m_canvas.clear();
    m_physicalRaster = {};
    m_physicalBounds = {};
}

bool ScreenshotCanvasColorSampler::ensureSnapshot(QWidget& canvas, const QRect& physicalBounds) {
    const QRect normalizedBounds = physicalBounds.normalized();
    if (!normalizedBounds.isValid() || normalizedBounds.isEmpty()) {
        reset();
        return false;
    }
    if (m_canvas == &canvas && m_physicalBounds == normalizedBounds && !m_physicalRaster.isNull()) {
        return true;
    }

    QImage raster = canvas.grab().toImage();
    if (raster.isNull()) {
        reset();
        return false;
    }
    raster.setDevicePixelRatio(1.0);
    m_canvas = &canvas;
    m_physicalRaster = std::move(raster);
    m_physicalBounds = normalizedBounds;
    return true;
}

QImage ScreenshotCanvasColorSampler::previewAtPhysicalPoint(const QPoint& physicalPoint) const {
    return previewFromPhysicalRaster(m_physicalRaster, m_physicalBounds, physicalPoint);
}

QPoint ScreenshotCanvasColorSampler::physicalPointForLocalPosition(const QPointF& localPosition,
                                                                   const QSize& localSize,
                                                                   const QRect& physicalBounds) {
    const QRect normalizedBounds = physicalBounds.normalized();
    if (!localSize.isValid() || localSize.isEmpty() || !normalizedBounds.isValid() ||
        normalizedBounds.isEmpty()) {
        return localPosition.toPoint();
    }

    const int x = qRound(normalizedBounds.left() +
                         localPosition.x() * normalizedBounds.width() / localSize.width());
    const int y = qRound(normalizedBounds.top() +
                         localPosition.y() * normalizedBounds.height() / localSize.height());
    return QPoint(std::clamp(x, normalizedBounds.left(), normalizedBounds.right()),
                  std::clamp(y, normalizedBounds.top(), normalizedBounds.bottom()));
}

QImage ScreenshotCanvasColorSampler::previewFromPhysicalRaster(const QImage& physicalRaster,
                                                               const QRect& physicalBounds,
                                                               const QPoint& physicalPoint) {
    const QRect normalizedBounds = physicalBounds.normalized();
    if (physicalRaster.isNull() || !normalizedBounds.isValid() || normalizedBounds.isEmpty() ||
        !normalizedBounds.contains(physicalPoint)) {
        return {};
    }

    constexpr int radius = PreviewSize / 2;
    QImage preview(PreviewSize, PreviewSize, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < PreviewSize; ++y) {
        const int physicalY = std::clamp(physicalPoint.y() + y - radius, normalizedBounds.top(),
                                         normalizedBounds.bottom());
        const int rasterY = rasterCoordinateForPhysicalPixel(
            physicalY, normalizedBounds.top(), normalizedBounds.height(), physicalRaster.height());
        for (int x = 0; x < PreviewSize; ++x) {
            const int physicalX = std::clamp(physicalPoint.x() + x - radius,
                                             normalizedBounds.left(), normalizedBounds.right());
            const int rasterX =
                rasterCoordinateForPhysicalPixel(physicalX, normalizedBounds.left(),
                                                 normalizedBounds.width(), physicalRaster.width());
            preview.setPixelColor(x, y, physicalRaster.pixelColor(rasterX, rasterY));
        }
    }
    return preview;
}
