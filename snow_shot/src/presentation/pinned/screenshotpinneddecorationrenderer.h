#pragma once

#include <QColor>
#include <QRect>
#include <QSizeF>

#include <cstddef>
#include <memory>

class QPainter;

struct ScreenshotPinnedShadowDiagnostics {
    std::size_t geometryBuilds = 0;
    std::size_t geometryHits = 0;
    std::size_t nineSliceDraws = 0;
    std::size_t vectorDraws = 0;
    std::size_t retainedBytes = 0;
    std::size_t retainedPathElements = 0;
    std::size_t retainedSpanRectangles = 0;
    std::size_t retainedPaletteBytes = 0;
    std::size_t paletteBuilds = 0;
    std::size_t paletteHits = 0;
};

// Exterior vector spans belong to the pin. Colors and active/locked state never change geometry.
// Equal, small radii use the existing bounded nine-slice asset cache. The fallback allocates only
// along the finite-width perimeter; no window-sized or radius-squared surface is retained.
class ScreenshotPinnedShadowCache final {
  public:
    ScreenshotPinnedShadowCache();
    ~ScreenshotPinnedShadowCache();
    void clear();
    [[nodiscard]] ScreenshotPinnedShadowDiagnostics diagnostics() const;

  private:
    friend class ScreenshotPinnedDecorationRenderer;
    QRect m_outline;
    QSizeF m_radii;
    int m_width = 0;
    struct Geometry;
    std::unique_ptr<Geometry> m_geometry;
    ScreenshotPinnedShadowDiagnostics m_diagnostics;
};

class ScreenshotPinnedDecorationRenderer final {
  public:
    // All coordinates are physical pixels. The caller supplies a painter in device coordinates,
    // including a unit device-pixel ratio, to keep fractional Windows DPI margins exact.
    static void renderShadow(QPainter& painter, const QRect& deviceContentOutline,
                             const QSizeF& physicalCornerRadii, int physicalShadowWidth,
                             const QColor& color, const QRect& deviceOuter,
                             ScreenshotPinnedShadowCache& cache);
};
