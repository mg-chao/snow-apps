#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOWPORTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOWPORTS_H

#include <QPoint>
#include <QRectF>
#include <QVector>

#include <cstdint>

class ScreenshotDisplaySession;
struct CapturedDisplayModel;

enum class ScreenshotSelectorHitTestMode {
    Window,
    WindowSubElement,
};

enum class ScreenshotSelectorResultPhase { Initial, Refinement, Finished };
enum class ScreenshotSelectorStopReason {
    Complete,
    BudgetExhausted,
    DecodingPending,
    ProviderTimeout,
    ProviderFailure,
    Cancelled,
    TraversalLimit,
    PermissionRequired,
    AccessibilityPending
};
struct ScreenshotSelectorResult {
    quint64 epoch = 0;
    quint64 requestId = 0;
    quint64 generation = 0;
    QPoint point;
    ScreenshotSelectorHitTestMode mode = ScreenshotSelectorHitTestMode::Window;
    ScreenshotSelectorResultPhase phase = ScreenshotSelectorResultPhase::Initial;
    ScreenshotSelectorStopReason stopReason = ScreenshotSelectorStopReason::Complete;
    bool ok = false;
    bool canRefine = false;
    quint64 elapsedUs = 0;
    quint32 displayId = 0;
    QVector<QRectF> rects;
};

class ScreenshotSelectorServicePort {
  public:
    virtual ~ScreenshotSelectorServicePort() = default;

    // Layout refresh never enumerates. Call startRefresh for a system enumeration.
    [[nodiscard]] virtual bool
    startRefreshWithDisplays(const QVector<std::uintptr_t>& excluded,
                             const QVector<CapturedDisplayModel>& displays) {
        Q_UNUSED(excluded);
        Q_UNUSED(displays);
        return false;
    }
    [[nodiscard]] virtual bool requestHitTestOnDisplay(const QPoint& point,
                                                       ScreenshotSelectorHitTestMode mode,
                                                       quint32 displayId) {
        if (displayId != 0)
            return false;
        return requestHitTest(point, mode);
    }
    [[nodiscard]] virtual bool ready() const = 0;
    [[nodiscard]] virtual bool refreshInFlight() const = 0;
    [[nodiscard]] virtual bool startRefresh(const QVector<std::uintptr_t>& excludedHwnds) = 0;
    [[nodiscard]] virtual bool requestHitTest(const QPoint& physicalPoint,
                                              ScreenshotSelectorHitTestMode mode) = 0;
};

class ScreenshotOverlayExclusionPort {
  public:
    virtual ~ScreenshotOverlayExclusionPort() = default;

    [[nodiscard]] virtual QVector<std::uintptr_t>
    excludedHwnds(const ScreenshotDisplaySession& displaySession) const = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOWPORTS_H
