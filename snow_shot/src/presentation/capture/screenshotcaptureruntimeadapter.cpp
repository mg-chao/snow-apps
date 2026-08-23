#include "snow_shot/presentation/screenshotcaptureruntimeadapter.h"

#include "snow_shot/presentation/screenshotcapturecoordinator.h"
#include "snow_shot/presentation/screenshotcolorpickercontroller.h"
#include "snow_shot/presentation/screenshotoverlaycoordinator.h"
#include "snow_shot/presentation/screenshotselectorcoordinator.h"
#include "snow_shot/presentation/screenshotselectorworkflow.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QObject>

#include <utility>

ScreenshotCaptureRuntimeAdapter::ScreenshotCaptureRuntimeAdapter(
    ScreenshotCaptureRuntimeAdapterContext context)
    : m_context(context) {}

ScreenshotCaptureRuntimeAdapter::~ScreenshotCaptureRuntimeAdapter() {
    shutdownCaptureWorker();
}

void ScreenshotCaptureRuntimeAdapter::ensureCaptureCoordinator() {
    if (m_captureCoordinator != nullptr) {
        return;
    }

    m_captureCoordinator = std::make_unique<ScreenshotCaptureCoordinator>();
    QObject::connect(m_captureCoordinator.get(), &ScreenshotCaptureCoordinator::prepared,
                     m_captureCoordinator.get(), [this](quint64 requestId, bool ok) {
                         if (m_captureEventSink != nullptr) {
                             m_captureEventSink->handleCapturePrepared(requestId, ok);
                         }
                     });
    QObject::connect(m_captureCoordinator.get(),
                     &ScreenshotCaptureCoordinator::captureEnvironmentReady,
                     m_captureCoordinator.get(), [this](quint64 requestId, bool ok) {
                         if (m_captureEventSink != nullptr) {
                             m_captureEventSink->handleCaptureEnvironmentReady(requestId, ok);
                         }
                     });
    QObject::connect(m_captureCoordinator.get(), &ScreenshotCaptureCoordinator::captureFinished,
                     m_captureCoordinator.get(), [this](const ScreenshotCaptureResult& result) {
                         if (m_captureEventSink != nullptr) {
                             m_captureEventSink->handleCaptureFinished(result);
                         }
                     });
}

void ScreenshotCaptureRuntimeAdapter::setEventSink(ScreenshotCaptureWorkerEventSink* sink) {
    m_captureEventSink = sink;
}

bool ScreenshotCaptureRuntimeAdapter::captureWorkerCreated() const {
    return hasCaptureWorker();
}

bool ScreenshotCaptureRuntimeAdapter::hasCaptureWorker() const {
    return m_captureCoordinator != nullptr && m_captureCoordinator->hasWorker();
}

void ScreenshotCaptureRuntimeAdapter::ensureCaptureWorker() {
    ensureCaptureCoordinator();
    m_captureCoordinator->ensureWorker();
}

void ScreenshotCaptureRuntimeAdapter::prepareAsync(quint64 requestId) {
    ensureCaptureCoordinator();
    m_captureCoordinator->prepareAsync(requestId);
}

void ScreenshotCaptureRuntimeAdapter::captureAsync(const ScreenshotCaptureRequest& request) {
    ensureCaptureCoordinator();
    m_captureCoordinator->captureAsync(request);
}

void ScreenshotCaptureRuntimeAdapter::cancelActiveCapture() {
    if (m_captureCoordinator != nullptr) {
        m_captureCoordinator->cancelActiveCapture();
    }
}

bool ScreenshotCaptureRuntimeAdapter::releaseIdleResourcesAsync(
    quint64 requestId, std::function<void(bool released)> completion) {
    if (m_captureCoordinator == nullptr) {
        return false;
    }

    const bool scheduled =
        m_captureCoordinator->releaseIdleResourcesAsync(requestId, std::move(completion));
    return scheduled;
}

void ScreenshotCaptureRuntimeAdapter::shutdownCaptureWorker() {
    if (m_captureCoordinator == nullptr) {
        return;
    }

    m_captureCoordinator->shutdown();
    m_captureCoordinator.reset();
}

bool ScreenshotCaptureRuntimeAdapter::selectorReady() const {
    return m_context.selectorCoordinator.ready();
}

bool ScreenshotCaptureRuntimeAdapter::selectorRefreshInFlight() const {
    return m_context.selectorCoordinator.refreshInFlight();
}

bool ScreenshotCaptureRuntimeAdapter::selectorHitTestInFlight() const {
    return m_context.selectorCoordinator.hitTestInFlight();
}

