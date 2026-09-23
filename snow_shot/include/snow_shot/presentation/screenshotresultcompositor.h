#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H

#include <QColor>
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
    int cornerRadius = 0;
    int shadowWidth = 0;
    QColor shadowColor = QColor(0x33, 0x33, 0x33);
    // Immutable geometry snapshot, relative to the content origin, in canvas units.
    std::optional<ScreenshotRegionGeometry> region;
    qreal regionScale = 1.0;
};

inline QByteArray encodeScreenshotResultStyle(const ScreenshotResultStyle& style) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream << style.cornerRadius << style.shadowWidth << style.shadowColor;
    stream << quint32(0x53535247) << quint8(1) << style.regionScale
           << (style.region ? QJsonDocument(style.region->toJson()).toJson(QJsonDocument::Compact)
                            : QByteArray());
    return bytes;
}

// An invalid extension rejects the record: silently dropping its geometry would
// expose pixels outside the selected outline when editing a restored pin.
inline std::optional<ScreenshotResultStyle> decodeScreenshotResultStyle(const QByteArray& bytes) {
    ScreenshotResultStyle style;
    if (bytes.isEmpty())
        return style;
    QDataStream stream(bytes);
    stream >> style.cornerRadius >> style.shadowWidth >> style.shadowColor;
    if (stream.status() != QDataStream::Ok)
        return std::nullopt;
    if (stream.atEnd())
        return style;
    quint32 marker = 0;
    quint8 version = 0;
    QByteArray geometry;
    stream >> marker >> version >> style.regionScale >> geometry;
    if (stream.status() != QDataStream::Ok || !stream.atEnd() || marker != 0x53535247 ||
        version != 1 || !std::isfinite(style.regionScale) || style.regionScale <= 0 ||
        style.regionScale > 256)
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

class ScreenshotResultCompositor final {
  public:
    [[nodiscard]] static ScreenshotResultStyle normalizedStyle(const ScreenshotResultStyle& style);
    [[nodiscard]] static ScreenshotResultLayout layoutForContent(const QSize& contentPixelSize,
                                                                 const ScreenshotResultStyle& style,
                                                                 qreal devicePixelRatio = 1.0);
    static void restoreBakedExterior(QImage& image, const QImage& background,
                                     const QPainterPath& path);
    [[nodiscard]] static QImage normalizeImage(const QImage& image);
    [[nodiscard]] static QImage compose(const QImage& content, const ScreenshotResultStyle& style,
                                        qreal devicePixelRatio = 1.0, qreal outputOpacity = 1.0);

    // Clear the part of the viewport outside the result shape, then place the
    // shadow behind the content. Content may cross the viewport boundary:
    // native pixel extents and integer-DIP widget extents round independently.
    static void finishLiveSurface(QPainter& painter, const QRectF& viewportBounds,
                                  const QRectF& contentBounds, const ScreenshotResultStyle& style,
                                  qreal devicePixelRatio, qreal canvasToViewScale = 1.0);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTRESULTCOMPOSITOR_H
