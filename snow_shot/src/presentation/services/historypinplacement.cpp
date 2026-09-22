#include "snow_shot/presentation/historypinplacement.h"

#include "snow_shot/platform/windows/monitorgeometry.h"

#include <QGuiApplication>
#include <QScreen>

namespace snow_shot::presentation {
namespace {
bool selectionMeetsDesktop(const ScreenshotDisplaySession& displays, const QRect& selection) {
    const ScreenshotHalfOpenRect target = ScreenshotHalfOpenRect::fromRect(selection);
    bool meets = false;
    displays.forEachActiveDisplay([&](qsizetype, const CapturedDisplayModel& display) {
        meets = meets || target.intersects(ScreenshotHalfOpenRect::fromRect(display.canvasRect));
    });
    return meets;
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

bool historyRecordSupportsSelectionPin(const snow_shot::storage::CaptureHistoryRecord& record) {
    // The scrolling marker only exists on records persisted after it was introduced; records
    // without it (scrolling or not) fall back to cursor-screen placement.
    return record.contentKind == snow_shot::storage::CaptureHistoryContentKind::ScreenshotSession &&
           record.result.has_value() && record.scrolling == false &&
           record.selection.rectangle.width() >= 1 && record.selection.rectangle.height() >= 1;
}

ScreenshotPinnedSelectionRequest
historySelectionPinPlacement(const snow_shot::storage::CaptureHistoryRecord& record,
                             const ScreenshotDisplaySession& displays,
                             const ScreenshotGeometryMapper& geometry) {
    if (!historyRecordSupportsSelectionPin(record) ||
        !selectionMeetsDesktop(displays, record.selection.rectangle)) {
        return {};
    }
    const QRect selection = record.selection.rectangle;
    const ScreenshotResultStyle style{record.selection.cornerRadius, record.selection.shadowWidth,
                                      record.selection.shadowColor};
    return screenshotSelectionPinRequest(displays, geometry, selection, style);
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
