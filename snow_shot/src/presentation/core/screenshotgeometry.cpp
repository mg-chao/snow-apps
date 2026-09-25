#include "snow_shot/presentation/screenshotgeometry.h"

#include "snow_shot/platform/windows/monitorgeometry.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotselectionlimits.h"

#include <QGuiApplication>
#include <QScreen>
#ifdef Q_OS_MACOS
#include <QtGui/qscreen_platform.h>
#import <AppKit/AppKit.h>
#endif
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QSize scaledSelectionPixelSize(const QSize& selection, qreal scale) {
    if (selection.width() < 1 || selection.height() < 1 || !std::isfinite(scale) || scale <= 0.0) {
        return {};
    }
    const double width = static_cast<double>(selection.width()) * scale;
    const double height = static_cast<double>(selection.height()) * scale;
    if (!std::isfinite(width) || !std::isfinite(height) || width < 1.0 || height < 1.0) {
        return {};
    }
    const double pixelWidth = std::ceil(width);
    const double pixelHeight = std::ceil(height);
    if (!std::isfinite(pixelWidth) || !std::isfinite(pixelHeight) || pixelWidth < 1.0 ||
        pixelHeight < 1.0 ||
        pixelWidth > static_cast<double>(std::numeric_limits<int>::max()) / 4.0 ||
        pixelHeight > static_cast<double>(std::numeric_limits<int>::max()) ||
        pixelWidth * pixelHeight >
            static_cast<double>(std::numeric_limits<qsizetype>::max()) / 4.0) {
        return {};
    }
    return QSize(static_cast<int>(pixelWidth), static_cast<int>(pixelHeight));
}
} // namespace

QSize screenshotSelectionRenderedPixelSize(const QSize& selection, qreal scale) {
    return scaledSelectionPixelSize(selection, scale);
}

int screenshotSelectionRenderedShadowPixels(int shadowWidth, qreal scale) {
    if (shadowWidth <= 0 || !std::isfinite(scale) || scale <= 0.0) {
        return 0;
    }
    const double scaled = static_cast<double>(shadowWidth) * scale;
    if (!std::isfinite(scaled)) {
        return 0;
    }
    return std::clamp(qRound(scaled), 0,
                      snow_shot::presentation::kScreenshotSelectionShadowWidthMax);
}

ScreenshotSelectionRenderSpec
screenshotSelectionRenderSpec(const ScreenshotDisplaySession& displays, const QRect& selection) {
    ScreenshotSelectionRenderSpec spec;
    spec.canvasRect = selection;
    if (selection.isEmpty())
        return spec;
    bool intersects = false;
    bool valid = true;
    displays.forEachImageSource([&](qsizetype, const CapturedDisplayModel& display) {
        const QRectF source = ScreenshotGeometryMapper::displayImageSourceCanvasRect(display);
        if (display.image.isNull() || !source.intersects(QRectF(selection)))
            return;
        intersects = true;
        if (display.canvasUsesPoints) {
            if (!std::isfinite(display.backingScale) || display.backingScale <= 0) {
                valid = false;
                return;
            }
            spec.scale = std::max(spec.scale, display.backingScale);
        }
    });
    if (!valid || !intersects)
        return spec;
    spec.pixelSize = screenshotSelectionRenderedPixelSize(selection.size(), spec.scale);
    if (!spec.isValid())
        return spec;
    spec.canvasToImage.scale(spec.scale, spec.scale);
    spec.canvasToImage.translate(-selection.x(), -selection.y());
    return spec;
}

namespace {
// Keep adaptive pinned-image sizing within the canvas engine's camera range.
constexpr double kMinimumZoom = 0.1;

int floorToInt(double value) {
    return static_cast<int>(std::floor(value));
}

int ceilToInt(double value) {
    return static_cast<int>(std::ceil(value));
}

double squaredDistanceToRect(const ScreenshotHalfOpenRect& rect, const QPointF& point) {
    if (rect.isEmpty()) {
        return std::numeric_limits<double>::max();
    }

    const double x = std::clamp(point.x(), rect.left, rect.right);
    const double y = std::clamp(point.y(), rect.top, rect.bottom);
    const double dx = point.x() - x;
    const double dy = point.y() - y;
    return dx * dx + dy * dy;
}

double scaleOrFallbackValue(double numerator, double denominator) {
    return denominator > 0.0 ? numerator / denominator : 1.0;
}

QPoint roundPointValue(const QPointF& point) {
    return QPoint(qRound(point.x()), qRound(point.y()));
}

QPointF mapPointBetweenRects(const QPointF& point, const QRect& from, const QRect& to) {
    const double scaleX =
        scaleOrFallbackValue(static_cast<double>(to.width()), static_cast<double>(from.width()));
    const double scaleY =
        scaleOrFallbackValue(static_cast<double>(to.height()), static_cast<double>(from.height()));
    return QPointF(
        static_cast<double>(to.left()) + (point.x() - static_cast<double>(from.left())) * scaleX,
        static_cast<double>(to.top()) + (point.y() - static_cast<double>(from.top())) * scaleY);
}

QSizeF scaleBetweenRects(const QRect& from, const QRect& to) {
    return QSizeF(
        scaleOrFallbackValue(static_cast<double>(to.width()), static_cast<double>(from.width())),
        scaleOrFallbackValue(static_cast<double>(to.height()), static_cast<double>(from.height())));
}

struct DisplayCoordinateTransform {
    explicit DisplayCoordinateTransform(const CapturedDisplayModel& sourceDisplay)
        : display(sourceDisplay) {}

    [[nodiscard]] QPointF canvasToLogical(const QPointF& point) const {
        if (hasDeviceScale()) {
            return QPointF(display.logicalRect.topLeft()) +
                   (point - QPointF(display.canvasRect.topLeft())) / display.logicalToPhysicalScale;
        }
        return mapPointBetweenRects(point, display.canvasRect, display.logicalRect);
    }

    [[nodiscard]] QPointF physicalToLogical(const QPointF& point) const {
        if (hasDeviceScale()) {
            return QPointF(display.logicalRect.topLeft()) +
                   (point - QPointF(display.physicalRect.topLeft())) /
                       display.logicalToPhysicalScale;
        }
        return mapPointBetweenRects(point, display.physicalRect, display.logicalRect);
    }

    [[nodiscard]] QPointF canvasToPhysical(const QPointF& point) const {
        return mapPointBetweenRects(point, display.canvasRect, display.physicalRect);
    }

    [[nodiscard]] QPointF physicalToCanvas(const QPointF& point) const {
        return mapPointBetweenRects(point, display.physicalRect, display.canvasRect);
    }

    [[nodiscard]] QPointF logicalToPhysical(const QPointF& point) const {
        if (hasDeviceScale()) {
            return QPointF(display.physicalRect.topLeft()) +
                   (point - QPointF(display.logicalRect.topLeft())) *
                       display.logicalToPhysicalScale;
        }
        return mapPointBetweenRects(point, display.logicalRect, display.physicalRect);
    }

