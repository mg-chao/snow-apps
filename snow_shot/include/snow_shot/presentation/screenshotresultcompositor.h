#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H

#include "snow_shot/image/screenshotlayoutcompaction.h"

#include "snow_shot/image/screenshotselectionbordertype.h"

#include <QColor>
#include <QDataStream>
#include <QImage>
#include <QMargins>
#include <QRect>
#include <QRectF>
#include <QSize>
#include "snow_shot/image/screenshotregiongeometry.h"
#include <QPainterPath>
#include <QIODevice>
#include <optional>

class QPainter;

struct ScreenshotResultStyle {
    // Lengths use the destination coordinate system: canvas units for a live
    // surface, output pixels after scaledStyle(). UI limits belong to the
    // selection model and persisted input, never to rendering normalization.
    int cornerRadius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
    // Immutable source geometry, relative to the content origin. regionScale
    // maps that geometry into the same destination units as the effect lengths.
    std::optional<ScreenshotRegionGeometry> region = std::nullopt;
    qreal regionScale = 1.0;
    int backgroundOpacityPercent = 0;
    ScreenshotSelectionBorderType borderType = ScreenshotSelectionBorderType::Shadow;
    int borderWidth = 0;
    QColor borderColor = QColor(Qt::white);

    [[nodiscard]] int effectWidth() const {
        return borderType == ScreenshotSelectionBorderType::Border ? borderWidth : shadowWidth;
    }
    [[nodiscard]] QColor effectColor() const {
        return borderType == ScreenshotSelectionBorderType::Border ? borderColor : shadowColor;
    }
};

inline QByteArray encodeScreenshotResultStyle(const ScreenshotResultStyle& style) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    // These bytes are persisted without a Qt stream-version header. Keep the format used by
    // existing records when upgrading Qt.
    stream.setVersion(QDataStream::Qt_6_11);
    stream << style.cornerRadius << style.shadowWidth << style.shadowColor;
    const bool hasBorderSettings = style.borderType != ScreenshotSelectionBorderType::Shadow ||
                                   style.borderWidth != 0 || style.borderColor != QColor(Qt::white);
    stream << quint32(0x53535247)
           << quint8(hasBorderSettings                     ? 3
                     : style.backgroundOpacityPercent == 0 ? 1
                                                           : 2)
           << style.regionScale
           << (style.region ? QJsonDocument(style.region->toJson()).toJson(QJsonDocument::Compact)
                            : QByteArray());
    if (hasBorderSettings || style.backgroundOpacityPercent != 0)
        stream << style.backgroundOpacityPercent;
    if (hasBorderSettings)
        stream << quint8(style.borderType) << style.borderWidth << style.borderColor;
    return bytes;
}

// An invalid extension rejects the record rather than discarding persisted selection geometry.
inline std::optional<ScreenshotResultStyle> decodeScreenshotResultStyle(const QByteArray& bytes) {
    ScreenshotResultStyle style;
    if (bytes.isEmpty())
        return style;
    QDataStream stream(bytes);
    stream.setVersion(QDataStream::Qt_6_11);
    stream >> style.cornerRadius >> style.shadowWidth >> style.shadowColor;
    if (stream.status() != QDataStream::Ok || style.cornerRadius < 0 || style.cornerRadius > 256 ||
        style.shadowWidth < 0 || style.shadowWidth > 64 || !style.shadowColor.isValid())
        return std::nullopt;
    if (stream.atEnd())
        return style;
    quint32 marker = 0;
    quint8 version = 0;
    QByteArray geometry;
    stream >> marker >> version >> style.regionScale >> geometry;
    if (version >= 2)
        stream >> style.backgroundOpacityPercent;
    if (version == 3) {
        quint8 type = 0;
        stream >> type >> style.borderWidth >> style.borderColor;
        if (type > 1 || style.borderWidth < 0 || style.borderWidth > 64 ||
            !style.borderColor.isValid())
            return std::nullopt;
        style.borderType = static_cast<ScreenshotSelectionBorderType>(type);
        style.borderColor.setAlpha(255);
    }
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || marker != 0x53535247 ||
        (version != 1 && version != 2 && version != 3) || style.backgroundOpacityPercent < 0 ||
        style.backgroundOpacityPercent > 100 || !std::isfinite(style.regionScale) ||
        style.regionScale <= 0 || style.regionScale > 256)
        return std::nullopt;
    if (!geometry.isEmpty()) {
        style.region =
            ScreenshotRegionGeometry::fromJson(QJsonDocument::fromJson(geometry).object());
        if (!style.region || style.region->isEmpty())
            return std::nullopt;
    }
    return style;
}

struct ScreenshotResultLayout {
    QRect contentRect;
    QRect outputRect;
    QMargins effectInsets;
    qreal devicePixelRatio = 1.0;

    [[nodiscard]] bool isValid() const {
        return contentRect.isValid() && !contentRect.isEmpty() && outputRect.isValid() &&
               !outputRect.isEmpty() && outputRect.contains(contentRect);
    }
};

