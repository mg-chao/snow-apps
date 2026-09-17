#include "snow_shot/presentation/screenshotocrrecognitionservice.h"
#include "screenshotocrtransport.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"
#include "snow_shot/diagnostics/diagnostics.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace {
using namespace snow_shot::ocr::protocol;

QPolygonF quadFromValues(const float* points, const QRectF& canvasRect, const QSize& imageSize) {
    const qreal scaleX = imageSize.width() > 0 ? canvasRect.width() / imageSize.width() : 1.0;
    const qreal scaleY = imageSize.height() > 0 ? canvasRect.height() / imageSize.height() : 1.0;
    QPolygonF polygon;
    polygon.reserve(4);
    for (int index = 0; index < 4; ++index) {
        polygon.push_back(
            QPointF(canvasRect.left() + static_cast<qreal>(points[index * 2]) * scaleX,
                    canvasRect.top() + static_cast<qreal>(points[index * 2 + 1]) * scaleY));
    }
    return polygon;
}

qreal edgeLength(const QPointF& first, const QPointF& second) {
    return std::hypot(second.x() - first.x(), second.y() - first.y());
}

ScreenshotOcrTextDirection textDirectionForQuad(const QPolygonF& quad) {
    if (quad.size() != 4)
        return ScreenshotOcrTextDirection::Horizontal;
    const qreal width =
        std::max(edgeLength(quad.at(0), quad.at(1)), edgeLength(quad.at(3), quad.at(2)));
    const qreal height =
        std::max(edgeLength(quad.at(0), quad.at(3)), edgeLength(quad.at(1), quad.at(2)));
    return height >= width * 1.5 ? ScreenshotOcrTextDirection::Vertical
                                 : ScreenshotOcrTextDirection::Horizontal;
}

QImage renderFilteredImage(QImage source, const QRectF& canvasRect,
                           const std::shared_ptr<ScreenshotOcrPresentation>& presentation,
                           const QColor& backgroundColor, SnowCanvasRegionFilterScratch* scratch,
                           QRectF* filteredImageCanvasRect) {
    if (filteredImageCanvasRect != nullptr)
        *filteredImageCanvasRect = {};
    if (source.isNull() || presentation == nullptr || !canvasRect.isValid() || canvasRect.isEmpty())
        return {};
    source.setDevicePixelRatio(1.0);
    const QRectF normalized = canvasRect.normalized();
    QRect filteredPixels;
    QImage filtered = renderScreenshotOcrFilteredImage(
        source, normalized, *presentation, backgroundColor,
        std::max<qreal>(1.0, source.width() / normalized.width()), &filteredPixels, scratch);
    if (filteredImageCanvasRect != nullptr) {
        *filteredImageCanvasRect =
            screenshotOcrFilteredImageCanvasRect(normalized, source.size(), filteredPixels);
    }
    return filtered;
}
using snow_shot::ocr::ScreenshotOcrTransport;

} // namespace

class ScreenshotOcrRecognitionService::Impl final {
  public:
    Impl(ScreenshotOcrRecognitionService* owner, const Options& options,
         ScreenshotOcrBackendPreference preference)
        : m_owner(owner),
          m_shutdownTimeoutMilliseconds(std::max(1, options.shutdownTimeoutMilliseconds)),
          m_retryTimeUnit(std::max(1, options.retryTimeUnitMilliseconds)),
          m_beforeLocalRender(options.beforeLocalRender), m_proxyUrl(options.proxyUrl),
          m_modelType(options.modelType), m_backendPreference(preference) {
        m_configuration.modelType = options.modelType;
        m_configuration.backend = preference;
        m_queueClock.start();
        m_transportThread.setObjectName(QStringLiteral("snow-ocr-transport"));
        m_localPool->setMaxThreadCount(1);
        if (!options.processPath.trimmed().isEmpty() &&
            !options.detectorModelPath.trimmed().isEmpty() &&
            !options.recognizerModelPath.trimmed().isEmpty() &&
            !options.dictionaryPath.trimmed().isEmpty()) {
            m_assets.runtimeVersion = QString::fromLatin1(kRuntimeVersion);
            m_assets.modelType = options.modelType;
            m_assets.modelId = screenshotOcrModelTypeValue(options.modelType);
            m_assets.processPath = options.processPath;
            m_assets.runtimeDirectory = QFileInfo(options.processPath).absolutePath();
            m_assets.detectorModelPath = options.detectorModelPath;
            m_assets.recognizerModelPath = options.recognizerModelPath;
            m_assets.dictionaryPath = options.dictionaryPath;
            m_assets.stateDirectory = options.stateDirectory;
            m_assetStatus = {ScreenshotOcrAssetPhase::ReadyCached, QStringLiteral("assets")};
        } else {
            const QString offlineRoot = options.offlineRoot.trimmed().isEmpty()
                                            ? QDir(QCoreApplication::applicationDirPath())
                                                  .filePath(QStringLiteral("assets/ocr"))
                                            : options.offlineRoot;
            m_assetManager = std::make_unique<ScreenshotOcrAssets>(
                ScreenshotOcrAssets::Options{offlineRoot, options.cacheRoot, options.proxyUrl,
                                             options.modelType},
                owner);
            connect(m_assetManager.get(), &ScreenshotOcrAssets::statusChanged, owner,
                    [this](const ScreenshotOcrAssetStatus& status) { m_assetStatus = status; });
            connect(m_assetManager.get(), &ScreenshotOcrAssets::ready, owner,
                    [this](const ScreenshotOcrResolvedAssets& assets) {
                        m_assetPreparing = false;
                        if (assets.modelType != m_modelType)
                            return;
                        m_assets = assets;
                        m_assetRetryCount = 0;
                        scheduleReconcile();
                    });
            connect(m_assetManager.get(), &ScreenshotOcrAssets::failed, owner,
                    [this](const QString& error) {
                        m_assetPreparing = false;
                        qWarning().noquote() << "OCR asset preparation failed:" << error;
                        failPendingForAssetError();
                        scheduleAssetRetry();
                    });
        }
    }

    ~Impl() {
        shutdown();
    }

