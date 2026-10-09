#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QStringView>

#include <optional>

namespace screenshot_pinned_resize_geometry {
enum class DragHandle {
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
};

enum class ScaleAnchor {
    MousePosition,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    Center,
};

// Outside-content side strips resize their nearest side. Outside both axes
// selects a corner; the existing inside-content edge band remains available.
[[nodiscard]] std::optional<DragHandle> hitHandle(const QPointF& position, const QRectF& outer,
                                                  const QRectF& content, const QSizeF& edgeBand);

[[nodiscard]] QSize dpiScaledOuterSize(const QSize& pendingOuter, int oldDpi, int newDpi,
                                       int frameMargin);

[[nodiscard]] QSize scaledSize(const QSize& baseline, double scale);

struct TrackSizeLimits {
    QSize minimum;
    QSize maximum;
};

[[nodiscard]] TrackSizeLimits trackSizeLimits(const QSize& minimum, const QSize& maximum,
                                              const QSize& current, const QSize& target);

[[nodiscard]] ScaleAnchor scaleAnchorFromSetting(QStringView value);

[[nodiscard]] QRect anchoredScaleRect(const QRect& reference, const QSize& targetSize,
                                      ScaleAnchor anchor, const QPointF& mousePosition = QPointF());

[[nodiscard]] bool proportionalResizeRect(const QRect& proposed, const QRect& reference,
                                          const QSize& baseline, DragHandle handle,
                                          double minimumScale, double maximumScale, QRect* result,
                                          int frameMargin = 0);

[[nodiscard]] bool dragResizeRect(const QRect& reference, const QPoint& delta,
                                  const QSize& baseline, DragHandle pressed, double minimumScale,
                                  double maximumScale, DragHandle* effective, QRect* result,
                                  int frameMargin = 0);
} // namespace screenshot_pinned_resize_geometry

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H
