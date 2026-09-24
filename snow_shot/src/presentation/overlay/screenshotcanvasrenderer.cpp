#include "snow_shot/presentation/screenshotcanvasrenderer.h"

#include "snow_shot/presentation/screenshotguidelinerendering.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrtextlayer.h"
#include "snow_shot/presentation/screenshotocrtextlayout.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "theme/theme_manager.h"
#include "widgets/checkerboard.h"

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QColorSpace>
#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRawFont>
#include <QRect>
#include <QRegion>
#include <QSizeF>
#include <QStyleOptionGraphicsItem>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>
#include <QTransform>
#include <QVector>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

class ScreenshotOcrGraphicsTextItem final : public QGraphicsItem {
  public:
    ScreenshotOcrTextLayout layout;
    bool selectionOnly = false;

    void setSelection(const ScreenshotOcrTextRange& selection) {
        layout.setSelection(selection);
        update();
    }
    [[nodiscard]] int cursorPositionAt(const QPointF& position) const {
        return layout.cursorPositionAt(position);
    }
    [[nodiscard]] QRectF boundingRect() const override {
        return layout.boundingRect();
    }
    bool configure(const ScreenshotOcrLine& line, const QTransform& canvasToView,
                   const QColor& color, const ScreenshotOcrTextRange& selection,
                   QTransform* transform) {
        prepareGeometryChange();
        const bool valid = configureScreenshotOcrTextLayout(
            layout, line, canvasToView, QApplication::font(), color, selection, transform);
        update();
        return valid;
    }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget* widget) override {
        const QPalette palette = widget != nullptr ? widget->palette() : QApplication::palette();
        QColor highlight = palette.highlight().color();
        if (selectionOnly) {
            highlight.setAlphaF(0.4F);
        }
        layout.paint(painter, highlight, palette.highlightedText().color(),
                     selectionOnly ? ScreenshotOcrTextLayout::PaintMode::SelectionOnly
                                   : ScreenshotOcrTextLayout::PaintMode::TextAndSelection);
    }
};

namespace {
constexpr double kSelectionBorderWidth = 2.0;
constexpr double kSelectionHandleRadius = 4.0;
constexpr double kSelectionHandleStrokeWidth = 1.5;
constexpr double kShowEndHandlesMinSize = 32.0;
constexpr double kShowMidHandlesMinSize = 64.0;
constexpr int kSelectionBorderUpdatePadding = 3;
constexpr int kSelectionHandleUpdatePadding = 6;
constexpr int kGuideLineUpdatePadding = 1;

#if defined(SNOW_SHOT_BENCH_INTERNALS)
thread_local QRegion g_selectionDamageRegion;
thread_local std::size_t g_selectionDamagePathFallbacks = 0;
thread_local QRegion g_guideLineDamageRegion;
thread_local std::size_t g_guideLineUpdateRequests = 0;

std::size_t regionPixelCount(const QRegion& region) {
    std::size_t pixels = 0;
    for (const QRect& rectangle : region) {
        pixels += static_cast<std::size_t>(rectangle.width()) *
                  static_cast<std::size_t>(rectangle.height());
    }
    return pixels;
}
#endif

QPainterPath selectionShapePath(const QRectF& selection, int cornerRadius,
                                const QTransform& canvasToViewTransform, qreal inset = 0.0) {
    QRectF viewRect = canvasToViewTransform.mapRect(selection.normalized());
    viewRect.adjust(inset, inset, -inset, -inset);

    QPainterPath path;
    if (!viewRect.isValid() || viewRect.isEmpty()) {
        return path;
    }

    const qreal canvasRadius = std::min<qreal>(
        std::max(0, cornerRadius), std::min(selection.width(), selection.height()) / 2.0);
    if (canvasRadius <= 0.0) {
        path.addRect(viewRect);
        return path;
    }

    const qreal horizontalScale =
        std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12());
    const qreal verticalScale =
        std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22());
    const qreal horizontalRadius =
        std::clamp(canvasRadius * horizontalScale - inset, 0.0, viewRect.width() / 2.0);
    const qreal verticalRadius =
        std::clamp(canvasRadius * verticalScale - inset, 0.0, viewRect.height() / 2.0);
    path.addRoundedRect(viewRect, horizontalRadius, verticalRadius, Qt::AbsoluteSize);
    return path;
}

bool rectFCovers(const QRectF& outer, const QRect& inner) {
    if (!inner.isValid() || inner.isEmpty() || !outer.isValid() || outer.isEmpty()) {
        return false;
    }
    const QRectF innerBounds(inner);
    return outer.left() <= innerBounds.left() && outer.top() <= innerBounds.top() &&
           outer.right() >= innerBounds.right() && outer.bottom() >= innerBounds.bottom();
}

void renderSelectionShadow(QPainter& painter, const SnowCanvasRenderContext& context,
                           const QRectF& selection, int cornerRadius, int shadowWidth,
                           const QColor& shadowColor, const QWidget* widget) {
    if (shadowWidth <= 0) {
        return;
    }

    const qreal horizontalScale =
        std::hypot(context.canvasToViewTransform.m11(), context.canvasToViewTransform.m12());
    const qreal verticalScale =
        std::hypot(context.canvasToViewTransform.m21(), context.canvasToViewTransform.m22());
    const qreal scale = std::max<qreal>(1.0e-6, std::min(horizontalScale, verticalScale));
    const QRectF selectionView = context.canvasToViewTransform.mapRect(selection.normalized());
    if (!selectionView.isValid() || selectionView.isEmpty()) {
        return;
    }
    ScreenshotSelectionShadowRenderer::renderPreview(
        painter, selectionView, std::max(0, cornerRadius) * scale, shadowWidth * scale, shadowColor,
        context.devicePixelRatio, widget);
}

bool hasSelectionBounds(const ScreenshotSelectionVisualState& state) {
    return state.present && state.bounds.isValid() && !state.bounds.isEmpty();
}

QRectF mappedSelectionBounds(const ScreenshotSelectionVisualState& state,
                             const QTransform& canvasToViewTransform) {
    return hasSelectionBounds(state) ? canvasToViewTransform.mapRect(state.bounds.normalized())
                                     : QRectF();
}

qreal viewScale(const QTransform& canvasToViewTransform) {
    return std::max<qreal>(
        1.0e-6, std::min(std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12()),
                         std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22())));
}

QRegion selectionStateDecorationRegion(const ScreenshotSelectionVisualState& state,
                                       const QRect& viewportRect,
                                       const QTransform& canvasToViewTransform) {
    const QRectF selectionBounds = mappedSelectionBounds(state, canvasToViewTransform);
    if (!selectionBounds.isValid() || selectionBounds.isEmpty()) {
        return {};
    }
    const qreal scale = viewScale(canvasToViewTransform);
    const qreal shadow = state.toolbarHovered ? std::max(0, state.shadowWidth) * scale : 0.0;
    const qreal padding = shadow + kSelectionBorderUpdatePadding;
    QRegion decoration(selectionBounds.adjusted(-padding, -padding, padding, padding)
                           .toAlignedRect()
                           .intersected(viewportRect));
    const QRect stableInterior =
        selectionBounds
            .adjusted(kSelectionBorderUpdatePadding, kSelectionBorderUpdatePadding,
                      -kSelectionBorderUpdatePadding, -kSelectionBorderUpdatePadding)
            .toAlignedRect();
    if (!stableInterior.isEmpty()) {
        decoration -= QRegion(stableInterior.intersected(viewportRect));
    }

    if (!state.toolbarHovered && state.handlesVisible) {
        const double minSide = std::min(selectionBounds.width(), selectionBounds.height());
        std::array<QPointF, 8> handles{};
        std::size_t handleCount = 0;
        if (state.cornerRadius <= 0 && minSide > kShowEndHandlesMinSize) {
            handles[handleCount++] = selectionBounds.topLeft();
            handles[handleCount++] = selectionBounds.topRight();
            handles[handleCount++] = selectionBounds.bottomRight();
            handles[handleCount++] = selectionBounds.bottomLeft();
        }
        if (minSide > kShowMidHandlesMinSize) {
            handles[handleCount++] = QPointF(selectionBounds.center().x(), selectionBounds.top());
            handles[handleCount++] = QPointF(selectionBounds.right(), selectionBounds.center().y());
            handles[handleCount++] =
                QPointF(selectionBounds.center().x(), selectionBounds.bottom());
            handles[handleCount++] = QPointF(selectionBounds.left(), selectionBounds.center().y());
        }
        for (std::size_t index = 0; index < handleCount; ++index) {
            decoration +=
                QRegion(QRectF(handles[index].x() - kSelectionHandleUpdatePadding,
                               handles[index].y() - kSelectionHandleUpdatePadding,
                               kSelectionHandleUpdatePadding * 2, kSelectionHandleUpdatePadding * 2)
                            .toAlignedRect()
                            .intersected(viewportRect));
        }
    }
    return decoration;
}

QRegion selectionStateMaskRegion(const ScreenshotSelectionVisualState& state,
                                 const QRect& viewportRect,
                                 const QTransform& canvasToViewTransform) {
    if (!hasSelectionBounds(state)) {
        return {};
    }
    const QRectF mapped = canvasToViewTransform.mapRect(state.bounds.normalized());
    return QRegion(mapped.adjusted(-1.0, -1.0, 1.0, 1.0).toAlignedRect().intersected(viewportRect));
}