    RequestToken enqueue(RequestToken token, ScreenshotOcrRequest request, QObject* receiver,
                         Completion completion) {
        auto job = makeJob(token, std::move(request), receiver, std::move(completion));
        {
            if (m_stopping) {
                QObject::disconnect(job->receiverDestroyed);
                return 0;
            }
            if (m_jobs.size() >= kMaximumOutstandingRequests) {
                QObject::disconnect(job->receiverDestroyed);
                return 0;
            }
            qsizetype receiverRequests = 0;
            for (auto it = m_jobs.cbegin(); it != m_jobs.cend(); ++it) {
                if (it.value()->receiver == receiver) {
                    ++receiverRequests;
                }
            }
            if (receiverRequests >= kMaximumOutstandingRequestsPerReceiver) {
                QObject::disconnect(job->receiverDestroyed);
                return 0;
            }
            job->queuedAtMilliseconds = m_queueClock.elapsed();
            if (m_pending.empty() && m_runningCount == 0)
                ++m_idleGeneration;
            m_jobs.insert(token, job);
            m_pending.push_back(job);
        }
        m_deliveryQueue.push_back(job);
        if (job->request.priority == ScreenshotOcrRequestPriority::Interactive)
            rearmResidency();
        if (m_sessionState == SessionState::Failed)
            m_sessionState = SessionState::Empty;
        if (!screenshotOcrImageWithinPixelLimit(job->imageSize)) {
            failJob(job, QCoreApplication::translate("ScreenshotOcrController",
                                                     "Text recognition failed"));
        }
        scheduleReconcile();
        return token;
    }

    RequestToken render(RequestToken token, ScreenshotOcrRequest request, QObject* receiver,
                        Completion completion) {
        auto job = makeJob(token, std::move(request), receiver, std::move(completion));
        {
            if (m_stopping) {
                QObject::disconnect(job->receiverDestroyed);
                return 0;
            }
            if (m_jobs.size() >= kMaximumOutstandingRequests) {
                QObject::disconnect(job->receiverDestroyed);
                return 0;
            }
            qsizetype receiverRequests = 0;
            for (auto it = m_jobs.cbegin(); it != m_jobs.cend(); ++it) {
                if (it.value()->receiver == receiver) {
                    ++receiverRequests;
                }
            }
            if (receiverRequests >= kMaximumOutstandingRequestsPerReceiver) {
                QObject::disconnect(job->receiverDestroyed);
                return 0;
            }
            m_jobs.insert(token, job);
            job->running = true;
            job->localRendering = true;
            job->submittedAt = 0;
            job->completedAt = 0;
            ++m_localRenderingCount;
        }
        const QPointer<ScreenshotOcrRecognitionService> service(m_owner);
        const auto alive = m_alive;
        m_localPool->start(
            QRunnable::create([service, job, alive, beforeRender = m_beforeLocalRender]() {
                if (alive->load(std::memory_order_acquire) && beforeRender)
                    beforeRender();
                SnowCanvasRegionFilterScratch scratch;
                ScreenshotOcrRecognitionResult result;
                if (alive->load(std::memory_order_acquire) &&
                    !job->cancelled.load(std::memory_order_acquire)) {
                    result.filteredImage =
                        renderFilteredImage(std::move(job->request.image), job->request.canvasRect,
                                            job->request.presentation, job->request.backgroundColor,
                                            &scratch, &result.filteredImageCanvasRect);
                    if (result.filteredImage.isNull() && job->request.presentation != nullptr) {
                        result.error = QCoreApplication::translate("ScreenshotOcrController",
                                                                   "Text recognition failed");
                    }
                }
                QMetaObject::invokeMethod(
                    service,
                    [service, job, alive, result = std::move(result)]() mutable {
                        if (alive->load(std::memory_order_acquire) && service != nullptr &&
                            service->m_impl != nullptr) {
                            service->m_impl->finishLocalJob(job, std::move(result));
                        }
                    },
                    Qt::QueuedConnection);
            }));
        return token;
    }

