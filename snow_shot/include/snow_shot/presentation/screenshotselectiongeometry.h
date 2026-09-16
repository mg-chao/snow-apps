#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONGEOMETRY_H

#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QtGlobal>

#include <optional>

enum class ScreenshotSelectionDragMode : int {
    None = 0,
    All,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Marquee,
};

[[nodiscard]] QRectF normalizedScreenshotSelection(const QPointF& start, const QPointF& end);

[[nodiscard]] QRect screenshotPixelRectForSelection(const QRectF& selection);

[[nodiscard]] ScreenshotSelectionDragMode
screenshotSelectionDragModeForPoint(const QRectF& selection, const QPointF& point, bool borderOnly,
                                    qreal edgeTolerance, qreal minimumSelectionSize);

[[nodiscard]] QRectF boundedScreenshotSelectionRect(const QRectF& selection, const QRectF& bounds,
                                                    bool preserveSize, qreal minimumSelectionSize);

[[nodiscard]] QRectF draggedScreenshotSelectionRect(ScreenshotSelectionDragMode dragMode,
                                                    const QRectF& origin,
                                                    const QPointF& originPosition,
                                                    const QPointF& position, const QRectF& bounds,
                                                    qreal minimumSelectionSize,
                                                    qreal lockedAspectRatio = 0.0);

// Move the grabbed border/corner onto the press position before a resize drag
// captures its origin, so the dragged edge tracks the pointer position itself
// instead of the pointer movement. Opposite edges stay anchored; whole-selection
// and marquee drags return the selection unchanged.
[[nodiscard]] QRectF grabAdjustedScreenshotSelectionRect(ScreenshotSelectionDragMode dragMode,
                                                         const QRectF& selection,
                                                         const QPointF& position,
                                                         const QRectF& bounds,
                                                         qreal minimumSelectionSize);

[[nodiscard]] std::optional<QPointF>
screenshotSelectionDragAnchor(const QRectF& selection, ScreenshotSelectionDragMode dragMode,
                              const QPointF& position, qreal minimumSelectionSize);

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTIONGEOMETRY_H