    [[nodiscard]] QPointF overlayLocalToCanvas(const QPointF& point) const {
        if (hasDeviceScale()) {
            return QPointF(display.canvasRect.topLeft()) + point * display.logicalToPhysicalScale;
        }
        const QRect overlayLocalRect(QPoint(0, 0), display.logicalRect.size());
        return mapPointBetweenRects(point, overlayLocalRect, display.canvasRect);
    }

    [[nodiscard]] ScreenshotHalfOpenRect
    physicalToCanvas(const ScreenshotHalfOpenRect& rect) const {
        if (rect.isEmpty()) {
            return {};
        }

        const QPointF topLeft = physicalToCanvas(rect.topLeft());
        const QPointF bottomRight = physicalToCanvas(rect.bottomRight());
        return ScreenshotHalfOpenRect::fromEdges(topLeft.x(), topLeft.y(), bottomRight.x(),
                                                 bottomRight.y());
    }

    [[nodiscard]] QSizeF canvasToLogicalScale() const {
        if (hasDeviceScale()) {
            const qreal scale = 1.0 / display.logicalToPhysicalScale;
            return QSizeF(scale, scale);
        }
        return scaleBetweenRects(display.canvasRect, display.logicalRect);
    }

    [[nodiscard]] bool hasDeviceScale() const {
        return !display.canvasUsesPoints && std::isfinite(display.logicalToPhysicalScale) &&
               display.logicalToPhysicalScale > 0.0;
    }

    const CapturedDisplayModel& display;
};

template <typename RectForDisplay>
const CapturedDisplayModel* nearestDisplayForPoint(const ScreenshotDisplaySession& displaySession,
                                                   const QPointF& point,
                                                   RectForDisplay rectForDisplay) {
    double bestDistance = std::numeric_limits<double>::max();
    const CapturedDisplayModel* bestDisplay = nullptr;
    displaySession.forEachActiveDisplay([&](qsizetype, const CapturedDisplayModel& display) {
        const ScreenshotHalfOpenRect rect =
            ScreenshotHalfOpenRect::fromRect(rectForDisplay(display));
        const double distance = squaredDistanceToRect(rect, point);
        if (distance < bestDistance) {
            bestDistance = distance;
            bestDisplay = &display;
        }
    });
    return bestDisplay;
}
} // namespace

ScreenshotHalfOpenRect ScreenshotHalfOpenRect::fromRect(const QRect& rect) {
    if (!rect.isValid() || rect.isEmpty()) {
        return {};
    }
    return fromEdges(static_cast<double>(rect.left()), static_cast<double>(rect.top()),
                     static_cast<double>(rect.left() + rect.width()),
                     static_cast<double>(rect.top() + rect.height()));
}

ScreenshotHalfOpenRect ScreenshotHalfOpenRect::fromRectF(const QRectF& rect) {
    const QRectF normalized = rect.normalized();
    if (!normalized.isValid() || normalized.isEmpty()) {
        return {};
    }
    return fromEdges(normalized.left(), normalized.top(), normalized.left() + normalized.width(),
                     normalized.top() + normalized.height());
}

ScreenshotHalfOpenRect ScreenshotHalfOpenRect::fromEdges(double left, double top, double right,
                                                         double bottom) {
    ScreenshotHalfOpenRect rect;
    rect.left = left;
    rect.top = top;
    rect.right = right;
    rect.bottom = bottom;
    return rect;
}

bool ScreenshotHalfOpenRect::isValid() const {
    return std::isfinite(left) && std::isfinite(top) && std::isfinite(right) &&
           std::isfinite(bottom);
}

bool ScreenshotHalfOpenRect::isEmpty() const {
    return !isValid() || right <= left || bottom <= top;
}

double ScreenshotHalfOpenRect::width() const {
    return right - left;
}

double ScreenshotHalfOpenRect::height() const {
    return bottom - top;
}

QPointF ScreenshotHalfOpenRect::topLeft() const {
    return QPointF(left, top);
}

QPointF ScreenshotHalfOpenRect::bottomRight() const {
    return QPointF(right, bottom);
}

QPointF ScreenshotHalfOpenRect::center() const {
    return QPointF((left + right) / 2.0, (top + bottom) / 2.0);
}

QRectF ScreenshotHalfOpenRect::toRectF() const {
    if (isEmpty()) {
        return {};
    }
    return QRectF(QPointF(left, top), QSizeF(width(), height()));
}

QRect ScreenshotHalfOpenRect::toAlignedQRect() const {
    if (isEmpty()) {
        return {};
    }
    const int alignedLeft = floorToInt(left);
    const int alignedTop = floorToInt(top);
    const int alignedRight = ceilToInt(right);
    const int alignedBottom = ceilToInt(bottom);
    return QRect(alignedLeft, alignedTop, std::max(0, alignedRight - alignedLeft),
                 std::max(0, alignedBottom - alignedTop));
}

bool ScreenshotHalfOpenRect::contains(const QPointF& point) const {
    return !isEmpty() && point.x() >= left && point.x() < right && point.y() >= top &&
           point.y() < bottom;
}

bool ScreenshotHalfOpenRect::intersects(const ScreenshotHalfOpenRect& other) const {
    return !isEmpty() && !other.isEmpty() && right > other.left && other.right > left &&
           bottom > other.top && other.bottom > top;
}

ScreenshotHalfOpenRect
ScreenshotHalfOpenRect::intersected(const ScreenshotHalfOpenRect& other) const {
    if (isEmpty() || other.isEmpty()) {
        return {};
    }
    return fromEdges(std::max(left, other.left), std::max(top, other.top),
                     std::min(right, other.right), std::min(bottom, other.bottom));
}

ScreenshotHalfOpenRect ScreenshotHalfOpenRect::united(const ScreenshotHalfOpenRect& other) const {
    if (isEmpty()) {
        return other;
    }
    if (other.isEmpty()) {
        return *this;
    }
    return fromEdges(std::min(left, other.left), std::min(top, other.top),
                     std::max(right, other.right), std::max(bottom, other.bottom));
}

