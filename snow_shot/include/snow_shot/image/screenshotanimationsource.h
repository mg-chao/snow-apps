#ifndef SNOW_SHOT_IMAGE_SCREENSHOTANIMATIONSOURCE_H
#define SNOW_SHOT_IMAGE_SCREENSHOTANIMATIONSOURCE_H

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>

#include <memory>

// Immutable admitted source shared by playback, import and private persistence.
// Frame publication never changes these bytes or the initial preview.
struct ScreenshotAnimationSource final {
    QByteArray encoded;
    QString format;
    QSize canvasSize;
    // Region of the decoded, EXIF-oriented canvas. Encoded bytes remain immutable.
    QRect sourceCropRect;
    QVector<qint64> frameDurationsNs;
    // Total plays, rather than GIF's repeat count. Zero means infinite.
    quint64 totalPlayCount = 1;
    // Premultiplied source pixels with their original profile. The QImage pixel
    // representation is an implementation detail; exports preserve these colors.
    QImage firstFrame;
    // Prepared on the import worker. Original pixels and their profile remain
    // in firstFrame; this raster is ARGB32 premultiplied and DPR-neutral.
    // Untagged input remains assumed sRGB, matching playback preparation. This
    // shares firstFrame storage when its format and profile already match.
    QImage firstDisplayFrame;

    [[nodiscard]] bool isAnimated() const {
        return frameDurationsNs.size() > 1;
    }
};

#endif // SNOW_SHOT_IMAGE_SCREENSHOTANIMATIONSOURCE_H
