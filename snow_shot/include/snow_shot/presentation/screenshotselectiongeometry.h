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

// Pointer positions address whole canvas pixels: the cursor hotspot sits on the
// pixel cell [round(p), round(p) + 1) and never on the half-open boundary that
// selection rectangles store. Rounding also absorbs the floating-point noise
// introduced by logical-to-physical coordinate round trips on scaled displays.
[[nodiscard]] QPointF screenshotPointerPixelCell(const QPointF& position);

// Marquee span between two pointer positions: inclusive of both pointer cells,
// so the pressed and released pixels both stay inside the selection. Each axis
// spans max - min + 1 cells, so a drag kept on one pointer row or column still
// selects that single pixel strip; the span is empty only when both endpoints
// round onto the same cell (a click is not a selection). The span is not
// clamped to a canvas; pointer-driven drag paths apply the shared bounds clamp
// so pointer cells rounding onto an exclusive canvas edge cannot grow the
// selection past the canvas.
[[nodiscard]] QRectF marqueeScreenshotSelectionRect(const QPointF& start, const QPointF& end);

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