void ScreenshotCaptureRuntimeAdapter::releaseSelectorCache() {
    m_context.selectorCoordinator.releaseCache();
}

void ScreenshotCaptureRuntimeAdapter::resetHitTestState() {
    m_context.selectorCoordinator.resetHitTestState();
}

void ScreenshotCaptureRuntimeAdapter::destroySelectorService() {
    m_context.selectorCoordinator.destroyService();
}

void ScreenshotCaptureRuntimeAdapter::startWorkflowRefresh() {
    m_context.selectorWorkflow.startRefresh();
}

void ScreenshotCaptureRuntimeAdapter::clearSelectorSelection() {
    m_context.selectorWorkflow.clearSelection();
}

bool ScreenshotCaptureRuntimeAdapter::updateSelectorSelectionAt(const QPoint& physicalPoint) {
    return m_context.selectorWorkflow.updateSelectionAt(physicalPoint);
}

bool ScreenshotCaptureRuntimeAdapter::applySelectorHitPath(const QVector<QRectF>& hitRects) {
    return m_context.selectorWorkflow.applyHitPath(hitRects);
}

void ScreenshotCaptureRuntimeAdapter::prewarmDisplayPool(ScreenshotDisplaySession& displaySession,
                                                         int displayCount) {
    m_context.overlayCoordinator.prewarmDisplayPool(displaySession, displayCount);
}

void ScreenshotCaptureRuntimeAdapter::ensureToolbar() {
    static_cast<void>(m_context.overlayCoordinator.ensureToolbar());
}

void ScreenshotCaptureRuntimeAdapter::prewarmToolbar() {
    m_context.overlayCoordinator.prewarmToolbar();
}

void ScreenshotCaptureRuntimeAdapter::clearOverlayCanvases(
    const ScreenshotDisplaySession& displaySession) const {
    m_context.overlayCoordinator.clearOverlayCanvases(displaySession);
}

void ScreenshotCaptureRuntimeAdapter::clearDisplays(ScreenshotDisplaySession& displaySession) {
    m_context.overlayCoordinator.clearDisplays(displaySession);
}

void ScreenshotCaptureRuntimeAdapter::hibernateDisplayPool(
    ScreenshotDisplaySession& displaySession) {
    m_context.overlayCoordinator.hibernateDisplayPool(displaySession);
}

void ScreenshotCaptureRuntimeAdapter::destroyDisplayPool(ScreenshotDisplaySession& displaySession) {
    m_context.overlayCoordinator.destroyDisplayPool(displaySession);
}

void ScreenshotCaptureRuntimeAdapter::resetForNewCapture(ScreenshotDisplaySession& displaySession) {
    m_context.overlayCoordinator.resetForNewCapture(displaySession);
}

void ScreenshotCaptureRuntimeAdapter::prepareDisplayModels(
    ScreenshotDisplaySession& displaySession) {
    m_context.overlayCoordinator.prepareDisplayModels(displaySession);
}

void ScreenshotCaptureRuntimeAdapter::applyDisplayModels(ScreenshotDisplaySession& displaySession) {
    m_context.overlayCoordinator.applyDisplayModels(displaySession);
}

bool ScreenshotCaptureRuntimeAdapter::preparePreCaptureOverlayWindows(
    ScreenshotDisplaySession& displaySession) {
    return m_context.overlayCoordinator.preparePreCaptureOverlayWindows(displaySession);
}

void ScreenshotCaptureRuntimeAdapter::showOverlayWindows(
    const ScreenshotDisplaySession& displaySession, ScreenshotOverlayShowMode mode) {
    m_context.overlayCoordinator.showOverlayWindows(displaySession, mode);
}

void ScreenshotCaptureRuntimeAdapter::activateOverlayWindows(
    const ScreenshotDisplaySession& displaySession, std::function<void()> interactionReady) {
    m_context.overlayCoordinator.activateOverlayWindows(displaySession,
                                                        std::move(interactionReady));
}

void ScreenshotCaptureRuntimeAdapter::hideOverlayWindows(
    const ScreenshotDisplaySession& displaySession) {
    m_context.overlayCoordinator.hideOverlayWindows(displaySession);
}

bool ScreenshotCaptureRuntimeAdapter::clearDocumentPreservingViewports() {
    return m_context.canvasRuntime.clearDocumentPreservingViewports();
}

bool ScreenshotCaptureRuntimeAdapter::resetCanvasRuntime() {
    return m_context.canvasRuntime.reset();
}

void ScreenshotCaptureRuntimeAdapter::resetColorPicker() {
    m_context.colorPickerController.reset();
}