namespace {
void rebuildDisplayGeometry(ScreenshotDisplaySession& displaySession, QPoint& canvasOrigin,
                            QRectF& canvasBounds) {
    canvasOrigin = QPoint();
    canvasBounds = QRectF();

    int minLeft = std::numeric_limits<int>::max();
    int minTop = std::numeric_limits<int>::max();
    displaySession.forEachActiveDisplay([&](qsizetype, const CapturedDisplayModel& display) {
        if (display.physicalRect.isNull()) {
            return;
        }
        minLeft = std::min(minLeft, display.physicalRect.left());
        minTop = std::min(minTop, display.physicalRect.top());
    });
    if (minLeft == std::numeric_limits<int>::max() || minTop == std::numeric_limits<int>::max()) {
        return;
    }

    canvasOrigin = QPoint(minLeft, minTop);
    ScreenshotHalfOpenRect computedCanvasBounds;
    displaySession.forEachMutableActiveDisplay([&](qsizetype, CapturedDisplayModel& display) {
        if (display.physicalRect.isNull()) {
            return;
        }

        if (display.geometryResolved) {
            display.canvasRect =
                (display.canvasUsesPoints ? display.logicalRect : display.physicalRect)
                    .translated(-canvasOrigin);
        } else if (display.canvasUsesPoints && !display.capturedLogicalRect.isEmpty()) {
            display.logicalRect = display.capturedLogicalRect;
            display.canvasRect = display.logicalRect.translated(-canvasOrigin);
#ifdef Q_OS_MACOS
            display.screen = nullptr;
            for (QScreen* screen : QGuiApplication::screens()) {
                auto* native = screen->nativeInterface<QNativeInterface::QCocoaScreen>();
                if (native &&
                    [[[native->nativeScreen() deviceDescription] objectForKey:@"NSScreenNumber"]
                        unsignedIntValue] == display.nativeDisplayId) {
                    display.screen = screen;
                    break;
                }
            }
#endif
        } else {
            display.canvasRect = display.physicalRect.translated(-canvasOrigin);
            display.screen = ScreenshotGeometryMapper::screenForCaptureDisplay(
                display.name, display.physicalRect);
            display.logicalToPhysicalScale =
                display.screen != nullptr ? display.screen->devicePixelRatio() : 0.0;
            display.logicalRect = ScreenshotGeometryMapper::logicalRectForPhysicalRect(
                display.physicalRect, display.screen);
            if (display.screen != nullptr &&
                display.physicalRect ==
                    ScreenshotGeometryMapper::physicalRectForScreen(*display.screen)) {
                // A full-monitor overlay uses Qt's window extent; its image transform
                // retains the fractional extent independently of this integer size.
                display.logicalRect = display.screen->geometry();
            }
        }

        const ScreenshotHalfOpenRect canvasRect =
            ScreenshotHalfOpenRect::fromRect(display.canvasRect);
        computedCanvasBounds = computedCanvasBounds.united(canvasRect);

        const double scaleX =
            scaleOrFallbackValue(static_cast<double>(display.logicalRect.width()),
                                 static_cast<double>(display.physicalRect.width()));
        const double scaleY =
            scaleOrFallbackValue(static_cast<double>(display.logicalRect.height()),
                                 static_cast<double>(display.physicalRect.height()));
        if (!display.logicalRect.isValid() || scaleX <= 0.0 || scaleY <= 0.0) {
            qWarning().noquote() << QStringLiteral(
                                        "Invalid screenshot display geometry stable_id=%1 name=%2 "
                                        "physical=%3 canvas=%4 logical=%5")
                                        .arg(display.stableId, display.name)
                                        .arg(QString::fromLatin1("%1,%2 %3x%4")
                                                 .arg(display.physicalRect.left())
                                                 .arg(display.physicalRect.top())
                                                 .arg(display.physicalRect.width())
                                                 .arg(display.physicalRect.height()))
                                        .arg(QString::fromLatin1("%1,%2 %3x%4")
                                                 .arg(display.canvasRect.left())
                                                 .arg(display.canvasRect.top())
                                                 .arg(display.canvasRect.width())
                                                 .arg(display.canvasRect.height()))
                                        .arg(QString::fromLatin1("%1,%2 %3x%4")
                                                 .arg(display.logicalRect.left())
                                                 .arg(display.logicalRect.top())
                                                 .arg(display.logicalRect.width())
                                                 .arg(display.logicalRect.height()));
        }
    });

    canvasBounds = computedCanvasBounds.toRectF();
}
} // namespace

void ScreenshotGeometryMapper::rebuild(ScreenshotDisplaySession& displaySession) {
    rebuildDisplayGeometry(displaySession, m_canvasOrigin, m_canvasBounds);
}

void ScreenshotGeometryMapper::clear() {
    m_canvasOrigin = QPoint();
    m_canvasBounds = QRectF();
}

bool ScreenshotGeometryMapper::isEmpty() const {
    return m_canvasBounds.isNull() || m_canvasBounds.isEmpty();
}

QPoint ScreenshotGeometryMapper::canvasOrigin() const {
    return m_canvasOrigin;
}

QRectF ScreenshotGeometryMapper::canvasBounds() const {
    return m_canvasBounds;
}

namespace {
const CapturedDisplayModel*
displayForOverlayInDisplaySession(const ScreenshotDisplaySession& displaySession,
                                  const ScreenshotOverlayWindow* overlay) {
    if (overlay == nullptr) {
        return nullptr;
    }
    for (qsizetype index = 0; index < displaySession.size(); ++index) {
        const CapturedDisplayModel& display = displaySession.displayAt(index);
        if (display.active && displaySession.overlayAt(index) == overlay) {
            return &display;
        }
    }
    return nullptr;
}
} // namespace

const CapturedDisplayModel*
ScreenshotGeometryMapper::displayForOverlay(const ScreenshotDisplaySession& displaySession,
                                            const ScreenshotOverlayWindow* overlay) const {
    return displayForOverlayInDisplaySession(displaySession, overlay);
}

namespace {
const CapturedDisplayModel*
displayForPhysicalPointInDisplaySession(const ScreenshotDisplaySession& displaySession,
                                        const QPointF& point) {
    for (qsizetype index = 0; index < displaySession.size(); ++index) {
        const CapturedDisplayModel& display = displaySession.displayAt(index);
        if (display.active &&
            ScreenshotHalfOpenRect::fromRect(display.physicalRect).contains(point)) {
            return &display;
        }
    }
    return nullptr;
}
} // namespace

const CapturedDisplayModel*
ScreenshotGeometryMapper::displayForPhysicalPoint(const ScreenshotDisplaySession& displaySession,
                                                  const QPointF& point) const {
    return displayForPhysicalPointInDisplaySession(displaySession, point);
}

namespace {
const CapturedDisplayModel*
displayForCanvasPointInDisplaySession(const ScreenshotDisplaySession& displaySession,
                                      const QPointF& point) {
    for (qsizetype index = 0; index < displaySession.size(); ++index) {
        const CapturedDisplayModel& display = displaySession.displayAt(index);
        if (display.active &&
            ScreenshotHalfOpenRect::fromRect(display.canvasRect).contains(point)) {
            return &display;
        }
    }
    return nullptr;
}
} // namespace

const CapturedDisplayModel*
ScreenshotGeometryMapper::displayForCanvasPoint(const ScreenshotDisplaySession& displaySession,
                                                const QPointF& point) const {
    return displayForCanvasPointInDisplaySession(displaySession, point);
}

