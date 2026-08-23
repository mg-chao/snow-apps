#include "screenrecordinggeometry.h"

#include <cmath>

namespace {
constexpr int kPhysicalBorderWidth = 2;

qreal validScale(qreal scale) {
    return std::isfinite(scale) && scale > 0.0 ? scale : 1.0;
}

} // namespace

namespace snow_shot::presentation::recording {
ScreenRecordingAreaFrameGeometry screenRecordingAreaFrameGeometry(const QRectF& logicalRegion,
                                                                qreal physicalScale) {
    if (!logicalRegion.isValid() || logicalRegion.isEmpty()) {
        return {};
    }

    const qreal scale = validScale(physicalScale);
    const qreal borderWidth = static_cast<qreal>(kPhysicalBorderWidth) / scale;
    const QRectF frameGeometry =
        logicalRegion.adjusted(-borderWidth, -borderWidth, borderWidth, borderWidth);
    const QRect windowGeometry = frameGeometry.toAlignedRect();
    const QRectF selectionRect = logicalRegion.translated(-windowGeometry.topLeft());
    const QRectF frameRect =
        selectionRect.adjusted(-borderWidth, -borderWidth, borderWidth, borderWidth);

    return ScreenRecordingAreaFrameGeometry{
        windowGeometry,
        frameRect,
        selectionRect,
        borderWidth,
    };
}

QRect screenRecordingCompatibleCaptureRegion(const QRect& selectedPhysicalRegion,
                                            const QRect& physicalBounds) {
    if (!selectedPhysicalRegion.isValid() || selectedPhysicalRegion.isEmpty()) {
        return {};
    }

    QRect captureRegion = selectedPhysicalRegion;
    if (captureRegion.width() % 2 != 0) {
        if (physicalBounds.isValid() && captureRegion.right() >= physicalBounds.right() &&
            captureRegion.left() > physicalBounds.left()) {
            captureRegion.setLeft(captureRegion.left() - 1);
        } else {
            captureRegion.setWidth(captureRegion.width() + 1);
        }
    }
    if (captureRegion.height() % 2 != 0) {
        if (physicalBounds.isValid() && captureRegion.bottom() >= physicalBounds.bottom() &&
            captureRegion.top() > physicalBounds.top()) {
            captureRegion.setTop(captureRegion.top() - 1);
        } else {
            captureRegion.setHeight(captureRegion.height() + 1);
        }
    }
    return captureRegion;
}

QSize screenRecordingMaximumSizeForClarity(const QString& clarity) {
    if (clarity == QStringLiteral("4k")) {
        return {3840, 2160};
    }
    if (clarity == QStringLiteral("2k")) {
        return {2560, 1440};
    }
    if (clarity == QStringLiteral("720p")) {
        return {1280, 720};
    }
    if (clarity == QStringLiteral("480p")) {
        return {854, 480};
    }
    return {1920, 1080};
}

QSize screenRecordingOrientedMaximumSize(const QSize& maximumSize, const QSize& captureSize) {
    const bool maximumIsLandscape = maximumSize.width() > maximumSize.height();
    const bool maximumIsPortrait = maximumSize.height() > maximumSize.width();
    const bool captureIsLandscape = captureSize.width() > captureSize.height();
    const bool captureIsPortrait = captureSize.height() > captureSize.width();
    if ((maximumIsLandscape && captureIsPortrait) ||
        (maximumIsPortrait && captureIsLandscape)) {
        return maximumSize.transposed();
    }
    return maximumSize;
}
} // namespace snow_shot::presentation::recording
