#include "snow_shot/presentation/screenshotocrrecognitionservice.h"

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
#include <QTemporaryFile>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace {
constexpr quint32 kProtocolMagic = 0x52434f53; // "SOCR" in little endian.
constexpr quint16 kProtocolVersion = 2;
constexpr auto kRuntimeVersion = "1.0.6";
constexpr quint16 kHello = 1;
constexpr quint16 kReady = 2;
constexpr quint16 kSubmit = 3;
constexpr quint16 kCancel = 4;
constexpr quint16 kComplete = 5;
constexpr quint16 kShutdown = 6;
constexpr qsizetype kSlotHeaderBytes = 32;
constexpr qsizetype kMaximumImageBytes = 3840LL * 2160LL * 4LL;
constexpr qsizetype kMaximumFrameBytes = 1024LL * 1024LL;
constexpr qsizetype kMaximumOutstandingRequests = 32;
constexpr qsizetype kMaximumOutstandingRequestsPerReceiver = 8;
constexpr qint64 kPrefetchAgingMilliseconds = 500;
constexpr qsizetype kSlotSequenceOffset = 0;
constexpr qsizetype kSlotStateOffset = 8;
constexpr qsizetype kSlotWidthOffset = 12;
constexpr qsizetype kSlotHeightOffset = 16;
constexpr qsizetype kSlotStrideOffset = 20;
constexpr qsizetype kSlotBytesOffset = 24;
constexpr qsizetype kSlotMagicOffset = 28;
constexpr quint32 kSlotFree = 0;
constexpr quint32 kSlotReady = 1;
constexpr quint32 kSlotMagic = 0x544f4c53; // "SLOT" in little endian.

void appendU8(QByteArray& bytes, quint8 value) {
    bytes.append(char(value));
}
void appendU32(QByteArray& bytes, quint32 value) {
    for (int index = 0; index < 4; ++index)
        bytes.append(char((value >> (index * 8)) & 0xff));
}
void appendU64(QByteArray& bytes, quint64 value) {
    for (int index = 0; index < 8; ++index)
        bytes.append(char((value >> (index * 8)) & 0xff));
}
void appendString(QByteArray& bytes, const QString& value) {
    const QByteArray utf8 = value.toUtf8();
    appendU32(bytes, static_cast<quint32>(utf8.size()));
    bytes.append(utf8);
}

void writeU32(uchar* destination, quint32 value) {
    destination[0] = static_cast<uchar>(value & 0xff);
    destination[1] = static_cast<uchar>((value >> 8) & 0xff);
    destination[2] = static_cast<uchar>((value >> 16) & 0xff);
    destination[3] = static_cast<uchar>((value >> 24) & 0xff);
}

void writeU64(uchar* destination, quint64 value) {
    for (int index = 0; index < 8; ++index) {
        destination[index] = static_cast<uchar>((value >> (index * 8)) & 0xff);
    }
}

bool takeU8(const QByteArray& bytes, qsizetype& offset, quint8* value) {
    if (offset + 1 > bytes.size())
        return false;
    *value = static_cast<quint8>(bytes.at(offset++));
    return true;
}
bool takeU32(const QByteArray& bytes, qsizetype& offset, quint32* value) {
    if (offset + 4 > bytes.size())
        return false;
    *value = static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 8) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 16) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3))) << 24);
    offset += 4;
    return true;
}
bool takeU64(const QByteArray& bytes, qsizetype& offset, quint64* value) {
    if (offset + 8 > bytes.size())
        return false;
    *value = 0;
    for (int index = 0; index < 8; ++index) {
        *value |= static_cast<quint64>(static_cast<quint8>(bytes.at(offset + index)))
                  << (index * 8);
    }
    offset += 8;
    return true;
}
bool takeF32(const QByteArray& bytes, qsizetype& offset, float* value) {
    quint32 raw = 0;
    if (!takeU32(bytes, offset, &raw))
        return false;
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}
bool takeString(const QByteArray& bytes, qsizetype& offset, QString* value) {
    quint32 length = 0;
    if (!takeU32(bytes, offset, &length) || length > static_cast<quint32>(bytes.size() - offset))
        return false;
    *value = QString::fromUtf8(bytes.constData() + offset, static_cast<qsizetype>(length));
    offset += static_cast<qsizetype>(length);
    return true;
}