QRegion selectionStateRoundedCornerRegion(const ScreenshotSelectionVisualState& state,
                                          const QRect& viewportRect,
                                          const QTransform& canvasToViewTransform) {
    if (!hasSelectionBounds(state) || state.cornerRadius <= 0) {
        return {};
    }

    const QRectF mapped = mappedSelectionBounds(state, canvasToViewTransform);
    if (!mapped.isValid() || mapped.isEmpty()) {
        return {};
    }

    const qreal canvasRadius = std::min<qreal>(
        state.cornerRadius, std::min(state.bounds.width(), state.bounds.height()) / 2.0);
    const qreal horizontalScale =
        std::hypot(canvasToViewTransform.m11(), canvasToViewTransform.m12());
    const qreal verticalScale =
        std::hypot(canvasToViewTransform.m21(), canvasToViewTransform.m22());
    const qreal radiusX = std::min(mapped.width() / 2.0, canvasRadius * horizontalScale);
    const qreal radiusY = std::min(mapped.height() / 2.0, canvasRadius * verticalScale);
    constexpr qreal padding = 2.0;
    QRegion corners;
    corners += QRectF(mapped.left() - padding, mapped.top() - padding, radiusX + padding * 2.0,
                      radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.right() - radiusX - padding, mapped.top() - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.left() - padding, mapped.bottom() - radiusY - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    corners += QRectF(mapped.right() - radiusX - padding, mapped.bottom() - radiusY - padding,
                      radiusX + padding * 2.0, radiusY + padding * 2.0)
                   .toAlignedRect();
    return corners.intersected(viewportRect);
}

QColor normalizedGuideLineColor(const QColor& color) {
    return color.isValid() && color.alpha() > 0 ? color : QColor(0, 0, 0, 0);
}

QPoint guideLinePixelPosition(const QPointF& position) {
    return QPoint(qFloor(position.x()), qFloor(position.y()));
}

QRegion guideLineVerticalRegion(const QRect& viewportRect, int x) {
    if (viewportRect.isEmpty()) {
        return {};
    }

    return QRegion(QRect(x - kGuideLineUpdatePadding, viewportRect.top(),
                         kGuideLineUpdatePadding * 2 + 1, viewportRect.height()))
        .intersected(viewportRect);
}

QRegion guideLineHorizontalRegion(const QRect& viewportRect, int y) {
    if (viewportRect.isEmpty()) {
        return {};
    }

    return QRegion(QRect(viewportRect.left(), y - kGuideLineUpdatePadding, viewportRect.width(),
                         kGuideLineUpdatePadding * 2 + 1))
        .intersected(viewportRect);
}

QRegion guideLineCrosshairRegion(const QRect& viewportRect, const QPoint& center) {
    return guideLineVerticalRegion(viewportRect, center.x()) +
           guideLineHorizontalRegion(viewportRect, center.y());
}

QPoint monitorCenterGuideLinePosition(const QRect& viewportRect) {
    const QPointF center = QRectF(viewportRect).center();
    return guideLinePixelPosition(center);
}

QRegion planGuideLineDamage(const QRect& viewportRect, const QPoint& previousCursorPosition,
                            const QColor& previousCursorColor,
                            const QColor& previousMonitorCenterColor,
                            const QPoint& nextCursorPosition, const QColor& nextCursorColor,
                            const QColor& nextMonitorCenterColor) {
    QRegion dirtyRegion;
    const bool cursorColorChanged = previousCursorColor != nextCursorColor;
    if (cursorColorChanged || previousCursorPosition.x() != nextCursorPosition.x()) {
        if (previousCursorColor.alpha() > 0) {
            dirtyRegion += guideLineVerticalRegion(viewportRect, previousCursorPosition.x());
        }
        if (nextCursorColor.alpha() > 0) {
            dirtyRegion += guideLineVerticalRegion(viewportRect, nextCursorPosition.x());
        }
    }
    if (cursorColorChanged || previousCursorPosition.y() != nextCursorPosition.y()) {
        if (previousCursorColor.alpha() > 0) {
            dirtyRegion += guideLineHorizontalRegion(viewportRect, previousCursorPosition.y());
        }
        if (nextCursorColor.alpha() > 0) {
            dirtyRegion += guideLineHorizontalRegion(viewportRect, nextCursorPosition.y());
        }
    }
    if (previousMonitorCenterColor != nextMonitorCenterColor &&
        (previousMonitorCenterColor.alpha() > 0 || nextMonitorCenterColor.alpha() > 0)) {
        dirtyRegion +=
            guideLineCrosshairRegion(viewportRect, monitorCenterGuideLinePosition(viewportRect));
    }
    return dirtyRegion;
}

QRegion mappedRegionPixels(const ScreenshotRegionGeometry& geometry,
                           const QTransform& canvasToViewTransform, const QRect& viewportRect) {
    QRegion pixels;
    for (const QRect& rectangle : geometry.rectangles()) {
        pixels += canvasToViewTransform.mapRect(QRectF(rectangle))
                      .toAlignedRect()
                      .intersected(viewportRect);
    }
    return pixels;
}

QRegion shapedPreviewCheckerboardPixels(const ScreenshotSelectionVisualState& state,
                                        const QRegion& selectedPixels,
                                        const QTransform& canvasToViewTransform,
                                        const QRect& viewportRect) {
    if (!state.present || !state.toolbarHovered || !state.region)
        return {};
    const QRect bounds = state.region->boundingRect();
    if (bounds.isEmpty())
        return {};
    const int shadow = std::max(0, state.shadowWidth);
    const QRect preview =
        canvasToViewTransform.mapRect(QRectF(bounds.adjusted(-shadow, -shadow, shadow, shadow)))
            .toAlignedRect()
            .intersected(viewportRect);
    return QRegion(preview).subtracted(selectedPixels);
}

QRegion regionEdgeDamage(const QRegion& region, int padding, const QRect& viewportRect,
                         int cornerRadius = 0) {
    QRegion edges;
    for (const QRect& rectangle : region) {
        QRegion band(rectangle.adjusted(-padding, -padding, padding, padding));
        const QRect interior = rectangle.adjusted(padding, padding, -padding, -padding);
        if (!interior.isEmpty()) {
            band -= QRegion(interior);
        }
        if (cornerRadius > padding) {
            const int extent = cornerRadius + padding;
            const int side = 2 * extent;
            for (const QPoint& corner : {rectangle.topLeft(), rectangle.topRight(),
                                         rectangle.bottomLeft(), rectangle.bottomRight()}) {
                // Concave corners can round into holes as well as into the
                // selected region, so cover both sides of each vertex.
                const int x = corner.x() + (corner.x() == rectangle.right() ? 1 : 0) - extent;
                const int y = corner.y() + (corner.y() == rectangle.bottom() ? 1 : 0) - extent;
                band += QRect(x, y, side, side);
            }
        }
        edges += band;
    }
    return edges.intersected(viewportRect);
}

QRegion marqueeEdgeDamage(const QRectF& marquee, const QTransform& canvasToViewTransform,
                          const QRect& viewportRect) {
    if (marquee.isEmpty()) {
        return {};
    }
    return regionEdgeDamage(QRegion(canvasToViewTransform.mapRect(marquee).toAlignedRect()), 3,
                            viewportRect);
}

QRegion boundedSelectionDamage(const QRegion& damage, const QRect& viewport) {
    if (damage.rectCount() <= 128)
        return damage.intersected(viewport);
    // Hundreds of small contour bands make source blits and raster clips more
    // expensive than moderate overdraw. Coalesce them on a fixed repaint grid.
    constexpr int tileSide = 64;
    QRegion tiles;
    for (const auto& rect : damage) {
        const int left = qFloor(qreal(rect.left()) / tileSide) * tileSide;
        const int top = qFloor(qreal(rect.top()) / tileSide) * tileSide;
        const int right = qCeil(qreal(rect.right() + 1) / tileSide) * tileSide;
        const int bottom = qCeil(qreal(rect.bottom() + 1) / tileSide) * tileSide;
        tiles += QRect(left, top, right - left, bottom - top);
    }
    return tiles.intersected(viewport);
}

} // namespace

QRegion planScreenshotGuideLineDamage(const QRect& viewportRect,
                                      const QPoint& previousCursorPosition,
                                      const QColor& previousCursorColor,
                                      const QColor& previousMonitorCenterColor,
                                      const QPoint& nextCursorPosition,
                                      const QColor& nextCursorColor,
                                      const QColor& nextMonitorCenterColor) {
    return planGuideLineDamage(viewportRect, previousCursorPosition, previousCursorColor,
                               previousMonitorCenterColor, nextCursorPosition, nextCursorColor,
                               nextMonitorCenterColor);
}

QRegion planScreenshotSelectionDamage(const ScreenshotSelectionVisualState& previous,
                                      const ScreenshotSelectionVisualState& next,
                                      const QRect& viewportRect,
                                      const QTransform& canvasToViewTransform, bool maskVisible) {
    if (previous == next || viewportRect.isEmpty()) {
        return {};
    }
    if (previous.region || next.region) {
        if (!canvasToViewTransform.isInvertible()) {
            return QRegion(viewportRect);
        }
        const bool rasterRegions = previous.present && next.present && previous.region &&
                                   next.region && !previous.region->custom() &&
                                   !next.region->custom() && previous.region->rectCount() <= 512 &&
                                   next.region->rectCount() <= 512 &&
                                   canvasToViewTransform.type() <= QTransform::TxScale;
        if (rasterRegions) {
            const QRegion previousPixels =
                mappedRegionPixels(*previous.region, canvasToViewTransform, viewportRect);
            const QRegion nextPixels =
                mappedRegionPixels(*next.region, canvasToViewTransform, viewportRect);
            const qreal scale = std::max(std::abs(canvasToViewTransform.m11()),
                                         std::abs(canvasToViewTransform.m22()));
            // Rounded contours differ from their integer region only in a band
            // around its edges. Include that band without repainting the gaps.
            const int padding =
                qCeil(scale * std::max(previous.toolbarHovered ? previous.shadowWidth : 0,
                                       next.toolbarHovered ? next.shadowWidth : 0)) +
                3;
            const int radius = qCeil(scale * std::max(previous.cornerRadius, next.cornerRadius));
            QRegion dirtyRegion = previousPixels.xored(nextPixels);
            dirtyRegion += regionEdgeDamage(previousPixels, padding, viewportRect, radius);
            dirtyRegion += regionEdgeDamage(nextPixels, padding, viewportRect, radius);
            if (previous.toolbarHovered || next.toolbarHovered) {
                dirtyRegion += shapedPreviewCheckerboardPixels(previous, previousPixels,
                                                               canvasToViewTransform, viewportRect)
                                   .xored(shapedPreviewCheckerboardPixels(
                                       next, nextPixels, canvasToViewTransform, viewportRect));
            }
            if (previous.confirmedRegion != next.confirmedRegion ||
                previous.borderVisible != next.borderVisible ||
                previous.cornerRadius != next.cornerRadius) {
                dirtyRegion +=
                    regionEdgeDamage(mappedRegionPixels(previous.confirmedRegion,
                                                        canvasToViewTransform, viewportRect),
                                     3, viewportRect, radius);
                dirtyRegion += regionEdgeDamage(
                    mappedRegionPixels(next.confirmedRegion, canvasToViewTransform, viewportRect),
                    3, viewportRect, radius);
            }
            dirtyRegion += marqueeEdgeDamage(previous.marquee, canvasToViewTransform, viewportRect);
            dirtyRegion += marqueeEdgeDamage(next.marquee, canvasToViewTransform, viewportRect);
            if (previous.draftPath != next.draftPath ||
                previous.draftVertices != next.draftVertices ||
                previous.subtracting != next.subtracting ||
                previous.dangerColor != next.dangerColor) {
                const QRectF draftBounds =
                    previous.draftPath.boundingRect().united(next.draftPath.boundingRect());
                if (!draftBounds.isEmpty()) {
                    dirtyRegion += QRegion(canvasToViewTransform.mapRect(draftBounds)
                                               .toAlignedRect()
                                               .adjusted(-5, -5, 5, 5));
                }
                for (const QPointF& vertex : previous.draftVertices + next.draftVertices) {
                    const QPointF view = canvasToViewTransform.map(vertex);
                    dirtyRegion += QRectF(view - QPointF(5, 5), QSizeF(10, 10)).toAlignedRect();
                }
            }
            return boundedSelectionDamage(dirtyRegion, viewportRect);
        }
        const QRectF affected = previous.bounds.united(next.bounds)
                                    .united(previous.marquee)
                                    .united(next.marquee)
                                    .united(previous.draftPath.boundingRect())
                                    .united(next.draftPath.boundingRect())
                                    .united(QRectF(previous.confirmedRegion.boundingRect()))
                                    .united(QRectF(next.confirmedRegion.boundingRect()));
        const int padding = std::max(previous.shadowWidth, next.shadowWidth) + 3;
        return QRegion(canvasToViewTransform.mapRect(affected).toAlignedRect().adjusted(
                           -padding, -padding, padding, padding))
            .intersected(viewportRect);
    }
    if (!canvasToViewTransform.isInvertible()) {
        return QRegion(viewportRect);
    }
    QRegion dirtyRegion =
        selectionStateDecorationRegion(previous, viewportRect, canvasToViewTransform);
    dirtyRegion += selectionStateDecorationRegion(next, viewportRect, canvasToViewTransform);
    // Shadow changes also repaint the transparent rounded corner squares.
    const bool hoveredShadowChanged =
        (previous.toolbarHovered || next.toolbarHovered) &&
        (previous.shadowWidth != next.shadowWidth || previous.shadowColor != next.shadowColor);
    if (previous.bounds != next.bounds || previous.cornerRadius != next.cornerRadius ||
        previous.toolbarHovered != next.toolbarHovered || hoveredShadowChanged) {
        dirtyRegion +=
            selectionStateRoundedCornerRegion(previous, viewportRect, canvasToViewTransform);
        dirtyRegion += selectionStateRoundedCornerRegion(next, viewportRect, canvasToViewTransform);
    }
    if (maskVisible) {
        dirtyRegion +=
            selectionStateMaskRegion(previous, viewportRect, canvasToViewTransform)
                .xored(selectionStateMaskRegion(next, viewportRect, canvasToViewTransform));
    }
    if (previous.draftPath != next.draftPath || previous.draftVertices != next.draftVertices ||
        previous.subtracting != next.subtracting || previous.dangerColor != next.dangerColor) {
        const QRectF draftBounds =
            previous.draftPath.boundingRect().united(next.draftPath.boundingRect());
        if (!draftBounds.isEmpty()) {
            dirtyRegion += QRegion(
                canvasToViewTransform.mapRect(draftBounds).toAlignedRect().adjusted(-5, -5, 5, 5));
        }
    }
    return dirtyRegion.intersected(viewportRect);
}

