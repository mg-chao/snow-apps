#ifndef SNOW_SHOT_STORAGE_PINNEDWINDOWTYPES_H
#define SNOW_SHOT_STORAGE_PINNEDWINDOWTYPES_H

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTransform>

namespace snow_shot::storage {

enum class PinnedWindowSourceKind {
    ImageData,
    ClipboardText,
    ClipboardImageFile,
};

struct PinnedWindowRecord final {
    QString id;
    PinnedWindowSourceKind sourceKind = PinnedWindowSourceKind::ImageData;
    QImage image;
    QString originalFilePath;
    QString originalFileName;
    QString originalHtml;
    QString originalText;
    double firstCreationTextDpi = 1.0;
    QRectF canvasSourceRect;
    QRectF contentCanvasRect;
    QRectF surfaceCanvasRect;
    QSize initialPhysicalSize;
    QRect nativeGeometry;
    QString screenName;
    QString screenSerial;
    QRect screenLogicalGeometry;
    QRect screenPhysicalGeometry;
    qreal screenDpi = 1.0;
    double scalePercent = 100.0;
    int opacityPercent = 100;
    QTransform imageTransform;
    int quarterTurns = 0;
    bool thumbnailMode = false;
    QRect preThumbnailNativeGeometry;
    QByteArray resultStyle;
    QByteArray canvasSession;
    QByteArray recognitionResults;
    QDateTime updatedUtc;
};

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_PINNEDWINDOWTYPES_H
