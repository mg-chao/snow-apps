#include "snow_shot/presentation/screenshotexportservice.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_shot/presentation/screenshotselectionpin.h"

#include "screenshotclipboardperfinstrumentation.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QThread>

#include <optional>
#include <utility>

namespace {
QList<CanvasExportSource> exportSourcesForSelection(const ScreenshotDisplaySession& displaySession,
                                                    const QRect& selection) {
    QList<CanvasExportSource> sources;
    sources.reserve(displaySession.size());
    const ScreenshotHalfOpenRect selectionRect = ScreenshotHalfOpenRect::fromRect(selection);
    displaySession.forEachImageSource([&sources, &selectionRect](
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

QImage composeSelectionResultFromRuntime(SnowCanvasRuntime& runtime, const QRect& selection,
                                         const ScreenshotResultStyle& style,
                                         const QList<CanvasExportSource>& sources,
                                         const ScreenshotSelectionRenderSpec& spec = {}) {
    if (selection.width() < 1 || selection.height() < 1) {
        qWarning(
            "Screenshot selection export failed: stage=selection_validation width=%d height=%d",
            selection.width(), selection.height());
        return {};
    }

    QImage content;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.render_canvas");
        SNOW_SHOT_PIN_PERF_SCOPE("export.render_canvas");
        content = runtime.renderToImage(
            QRectF(selection), spec.isValid() ? spec.pixelSize : selection.size(), sources);
    }
    if (content.isNull()) {
        qWarning("Screenshot selection export failed: stage=render_canvas width=%d height=%d "
                 "sources=%lld",
                 selection.width(), selection.height(), static_cast<long long>(sources.size()));
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.render_canvas", 1);
        return {};
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.rendered_bytes", content.sizeInBytes());
    SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.compose_result");
    SNOW_SHOT_PIN_PERF_SCOPE("export.compose_result");
    ScreenshotResultStyle outputStyle = style;
    outputStyle.regionScale = style.regionScale * spec.scale;
    outputStyle.cornerRadius = qRound(style.cornerRadius * spec.scale);
    outputStyle.shadowWidth =
        screenshotSelectionRenderedShadowPixels(style.shadowWidth, spec.scale);
    QImage result = ScreenshotResultCompositor::compose(content, outputStyle);
    if (result.isNull()) {
        qWarning("Screenshot selection export failed: stage=compose_result width=%d height=%d",
                 content.width(), content.height());
    }
    return result;
}

class ScreenshotExportWorker final : public QObject {
  public:
    QImage renderSelection(const QByteArray& documentSession,
                           const SnowCanvasSmartEraseSnapshot& smartErase, const QRect& selection,
                           const ScreenshotResultStyle& style,
                           const QList<CanvasExportSource>& sources,
                           const ScreenshotSelectionRenderSpec& spec = {}) {
        // The pin trace needs the export baseline even though these stages are
        // shared with the clipboard-copy flows; the sink drops records whenever
        // no pin sample is active.
        SNOW_SHOT_PIN_PERF_SCOPE("export.render_selection");
        QImage image;
        {
            SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.ensure_worker_runtime");
            SNOW_SHOT_PIN_PERF_SCOPE("export.ensure_worker_runtime");
            if (!ensureRuntime()) {
                qWarning("Screenshot selection export failed: stage=worker_runtime");
                SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.worker_runtime", 1);
                return {};
            }
        }
        {
            SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.restore_document");
            SNOW_SHOT_PIN_PERF_SCOPE("export.restore_document");
            if (!m_runtime->restoreDocumentSession(documentSession)) {
                qWarning("Screenshot selection export failed: stage=restore_document bytes=%lld",
                         static_cast<long long>(documentSession.size()));
                SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.restore_document", 1);
                return {};
            }
        }
        m_runtime->restoreSmartEraseSnapshot(smartErase);
        image = composeSelectionResultFromRuntime(*m_runtime, selection, style, sources, spec);
        if (image.isNull()) {
            return {};
        }
        SNOW_SHOT_PIN_PERF_COUNTER("export.output_bytes", image.sizeInBytes());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.output_width", image.width());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.output_height", image.height());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.output_bytes", image.sizeInBytes());
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.success", 1);
        return image;
    }

    ScreenshotSelectionClipboardResult prepareSelectionClipboard(
        const QByteArray& documentSession, const SnowCanvasSmartEraseSnapshot& smartErase,
        const QRect& selection, const ScreenshotResultStyle& style,
        const QList<CanvasExportSource>& sources, const ScreenshotSelectionRenderSpec& spec) {
        ScreenshotSelectionClipboardResult result;
        result.image =
            renderSelection(documentSession, smartErase, selection, style, sources, spec);
        result.payload = ScreenshotClipboardService::prepareImage(result.image);
        return result;
    }

  private:
    bool ensureRuntime() {
        if (m_runtime == nullptr) {
            m_runtime = std::make_unique<SnowCanvasRuntime>(
                SnowCanvasRuntimeConfig{snow_shot::presentation::screenshotCanvasStyleDefaults()});
        }
        return m_runtime->isValid();
    }

    std::unique_ptr<SnowCanvasRuntime> m_runtime;
};
} // namespace

ScreenshotExportService::ScreenshotExportService(ScreenshotExportServiceContext context)
    : m_context(context), m_thread(std::make_unique<QThread>()),
      m_worker(new ScreenshotExportWorker), m_completionContext(new QObject) {
    m_thread->setObjectName(QStringLiteral("ScreenshotExportWorker"));
    m_worker->moveToThread(m_thread.get());
    QObject::connect(m_thread.get(), &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

ScreenshotExportService::~ScreenshotExportService() {
    delete m_completionContext;
    m_completionContext = nullptr;
    if (m_thread != nullptr) {
        m_thread->quit();
        m_thread->wait();
    }
    m_worker = nullptr;
}

bool ScreenshotExportService::requestSelectionResult(const QRect& selection,
                                                     const ScreenshotResultStyle& style,
                                                     QObject* receiver, ImageCallback callback) {
    if (selection.isEmpty() || receiver == nullptr || !callback || m_worker == nullptr ||
        m_thread == nullptr || !m_thread->isRunning()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.invalid_request", 1);
        return false;
    }
    const snow_shot::presentation::clipboard_perf::Stopwatch requestTimer;
    const auto smartErase = m_context.runtime.smartEraseSnapshot();
    QByteArray documentSession;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.serialize_document");
        documentSession = m_context.runtime.serializeDocumentSession();
    }
    const auto spec = screenshotSelectionRenderSpec(m_context.displaySession, selection);
    if (!spec.isValid())
        return false;
    QList<CanvasExportSource> sources;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.collect_sources");
        sources = exportSourcesForSelection(m_context.displaySession, selection);
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.document_bytes", documentSession.size());
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.source_count", sources.size());
    if (documentSession.isEmpty() || sources.isEmpty()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.empty_input", 1);
        return false;
    }
    auto* worker = static_cast<ScreenshotExportWorker*>(m_worker);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<QObject> guardedCompletionContext(m_completionContext);
    const snow_shot::presentation::clipboard_perf::Stopwatch workerQueueTimer;
    bool scheduled = false;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.schedule_worker");
        scheduled = QMetaObject::invokeMethod(
            worker,
            [worker, guardedReceiver, guardedCompletionContext, documentSession, smartErase,
             selection, style, sources, spec, requestTimer, workerQueueTimer,
             callback = std::move(callback)]() mutable {
                snow_shot::presentation::clipboard_perf::duration(
                    "export.worker_queue_delay", workerQueueTimer.elapsedNanoseconds());
                QImage image = worker->renderSelection(documentSession, smartErase, selection,
                                                       style, sources, spec);
                if (guardedReceiver.isNull() || guardedCompletionContext.isNull()) {
                    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.receiver_destroyed", 1);
                    return;
                }
                const snow_shot::presentation::clipboard_perf::Stopwatch callbackQueueTimer;
                const bool callbackScheduled = QMetaObject::invokeMethod(
                    guardedCompletionContext,
                    [guardedReceiver, guardedCompletionContext, image = std::move(image),
                     callback = std::move(callback), requestTimer, callbackQueueTimer]() mutable {
                        snow_shot::presentation::clipboard_perf::duration(
                            "export.callback_queue_delay", callbackQueueTimer.elapsedNanoseconds());
                        snow_shot::presentation::clipboard_perf::duration(
                            "export.request_to_result", requestTimer.elapsedNanoseconds());
                        if (!guardedReceiver.isNull() && !guardedCompletionContext.isNull()) {
                            callback(std::move(image));
                        }
                    },
                    Qt::QueuedConnection);
                if (!callbackScheduled) {
                    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.schedule_callback", 1);
                }
            },
            Qt::QueuedConnection);
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER(
        scheduled ? "export.request_scheduled" : "export.failure.schedule_worker", 1);
    return scheduled;
}

bool ScreenshotExportService::requestSelectionClipboard(const QRect& selection,
                                                        const ScreenshotResultStyle& style,
                                                        QObject* receiver,
                                                        ClipboardCallback callback) {
    if (selection.isEmpty() || receiver == nullptr || !callback || m_worker == nullptr ||
        m_thread == nullptr || !m_thread->isRunning()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.invalid_clipboard_request", 1);
        return false;
    }

    const snow_shot::presentation::clipboard_perf::Stopwatch requestTimer;
    const auto smartErase = m_context.runtime.smartEraseSnapshot();
    QByteArray documentSession;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.serialize_document");
        documentSession = m_context.runtime.serializeDocumentSession();
    }
    const auto spec = screenshotSelectionRenderSpec(m_context.displaySession, selection);
    if (!spec.isValid())
        return false;
    QList<CanvasExportSource> sources;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.collect_sources");
        sources = exportSourcesForSelection(m_context.displaySession, selection);
    }
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.document_bytes", documentSession.size());
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.source_count", sources.size());
    if (documentSession.isEmpty() || sources.isEmpty()) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.empty_input", 1);
        return false;
    }

