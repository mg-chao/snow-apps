#include "snow_shot/presentation/historypinplacement.h"

#include "snow_shot/platform/windows/monitorgeometry.h"

#include <QGuiApplication>
#include <QScreen>

#include <limits>

namespace snow_shot::presentation {
namespace {
bool selectionMeetsDesktop(const ScreenshotDisplaySession& displays, const QRect& selection,
                           bool canvasUsesPoints) {
    const ScreenshotHalfOpenRect target = ScreenshotHalfOpenRect::fromRect(selection);
    bool meets = false;
    bool compatible = true;
    displays.forEachActiveDisplay([&](qsizetype, const CapturedDisplayModel& display) {
        compatible = compatible && display.canvasUsesPoints == canvasUsesPoints;
        meets = meets || target.intersects(ScreenshotHalfOpenRect::fromRect(display.canvasRect));
    });
    return compatible && meets;
}

#if defined(Q_OS_WIN)
// Pre-capture geometry keeps Qt's logical origin for the overlay. Capture canvas coordinates
// are the physical monitor rectangle, so a missing monitor cannot fall back to that Qt rect
// without moving the pin.
bool applyNativeMonitorRect(CapturedDisplayModel& display, const QScreen& screen,
                            const HistoryPinNativeMonitorRect& nativeMonitorRect) {
    const QRect monitor = nativeMonitorRect(screen);
    if (monitor.isEmpty()) {
        return false;
    }
    display.physicalRect = monitor;
    return true;
}
#endif
} // namespace

std::optional<snow_shot::storage::PinnedBorderAppearance>
historySelectionBorderAppearance(const snow_shot::storage::CaptureHistoryRecord& record) {
    if (record.contentKind != snow_shot::storage::CaptureHistoryContentKind::ScreenshotSession ||
        !record.result || record.result->imageSize.isEmpty()) {
        return {};
    }
    const auto style = ScreenshotResultCompositor::normalizedStyle({record.selection.cornerRadius,
                                                                    record.selection.shadowWidth,
                                                                    record.selection.shadowColor});
    if (!record.selection.rectangle.size().isEmpty() && record.scrolling != true) {
        return screenshotSelectionBorderAppearance(record.selection.rectangle.size(), style);
    }
    // Scrolling changes the content height, but retains the selection width.
    const qreal scale = record.selection.rectangle.width() > 0
                            ? qreal(record.result->imageSize.width()) /
                                  (record.selection.rectangle.width() + 2 * style.shadowWidth)
                            : 1.0;
    const qreal padding = style.shadowWidth * scale;
    const QRectF content = QRectF(QPointF(), QSizeF(record.result->imageSize))
                               .adjusted(padding, padding, -padding, -padding);
    if (!content.isValid()) {
        return {};
    }
    return snow_shot::storage::PinnedBorderAppearance{
        record.result->imageSize, content, style.cornerRadius * scale, style.shadowWidth > 0};
}

bool historyRecordSupportsSelectionPin(const snow_shot::storage::CaptureHistoryRecord& record) {
    // Legacy canvas coordinates alone cannot recover an absolute desktop position.
    return record.contentKind == snow_shot::storage::CaptureHistoryContentKind::ScreenshotSession &&
           record.result.has_value() && record.scrolling == false && record.desktopGeometry &&
           record.selection.rectangle.width() >= 1 && record.selection.rectangle.height() >= 1;
}

ScreenshotPinnedSelectionRequest
historySelectionPinPlacement(const snow_shot::storage::CaptureHistoryRecord& record,
                             const ScreenshotDisplaySession& displays,
                             const ScreenshotGeometryMapper& geometry) {
    if (!historyRecordSupportsSelectionPin(record)) {
        return {};
    }
    const auto& desktop = *record.desktopGeometry;
    const QRect saved = record.selection.rectangle;
    // Translate via desktop coordinates, using wide arithmetic for untrusted persisted data.
    const qint64 x = qint64(saved.x()) + desktop.canvasOrigin.x() - geometry.canvasOrigin().x();
    const qint64 y = qint64(saved.y()) + desktop.canvasOrigin.y() - geometry.canvasOrigin().y();
    if (x < std::numeric_limits<int>::min() || y < std::numeric_limits<int>::min() ||
        x > qint64(std::numeric_limits<int>::max()) - saved.width() ||
        y > qint64(std::numeric_limits<int>::max()) - saved.height()) {
        return {};
    }
    const QRect selection(QPoint(static_cast<int>(x), static_cast<int>(y)), saved.size());
    if (!selectionMeetsDesktop(displays, selection, desktop.canvasUsesPoints)) {
        return {};
    }
    const ScreenshotResultStyle style{record.selection.cornerRadius, record.selection.shadowWidth,
                                      record.selection.shadowColor};
    return screenshotSelectionPinRequest(displays, geometry, selection, style);
}

ScreenshotPinnedSelectionRequest
historySelectionPinPlacement(const snow_shot::storage::CaptureHistoryRecord& record) {
    if (!historyRecordSupportsSelectionPin(record)) {
        return {};
    }
    auto displays = currentHistoryPinDisplaySession();
    ScreenshotGeometryMapper geometry;
    geometry.rebuild(displays);
    return historySelectionPinPlacement(record, displays, geometry);
}

ScreenshotDisplaySession
historyPinDisplaySession(const QList<QScreen*>& screens,
                         const HistoryPinNativeMonitorRect& nativeMonitorRect) {
    ScreenshotDisplaySession session;
    session.reserve(screens.size());
    for (QScreen* screen : screens) {
        if (screen == nullptr) {
            continue;
        }
        CapturedDisplayModel display = ScreenshotGeometryMapper::preCaptureDisplayModel(*screen);
#if defined(Q_OS_WIN)
        if (!applyNativeMonitorRect(display, *screen, nativeMonitorRect)) {
            return {};
        }
#else
        static_cast<void>(nativeMonitorRect);
#endif
        session.appendDisplay(std::move(display));
    }
    return session;
}

ScreenshotDisplaySession currentHistoryPinDisplaySession() {
    return historyPinDisplaySession(QGuiApplication::screens(), [](const QScreen& screen) {
        return snow_shot::platform::windows::nativeMonitorRect(screen);
    });
}
} // namespace snow_shot::presentation
