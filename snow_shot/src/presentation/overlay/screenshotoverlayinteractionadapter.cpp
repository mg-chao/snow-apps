#include "snow_shot/presentation/screenshotoverlayinteractionadapter.h"

#include "snow_shot/presentation/screenshotoverlayinputhandler.h"

#include <utility>

void ScreenshotOverlayEventAdapter::setEventTargets(
    ScreenshotOverlayInputHandler& inputHandler,
    std::function<void()> raiseToolbarForCanvasInteraction,
    std::function<void(ScreenshotOverlayWindow*, const QPointF&)> presentPointer) {
    m_inputHandler = &inputHandler;
    m_raiseToolbarForCanvasInteraction = std::move(raiseToolbarForCanvasInteraction);
    m_presentPointer = std::move(presentPointer);
}

bool ScreenshotOverlayEventAdapter::presentOverlayPointer(ScreenshotOverlayWindow* overlay,
                                                          const QPointF& position) {
    if (!m_presentPointer)
        return false;
    m_presentPointer(overlay, position);
    return true;
}

bool ScreenshotOverlayEventAdapter::acceptOverlayInput(bool genuine) {
    return m_inputHandler && m_inputHandler->acceptInput(genuine);
}

void ScreenshotOverlayEventAdapter::clearEventTargets() {
    m_inputHandler = nullptr;
    m_raiseToolbarForCanvasInteraction = nullptr;
    m_presentPointer = nullptr;
}

bool ScreenshotOverlayEventAdapter::shouldHandleOverlayMouseEvent(
    const ScreenshotOverlayWindow* overlay, const QPointF& localPosition,
    bool leftButtonActive) const {
    if (m_inputHandler == nullptr) {
        return false;
    }
    return m_inputHandler->shouldHandleMouseEvent(overlay, localPosition, leftButtonActive);
}

void ScreenshotOverlayEventAdapter::handleOverlayMousePress(ScreenshotOverlayWindow* overlay,
                                                            const QPointF& localPosition) {
    if (m_inputHandler != nullptr) {
        m_inputHandler->handleMousePress(overlay, localPosition);
    }
}

void ScreenshotOverlayEventAdapter::handleOverlayMouseMove(ScreenshotOverlayWindow* overlay,
                                                           const QPointF& localPosition) {
    if (m_inputHandler != nullptr) {
        m_inputHandler->handleMouseMove(overlay, localPosition);
    }
}

void ScreenshotOverlayEventAdapter::handleOverlayMouseRelease(ScreenshotOverlayWindow* overlay,
                                                              const QPointF& localPosition) {
    if (m_inputHandler != nullptr) {
        m_inputHandler->handleMouseRelease(overlay, localPosition);
    }
}

ScreenshotOverlayRightClickResult
ScreenshotOverlayEventAdapter::handleOverlayRightClick(ScreenshotOverlayWindow* overlay,
                                                       const QPointF& localPosition) {
    if (m_inputHandler == nullptr) {
        return ScreenshotOverlayRightClickResult::Ignored;
    }
    return m_inputHandler->handleRightClick(overlay, localPosition);
}

void ScreenshotOverlayEventAdapter::completeRightClickCancellation() {
    if (m_inputHandler) {
        m_inputHandler->completeRightClickCancellation();
    }
}

bool ScreenshotOverlayEventAdapter::effectDragActive() const {
    return m_inputHandler != nullptr && m_inputHandler->effectDragActive();
}

void ScreenshotOverlayEventAdapter::leaveEffectEditors() {
    if (m_inputHandler != nullptr)
        m_inputHandler->leaveEffectEditors();
}

void ScreenshotOverlayEventAdapter::cancelEffectDrag() {
    if (m_inputHandler != nullptr)
        static_cast<void>(m_inputHandler->cancelEffectDrag());
}

bool ScreenshotOverlayEventAdapter::handleEffectDoubleClick(ScreenshotOverlayWindow* overlay,
                                                            const QPointF& position) {
    return m_inputHandler != nullptr && m_inputHandler->handleEffectDoubleClick(overlay, position);
}

void ScreenshotOverlayEventAdapter::handleUnhandledLeftDoubleClick() {
    if (m_inputHandler != nullptr) {
        m_inputHandler->handleUnhandledLeftDoubleClick();
    }
}

void ScreenshotOverlayEventAdapter::handleUnhandledMiddleClick() {
    if (m_inputHandler != nullptr) {
        m_inputHandler->handleUnhandledMiddleClick();
    }
}

bool ScreenshotOverlayEventAdapter::handleOverlayWheel(ScreenshotOverlayWindow* overlay,
                                                       const QWheelEvent& event) {
    if (m_inputHandler == nullptr) {
        return false;
    }
    return m_inputHandler->handleWheel(overlay, event);
}

bool ScreenshotOverlayEventAdapter::shouldBlockUnhandledOverlayKeyInput() const {
    return m_inputHandler != nullptr && m_inputHandler->shouldBlockUnhandledKeyInput();
}

void ScreenshotOverlayEventAdapter::raiseToolbarForCanvasInteraction() {
    if (m_raiseToolbarForCanvasInteraction) {
        m_raiseToolbarForCanvasInteraction();
    }
}

bool ScreenshotOverlayEventAdapter::handleRegionDoubleClick(ScreenshotOverlayWindow* overlay,
                                                            const QPointF& position) {
    return m_inputHandler && m_inputHandler->handleRegionDoubleClick(overlay, position);
}