namespace {
QRectF sourcePixelsForImageLayer(const ScreenshotImageLayer& layer) {
    if (!layer.isValid()) {
        return {};
    }
    const qreal scaleX = layer.image.width() / layer.imageCanvasRect.width();
    const qreal scaleY = layer.image.height() / layer.imageCanvasRect.height();
    return QRectF((layer.destinationCanvasRect.left() - layer.imageCanvasRect.left()) * scaleX,
                  (layer.destinationCanvasRect.top() - layer.imageCanvasRect.top()) * scaleY,
                  layer.destinationCanvasRect.width() * scaleX,
                  layer.destinationCanvasRect.height() * scaleY);
}

constexpr qreal kMaximumRasterSourceCoordinate = 32768.0;
constexpr qreal kMaximumRasterSourceScale = 16384.0;
constexpr qreal kMaximumSourceChunkSpan = 2048.0;
constexpr qreal kMinimumSourceSamplingPadding = 1.0;

bool finiteRect(const QRectF& rect) {
    return std::isfinite(rect.left()) && std::isfinite(rect.top()) && std::isfinite(rect.width()) &&
           std::isfinite(rect.height());
}

QImage imageWindow(const QImage& image, const QRect& bounds) {
    if (image.isNull() || bounds.isEmpty() || !image.rect().contains(bounds)) {
        return {};
    }

    // A read-only QImage view keeps the source pixels shared while rebasing the coordinates.
    // Screenshot images are normally 32-bit; copy is retained for indexed and packed formats
    // whose palette or bit offset cannot be represented by the public QImage view constructor.
    const int depth = image.depth();
    if (depth > 0 && depth % 8 == 0 && image.colorTable().isEmpty()) {
        const int bytesPerPixel = depth / 8;
        const uchar* data = image.constScanLine(bounds.top()) +
                            static_cast<qsizetype>(bounds.left()) * bytesPerPixel;
        const qsizetype byteOffset = data - image.constBits();
        const qsizetype availableBytes = image.sizeInBytes() - byteOffset;
        const qsizetype requiredBytes = image.bytesPerLine() * bounds.height();
        const bool scanLinesAligned = reinterpret_cast<quintptr>(data) % alignof(quint32) == 0;
        if (scanLinesAligned && requiredBytes <= availableBytes) {
            QImage view(data, bounds.width(), bounds.height(), image.bytesPerLine(),
                        image.format());
            if (!view.isNull()) {
                view.setColorSpace(image.colorSpace());
                view.setDevicePixelRatio(1.0);
                return view;
            }
        }
    }

    QImage copy = image.copy(bounds);
    if (!copy.isNull()) {
        copy.setDevicePixelRatio(1.0);
    }
    return copy;
}

bool sourceMappingFitsRaster(const QRectF& source, qreal sourcePerTargetX, qreal sourcePerTargetY) {
    return finiteRect(source) && source.left() >= 0.0 && source.top() >= 0.0 &&
           source.right() < kMaximumRasterSourceCoordinate &&
           source.bottom() < kMaximumRasterSourceCoordinate && sourcePerTargetX > 0.0 &&
           sourcePerTargetY > 0.0 && sourcePerTargetX <= kMaximumRasterSourceScale &&
           sourcePerTargetY <= kMaximumRasterSourceScale;
}

void paintScaledSourceWindow(QPainter& painter, const QRectF& targetWindow, const QImage& image,
                             const QRect& sourceBounds) {
    if (targetWindow.isEmpty() || sourceBounds.isEmpty()) {
        return;
    }
    const QImage sourceWindow = imageWindow(image, sourceBounds);
    if (sourceWindow.isNull()) {
        return;
    }

    // Scale in QImage, whose transform path does not use the raster paint engine's 16.16
    // source-coordinate representation. The resulting image is deliberately bounded so the
    // final painter call itself remains inside that representation as well.
    const QRectF deviceWindow = painter.deviceTransform().mapRect(targetWindow);
    constexpr int kMaximumScaledDimension = static_cast<int>(kMaximumRasterSourceCoordinate) - 4;
    const int scaledWidth =
        std::clamp(qCeil(std::abs(deviceWindow.width())), 1, kMaximumScaledDimension);
    const int scaledHeight =
        std::clamp(qCeil(std::abs(deviceWindow.height())), 1, kMaximumScaledDimension);
    const Qt::TransformationMode mode = painter.testRenderHint(QPainter::SmoothPixmapTransform)
                                            ? Qt::SmoothTransformation
                                            : Qt::FastTransformation;
    QImage scaled =
        sourceWindow.scaled(QSize(scaledWidth, scaledHeight), Qt::IgnoreAspectRatio, mode);
    if (scaled.isNull()) {
        return;
    }
    scaled.setDevicePixelRatio(1.0);
    painter.drawImage(targetWindow, scaled, QRectF(scaled.rect()));
}

void paintExposedImageSlice(QPainter& painter, const QRectF& targetRect, const QImage& image,
                            const QRectF& sourceRect, const QRegion& exposedRegion) {
    if (image.isNull() || !finiteRect(targetRect) || !targetRect.isValid() ||
        targetRect.isEmpty() || !finiteRect(sourceRect) || !sourceRect.isValid() ||
        sourceRect.isEmpty() || exposedRegion.isEmpty()) {
        return;
    }

    const qreal sourcePerTargetX = sourceRect.width() / targetRect.width();
    const qreal sourcePerTargetY = sourceRect.height() / targetRect.height();
    if (!std::isfinite(sourcePerTargetX) || !std::isfinite(sourcePerTargetY) ||
        sourcePerTargetX <= 0.0 || sourcePerTargetY <= 0.0) {
        return;
    }

    const QRectF drawableSource = sourceRect.intersected(QRectF(image.rect()));
    if (!drawableSource.isValid() || drawableSource.isEmpty()) {
        return;
    }
    const QRectF drawableTarget(
        targetRect.left() + (drawableSource.left() - sourceRect.left()) / sourcePerTargetX,
        targetRect.top() + (drawableSource.top() - sourceRect.top()) / sourcePerTargetY,
        drawableSource.width() / sourcePerTargetX, drawableSource.height() / sourcePerTargetY);
    if (!drawableTarget.isValid() || drawableTarget.isEmpty()) {
        return;
    }

    // Keep the ordinary path as a single draw. The explicit clip makes this
    // correct even when a caller supplies a damage region without clipping its painter first.
    if (sourceMappingFitsRaster(sourceRect, sourcePerTargetX, sourcePerTargetY)) {
        painter.save();
        painter.setClipRegion(exposedRegion, Qt::IntersectClip);
        painter.drawImage(targetRect, image, sourceRect);
        painter.restore();
        return;
    }

    const auto sourceForTarget = [&](const QRectF& target) {
        return QRectF(sourceRect.left() + (target.left() - targetRect.left()) * sourcePerTargetX,
                      sourceRect.top() + (target.top() - targetRect.top()) * sourcePerTargetY,
                      target.width() * sourcePerTargetX, target.height() * sourcePerTargetY);
    };
    const auto targetForSource = [&](const QRectF& source) {
        return QRectF(targetRect.left() + (source.left() - sourceRect.left()) / sourcePerTargetX,
                      targetRect.top() + (source.top() - sourceRect.top()) / sourcePerTargetY,
                      source.width() / sourcePerTargetX, source.height() / sourcePerTargetY);
    };
    const bool smoothSampling = painter.testRenderHint(QPainter::SmoothPixmapTransform);
    const qreal samplingPaddingX =
        smoothSampling ? std::max(kMinimumSourceSamplingPadding, sourcePerTargetX) : 0.0;
    const qreal samplingPaddingY =
        smoothSampling ? std::max(kMinimumSourceSamplingPadding, sourcePerTargetY) : 0.0;

    for (const QRect& exposedRectangle : exposedRegion) {
        const QRectF exposedTarget = drawableTarget.intersected(QRectF(exposedRectangle));
        if (!exposedTarget.isValid() || exposedTarget.isEmpty()) {
            continue;
        }

        const QRectF exposedSource = sourceForTarget(exposedTarget).intersected(drawableSource);
        if (!exposedSource.isValid() || exposedSource.isEmpty()) {
            continue;
        }

        // Keep each source chunk bounded in both dimensions. A ratio above the signed 16.16
        // increment limit is handled by QImage::scaled below, so that axis does not need to be
        // split into sub-pixel target cells.
        const int chunkCountX =
            sourcePerTargetX > kMaximumRasterSourceScale
                ? 1
                : qMax(1, qCeil(exposedSource.width() / kMaximumSourceChunkSpan));
        const int chunkCountY =
            sourcePerTargetY > kMaximumRasterSourceScale
                ? 1
                : qMax(1, qCeil(exposedSource.height() / kMaximumSourceChunkSpan));

        painter.save();
        painter.setClipRect(exposedRectangle, Qt::IntersectClip);
        for (int chunkY = 0; chunkY < chunkCountY; ++chunkY) {
            const qreal sourceTop =
                exposedSource.top() + exposedSource.height() * chunkY / chunkCountY;
            const qreal sourceBottom =
                exposedSource.top() + exposedSource.height() * (chunkY + 1) / chunkCountY;
            for (int chunkX = 0; chunkX < chunkCountX; ++chunkX) {
                const qreal sourceLeft =
                    exposedSource.left() + exposedSource.width() * chunkX / chunkCountX;
                const qreal sourceRight =
                    exposedSource.left() + exposedSource.width() * (chunkX + 1) / chunkCountX;
                const QRectF chunkSource(sourceLeft, sourceTop, sourceRight - sourceLeft,
                                         sourceBottom - sourceTop);
                const QRectF chunkTarget = targetForSource(chunkSource).intersected(exposedTarget);
                if (!chunkTarget.isValid() || chunkTarget.isEmpty()) {
                    continue;
                }

                const QRectF sampleSource = chunkSource
                                                .adjusted(-samplingPaddingX, -samplingPaddingY,
                                                          samplingPaddingX, samplingPaddingY)
                                                .intersected(drawableSource);
                const QRect sourceBounds = sampleSource.toAlignedRect().intersected(image.rect());
                if (!sampleSource.isValid() || sampleSource.isEmpty() || sourceBounds.isEmpty()) {
                    continue;
                }
                const QRectF sampleTarget = targetForSource(sampleSource);

                painter.save();
                painter.setClipRect(chunkTarget, Qt::IntersectClip);
                if (sourcePerTargetX > kMaximumRasterSourceScale ||
                    sourcePerTargetY > kMaximumRasterSourceScale) {
                    // This also handles a single target pixel representing tens of thousands of
                    // source pixels; splitting that target pixel cannot make the 16.16 increment
                    // finite, while QImage's scaler can reduce the source safely.
                    const QRectF targetWindow = targetForSource(QRectF(sourceBounds));
                    paintScaledSourceWindow(painter, targetWindow, image, sourceBounds);
                } else if (sourceMappingFitsRaster(sampleSource, sourcePerTargetX,
                                                   sourcePerTargetY)) {
                    painter.drawImage(sampleTarget, image, sampleSource);
                } else {
                    const QImage boundedSource = imageWindow(image, sourceBounds);
                    if (!boundedSource.isNull()) {
                        painter.drawImage(sampleTarget, boundedSource,
                                          sampleSource.translated(-sourceBounds.topLeft()));
                    }
                }
                painter.restore();
            }
        }
        painter.restore();
    }
}

void paintImageLayer(QPainter& painter, const ScreenshotImageLayer& layer,
                     const QTransform& canvasToTarget, const QRegion* exposedRegion = nullptr) {
    const QRectF sourcePixels = sourcePixelsForImageLayer(layer);
    if (sourcePixels.isEmpty()) {
        return;
    }
    const QRectF targetRect = canvasToTarget.mapRect(layer.destinationCanvasRect);
    if (exposedRegion != nullptr) {
        paintExposedImageSlice(painter, targetRect, layer.image, sourcePixels, *exposedRegion);
    } else {
        painter.drawImage(targetRect, layer.image, sourcePixels);
    }
}

// A pinned result drawn 1:1 in device pixels must stay pixel-exact. Minification resamples with
// linear filtering because the raster engine's default nearest-neighbour sampling drops pixels and
// aliases badly; magnification keeps nearest-neighbour sampling so zoomed-in pixels stay crisp
// instead of blurring.
bool pinnedResultUsesLinearFiltering(const SnowCanvasRenderContext& context,
                                     const QRectF& targetRect, const QSize& sourceSize) {
    if (context.devicePixelRatio <= 0.0) {
        return false;
    }
    const QSize deviceSize(qRound(targetRect.width() * context.devicePixelRatio),
                           qRound(targetRect.height() * context.devicePixelRatio));
    return deviceSize.width() < sourceSize.width() || deviceSize.height() < sourceSize.height();
}
} // namespace

