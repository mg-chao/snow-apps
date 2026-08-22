#include "snow_shot/presentation/screenshotpinnedcopyservice.h"

#include "snow_shot/presentation/screenshotdefaultstyles.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QList>
#include <QObject>

#include <utility>

namespace {
SnowCanvasRuntime* workerRuntime() {
    thread_local std::unique_ptr<SnowCanvasRuntime> runtime;
    if (runtime == nullptr) {
        runtime = std::make_unique<SnowCanvasRuntime>(
            SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
    }
    return runtime->isValid() ? runtime.get() : nullptr;
}

QImage renderCurrentViewport(const ScreenshotPinnedViewportCopyRequest& request) {
    SnowCanvasRuntime* runtime = workerRuntime();
    if (request.backgroundImage.isNull() || !request.backgroundCanvasRect.isValid() ||
        request.backgroundCanvasRect.isEmpty() || !request.contentPixelSize.isValid() ||
        request.contentPixelSize.isEmpty() || runtime == nullptr) {
        return {};
    }
    if (!request.documentSession.isEmpty() &&
        !runtime->restoreDocumentSession(request.documentSession)) {
        return {};
    }
    if (request.documentSession.isEmpty() && !runtime->clearDocumentPreservingViewports()) {
        return {};
    }

    const QList<CanvasExportSource> sources{
        CanvasExportSource{request.backgroundImage, request.backgroundCanvasRect}};
    QImage content =
        runtime->renderToImage(request.backgroundCanvasRect, request.contentPixelSize, sources);
    if (content.isNull()) {
        return {};
    }
    return ScreenshotResultCompositor::compose(content, request.resultStyle);
}

ScreenshotClipboardPayload
prepareCurrentViewport(const ScreenshotPinnedViewportCopyRequest& request) {
    return ScreenshotClipboardService::prepareImage(renderCurrentViewport(request));
}
} // namespace

struct ScreenshotPinnedCopyService::State final {
    quint64 generation = 0;
    RequestKind activeKind = RequestKind::None;
    bool requestInFlight = false;
};

ScreenshotPinnedCopyService::ScreenshotPinnedCopyService() : m_state(std::make_shared<State>()) {}

ScreenshotPinnedCopyService::~ScreenshotPinnedCopyService() {
    invalidate();
}

bool ScreenshotPinnedCopyService::beginRequest(RequestKind kind, quint64* generation) {
    if (generation == nullptr || kind == RequestKind::None || m_state == nullptr) {
        return false;
    }
    if (m_state->requestInFlight && m_state->activeKind == kind) {
        return false;
    }
    m_job.cancel();
    *generation = ++m_state->generation;
    m_state->activeKind = kind;
    m_state->requestInFlight = true;
    return true;
}

bool ScreenshotPinnedCopyService::requestCurrentViewport(
    ScreenshotPinnedViewportCopyRequest request, QObject* receiver, Callback callback) {
    if (receiver == nullptr || !callback || request.backgroundImage.isNull()) {
        return false;
    }
    quint64 generation = 0;
    if (!beginRequest(RequestKind::CurrentViewport, &generation)) {
        return false;
    }

    m_job = ScreenshotExportCoordinator::shared().submit(
        receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [request = std::move(request)](const ScreenshotExportCancellation& cancellation) mutable {
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The pinned image copy was cancelled"));
            }
            auto payload =
                std::make_shared<ScreenshotClipboardPayload>(prepareCurrentViewport(request));
            if (!payload->isValid()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Render,
                    QStringLiteral("The pinned image could not be rendered"));
            }
            ScreenshotExportTaskResult result;
            result.clipboardPayload = std::move(payload);
            return result;
        },
        [state = m_state, generation,
         callback = std::move(callback)](ScreenshotExportTaskResult result) mutable {
            if (state == nullptr || generation != state->generation) {
                return;
            }
            state->requestInFlight = false;
            state->activeKind = RequestKind::None;
            callback(result.clipboardPayload != nullptr ? std::move(*result.clipboardPayload)
                                                        : ScreenshotClipboardPayload{});
        });
    if (!m_job.isValid()) {
        m_state->requestInFlight = false;
        m_state->activeKind = RequestKind::None;
        return false;
    }
    return true;
}

bool ScreenshotPinnedCopyService::requestOriginalImage(QImage image, QObject* receiver,
                                                       Callback callback) {
    if (receiver == nullptr || !callback || image.isNull()) {
        return false;
    }
    quint64 generation = 0;
    if (!beginRequest(RequestKind::OriginalImage, &generation)) {
        return false;
    }

    m_job = ScreenshotExportCoordinator::shared().submit(
        receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [image = std::move(image)](const ScreenshotExportCancellation& cancellation) mutable {
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The original image copy was cancelled"));
            }
            auto payload = std::make_shared<ScreenshotClipboardPayload>(
                ScreenshotClipboardService::prepareImage(image));
            if (!payload->isValid()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Clipboard,
                    QStringLiteral("The original image could not be prepared"));
            }
            ScreenshotExportTaskResult result;
            result.clipboardPayload = std::move(payload);
            return result;
        },
        [state = m_state, generation,
         callback = std::move(callback)](ScreenshotExportTaskResult result) mutable {
            if (state == nullptr || generation != state->generation) {
                return;
            }
            state->requestInFlight = false;
            state->activeKind = RequestKind::None;
            callback(result.clipboardPayload != nullptr ? std::move(*result.clipboardPayload)
                                                        : ScreenshotClipboardPayload{});
        });
    if (!m_job.isValid()) {
        m_state->requestInFlight = false;
        m_state->activeKind = RequestKind::None;
        return false;
    }
    return true;
}

bool ScreenshotPinnedCopyService::requestCurrentImage(ScreenshotPinnedViewportCopyRequest request,
                                                       QObject* receiver,
                                                       ImageCallback callback) {
    if (receiver == nullptr || !callback || request.backgroundImage.isNull()) {
        return false;
    }
    quint64 generation = 0;
    if (!beginRequest(RequestKind::CurrentImage, &generation)) {
        return false;
    }

    m_job = ScreenshotExportCoordinator::shared().submit(
        receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [request = std::move(request)](const ScreenshotExportCancellation& cancellation) mutable {
            if (cancellation.isCancellationRequested()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Cancelled,
                    QStringLiteral("The pinned image export was cancelled"));
            }
            QImage image = renderCurrentViewport(request);
            if (image.isNull()) {
                return ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Render,
                    QStringLiteral("The pinned image could not be rendered"));
            }
            ScreenshotExportTaskResult result;
            result.image = std::move(image);
            return result;
        },
        [state = m_state, generation,
         callback = std::move(callback)](ScreenshotExportTaskResult result) mutable {
            if (state == nullptr || generation != state->generation) {
                return;
            }
            state->requestInFlight = false;
            state->activeKind = RequestKind::None;
            callback(result.succeeded() ? std::move(result.image) : QImage{});
        });
    if (!m_job.isValid()) {
        m_state->requestInFlight = false;
        m_state->activeKind = RequestKind::None;
        return false;
    }
    return true;
}

void ScreenshotPinnedCopyService::invalidate() {
    m_job.cancel();
    m_job = {};
    if (m_state != nullptr) {
        ++m_state->generation;
        m_state->requestInFlight = false;
        m_state->activeKind = RequestKind::None;
    }
}
