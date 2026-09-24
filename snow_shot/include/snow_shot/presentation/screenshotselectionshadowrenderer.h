#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSHADOWRENDERER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSHADOWRENDERER_H

#include <QColor>
#include <QImage>
#include <QRectF>

#include <cstddef>

class QPainter;
class QWidget;

struct ScreenshotSelectionShadowDiagnostics {
    std::size_t cacheHits = 0;
    std::size_t cacheBuilds = 0;
    std::size_t retainedBytes = 0;
    std::size_t retainedEntries = 0;
    std::size_t selectionSizedTransientAllocations = 0;
    std::size_t regionMaskBuilds = 0;
    std::size_t regionShadowBuilds = 0;
    std::size_t regionScratchPeakBytes = 0;
    std::size_t regionCacheRetainedBytes = 0;
    std::size_t regionPathCacheElements = 0;
};

class ScreenshotSelectionShadowRenderer final {
  public:
    static void renderPreview(QPainter& painter, const QRectF& selectionBounds, qreal cornerRadius,
                              qreal shadowWidth, const QColor& shadowColor, qreal devicePixelRatio,
                              const QWidget* widget = nullptr);

    static QImage composeExport(const QImage& content, int cornerRadius, int shadowWidth,
                                const QColor& shadowColor);

    static void renderResultShadow(QPainter& painter, const QRectF& contentBounds,
                                   qreal cornerRadius, qreal shadowWidth, const QColor& shadowColor,
                                   qreal devicePixelRatio);

    static ScreenshotSelectionShadowDiagnostics diagnosticsForCurrentThread();
    static void resetDiagnosticsForCurrentThread();
    // Clears rectangle shadow assets, rounded region contours, and export masks/shadows.
    // Must run on each owning thread after its session/export work has finished.
    static void resetCacheForCurrentThread();
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONSHADOWRENDERER_H
