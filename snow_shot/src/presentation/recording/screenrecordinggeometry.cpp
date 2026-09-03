#include "screenrecordinggeometry.h"

#include <cmath>

namespace {
constexpr int kPhysicalBorderWidth = 2;
// The encoder may expand an odd-sized capture by one physical pixel. Keep
// that expansion in a transparent gap instead of on the visible frame.
constexpr int kPhysicalBorderPadding = 1;

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
    const qreal paddingWidth = static_cast<qreal>(kPhysicalBorderPadding) / scale;
    const qreal frameInset = borderWidth + paddingWidth;
    const QRectF frameGeometry =
        logicalRegion.adjusted(-frameInset, -frameInset, frameInset, frameInset);
    const QRect windowGeometry = frameGeometry.toAlignedRect();
    const QRectF selectionRect = logicalRegion.translated(-windowGeometry.topLeft());
    const QRectF frameRect =
        selectionRect.adjusted(-frameInset, -frameInset, frameInset, frameInset);

    return ScreenRecordingAreaFrameGeometry{
        windowGeometry,
        frameRect,
        selectionRect,
        borderWidth,
        paddingWidth,
    };
}

ScreenRecordingAreaBorderGeometry screenRecordingAreaBorderGeometry(const QRectF& frameRect,
                                                                    const QRectF& selectionRect,
                                                                    qreal paddingWidth) {
    const qreal selectionRight = selectionRect.left() + selectionRect.width();
    const qreal selectionBottom = selectionRect.top() + selectionRect.height();
    const qreal borderInnerLeft = selectionRect.left() - paddingWidth;
    const qreal borderInnerTop = selectionRect.top() - paddingWidth;
    const qreal borderInnerRight = selectionRight + paddingWidth;
    const qreal borderInnerBottom = selectionBottom + paddingWidth;
    const qreal frameRight = frameRect.left() + frameRect.width();
    const qreal frameBottom = frameRect.top() + frameRect.height();

    // Extend the vertical strips through the frame corners so the padded gap
    // cannot leave a transparent seam between adjacent sides.
    return ScreenRecordingAreaBorderGeometry{
        QRectF(frameRect.left(), frameRect.top(), frameRect.width(),
               borderInnerTop - frameRect.top()),
        QRectF(frameRect.left(), borderInnerBottom, frameRect.width(),
               frameBottom - borderInnerBottom),
        QRectF(frameRect.left(), frameRect.top(), borderInnerLeft - frameRect.left(),
               frameRect.height()),
        QRectF(borderInnerRight, frameRect.top(), frameRight - borderInnerRight,
               frameRect.height()),
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
