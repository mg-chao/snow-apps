#include "pinnedwindowselectiongeometry.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace {
enum class Axis { Horizontal, Vertical };
enum class Edge { Start, Center, End };

struct Alignment {
    Axis axis;
    Edge edge;
    bool distribute = false;
};

struct Span {
    double start;
    double end;

    [[nodiscard]] double midpoint() const {
        return std::midpoint(start, end);
    }
    [[nodiscard]] double extent() const {
        return end - start;
    }
};

std::optional<Alignment> operation(SnowCanvasSelectionAlignment alignment) {
    switch (alignment) {
    case SnowCanvasSelectionAlignment::AlignLeft:
        return Alignment{Axis::Horizontal, Edge::Start};
    case SnowCanvasSelectionAlignment::AlignCenterHorizontally:
        return Alignment{Axis::Horizontal, Edge::Center};
    case SnowCanvasSelectionAlignment::AlignRight:
        return Alignment{Axis::Horizontal, Edge::End};
    case SnowCanvasSelectionAlignment::AlignTop:
        return Alignment{Axis::Vertical, Edge::Start};
    case SnowCanvasSelectionAlignment::AlignCenterVertically:
        return Alignment{Axis::Vertical, Edge::Center};
    case SnowCanvasSelectionAlignment::AlignBottom:
        return Alignment{Axis::Vertical, Edge::End};
    case SnowCanvasSelectionAlignment::DistributeHorizontally:
        return Alignment{Axis::Horizontal, Edge::Start, true};
    case SnowCanvasSelectionAlignment::DistributeVertically:
        return Alignment{Axis::Vertical, Edge::Start, true};
    }
    return std::nullopt;
}

bool validRectangle(const QRectF& rectangle) {
    return rectangle.isValid() && std::isfinite(rectangle.x()) && std::isfinite(rectangle.y()) &&
           std::isfinite(rectangle.width()) && std::isfinite(rectangle.height()) &&
           std::isfinite(rectangle.x() + rectangle.width()) &&
           std::isfinite(rectangle.y() + rectangle.height());
}

Span span(const QRectF& rectangle, Axis axis) {
    return axis == Axis::Horizontal ? Span{rectangle.x(), rectangle.x() + rectangle.width()}
                                    : Span{rectangle.y(), rectangle.y() + rectangle.height()};
}

void translate(QRectF& rectangle, Axis axis, double delta) {
    if (axis == Axis::Horizontal)
        rectangle.translate(delta, 0.0);
    else
        rectangle.translate(0.0, delta);
}

bool validTargets(const QList<QRectF>& rectangles) {
    return std::all_of(rectangles.begin(), rectangles.end(), validRectangle);
}
} // namespace

std::optional<QList<QRectF>>
pinned_window_selection_geometry::alignmentTargets(const QList<QRectF>& rectangles,
                                                   SnowCanvasSelectionAlignment alignment) {
    const auto requested = operation(alignment);
    if (!requested || rectangles.size() < (requested->distribute ? 3 : 2) ||
        !validTargets(rectangles))
        return std::nullopt;

    const Axis axis = requested->axis;
    Span reference = span(rectangles.first(), axis);
    for (const auto& rectangle : rectangles) {
        const Span current = span(rectangle, axis);
        reference.start = std::min(reference.start, current.start);
        reference.end = std::max(reference.end, current.end);
    }
    if (!std::isfinite(reference.extent()))
        return std::nullopt;

    QList<QRectF> result = rectangles;
    if (!requested->distribute) {
        for (qsizetype index = 0; index < rectangles.size(); ++index) {
            const Span current = span(rectangles[index], axis);
            double delta = 0.0;
            switch (requested->edge) {
            case Edge::Start:
                delta = reference.start - current.start;
                break;
            case Edge::Center:
                delta = reference.midpoint() - current.midpoint();
                break;
            case Edge::End:
                delta = reference.end - current.end;
                break;
            }
            translate(result[index], axis, delta);
        }
    } else {
        std::vector<qsizetype> order(static_cast<std::size_t>(rectangles.size()));
        std::iota(order.begin(), order.end(), qsizetype{0});
        std::stable_sort(order.begin(), order.end(), [&](qsizetype left, qsizetype right) {
            return span(rectangles[left], axis).midpoint() <
                   span(rectangles[right], axis).midpoint();
        });

        double totalExtent = 0.0;
        for (const auto& rectangle : rectangles)
            totalExtent += span(rectangle, axis).extent();
        if (!std::isfinite(totalExtent))
            return std::nullopt;
        const double intervals = static_cast<double>(rectangles.size() - 1);
        const double gap = (reference.extent() - totalExtent) / intervals;
        if (gap < 0.0) {
            // Match Snow Draw Engine's overlap fallback, including stable
            // center ordering when nested rectangles share an outer edge.
            const auto first = std::find_if(order.begin(), order.end(), [&](qsizetype index) {
                return span(rectangles[index], axis).start == reference.start;
            });
            const auto last = std::find_if(order.begin(), order.end(), [&](qsizetype index) {
                return span(rectangles[index], axis).end == reference.end;
            });
            if (first != order.end() && last != order.end()) {
                const double step = (span(rectangles[*last], axis).midpoint() -
                                     span(rectangles[*first], axis).midpoint()) /
                                    intervals;
                double position = span(rectangles[*first], axis).midpoint();
                for (auto iterator = order.begin(); iterator != order.end(); ++iterator) {
                    if (iterator == first || iterator == last)
                        continue;
                    position += step;
                    translate(result[*iterator], axis,
                              position - span(rectangles[*iterator], axis).midpoint());
                }
            }
        } else {
            double position = reference.start;
            for (const qsizetype index : order) {
                const Span current = span(rectangles[index], axis);
                translate(result[index], axis, position - current.start);
                position += gap + current.extent();
            }
        }
    }
    return validTargets(result) ? std::optional(std::move(result)) : std::nullopt;
}

std::optional<double>
pinned_window_selection_geometry::sharedScaleFactor(std::span<const double> currentScalePercents,
                                                    double requestedFactor, double minimumPercent,
                                                    double maximumPercent) {
    if (currentScalePercents.empty() || !std::isfinite(requestedFactor) || requestedFactor <= 0.0 ||
        !std::isfinite(minimumPercent) || !std::isfinite(maximumPercent) || minimumPercent <= 0.0 ||
        maximumPercent < minimumPercent)
        return std::nullopt;

    double minimumFactor = 0.0;
    double maximumFactor = std::numeric_limits<double>::max();
    for (const double percent : currentScalePercents) {
        if (!std::isfinite(percent) || percent <= 0.0)
            return std::nullopt;
        minimumFactor = std::max(minimumFactor, minimumPercent / percent);
        maximumFactor = std::min(maximumFactor, maximumPercent / percent);
    }
    if (!std::isfinite(minimumFactor) || minimumFactor > maximumFactor || maximumFactor <= 0.0)
        return std::nullopt;
    return std::clamp(requestedFactor, minimumFactor, maximumFactor);
}

std::optional<QList<QRectF>>
pinned_window_selection_geometry::scaledTargets(const QList<QRectF>& rectangles, double factor) {
    if (rectangles.isEmpty() || !std::isfinite(factor) || factor <= 0.0 ||
        !validTargets(rectangles))
        return std::nullopt;
    QList<QRectF> result;
    result.reserve(rectangles.size());
    for (const auto& rectangle : rectangles)
        result.append(QRectF(rectangle.topLeft(), rectangle.size() * factor));
    return validTargets(result) ? std::optional(std::move(result)) : std::nullopt;
}
