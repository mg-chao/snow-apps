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

bool entersFollowMargin(qreal cursor, qreal origin, qreal extent, qreal minOrigin,
                        qreal maxOrigin) {
    const qreal margin = extent / 12.0;
    return (origin > minOrigin && cursor <= origin + margin) ||
           (origin < maxOrigin && cursor >= origin + extent - margin);
}

qreal easeFollowOrigin(qreal origin, qreal target, qreal fraction) {
#ifdef Q_OS_MACOS
    constexpr qreal settleDistance = 0.25;
#else
    constexpr qreal settleDistance = 0.5;
#endif
    const qreal next = origin + (target - origin) * fraction;
    return qAbs(target - next) <= settleDistance ? target : next;
}

qreal keepFollowCursorInView(qreal origin, qreal cursor, qreal extent, qreal minOrigin,
                             qreal maxOrigin) {
    // Leave room for the cursor at the far edge, including when the guide is rasterized.
    const qreal edgeInset = qMin(1.0, extent * 0.5);
    return qBound(minOrigin, qBound(cursor - (extent - edgeInset), origin, cursor), maxOrigin);
}

} // namespace

namespace snow_shot::presentation::recording {
ScreenRecordingFollowGuideMotion::ScreenRecordingFollowGuideMotion(const QSizeF& selectionSize,
                                                                   const QRectF& bounds,
                                                                   const QPointF& cursor,
                                                                   bool smoothMovement,
                                                                   bool reduceJitter)
    : m_interior(bounds), m_smoothMovement(smoothMovement), m_reduceJitter(reduceJitter) {
    constexpr qreal inset = screenRecordingPhysicalFrameInset;
    m_interior.adjust(inset, inset, -inset, -inset);
    if (selectionSize.isEmpty() || !m_interior.isValid())
        return;
    const qreal fit = qMin(1.0, qMin(m_interior.width() / selectionSize.width(),
                                     m_interior.height() / selectionSize.height()));
    m_size = selectionSize * fit;
    recenter(cursor);
}

QPointF ScreenRecordingFollowGuideMotion::centeredOrigin(const QPointF& center) const {
    return {qBound(m_interior.left(), center.x() - m_size.width() / 2.0,
                   qMax(m_interior.left(), m_interior.right() - m_size.width())),
            qBound(m_interior.top(), center.y() - m_size.height() / 2.0,
                   qMax(m_interior.top(), m_interior.bottom() - m_size.height()))};
}

void ScreenRecordingFollowGuideMotion::track(const QPointF& cursor) {
    if (m_size.isEmpty())
        return;
    // Use the destination's margins while the guide is catching up, as the capture crop does.
    const qreal maxX = qMax(m_interior.left(), m_interior.right() - m_size.width());
    const qreal maxY = qMax(m_interior.top(), m_interior.bottom() - m_size.height());
    if (!m_reduceJitter ||
        entersFollowMargin(cursor.x(), m_target.x(), m_size.width(), m_interior.left(), maxX) ||
        entersFollowMargin(cursor.y(), m_target.y(), m_size.height(), m_interior.top(), maxY)) {
        m_target = centeredOrigin(cursor);
    }
    if (!m_smoothMovement) {
        m_position = m_target;
        return;
    }
    m_position = {
        keepFollowCursorInView(m_position.x(), cursor.x(), m_size.width(), m_interior.left(), maxX),
        keepFollowCursorInView(m_position.y(), cursor.y(), m_size.height(), m_interior.top(), maxY),
    };
}

void ScreenRecordingFollowGuideMotion::advance(std::chrono::duration<qreal> elapsed) {
    // Match the recording crop's 50 ms half-life.
    constexpr auto halfLife = std::chrono::duration<qreal>(std::chrono::milliseconds(50));
    const qreal fraction = 1.0 - std::exp2(-elapsed / halfLife);
    m_position = {easeFollowOrigin(m_position.x(), m_target.x(), fraction),
                  easeFollowOrigin(m_position.y(), m_target.y(), fraction)};
}

void ScreenRecordingFollowGuideMotion::recenter(const QPointF& center) {
    m_position = m_target = centeredOrigin(center);
}

bool ScreenRecordingFollowGuideMotion::isMoving() const {
    // A final tick sets each axis exactly to its target.
    return m_position.x() != m_target.x() || m_position.y() != m_target.y();
}

QRectF ScreenRecordingFollowGuideMotion::viewport() const {
    return m_size.isEmpty() ? QRectF() : QRectF(m_position, m_size);
}

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