ScreenshotOcrTextLayer::ScreenshotOcrTextLayer(QWidget* parent)
    : QGraphicsView(parent), m_scene(new QGraphicsScene(this)) {
    setObjectName(QStringLiteral("snowShotOcrTextLayer"));
    setScene(m_scene);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    setBackgroundBrush(Qt::NoBrush);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAutoFillBackground(false);
    viewport()->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    viewport()->setAttribute(Qt::WA_NoSystemBackground, true);
    viewport()->setAttribute(Qt::WA_TranslucentBackground, true);
    viewport()->setAutoFillBackground(false);
    setOptimizationFlag(QGraphicsView::DontSavePainterState, true);
    setStyleSheet(QStringLiteral("QGraphicsView#snowShotOcrTextLayer {"
                                 " background: transparent; border: none;"
                                 "}"));
    hide();
}

void ScreenshotOcrTextLayer::setPresentation(
    std::shared_ptr<ScreenshotOcrPresentation> presentation, RenderingMode mode) {
    m_renderingMode = mode;
    m_textItems.clear();
    m_scene->clear();
    m_presentation = std::move(presentation);
    const adqt::theme::ThemeMapToken theme =
        adqt::theme::ThemeManager::instance().resolveTheme(this);
    m_textColor = theme.colorText;
    if (m_presentation != nullptr) {
        m_presentation->prepareForRendering();
    }
    m_viewportRect = {};
    m_selectionAnchor = {};
    m_selectionFocus = {};
    m_selectionRevision = std::numeric_limits<quint64>::max();
    m_synchronized = false;
    rebuildTextItems();
    hide();
}

void ScreenshotOcrTextLayer::clearPresentation() {
    m_textItems.clear();
    m_scene->clear();
    m_presentation.reset();
    m_textColor = {};
    m_viewportRect = {};
    m_selectionAnchor = {};
    m_selectionFocus = {};
    m_selectionRevision = std::numeric_limits<quint64>::max();
    m_synchronized = false;
    hide();
}

void ScreenshotOcrTextLayer::synchronize(const QTransform& canvasToViewTransform,
                                         const QRect& viewportRect) {
    if (m_presentation == nullptr || m_textItems.empty() || viewportRect.isEmpty()) {
        hide();
        return;
    }

    const bool geometryChanged = !m_synchronized ||
                                 m_canvasToViewTransform != canvasToViewTransform ||
                                 m_viewportRect != viewportRect;
    if (geometryChanged && geometry() != viewportRect) {
        setGeometry(viewportRect);
    }
    if (geometryChanged) {
        setSceneRect(QRectF(QPointF(0.0, 0.0), viewportRect.size()));
        m_canvasToViewTransform = canvasToViewTransform;
        m_viewportRect = viewportRect;
        m_synchronized = true;
        for (TextItem& item : m_textItems) {
            synchronizeTextItem(item, canvasToViewTransform);
#if defined(SNOW_SHOT_BENCH_INTERNALS)
            ++m_geometrySynchronizationCount;
#endif
        }
    }
    updateSelection();
    show();
    raise();
    if (geometryChanged) {
        viewport()->update();
    }
}

void ScreenshotOcrTextLayer::updateLineText(int lineIndex) {
    if (!m_synchronized || lineIndex < 0 || lineIndex >= static_cast<int>(m_textItems.size())) {
        return;
    }
    synchronizeTextItem(m_textItems.at(static_cast<std::size_t>(lineIndex)),
                        m_canvasToViewTransform);
    // A text replacement can change selection ranges even when both endpoints stay put.
    m_selectionRevision = std::numeric_limits<quint64>::max();
    updateSelection();
}

void ScreenshotOcrTextLayer::updateSelection() {
    if (m_presentation == nullptr || m_selectionRevision == m_presentation->selectionRevision()) {
        return;
    }
    const ScreenshotOcrTextPosition nextAnchor = m_presentation->selectionAnchor();
    const ScreenshotOcrTextPosition nextFocus = m_presentation->selectionFocus();
    int firstLine = 0;
    int lastLine = static_cast<int>(m_textItems.size()) - 1;
    const bool anchorUnchanged = m_selectionAnchor.lineIndex == nextAnchor.lineIndex &&
                                 m_selectionAnchor.characterIndex == nextAnchor.characterIndex;
    if (m_selectionRevision != std::numeric_limits<quint64>::max() && anchorUnchanged &&
        m_selectionFocus.valid() && nextFocus.valid()) {
        firstLine = std::min(m_selectionFocus.lineIndex, nextFocus.lineIndex);
        lastLine = std::max(m_selectionFocus.lineIndex, nextFocus.lineIndex);
    }
    for (int lineIndex = firstLine; lineIndex <= lastLine; ++lineIndex) {
        TextItem& item = m_textItems.at(static_cast<std::size_t>(lineIndex));
        if (item.graphicsText != nullptr) {
            item.graphicsText->setSelection(m_presentation->textSelectionForLine(item.lineIndex));
        }
    }
    m_selectionAnchor = nextAnchor;
    m_selectionFocus = nextFocus;
    m_selectionRevision = m_presentation->selectionRevision();
}

ScreenshotOcrTextPosition ScreenshotOcrTextLayer::textPositionAt(const QPointF& canvasPosition,
                                                                 bool useClosestLine) const {
    if (m_presentation == nullptr) {
        return {};
    }

    ScreenshotOcrTextPosition position =
        m_presentation->textPositionAt(canvasPosition, useClosestLine);
    if (!position.valid() || !m_synchronized ||
        position.lineIndex >= static_cast<int>(m_textItems.size())) {
        return position;
    }

    const TextItem& item = m_textItems.at(static_cast<std::size_t>(position.lineIndex));
    if (item.graphicsText == nullptr || !item.graphicsText->isVisible()) {
        return position;
    }

    const QPointF viewPosition = m_canvasToViewTransform.map(canvasPosition);
    const QPointF itemPosition = item.graphicsText->mapFromScene(viewPosition);
    position.characterIndex = item.graphicsText->cursorPositionAt(itemPosition);
    return position;
}