    auto* worker = static_cast<ScreenshotExportWorker*>(m_worker);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<QObject> guardedCompletionContext(m_completionContext);
    const snow_shot::presentation::clipboard_perf::Stopwatch workerQueueTimer;
    const bool scheduled = QMetaObject::invokeMethod(
        worker,
        [worker, guardedReceiver, guardedCompletionContext, documentSession, smartErase, selection,
         style, sources, spec, requestTimer, workerQueueTimer,
         callback = std::move(callback)]() mutable {
            snow_shot::presentation::clipboard_perf::duration(
                "export.worker_queue_delay", workerQueueTimer.elapsedNanoseconds());
            auto result = std::make_shared<ScreenshotSelectionClipboardResult>(
                worker->prepareSelectionClipboard(documentSession, smartErase, selection, style,
                                                  sources, spec));
            if (guardedReceiver.isNull() || guardedCompletionContext.isNull()) {
                SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.receiver_destroyed", 1);
                return;
            }
            const snow_shot::presentation::clipboard_perf::Stopwatch callbackQueueTimer;
            const bool callbackScheduled = QMetaObject::invokeMethod(
                guardedCompletionContext,
                [guardedReceiver, guardedCompletionContext, result, callback = std::move(callback),
                 requestTimer, callbackQueueTimer]() mutable {
                    snow_shot::presentation::clipboard_perf::duration(
                        "export.callback_queue_delay", callbackQueueTimer.elapsedNanoseconds());
                    snow_shot::presentation::clipboard_perf::duration(
                        "export.request_to_result", requestTimer.elapsedNanoseconds());
                    if (!guardedReceiver.isNull() && !guardedCompletionContext.isNull()) {
                        callback(std::move(*result));
                    }
                },
                Qt::QueuedConnection);
            if (!callbackScheduled) {
                SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.schedule_callback", 1);
            }
        },
        Qt::QueuedConnection);
    SNOW_SHOT_CLIPBOARD_PERF_COUNTER(
        scheduled ? "export.clipboard_request_scheduled" : "export.failure.schedule_worker", 1);
    return scheduled;
}

