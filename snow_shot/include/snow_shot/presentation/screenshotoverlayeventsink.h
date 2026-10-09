#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYEVENTSINK_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYEVENTSINK_H

#include "snow_shot/image/screenshotregiongeometry.h"
#include <QPoint>
#include <QPointF>
#include <Qt>

class ScreenshotOverlayWindow;
class QWheelEvent;

enum class ScreenshotOverlayRightClickResult { Ignored, Handled, CancelCapture };

class ScreenshotOverlayEventSink {
  public:
    virtual ~ScreenshotOverlayEventSink() = default;
    virtual bool acceptOverlayInput(bool genuine) {
        Q_UNUSED(genuine);
        return true;
    }
    // Hosts may coalesce guide presentation even while the canvas owns input.
    virtual bool presentOverlayPointer(ScreenshotOverlayWindow*, const QPointF&) {
        return false;
    }

    [[nodiscard]] virtual bool shouldHandleOverlayMouseEvent(const ScreenshotOverlayWindow* overlay,
                                                             const QPointF& localPosition,
                                                             bool leftButtonActive) const = 0;
    virtual void handleOverlayMousePress(ScreenshotOverlayWindow* overlay,
                                         const QPointF& localPosition) = 0;
    virtual void handleOverlayMouseMove(ScreenshotOverlayWindow* overlay,
                                        const QPointF& localPosition) = 0;
    virtual void handleOverlayMouseRelease(ScreenshotOverlayWindow* overlay,
                                           const QPointF& localPosition) = 0;
    [[nodiscard]] virtual ScreenshotOverlayRightClickResult
    handleOverlayRightClick(ScreenshotOverlayWindow* overlay, const QPointF& localPosition) = 0;
    virtual void completeRightClickCancellation() {}
    virtual bool effectDragActive() const {
        return false;
    }
    virtual void leaveEffectEditors() {}
    virtual void cancelEffectDrag() {}
    virtual bool handleEffectDoubleClick(ScreenshotOverlayWindow*, const QPointF&) {
        return false;
    }
    // Optional completion-gesture notifications. Lightweight event sinks can
    // keep the defaults when they only handle the mouse and keyboard surface.
    virtual bool handleRegionDoubleClick(ScreenshotOverlayWindow*, const QPointF&) {
        return false;
    }
    virtual void handleUnhandledLeftDoubleClick() {}
    virtual void handleUnhandledMiddleClick() {}
    [[nodiscard]] virtual bool handleOverlayWheel(ScreenshotOverlayWindow* overlay,
                                                  const QWheelEvent& event) = 0;
    [[nodiscard]] virtual bool shouldBlockUnhandledOverlayKeyInput() const = 0;
    virtual void raiseToolbarForCanvasInteraction() = 0;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYEVENTSINK_H