namespace {
const CapturedDisplayModel*
displayForCanvasRectInDisplaySession(const ScreenshotDisplaySession& displaySession,
                                     const QRectF& rect) {
    const ScreenshotHalfOpenRect target = ScreenshotHalfOpenRect::fromRectF(rect);
    if (target.isEmpty()) {
        return displayForCanvasPointInDisplaySession(displaySession, rect.center());
    }

    const QPointF points[] = {
        target.center(),
        target.topLeft(),
        QPointF(std::nextafter(target.right, target.left), target.top),
        QPointF(std::nextafter(target.right, target.left),
                std::nextafter(target.bottom, target.top)),
        QPointF(target.left, std::nextafter(target.bottom, target.top)),
    };
    for (const QPointF& point : points) {
        const CapturedDisplayModel* display =
            displayForCanvasPointInDisplaySession(displaySession, point);
        if (display != nullptr) {
            return display;
        }
    }

    double bestDistance = std::numeric_limits<double>::max();
    const CapturedDisplayModel* bestDisplay = nullptr;
    const QPointF center = target.center();
    displaySession.forEachActiveDisplay([&](qsizetype, const CapturedDisplayModel& display) {
        const QPointF displayCenter = ScreenshotHalfOpenRect::fromRect(display.canvasRect).center();
        const double dx = displayCenter.x() - center.x();
        const double dy = displayCenter.y() - center.y();
        const double distance = dx * dx + dy * dy;
        if (distance < bestDistance) {
            bestDistance = distance;
            bestDisplay = &display;
        }
    });
    return bestDisplay;
}
} // namespace

const CapturedDisplayModel*
ScreenshotGeometryMapper::displayForCanvasRect(const ScreenshotDisplaySession& displaySession,
                                               const QRectF& rect) const {
    return displayForCanvasRectInDisplaySession(displaySession, rect);
}

namespace {
QPointF
canvasPositionForOverlayLocalPointInDisplaySession(const ScreenshotDisplaySession& displaySession,
                                                   const ScreenshotOverlayWindow* overlay,
                                                   const QPointF& localPosition) {
    const CapturedDisplayModel* display =
        displayForOverlayInDisplaySession(displaySession, overlay);
    if (display == nullptr) {
        return localPosition;
    }
    return DisplayCoordinateTransform(*display).overlayLocalToCanvas(localPosition);
}
} // namespace

QPointF ScreenshotGeometryMapper::canvasPositionForOverlayLocalPoint(
    const ScreenshotDisplaySession& displaySession, const ScreenshotOverlayWindow* overlay,
    const QPointF& localPosition) const {
    return canvasPositionForOverlayLocalPointInDisplaySession(displaySession, overlay,
                                                              localPosition);
}

QPointF ScreenshotGeometryMapper::logicalPositionForCanvasPoint(const CapturedDisplayModel& display,
                                                                const QPointF& point) const {
    return DisplayCoordinateTransform(display).canvasToLogical(point);
}

QPointF
ScreenshotGeometryMapper::logicalPositionForPhysicalPoint(const CapturedDisplayModel& display,
                                                          const QPointF& point) const {
    return DisplayCoordinateTransform(display).physicalToLogical(point);
}

namespace {
QPoint
physicalPositionForCanvasPointInDisplaySession(const ScreenshotGeometryMapper& geometry,
                                               const ScreenshotDisplaySession& displaySession,
                                               const QPointF& point) {
    const CapturedDisplayModel* display =
        displayForCanvasPointInDisplaySession(displaySession, point);
    if (display == nullptr) {
        display = nearestDisplayForPoint(
            displaySession, point,
            [](const CapturedDisplayModel& candidate) { return candidate.canvasRect; });
    }
    if (display == nullptr) {
        return roundPointValue(point);
    }

    const DisplayCoordinateTransform transform(*display);
    return geometry.clampPhysicalPointToDisplay(*display,
                                                roundPointValue(transform.canvasToPhysical(point)));
}
} // namespace

QPoint ScreenshotGeometryMapper::physicalPositionForCanvasPoint(
    const ScreenshotDisplaySession& displaySession, const QPointF& point) const {
    return physicalPositionForCanvasPointInDisplaySession(*this, displaySession, point);
}

namespace {
QPointF
canvasPositionForPhysicalPointInDisplaySession(const ScreenshotDisplaySession& displaySession,
                                               const QPointF& point) {
    const CapturedDisplayModel* display =
        displayForPhysicalPointInDisplaySession(displaySession, point);
    if (display == nullptr) {
        display = nearestDisplayForPoint(
            displaySession, point,
            [](const CapturedDisplayModel& candidate) { return candidate.physicalRect; });
    }
    if (display == nullptr) {
        return point;
    }

    return DisplayCoordinateTransform(*display).physicalToCanvas(point);
}
} // namespace

const CapturedDisplayModel*
ScreenshotGeometryMapper::displayForLogicalPoint(const ScreenshotDisplaySession& displays,
                                                 const QPointF& point) const {
    const CapturedDisplayModel* found = nullptr;
    displays.forEachActiveDisplay([&](qsizetype, const CapturedDisplayModel& display) {
        if (!found && ScreenshotHalfOpenRect::fromRect(display.logicalRect).contains(point))
            found = &display;
    });
    return found;
}

QPointF
ScreenshotGeometryMapper::canvasPositionForPhysicalPoint(const CapturedDisplayModel& display,
                                                         const QPointF& point) const {
    return DisplayCoordinateTransform(display).physicalToCanvas(point);
}

QPointF ScreenshotGeometryMapper::canvasPositionForPhysicalPoint(
    const ScreenshotDisplaySession& displaySession, const QPointF& point) const {
    return canvasPositionForPhysicalPointInDisplaySession(displaySession, point);
}

namespace {
QRectF canvasRectForPhysicalRectInDisplaySession(const ScreenshotDisplaySession& displaySession,
                                                 const QRectF& rect, const QString& displayId) {
    const ScreenshotHalfOpenRect target = ScreenshotHalfOpenRect::fromRectF(rect);
    if (target.isEmpty()) {
        return {};
    }

    ScreenshotHalfOpenRect canvasRect;
    displaySession.forEachActiveDisplay([&](qsizetype, const CapturedDisplayModel& display) {
        if (!displayId.isEmpty() && display.stableId != displayId && display.name != displayId)
            return;
        const ScreenshotHalfOpenRect physicalDisplay =
            ScreenshotHalfOpenRect::fromRect(display.physicalRect);
        const ScreenshotHalfOpenRect intersection = target.intersected(physicalDisplay);
        if (intersection.isEmpty()) {
            return;
        }

        const ScreenshotHalfOpenRect mapped =
            DisplayCoordinateTransform(display).physicalToCanvas(intersection);
        canvasRect = canvasRect.united(mapped);
    });

    return canvasRect.toRectF();
}
} // namespace

QRectF
ScreenshotGeometryMapper::canvasRectForPhysicalRect(const ScreenshotDisplaySession& displaySession,
                                                    const QRectF& rect,
                                                    const QString& displayId) const {
    return canvasRectForPhysicalRectInDisplaySession(displaySession, rect, displayId);
}

