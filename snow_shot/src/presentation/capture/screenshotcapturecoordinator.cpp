#include "snow_shot/presentation/screenshotcapturecoordinator.h"

#include "screenshotcaptureworker.h"

#include "snow_capture.h"

#include <QMetaObject>
#include <QPointer>
#include <QThread>

#include <utility>

struct ScreenshotCaptureCoordinator::CancellationState final {
    CancellationState() : token(snow_capture_cancellation_token_create()) {}

    ~CancellationState() {
        if (token != nullptr) {
            snow_capture_cancellation_token_destroy(token);
        }
    }

    void cancel() const {
        if (token != nullptr) {
            snow_capture_cancellation_token_cancel(token);
        }
    }

    SnowCaptureCancellationToken* token = nullptr;
};

ScreenshotCaptureCoordinator::ScreenshotCaptureCoordinator(QObject* parent) : QObject(parent) {}

ScreenshotCaptureCoordinator::~ScreenshotCaptureCoordinator() {
    shutdown();
}

bool ScreenshotCaptureCoordinator::hasWorker() const {
    return m_worker != nullptr;
}

void ScreenshotCaptureCoordinator::prepareAsync(quint64 requestId) {
    const QPointer<ScreenshotCaptureCoordinator> coordinator(this);
    static_cast<void>(postWorkerTask([coordinator, requestId](ScreenshotCaptureWorker& worker) {
        worker.prepare(requestId, coordinator);
    }));
}

void ScreenshotCaptureCoordinator::refreshLayoutAsync(quint64 requestId) {
    const QPointer<ScreenshotCaptureCoordinator> coordinator(this);
    static_cast<void>(postWorkerTask([requestId, coordinator](ScreenshotCaptureWorker& worker) {
        worker.refreshLayout(requestId, coordinator);
    }));
}

void ScreenshotCaptureCoordinator::captureAsync(const ScreenshotCaptureRequest& request) {
    cancelActiveCapture();
    auto cancellation = std::make_shared<CancellationState>();
    if (cancellation->token == nullptr) {
        ScreenshotCaptureResult result;
        result.requestId = request.requestId;
        result.errorMessage = QStringLiteral("Failed to create capture cancellation token");
        emit captureFinished(std::move(result));
        return;
    }
    m_activeCancellation = cancellation;
    const QPointer<ScreenshotCaptureCoordinator> coordinator(this);
    if (!postWorkerTask([coordinator, request, cancellation](ScreenshotCaptureWorker& worker) {
            worker.capture(request, coordinator, cancellation->token);
        })) {
        cancellation->cancel();
        ScreenshotCaptureResult result;
        result.requestId = request.requestId;
        result.errorMessage = QStringLiteral("Capture worker is unavailable");
        emit captureFinished(std::move(result));
    }
}

void ScreenshotCaptureCoordinator::cancelActiveCapture() {
    if (m_activeCancellation != nullptr) {
        m_activeCancellation->cancel();
        m_activeCancellation.reset();
    }
}

void ScreenshotCaptureCoordinator::releaseIdleResourcesAsync(quint64 requestId) {
    if (!hasWorker()) {
        return;
    }

    static_cast<void>(postWorkerTask(
        [requestId](ScreenshotCaptureWorker& worker) { worker.releaseIdleResources(requestId); }));
}

void ScreenshotCaptureCoordinator::shutdown() {
    cancelActiveCapture();
    if (m_thread == nullptr) {
        m_worker = nullptr;
        return;
    }

    m_thread->quit();
    m_thread->wait();
    delete m_thread;
    m_thread = nullptr;
    m_worker = nullptr;
}

void ScreenshotCaptureCoordinator::ensureWorker() {
    if (hasWorker()) {
        return;
    }

    shutdown();
    m_thread = new QThread(this);
    m_thread->setObjectName(QStringLiteral("snow-shot-capture"));
    m_worker = new ScreenshotCaptureWorker;
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread->start();
}

template <typename Task> bool ScreenshotCaptureCoordinator::postWorkerTask(Task&& task) {
    ensureWorker();
    if (!hasWorker()) {
        return false;
    }

    const QPointer<ScreenshotCaptureWorker> worker(m_worker);
    return QMetaObject::invokeMethod(
        m_worker,
        [worker, task = std::forward<Task>(task)]() mutable {
            if (!worker.isNull()) {
                task(*worker);
            }
        },
        Qt::QueuedConnection);
}
