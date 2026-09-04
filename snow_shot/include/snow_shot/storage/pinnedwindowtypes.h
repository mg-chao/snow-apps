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

struct PinnedWindowGroup final {
    QString id;
    QString name;
    bool builtIn = false;
};

enum class PinnedWindowSourceKind {
    ImageData,
    ClipboardText,
    ClipboardImageFile,
};

struct PinnedWindowRecord final {
    QString id;
    QString groupId = QStringLiteral("default");
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
    // Informational only: the restore path recreates the window at the saved
    // physical pixel size and derives the scale from those pixels, so the
    // saving monitor's DPI never influences a restore.
    qreal screenDpi = 1.0;
    // Informational snapshot of the derived scale value at save time; restore
    // re-derives it from initialPhysicalSize and nativeGeometry instead.
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