std::optional<ScreenshotPinnedSelectionRequest>
ScreenshotExportService::preparePinnedSelection(const QRect& selection,
                                                const ScreenshotResultStyle& style) const {
    if (selection.isEmpty()) {
        return std::nullopt;
    }
    SNOW_SHOT_PIN_PERF_SCOPE("export.prepare_pin_plan");
    ScreenshotPinnedSelectionRequest request = screenshotSelectionPinRequest(
        m_context.displaySession, m_context.geometry, selection, style);
    if (!request.isPrepared()) {
        return std::nullopt;
    }
    return request;
}

bool ScreenshotExportService::schedulePinnedSelection(ScreenshotPinnedSelectionRequest request,
                                                      QObject* receiver,
                                                      PinRequestCallback callback) {
    if (!request.isPrepared() || receiver == nullptr || !callback || m_worker == nullptr ||
        m_thread == nullptr || !m_thread->isRunning() || m_completionContext == nullptr) {
        return false;
    }

    const QPointer<QObject> guardedReceiver(receiver);
    QList<CanvasExportSource> sources =
        exportSourcesForSelection(m_context.displaySession, request.selection);
    if (sources.isEmpty()) {
        return false;
    }

    const auto smartErase = m_context.runtime.smartEraseSnapshot();
    QByteArray documentSession;
    {
        SNOW_SHOT_PIN_PERF_SCOPE("export.serialize_document");
        documentSession = m_context.runtime.serializeDocumentSession();
        if (documentSession.isEmpty()) {
            return false;
        }
    }

    auto* worker = static_cast<ScreenshotExportWorker*>(m_worker);
    const auto renderSpec =
        screenshotSelectionRenderSpec(m_context.displaySession, request.selection);
    if (!renderSpec.isValid())
        return false;
    const auto resultState = std::make_shared<ScreenshotPinnedSelectionResultHandle::State>();
    const QPointer<ScreenshotExportWorker> guardedWorker(worker);
    const bool scheduled = QMetaObject::invokeMethod(
        worker,
        [guardedWorker, resultState, documentSession = std::move(documentSession), smartErase,
         sources = std::move(sources), selection = request.selection, style = request.resultStyle,
         renderSpec]() mutable {
            SNOW_SHOT_PIN_PERF_SCOPE("export.worker_callback");
            if (guardedWorker.isNull() || resultState->isCancelled()) {
                return;
            }
            SNOW_SHOT_PIN_PERF_MILESTONE("export.render_started");
            QImage image = guardedWorker->renderSelection(documentSession, smartErase, selection,
                                                          style, sources, renderSpec);
            SNOW_SHOT_PIN_PERF_MILESTONE("export.render_finished");
            SNOW_SHOT_PIN_PERF_MILESTONE("export.result_published");
            const bool succeeded = !image.isNull();
            resultState->publish(succeeded, std::move(image));
        },
        Qt::QueuedConnection);
    if (!scheduled) {
        return false;
    }

    SNOW_SHOT_PIN_PERF_MILESTONE("export.dispatch_started");
    SNOW_SHOT_PIN_PERF_COUNTER("export.pin_dispatch_count", 1);
    if (guardedReceiver.isNull()) {
        resultState->cancel();
        return false;
    }

    SNOW_SHOT_PIN_PERF_SCOPE("export.pin_callback");
    callback(std::move(request), ScreenshotPinnedSelectionResultHandle(resultState));
    return true;
}
