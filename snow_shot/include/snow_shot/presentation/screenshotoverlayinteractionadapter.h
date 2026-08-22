#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYINTERACTIONADAPTER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYINTERACTIONADAPTER_H

#include "snow_shot/presentation/screenshotoverlayeventsink.h"

#include <QPoint>
#include <QPointF>
#include <Qt>

#include <functional>

class ScreenshotOverlayInputHandler;
class ScreenshotOverlayWindow;

class ScreenshotOverlayEventAdapter final : public ScreenshotOverlayEventSink {
  public:
    void setEventTargets(ScreenshotOverlayInputHandler& inputHandler,
                         std::function<void()> raiseToolbarForCanvasInteraction);
    void clearEventTargets();

    [[nodiscard]] bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow* overlay,
                                                     const QPointF& localPosition,
                                                     bool leftButtonActive) const override;
    void handleOverlayMousePress(ScreenshotOverlayWindow* overlay,
                                 const QPointF& localPosition) override;
    void handleOverlayMouseMove(ScreenshotOverlayWindow* overlay,
                                const QPointF& localPosition) override;
    void handleOverlayMouseRelease(ScreenshotOverlayWindow* overlay,
                                   const QPointF& localPosition) override;
    [[nodiscard]] bool handleOverlayRightClick(ScreenshotOverlayWindow* overlay,
                                               const QPointF& localPosition) override;
    void handleUnhandledLeftDoubleClick() override;
    void handleUnhandledMiddleClick() override;
    [[nodiscard]] bool handleOverlayWheel(ScreenshotOverlayWindow* overlay,
                                          const QPointF& localPosition, const QPoint& angleDelta,
                                          const QPoint& pixelDelta) override;
    [[nodiscard]] bool shouldBlockUnhandledOverlayKeyInput() const override;
    void raiseToolbarForCanvasInteraction() override;

  private:
    ScreenshotOverlayInputHandler* m_inputHandler = nullptr;
    std::function<void()> m_raiseToolbarForCanvasInteraction;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYINTERACTIONADAPTER_H
