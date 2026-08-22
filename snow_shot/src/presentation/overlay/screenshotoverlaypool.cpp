#include "snow_shot/presentation/screenshotoverlaypool.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_shot/presentation/screenshotcapturedisplaymodelreconciler.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotoverlayeventsink.h"
#include "snow_shot/presentation/screenshotoverlaywindow.h"
#include "snow_shot/presentation/windowshortcutmanager.h"

#include <algorithm>
#include <utility>

ScreenshotOverlayPool::ScreenshotOverlayPool(
    ScreenshotOverlayEventSink& eventSink, SnowCanvasRuntime& canvasRuntime,
    snow_shot::presentation::WindowShortcutManager& shortcutManager,
    ScreenshotOverlayPoolCallbacks callbacks)
    : m_eventSink(eventSink), m_canvasRuntime(canvasRuntime), m_shortcutManager(shortcutManager),
      m_callbacks(std::move(callbacks)) {}

void ScreenshotOverlayPool::prewarmDisplayPool(ScreenshotDisplaySession& displaySession,
                                               int displayCount) {
    if (displayCount <= 0) {
        return;
    }

    const qsizetype targetDisplayCount = static_cast<qsizetype>(displayCount);
    displaySession.removeInactiveDisplaysBeyond(
        targetDisplayCount, [this](ScreenshotOverlayWindow* overlay) { deleteOverlay(overlay); });

    displaySession.reserve(std::max(displaySession.size(), targetDisplayCount));
    while (displaySession.size() < targetDisplayCount) {
        displaySession.appendDisplay({}, ensureOverlay(nullptr));
    }
}

void ScreenshotOverlayPool::clearOverlayCanvases(
    const ScreenshotDisplaySession& displaySession) const {
    displaySession.forEachOverlay(
        [this](qsizetype, ScreenshotOverlayWindow* overlay) { clearOverlayCanvas(overlay); });
}

void ScreenshotOverlayPool::clearDisplays(ScreenshotDisplaySession& displaySession) const {
    displaySession.forEachMutableDisplayWithOverlay(
        [](qsizetype, CapturedDisplayModel& display, ScreenshotOverlayWindow*) {
            ScreenshotCaptureDisplayModelReconciler::clearCaptureMetadata(display);
        });
}

void ScreenshotOverlayPool::hibernateDisplayPool(ScreenshotDisplaySession& displaySession) const {
    clearDisplays(displaySession);
    displaySession.forEachOverlay([this](qsizetype, ScreenshotOverlayWindow* overlay) {
        if (overlay == nullptr) {
            return;
        }
        clearOverlayCanvas(overlay);
        overlay->resetScreenshotRendering();
        overlay->hibernateNativeSurface();
    });
}

void ScreenshotOverlayPool::destroyDisplayPool(ScreenshotDisplaySession& displaySession) const {
    displaySession.takeEachOverlay(
        [this](qsizetype, ScreenshotOverlayWindow* overlay) { deleteOverlay(overlay); });
    displaySession.forEachMutableDisplay([](qsizetype, CapturedDisplayModel& display) {
        display.stableId.clear();
        ScreenshotCaptureDisplayModelReconciler::clearCaptureMetadata(display);
    });
}

void ScreenshotOverlayPool::resetForNewCapture(ScreenshotDisplaySession& displaySession) const {
    displaySession.forEachMutableDisplayWithOverlay(
        [](qsizetype, CapturedDisplayModel& display, ScreenshotOverlayWindow* overlay) {
            display.image = QImage();
            if (overlay != nullptr) {
                overlay->resetScreenshotRendering();
                if (overlay->canvas() != nullptr) {
                    overlay->canvas()->setInteractionEnabled(false);
                }
            }
        });
}

ScreenshotOverlayWindow*
ScreenshotOverlayPool::ensureOverlay(ScreenshotOverlayWindow* overlay) const {
    const bool created = overlay == nullptr;
    if (created) {
        auto* canvas = new SnowCanvasWidget(m_canvasRuntime);
        overlay = new ScreenshotOverlayWindow(m_eventSink, canvas);
        m_shortcutManager.addScopeWindow(overlay);
    }

    overlay->restoreNativeSurface();
    if (created) {
        clearOverlayCanvas(overlay);
    }
    return overlay;
}

void ScreenshotOverlayPool::clearOverlayCanvas(ScreenshotOverlayWindow* overlay) const {
    if (m_callbacks.clearOverlayCanvas) {
        m_callbacks.clearOverlayCanvas(overlay);
    }
}

void ScreenshotOverlayPool::detachOverlayUi(ScreenshotOverlayWindow* overlay) const {
    if (m_callbacks.detachOverlayUi) {
        m_callbacks.detachOverlayUi(overlay);
    }
}

void ScreenshotOverlayPool::deleteOverlay(ScreenshotOverlayWindow* overlay) const {
    if (overlay == nullptr) {
        return;
    }

    detachOverlayUi(overlay);
    // deleteLater() deliberately keeps the QObject alive until the current
    // event dispatch is complete. Drop the renderer's QImage now so a frame
    // lease cannot keep a full desktop raster alive during that interval.
    clearOverlayCanvas(overlay);
    m_shortcutManager.removeScopeWindow(overlay);
    overlay->releaseNativeSurface();
    m_retiredOverlays.add(overlay);
    overlay->deleteLater();
}
