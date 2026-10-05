#ifndef SNOW_SHOT_PRESENTATION_PINNEDWINDOWSELECTIONGEOMETRY_H
#define SNOW_SHOT_PRESENTATION_PINNEDWINDOWSELECTIONGEOMETRY_H

#include <QList>
#include <QRectF>

#include <optional>
#include <span>

enum class SnowCanvasSelectionAlignment;

namespace pinned_window_selection_geometry {
// Logical desktop rectangles use half-open edges. Results retain the input
// order and sizes; only their origins change. Invalid geometry, an unknown
// operation, or too few windows produces no targets.
[[nodiscard]] std::optional<QList<QRectF>> alignmentTargets(const QList<QRectF>& rectangles,
                                                            SnowCanvasSelectionAlignment alignment);

// Clamp once against every window's scale range so all members receive the
// same relative factor. An empty or inconsistent range produces no factor.
[[nodiscard]] std::optional<double> sharedScaleFactor(std::span<const double> currentScalePercents,
                                                      double requestedFactor,
                                                      double minimumPercent = 10.0,
                                                      double maximumPercent = 500.0);

// Scale each rectangle in place, retaining its own top-left anchor. Callers
// round the final native size once, and derive each update from the gesture's
// initial rectangles rather than repeatedly scaling rounded frames.
[[nodiscard]] std::optional<QList<QRectF>> scaledTargets(const QList<QRectF>& rectangles,
                                                         double factor);
} // namespace pinned_window_selection_geometry

#endif // SNOW_SHOT_PRESENTATION_PINNEDWINDOWSELECTIONGEOMETRY_H