namespace {
QPoint
physicalPositionForLogicalPointInDisplaySession(const ScreenshotGeometryMapper& geometry,
                                                const ScreenshotDisplaySession& displaySession,
                                                const QPointF& point) {
    const CapturedDisplayModel* matchedDisplay = nullptr;
    for (qsizetype index = 0; index < displaySession.size(); ++index) {
        const CapturedDisplayModel& display = displaySession.displayAt(index);
        if (display.active &&
            ScreenshotHalfOpenRect::fromRect(display.logicalRect).contains(point)) {
            matchedDisplay = &display;
            break;
        }
    }

    if (matchedDisplay == nullptr) {
        matchedDisplay = nearestDisplayForPoint(
            displaySession, point,
            [](const CapturedDisplayModel& candidate) { return candidate.logicalRect; });
    }
    if (matchedDisplay == nullptr) {
        return roundPointValue(point);
    }

    const CapturedDisplayModel& display = *matchedDisplay;
    const DisplayCoordinateTransform transform(display);
    return geometry.clampPhysicalPointToDisplay(
        display, roundPointValue(transform.logicalToPhysical(point)));
}
} // namespace

QPoint ScreenshotGeometryMapper::physicalPositionForLogicalPoint(
    const ScreenshotDisplaySession& displaySession, const QPointF& point) const {
    return physicalPositionForLogicalPointInDisplaySession(*this, displaySession, point);
}

QPoint ScreenshotGeometryMapper::clampPhysicalPointToDisplay(const CapturedDisplayModel& display,
                                                             const QPoint& point) const {
    const ScreenshotHalfOpenRect rect = ScreenshotHalfOpenRect::fromRect(display.physicalRect);
    if (rect.isEmpty()) {
        return point;
    }
    return QPoint(std::clamp(point.x(), floorToInt(rect.left), ceilToInt(rect.right) - 1),
                  std::clamp(point.y(), floorToInt(rect.top), ceilToInt(rect.bottom) - 1));
}

qreal ScreenshotGeometryMapper::canvasToLogicalScale(const CapturedDisplayModel& display) {
    const QSizeF scale = DisplayCoordinateTransform(display).canvasToLogicalScale();
    const qreal scaleX = scale.width();
    const qreal scaleY = scale.height();
    if (scaleX > 0.0 && scaleY > 0.0) {
        return (scaleX + scaleY) / 2.0;
    }
    if (scaleX > 0.0) {
        return scaleX;
    }
    if (scaleY > 0.0) {
        return scaleY;
    }
    return 1.0;
}

QRectF ScreenshotGeometryMapper::displayCanvasRect(const CapturedDisplayModel& display) {
    if (display.canvasRect.isValid() && !display.canvasRect.isEmpty()) {
        return QRectF(display.canvasRect);
    }
    return QRectF(display.physicalRect);
}

QRectF ScreenshotGeometryMapper::displayImageSourceCanvasRect(const CapturedDisplayModel& display) {
    if (display.imageSourceCanvasRect.isValid() && !display.imageSourceCanvasRect.isEmpty()) {
        return QRectF(display.imageSourceCanvasRect);
    }
    return displayCanvasRect(display);
}

ScreenshotDisplayViewportGeometry
ScreenshotGeometryMapper::displayViewportGeometry(const CapturedDisplayModel& display) {
    ScreenshotDisplayViewportGeometry geometry;
    geometry.canvasRect = displayCanvasRect(display);
    geometry.logicalRect = display.logicalRect;
    geometry.valid = !geometry.canvasRect.isEmpty() && geometry.logicalRect.isValid() &&
                     !geometry.logicalRect.isEmpty();
    if (!geometry.valid) {
        return geometry;
    }

    const DisplayCoordinateTransform transform(display);
    geometry.canvasToLogicalScale = transform.canvasToLogicalScale().width();
    // Center the logical viewport, not the rounded physical display extent.
    // This keeps the first captured pixel at local (0, 0) at fractional DPI.
    geometry.canvasCenter = transform.overlayLocalToCanvas(
        QPointF(geometry.logicalRect.width() / 2.0, geometry.logicalRect.height() / 2.0));
    return geometry;
}

ScreenshotDisplayPlacementGeometry
ScreenshotGeometryMapper::displayPlacementGeometry(const CapturedDisplayModel* display,
                                                   const QRect& fallbackLogicalBounds) {
    ScreenshotDisplayPlacementGeometry geometry;
    geometry.logicalBounds =
        display != nullptr && display->logicalRect.isValid() && !display->logicalRect.isEmpty()
            ? display->logicalRect
            : fallbackLogicalBounds;
    if (!geometry.logicalBounds.isValid() || geometry.logicalBounds.isEmpty()) {
        geometry.logicalBounds = QGuiApplication::primaryScreen() != nullptr
                                     ? QGuiApplication::primaryScreen()->geometry()
                                     : QRect(0, 0, 800, 600);
    }

    geometry.physicalBounds =
        display != nullptr && display->physicalRect.isValid() && !display->physicalRect.isEmpty()
            ? display->physicalRect
            : geometry.logicalBounds;
    geometry.screen = display != nullptr && display->screen != nullptr
                          ? display->screen.data()
                          : QGuiApplication::primaryScreen();
    geometry.valid = geometry.logicalBounds.isValid() && !geometry.logicalBounds.isEmpty() &&
                     geometry.physicalBounds.isValid() && !geometry.physicalBounds.isEmpty();
    return geometry;
}

CapturedDisplayModel ScreenshotGeometryMapper::preCaptureDisplayModel(QScreen& screen) {
    CapturedDisplayModel display;
    display.name = screen.name();
    display.logicalRect = screen.geometry();
    display.physicalRect = physicalRectForScreen(screen);
    display.canvasRect = display.physicalRect;
    display.screen = &screen;
    display.logicalToPhysicalScale = screen.devicePixelRatio();
    display.primary = &screen == QGuiApplication::primaryScreen();
    display.active = true;
#ifdef Q_OS_MACOS
    // Selection can finish before image acquisition. Use the same display identity
    // and point-based canvas as the captured frame from the start of the session.
    auto* native = screen.nativeInterface<QNativeInterface::QCocoaScreen>();
    if (native) {
        display.nativeDisplayId = [[[native->nativeScreen() deviceDescription]
            objectForKey:@"NSScreenNumber"] unsignedIntValue];
        display.stableId = QStringLiteral("display:%1").arg(display.nativeDisplayId);
        display.capturedLogicalRect = display.logicalRect;
        display.backingScale = screen.devicePixelRatio();
        display.canvasUsesPoints = true;
        display.canvasRect = display.logicalRect;
    }
#endif
    return display;
}

QRect ScreenshotGeometryMapper::physicalRectForScreen(const QScreen& screen) {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        // QScreen::name() can be a friendly monitor name, not a GDI device name.
        // Use the capture backend's native coordinate space; rounded logical
        // extents cannot reconstruct physical pixels at fractional DPI.
        return snow_shot::platform::windows::nativeMonitorRect(screen);
    }
#endif
    const QRect logicalGeometry = screen.geometry();
    const qreal devicePixelRatio = screen.devicePixelRatio();
    if (devicePixelRatio <= 0.0) {
        return logicalGeometry;
    }

    return QRect(logicalGeometry.left(), logicalGeometry.top(),
                 qRound(static_cast<qreal>(logicalGeometry.width()) * devicePixelRatio),
                 qRound(static_cast<qreal>(logicalGeometry.height()) * devicePixelRatio));
}

