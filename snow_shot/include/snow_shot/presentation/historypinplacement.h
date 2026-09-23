#ifndef SNOW_SHOT_PRESENTATION_HISTORYPINPLACEMENT_H
#define SNOW_SHOT_PRESENTATION_HISTORYPINPLACEMENT_H

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotselectionpin.h"
#include "snow_shot/storage/capturehistorytypes.h"

#include <functional>

class QScreen;

namespace snow_shot::presentation {

[[nodiscard]] std::optional<snow_shot::storage::PinnedBorderAppearance>
historySelectionBorderAppearance(const snow_shot::storage::CaptureHistoryRecord& record);

// Native monitor rectangle for one screen in the coordinate space the capture backend reports.
// Injected so tests can supply rectangles without native screens.
using HistoryPinNativeMonitorRect = std::function<QRect(const QScreen&)>;

// Whether the record can map onto a desktop at all: a screenshot session captured without
// scrolling, with a recorded desktop origin, result and usable selection. Desktop-independent, so
// callers can skip building a display session for ineligible records.
[[nodiscard]] bool
historyRecordSupportsSelectionPin(const snow_shot::storage::CaptureHistoryRecord& record);

// The live selection-pin request for a non-scrolling screenshot whose selection meets the
// supplied desktop after translating from the recorded desktop origin. Unprepared for scrolling
// captures, records without placement metadata, incompatible coordinate spaces, imported images,
// and selections that miss every display. displays must already be in capture coordinates;
// the caller rebuilds geometry. Off-desktop selections do
// not use the live pin's nearest-display fallback.
[[nodiscard]] ScreenshotPinnedSelectionRequest
historySelectionPinPlacement(const snow_shot::storage::CaptureHistoryRecord& record,
                             const ScreenshotDisplaySession& displays,
                             const ScreenshotGeometryMapper& geometry);

// Resolve against the current desktop immediately before presentation, including after image I/O.
[[nodiscard]] ScreenshotPinnedSelectionRequest
historySelectionPinPlacement(const snow_shot::storage::CaptureHistoryRecord& record);

// The history-pin desktop for screens. Windows replaces each screen's pre-capture rectangle
// with nativeMonitorRect and fails the whole session when one is unavailable: capture canvas
// coordinates are the physical monitor rectangle, so a missing monitor cannot fall back to
// Qt's rectangle without moving the pin. macOS keeps the point-based pre-capture model, where
// the resolver is unused.
[[nodiscard]] ScreenshotDisplaySession
historyPinDisplaySession(const QList<QScreen*>& screens,
                         const HistoryPinNativeMonitorRect& nativeMonitorRect);

// Current desktop in capture coordinates. Empty when that coordinate space cannot be
// built. Does not touch a live screenshot session.
[[nodiscard]] ScreenshotDisplaySession currentHistoryPinDisplaySession();
} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_HISTORYPINPLACEMENT_H