    void cancel(RequestToken token) {
        auto it = m_jobs.find(token);
        if (it == m_jobs.end() || it.value()->cancelled.exchange(true))
            return;
        const auto job = it.value();
        auto fields = jobFields(job);
        fields.insert(QStringLiteral("outcome"), QStringLiteral("cancelled"));
        fields.insert(QStringLiteral("stage"), job->processSubmitted ? QStringLiteral("worker")
                                               : job->localRendering ? QStringLiteral("render")
                                                                     : QStringLiteral("queue"));
        snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.ocr"),
                                         QStringLiteral("ocr.cancelled"), fields);
        m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), job), m_pending.end());
        if (m_staged == job) {
            sendFrame(makeFrame(kDiscardImage, token));
            m_staged.reset();
        }
        if (job->processSubmitted) {
            sendFrame(makeFrame(kCancel, token));
            if (!m_configuration.residentProcess && !job->residentSubmission && m_pending.empty()) {
                m_processState = ProcessState::Stopping;

                m_processStopReason = ProcessStopReason::Cancelled;
                m_active.reset();
                m_runningCount = 0;
                job->running = job->processSubmitted = false;
                stopTransport(true);
            }
        }
        QObject::disconnect(job->receiverDestroyed);
        if (!job->request.renderOnly) {
            job->result.emplace();
            drainCompletions();
        }
        if (!job->running && !job->localRendering && m_transferring != job)
            m_jobs.remove(token);
        scheduleReconcile();
    }

    bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) {
        auto it = m_jobs.find(token);
        if (it == m_jobs.end() || it.value()->cancelled.load())
            return false;
        it.value()->request.priority = priority;
        if (priority == ScreenshotOcrRequestPriority::Interactive)
            rearmResidency();
        scheduleReconcile();
        return true;
    }

    bool setRenderFilteredImage(RequestToken token, bool enabled, const QColor& backgroundColor) {
        auto it = m_jobs.find(token);
        if (it == m_jobs.end() || it.value()->request.renderOnly || it.value()->localRendering ||
            it.value()->cancelled.load())
            return false;
        it.value()->request.renderFilteredImage = enabled;
        it.value()->request.backgroundColor = enabled ? backgroundColor : QColor();
        return true;
    }

    void setBackendPreference(ScreenshotOcrBackendPreference preference) {
        auto configuration = m_configuration;
        configuration.backend = preference;
        setRuntimeConfiguration(configuration);
    }

    void setProxyUrl(const QString& proxyUrl) {
        if (m_proxyUrl == proxyUrl)
            return;
        m_proxyUrl = proxyUrl;
        if (m_assetManager != nullptr)
            m_assetManager->setProxyUrl(proxyUrl);
    }

    void setModelType(ScreenshotOcrModelType modelType) {
        auto configuration = m_configuration;
        configuration.modelType = modelType;
        setRuntimeConfiguration(configuration);
    }

    void setRuntimeConfiguration(const ScreenshotOcrRuntimeConfiguration& configuration) {
        if (m_configuration == configuration)
            return;
        const bool modelChanged = m_modelType != configuration.modelType;
        const bool sessionChanged = modelChanged || m_backendPreference != configuration.backend;
        if (sessionChanged)
            ++m_configurationGeneration;
        if (m_configuration.modelHotStart &&
            (!configuration.modelHotStart || !configuration.residentProcess))
            m_invalidateWarmSession = true;
        m_configuration = configuration;
        m_modelType = configuration.modelType;
        m_backendPreference = configuration.backend;
        if (modelChanged) {
            if (m_assetManager != nullptr) {
                m_assetManager->setModelType(m_modelType);
                m_assetPreparing = false;
            } else {
                m_assets.modelType = m_modelType;
                m_assets.modelId = screenshotOcrModelTypeValue(m_modelType);
            }
        }
        ++m_retryGeneration;
        m_retryPending = false;
        m_assetRetryCount = 0;
        if (m_sessionState == SessionState::Failed)
            m_sessionState = SessionState::Empty;
        scheduleReconcile();
    }

    int liveWorkerCount() const {
        return m_transport != nullptr && !processStopping()
                   ? std::min(1, m_runningCount + static_cast<int>(m_pending.size()))
                   : 0;
    }

    qint64 processId() const {
        return m_transport != nullptr ? m_childPid : 0;
    }
    QString processPath() const {
        return m_transport != nullptr ? m_assets.processPath : QString();
    }

    bool modelFilesReady() const {
        return assetsReady();
    }
    ScreenshotOcrAssetStatus assetStatus() const {
        return m_assetStatus;
    }

  private:
    enum class ProcessStopReason { None, Cancelled, Shutdown };
    enum class ProcessState { Absent, Starting, Ready, Stopping };
    enum class SessionState { Empty, Preparing, Ready, Releasing, Failed };
    enum class BufferState { Empty, Attaching, Ready, Detaching };

    struct Job {
        Job() {
            elapsed.start();
        }
        QElapsedTimer elapsed;
        RequestToken token = 0;
        ScreenshotOcrRequest request;
        QSize imageSize;
        QPointer<QObject> receiver;
        Completion completion;
        QMetaObject::Connection receiverDestroyed;
        std::atomic_bool cancelled{false};
        bool running = false;
        bool processSubmitted = false;
        bool localRendering = false;
        qint64 submittedAt = -1;
        qint64 completedAt = -1;
        qint64 childPid = 0;
        qint64 queuedAtMilliseconds = 0;
        bool residentSubmission = false;
        std::optional<ScreenshotOcrRecognitionResult> result;
    };

    std::shared_ptr<Job> makeJob(RequestToken token, ScreenshotOcrRequest request,
                                 QObject* receiver, Completion completion) {
        auto job = std::make_shared<Job>();
        job->token = token;
        job->imageSize = request.image.size();
        job->request = std::move(request);
        job->receiver = receiver;
        job->completion = std::move(completion);
        QPointer<ScreenshotOcrRecognitionService> service(m_owner);
        job->receiverDestroyed =
            QObject::connect(receiver, &QObject::destroyed, m_owner, [service, token]() {
                if (service != nullptr)
                    service->cancel(token);
            });
        return job;
    }

    bool ensureProcess() {
        if (!assetsReady() || m_stopping)
            return false;
        if (m_transport != nullptr)
            return !processStopping();
        if (m_pending.empty() && !residentEffective())
            return true;
        m_processState = ProcessState::Starting;
        m_slotBytes = 0;
        m_sessionState = SessionState::Empty;
        m_bufferState = BufferState::Empty;
        m_sessionUsed = false;
        m_staged.reset();
        m_transferring.reset();
        m_readBuffer.clear();
        m_stderrBuffer.clear();
        m_childPid = 0;

        m_processStopReason = ProcessStopReason::None;
        const quint64 generation = ++m_processGeneration;
        const auto alive = m_alive;
        const auto valid = [this, alive, generation]() {
            return alive->load(std::memory_order_acquire) && generation == m_processGeneration;
        };
        m_transport = new ScreenshotOcrTransport(
            m_owner,
            {[this, valid](qint64 pid) {
                 if (!valid())
                     return;
                 m_childPid = pid;
                 snow_shot::diagnostics::logEvent(
                     QStringLiteral("snow_shot.ocr"), QStringLiteral("ocr.process_started"),
                     {{QStringLiteral("child_pid"), pid},
                      {QStringLiteral("slot_count"), 0},
                      {QStringLiteral("shared_memory_bytes"), qint64(0)}});
             },
             [this, valid](QByteArray bytes) {
                 if (valid())
                     readProcessOutput(bytes);
             },
             [this, valid](QByteArray bytes) {
                 if (valid())
                     relayStderr(bytes, m_childPid);
             },
             [this, valid](int code, QProcess::ExitStatus status) {
                 if (!valid())
                     return;
                 relayStderr({}, m_childPid, true);
                 const bool expected = m_processStopReason != ProcessStopReason::None;
                 const bool crashed = status == QProcess::CrashExit;
                 const QString outcome = m_processStopReason == ProcessStopReason::Cancelled
                                             ? QStringLiteral("cancelled")
                                         : expected ? QStringLiteral("shutdown")
                                         : crashed  ? QStringLiteral("crashed")
                                                    : QStringLiteral("exited");
                 snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.ocr"),
                                                  QStringLiteral("ocr.process_exit"),
                                                  {{QStringLiteral("exit_code"), code},
                                                   {QStringLiteral("outcome"), outcome},
                                                   {QStringLiteral("child_pid"), m_childPid}},
                                                  crashed && !expected ? QtCriticalMsg : QtInfoMsg);
                 snow_shot::diagnostics::DiagnosticsService::instance().requestMaintenance();
                 if (processStopping())
                     finishShutdown();
                 else
                     processFailed();
             },
             [this, valid](const QString& stage) {
                 if (valid())
                     processFailed(stage.toLatin1().constData());
             }});
        m_transport->moveToThread(&m_transportThread);
        if (!m_transportThread.isRunning())
            m_transportThread.start();
        const auto& diagnostics = snow_shot::diagnostics::DiagnosticsService::instance();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("SNOW_SHOT_CRASHPAD_PIPE"), diagnostics.crashPipeName());
        environment.insert(QStringLiteral("SNOW_SHOT_DIAGNOSTICS_SESSION"),
                           diagnostics.status().sessionId);
        QMetaObject::invokeMethod(
            m_transport,
            [transport = m_transport, assets = m_assets, environment]() {
                transport->start(assets, environment);
            },
            Qt::QueuedConnection);
        QTimer::singleShot(5000, m_owner, [this, valid]() {
            if (valid() && m_transport != nullptr && !processReady() && !processStopping())
                processFailed("ready_timeout");
        });
        return true;
    }

    void sendFrame(const QByteArray& frame) {
        if (m_transport != nullptr)
            QMetaObject::invokeMethod(
                m_transport, [transport = m_transport, frame]() { transport->send(frame); },
                Qt::QueuedConnection);
    }

    void stopTransport(bool force) {
        if (m_transport != nullptr)
            QMetaObject::invokeMethod(
                m_transport, [transport = m_transport, force]() { transport->stop(force); },
                Qt::QueuedConnection);
    }

    void releaseTransport() {
        if (m_transport != nullptr) {
            m_transport->deleteLater();
            m_transport = nullptr;
        }
        ++m_processGeneration;
    }

    void relayStderr(const QByteArray& bytes, qint64 pid, bool flush = false) {
        // QProcess chunks can split a UTF-8 character or one ONNX error across callbacks.
        m_stderrBuffer.append(bytes);
        while (!m_stderrBuffer.isEmpty()) {
            const qsizetype newline = m_stderrBuffer.indexOf('\n');
            if (newline < 0 && m_stderrBuffer.size() < 8192 && !flush)
                break;
            const qsizetype length = newline >= 0 ? qMin(newline, qsizetype(8192))
                                                  : qMin(m_stderrBuffer.size(), qsizetype(8192));
            const QByteArray line = m_stderrBuffer.left(length);
            m_stderrBuffer.remove(0, length + (newline == length ? 1 : 0));
            const QString output = QString::fromUtf8(line).trimmed();
            if (output.isEmpty())
                continue;
            const QJsonObject structured = QJsonDocument::fromJson(line).object();
            const QString event = structured.value(QStringLiteral("event")).toString();
            const bool workerEvent = event == QStringLiteral("ocr.worker_finished") ||
                                     event == QStringLiteral("ocr.backend_fallback") ||
                                     event == QStringLiteral("ocr.engine_ready");
            QJsonObject fields =
                workerEvent ? structured.value(QStringLiteral("fields")).toObject() : QJsonObject{};
            fields.insert(QStringLiteral("child_pid"), pid);
            const bool info =
                workerEvent && fields.value(QStringLiteral("outcome")) != QStringLiteral("failed");
            snow_shot::diagnostics::DiagnosticsService::instance().record(
                info || output == QStringLiteral("snow.diagnostics: crash capture registered")
                    ? QtInfoMsg
                    : QtWarningMsg,
                QStringLiteral("snow_shot.ocr"), workerEvent ? event : QStringLiteral("ocr.stderr"),
                workerEvent ? structured.value(QStringLiteral("message")).toString() : output,
                fields);
        }
    }

    QJsonObject jobFields(const std::shared_ptr<Job>& job) const {
        const qint64 now = job->elapsed.elapsed();
        return {{QStringLiteral("operation"), QString::number(job->token)},
                {QStringLiteral("child_pid"), job->childPid},
                {QStringLiteral("duration_ms"), now},
                {QStringLiteral("queue_ms"), job->submittedAt < 0 ? now : job->submittedAt},
                {QStringLiteral("worker_ms"),
                 job->submittedAt < 0
                     ? 0
                     : (job->completedAt < 0 ? now : job->completedAt) - job->submittedAt},
                {QStringLiteral("render_ms"), job->completedAt < 0 ? 0 : now - job->completedAt},
                {QStringLiteral("width"), job->imageSize.width()},
                {QStringLiteral("height"), job->imageSize.height()},
                {QStringLiteral("priority"),
                 job->request.priority == ScreenshotOcrRequestPriority::Interactive
                     ? QStringLiteral("interactive")
                     : QStringLiteral("prefetch")}};
    }

    bool processReady() const {
        return m_processState == ProcessState::Ready;
    }
    bool processStopping() const {
        return m_processState == ProcessState::Stopping;
    }
    bool residentEffective() const {
        return m_configuration.residentProcess && !m_residentSuspended;
    }
    bool hotEffective() const {
        return residentEffective() && m_configuration.modelHotStart;
    }
    void rearmResidency() {
        if (!m_residentSuspended && !m_retryPending)
            return;
        m_residentSuspended = false;
        m_crashes.clear();
        m_retryPending = false;
        ++m_retryGeneration;
    }
    void scheduleReconcile() {
        if (m_reconcileScheduled || m_stopping)
            return;
        m_reconcileScheduled = true;
        QTimer::singleShot(0, m_owner, [this]() {
            m_reconcileScheduled = false;
            reconcile();
        });
    }
    void scheduleRetry(int seconds) {
        if (!residentEffective() || m_stopping)
            return;
        m_retryPending = true;
        const auto generation = ++m_retryGeneration;
        QTimer::singleShot(seconds * m_retryTimeUnit, m_owner, [this, generation]() {
            if (generation != m_retryGeneration || !residentEffective() || m_stopping)
                return;
            m_retryPending = false;
            scheduleReconcile();
        });
    }
    void scheduleAssetRetry() {
        const int seconds = m_assetRetryCount == 0 ? 5 : m_assetRetryCount == 1 ? 15 : 60;
        ++m_assetRetryCount;
        scheduleRetry(seconds);
    }
    void prepareAssets() {
        if (m_assetManager == nullptr) {
            failPendingForAssetError();
            if (!m_retryPending)
                scheduleAssetRetry();
            return;
        }
        if (!m_assetPreparing) {
            m_assetPreparing = true;
            m_assetManager->prepare();
        }
    }
    void releaseSession() {
        m_sessionState = SessionState::Releasing;
        sendFrame(makeFrame(kReleaseSession, ++m_sessionOperation));
    }
    void prepareSession(bool warm) {
        m_sessionState = SessionState::Preparing;
        m_sessionGeneration = m_configurationGeneration;
        m_sessionWarm = warm;
        m_sessionIdleGeneration = m_idleGeneration;
        m_invalidateWarmSession = false;
        QByteArray payload;
        appendU8(payload, m_backendPreference == ScreenshotOcrBackendPreference::DirectMl ? 1 : 0);
        appendString(payload, m_assets.detectorModelPath);
        appendString(payload, m_assets.recognizerModelPath);
        appendString(payload, m_assets.dictionaryPath);
        sendFrame(makeFrame(kPrepareSession, ++m_sessionOperation, payload));
    }
    void releaseBuffer() {
        if (m_bufferState != BufferState::Ready || m_transferring != nullptr)
            return;
        m_bufferState = BufferState::Detaching;
        sendFrame(makeFrame(kDetachBuffer, m_bufferGeneration));
    }
    void flushPending() {
        scheduleReconcile();
    }

    void reconcile() {
        if (m_stopping || processStopping())
            return;
        const bool demand = !m_pending.empty() || m_runningCount != 0;
        if (!demand && !residentEffective()) {
            if (m_transport != nullptr) {
                m_processState = ProcessState::Stopping;

                m_processStopReason = ProcessStopReason::Shutdown;
                stopTransport(false);
            }
            return;
        }
        const bool undelivered =
            std::any_of(m_pending.begin(), m_pending.end(), [this](const auto& job) {
                return job != m_staged && !job->cancelled.load();
            });
        if (!undelivered && m_transferring == nullptr)
            releaseBuffer();
        if (m_retryPending && !demand)
            return;
        if (!assetsReady()) {
            // Old inference may drain while the next model is being acquired.
            if (m_runningCount == 0 && m_sessionState == SessionState::Ready)
                releaseSession();
            prepareAssets();
            return;
        }
        if (!ensureProcess() || !processReady())
            return;

        // Only one transfer may own the mapping, and only one copied image waits ahead.
        std::shared_ptr<Job> nextImage;
        qsizetype largest = 0;
        for (const auto& job : m_pending) {
            if (job == m_staged || job == m_transferring || job->cancelled.load())
                continue;
            if (nextImage == nullptr)
                nextImage = job;
            largest = std::max(largest, static_cast<qsizetype>(job->imageSize.width()) *
                                            job->imageSize.height() * 4);
        }
        if (m_transferring == nullptr) {
            if (nextImage == nullptr)
                releaseBuffer();
            else if (m_staged == nullptr) {
                if (m_bufferState == BufferState::Empty) {
                    m_slotBytes = kSlotHeaderBytes + largest;
                    m_bufferState = BufferState::Attaching;
                    ++m_bufferGeneration;
                    m_transferSequence = 0;
                    QMetaObject::invokeMethod(
                        m_transport,
                        [transport = m_transport, bytes = m_slotBytes,
                         generation = m_bufferGeneration]() {
                            transport->allocateBuffer(bytes, generation);
                        },
                        Qt::QueuedConnection);
                } else if (m_bufferState == BufferState::Ready) {
                    const qsizetype bytes = static_cast<qsizetype>(nextImage->imageSize.width()) *
                                            nextImage->imageSize.height() * 4;
                    if (bytes > m_slotBytes - kSlotHeaderBytes)
                        releaseBuffer();
                    else {
                        m_transferring = nextImage;
                        const auto sequence = ++m_transferSequence;
                        QMetaObject::invokeMethod(
                            m_transport,
                            [transport = m_transport, image = nextImage->request.image, sequence,
                             token = nextImage->token]() {
                                transport->submit(image, sequence, token);
                            },
                            Qt::QueuedConnection);
                    }
                }
            }
        }
        if (m_runningCount != 0 || m_sessionState == SessionState::Preparing ||
            m_sessionState == SessionState::Releasing)
            return;
        const bool idle = m_pending.empty();
        if (m_sessionState == SessionState::Ready &&
            (m_sessionGeneration != m_configurationGeneration ||
             (m_sessionWarm && m_invalidateWarmSession) ||
             (idle && (m_sessionUsed || !m_sessionWarm ||
                       m_sessionIdleGeneration != m_idleGeneration || !hotEffective())))) {
            releaseSession();
            return;
        }
        if (m_sessionState == SessionState::Empty && (!idle || hotEffective())) {
            // Idle warm-up begins only after the image mapping is completely detached.
            if (!idle || m_bufferState == BufferState::Empty)
                prepareSession(idle);
            return;
        }
        if (!idle && m_sessionState == SessionState::Ready && m_staged != nullptr) {
            const auto job = m_staged;
            if (m_pending.front() != job) {
                processFailed("queue_order");
                return;
            }
            m_pending.erase(m_pending.begin());
            m_staged.reset();
            m_active = job;
            job->running = job->processSubmitted = true;
            job->residentSubmission = m_configuration.residentProcess;
            job->submittedAt = job->elapsed.elapsed();
            job->childPid = m_childPid;
            m_runningCount = 1;
            m_sessionUsed = true;
            m_sessionWarm = false;
            auto fields = jobFields(job);
            fields.insert(QStringLiteral("pending_count"), static_cast<int>(m_pending.size()));
            snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.ocr"),
                                             QStringLiteral("ocr.submitted"), fields);
            sendFrame(makeFrame(kRecognize, job->token));
            scheduleReconcile();
        }
    }

    void readProcessOutput(const QByteArray& bytes) {
        const auto generation = m_processGeneration;
        m_readBuffer.append(bytes);
        while (m_readBuffer.size() >= 20) {
            const QByteArray magicBytes("SOCR", 4);
            const qsizetype magicPosition = m_readBuffer.indexOf(magicBytes);
            if (magicPosition < 0) {
                if (m_readBuffer.size() > 3)
                    m_readBuffer.remove(0, m_readBuffer.size() - 3);
                return;
            }
            if (magicPosition > 0)
                m_readBuffer.remove(0, magicPosition);
            if (m_readBuffer.size() < 20)
                return;
            const quint32 magic =
                static_cast<quint32>(static_cast<quint8>(m_readBuffer.at(0))) |
                (static_cast<quint32>(static_cast<quint8>(m_readBuffer.at(1))) << 8) |
                (static_cast<quint32>(static_cast<quint8>(m_readBuffer.at(2))) << 16) |
                (static_cast<quint32>(static_cast<quint8>(m_readBuffer.at(3))) << 24);
            const quint16 version = static_cast<quint16>(
                static_cast<quint8>(m_readBuffer.at(4)) |
                (static_cast<unsigned int>(static_cast<quint8>(m_readBuffer.at(5))) << 8));
            const quint16 kind = static_cast<quint16>(
                static_cast<quint8>(m_readBuffer.at(6)) |
                (static_cast<unsigned int>(static_cast<quint8>(m_readBuffer.at(7))) << 8));
            qsizetype offset = 8;
            quint64 id = 0;
            quint32 length = 0;
            if (!takeU64(m_readBuffer, offset, &id) || !takeU32(m_readBuffer, offset, &length) ||
                magic != kProtocolMagic || version != kProtocolVersion ||
                length > static_cast<quint32>(kMaximumFrameBytes)) {
                processFailed("protocol_frame");
                return;
            }
            if (m_readBuffer.size() < 20 + static_cast<qsizetype>(length))
                return;
            const QByteArray payload = m_readBuffer.mid(20, static_cast<qsizetype>(length));
            m_readBuffer.remove(0, 20 + static_cast<qsizetype>(length));
            if (kind == kReady)
                handleReady(payload);
            else if (kind == kComplete)
                handleComplete(id, payload);
            else
                handleControl(kind, id, payload);
            if (generation != m_processGeneration)
                return;
            // ShutdownAck precedes process exit. Keep ownership until finished().
        }
    }

    void handleControl(quint16 kind, quint64 id, const QByteArray& payload) {
        if (kind == kBufferAttached && id == m_bufferGeneration &&
            m_bufferState == BufferState::Attaching) {
            m_bufferState = BufferState::Ready;
        } else if (kind == kBufferDetached && id == m_bufferGeneration &&
                   m_bufferState == BufferState::Detaching) {
            QMetaObject::invokeMethod(
                m_transport, [transport = m_transport]() { transport->releaseBuffer(); },
                Qt::QueuedConnection);
            m_bufferState = BufferState::Empty;
            m_slotBytes = 0;
        } else if (kind == kImageConsumed && m_transferring != nullptr &&
                   id == m_transferring->token) {
            qsizetype offset = 0;
            quint64 generation = 0, sequence = 0;
            if (!takeU64(payload, offset, &generation) || !takeU64(payload, offset, &sequence) ||
                offset != payload.size() || generation != m_bufferGeneration ||
                sequence != m_transferSequence) {
                processFailed("transfer_ack");
                return;
            }
            if (m_transferring->cancelled.load() ||
                std::find(m_pending.begin(), m_pending.end(), m_transferring) == m_pending.end()) {
                sendFrame(makeFrame(kDiscardImage, id));
                m_jobs.remove(id);
            } else
                m_staged = m_transferring;
            m_transferring.reset();
        } else if (kind == kSessionReady && id == m_sessionOperation &&
                   m_sessionState == SessionState::Preparing) {
            if (payload.size() != 1) {
                processFailed("session_ack");
                return;
            }
            const bool ok = payload.at(0) == 1;
            m_sessionState = ok ? SessionState::Ready : SessionState::Failed;
            m_sessionUsed = false;
            if (!ok && !m_pending.empty())
                failPendingForAssetError();
        } else if (kind == kSessionReleased && id == m_sessionOperation &&
                   m_sessionState == SessionState::Releasing) {
            m_sessionState = SessionState::Empty;
            m_sessionUsed = false;
        } else if (kind != 7) {
            processFailed("control_ack");
            return;
        }
        scheduleReconcile();
    }

    void handleReady(const QByteArray& payload) {
        qsizetype offset = 0;
        quint8 ok = 0, directMl = 0;
        QString provider, runtimeVersion;
        quint32 protocolVersion = 0;
        if (!takeU8(payload, offset, &ok) || !takeU8(payload, offset, &directMl) ||
            !takeString(payload, offset, &provider) ||
            !takeString(payload, offset, &runtimeVersion) ||
            !takeU32(payload, offset, &protocolVersion) || offset != payload.size() || ok == 0 ||
            runtimeVersion != QString::fromLatin1(kRuntimeVersion) ||
            protocolVersion != kProtocolVersion) {
            processFailed("handshake");
            return;
        }
        snow_shot::diagnostics::logEvent(
            QStringLiteral("snow_shot.ocr"), QStringLiteral("ocr.process_ready"),
            {{QStringLiteral("backend"), provider},
             {QStringLiteral("version"), runtimeVersion},
             {QStringLiteral("child_pid"), m_childPid},
             {QStringLiteral("stage"), QStringLiteral("command_channel")},
             {QStringLiteral("outcome"), QStringLiteral("ready")}});

        m_processState = ProcessState::Ready;
        m_assetStatus = {m_assets.offline ? ScreenshotOcrAssetPhase::ReadyOffline
                                          : ScreenshotOcrAssetPhase::ReadyCached,
                         QStringLiteral("assets")};
        flushPending();
        maybeShutdownProcess();
    }

    void handleComplete(quint64 id, const QByteArray& payload) {
        if (processStopping())
            return;
        std::shared_ptr<Job> job;
        ScreenshotOcrRecognitionResult result;
        {
            auto it = m_jobs.find(id);
            if (it == m_jobs.end() || !it.value()->processSubmitted || m_active != it.value()) {
                processFailed("completion_ownership");
                return;
            }
            job = it.value();
            job->completedAt = job->elapsed.elapsed();
            qsizetype offset = 0;
            quint8 status = 0;
            if (!takeU8(payload, offset, &status)) {
                failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController",
                                                               "Text recognition failed"));
                return;
            }
            if (status == 0) {
                QString error;
                takeString(payload, offset, &error);
                result.error = error.isEmpty()
                                   ? QCoreApplication::translate("ScreenshotOcrController",
                                                                 "Text recognition failed")
                                   : error;
            } else if (status == 1) {
                QString ignored;
                quint32 lineCount = 0;
                if (!takeString(payload, offset, &ignored) ||
                    !takeU32(payload, offset, &lineCount) || lineCount > 100000) {
                    failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController",
                                                                   "Text recognition failed"));
                    return;
                }
                result.presentation = std::make_shared<ScreenshotOcrPresentation>();
                result.presentation->selection = job->request.canvasRect.toAlignedRect();
                result.presentation->lines.reserve(static_cast<qsizetype>(lineCount));
                for (quint32 index = 0; index < lineCount; ++index) {
                    QString text;
                    float confidence = 0.0F;
                    float points[8]{};
                    if (!takeString(payload, offset, &text) ||
                        !takeF32(payload, offset, &confidence)) {
                        failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController",
                                                                       "Text recognition failed"));
                        return;
                    }
                    for (float& point : points)
                        if (!takeF32(payload, offset, &point)) {
                            failJobLocked(job,
                                          QCoreApplication::translate("ScreenshotOcrController",
                                                                      "Text recognition failed"));
                            return;
                        }
                    ScreenshotOcrLine line;
                    line.text = std::move(text);
                    line.confidence = static_cast<qreal>(confidence);
                    line.quad =
                        quadFromValues(points, job->request.canvasRect, job->request.image.size());
                    line.direction = textDirectionForQuad(line.quad);
                    result.presentation->lines.push_back(std::move(line));
                }
                result.presentation->prepareForRendering();
            } else {
                result.error = QCoreApplication::translate("ScreenshotOcrController",
                                                           "Text recognition failed");
            }
            if (m_active == job)
                m_active.reset();
            job->running = false;
            job->processSubmitted = false;
            --m_runningCount;
            const bool renderFiltered = !job->cancelled.load() && result.error.isEmpty() &&
                                        result.presentation != nullptr &&
                                        job->request.renderFilteredImage;
            if (renderFiltered) {
                job->localRendering = true;
                ++m_localRenderingCount;
            }
        }
        if (!job->cancelled.load() && result.error.isEmpty() && result.presentation != nullptr &&
            job->request.renderFilteredImage) {
            const QImage source = job->request.image;
            const QRectF canvasRect = job->request.canvasRect;
            const QColor background = job->request.backgroundColor;
            const QPointer<ScreenshotOcrRecognitionService> service(m_owner);
            const auto alive = m_alive;
            m_localPool->start(QRunnable::create(
                [service, job, alive, beforeRender = m_beforeLocalRender,
                 result = std::move(result), source, canvasRect, background]() mutable {
                    if (alive->load(std::memory_order_acquire) && beforeRender)
                        beforeRender();
                    SnowCanvasRegionFilterScratch scratch;
                    if (alive->load(std::memory_order_acquire)) {
                        result.filteredImage =
                            renderFilteredImage(source, canvasRect, result.presentation, background,
                                                &scratch, &result.filteredImageCanvasRect);
                    }
                    QMetaObject::invokeMethod(
                        service,
                        [service, job, alive, result = std::move(result)]() mutable {
                            if (alive->load(std::memory_order_acquire) && service != nullptr &&
                                service->m_impl != nullptr) {
                                service->m_impl->finishLocalJob(job, std::move(result));
                            }
                        },
                        Qt::QueuedConnection);
                }));
        } else {
            deliver(job, std::move(result));
        }
        flushPending();
        maybeShutdownProcess();
    }

    void deliver(const std::shared_ptr<Job>& job, ScreenshotOcrRecognitionResult result) {
        auto fields = jobFields(job);
        fields.insert(QStringLiteral("outcome"), job->cancelled.load() ? QStringLiteral("cancelled")
                                                 : result.error.isEmpty()
                                                     ? QStringLiteral("succeeded")
                                                     : QStringLiteral("failed"));
        snow_shot::diagnostics::DiagnosticsService::instance().record(
            result.error.isEmpty() || job->cancelled.load() ? QtInfoMsg : QtWarningMsg,
            QStringLiteral("snow_shot.ocr"), QStringLiteral("ocr.finished"), result.error, fields);
        if (job->request.renderOnly) {
            QObject::disconnect(job->receiverDestroyed);
            m_jobs.remove(job->token);
            if (!job->cancelled.load() && job->receiver != nullptr && job->completion)
                job->completion(std::move(result));
            return;
        }
        job->result = std::move(result);
        drainCompletions();
        if (job->cancelled.load() && !job->running && !job->localRendering)
            m_jobs.remove(job->token);
    }

    void drainCompletions() {
        if (m_delivering)
            return;
        m_delivering = true;
        while (!m_deliveryQueue.empty() && m_deliveryQueue.front()->result.has_value()) {
            const auto job = m_deliveryQueue.front();
            m_deliveryQueue.pop_front();
            if (!job->running && !job->localRendering && m_transferring != job)
                m_jobs.remove(job->token);
            QObject::disconnect(job->receiverDestroyed);
            auto result = std::move(*job->result);
            job->result.reset();
            if (!job->cancelled.load() && job->receiver != nullptr && job->completion)
                job->completion(std::move(result));
        }
        m_delivering = false;
    }

    void finishLocalJob(const std::shared_ptr<Job>& job, ScreenshotOcrRecognitionResult result) {
        {
            auto it = m_jobs.find(job->token);
            if (it == m_jobs.end())
                return;
            job->localRendering = false;
            if (m_localRenderingCount > 0)
                --m_localRenderingCount;
        }
        deliver(job, std::move(result));
        maybeShutdownProcess();
    }

    void failJob(const std::shared_ptr<Job>& job, const QString& error) {
        {
            failJobLocked(job, error);
        }
        maybeShutdownProcess();
    }

    void failJobLocked(const std::shared_ptr<Job>& job, const QString& error) {
        m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), job), m_pending.end());
        if (m_staged == job) {
            sendFrame(makeFrame(kDiscardImage, job->token));
            m_staged.reset();
        }
        job->running = job->processSubmitted = false;
        if (m_active == job) {
            m_active.reset();
            m_runningCount = 0;
        }
        ScreenshotOcrRecognitionResult result;
        result.error = error;
        deliver(job, std::move(result));
        scheduleReconcile();
    }

    void processFailed(const char* stage = "process_exit") {
        if (m_transport == nullptr || m_stopping || processStopping())
            return;
        snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.ocr"),
                                         QStringLiteral("ocr.process_failed"),
                                         {{QStringLiteral("stage"), QString::fromLatin1(stage)},
                                          {QStringLiteral("child_pid"), m_childPid},
                                          {QStringLiteral("outcome"), QStringLiteral("failed")}},
                                         QtWarningMsg);
        std::vector<std::shared_ptr<Job>> failed;
        for (const auto& job : m_deliveryQueue) {
            if (!job->localRendering && !job->result.has_value())
                failed.push_back(job);
        }
        for (auto it = m_jobs.begin(); it != m_jobs.end();) {
            if (it.value()->cancelled.load() && !it.value()->localRendering)
                it = m_jobs.erase(it);
            else
                ++it;
        }
        m_pending.clear();
        m_staged.reset();
        m_transferring.reset();
        m_active.reset();
        m_runningCount = 0;

        releaseTransport();
        m_processState = ProcessState::Absent;
        m_sessionState = SessionState::Empty;
        m_bufferState = BufferState::Empty;
        m_slotBytes = 0;
        const qint64 now = m_queueClock.elapsed();
        while (!m_crashes.empty() && now - m_crashes.front() >= 60LL * m_retryTimeUnit)
            m_crashes.pop_front();
        m_crashes.push_back(now);
        if (m_configuration.residentProcess) {
            if (m_crashes.size() >= 3)
                m_residentSuspended = true;
            else
                scheduleRetry(static_cast<int>(m_crashes.size()));
        }
        for (const auto& job : failed) {
            job->childPid = m_childPid;
            failJobLocked(job, QCoreApplication::translate(
                                   "ScreenshotOcrController",
                                   "Text recognition components could not be prepared"));
        }
    }

    void failPendingForAssetError() {
        const QString message =
            m_assetStatus.error == QStringLiteral("bundled_runtime_invalid")
                ? QCoreApplication::translate(
                      "ScreenshotOcrController",
                      "The bundled text recognition runtime is damaged or incompatible. Reinstall "
                      "Snow Shot for Apple Silicon.")
                : QCoreApplication::translate("ScreenshotOcrController",
                                              "Text recognition components could not be prepared");
        const auto failed = m_pending;
        for (const auto& job : failed)
            failJobLocked(job, message);
    }

    bool assetsReady() const {
        return m_assets.valid() && m_assets.modelType == m_modelType;
    }

    void maybeShutdownProcess() {
        scheduleReconcile();
    }

    void finishShutdown() {
        releaseTransport();

        m_processState = ProcessState::Absent;
        m_sessionState = SessionState::Empty;
        m_bufferState = BufferState::Empty;
        m_staged.reset();
        m_transferring.reset();
        m_slotBytes = 0;
        scheduleReconcile();
    }

    void shutdown() {
        m_alive->store(false, std::memory_order_release);
        {
            m_stopping = true;
            for (const auto& job : std::as_const(m_jobs)) {
                job->cancelled.store(true, std::memory_order_release);
                QObject::disconnect(job->receiverDestroyed);
            }
            m_jobs.clear();
            m_pending.clear();
            m_localRenderingCount = 0;
            releaseTransport();
        }
        // Deleting the pool would wait for in-flight renders unconditionally;
        // orphan it past the deadline. Its runnables hold only shared state, a
        // QPointer to the service and the liveness flag, so completing after
        // destruction is safe.
        if (!m_localPool->waitForDone(m_shutdownTimeoutMilliseconds)) {
            qWarning("OCR render workers did not stop within %d milliseconds; abandoning them",
                     m_shutdownTimeoutMilliseconds);
            m_localPool.release();
        }
        m_transportThread.quit();
        if (!m_transportThread.wait(kTransportStopTimeoutMilliseconds)) {
            qWarning("OCR transport thread did not stop within %lu milliseconds; still waiting",
                     kTransportStopTimeoutMilliseconds);
            // A running QThread must never be destroyed, so keep waiting past
            // the deadline once the stall is visible in diagnostics.
            m_transportThread.wait();
        }
    }

    ScreenshotOcrRecognitionService* m_owner = nullptr;
    const int m_shutdownTimeoutMilliseconds;
    const int m_retryTimeUnit;
    const std::function<void()> m_beforeLocalRender;
    QString m_proxyUrl;
    ScreenshotOcrModelType m_modelType = ScreenshotOcrModelType::Small;
    ScreenshotOcrResolvedAssets m_assets;
    ScreenshotOcrAssetStatus m_assetStatus;
    std::unique_ptr<ScreenshotOcrAssets> m_assetManager;
    QHash<RequestToken, std::shared_ptr<Job>> m_jobs;
    std::vector<std::shared_ptr<Job>> m_pending;
    std::shared_ptr<Job> m_active;
    std::unique_ptr<QThreadPool> m_localPool = std::make_unique<QThreadPool>();
    QElapsedTimer m_queueClock;
    std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);
    QThread m_transportThread;
    ScreenshotOcrTransport* m_transport = nullptr;
    ProcessStopReason m_processStopReason = ProcessStopReason::None;
    qsizetype m_slotBytes = 0;
    int m_runningCount = 0, m_localRenderingCount = 0;
    ScreenshotOcrBackendPreference m_backendPreference = ScreenshotOcrBackendPreference::Cpu;
    QByteArray m_readBuffer;
    QByteArray m_stderrBuffer;
    qint64 m_childPid = 0;
    bool m_stopping = false;
    quint64 m_processGeneration = 0;
    ScreenshotOcrRuntimeConfiguration m_configuration;
    ProcessState m_processState = ProcessState::Absent;
    SessionState m_sessionState = SessionState::Empty;
    BufferState m_bufferState = BufferState::Empty;
    quint64 m_configurationGeneration = 1, m_sessionGeneration = 0, m_sessionOperation = 0;
    quint64 m_bufferGeneration = 0, m_transferSequence = 0, m_retryGeneration = 0;
    quint64 m_idleGeneration = 0, m_sessionIdleGeneration = 0;
    bool m_sessionUsed = false, m_sessionWarm = false, m_invalidateWarmSession = false;
    bool m_residentSuspended = false, m_retryPending = false, m_assetPreparing = false;
    bool m_reconcileScheduled = false, m_delivering = false;
    int m_assetRetryCount = 0;
    std::deque<qint64> m_crashes;
    std::shared_ptr<Job> m_transferring, m_staged;
    std::deque<std::shared_ptr<Job>> m_deliveryQueue;
};

ScreenshotOcrRecognitionService::ScreenshotOcrRecognitionService(QObject* parent)
    : ScreenshotOcrRecognitionService(Options{}, ScreenshotOcrBackendPreference::Cpu, parent) {}
ScreenshotOcrRecognitionService::ScreenshotOcrRecognitionService(
    const Options& options, ScreenshotOcrBackendPreference preference, QObject* parent)
    : ScreenshotOcrRecognitionPort(parent),
      m_impl(std::make_unique<Impl>(this, options, preference)) {}
ScreenshotOcrRecognitionService::~ScreenshotOcrRecognitionService() = default;

ScreenshotOcrRecognitionPort::RequestToken
ScreenshotOcrRecognitionService::recognize(ScreenshotOcrRequest request, QObject* receiver,
                                           Completion completion) {
    if (request.image.isNull() || !request.canvasRect.isValid() || request.canvasRect.isEmpty() ||
        receiver == nullptr || !completion || m_impl == nullptr)
        return 0;
    do {
        ++m_nextToken;
    } while (m_nextToken == 0);
    return m_impl->enqueue(m_nextToken, std::move(request), receiver, std::move(completion));
}
ScreenshotOcrRecognitionPort::RequestToken
ScreenshotOcrRecognitionService::render(ScreenshotOcrRequest request, QObject* receiver,
                                        Completion completion) {
    if (request.image.isNull() || !request.canvasRect.isValid() || request.canvasRect.isEmpty() ||
        request.presentation == nullptr || receiver == nullptr || !completion || m_impl == nullptr)
        return 0;
    request.renderOnly = true;
    request.renderFilteredImage = false;
    do {
        ++m_nextToken;
    } while (m_nextToken == 0);
    return m_impl->render(m_nextToken, std::move(request), receiver, std::move(completion));
}
bool ScreenshotOcrRecognitionService::setRenderFilteredImage(RequestToken token, bool enabled,
                                                             const QColor& backgroundColor) {
    return m_impl != nullptr && token != 0 &&
           m_impl->setRenderFilteredImage(token, enabled, backgroundColor);
}
void ScreenshotOcrRecognitionService::cancel(RequestToken token) {
    if (m_impl != nullptr && token != 0)
        m_impl->cancel(token);
}
bool ScreenshotOcrRecognitionService::reprioritize(RequestToken token,
                                                   ScreenshotOcrRequestPriority priority) {
    return m_impl != nullptr && token != 0 && m_impl->reprioritize(token, priority);
}
bool ScreenshotOcrRecognitionService::modelFilesReady() const {
    return m_impl != nullptr && m_impl->modelFilesReady();
}
ScreenshotOcrAssetStatus ScreenshotOcrRecognitionService::assetStatus() const {
    return m_impl != nullptr ? m_impl->assetStatus() : ScreenshotOcrAssetStatus{};
}
void ScreenshotOcrRecognitionService::setBackendPreference(
    ScreenshotOcrBackendPreference preference) {
    if (m_impl != nullptr)
        m_impl->setBackendPreference(preference);
}
void ScreenshotOcrRecognitionService::setProxyUrl(const QString& proxyUrl) {
    if (m_impl != nullptr)
        m_impl->setProxyUrl(proxyUrl);
}
void ScreenshotOcrRecognitionService::setModelType(ScreenshotOcrModelType modelType) {
    if (m_impl != nullptr)
        m_impl->setModelType(modelType);
}
int ScreenshotOcrRecognitionService::liveWorkerCount() const {
    return m_impl != nullptr ? m_impl->liveWorkerCount() : 0;
}

qint64 ScreenshotOcrRecognitionService::processId() const {
    return m_impl != nullptr ? m_impl->processId() : 0;
}
QString ScreenshotOcrRecognitionService::processPath() const {
    return m_impl != nullptr ? m_impl->processPath() : QString();
}

void ScreenshotOcrRecognitionService::setRuntimeConfiguration(
    const ScreenshotOcrRuntimeConfiguration& configuration) {
    if (m_impl != nullptr)
        m_impl->setRuntimeConfiguration(configuration);
}
