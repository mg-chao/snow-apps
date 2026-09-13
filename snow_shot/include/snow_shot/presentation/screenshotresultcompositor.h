#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H

#include <QColor>
#include <QImage>
#include <QMargins>
#include <QRect>
#include <QRectF>
#include <QSize>

class QPainter;

struct ScreenshotResultStyle {
    int cornerRadius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
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

class ScreenshotResultCompositor final {
  public:
    [[nodiscard]] static ScreenshotResultStyle normalizedStyle(const ScreenshotResultStyle& style);
    [[nodiscard]] static ScreenshotResultLayout layoutForContent(const QSize& contentPixelSize,
                                                                 const ScreenshotResultStyle& style,
                                                                 qreal devicePixelRatio = 1.0);
    [[nodiscard]] static QImage normalizeImage(const QImage& image);
    [[nodiscard]] static QImage compose(const QImage& content, const ScreenshotResultStyle& style,
                                        qreal devicePixelRatio = 1.0);

    // Called after the live canvas has painted. It clips every existing pixel
    // to the result shape, then places the shared shadow behind that content.
    static void finishLiveSurface(QPainter& painter, const QRectF& viewportBounds,
                                  const QRectF& contentBounds, const ScreenshotResultStyle& style,
                                  qreal devicePixelRatio, qreal canvasToViewScale = 1.0);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