QRectF ScreenshotGeometryMapper::logicalRectFForPhysicalRect(const QRect& rect,
                                                             const QScreen* screen) {
    if (!rect.isValid() || rect.isEmpty()) {
        return {};
    }

    if (screen != nullptr) {
        const QRect logicalBounds = screen->geometry();
        const QRect physicalBounds = physicalRectForScreen(*screen);
        if (logicalBounds.isValid() && !logicalBounds.isEmpty() && physicalBounds.isValid() &&
            !physicalBounds.isEmpty()) {
            const qreal dpr = screen->devicePixelRatio();
            const QPointF topLeft =
                QPointF(logicalBounds.topLeft()) +
                (QPointF(rect.topLeft()) - QPointF(physicalBounds.topLeft())) / dpr;
            return QRectF(topLeft, QSizeF(rect.size()) / dpr);
        }
    }

    const qreal fallbackScale = qApp != nullptr ? qApp->devicePixelRatio() : 1.0;
    if (fallbackScale <= 0.0) {
        return rect;
    }
    return QRectF(rect.left(), rect.top(), static_cast<qreal>(rect.width()) / fallbackScale,
                  static_cast<qreal>(rect.height()) / fallbackScale);
}

QRect ScreenshotGeometryMapper::logicalRectForPhysicalRect(const QRect& rect,
                                                           const QScreen* screen) {
    return ScreenshotHalfOpenRect::fromRectF(logicalRectFForPhysicalRect(rect, screen))
        .toAlignedQRect();
}

QRect ScreenshotGeometryMapper::nativeRectForLogicalRect(const QRect& logicalRect,
                                                         const QRect& ownerLogicalBounds,
                                                         const QRect& ownerPhysicalBounds) {
    if (!logicalRect.isValid() || logicalRect.isEmpty()) {
        return {};
    }

    if (!ownerLogicalBounds.isValid() || ownerLogicalBounds.isEmpty() ||
        !ownerPhysicalBounds.isValid() || ownerPhysicalBounds.isEmpty()) {
        return logicalRect;
    }

    const QPointF rectCenter = ScreenshotHalfOpenRect::fromRect(logicalRect).center();
    QRect sourceLogicalBounds = ownerLogicalBounds;
    QRect targetPhysicalBounds = ownerPhysicalBounds;
    if (!ScreenshotHalfOpenRect::fromRect(ownerLogicalBounds).contains(rectCenter)) {
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen == nullptr) {
                continue;
            }
            const QRect screenLogicalBounds = screen->geometry();
            if (ScreenshotHalfOpenRect::fromRect(screenLogicalBounds).contains(rectCenter)) {
                sourceLogicalBounds = screenLogicalBounds;
                targetPhysicalBounds = physicalRectForScreen(*screen);
                break;
            }
        }
    }

    const double scaleX = scaleOrFallbackValue(static_cast<double>(targetPhysicalBounds.width()),
                                               static_cast<double>(sourceLogicalBounds.width()));
    const double scaleY = scaleOrFallbackValue(static_cast<double>(targetPhysicalBounds.height()),
                                               static_cast<double>(sourceLogicalBounds.height()));
    const auto mapStart = [](int value, int logicalStart, int physicalStart, double scale) {
        return static_cast<int>(
            std::floor(static_cast<double>(physicalStart) +
                       (static_cast<double>(value) - static_cast<double>(logicalStart)) * scale));
    };
    const auto mapEnd = [](int value, int logicalStart, int physicalStart, double scale) {
        const double mapped =
            static_cast<double>(physicalStart) +
            (static_cast<double>(value) - static_cast<double>(logicalStart)) * scale;
        if (mapped >= static_cast<double>(std::numeric_limits<int>::max())) {
            return std::numeric_limits<int>::max();
        }
        return static_cast<int>(std::ceil(mapped));
    };

    const int left = mapStart(logicalRect.left(), sourceLogicalBounds.left(),
                              targetPhysicalBounds.left(), scaleX);
    const int top =
        mapStart(logicalRect.top(), sourceLogicalBounds.top(), targetPhysicalBounds.top(), scaleY);
    const int right = mapEnd(logicalRect.left() + logicalRect.width(), sourceLogicalBounds.left(),
                             targetPhysicalBounds.left(), scaleX);
    const int bottom = mapEnd(logicalRect.top() + logicalRect.height(), sourceLogicalBounds.top(),
                              targetPhysicalBounds.top(), scaleY);

    return QRect(left, top, std::max(1, right - left), std::max(1, bottom - top));
}

ScreenshotPinnedImageFit ScreenshotGeometryMapper::fitImageToAvailableGeometry(
    const QSize& initialWindowSize, const QRect& availableLogicalGeometry,
    const QRect& screenLogicalGeometry, const QRect& screenNativeGeometry, int logicalMargin) {
    ScreenshotPinnedImageFit fit;
    fit.initialWindowSize = initialWindowSize;
    if (!initialWindowSize.isValid() || initialWindowSize.isEmpty() ||
        !availableLogicalGeometry.isValid() || availableLogicalGeometry.isEmpty() ||
        !screenLogicalGeometry.isValid() || screenLogicalGeometry.isEmpty() ||
        !screenNativeGeometry.isValid() || screenNativeGeometry.isEmpty()) {
        return fit;
    }

    const int margin = std::max(0, logicalMargin);
    const QRect insetLogical = availableLogicalGeometry.adjusted(margin, margin, -margin, -margin);
    if (!insetLogical.isValid() || insetLogical.isEmpty()) {
        return fit;
    }
    const QRect insetNative =
        nativeRectForLogicalRect(insetLogical, screenLogicalGeometry, screenNativeGeometry);
    if (!insetNative.isValid() || insetNative.isEmpty()) {
        return fit;
    }

    const double scale = std::max(
        kMinimumZoom,
        std::min({1.0, static_cast<double>(insetNative.width()) / initialWindowSize.width(),
                  static_cast<double>(insetNative.height()) / initialWindowSize.height()}));
    if (!(scale > 0.0)) {
        return fit;
    }
    const QSize fittedSize(std::max(1, qRound(initialWindowSize.width() * scale)),
                           std::max(1, qRound(initialWindowSize.height() * scale)));
    fit.nativeGeometry =
        QRect(QPoint(insetNative.left() + (insetNative.width() - fittedSize.width()) / 2,
                     insetNative.top() + (insetNative.height() - fittedSize.height()) / 2),
              fittedSize);
    fit.scalePercent = scale * 100.0;
    fit.valid = fit.nativeGeometry.isValid() && !fit.nativeGeometry.isEmpty();
    return fit;
}