void ScreenshotOcrTextLayer::rebuildTextItems() {
    if (m_presentation == nullptr) {
        return;
    }

    m_textItems.reserve(static_cast<std::size_t>(m_presentation->lines.size()));

    const auto appendTextItem = [this](int lineIndex) {
        auto* graphicsText = new ScreenshotOcrGraphicsTextItem;
        graphicsText->selectionOnly = m_renderingMode == RenderingMode::SelectionOnly;
        m_scene->addItem(graphicsText);
        m_textItems.push_back(TextItem{
            lineIndex,
            graphicsText,
        });
    };

    for (int lineIndex = 0; lineIndex < m_presentation->lines.size(); ++lineIndex) {
        appendTextItem(lineIndex);
    }
}

void ScreenshotOcrTextLayer::synchronizeTextItem(TextItem& item,
                                                 const QTransform& canvasToViewTransform) {
    if (m_presentation == nullptr || item.graphicsText == nullptr || item.lineIndex < 0 ||
        item.lineIndex >= m_presentation->lines.size()) {
        return;
    }

    const ScreenshotOcrLine& line = m_presentation->lines.at(item.lineIndex);
    const QString& text = line.text;
    const QPolygonF viewQuad = canvasToViewTransform.map(line.quad);
    if (text.isEmpty() || viewQuad.size() != 4 ||
        !viewQuad.boundingRect().intersects(sceneRect())) {
        item.graphicsText->hide();
        return;
    }

    QTransform transform;
    if (!item.graphicsText->configure(line, canvasToViewTransform, m_textColor,
                                      m_presentation->textSelectionForLine(item.lineIndex),
                                      &transform)) {
        item.graphicsText->hide();
        return;
    }
    item.graphicsText->setPos(0.0, 0.0);
    item.graphicsText->setTransform(transform);
    item.graphicsText->show();
}

bool ScreenshotCanvasRenderer::PathRasterCache::draw(QPainter& painter, const QPainterPath& path,
                                                     const QColor& color, bool exterior,
                                                     const QRect& viewport) {
    // QWidget::render can redirect painting to a differently scaled device.
    // Its actual device transform, including pixel phase, owns raster alignment.
    const auto device = painter.deviceTransform();
    if (device.type() > QTransform::TxScale || device.m11() <= 0 ||
        !qFuzzyCompare(device.m11(), device.m22()))
        return false;
    const qreal scale = device.m11();
    const QRect pixels = device.mapRect(path.boundingRect().adjusted(-2, -2, 2, 2)).toAlignedRect();
    constexpr qsizetype byteLimit = 64 * 1024 * 1024;
    const qint64 imageBytes = qint64(pixels.width()) * pixels.height() * 4;
    if (pixels.isEmpty() || imageBytes > byteLimit)
        return false;
    const QPointF origin = device.inverted().map(QPointF(pixels.topLeft()));
    const QPainterPath localPath = path.translated(-origin);
    auto matches = [&](const Entry& entry) {
        return !entry.image.isNull() && entry.path == localPath && entry.scale == scale &&
               entry.color == color && entry.image.size() == pixels.size();
    };
    if (!matches(entries[0])) {
        if (matches(entries[1])) {
            std::swap(entries[0], entries[1]);
        } else {
            if (imageBytes + entries[0].image.sizeInBytes() <= byteLimit)
                entries[1] = std::move(entries[0]);
            else
                entries[1] = {};
            entries[0] = {};
            auto& entry = entries[0];
            entry.path = localPath;
            entry.color = color;
            entry.scale = scale;
            entry.image = QImage(pixels.size(), QImage::Format_ARGB32_Premultiplied);
            if (entry.image.isNull())
                return false;
            entry.image.setDevicePixelRatio(scale);
            entry.image.fill(Qt::transparent);
            QPainter raster(&entry.image);
            raster.setRenderHint(QPainter::Antialiasing);
            if (exterior) {
                QPainterPath complement;
                complement.setFillRule(Qt::OddEvenFill);
                complement.addRect(QRectF(QPointF(), QSizeF(pixels.size()) / scale));
                complement.addPath(localPath);
                raster.fillPath(complement, color);
            } else {
                raster.setPen(QPen(color, kSelectionBorderWidth));
                raster.setBrush(Qt::NoBrush);
                raster.drawPath(localPath);
            }
        }
    }
    if (exterior) {
        // The cached image covers only the shape bounds. Fill the rest of the
        // viewport on exact physical-pixel boundaries, without overlapping it.
        const QRectF bounds(origin, QSizeF(pixels.size()) / scale);
        const QRectF view(viewport);
        painter.fillRect(QRectF(view.left(), view.top(), view.width(),
                                std::max(qreal(0), bounds.top() - view.top())),
                         color);
        painter.fillRect(QRectF(view.left(), std::max(view.top(), bounds.bottom()), view.width(),
                                std::max(qreal(0), view.bottom() - bounds.bottom())),
                         color);
        const qreal top = std::max(view.top(), bounds.top());
        const qreal height = std::max(qreal(0), std::min(view.bottom(), bounds.bottom()) - top);
        painter.fillRect(
            QRectF(view.left(), top, std::max(qreal(0), bounds.left() - view.left()), height),
            color);
        painter.fillRect(QRectF(std::max(view.left(), bounds.right()), top,
                                std::max(qreal(0), view.right() - bounds.right()), height),
                         color);
    }
    painter.drawImage(origin, entries[0].image);
    return true;
}

ScreenshotCanvasRenderer::ScreenshotCanvasRenderer(SnowCanvasWidget& canvas) : m_canvas(canvas) {
    m_themeConnection = QObject::connect(&adqt::theme::ThemeManager::instance(),
                                         &adqt::theme::ThemeManager::themeChanged, &canvas,
                                         [&canvas]() { canvas.update(); });
}

ScreenshotCanvasRenderer::~ScreenshotCanvasRenderer() {
    QObject::disconnect(m_themeConnection);
    delete m_ocrTextLayer.data();
}