QByteArray makeFrame(quint16 kind, quint64 requestId, const QByteArray& payload = {}) {
    QByteArray frame;
    frame.reserve(20 + payload.size());
    appendU32(frame, kProtocolMagic);
    frame.append(char(kProtocolVersion & 0xff));
    frame.append(char((kProtocolVersion >> 8) & 0xff));
    frame.append(char(kind & 0xff));
    frame.append(char((kind >> 8) & 0xff));
    appendU64(frame, requestId);
    appendU32(frame, static_cast<quint32>(payload.size()));
    frame.append(payload);
    return frame;
}

QPolygonF quadFromValues(const float* points, const QRectF& canvasRect, const QSize& imageSize) {
    const qreal scaleX = imageSize.width() > 0 ? canvasRect.width() / imageSize.width() : 1.0;
    const qreal scaleY = imageSize.height() > 0 ? canvasRect.height() / imageSize.height() : 1.0;
    QPolygonF polygon;
    polygon.reserve(4);
    for (int index = 0; index < 4; ++index) {
        polygon.push_back(QPointF(canvasRect.left() + points[index * 2] * scaleX,
                                  canvasRect.top() + points[index * 2 + 1] * scaleY));
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
// Private transport: every method and all Qt/file objects belong to its I/O thread.
// The service retains scheduling and receiver ownership on the application thread.
class ScreenshotOcrTransport final : public QObject {
  public:
    struct Callbacks {
        std::function<void(qint64)> started;
        std::function<void(QByteArray)> output;
        std::function<void(QByteArray)> errorOutput;
        std::function<void(int, QProcess::ExitStatus)> finished;
        std::function<void(QString)> failed;
    };

    ScreenshotOcrTransport(QObject* receiver, Callbacks callbacks)
        : m_receiver(receiver), m_callbacks(std::move(callbacks)) {}

    ~ScreenshotOcrTransport() override {
        if (m_process != nullptr) {
            m_process->disconnect(this);
            if (m_process->state() != QProcess::NotRunning) {
                const auto pid = m_process->processId();
                m_process->kill();
                m_process->waitForFinished(1000);
                snow_shot::diagnostics::logEvent(
                    QStringLiteral("snow_shot.ocr"), QStringLiteral("ocr.process_exit"),
                    {{QStringLiteral("exit_code"), m_process->exitCode()},
                     {QStringLiteral("outcome"), QStringLiteral("shutdown")},
                     {QStringLiteral("child_pid"), pid}});
            }
        }
    }

    void start(const ScreenshotOcrResolvedAssets& assets, qsizetype slotBytes, int slotCount,
               int workers, ScreenshotOcrBackendPreference backend,
               const QProcessEnvironment& environment) {
        m_slotBytes = slotBytes;
        m_file = std::make_unique<QTemporaryFile>();
        const qint64 bytes = slotBytes * slotCount;
        if (!m_file->open() || !m_file->resize(bytes) ||
            (m_mapping = m_file->map(0, bytes)) == nullptr) {
            post(m_callbacks.failed, QStringLiteral("shared_memory"));
            return;
        }
        m_file->close();
        for (int slot = 0; slot < slotCount; ++slot)
            std::memset(m_mapping + slot * slotBytes, 0, kSlotHeaderBytes);

        QByteArray hello;
        appendU32(hello, static_cast<quint32>(workers));
        appendU8(hello, backend == ScreenshotOcrBackendPreference::DirectMl ? 1 : 0);
        appendString(hello, assets.detectorModelPath);
        appendString(hello, assets.recognizerModelPath);
        appendString(hello, assets.dictionaryPath);
        appendString(hello, assets.stateDirectory);
        appendString(hello, m_file->fileName());
        appendU64(hello, static_cast<quint64>(slotBytes));
        appendU32(hello, static_cast<quint32>(slotCount));
        m_process = std::make_unique<QProcess>();
        connect(m_process.get(), &QProcess::started, this, [this, hello]() {
            post(m_callbacks.started, m_process->processId());
            send(makeFrame(kHello, 0, hello));
        });
        connect(m_process.get(), &QProcess::readyReadStandardOutput, this,
                [this]() { post(m_callbacks.output, m_process->readAllStandardOutput()); });
        connect(m_process.get(), &QProcess::readyReadStandardError, this,
                [this]() { post(m_callbacks.errorOutput, m_process->readAllStandardError()); });
        connect(m_process.get(), &QProcess::errorOccurred, this,
                [this](QProcess::ProcessError error) {
                    if (error == QProcess::FailedToStart)
                        post(m_callbacks.failed, QStringLiteral("start_failed"));
                });
        connect(m_process.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this](int code, QProcess::ExitStatus status) {
                    post(m_callbacks.output, m_process->readAllStandardOutput());
                    post(m_callbacks.errorOutput, m_process->readAllStandardError());
                    post(m_callbacks.finished, code, status);
                });
        m_process->setWorkingDirectory(assets.runtimeDirectory);
        m_process->setProcessEnvironment(environment);
        m_process->start(assets.processPath);
    }

    void submit(QImage image, int slot, quint64 sequence, quint64 token, quint8 priority) {
        if (m_process == nullptr || m_process->state() != QProcess::Running)
            return;
        image = image.convertToFormat(QImage::Format_RGBA8888);
        const qsizetype stride = static_cast<qsizetype>(image.width()) * 4;
        if (image.isNull() || stride * image.height() > m_slotBytes - kSlotHeaderBytes) {
            post(m_callbacks.failed, QStringLiteral("image_transfer"));
            return;
        }
        uchar* header = m_mapping + slot * m_slotBytes;
        writeU32(header + kSlotStateOffset, kSlotFree);
        for (int row = 0; row < image.height(); ++row)
            std::memcpy(header + kSlotHeaderBytes + row * stride, image.constScanLine(row), stride);
        writeU64(header + kSlotSequenceOffset, sequence);
        writeU32(header + kSlotWidthOffset, static_cast<quint32>(image.width()));
        writeU32(header + kSlotHeightOffset, static_cast<quint32>(image.height()));
        writeU32(header + kSlotStrideOffset, static_cast<quint32>(stride));
        writeU32(header + kSlotBytesOffset, static_cast<quint32>(stride * image.height()));
        writeU32(header + kSlotMagicOffset, kSlotMagic);
        QByteArray payload;
        appendU32(payload, static_cast<quint32>(slot));
        appendU32(payload, static_cast<quint32>(image.width()));
        appendU32(payload, static_cast<quint32>(image.height()));
        appendU32(payload, static_cast<quint32>(stride));
        appendU64(payload, sequence);
        appendU8(payload, priority);
        std::atomic_thread_fence(std::memory_order_release);
        writeU32(header + kSlotStateOffset, kSlotReady);
        send(makeFrame(kSubmit, token, payload));
    }

    void send(const QByteArray& frame) {
        if (m_process != nullptr && m_process->state() == QProcess::Running)
            m_process->write(frame);
    }

    void stop(bool force) {
        if (m_process == nullptr || m_process->state() == QProcess::NotRunning)
            return;
        if (force) {
            m_process->kill();
        } else {
            send(makeFrame(kShutdown, 0));
            m_process->closeWriteChannel();
            QTimer::singleShot(1000, this, [this]() { stop(true); });
        }
    }

  private:
    template <typename Callback, typename... Args> void post(Callback callback, Args... args) {
        QMetaObject::invokeMethod(
            m_receiver, [callback, args...]() mutable { callback(args...); }, Qt::QueuedConnection);
    }
    QObject* m_receiver;
    Callbacks m_callbacks;
    std::unique_ptr<QProcess> m_process;
    std::unique_ptr<QTemporaryFile> m_file;
    uchar* m_mapping = nullptr;
    qsizetype m_slotBytes = 0;
};

} // namespace