ScreenshotPinnedImageFit ScreenshotGeometryMapper::centerImageAtFullResolution(
    const QSize& initialWindowSize, const QRect& availableLogicalGeometry,
    const QRect& screenLogicalGeometry, const QRect& screenNativeGeometry) {
    ScreenshotPinnedImageFit placement;
    placement.initialWindowSize = initialWindowSize;
    if (!initialWindowSize.isValid() || initialWindowSize.isEmpty() ||
        !availableLogicalGeometry.isValid() || availableLogicalGeometry.isEmpty() ||
        !screenLogicalGeometry.isValid() || screenLogicalGeometry.isEmpty() ||
        !screenNativeGeometry.isValid() || screenNativeGeometry.isEmpty()) {
        return placement;
    }

    const QRect availableNative = nativeRectForLogicalRect(
        availableLogicalGeometry, screenLogicalGeometry, screenNativeGeometry);
    if (!availableNative.isValid() || availableNative.isEmpty()) {
        return placement;
    }
    const QPoint topLeft(qRound(availableNative.left() +
                                (availableNative.width() - initialWindowSize.width()) / 2.0),
                         qRound(availableNative.top() +
                                (availableNative.height() - initialWindowSize.height()) / 2.0));
    placement.nativeGeometry = QRect(topLeft, initialWindowSize);
    placement.scalePercent = 100.0;
    placement.valid = true;
    return placement;
}

QPoint ScreenshotGeometryMapper::clampContentPositionToRect(const QPoint& desiredPosition,
                                                            const QRect& contentRect,
                                                            const QRect& bounds) {
    if (contentRect.isEmpty() || !bounds.isValid()) {
        return desiredPosition;
    }

    const int minX = bounds.left() - contentRect.left();
    const int minY = bounds.top() - contentRect.top();
    const int maxX = std::max(minX, bounds.right() - contentRect.right());
    const int maxY = std::max(minY, bounds.bottom() - contentRect.bottom());
    return QPoint(std::clamp(desiredPosition.x(), minX, maxX),
                  std::clamp(desiredPosition.y(), minY, maxY));
}

QPoint ScreenshotGeometryMapper::cursorPanelPosition(const QPoint& cursorPosition,
                                                     const QSize& panelSize, const QRect& bounds,
                                                     int gap) {
    const int effectiveGap = std::max(0, gap);
    const QPoint bottomRightPosition = cursorPosition + QPoint(effectiveGap, effectiveGap);
    if (panelSize.isEmpty() || !bounds.isValid()) {
        return bottomRightPosition;
    }

    const int boundsRight = bounds.left() + bounds.width();
    const int boundsBottom = bounds.top() + bounds.height();
    const bool useLeft = bottomRightPosition.x() + panelSize.width() > boundsRight;
    const bool useTop = bottomRightPosition.y() + panelSize.height() > boundsBottom;
    const QPoint desiredPosition(
        useLeft ? cursorPosition.x() - effectiveGap - panelSize.width() : bottomRightPosition.x(),
        useTop ? cursorPosition.y() - effectiveGap - panelSize.height() : bottomRightPosition.y());

    return clampContentPositionToRect(desiredPosition, QRect(QPoint(), panelSize), bounds);
}

QPoint ScreenshotGeometryMapper::selectionToolbarContentPosition(const QRectF& selectionLogical,
                                                                 const QSize& toolbarSize,
                                                                 const QRect& bounds, int gap) {
    const int effectiveGap = std::max(0, gap);
    const int left = qRound(selectionLogical.left());
    const int top = qRound(selectionLogical.top());
    const int right = qRound(selectionLogical.right());
    const int bottom = qRound(selectionLogical.bottom());
    const QRect content(QPoint(0, 0), toolbarSize);
    const QPoint topLeft(left, top - toolbarSize.height() - effectiveGap);
    const QPoint candidates[] = {
        topLeft,
        QPoint(right + effectiveGap, top),
        QPoint(left - toolbarSize.width() - effectiveGap, top),
        QPoint(left, bottom + effectiveGap),
    };
    const auto fullyVisible = [&](const QPoint& position) {
        if (content.isEmpty() || !bounds.isValid() || bounds.isEmpty()) {
            return false;
        }
        const QRect translated = content.translated(position);
        return translated.intersected(bounds) == translated;
    };
    for (const QPoint& candidate : candidates) {
        if (fullyVisible(candidate)) {
            return candidate;
        }
    }
    return clampContentPositionToRect(topLeft, content, bounds);
}

ScreenshotAnchoredToolbarPlacement ScreenshotGeometryMapper::anchoredToolbarPlacement(
    const QPoint& bottomRightAnchor, const QPoint& topRightAnchor,
    const ScreenshotToolbarPlacementGeometry& bottomPlacement,
    const ScreenshotToolbarPlacementGeometry& topPlacement, const QRect& bounds, int gap) {
    const bool bottomMainValid = !bottomPlacement.mainToolbarContentRect.isEmpty();
    const bool topMainValid = !topPlacement.mainToolbarContentRect.isEmpty();
    if (!bottomMainValid && !topMainValid) {
        return {};
    }

    const int effectiveGap = std::max(0, gap);
    const auto contentPositionForAnchor = [effectiveGap](const QPoint& anchor,
                                                         const QRect& mainRect, bool aboveAnchor) {
        return QPoint(anchor.x() - mainRect.right(),
                      aboveAnchor ? anchor.y() - mainRect.bottom() - effectiveGap - 1
                                  : anchor.y() + effectiveGap + 1 - mainRect.top());
    };
    const auto fullyVisible = [&bounds](const QRect& occupiedRect, const QPoint& position) {
        if (occupiedRect.isEmpty() || !bounds.isValid() || bounds.isEmpty()) {
            return false;
        }
        const QRect translated = occupiedRect.translated(position);
        return translated.intersected(bounds) == translated;
    };

    const auto occupiedRectFor = [](const ScreenshotToolbarPlacementGeometry& placement) {
        return placement.occupiedContentRect.isEmpty() ? placement.mainToolbarContentRect
                                                       : placement.occupiedContentRect;
    };

    QPoint bottomPosition;
    QRect bottomOccupied;
    bool bottomFits = false;
    if (bottomMainValid) {
        bottomPosition = contentPositionForAnchor(bottomRightAnchor,
                                                  bottomPlacement.mainToolbarContentRect, false);
        bottomOccupied = occupiedRectFor(bottomPlacement);
        bottomFits = fullyVisible(bottomOccupied, bottomPosition);
    }
    if (bottomFits) {
        return ScreenshotAnchoredToolbarPlacement{
            clampContentPositionToRect(bottomPosition, bottomOccupied, bounds), false};
    }

    QPoint topPosition;
    QRect topOccupied;
    bool topFits = false;
    if (topMainValid) {
        topPosition =
            contentPositionForAnchor(topRightAnchor, topPlacement.mainToolbarContentRect, true);
        topOccupied = occupiedRectFor(topPlacement);
        topFits = fullyVisible(topOccupied, topPosition);
    }
    if (topFits) {
        return ScreenshotAnchoredToolbarPlacement{
            clampContentPositionToRect(topPosition, topOccupied, bounds), true};
    }

    // If neither candidate fits, retain the normal bottom-first preference and
    // clamp its occupied extent. This keeps an oversized toolbar deterministic
    // while still choosing the top candidate when the bottom geometry is absent.
    const bool useTopRightPlacement = !bottomMainValid;
    const QRect& selectedOccupied = useTopRightPlacement ? topOccupied : bottomOccupied;
    const QPoint& selectedPosition = useTopRightPlacement ? topPosition : bottomPosition;

    return ScreenshotAnchoredToolbarPlacement{
        clampContentPositionToRect(selectedPosition, selectedOccupied, bounds),
        useTopRightPlacement,
    };
}