[[nodiscard]] QPainterPath screenshotRegionPath(const ScreenshotRegionGeometry& region,
                                                qreal radius = 0.0);

// Bounds and radius are in destination units; region geometry is relative to the
// content origin and is mapped by regionScale. Shared by masking and decoration.
[[nodiscard]] QPainterPath
screenshotSelectionPath(const QRectF& contentBounds,
                        const std::optional<ScreenshotRegionGeometry>& region, qreal cornerRadius,
                        qreal regionScale = 1.0);

class ScreenshotResultCompositor final {
  public:
    // Normalize rendering values without applying canvas-unit editor limits.
    [[nodiscard]] static ScreenshotResultStyle normalizedStyle(const ScreenshotResultStyle& style);
    // Convert all lengths and geometry once. Callers supply a positive finite
    // scale from their validated source/output geometry.
    [[nodiscard]] static ScreenshotResultStyle scaledStyle(const ScreenshotResultStyle& style,
                                                           qreal scale);
    // Resolve the active effect once here so every output reserves the same
    // spacing between components, regardless of inactive shadow/border settings.
    [[nodiscard]] static std::shared_ptr<const ScreenshotLayoutCompactionPlan>
    layoutCompactionForContent(const QSize& contentPixelSize, const ScreenshotResultStyle& style,
                               ScreenshotLayoutCompaction options, qreal scale = 1.0);
    [[nodiscard]] static ScreenshotResultLayout layoutForContent(const QSize& contentPixelSize,
                                                                 const ScreenshotResultStyle& style,
                                                                 qreal devicePixelRatio = 1.0);
    [[nodiscard]] static QImage normalizeImage(const QImage& image);
    // Prepare an output-aligned, premultiplied background for destination-over
    // painting. Geometric coverage keeps translucent selected pixels unchanged.
    // Preview surfaces use the same operation as exports before adding checkerboard.
    [[nodiscard]] static QImage maskBackgroundForSelection(QImage background, const QImage& content,
                                                           const QPainterPath& selectionPath,
                                                           const QPoint& contentOffset = {});
    // Background uses output pixel coordinates, including effect padding, and contains
    // capture pixels only. Its opacity is applied beneath the masked content and shadow.
    [[nodiscard]] static QImage compose(const QImage& content, const ScreenshotResultStyle& style,
                                        qreal devicePixelRatio = 1.0, qreal outputOpacity = 1.0,
                                        const QImage& background = {});
    // Compose only a pixel viewport of the DPR-1 result, preserving the full content origin,
    // rounded geometry, shadow tile phase and transparent padding outside the result bounds.
    [[nodiscard]] static QImage composeViewport(const QImage& content,
                                                const ScreenshotResultStyle& style,
                                                const QRect& viewport, qreal outputOpacity = 1.0);
    // Includes complete corner and border curves where clipping would change pixel coverage.
    // content is an unscaled tile of the full content raster at contentOffset.
    [[nodiscard]] static QImage composeViewport(const QImage& content, const QSize& fullContentSize,
                                                QPoint contentOffset,
                                                const ScreenshotResultStyle& style,
                                                const QRect& viewport, qreal outputOpacity = 1.0);

    [[nodiscard]] static QRect viewportRenderRect(const QSize& contentPixelSize,
                                                  const ScreenshotResultStyle& style,
                                                  const QRect& viewport);

    // Paint color over the viewport outside the selection, preserving its fill rule
    // and curves. Honors the caller's transform, clip and opacity and restores state.
    // Winding coverage uses bounded physical-pixel tiles; parity paths need no raster buffer.
    static void fillOutsideSelection(QPainter& painter, const QRectF& viewportBounds,
                                     const QPainterPath& selectionPath, const QColor& color);

    // Erase capture pixels outside the selection before painting document annotations.
    static void maskLiveSurface(QPainter& painter, const QRectF& viewportBounds,
                                const QPainterPath& selectionPath);

    // Clear the part of the viewport outside the result shape, then place the
    // shadow behind the content. Content may cross the viewport boundary:
    // native pixel extents and integer-DIP widget extents round independently.
    static void finishLiveSurface(QPainter& painter, const QRectF& viewportBounds,
                                  const QRectF& contentBounds, const ScreenshotResultStyle& style,
                                  qreal devicePixelRatio, qreal canvasToViewScale = 1.0);
};

// The compaction plan operates on pixels; bake the original mask before moving
// it so newly touching regions do not lose their individually rounded corners.
inline ScreenshotResultStyle
compactedScreenshotResultStyle(const ScreenshotResultStyle& style,
                               const ScreenshotLayoutCompactionPlan& plan) {
    if (!plan.changed || !style.region)
        return style;
    auto result = ScreenshotResultCompositor::normalizedStyle(style);
    const qreal scale = result.regionScale;
    const auto mask = screenshotSelectionPath(QRectF(QPointF(), plan.sourceSize), result.region,
                                              result.cornerRadius, scale);
    result.region =
        ScreenshotRegionGeometry::fromPath(plan.mapPath(mask), ScreenshotRegionType::Curve);
    result.regionScale = 1.0;
    result.cornerRadius = 0;
    return result;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
