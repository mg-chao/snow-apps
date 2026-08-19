#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYPOOL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYPOOL_H

#include "snow_shot/presentation/screenshottypes.h"

#include <QObjectCleanupHandler>
#include <QVector>

#include <functional>

class ScreenshotDisplaySession;
class ScreenshotOverlayEventSink;
class ScreenshotOverlayWindow;
class SnowCanvasRuntime;
namespace snow_shot::presentation {
class WindowShortcutManager;
}

struct ScreenshotOverlayPoolCallbacks {
    std::function<void(ScreenshotOverlayWindow*)> detachOverlayUi;
    std::function<void(ScreenshotOverlayWindow*)> clearOverlayCanvas;
};

class ScreenshotOverlayPool final {
  public:
    ScreenshotOverlayPool(ScreenshotOverlayEventSink& eventSink, SnowCanvasRuntime& canvasRuntime,
                          snow_shot::presentation::WindowShortcutManager& shortcutManager,
                          ScreenshotOverlayPoolCallbacks callbacks);

    void prewarmDisplayPool(ScreenshotDisplaySession& displaySession, int displayCount);
    void clearOverlayCanvases(const ScreenshotDisplaySession& displaySession) const;
    void clearDisplays(ScreenshotDisplaySession& displaySession) const;
    void destroyDisplayPool(ScreenshotDisplaySession& displaySession) const;
    void resetForNewCapture(ScreenshotDisplaySession& displaySession) const;
    [[nodiscard]] ScreenshotOverlayWindow* ensureOverlay(ScreenshotOverlayWindow* overlay) const;

  private:
    void clearOverlayCanvas(ScreenshotOverlayWindow* overlay) const;
    void detachOverlayUi(ScreenshotOverlayWindow* overlay) const;
    void deleteOverlay(ScreenshotOverlayWindow* overlay) const;

    ScreenshotOverlayEventSink& m_eventSink;
    SnowCanvasRuntime& m_canvasRuntime;
    snow_shot::presentation::WindowShortcutManager& m_shortcutManager;
    ScreenshotOverlayPoolCallbacks m_callbacks;
    mutable QObjectCleanupHandler m_retiredOverlays;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOVERLAYPOOL_H