QPointF ScreenshotGeometryMapper::logicalDragPositionForPhysicalPoint(
    const QPointF& globalLogicalPosition, const QPointF& physicalPosition,
    const QRect& ownerLogicalBounds, const QRect& ownerPhysicalBounds) {
    if (!ownerLogicalBounds.isValid() || ownerLogicalBounds.isEmpty() ||
        !ownerPhysicalBounds.isValid() || ownerPhysicalBounds.isEmpty() ||
        !ScreenshotHalfOpenRect::fromRect(ownerPhysicalBounds).contains(physicalPosition)) {
        return globalLogicalPosition;
    }

    return mapPointBetweenRects(physicalPosition, ownerPhysicalBounds, ownerLogicalBounds);
}

ScreenshotPinnedImageGeometry
ScreenshotGeometryMapper::pinnedImageGeometry(const QRect& nativeWindowGeometry,
                                              const QSize& imagePixelSize) {
    if (imagePixelSize.isEmpty() || !nativeWindowGeometry.isValid() ||
        nativeWindowGeometry.isEmpty()) {
        return {};
    }

    ScreenshotPinnedImageGeometry geometry;
    geometry.nativeGeometry = nativeWindowGeometry;
    geometry.canvasSourceRect = QRectF(QPointF(0.0, 0.0), QSizeF(imagePixelSize));
    geometry.initialWindowSize = nativeWindowGeometry.size();
    return geometry;
}

ScreenshotPinnedImagePlacement ScreenshotGeometryMapper::pinnedImagePlacement(
    const ScreenshotDisplaySession& displaySession, const QRect& canvasSelection,
    const QSize& imagePixelSize, int shadowPadding) const {
    ScreenshotPinnedImagePlacement placement;
    if (canvasSelection.width() < 1 || canvasSelection.height() < 1 || imagePixelSize.isEmpty()) {
        return placement;
    }

    const QPointF canvasTopLeft(static_cast<qreal>(canvasSelection.left()),
                                static_cast<qreal>(canvasSelection.top()));
    const CapturedDisplayModel* anchorDisplay =
        displayForCanvasPoint(displaySession, canvasTopLeft);
    if (anchorDisplay == nullptr) {
        anchorDisplay = displayForCanvasRect(displaySession, QRectF(canvasSelection));
    }
    if (anchorDisplay == nullptr) {
        return placement;
    }

    const QPoint nativeContentTopLeft = canvasSelection.topLeft() + m_canvasOrigin;
    const QRect nativeGeometry(nativeContentTopLeft - QPoint(shadowPadding, shadowPadding),
                               imagePixelSize);

    const CapturedDisplayModel* placementDisplay =
        anchorDisplay->canvasUsesPoints
            ? displayForCanvasPoint(displaySession,
                                    ScreenshotHalfOpenRect::fromRect(nativeGeometry).center() -
                                        QPointF(m_canvasOrigin))
            : displayForPhysicalPoint(displaySession,
                                      ScreenshotHalfOpenRect::fromRect(nativeGeometry).center());
    if (placementDisplay == nullptr) {
        placementDisplay = anchorDisplay;
    }

    placement.geometry = pinnedImageGeometry(nativeGeometry, imagePixelSize);
    placement.screen = placementDisplay->screen;
    placement.valid = !placement.geometry.nativeGeometry.isEmpty() &&
                      !placement.geometry.canvasSourceRect.isEmpty() &&
                      !placement.geometry.initialWindowSize.isEmpty();
    return placement;
}

QScreen* ScreenshotGeometryMapper::screenForCaptureDisplay(const QString& name,
                                                           const QRect& physicalRect) {
    if (!name.isEmpty()) {
        for (QScreen* screen : QGuiApplication::screens()) {
            if (screen != nullptr && screen->name().compare(name, Qt::CaseInsensitive) == 0) {
                return screen;
            }
        }
    }
    return screenForPhysicalRect(physicalRect);
}

QScreen* ScreenshotGeometryMapper::screenForPhysicalRect(const QRect& rect) {
    const QPointF center = ScreenshotHalfOpenRect::fromRect(rect).center();
    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != nullptr &&
            ScreenshotHalfOpenRect::fromRect(physicalRectForScreen(*screen)).contains(center)) {
            return screen;
        }
    }
    return QGuiApplication::primaryScreen();
}

ScreenshotSelectionDisplayConversion
screenshotSelectionDisplayConversion(const ScreenshotGeometryMapper& geometry,
                                     const ScreenshotDisplaySession& displays,
                                     const QRect& selection, ScreenshotSelectionDisplayUnit unit,
                                     const CapturedDisplayModel* fallbackDisplay) {
    ScreenshotSelectionDisplayConversion result;
    const auto* owner =
        selection.isEmpty() ? fallbackDisplay : geometry.displayForCanvasRect(displays, selection);
    if (!owner)
        owner = fallbackDisplay;
    result.canvasUsesPoints = owner && owner->canvasUsesPoints;
    result.selection.unit = unit;
    result.selection.canvasUsesPoints = result.canvasUsesPoints;
    if (result.canvasUsesPoints && unit == ScreenshotSelectionDisplayUnit::PhysicalPixels) {
        const auto spec = screenshotSelectionRenderSpec(displays, selection);
        result.scale = spec.isValid() ? spec.scale : owner->backingScale;
        if (!std::isfinite(result.scale) || result.scale <= 0.0)
            result.scale = 1.0;
        result.selection.size =
            spec.isValid() ? spec.pixelSize
                           : screenshotSelectionRenderedPixelSize(selection.size(), result.scale);
    } else {
        if (owner && !result.canvasUsesPoints &&
            unit == ScreenshotSelectionDisplayUnit::LogicalPixels)
            result.scale = ScreenshotGeometryMapper::canvasToLogicalScale(*owner);
        if (!std::isfinite(result.scale) || result.scale <= 0.0)
            result.scale = 1.0;
        result.selection.size = QSizeF(selection.size()) * result.scale;
    }
    result.selection.position = QPointF(selection.topLeft()) * result.scale;
    return result;
}

QPointF screenshotMagnifierDisplayPosition(const ScreenshotGeometryMapper& geometry,
                                           const CapturedDisplayModel& sampleDisplay,
                                           const QPoint& physicalPoint,
                                           const ScreenshotSelectionDisplayConversion& conversion) {
    const QPointF desktopPoint =
        sampleDisplay.canvasUsesPoints
            ? geometry.logicalPositionForPhysicalPoint(sampleDisplay, physicalPoint)
            : QPointF(physicalPoint);
    return desktopPoint * conversion.scale;
}
