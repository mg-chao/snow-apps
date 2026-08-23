#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H

#include <QPointF>
#include <QRect>
#include <QSize>
#include <QStringView>

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

[[nodiscard]] QSize scaledSize(const QSize& baseline, double scale);

[[nodiscard]] ScaleAnchor scaleAnchorFromSetting(QStringView value);

[[nodiscard]] QRect anchoredScaleRect(const QRect& reference, const QSize& targetSize,
                                      ScaleAnchor anchor, const QPointF& mousePosition = QPointF());

[[nodiscard]] bool proportionalResizeRect(const QRect& proposed, const QRect& reference,
                                          const QSize& baseline, DragHandle handle,
                                          double minimumScale, double maximumScale, QRect* result);
} // namespace screenshot_pinned_resize_geometry

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTPINNEDRESIZEGEOMETRY_H
