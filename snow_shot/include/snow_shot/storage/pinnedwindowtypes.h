#ifndef SNOW_SHOT_STORAGE_PINNEDWINDOWTYPES_H
#define SNOW_SHOT_STORAGE_PINNEDWINDOWTYPES_H

#include "pinnedwindowplacement.h"
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
    QSize initialWindowSize;
    PinnedWindowPlacement placement;
    PinnedWindowPlacement preThumbnailPlacement;
    PinnedWindowPlacement hideToTopPlacement;
    // Window geometry snapshots in placement.units; image dimensions stay in image.
    QRect nativeGeometry;
    QString screenName;
    QString screenSerial;
    QRect screenLogicalGeometry;
    QRect screenWindowGeometry;
    // Informational backing scale. Restore preserves window units, independently
    // of the destination display backing scale.
    qreal screenDpi = 1.0;
    // Informational snapshot of the derived scale value at save time; restore
    // re-derives it from initialWindowSize and nativeGeometry instead.
    double scalePercent = 100.0;
    int opacityPercent = 100;
    int clickThroughOpacityPercent = 50;
    QTransform imageTransform;
    int quarterTurns = 0;
    bool hideToTopMode = false;
    QRect hideToTopHandleNativeGeometry;
    int hideToTopAccentIndex = -1;
    bool thumbnailMode = false;
    bool clickThroughMode = false;
    bool alwaysOnTop = true;
    QRect preThumbnailNativeGeometry;
    QByteArray resultStyle;
    QByteArray canvasSession;
    QByteArray recognitionResults;
    bool recognitionVisible = false;
    bool translationVisible = false;
    QDateTime updatedUtc;
};

} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_PINNEDWINDOWTYPES_H