class ScreenshotOcrRecognitionService::Impl final {
  public:
    Impl(ScreenshotOcrRecognitionService* owner, const Options& options,
         ScreenshotOcrBackendPreference preference)
        : m_owner(owner), m_workerLimit(std::clamp(options.workerCount, 1, 2)),
          m_proxyUrl(options.proxyUrl), m_modelType(options.modelType),
          m_backendPreference(preference) {
        m_queueClock.start();
        m_slots.resize(m_workerLimit);
        m_transportThread.setObjectName(QStringLiteral("snow-ocr-transport"));
        m_localPool.setMaxThreadCount(m_workerLimit);
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
                        m_assets = assets;
                        if (ensureProcess()) {
                            flushPending();
                            return;
                        }
                        bool waitingForShutdown = false;
                        {
                            std::lock_guard lock(m_mutex);
                            waitingForShutdown = m_shuttingDown || m_stopping;
                        }
                        if (!waitingForShutdown)
                            failPendingForAssetError();
                    });
            connect(m_assetManager.get(), &ScreenshotOcrAssets::failed, owner,
                    [this](const QString& error) {
                        // The UI only surfaces a generic message; keep the
                        // actionable detail (missing manifest, hash mismatch,
                        // network failure, ...) in the application log.
                        qWarning().noquote() << "OCR asset preparation failed:" << error;
                        failPendingForAssetError();
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
            std::lock_guard lock(m_mutex);
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
            m_jobs.insert(token, job);
            m_pending.push_back(job);
        }
        if (!assetsReady()) {
            if (m_assetManager != nullptr)
                m_assetManager->prepare();
        } else if (!ensureProcess()) {
            bool shuttingDown = false;
            {
                std::lock_guard lock(m_mutex);
                shuttingDown = m_shuttingDown;
            }
            if (!shuttingDown)
                failPendingForAssetError();
        } else {
            flushPending();
        }
        return token;
    }

    RequestToken render(RequestToken token, ScreenshotOcrRequest request, QObject* receiver,
                        Completion completion) {
        auto job = makeJob(token, std::move(request), receiver, std::move(completion));
        {
            std::lock_guard lock(m_mutex);
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
        m_localPool.start(QRunnable::create([service, job, alive]() {
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
        std::shared_ptr<Job> job;
        bool abortProcess = false;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_jobs.find(token);
            if (it == m_jobs.end())
                return;
            job = it.value();
            if (job->cancelled.load())
                return;
            auto fields = jobFields(job);
            fields.insert(QStringLiteral("outcome"), QStringLiteral("cancelled"));
            fields.insert(QStringLiteral("stage"), job->processSubmitted ? QStringLiteral("worker")
                                                   : job->localRendering ? QStringLiteral("render")
                                                                         : QStringLiteral("queue"));
            snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.ocr"),
                                             QStringLiteral("ocr.cancelled"), fields);
            job->cancelled.store(true, std::memory_order_release);
            if (!job->running && !job->localRendering) {
                m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), job),
                                m_pending.end());
                m_jobs.erase(it);
            }
            // The child cannot reliably interrupt an ONNX call. If this was
            // the final process-bound task, tear down the child instead of
            // retaining it until the canceled inference returns.
            abortProcess = job->running && job->processSubmitted && m_runningCount == 1 &&
                           m_pending.empty() && m_transport != nullptr;
            if (abortProcess) {
                // Retire this generation before another request can reuse it.
                // The finished handler starts a fresh child for queued work.
                m_shuttingDown = true;
                m_ready = false;
                m_processStopReason = ProcessStopReason::Cancelled;
                m_jobs.erase(it);
                m_slots[job->slot].reset();
                m_runningCount = 0;
            }
        }
        if (job->running && job->processSubmitted)
            sendFrame(makeFrame(kCancel, token));
        if (!job->localRendering)
            QObject::disconnect(job->receiverDestroyed);
        if (abortProcess)
            stopTransport(true);
        maybeShutdownProcess();
    }

    bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) {
        std::lock_guard lock(m_mutex);
        auto it = m_jobs.find(token);
        if (it == m_jobs.end() || it.value()->running || it.value()->cancelled.load())
            return false;
        it.value()->request.priority = priority;
        return true;
    }

    bool setRenderFilteredImage(RequestToken token, bool enabled, const QColor& backgroundColor) {
        std::lock_guard lock(m_mutex);
        auto it = m_jobs.find(token);
        if (it == m_jobs.end() || it.value()->request.renderOnly || it.value()->localRendering ||
            it.value()->cancelled.load())
            return false;
        it.value()->request.renderFilteredImage = enabled;
        it.value()->request.backgroundColor = enabled ? backgroundColor : QColor();
        return true;
    }

    void setBackendPreference(ScreenshotOcrBackendPreference preference) {
        bool restart = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_backendPreference == preference)
                return;
            m_backendPreference = preference;
            restart = m_transport != nullptr && !m_stopping;
            if (restart)
                m_configurationDirty = true;
        }
        if (restart)
            maybeShutdownProcess();
    }

    void setProxyUrl(const QString& proxyUrl) {
        if (m_proxyUrl == proxyUrl)
            return;
        m_proxyUrl = proxyUrl;
        if (m_assetManager != nullptr)
            m_assetManager->setProxyUrl(proxyUrl);
    }

    void setModelType(ScreenshotOcrModelType modelType) {
        if (m_modelType == modelType)
            return;
        m_modelType = modelType;
        if (m_assetManager != nullptr) {
            m_assetManager->setModelType(modelType);
        } else {
            m_assets.modelType = modelType;
            m_assets.modelId = screenshotOcrModelTypeValue(modelType);
        }
        bool restart = false;
        bool prepare = false;
        {
            std::lock_guard lock(m_mutex);
            restart = m_transport != nullptr && !m_stopping;
            prepare = !m_pending.empty() && !m_stopping;
            if (restart)
                m_configurationDirty = true;
        }
        if (prepare && m_assetManager != nullptr)
            m_assetManager->prepare();
        if (restart)
            maybeShutdownProcess();
    }

    int liveWorkerCount() const {
        std::lock_guard lock(m_mutex);
        return m_transport != nullptr && !m_shuttingDown
                   ? std::min(m_workerLimit, m_runningCount + static_cast<int>(m_pending.size()))
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
        int slot = -1;
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
        std::lock_guard lock(m_mutex);
        if (!assetsReady() || m_stopping)
            return false;
        if (m_transport != nullptr)
            return !m_shuttingDown;
        if (m_pending.empty())
            return true;
        qsizetype imageBytes = 4;
        for (const auto& job : m_pending) {
            const qsizetype required =
                static_cast<qsizetype>(job->imageSize.width()) * job->imageSize.height() * 4;
            imageBytes = std::max(imageBytes, std::min(required, kMaximumImageBytes));
        }
        m_slotBytes = kSlotHeaderBytes + imageBytes;
        m_slotSequences.assign(m_slots.size(), 0);
        m_readBuffer.clear();
        m_stderrBuffer.clear();
        m_childPid = 0;
        m_ready = false;
        m_shuttingDown = false;
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
                      {QStringLiteral("slot_count"), static_cast<int>(m_slots.size())},
                      {QStringLiteral("shared_memory_bytes"),
                       static_cast<qint64>(m_slotBytes * m_slots.size())}});
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
                 if (m_shuttingDown)
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
            [transport = m_transport, assets = m_assets, bytes = m_slotBytes,
             count = static_cast<int>(m_slots.size()), workers = m_workerLimit,
             backend = m_backendPreference, environment]() {
                transport->start(assets, bytes, count, workers, backend, environment);
            },
            Qt::QueuedConnection);
        QTimer::singleShot(5000, m_owner, [this, valid]() {
            if (valid() && m_transport != nullptr && !m_ready && !m_shuttingDown)
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

    int acquireSlot() const {
        for (int index = 0; index < static_cast<int>(m_slots.size()); ++index)
            if (!m_slots[index])
                return index;
        return -1;
    }

    std::shared_ptr<Job> nextPending() {
        const qint64 now = m_queueClock.elapsed();
        auto aged = std::min_element(
            m_pending.begin(), m_pending.end(), [now](const auto& first, const auto& second) {
                const bool firstAged =
                    first->request.priority == ScreenshotOcrRequestPriority::Prefetch &&
                    now - first->queuedAtMilliseconds >= kPrefetchAgingMilliseconds;
                const bool secondAged =
                    second->request.priority == ScreenshotOcrRequestPriority::Prefetch &&
                    now - second->queuedAtMilliseconds >= kPrefetchAgingMilliseconds;
                if (firstAged != secondAged)
                    return firstAged;
                return first->queuedAtMilliseconds < second->queuedAtMilliseconds;
            });
        if (aged != m_pending.end() &&
            (*aged)->request.priority == ScreenshotOcrRequestPriority::Prefetch &&
            now - (*aged)->queuedAtMilliseconds >= kPrefetchAgingMilliseconds) {
            auto job = *aged;
            m_pending.erase(aged);
            return job;
        }
        auto choose = [&](ScreenshotOcrRequestPriority priority) {
            auto it = std::find_if(m_pending.begin(), m_pending.end(), [priority](const auto& job) {
                return job->request.priority == priority && !job->cancelled.load();
            });
            if (it == m_pending.end())
                return std::shared_ptr<Job>();
            auto job = *it;
            m_pending.erase(it);
            return job;
        };
        auto job = choose(ScreenshotOcrRequestPriority::Interactive);
        return job != nullptr ? job : choose(ScreenshotOcrRequestPriority::Prefetch);
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

    void flushPending() {
        std::lock_guard lock(m_mutex);
        if (!m_ready || m_shuttingDown || m_configurationDirty)
            return;
        while (true) {
            const int slot = acquireSlot();
            if (slot < 0)
                return;
            auto job = nextPending();
            if (job == nullptr)
                return;
            const qsizetype imageBytes =
                static_cast<qsizetype>(job->imageSize.width()) * job->imageSize.height() * 4;
            if (imageBytes > kMaximumImageBytes) {
                failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController",
                                                               "Text recognition failed"));
                continue;
            }
            if (imageBytes > m_slotBytes - kSlotHeaderBytes) {
                // Never resize a mapping while the child owns it. Drain this generation,
                // then size its replacement for the largest request still queued.
                m_pending.insert(m_pending.begin(), job);
                m_configurationDirty = true;
                if (m_runningCount == 0) {
                    m_shuttingDown = true;
                    m_processStopReason = ProcessStopReason::Shutdown;
                    stopTransport(false);
                }
                return;
            }
            const quint64 sequence = ++m_slotSequences[slot];
            m_slots[slot] = job;
            job->slot = slot;
            job->running = true;
            job->processSubmitted = true;
            job->submittedAt = job->elapsed.elapsed();
            job->childPid = m_childPid;
            ++m_runningCount;
            auto fields = jobFields(job);
            fields.insert(QStringLiteral("pending_count"), static_cast<int>(m_pending.size()));
            fields.insert(QStringLiteral("running_count"), m_runningCount);
            snow_shot::diagnostics::logEvent(QStringLiteral("snow_shot.ocr"),
                                             QStringLiteral("ocr.submitted"), fields);
            QMetaObject::invokeMethod(
                m_transport,
                [transport = m_transport, image = job->request.image, slot, sequence,
                 token = job->token, priority = job->request.priority]() {
                    transport->submit(image, slot, sequence, token,
                                      priority == ScreenshotOcrRequestPriority::Interactive ? 0
                                                                                            : 1);
                },
                Qt::QueuedConnection);
        }
    }

    void readProcessOutput(const QByteArray& bytes) {
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
            const quint16 version =
                static_cast<quint16>(static_cast<quint8>(m_readBuffer.at(4))) |
                (static_cast<quint16>(static_cast<quint8>(m_readBuffer.at(5))) << 8);
            const quint16 kind =
                static_cast<quint16>(static_cast<quint8>(m_readBuffer.at(6))) |
                (static_cast<quint16>(static_cast<quint8>(m_readBuffer.at(7))) << 8);
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
            // ShutdownAck precedes process exit. Keep ownership until finished().
        }
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
             {QStringLiteral("stage"), QStringLiteral("capability_probe")},
             {QStringLiteral("outcome"),
              directMl ? QStringLiteral("directml_available") : QStringLiteral("cpu_only")}});
        m_ready = true;
        m_assetStatus = {m_assets.offline ? ScreenshotOcrAssetPhase::ReadyOffline
                                          : ScreenshotOcrAssetPhase::ReadyCached,
                         QStringLiteral("assets")};
        flushPending();
        maybeShutdownProcess();
    }

    void handleComplete(quint64 id, const QByteArray& payload) {
        std::shared_ptr<Job> job;
        ScreenshotOcrRecognitionResult result;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_jobs.find(id);
            if (it == m_jobs.end())
                return;
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
                    line.confidence = confidence;
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
            if (job->slot >= 0) {
                m_slots[job->slot].reset();
            }
            job->slot = -1;
            job->running = false;
            job->processSubmitted = false;
            --m_runningCount;
            const bool renderFiltered = !job->cancelled.load() && result.error.isEmpty() &&
                                        result.presentation != nullptr &&
                                        job->request.renderFilteredImage;
            if (renderFiltered) {
                job->localRendering = true;
                ++m_localRenderingCount;
            } else {
                m_jobs.remove(id);
            }
        }
        if (!job->localRendering)
            QObject::disconnect(job->receiverDestroyed);
        if (!job->cancelled.load() && result.error.isEmpty() && result.presentation != nullptr &&
            job->request.renderFilteredImage) {
            const QImage source = job->request.image;
            const QRectF canvasRect = job->request.canvasRect;
            const QColor background = job->request.backgroundColor;
            const QPointer<ScreenshotOcrRecognitionService> service(m_owner);
            const auto alive = m_alive;
            m_localPool.start(QRunnable::create([service, job, alive, result = std::move(result),
                                                 source, canvasRect, background]() mutable {
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
        if (!job->cancelled.load() && job->receiver != nullptr && job->completion)
            job->completion(std::move(result));
    }

    void finishLocalJob(const std::shared_ptr<Job>& job, ScreenshotOcrRecognitionResult result) {
        {
            std::lock_guard lock(m_mutex);
            auto it = m_jobs.find(job->token);
            if (it == m_jobs.end())
                return;
            m_jobs.erase(it);
            job->localRendering = false;
            if (m_localRenderingCount > 0)
                --m_localRenderingCount;
        }
        QObject::disconnect(job->receiverDestroyed);
        deliver(job, std::move(result));
        maybeShutdownProcess();
    }

    void failJob(const std::shared_ptr<Job>& job, const QString& error) {
        {
            std::lock_guard lock(m_mutex);
            failJobLocked(job, error);
        }
        maybeShutdownProcess();
    }

    void failJobLocked(const std::shared_ptr<Job>& job, const QString& error) {
        auto fields = jobFields(job);
        fields.insert(QStringLiteral("outcome"), job->cancelled.load() ? QStringLiteral("cancelled")
                                                                       : QStringLiteral("failed"));
        snow_shot::diagnostics::DiagnosticsService::instance().record(
            job->cancelled.load() ? QtInfoMsg : QtWarningMsg, QStringLiteral("snow_shot.ocr"),
            QStringLiteral("ocr.failed"), error, fields);
        m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), job), m_pending.end());
        if (job->slot >= 0) {
            m_slots[job->slot].reset();
            job->slot = -1;
            job->running = false;
            job->processSubmitted = false;
            if (m_runningCount > 0)
                --m_runningCount;
        }
        job->localRendering = false;
        m_jobs.remove(job->token);
        QObject::disconnect(job->receiverDestroyed);
        if (!job->cancelled.load() && job->receiver != nullptr && job->completion) {
            ScreenshotOcrRecognitionResult result;
            result.error = error;
            QMetaObject::invokeMethod(
                m_owner,
                [job, result = std::move(result)]() mutable {
                    if (!job->cancelled.load() && job->receiver != nullptr && job->completion)
                        job->completion(std::move(result));
                },
                Qt::QueuedConnection);
        }
    }

    void processFailed(const char* stage = "process_exit") {
        std::vector<std::shared_ptr<Job>> failed;
        {
            std::lock_guard lock(m_mutex);
            if (m_transport == nullptr || m_stopping || m_shuttingDown)
                return;
            snow_shot::diagnostics::DiagnosticsService::instance().record(
                QtWarningMsg, QStringLiteral("snow_shot.ocr"), QStringLiteral("ocr.process_failed"),
                QString::fromLatin1(stage),
                {{QStringLiteral("stage"), QString::fromLatin1(stage)},
                 {QStringLiteral("child_pid"), m_childPid},
                 {QStringLiteral("outcome"), QStringLiteral("failed")}});
            const bool initializationFailed = !m_ready;
            for (auto it = m_jobs.begin(); it != m_jobs.end();) {
                const auto job = it.value();
                if (!job->localRendering) {
                    job->childPid = m_childPid;
                    failed.push_back(job);
                    it = m_jobs.erase(it);
                } else {
                    ++it;
                }
            }
            m_pending.clear();
            m_runningCount = 0;
            m_slots.assign(m_slots.size(), {});
            m_ready = false;
            m_shuttingDown = false;
            m_configurationDirty = false;
            if (initializationFailed) {
                m_assetStatus = {ScreenshotOcrAssetPhase::Failed, QStringLiteral("assets"), 0, 0,
                                 QStringLiteral("OCR model initialization failed")};
                if (m_assetManager != nullptr)
                    m_assets = {};
            }
            releaseTransport();
        }
        for (const auto& job : failed) {
            QObject::disconnect(job->receiverDestroyed);
            ScreenshotOcrRecognitionResult result;
            result.error =
                QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed");
            deliver(job, std::move(result));
        }
    }

    void failPendingForAssetError() {
        std::vector<std::shared_ptr<Job>> failed;
        {
            std::lock_guard lock(m_mutex);
            failed = m_pending;
            for (const auto& job : failed)
                m_jobs.remove(job->token);
            m_pending.clear();
        }
        for (const auto& job : failed) {
            QObject::disconnect(job->receiverDestroyed);
            if (!job->cancelled.load() && job->receiver != nullptr && job->completion) {
                ScreenshotOcrRecognitionResult result;
                result.error = QCoreApplication::translate(
                    "ScreenshotOcrController", "Text recognition components could not be prepared");
                job->completion(std::move(result));
            }
        }
    }

    bool assetsReady() const {
        return m_assets.valid() && m_assets.modelType == m_modelType;
    }

    void maybeShutdownProcess() {
        std::lock_guard lock(m_mutex);
        if (m_transport == nullptr || !m_ready || m_shuttingDown || m_runningCount != 0 ||
            (!m_pending.empty() && !m_configurationDirty))
            return;
        m_shuttingDown = true;
        m_processStopReason = ProcessStopReason::Shutdown;
        stopTransport(false);
    }

    void finishShutdown() {
        {
            std::lock_guard lock(m_mutex);
            releaseTransport();
            m_ready = false;
            m_shuttingDown = false;
            m_configurationDirty = false;
        }
        if (!m_pending.empty() && !m_stopping) {
            if (ensureProcess())
                flushPending();
            else if (m_assetManager != nullptr)
                m_assetManager->prepare();
        }
    }

    void shutdown() {
        m_alive->store(false, std::memory_order_release);
        {
            std::lock_guard lock(m_mutex);
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
        m_localPool.waitForDone();
        m_transportThread.quit();
        m_transportThread.wait();
    }

    ScreenshotOcrRecognitionService* m_owner = nullptr;
    const int m_workerLimit;
    QString m_proxyUrl;
    ScreenshotOcrModelType m_modelType = ScreenshotOcrModelType::Small;
    ScreenshotOcrResolvedAssets m_assets;
    ScreenshotOcrAssetStatus m_assetStatus;
    std::unique_ptr<ScreenshotOcrAssets> m_assetManager;
    mutable std::mutex m_mutex;
    QHash<RequestToken, std::shared_ptr<Job>> m_jobs;
    std::vector<std::shared_ptr<Job>> m_pending, m_slots;
    QThreadPool m_localPool;
    QElapsedTimer m_queueClock;
    std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);
    std::vector<quint64> m_slotSequences;
    QThread m_transportThread;
    ScreenshotOcrTransport* m_transport = nullptr;
    ProcessStopReason m_processStopReason = ProcessStopReason::None;
    qsizetype m_slotBytes = 0;
    int m_runningCount = 0, m_localRenderingCount = 0;
    ScreenshotOcrBackendPreference m_backendPreference = ScreenshotOcrBackendPreference::Cpu;
    QByteArray m_readBuffer;
    QByteArray m_stderrBuffer;
    qint64 m_childPid = 0;
    bool m_ready = false, m_shuttingDown = false, m_stopping = false;
    bool m_configurationDirty = false;
    quint64 m_processGeneration = 0;
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
