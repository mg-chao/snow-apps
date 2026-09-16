#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOWPORTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOWPORTS_H

#include <QPoint>
#include <QRectF>
#include <QVector>

#include <cstdint>

class ScreenshotDisplaySession;

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
    TraversalLimit
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
    QVector<QRectF> rects;
};

class ScreenshotSelectorServicePort {
  public:
    virtual ~ScreenshotSelectorServicePort() = default;

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
