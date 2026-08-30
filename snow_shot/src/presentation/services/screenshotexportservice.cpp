#include "snow_shot/presentation/screenshotexportservice.h"

#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"

#include "screenshotclipboardperfinstrumentation.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

#include "snow_draw_engine_qt/snow_canvas_runtime.h"

#include <QList>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QScreen>
#include <QThread>

#include <algorithm>
#include <optional>
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
    const QRect& selection, const ScreenshotResultStyle& style) {
    ScreenshotPinnedSelectionRequest request;
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
    request.selection = selection;
    request.geometry = placement.geometry;
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
    auto* worker = static_cast<ScreenshotExportWorker*>(m_worker);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<QObject> guardedCompletionContext(m_completionContext);
    const snow_shot::presentation::clipboard_perf::Stopwatch workerQueueTimer;
    bool scheduled = false;
    {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("export.schedule_worker");
        scheduled = QMetaObject::invokeMethod(
            worker,
            [worker, guardedReceiver, guardedCompletionContext, documentSession, selection, style,
             sources, directSourceImage, requestTimer, workerQueueTimer,
         callback = std::move(callback)]() mutable {
            snow_shot::presentation::clipboard_perf::duration(
                "export.worker_queue_delay", workerQueueTimer.elapsedNanoseconds());
            QImage image = worker->renderSelection(documentSession, selection, style, sources,
                                                   directSourceImage);
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

    auto* worker = static_cast<ScreenshotExportWorker*>(m_worker);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<QObject> guardedCompletionContext(m_completionContext);
    const snow_shot::presentation::clipboard_perf::Stopwatch workerQueueTimer;
    const bool scheduled = QMetaObject::invokeMethod(
        worker,
        [worker, guardedReceiver, guardedCompletionContext, documentSession, selection, style,
         sources, directSourceImage, requestTimer, workerQueueTimer,
         callback = std::move(callback)]() mutable {
            snow_shot::presentation::clipboard_perf::duration(
                "export.worker_queue_delay", workerQueueTimer.elapsedNanoseconds());
            auto result = std::make_shared<ScreenshotSelectionClipboardResult>(
                worker->prepareSelectionClipboard(documentSession, selection, style, sources,
                                                  directSourceImage));
            if (guardedReceiver.isNull() || guardedCompletionContext.isNull()) {
                SNOW_SHOT_CLIPBOARD_PERF_COUNTER("export.failure.receiver_destroyed", 1);
                return;
            }
            const snow_shot::presentation::clipboard_perf::Stopwatch callbackQueueTimer;
            const bool callbackScheduled = QMetaObject::invokeMethod(
                guardedCompletionContext,
                [guardedReceiver, guardedCompletionContext, result,
                 callback = std::move(callback), requestTimer, callbackQueueTimer]() mutable {
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
    ScreenshotPinnedSelectionRequest request = preparePinnedSelectionRequest(
        m_context.displaySession, m_context.geometry, selection, style);
    if (!request.isPrepared()) {
        return std::nullopt;
    }
    return request;
}

bool ScreenshotExportService::schedulePinnedSelection(
    ScreenshotPinnedSelectionRequest request, QObject* receiver,
    PinRequestCallback callback) {
    if (!request.isPrepared() || receiver == nullptr || !callback || m_worker == nullptr ||
        m_thread == nullptr || !m_thread->isRunning() || m_completionContext == nullptr) {
        return false;
    }

    QByteArray documentSession;
    QImage directSourceImage = std::exchange(m_nextSelectionSourceImage, QImage());
    if (directSourceImage.isNull()) {
        SNOW_SHOT_PIN_PERF_SCOPE("export.serialize_pin_document");
        documentSession = m_context.runtime.serializeDocumentSession();
    }
    QList<CanvasExportSource> sources;
    if (directSourceImage.isNull()) {
        sources = exportSourcesForSelection(m_context.displaySession, request.selection);
    }
    if (directSourceImage.isNull() && (documentSession.isEmpty() || sources.isEmpty())) {
        return false;
    }

    auto* worker = static_cast<ScreenshotExportWorker*>(m_worker);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<QObject> guardedCompletionContext(m_completionContext);
    return QMetaObject::invokeMethod(
        worker,
        [worker, guardedReceiver, guardedCompletionContext, documentSession,
         sources = std::move(sources), directSourceImage = std::move(directSourceImage),
         request = std::move(request), callback = std::move(callback)]() mutable {
            request.image = worker->renderSelection(documentSession, request.selection,
                                                    request.resultStyle, sources,
                                                    directSourceImage);
            if (guardedReceiver.isNull() || guardedCompletionContext.isNull()) {
                return;
            }
            const bool callbackScheduled = QMetaObject::invokeMethod(
                guardedCompletionContext,
                [guardedReceiver, guardedCompletionContext, request = std::move(request),
                 callback = std::move(callback)]() mutable {
                    SNOW_SHOT_PIN_PERF_SCOPE("export.pin_callback");
                    if (!guardedReceiver.isNull() && !guardedCompletionContext.isNull()) {
                        if (request.isValid()) {
                            SNOW_SHOT_PIN_PERF_COUNTER("source.mode.materialized", 1);
                            SNOW_SHOT_PIN_PERF_COUNTER("source.retained_bytes",
                                                       request.image.sizeInBytes());
                        }
                        callback(std::move(request));
                    }
                },
                Qt::QueuedConnection);
            if (!callbackScheduled) {
                SNOW_SHOT_PIN_PERF_COUNTER("export.failure.schedule_callback", 1);
            }
        },
        Qt::QueuedConnection);
}
