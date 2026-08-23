#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOWPORTS_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOWPORTS_H

#include <QPoint>
#include <QVector>

#include <cstdint>

class ScreenshotDisplaySession;

enum class ScreenshotSelectorHitTestMode {
    Window,
    WindowSubElement,
};

class ScreenshotSelectorServicePort {
  public:
    virtual ~ScreenshotSelectorServicePort() = default;

    [[nodiscard]] virtual bool ready() const = 0;
    [[nodiscard]] virtual bool refreshInFlight() const = 0;
    [[nodiscard]] virtual bool startRefresh(const QVector<std::uintptr_t>& excludedHwnds) = 0;
    [[nodiscard]] virtual bool requestHitTest(const QPoint& physicalPoint,
                                              ScreenshotSelectorHitTestMode mode) = 0;
    virtual void startNextHitTest() = 0;
};

class ScreenshotOverlayExclusionPort {
  public:
    virtual ~ScreenshotOverlayExclusionPort() = default;

    [[nodiscard]] virtual QVector<std::uintptr_t>
    excludedHwnds(const ScreenshotDisplaySession& displaySession) const = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTSELECTORWORKFLOWPORTS_H
