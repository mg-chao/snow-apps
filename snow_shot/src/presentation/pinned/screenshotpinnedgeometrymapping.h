#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDGEOMETRYMAPPING_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDGEOMETRYMAPPING_H

#include <QRectF>
#include <QSizeF>
#include <QtMath>

#include <algorithm>
#include <cmath>

// A snapshot of the observed client, never a source of geometry requests.
// Extent mapping is used by QWidget hit testing. Painting instead uses DPR:
// integer QWidget dimensions need not cover exactly the native client extent.
class ScreenshotPinnedGeometryMapping final {
  public:
    ScreenshotPinnedGeometryMapping(const QRect& physicalClient, const QSizeF& logicalSize,
                                    qreal devicePixelRatio)
        : m_client(physicalClient), m_logicalSize(logicalSize), m_dpr(devicePixelRatio) {}

    [[nodiscard]] bool isValid() const {
        return m_client.isValid() && m_logicalSize.width() > 0 && m_logicalSize.height() > 0 &&
               std::isfinite(m_logicalSize.width()) && std::isfinite(m_logicalSize.height()) &&
               std::isfinite(m_dpr) && m_dpr > 0;
    }

    [[nodiscard]] QPointF nativePosition(const QPointF& local) const {
        if (!isValid())
            return {};
        return QPointF(m_client.x() + local.x() * m_client.width() / m_logicalSize.width(),
                       m_client.y() + local.y() * m_client.height() / m_logicalSize.height());
    }

    [[nodiscard]] QPointF localPosition(const QPointF& native) const {
        if (!isValid())
            return {};
        return QPointF((native.x() - m_client.x()) * m_logicalSize.width() / m_client.width(),
                       (native.y() - m_client.y()) * m_logicalSize.height() / m_client.height());
    }

    [[nodiscard]] bool containsNativePosition(const QPointF& native) const {
        return isValid() && native.x() >= m_client.x() && native.y() >= m_client.y() &&
               native.x() < qreal(m_client.x()) + m_client.width() &&
               native.y() < qreal(m_client.y()) + m_client.height();
    }

    [[nodiscard]] QSize nativeHitSize(qreal logicalWidth) const {
        if (!isValid())
            return {};
        return QSize(
            std::max(1, qRound(logicalWidth * m_client.width() / m_logicalSize.width())),
            std::max(1, qRound(logicalWidth * m_client.height() / m_logicalSize.height())));
    }

    [[nodiscard]] QSizeF logicalViewportSize() const {
        return isValid() ? QSizeF(m_client.size()) / m_dpr : QSizeF();
    }

    // Smallest integer DIP extent whose Qt native rounding still covers the
    // client. Ceil(client / DPR) can be one DIP larger, and qRound of that
    // size is then a device pixel past the HWND. On a fractional scale the
    // extra column is clipped from the origin side, so the screenshot slides
    // by one physical pixel inside an unmoved window.
    [[nodiscard]] static int minimumCoveringLogicalExtent(int nativePixels, qreal dpr) {
        if (nativePixels <= 0 || !(dpr > 0.0))
            return 0;
        int logical = std::max(1, qRound(static_cast<qreal>(nativePixels) / dpr));
        while (qRound(logical * dpr) < nativePixels)
            ++logical;
        return logical;
    }

    [[nodiscard]] QSize coveringLogicalSize() const {
        if (!isValid())
            return {};
        return QSize(minimumCoveringLogicalExtent(m_client.width(), m_dpr),
                     minimumCoveringLogicalExtent(m_client.height(), m_dpr));
    }

    [[nodiscard]] double viewportZoom(const QSizeF& canvasSize) const {
        if (!isValid() || canvasSize.width() <= 0 || canvasSize.height() <= 0)
            return 1.0;
        const QSizeF viewport = logicalViewportSize();
        const double x = viewport.width() / canvasSize.width();
        const double y = viewport.height() / canvasSize.height();
        return qFuzzyCompare(x, y) ? x : std::min(x, y);
    }

    [[nodiscard]] QPointF viewportCenterOffset(double zoom) const {
        if (!isValid() || !std::isfinite(zoom) || zoom <= 0)
            return {};
        const QSizeF viewport = logicalViewportSize();
        return QPointF(m_logicalSize.width() - viewport.width(),
                       m_logicalSize.height() - viewport.height()) /
               (2 * zoom);
    }

    // mappedExtent is already in painter device pixels (combinedTransform).
    // Round edges independently and clip surplus backing-store rows/columns.
    [[nodiscard]] QRect clippedDeviceRect(const QRectF& mappedExtent) const {
        if (!isValid())
            return {};
        return roundedDeviceRect(mappedExtent).intersected(QRect(QPoint(), m_client.size()));
    }

    [[nodiscard]] static QRect roundedDeviceRect(const QRectF& mappedExtent) {
        const QRect rounded(
            qRound(mappedExtent.x()), qRound(mappedExtent.y()),
            qRound(mappedExtent.x() + mappedExtent.width()) - qRound(mappedExtent.x()),
            qRound(mappedExtent.y() + mappedExtent.height()) - qRound(mappedExtent.y()));
        return rounded;
    }

  private:
    QRect m_client;
    QSizeF m_logicalSize;
    qreal m_dpr;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDGEOMETRYMAPPING_H
