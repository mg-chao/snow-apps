#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLER_H

#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QRect>
#include <QSize>

class QWidget;

class ScreenshotCanvasColorSampler final {
  public:
    static constexpr int PreviewSize = 7;

    void reset();
    [[nodiscard]] bool ensureSnapshot(QWidget& canvas, const QRect& physicalBounds);
    [[nodiscard]] QImage previewAtPhysicalPoint(const QPoint& physicalPoint) const;

    [[nodiscard]] static QPoint physicalPointForLocalPosition(const QPointF& localPosition,
                                                              const QSize& localSize,
                                                              const QRect& physicalBounds);
    [[nodiscard]] static QImage previewFromPhysicalRaster(const QImage& physicalRaster,
                                                          const QRect& physicalBounds,
                                                          const QPoint& physicalPoint);

  private:
    QPointer<QWidget> m_canvas;
    QImage m_physicalRaster;
    QRect m_physicalBounds;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCANVASCOLORSAMPLER_H
