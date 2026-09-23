#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H

#include <QColor>
#include <QImage>
#include <QMargins>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QRegion>
#include <QPainterPath>
#include <optional>

class QPainter;

struct ScreenshotResultStyle {
    int cornerRadius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
    // Immutable geometry snapshot, relative to the content origin, in canvas units.
    std::optional<QRegion> region;
    qreal regionScale = 1.0;
};

struct ScreenshotResultLayout {
    QRect contentRect;
    QRect outputRect;
    QMargins effectInsets;
    qreal devicePixelRatio = 1.0;

    [[nodiscard]] bool isValid() const {
        return contentRect.isValid() && !contentRect.isEmpty() && outputRect.isValid() &&
               !outputRect.isEmpty() && outputRect.contains(contentRect);
    }
};

[[nodiscard]] QPainterPath screenshotRegionPath(const QRegion& region, qreal radius = 0.0);

class ScreenshotResultCompositor final {
  public:
    [[nodiscard]] static ScreenshotResultStyle normalizedStyle(const ScreenshotResultStyle& style);
    [[nodiscard]] static ScreenshotResultLayout layoutForContent(const QSize& contentPixelSize,
                                                                 const ScreenshotResultStyle& style,
                                                                 qreal devicePixelRatio = 1.0);
    static void restoreBakedExterior(QImage& image, const QImage& background,
                                     const QPainterPath& path);
    [[nodiscard]] static QImage normalizeImage(const QImage& image);
    [[nodiscard]] static QImage compose(const QImage& content, const ScreenshotResultStyle& style,
                                        qreal devicePixelRatio = 1.0, qreal outputOpacity = 1.0);

    // Clear the part of the viewport outside the result shape, then place the
    // shadow behind the content. Content may cross the viewport boundary:
    // native pixel extents and integer-DIP widget extents round independently.
    static void finishLiveSurface(QPainter& painter, const QRectF& viewportBounds,
                                  const QRectF& contentBounds, const ScreenshotResultStyle& style,
                                  qreal devicePixelRatio, qreal canvasToViewScale = 1.0);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
