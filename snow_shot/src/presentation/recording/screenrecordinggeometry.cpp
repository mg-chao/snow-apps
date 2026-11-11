#include "screenrecordinggeometry.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include <QGuiApplication>
#include <QScreen>

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
int screenRecordingMinimumExtent(qreal physicalScale) {
    return qMax(1, static_cast<int>(std::ceil(10.0 / validScale(physicalScale))));
}

QRect screenRecordingNormalizedRegion(const QRect& region, const QRect& bounds,
                                      qreal physicalScale) {
    if (!region.isValid() || region.isEmpty())
        return {};
    const int minimum = screenRecordingMinimumExtent(physicalScale);
    QRect result = region;
    if (result.width() < minimum) {
        result.setWidth(minimum);
        if (bounds.isValid() && bounds.width() >= minimum)
            result.moveLeft(
                qBound(bounds.left(), result.left(), bounds.x() + bounds.width() - minimum));
    }
    if (result.height() < minimum) {
        result.setHeight(minimum);
        if (bounds.isValid() && bounds.height() >= minimum)
            result.moveTop(
                qBound(bounds.top(), result.top(), bounds.y() + bounds.height() - minimum));
    }
    return result;
}

QRect screenRecordingNormalizedRegion(const QRect& region) {
#ifdef Q_OS_MACOS
    QScreen* screen = QGuiApplication::screenAt(region.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    return screenRecordingNormalizedRegion(region, screen ? screen->geometry() : QRect(),
                                           screen ? screen->devicePixelRatio() : 1.0);
#else
    QScreen* screen = ScreenshotGeometryMapper::screenForPhysicalRect(region);
    return screenRecordingNormalizedRegion(
        region, screen ? ScreenshotGeometryMapper::physicalRectForScreen(*screen) : QRect());
#endif
}

ScreenRecordingObservedGeometry screenRecordingObservedGeometry(const QRect& physicalClientRect,
                                                                qreal physicalScale,
                                                                const QMargins& physicalInsets) {
    const QRect selected = physicalClientRect.marginsRemoved(physicalInsets);
    if (selected.width() < 2 || selected.height() < 2) {
        return {};
    }
    const qreal scale = validScale(physicalScale);
    const QRectF selection(physicalInsets.left() / scale, physicalInsets.top() / scale,
                           selected.width() / scale, selected.height() / scale);
    const qreal inset = screenRecordingPhysicalFrameInset / scale;
    return {selected, selection.adjusted(-inset, -inset, inset, inset), selection,
            kPhysicalBorderPadding / scale};
}

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
        windowGeometry, frameRect, selectionRect, borderWidth, paddingWidth,
    };
}

ScreenRecordingAreaFrameGeometry screenRecordingFollowFrameGeometry(const QRectF& logicalRegion,
                                                                    qreal physicalScale) {
    auto geometry = screenRecordingAreaFrameGeometry(logicalRegion, physicalScale);
    if (!logicalRegion.isValid() || logicalRegion.isEmpty())
        return geometry;
    // Keep one physical pixel outside the frame for native origin/size rounding.
    // The extra logical pixel covers every fractional origin without resizing
    // the backing store as the viewport pans. The painted frame remains exact.
    const qreal guard = 1.0 / validScale(physicalScale);
    const QRectF frame = logicalRegion.adjusted(-geometry.borderWidth - geometry.paddingWidth,
                                                -geometry.borderWidth - geometry.paddingWidth,
                                                geometry.borderWidth + geometry.paddingWidth,
                                                geometry.borderWidth + geometry.paddingWidth);
    const QRectF envelope = frame.adjusted(-guard, -guard, guard, guard);
    geometry.windowGeometry = QRect(QPoint(static_cast<int>(std::floor(envelope.left())),
                                           static_cast<int>(std::floor(envelope.top()))),
                                    QSize(static_cast<int>(std::ceil(envelope.width())) + 1,
                                          static_cast<int>(std::ceil(envelope.height())) + 1));
    geometry.selectionRect = logicalRegion.translated(-geometry.windowGeometry.topLeft());
    geometry.frameRect = frame.translated(-geometry.windowGeometry.topLeft());
    return geometry;
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

QRect screenRecordingCompatibleCaptureRegion(const QRect& selectedRecordingRegion,
                                             const QRect& physicalBounds) {
    if (!selectedRecordingRegion.isValid() || selectedRecordingRegion.isEmpty()) {
        return {};
    }

    QRect captureRegion = selectedRecordingRegion;
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
    if ((maximumIsLandscape && captureIsPortrait) || (maximumIsPortrait && captureIsLandscape)) {
        return maximumSize.transposed();
    }
    return maximumSize;
}
} // namespace snow_shot::presentation::recording
