#include "snow_shot/presentation/screenshotexportservice.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotasyncactivitytracker.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include "screenshotclipboardperfinstrumentation.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QList>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QPointer>
#include <QScreen>
#include <QThread>

#include <algorithm>
#include <atomic>
#include <utility>

namespace {
QList<CanvasExportSource> exportSourcesForSelection(const ScreenshotDisplaySession& displaySession,
                                                    const QRect& selection) {
    QList<CanvasExportSource> sources;
    sources.reserve(displaySession.size());
    const ScreenshotHalfOpenRect selectionRect = ScreenshotHalfOpenRect::fromRect(selection);
    displaySession.forEachActiveDisplay([&sources, &selectionRect](
                                            qsizetype, const CapturedDisplayModel& display) {
        const QRectF canvasRect = ScreenshotGeometryMapper::displayImageSourceCanvasRect(display);
        if (display.image.isNull() ||
            !selectionRect.intersects(ScreenshotHalfOpenRect::fromRectF(canvasRect))) {
            return;
        }

        sources.push_back(CanvasExportSource{
            display.image,
            canvasRect,
        });
    });
    return sources;
}

QRectF pinnedSurfaceCanvasRect(const QRect& selection, int shadowPadding) {
    QRectF canvasRect(selection);
    if (shadowPadding <= 0) {
        return canvasRect;
    }
    return canvasRect.adjusted(
        -static_cast<qreal>(shadowPadding), -static_cast<qreal>(shadowPadding),
        static_cast<qreal>(shadowPadding), static_cast<qreal>(shadowPadding));
}

const CapturedDisplayModel* displayForPinAnchor(const ScreenshotDisplaySession& displaySession,
                                                const ScreenshotGeometryMapper& geometry,
                                                const QRect& selection) {
    const auto topLeft =
        QPointF(static_cast<qreal>(selection.left()), static_cast<qreal>(selection.top()));
    const CapturedDisplayModel* display = geometry.displayForCanvasPoint(displaySession, topLeft);
    if (display == nullptr) {
        display = geometry.displayForCanvasRect(displaySession, QRectF(selection));
    }
    return display;
}

QImage composeSelectionResultFromRuntime(SnowCanvasRuntime& runtime, const QRect& selection,
                                         const ScreenshotResultStyle& style,
                                         const QList<CanvasExportSource>& sources) {
    if (selection.width() < 1 || selection.height() < 1) {
        return {};
    }

    QImage content;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.render_canvas");
        content = runtime.renderToImage(QRectF(selection), selection.size(), sources);
    }
    if (content.isNull()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.render_canvas", 1);
        return {};
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.rendered_bytes", content.sizeInBytes());
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.compose_result");
    return ScreenshotResultCompositor::compose(content, style);
}

ScreenshotClipboardFormatMode clipboardFormatForStyle(const ScreenshotResultStyle& style) {
    const ScreenshotResultStyle normalized = ScreenshotResultCompositor::normalizedStyle(style);
    return normalized.cornerRadius == 0 && normalized.shadowWidth == 0
               ? ScreenshotClipboardFormatMode::CompatibleDib
               : ScreenshotClipboardFormatMode::DibV5;
}

ScreenshotPinnedSelectionRequest preparePinnedSelectionRequest(
    const ScreenshotDisplaySession& displaySession, const ScreenshotGeometryMapper& geometry,
    const QRect& selection, const ScreenshotResultStyle& style,
    const QList<CanvasExportSource>& sources) {
    ScreenshotPinnedSelectionRequest request;
    const CapturedDisplayModel* display = displayForPinAnchor(displaySession, geometry, selection);
    if (display == nullptr) {
        return request;
    }

    request.resultStyle = ScreenshotResultCompositor::normalizedStyle(style);
    const ScreenshotResultLayout layout = ScreenshotResultCompositor::layoutForContent(
        selection.size(), request.resultStyle);
    if (!layout.isValid()) {
        return request;
    }
    const int shadowPadding = layout.effectInsets.left();
    const ScreenshotPinnedImagePlacement placement =
        geometry.pinnedImagePlacement(displaySession, selection, layout.outputRect.size(),
                                      shadowPadding);
    if (!placement.valid) {
        return request;
    }
    QList<ScreenshotImageLayer> layers;
    layers.reserve(sources.size());
    const QRectF contentCanvasRect(selection);
    for (const CanvasExportSource& source : sources) {
        const QRectF destination = source.canvasRect.intersected(contentCanvasRect);
        ScreenshotImageLayer layer{source.image, source.canvasRect, destination};
        if (layer.isValid()) {
            layers.push_back(std::move(layer));
        }
    }
    request.imageSource = ScreenshotImageSource::fromLayers(std::move(layers));
    request.geometry = placement.geometry;
    request.contentCanvasRect = contentCanvasRect;
    request.surfaceCanvasRect = pinnedSurfaceCanvasRect(selection, shadowPadding);
    request.geometry.canvasSourceRect = request.surfaceCanvasRect;
    request.fullResolutionScaleBasis = layout.outputRect.size();
    request.screen = placement.screen;
    return request;
}

class ScreenshotExportWorker final : public QObject {
  public:
    QImage renderSelection(const QByteArray& documentSession, const QRect& selection,
                           const ScreenshotResultStyle& style,
                           const QList<CanvasExportSource>& sources,
                           const QImage& directSourceImage) {
        QImage image;
        if (!directSourceImage.isNull()) {
            SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.compose_direct_source");
            image = ScreenshotResultCompositor::compose(directSourceImage, style);
        } else {
            {
                SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.ensure_worker_runtime");
                if (!ensureRuntime()) {
                    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.worker_runtime", 1);
                    return {};
                }
            }
            {
                SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.restore_document");
                if (!m_runtime->restoreDocumentSession(documentSession)) {
                    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.restore_document", 1);
                    return {};
                }
            }
            image = composeSelectionResultFromRuntime(*m_runtime, selection, style, sources);
        }
        if (image.isNull()) {
            return {};
        }
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (image.format() != QImage::Format_ARGB32) {
            SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.convert_argb32");
            image = image.convertToFormat(QImage::Format_ARGB32);
        }
#endif
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.output_width", image.width());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.output_height", image.height());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.output_bytes", image.sizeInBytes());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.success", 1);
        return image;
    }

    ScreenshotSelectionClipboardResult prepareSelectionClipboard(
        const QByteArray& documentSession, const QRect& selection,
        const ScreenshotResultStyle& style, const QList<CanvasExportSource>& sources,
        const QImage& directSourceImage) {
        ScreenshotSelectionClipboardResult result;
        result.image =
            renderSelection(documentSession, selection, style, sources, directSourceImage);
        result.payload = ScreenshotClipboardService::prepareImage(
            result.image, clipboardFormatForStyle(style));
        return result;
    }

  private:
    bool ensureRuntime() {
        if (m_runtime == nullptr) {
            m_runtime = std::make_unique<SnowCanvasRuntime>(SnowCanvasRuntimeConfig{
                snow_shot::presentation::screenshotCanvasStyleDefaults()});
        }
        return m_runtime->isValid();
    }

    std::unique_ptr<SnowCanvasRuntime> m_runtime;
};
} // namespace

struct ScreenshotExportService::PendingRequestState final {
    PendingRequestState(QObject* completionContext, std::function<void()> becameIdle)
        : completionContext(completionContext), becameIdle(std::move(becameIdle)) {}

    void requestStarted() noexcept {
        pending.fetch_add(1, std::memory_order_acq_rel);
    }

    void requestFinished() {
        const int previous = pending.fetch_sub(1, std::memory_order_acq_rel);
        if (previous != 1) {
            return;
        }

        QMutexLocker lock(&notificationMutex);
        const QPointer<QObject> guardedCompletionContext(completionContext);
        if (guardedCompletionContext.isNull() || !becameIdle) {
            return;
        }
        const auto callback = becameIdle;
        static_cast<void>(QMetaObject::invokeMethod(
            guardedCompletionContext,
            [guardedCompletionContext, callback]() {
                if (!guardedCompletionContext.isNull()) {
                    callback();
                }
            },
            Qt::QueuedConnection));
    }

    [[nodiscard]] bool hasPendingRequests() const noexcept {
        return pending.load(std::memory_order_acquire) > 0;
    }

    void disableIdleNotification() {
        QMutexLocker lock(&notificationMutex);
        completionContext = nullptr;
        becameIdle = {};
    }

    std::atomic_int pending{0};
    QMutex notificationMutex;
    QPointer<QObject> completionContext;
    std::function<void()> becameIdle;
};

