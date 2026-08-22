#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYEVENTSINK_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYEVENTSINK_H

#include <QPoint>
#include <QPointF>
#include <Qt>

class ScreenshotOverlayWindow;

class ScreenshotOverlayEventSink {
  public:
    virtual ~ScreenshotOverlayEventSink() = default;

    [[nodiscard]] virtual bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow* overlay,
                                                             const QPointF& localPosition,
                                                             bool leftButtonActive) const = 0;
    virtual void handleOverlayMousePress(ScreenshotOverlayWindow* overlay,
                                         const QPointF& localPosition) = 0;
    virtual void handleOverlayMouseMove(ScreenshotOverlayWindow* overlay,
                                        const QPointF& localPosition) = 0;
    virtual void handleOverlayMouseRelease(ScreenshotOverlayWindow* overlay,
                                           const QPointF& localPosition) = 0;
    [[nodiscard]] virtual bool handleOverlayRightClick(ScreenshotOverlayWindow* overlay,
                                                       const QPointF& localPosition) = 0;
    // Optional completion-gesture notifications. Keeping these callbacks
    // non-pure preserves source compatibility for lightweight event sinks
    // that only care about the original overlay event surface.
    virtual void handleUnhandledLeftDoubleClick() {}
    virtual void handleUnhandledMiddleClick() {}
    [[nodiscard]] virtual bool handleOverlayWheel(ScreenshotOverlayWindow* overlay,
                                                  const QPointF& localPosition,
                                                  const QPoint& angleDelta,
                                                  const QPoint& pixelDelta) = 0;
    [[nodiscard]] virtual bool shouldBlockUnhandledOverlayKeyInput() const = 0;
    virtual void raiseToolbarForCanvasInteraction() = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYEVENTSINK_H