void ScreenshotCanvasRenderer::setRenderMode(RenderMode mode) {
    if (m_renderMode == mode) {
        return;
    }
    m_renderMode = mode;
    invalidateCachedContent();
    if (m_renderMode == RenderMode::ScrollingCapture && m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->hide();
    }
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setImage(QImage image, const QRectF& canvasRect) {
    setImageSource(ScreenshotImageSource::fromImage(std::move(image), canvasRect));
}

void ScreenshotCanvasRenderer::setImageSource(ScreenshotImageSource source) {
    if (source.isMaterialized()) {
        source.materializedImage.setDevicePixelRatio(1.0);
    }
    m_imageSource = std::move(source);
    QList<SnowCanvasBaseImageSource> baseSources;
    if (m_imageSource.isMaterialized()) {
        baseSources.push_back(
            {m_imageSource.materializedImage, m_imageSource.materializedCanvasRect, {}});
    } else {
        for (const auto& layer : m_imageSource.layers)
            baseSources.push_back(
                {layer.image, layer.imageCanvasRect, layer.destinationCanvasRect});
    }
    m_canvas.setBaseImageSources(baseSources);
    clearOcrFilteredImage();
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setImageViewportPhysicalSize(const QSize& size) {
    const QSize normalized = size.isValid() && !size.isEmpty() ? size : QSize();
    if (m_imageViewportPhysicalSize == normalized) {
        return;
    }
    m_imageViewportPhysicalSize = normalized;
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setPinnedResultSurface(const QRectF& contentCanvasRect,
                                                      const QRectF& surfaceCanvasRect,
                                                      const ScreenshotResultStyle& style) {
    m_pinnedContentCanvasRect = contentCanvasRect.normalized();
    m_pinnedSurfaceCanvasRect = surfaceCanvasRect.normalized();
    m_pinnedResultStyle = ScreenshotResultCompositor::normalizedStyle(style);
    setRenderMode(RenderMode::PinnedResult);
}

void ScreenshotCanvasRenderer::setBakedSelectionPath(const QPainterPath& path) {
    if (m_bakedSelectionPath != path) {
        m_bakedSelectionPath = path;
        m_canvas.update();
    }
}

void ScreenshotCanvasRenderer::setPinnedBackgroundColor(const QColor& color) {
    const QColor normalized = color.isValid() ? color : QColor();
    if (m_pinnedBackgroundColor == normalized) {
        return;
    }
    m_pinnedBackgroundColor = normalized;
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setPinnedCheckerboardEnabled(bool enabled) {
    if (m_pinnedCheckerboardEnabled == enabled)
        return;
    m_pinnedCheckerboardEnabled = enabled;
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setMaskVisible(bool visible) {
    if (m_maskVisible == visible) {
        return;
    }
    m_maskVisible = visible;
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setSelectionBorderColor(const QColor& color) {
    const QColor next = color.isValid() ? color : QColor(0x40, 0x96, 0xff);
    if (m_selectionBorderColor == next) {
        return;
    }
    m_selectionBorderColor = next;
    if (m_selectionState.present) {
        m_canvas.update();
    }
}

void ScreenshotCanvasRenderer::setMaskColor(const QColor& color) {
    const QColor next = color.isValid() ? color : QColor(0, 0, 0, 128);
    if (m_maskColor == next) {
        return;
    }
    m_maskColor = next;
    if (m_maskVisible) {
        m_canvas.update();
    }
}

void ScreenshotCanvasRenderer::setGuideLines(const QPointF& cursorPosition,
                                             const QColor& cursorColor,
                                             const QColor& monitorCenterColor) {
    const QColor nextCursorColor = normalizedGuideLineColor(cursorColor);
    const QColor nextMonitorColor = normalizedGuideLineColor(monitorCenterColor);
    const QPoint nextCursorPosition =
        nextCursorColor.alpha() > 0 ? guideLinePixelPosition(cursorPosition) : QPoint();
    const bool nextVisible = nextCursorColor.alpha() > 0 || nextMonitorColor.alpha() > 0;
    if (m_guideLineCursorPosition == nextCursorPosition &&
        m_cursorGuideLineColor == nextCursorColor &&
        m_monitorCenterGuideLineColor == nextMonitorColor && m_guideLinesVisible == nextVisible) {
        return;
    }
    const QRegion dirtyRegion = planScreenshotGuideLineDamage(
        m_canvas.rect(), m_guideLineCursorPosition, m_cursorGuideLineColor,
        m_monitorCenterGuideLineColor, nextCursorPosition, nextCursorColor, nextMonitorColor);
    m_guideLineCursorPosition = nextCursorPosition;
    m_cursorGuideLineColor = nextCursorColor;
    m_monitorCenterGuideLineColor = nextMonitorColor;
    m_guideLinesVisible = nextVisible;
    if (!dirtyRegion.isEmpty()) {
#if defined(SNOW_SHOT_BENCH_INTERNALS)
        g_guideLineDamageRegion += dirtyRegion;
        ++g_guideLineUpdateRequests;
#endif
        m_canvas.update(dirtyRegion);
    }
}

void ScreenshotCanvasRenderer::clearGuideLines() {
    setGuideLines({}, Qt::transparent, Qt::transparent);
}

void ScreenshotCanvasRenderer::setSelection(const QRectF& selection, bool handlesVisible,
                                            int cornerRadius, int shadowWidth,
                                            const QColor& shadowColor) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.region.reset();
    next.confirmedRegion = {};
    next.marquee = {};
    next.bounds = selection.normalized();
    next.present = next.bounds.isValid() && !next.bounds.isEmpty();
    next.handlesVisible = handlesVisible;
    next.cornerRadius = next.present ? std::max(0, cornerRadius) : 0;
    next.shadowWidth = next.present ? std::max(0, shadowWidth) : 0;
    next.shadowColor = shadowColor.isValid() ? shadowColor : QColor(0x33, 0x33, 0x33);
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::setSelectionRegion(const ScreenshotRegionGeometry& region,
                                                  const ScreenshotRegionGeometry& confirmed,
                                                  const QRectF& marquee, bool subtracting,
                                                  const QColor& danger) {
    auto state = m_selectionState;
    state.region = region;
    state.confirmedRegion = confirmed;
    state.marquee = marquee;
    state.subtracting = subtracting;
    state.dangerColor = danger;
    state.bounds =
        QRectF(region.boundingRect()).united(QRectF(confirmed.boundingRect())).united(marquee);
    state.present = !state.bounds.isEmpty();
    state.handlesVisible = false;
    applySelectionState(state);
}

void ScreenshotCanvasRenderer::applySelectionState(const ScreenshotSelectionVisualState& state) {
    ScreenshotSelectionVisualState next = state;
    next.bounds = next.bounds.normalized();
    next.present = next.present && next.bounds.isValid() && !next.bounds.isEmpty();
    next.cornerRadius = next.present ? std::max(0, next.cornerRadius) : 0;
    next.shadowWidth = next.present ? std::max(0, next.shadowWidth) : 0;
    if (!next.shadowColor.isValid()) {
        next.shadowColor = QColor(0x33, 0x33, 0x33);
    }
    if (next == m_selectionState) {
        return;
    }
    const ScreenshotSelectionVisualState previous = m_selectionState;
    m_selectionState = next;
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    const QTransform canvasToViewTransform = m_canvas.canvasToViewTransform();
    if (!canvasToViewTransform.isInvertible()) {
        ++g_selectionDamagePathFallbacks;
    }
#else
    const QTransform canvasToViewTransform = m_canvas.canvasToViewTransform();
#endif
    const QRegion dirtyRegion = planScreenshotSelectionDamage(
        previous, m_selectionState, m_canvas.rect(), canvasToViewTransform, m_maskVisible);
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    g_selectionDamageRegion += dirtyRegion;
#endif
    if (!dirtyRegion.isEmpty()) {
        m_canvas.update(dirtyRegion);
    }
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
ScreenshotSelectionRenderDiagnostics selectionRenderDiagnosticsForCurrentThread() {
    return ScreenshotSelectionRenderDiagnostics{
        regionPixelCount(g_selectionDamageRegion),
        g_selectionDamagePathFallbacks,
    };
}

void resetSelectionRenderDiagnosticsForCurrentThread() {
    g_selectionDamageRegion = {};
    g_selectionDamagePathFallbacks = 0;
}

ScreenshotGuideLineRenderDiagnostics guideLineRenderDiagnosticsForCurrentThread() {
    return ScreenshotGuideLineRenderDiagnostics{
        regionPixelCount(g_guideLineDamageRegion),
        g_guideLineUpdateRequests,
    };
}

void resetGuideLineRenderDiagnosticsForCurrentThread() {
    g_guideLineDamageRegion = {};
    g_guideLineUpdateRequests = 0;
}
#endif

void ScreenshotCanvasRenderer::setSelectionToolbarHovered(bool hovered) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.toolbarHovered = hovered;
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::setSelectionBorderVisible(bool visible) {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.borderVisible = visible;
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::clearSelection() {
    ScreenshotSelectionVisualState next = m_selectionState;
    next.region.reset();
    next.confirmedRegion = {};
    next.marquee = {};
    next.bounds = QRectF();
    next.present = false;
    next.handlesVisible = true;
    next.cornerRadius = 0;
    next.shadowWidth = 0;
    next.shadowColor = QColor(0x33, 0x33, 0x33);
    applySelectionState(next);
}

void ScreenshotCanvasRenderer::setOcrVisible(bool visible) {
    if (m_ocrVisible == visible) {
        return;
    }
    m_ocrVisible = visible;
    if (m_ocrTextLayer != nullptr) {
        if (visible && m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText) {
            m_ocrTextLayer->setPresentation(m_ocrPresentation);
        } else {
            m_ocrTextLayer->clearPresentation();
        }
    }
    invalidateCachedContent();
    m_canvas.update();
}

void ScreenshotCanvasRenderer::setOcrPresentation(
    std::shared_ptr<ScreenshotOcrPresentation> presentation, OcrPresentationMode mode) {
    const bool presentationChanged = m_ocrPresentation != presentation;
    const bool hadPresentation = m_ocrPresentation != nullptr;
    const bool modeChanged = m_ocrPresentationMode != mode;
    const QRectF clearedFilteredCanvasRect = m_ocrFilteredCanvasRect;
    const bool hadFilteredImage = !m_ocrFilteredImage.isNull();
    m_ocrPresentation = std::move(presentation);
    if (presentationChanged) {
        clearOcrFilteredImage();
    }
    m_ocrPresentationMode = mode;
    m_ocrBackgroundColor = {};
    if (m_ocrPresentation != nullptr) {
        const adqt::theme::ThemeMapToken theme =
            adqt::theme::ThemeManager::instance().resolveTheme(&m_canvas);
        m_ocrBackgroundColor =
            theme.colorBgContainer.isValid() ? theme.colorBgContainer : QColor(Qt::white);
    }
    invalidateCachedContent();
    if (m_ocrVisible && m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText) {
        ensureOcrTextLayer()->setPresentation(m_ocrPresentation);
    } else if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    // The presentation alone repaints pixels only when the text layer relayouts
    // (BackgroundAndText) or when the selection decorations toggle with its
    // presence; otherwise the damage comes from the filtered image, which is set
    // separately with its own targeted update.
    if (m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText || modeChanged ||
        hadPresentation != (m_ocrPresentation != nullptr)) {
        m_canvas.update();
    } else if (hadFilteredImage) {
        const QRegion dirtyRegion = ocrFilterImageDamageRegion(clearedFilteredCanvasRect);
        if (!dirtyRegion.isEmpty()) {
            m_canvas.update(dirtyRegion);
        }
    }
}

QRegion ScreenshotCanvasRenderer::ocrFilterImageDamageRegion(const QRectF& canvasRect) const {
    if (!canvasRect.isValid() || canvasRect.isEmpty()) {
        return {};
    }
    if (!m_canvas.canvasToViewTransform().isInvertible()) {
        // The display cache is not synchronized; repaint everything rather than
        // risk stale content.
        return QRegion(m_canvas.rect());
    }
    // One pixel of padding covers view-space rounding at the rect edges.
    const QRect viewRect = m_canvas.viewRectForCanvasRect(canvasRect, 1);
    if (viewRect.isEmpty()) {
        return {};
    }
    return QRegion(viewRect.intersected(m_canvas.rect()));
}

void ScreenshotCanvasRenderer::setOcrFilteredImage(QImage image, const QRectF& canvasRect) {
    const QRectF previousCanvasRect = m_ocrFilteredCanvasRect;
    QRegion dirtyRegion;
    if (!image.isNull() && canvasRect.isValid() && !canvasRect.isEmpty()) {
        image.setDevicePixelRatio(1.0);
        m_ocrFilteredImage = std::move(image);
        m_ocrFilteredCanvasRect = canvasRect.normalized();
        dirtyRegion = ocrFilterImageDamageRegion(previousCanvasRect) +
                      ocrFilterImageDamageRegion(m_ocrFilteredCanvasRect);
    } else {
        dirtyRegion = ocrFilterImageDamageRegion(previousCanvasRect);
        m_ocrFilteredImage = {};
        m_ocrFilteredCanvasRect = {};
    }
    invalidateCachedContent();
    if (!dirtyRegion.isEmpty()) {
        m_canvas.update(dirtyRegion);
    }
}

void ScreenshotCanvasRenderer::clearOcrFilteredImage() {
    if (m_ocrFilteredImage.isNull() && !m_ocrFilteredCanvasRect.isValid()) {
        return;
    }
    m_ocrFilteredImage = {};
    m_ocrFilteredCanvasRect = {};
    invalidateCachedContent();
}

void ScreenshotCanvasRenderer::updateOcrSelection() {
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->updateSelection();
    }
}

ScreenshotOcrTextPosition ScreenshotCanvasRenderer::ocrTextPositionAt(const QPointF& canvasPosition,
                                                                      bool useClosestLine) const {
    if (m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText &&
        m_ocrTextLayer != nullptr) {
        return m_ocrTextLayer->textPositionAt(canvasPosition, useClosestLine);
    }
    return m_ocrVisible && m_ocrPresentation != nullptr
               ? m_ocrPresentation->textPositionAt(canvasPosition, useClosestLine)
               : ScreenshotOcrTextPosition{};
}

void ScreenshotCanvasRenderer::clearOcrPresentation() {
    if (m_ocrPresentation == nullptr && m_ocrFilteredImage.isNull()) {
        return;
    }
    m_ocrPresentation.reset();
    m_ocrFilteredImage = {};
    m_ocrFilteredCanvasRect = {};
    m_ocrBackgroundColor = {};
    m_ocrPresentationMode = OcrPresentationMode::BackgroundAndText;
    invalidateCachedContent();
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    m_canvas.update();
}

void ScreenshotCanvasRenderer::reset() {
    // Pinned/export snapshots can share geometry with the ending capture.
    // Release only derived data, leaving those snapshots fully usable.
    if (m_selectionState.region)
        m_selectionState.region->clearDerivedCache();
    m_selectionState.confirmedRegion.clearDerivedCache();
    if (m_regionHoverCacheRegion)
        m_regionHoverCacheRegion->clearDerivedCache();
    if (m_pinnedResultStyle.region)
        m_pinnedResultStyle.region->clearDerivedCache();
    setOcrVisible(true);
    const bool hadCachedContent =
        m_imageSource.isValid() || !m_imageViewportPhysicalSize.isEmpty() ||
        m_renderMode != RenderMode::Standard || m_ocrPresentation != nullptr;
    const bool hadState = m_imageSource.isValid() || !m_imageViewportPhysicalSize.isEmpty() ||
                          m_renderMode != RenderMode::Standard || m_maskVisible ||
                          m_selectionState.present || m_selectionState.shadowWidth > 0 ||
                          m_selectionState.toolbarHovered || !m_selectionState.borderVisible ||
                          m_ocrPresentation != nullptr || m_guideLinesVisible;
    m_imageSource = {};
    m_canvas.setBaseImageSources({});
    m_imageViewportPhysicalSize = QSize();
    m_pinnedContentCanvasRect = {};
    m_pinnedSurfaceCanvasRect = {};
    m_pinnedResultStyle = {};
    m_pinnedBackgroundColor = {};
    m_bakedSelectionPath = {};
    m_regionHoverCacheRegion.reset();
    m_regionHoverCache = {};
    m_outlineCache = {};
    m_maskCache = {};
    m_pinnedCheckerboardEnabled = false;
    m_selectionState = ScreenshotSelectionVisualState{};
    m_renderMode = RenderMode::Standard;
    m_maskVisible = false;
    m_guideLineCursorPosition = {};
    m_cursorGuideLineColor = QColor(0, 0, 0, 0);
    m_monitorCenterGuideLineColor = QColor(0, 0, 0, 0);
    m_guideLinesVisible = false;
    m_ocrPresentation.reset();
    m_ocrBackgroundColor = {};
    m_ocrPresentationMode = OcrPresentationMode::BackgroundAndText;
    if (m_ocrTextLayer != nullptr) {
        m_ocrTextLayer->clearPresentation();
    }
    if (hadCachedContent) {
        invalidateCachedContent();
    }
    if (hadState) {
        m_canvas.update();
    }
}

std::uint64_t ScreenshotCanvasRenderer::contentRevision() const {
    return m_contentRevision;
}

ScreenshotCanvasRenderer::RenderMode ScreenshotCanvasRenderer::renderMode() const {
    return m_renderMode;
}

bool ScreenshotCanvasRenderer::maskVisible() const {
    return m_maskVisible;
}

QColor ScreenshotCanvasRenderer::maskColor() const {
    return m_maskColor;
}

bool ScreenshotCanvasRenderer::guideLinesVisible() const {
    return m_guideLinesVisible;
}

bool ScreenshotCanvasRenderer::hasSelection() const {
    return m_selectionState.present;
}

bool ScreenshotCanvasRenderer::selectionHandlesVisible() const {
    return m_selectionState.handlesVisible;
}

int ScreenshotCanvasRenderer::selectionCornerRadius() const {
    return m_selectionState.cornerRadius;
}

int ScreenshotCanvasRenderer::selectionShadowWidth() const {
    return m_selectionState.shadowWidth;
}

bool ScreenshotCanvasRenderer::selectionToolbarHovered() const {
    return m_selectionState.toolbarHovered;
}

bool ScreenshotCanvasRenderer::selectionBorderVisible() const {
    return m_selectionState.borderVisible;
}

QRectF ScreenshotCanvasRenderer::selection() const {
    return m_selectionState.bounds;
}

bool ScreenshotCanvasRenderer::coversWidgetRect(const QRect& widgetRect) const {
    if (!widgetRect.isValid() || widgetRect.isEmpty() || !m_canvas.rect().contains(widgetRect)) {
        return false;
    }

    if (m_renderMode == RenderMode::ScrollingCapture || m_renderMode == RenderMode::PinnedResult) {
        return true;
    }

    if (!m_imageSource.isMaterialized()) {
        return false;
    }

    const QRect canvasRect = m_canvas.rect();
    const qreal devicePixelRatio = m_canvas.devicePixelRatioF();
    if (m_imageViewportPhysicalSize.isValid() && !m_imageViewportPhysicalSize.isEmpty() &&
        devicePixelRatio > 0.0) {
        const QRectF targetRect(QPointF(canvasRect.topLeft()),
                                QSizeF(m_imageViewportPhysicalSize.width() / devicePixelRatio,
                                       m_imageViewportPhysicalSize.height() / devicePixelRatio));
        return rectFCovers(targetRect, widgetRect);
    }

    const QTransform canvasToView = m_canvas.canvasToViewTransform();
    if (!canvasToView.isInvertible()) {
        // Overlay paintEvent runs before the child canvas publishes its view
        // transform. A full-canvas blit of a materialized screenshot still
        // replaces every parent pixel of that first frame.
        return widgetRect == canvasRect;
    }

    const QRectF targetRect = canvasToView.mapRect(m_imageSource.materializedCanvasRect);
    return rectFCovers(targetRect, widgetRect);
}

#if defined(SNOW_SHOT_BENCH_INTERNALS)
quint64 ScreenshotCanvasRenderer::ocrGeometrySynchronizationCountForTesting() const {
    return m_ocrTextLayer != nullptr ? m_ocrTextLayer->geometrySynchronizationCount() : 0;
}

#endif

void ScreenshotCanvasRenderer::invalidateCachedContent() {
    ++m_contentRevision;
}

ScreenshotOcrTextLayer* ScreenshotCanvasRenderer::ensureOcrTextLayer() {
    if (m_ocrTextLayer == nullptr) {
        m_ocrTextLayer = new ScreenshotOcrTextLayer(&m_canvas);
    }
    return m_ocrTextLayer;
}

void ScreenshotCanvasRenderer::renderBeforeCanvas(QPainter& painter,
                                                  const SnowCanvasRenderContext& context) {
    if (m_renderMode == RenderMode::ScrollingCapture || m_renderMode == RenderMode::PinnedResult) {
        painter.save();
        // Replace every covered device pixel, including fractional-DPI edges.
        // Antialiasing a clear leaves residual canvas/parent background alpha.
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(context.viewportRect, QColor(Qt::transparent));
        painter.restore();
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        if (m_renderMode == RenderMode::ScrollingCapture) {
            return;
        }
    }

    if (!m_imageSource.isValid()) {
        return;
    }

    const bool physicalViewport = m_imageViewportPhysicalSize.isValid() &&
                                  !m_imageViewportPhysicalSize.isEmpty() &&
                                  context.devicePixelRatio > 0.0 && m_imageSource.isMaterialized();
    if (physicalViewport) {
        const QRectF targetRect(
            QPointF(context.viewportRect.topLeft()),
            QSizeF(m_imageViewportPhysicalSize.width() / context.devicePixelRatio,
                   m_imageViewportPhysicalSize.height() / context.devicePixelRatio));
        if (!context.exposedRegion.intersects(targetRect.toAlignedRect())) {
            return;
        }
        painter.save();
        painter.setRenderHint(QPainter::SmoothPixmapTransform,
                              m_imageSource.materializedImage.size() !=
                                  m_imageViewportPhysicalSize);
        paintExposedImageSlice(painter, targetRect, m_imageSource.materializedImage,
                               QRectF(m_imageSource.materializedImage.rect()),
                               context.exposedRegion);
        painter.restore();
    } else if (m_imageSource.isMaterialized()) {
        const QRectF targetRect =
            context.canvasToViewTransform.mapRect(m_imageSource.materializedCanvasRect);
        if (context.exposedRegion.intersects(targetRect.toAlignedRect())) {
            painter.save();
            if (m_renderMode == RenderMode::PinnedResult) {
                painter.setRenderHint(
                    QPainter::SmoothPixmapTransform,
                    pinnedResultUsesLinearFiltering(context, targetRect,
                                                    m_imageSource.materializedImage.size()));
            }
            paintExposedImageSlice(painter, targetRect, m_imageSource.materializedImage,
                                   QRectF(m_imageSource.materializedImage.rect()),
                                   context.exposedRegion);
            painter.restore();
        }
    } else {
        for (const ScreenshotImageLayer& layer : m_imageSource.layers) {
            const QRectF targetRect =
                context.canvasToViewTransform.mapRect(layer.destinationCanvasRect);
            if (context.exposedRegion.intersects(targetRect.toAlignedRect())) {
                paintImageLayer(painter, layer, context.canvasToViewTransform,
                                &context.exposedRegion);
            }
        }
    }
    if (m_ocrVisible && m_ocrPresentation != nullptr && !m_ocrFilteredImage.isNull()) {
        const QRectF canvasRect = m_ocrFilteredCanvasRect.isValid()
                                      ? m_ocrFilteredCanvasRect
                                      : QRectF(m_ocrPresentation->selection).normalized();
        const QRectF targetRect = context.canvasToViewTransform.mapRect(canvasRect);
        if (!canvasRect.isEmpty() && targetRect.isValid() && !targetRect.isEmpty() &&
            context.exposedRegion.intersects(targetRect.toAlignedRect())) {
            painter.save();
            if (m_renderMode == RenderMode::PinnedResult) {
                painter.setRenderHint(QPainter::SmoothPixmapTransform,
                                      pinnedResultUsesLinearFiltering(context, targetRect,
                                                                      m_ocrFilteredImage.size()));
            }
            painter.setClipRegion(context.exposedRegion, Qt::IntersectClip);
            painter.setClipRect(targetRect, Qt::IntersectClip);
            painter.drawImage(targetRect, m_ocrFilteredImage);
            painter.restore();
        }
    }
}

void ScreenshotCanvasRenderer::renderAfterCanvas(QPainter& painter,
                                                 const SnowCanvasRenderContext& context) {
    if (m_renderMode == RenderMode::PinnedResult) {
        if (!m_bakedSelectionPath.isEmpty() && m_imageSource.isMaterialized()) {
            QPainterPath outside;
            outside.addRect(QRectF(context.viewportRect));
            outside = outside.subtracted(context.canvasToViewTransform.map(m_bakedSelectionPath));
            painter.save();
            painter.setClipPath(outside, Qt::IntersectClip);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.drawImage(
                context.canvasToViewTransform.mapRect(m_imageSource.materializedCanvasRect),
                m_imageSource.materializedImage);
            painter.restore();
        }
        const QRectF contentView = context.canvasToViewTransform.mapRect(m_pinnedContentCanvasRect);
        ScreenshotResultCompositor::finishLiveSurface(
            painter, QRectF(context.viewportRect), contentView, m_pinnedResultStyle,
            context.devicePixelRatio,
            std::hypot(context.canvasToViewTransform.m11(), context.canvasToViewTransform.m12()));
        // Fill behind the composed image and annotations. The compositor has already
        // cleared the exterior of rounded and shaped results, so an earlier fill
        // would be removed along with those pixels.
        if (m_pinnedCheckerboardEnabled || m_pinnedBackgroundColor.isValid()) {
            painter.save();
            painter.setClipRegion(context.exposedRegion, Qt::IntersectClip);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationOver);
            if (m_pinnedCheckerboardEnabled) {
                const QRectF checkerBounds =
                    m_pinnedBackgroundColor.isValid()
                        ? context.canvasToViewTransform.mapRect(m_pinnedSurfaceCanvasRect)
                        : QRectF(context.viewportRect);
                painter.fillRect(checkerBounds.intersected(QRectF(context.viewportRect)),
                                 adqt::widgets::themedCheckerboardBrush(&m_canvas));
            }
            if (m_pinnedBackgroundColor.isValid())
                painter.fillRect(context.viewportRect, m_pinnedBackgroundColor);
            painter.restore();
        }
        if (m_ocrVisible && m_ocrPresentation != nullptr &&
            m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText &&
            m_ocrTextLayer != nullptr) {
            m_ocrTextLayer->synchronize(context.canvasToViewTransform, context.viewportRect);
        }
        return;
    }
    if (!m_maskVisible && !m_selectionState.present && m_ocrPresentation == nullptr &&
        !m_guideLinesVisible) {
        return;
    }

    painter.save();
    const int visibleCornerRadius =
        m_renderMode == RenderMode::Standard ? m_selectionState.cornerRadius : 0;
    const int selectionBorderCornerRadius = m_ocrPresentation == nullptr ? visibleCornerRadius : 0;
    const bool shaped = m_selectionState.region.has_value();
    const QPainterPath effectivePath =
        shaped ? context.canvasToViewTransform.map(
                     screenshotRegionPath(*m_selectionState.region, visibleCornerRadius))
               : selectionShapePath(m_selectionState.bounds, visibleCornerRadius,
                                    context.canvasToViewTransform);
    const QPainterPath outlinePath =
        shaped ? context.canvasToViewTransform.map(screenshotRegionPath(
                     m_selectionState.confirmedRegion.isEmpty() ? *m_selectionState.region
                                                                : m_selectionState.confirmedRegion,
                     selectionBorderCornerRadius))
               : selectionShapePath(m_selectionState.bounds, selectionBorderCornerRadius,
                                    context.canvasToViewTransform, 0.5);
    if (visibleCornerRadius > 0 || shaped || !m_selectionState.draftPath.isEmpty()) {
        painter.setRenderHint(QPainter::Antialiasing, true);
    }

    const bool cachedMask =
        m_maskVisible && shaped && m_selectionState.present &&
        m_selectionState.draftPath.isEmpty() &&
        // Small masks are cheaper to fill than to fetch from a selection-sized
        // texture. Cache contours whose edge processing can amortize that blit.
        effectivePath.elementCount() > 32 &&
        *m_selectionState.region == m_selectionState.confirmedRegion &&
        m_maskCache.draw(painter, effectivePath, m_maskColor, true, context.viewportRect);
    if (m_maskVisible && !cachedMask) {
        QPainterPath dimPath;
        dimPath.setFillRule(Qt::OddEvenFill);
        dimPath.addRect(QRectF(context.viewportRect));
        if (m_selectionState.present) {
            // The painter clips to this viewport. Odd-even filling complements the
            // selection inside it, including holes and contours spanning monitors,
            // without rebuilding a Boolean path for every paint.
            dimPath.addPath(effectivePath);
        }
        painter.fillPath(dimPath, m_maskColor);
    }
    if (m_renderMode == RenderMode::Standard && m_guideLinesVisible) {
        const QRectF viewport(context.viewportRect);
        paintScreenshotGuideLines(painter, viewport, QPointF(m_guideLineCursorPosition),
                                  m_cursorGuideLineColor, m_monitorCenterGuideLineColor,
                                  &context.exposedRegion);
    }
    const auto draftViewPath = context.canvasToViewTransform.map(m_selectionState.draftPath);
    const bool sharedDraftOutline =
        !m_selectionState.subtracting && !draftViewPath.isEmpty() && draftViewPath == outlinePath;
    if (m_renderMode == RenderMode::Standard && !m_selectionState.draftPath.isEmpty()) {
        const auto color =
            m_selectionState.subtracting ? m_selectionState.dangerColor : m_selectionBorderColor;
        QPen pen(color, kSelectionBorderWidth);
        if (m_selectionState.subtracting)
            pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        // Replacement drafts and their selection outline can be the same curve.
        // Preserve both paint layers, but rasterize that curve only once.
        if (!sharedDraftOutline ||
            !m_outlineCache.draw(painter, draftViewPath, color, false, context.viewportRect))
            painter.drawPath(draftViewPath);
    }
    if (m_renderMode == RenderMode::Standard && m_selectionState.present) {
        const QColor selectionAccent = m_selectionBorderColor;
        const QRectF selectionView = context.canvasToViewTransform.mapRect(m_selectionState.bounds);
        if (m_selectionState.toolbarHovered) {
            if (shaped) {
                const QRect bounds = m_selectionState.region->boundingRect();
                if (!bounds.isEmpty()) {
                    const int padding = m_selectionState.shadowWidth;
                    const QRectF previewBounds = context.canvasToViewTransform.mapRect(
                        QRectF(bounds.adjusted(-padding, -padding, padding, padding)));
                    // Compound results are transparent between regions and in holes,
                    // even when no shadow is configured.
                    QPainterPath previewArea;
                    previewArea.addRect(previewBounds);
                    painter.fillPath(previewArea.subtracted(effectivePath),
                                     adqt::widgets::themedCheckerboardBrush(&m_canvas));
                }
                if (!bounds.isEmpty() && m_selectionState.shadowWidth > 0) {
                    const ScreenshotRegionGeometry localRegion =
                        m_selectionState.region->translated(-bounds.topLeft());
                    const bool cacheHit =
                        !m_regionHoverCache.isNull() && m_regionHoverCacheRegion == localRegion &&
                        m_regionHoverCacheRadius == visibleCornerRadius &&
                        m_regionHoverCacheShadowWidth == m_selectionState.shadowWidth &&
                        m_regionHoverCacheShadowColor == m_selectionState.shadowColor;
                    QImage uncachedShadow;
                    if (!cacheHit) {
                        ScreenshotResultStyle style{visibleCornerRadius,
                                                    m_selectionState.shadowWidth,
                                                    m_selectionState.shadowColor, {}, 1.0};
                        style.region = localRegion;
                        QImage empty(bounds.size(), QImage::Format_ARGB32_Premultiplied);
                        empty.fill(Qt::transparent);
                        uncachedShadow = ScreenshotResultCompositor::compose(empty, style);
                        constexpr qsizetype kRegionHoverCacheByteLimit = 64 * 1024 * 1024;
                        if (uncachedShadow.sizeInBytes() <= kRegionHoverCacheByteLimit) {
                            m_regionHoverCacheRegion = localRegion;
                            m_regionHoverCacheRadius = visibleCornerRadius;
                            m_regionHoverCacheShadowWidth = m_selectionState.shadowWidth;
                            m_regionHoverCacheShadowColor = m_selectionState.shadowColor;
                            m_regionHoverCache = uncachedShadow;
                        } else {
                            m_regionHoverCacheRegion.reset();
                            m_regionHoverCache = {};
                        }
                    }
                    const int padding = m_selectionState.shadowWidth;
                    painter.drawImage(context.canvasToViewTransform.mapRect(QRectF(
                                          bounds.adjusted(-padding, -padding, padding, padding))),
                                      cacheHit ? m_regionHoverCache : uncachedShadow);
                }
            } else {
                renderSelectionShadow(painter, context, m_selectionState.bounds,
                                      visibleCornerRadius, m_selectionState.shadowWidth,
                                      m_selectionState.shadowColor, &m_canvas);
            }
        } else if (m_selectionState.borderVisible) {
            painter.setPen(QPen(selectionAccent, kSelectionBorderWidth));
            painter.setBrush(Qt::NoBrush);
            // Cache in local physical-pixel coordinates. Integer-pixel moves
            // reuse the raster; fractional moves retain their exact sample phase.
            if (!shaped || (m_selectionState.confirmedRegion.isEmpty() && !sharedDraftOutline) ||
                !m_outlineCache.draw(painter, outlinePath, selectionAccent, false,
                                     context.viewportRect)) {
                painter.drawPath(outlinePath);
            }
            if (!m_selectionState.marquee.isEmpty()) {
                QPen pen(m_selectionState.subtracting ? m_selectionState.dangerColor
                                                      : selectionAccent,
                         kSelectionBorderWidth);
                if (m_selectionState.subtracting)
                    pen.setStyle(Qt::DashLine);
                painter.setPen(pen);
                painter.drawRect(context.canvasToViewTransform.mapRect(m_selectionState.marquee));
            }
        }

        const double minSide = std::min(selectionView.width(), selectionView.height());
        std::array<QPointF, 8> handles{};
        std::size_t handleCount = 0;
        if (!m_selectionState.toolbarHovered && m_selectionState.handlesVisible &&
            visibleCornerRadius <= 0 && minSide > kShowEndHandlesMinSize) {
            handles[handleCount++] = selectionView.topLeft();
            handles[handleCount++] = selectionView.topRight();
            handles[handleCount++] = selectionView.bottomRight();
            handles[handleCount++] = selectionView.bottomLeft();
        }
        if (!m_selectionState.toolbarHovered && m_selectionState.handlesVisible &&
            minSide > kShowMidHandlesMinSize) {
            handles[handleCount++] = QPointF(selectionView.center().x(), selectionView.top());
            handles[handleCount++] = QPointF(selectionView.right(), selectionView.center().y());
            handles[handleCount++] = QPointF(selectionView.center().x(), selectionView.bottom());
            handles[handleCount++] = QPointF(selectionView.left(), selectionView.center().y());
        }
        if (handleCount != 0) {
            painter.setBrush(selectionAccent);
            painter.setPen(QPen(Qt::white, kSelectionHandleStrokeWidth));
            for (std::size_t index = 0; index < handleCount; ++index) {
                painter.drawEllipse(handles[index], kSelectionHandleRadius, kSelectionHandleRadius);
            }
        }
    }
    if (m_ocrVisible && m_renderMode == RenderMode::Standard && m_ocrPresentation != nullptr &&
        m_ocrPresentationMode == OcrPresentationMode::BackgroundAndText) {
        if (m_ocrTextLayer != nullptr) {
            m_ocrTextLayer->synchronize(context.canvasToViewTransform, context.viewportRect);
        }
    }
    painter.restore();
}

void ScreenshotCanvasRenderer::setSelectionDraft(const QPainterPath& path,
                                                 const QVector<QPointF>& vertices) {
    auto next = m_selectionState;
    next.draftPath = path;
    next.draftVertices = vertices;
    if (next == m_selectionState)
        return;
    const QRectF before = m_selectionState.draftPath.boundingRect();
    applySelectionState(next);
    QRegion damage(m_canvas.canvasToViewTransform()
                       .mapRect(before.united(path.boundingRect()))
                       .adjusted(-5, -5, 5, 5)
                       .toAlignedRect());
    m_canvas.update(damage);
}