class ScreenshotExportService::PendingRequest final {
  public:
    explicit PendingRequest(std::shared_ptr<PendingRequestState> state)
        : m_state(std::move(state)),
          m_activityLease(ScreenshotAsyncActivityTracker::shared().acquire()) {
        m_state->requestStarted();
    }

    ~PendingRequest() {
        finish();
    }

    void finish() {
        if (m_state == nullptr) {
            return;
        }
        m_activityLease.reset();
        std::shared_ptr<PendingRequestState> state = std::move(m_state);
        state->requestFinished();
    }

  private:
    std::shared_ptr<PendingRequestState> m_state;
    ScreenshotAsyncActivityLease m_activityLease;
};

ScreenshotExportService::ScreenshotExportService(ScreenshotExportServiceContext context)
    : m_context(context), m_thread(std::make_unique<QThread>()),
      m_worker(new ScreenshotExportWorker), m_completionContext(new QObject) {
    m_pendingRequestState = std::make_shared<PendingRequestState>(
        m_completionContext, std::move(m_context.becameIdle));
    m_thread->setObjectName(QStringLiteral("ScreenshotExportWorker"));
    m_worker->moveToThread(m_thread.get());
    QObject::connect(m_thread.get(), &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

ScreenshotExportService::~ScreenshotExportService() {
    m_pendingRequestState->disableIdleNotification();
    if (m_thread != nullptr) {
        m_thread->quit();
        m_thread->wait();
    }
    delete m_completionContext;
    m_completionContext = nullptr;
    m_worker = nullptr;
}

bool ScreenshotExportService::hasPendingRequests() const noexcept {
    return m_pendingRequestState != nullptr && m_pendingRequestState->hasPendingRequests();
}

void ScreenshotExportService::setNextSelectionSourceImage(QImage image) {
    image.setDevicePixelRatio(1.0);
    m_nextSelectionSourceImage = std::move(image);
}

void ScreenshotExportService::clearNextSelectionSourceImage() {
    m_nextSelectionSourceImage = {};
}

bool ScreenshotExportService::requestSelectionResult(
    const QRect& selection, const ScreenshotResultStyle& style, QObject* receiver,
    ImageCallback callback) {
    if (selection.isEmpty() || receiver == nullptr || !callback || m_worker == nullptr ||
        m_thread == nullptr || !m_thread->isRunning()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.invalid_request", 1);
        return false;
    }
    const snow_shot::presentation::clipboard_perf::Stopwatch requestTimer;
    QImage directSourceImage = std::exchange(m_nextSelectionSourceImage, QImage());
    QByteArray documentSession;
    if (directSourceImage.isNull()) {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.serialize_document");
        documentSession = m_context.runtime.serializeDocumentSession();
    }
    QList<CanvasExportSource> sources;
    if (directSourceImage.isNull()) {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.collect_sources");
        sources = exportSourcesForSelection(m_context.displaySession, selection);
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.document_bytes", documentSession.size());
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.source_count",
                                     directSourceImage.isNull() ? sources.size() : 1);
    if (directSourceImage.isNull() && (documentSession.isEmpty() || sources.isEmpty())) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.empty_input", 1);
        return false;
    }
    auto pendingRequest = std::make_shared<PendingRequest>(m_pendingRequestState);
    auto* worker = static_cast<ScreenshotExportWorker*>(m_worker);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<QObject> guardedCompletionContext(m_completionContext);
    const snow_shot::presentation::clipboard_perf::Stopwatch workerQueueTimer;
    bool scheduled = false;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.schedule_worker");
        scheduled = QMetaObject::invokeMethod(
            worker,
            [worker, guardedReceiver, guardedCompletionContext,
             documentSession = std::move(documentSession), selection, style,
             sources = std::move(sources), directSourceImage = std::move(directSourceImage),
             requestTimer, workerQueueTimer, pendingRequest,
             callback = std::move(callback)]() mutable {
            snow_shot::presentation::clipboard_perf::duration(
                "export.worker_queue_delay", workerQueueTimer.elapsedNanoseconds());
            QImage image = worker->renderSelection(documentSession, selection, style, sources,
                                                   directSourceImage);
            documentSession.clear();
            sources.clear();
            directSourceImage = {};
            if (guardedCompletionContext.isNull()) {
                callback = {};
                image = {};
                pendingRequest->finish();
                return;
            }
            const snow_shot::presentation::clipboard_perf::Stopwatch callbackQueueTimer;
            const bool callbackScheduled = QMetaObject::invokeMethod(
                guardedCompletionContext,
                [guardedReceiver, guardedCompletionContext, image = std::move(image),
                 callback = std::move(callback), requestTimer, callbackQueueTimer,
                 pendingRequest]() mutable {
                    snow_shot::presentation::clipboard_perf::duration(
                        "export.callback_queue_delay", callbackQueueTimer.elapsedNanoseconds());
                    snow_shot::presentation::clipboard_perf::duration(
                        "export.request_to_result", requestTimer.elapsedNanoseconds());
                    if (!guardedReceiver.isNull() && !guardedCompletionContext.isNull()) {
                        callback(std::move(image));
                    } else {
                        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.receiver_destroyed", 1);
                    }
                    callback = {};
                    image = {};
                    pendingRequest->finish();
                },
                Qt::QueuedConnection);
            if (!callbackScheduled) {
                SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.schedule_callback", 1);
                callback = {};
                image = {};
                pendingRequest->finish();
            }
        },
            Qt::QueuedConnection);
    }
    if (!scheduled) {
        pendingRequest->finish();
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER(scheduled ? "export.request_scheduled"
                                               : "export.failure.schedule_worker",
                                     1);
    return scheduled;
}

bool ScreenshotExportService::requestSelectionClipboard(
    const QRect& selection, const ScreenshotResultStyle& style, QObject* receiver,
    ClipboardCallback callback) {
    if (selection.isEmpty() || receiver == nullptr || !callback || m_worker == nullptr ||
        m_thread == nullptr || !m_thread->isRunning()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.invalid_clipboard_request", 1);
        return false;
    }

    const snow_shot::presentation::clipboard_perf::Stopwatch requestTimer;
    QImage directSourceImage = std::exchange(m_nextSelectionSourceImage, QImage());
    QByteArray documentSession;
    if (directSourceImage.isNull()) {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.serialize_document");
        documentSession = m_context.runtime.serializeDocumentSession();
    }
    QList<CanvasExportSource> sources;
    if (directSourceImage.isNull()) {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.collect_sources");
        sources = exportSourcesForSelection(m_context.displaySession, selection);
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.document_bytes", documentSession.size());
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.source_count",
                                     directSourceImage.isNull() ? sources.size() : 1);
    if (directSourceImage.isNull() && (documentSession.isEmpty() || sources.isEmpty())) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.empty_input", 1);
        return false;
    }

    auto pendingRequest = std::make_shared<PendingRequest>(m_pendingRequestState);
    auto* worker = static_cast<ScreenshotExportWorker*>(m_worker);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<QObject> guardedCompletionContext(m_completionContext);
    const snow_shot::presentation::clipboard_perf::Stopwatch workerQueueTimer;
    const bool scheduled = QMetaObject::invokeMethod(
        worker,
        [worker, guardedReceiver, guardedCompletionContext,
         documentSession = std::move(documentSession), selection, style,
         sources = std::move(sources), directSourceImage = std::move(directSourceImage),
         requestTimer, workerQueueTimer, pendingRequest,
         callback = std::move(callback)]() mutable {
            snow_shot::presentation::clipboard_perf::duration(
                "export.worker_queue_delay", workerQueueTimer.elapsedNanoseconds());
            auto result = std::make_shared<ScreenshotSelectionClipboardResult>(
                worker->prepareSelectionClipboard(documentSession, selection, style, sources,
                                                  directSourceImage));
            documentSession.clear();
            sources.clear();
            directSourceImage = {};
            if (guardedCompletionContext.isNull()) {
                callback = {};
                result.reset();
                pendingRequest->finish();
                return;
            }
            const snow_shot::presentation::clipboard_perf::Stopwatch callbackQueueTimer;
            const bool callbackScheduled = QMetaObject::invokeMethod(
                guardedCompletionContext,
                [guardedReceiver, guardedCompletionContext, result,
                 callback = std::move(callback), requestTimer, callbackQueueTimer,
                 pendingRequest]() mutable {
                    snow_shot::presentation::clipboard_perf::duration(
                        "export.callback_queue_delay", callbackQueueTimer.elapsedNanoseconds());
                    snow_shot::presentation::clipboard_perf::duration(
                        "export.request_to_result", requestTimer.elapsedNanoseconds());
                    if (!guardedReceiver.isNull() && !guardedCompletionContext.isNull()) {
                        callback(std::move(*result));
                    } else {
                        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.receiver_destroyed", 1);
                    }
                    callback = {};
                    result.reset();
                    pendingRequest->finish();
                },
                Qt::QueuedConnection);
            result.reset();
            if (!callbackScheduled) {
                SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.schedule_callback", 1);
                callback = {};
                pendingRequest->finish();
            }
        },
        Qt::QueuedConnection);
    if (!scheduled) {
        pendingRequest->finish();
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER(scheduled ? "export.clipboard_request_scheduled"
                                               : "export.failure.schedule_worker",
                                     1);
    return scheduled;
}

std::optional<ScreenshotPinnedSelectionRequest>
ScreenshotExportService::preparePinnedSelection(const QRect& selection,
                                                const ScreenshotResultStyle& style) const {
    if (selection.isEmpty()) {
        return std::nullopt;
    }
    SNOW_SHOT_PIN_PERF_SCOPE("export.prepare_pin_plan");
    const QList<CanvasExportSource> sources =
        exportSourcesForSelection(m_context.displaySession, selection);
    ScreenshotPinnedSelectionRequest request = preparePinnedSelectionRequest(
        m_context.displaySession, m_context.geometry, selection, style, sources);
    if (!request.isValid()) {
        return std::nullopt;
    }
    SNOW_SHOT_PIN_PERF_COUNTER("source.mode.layered", 1);
    SNOW_SHOT_PIN_PERF_COUNTER("source.layer_count", request.imageSource.layers.size());
    SNOW_SHOT_PIN_PERF_COUNTER("source.retained_bytes",
                               request.imageSource.retainedBytes());
    return request;
}

bool ScreenshotExportService::schedulePinnedSelection(
    ScreenshotPinnedSelectionRequest request, QObject* receiver,
    PinRequestCallback callback) {
    if (!request.isValid() || receiver == nullptr || !callback ||
        m_completionContext == nullptr) {
        return false;
    }
    auto pendingRequest = std::make_shared<PendingRequest>(m_pendingRequestState);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<QObject> guardedCompletionContext(m_completionContext);
    const bool scheduled = QMetaObject::invokeMethod(
        m_completionContext,
        [guardedReceiver, guardedCompletionContext, request = std::move(request),
         callback = std::move(callback), pendingRequest]() mutable {
            SNOW_SHOT_PIN_PERF_SCOPE("export.pin_callback");
            if (!guardedReceiver.isNull() && !guardedCompletionContext.isNull()) {
                callback(std::move(request));
            }
            callback = {};
            request = {};
            pendingRequest->finish();
        },
        Qt::QueuedConnection);
    if (!scheduled) {
        pendingRequest->finish();
    }
    return scheduled;
}
