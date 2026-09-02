#include "snow_shot/presentation/screenshotocrrecognitionservice.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrvisuals.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QPointer>
#include <QRunnable>
#include <QThreadPool>
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
constexpr auto kRuntimeVersion = "1.0.0";
constexpr quint16 kHello = 1;
constexpr quint16 kReady = 2;
constexpr quint16 kSubmit = 3;
constexpr quint16 kCancel = 4;
constexpr quint16 kComplete = 5;
constexpr quint16 kShutdown = 6;
constexpr quint16 kShutdownAck = 7;
constexpr qsizetype kSlotHeaderBytes = 32;
constexpr qsizetype kMaximumImageBytes = 3840LL * 2160LL * 4LL;
constexpr qsizetype kMaximumFrameBytes = 1024LL * 1024LL;
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

void appendU8(QByteArray& bytes, quint8 value) { bytes.append(char(value)); }
void appendU32(QByteArray& bytes, quint32 value) {
    for (int index = 0; index < 4; ++index) bytes.append(char((value >> (index * 8)) & 0xff));
}
void appendU64(QByteArray& bytes, quint64 value) {
    for (int index = 0; index < 8; ++index) bytes.append(char((value >> (index * 8)) & 0xff));
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
    if (offset + 1 > bytes.size()) return false;
    *value = static_cast<quint8>(bytes.at(offset++));
    return true;
}
bool takeU32(const QByteArray& bytes, qsizetype& offset, quint32* value) {
    if (offset + 4 > bytes.size()) return false;
    *value = static_cast<quint32>(static_cast<quint8>(bytes.at(offset))) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 1))) << 8) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 2))) << 16) |
             (static_cast<quint32>(static_cast<quint8>(bytes.at(offset + 3))) << 24);
    offset += 4;
    return true;
}
bool takeU64(const QByteArray& bytes, qsizetype& offset, quint64* value) {
    if (offset + 8 > bytes.size()) return false;
    *value = 0;
    for (int index = 0; index < 8; ++index) {
        *value |= static_cast<quint64>(static_cast<quint8>(bytes.at(offset + index))) << (index * 8);
    }
    offset += 8;
    return true;
}
bool takeF32(const QByteArray& bytes, qsizetype& offset, float* value) {
    quint32 raw = 0;
    if (!takeU32(bytes, offset, &raw)) return false;
    std::memcpy(value, &raw, sizeof(raw));
    return true;
}
bool takeString(const QByteArray& bytes, qsizetype& offset, QString* value) {
    quint32 length = 0;
    if (!takeU32(bytes, offset, &length) || length > static_cast<quint32>(bytes.size() - offset)) return false;
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
    if (quad.size() != 4) return ScreenshotOcrTextDirection::Horizontal;
    const qreal width = std::max(edgeLength(quad.at(0), quad.at(1)), edgeLength(quad.at(3), quad.at(2)));
    const qreal height = std::max(edgeLength(quad.at(0), quad.at(3)), edgeLength(quad.at(1), quad.at(2)));
    return height >= width * 1.5 ? ScreenshotOcrTextDirection::Vertical
                                 : ScreenshotOcrTextDirection::Horizontal;
}

QImage renderFilteredImage(QImage source, const QRectF& canvasRect,
                           const std::shared_ptr<ScreenshotOcrPresentation>& presentation,
                           const QColor& backgroundColor,
                           SnowCanvasRegionFilterScratch* scratch,
                           QRectF* filteredImageCanvasRect) {
    if (filteredImageCanvasRect != nullptr) *filteredImageCanvasRect = {};
    if (source.isNull() || presentation == nullptr || !canvasRect.isValid() || canvasRect.isEmpty()) return {};
    source.setDevicePixelRatio(1.0);
    const QRectF normalized = canvasRect.normalized();
    QRect filteredPixels;
    QImage filtered = renderScreenshotOcrFilteredImage(
        source, normalized, *presentation, backgroundColor,
        std::max<qreal>(1.0, source.width() / normalized.width()), &filteredPixels, scratch);
    if (filteredImageCanvasRect != nullptr) {
        *filteredImageCanvasRect = screenshotOcrFilteredImageCanvasRect(normalized, source.size(), filteredPixels);
    }
    return filtered;
}
} // namespace

class ScreenshotOcrRecognitionService::Impl final {
  public:
    Impl(ScreenshotOcrRecognitionService* owner, const Options& options,
         ScreenshotOcrBackendPreference preference)
        : m_owner(owner),
          m_workerLimit(std::clamp(options.workerCount, 1, 2)),
          m_proxyUrl(options.proxyUrl),
          m_backendPreference(preference) {
        m_slots.resize(std::max(4, m_workerLimit + 2));
        m_localPool.setMaxThreadCount(m_workerLimit);
        if (!options.processPath.trimmed().isEmpty() &&
            !options.detectorModelPath.trimmed().isEmpty() &&
            !options.recognizerModelPath.trimmed().isEmpty() &&
            !options.dictionaryPath.trimmed().isEmpty()) {
            m_assets.runtimeVersion = QString::fromLatin1(kRuntimeVersion);
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
                ScreenshotOcrAssets::Options{offlineRoot, options.cacheRoot, options.proxyUrl},
                owner);
            connect(m_assetManager.get(), &ScreenshotOcrAssets::statusChanged, owner,
                    [this](const ScreenshotOcrAssetStatus& status) {
                        m_assetStatus = status;
                    });
            connect(m_assetManager.get(), &ScreenshotOcrAssets::ready, owner,
                    [this](const ScreenshotOcrResolvedAssets& assets) {
                        m_assets = assets;
                        if (ensureProcess()) flushPending();
                    });
            connect(m_assetManager.get(), &ScreenshotOcrAssets::failed, owner,
                    [this](const QString&) { failPendingForAssetError(); });
        }
    }

    ~Impl() { shutdown(); }

    RequestToken enqueue(RequestToken token, ScreenshotOcrRequest request, QObject* receiver,
                         Completion completion) {
        auto job = makeJob(token, std::move(request), receiver, std::move(completion));
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) { QObject::disconnect(job->receiverDestroyed); return 0; }
            m_jobs.insert(token, job);
            m_pending.push_back(job);
        }
        if (!assetsReady()) {
            if (m_assetManager != nullptr) m_assetManager->prepare();
        } else if (!ensureProcess()) {
            bool shuttingDown = false;
            {
                std::lock_guard lock(m_mutex);
                shuttingDown = m_shuttingDown;
            }
            if (!shuttingDown) {
                failJob(job, QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed"));
            }
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
            if (m_stopping) { QObject::disconnect(job->receiverDestroyed); return 0; }
            m_jobs.insert(token, job);
            job->running = true;
            job->localRendering = true;
            ++m_localRenderingCount;
        }
        const QPointer<ScreenshotOcrRecognitionService> service(m_owner);
        const auto alive = m_alive;
        m_localPool.start(QRunnable::create([service, job, alive]() {
            SnowCanvasRegionFilterScratch scratch;
            ScreenshotOcrRecognitionResult result;
            if (alive->load(std::memory_order_acquire) &&
                !job->cancelled.load(std::memory_order_acquire)) {
                result.filteredImage = renderFilteredImage(std::move(job->request.image), job->request.canvasRect,
                                                            job->request.presentation, job->request.backgroundColor,
                                                            &scratch, &result.filteredImageCanvasRect);
                if (result.filteredImage.isNull() && job->request.presentation != nullptr) {
                    result.error = QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed");
                }
            }
            QMetaObject::invokeMethod(service, [service, job, alive, result = std::move(result)]() mutable {
                if (alive->load(std::memory_order_acquire) && service != nullptr &&
                    service->m_impl != nullptr) {
                    service->m_impl->finishLocalJob(job, std::move(result));
                }
            }, Qt::QueuedConnection);
        }));
        return token;
    }

    void cancel(RequestToken token) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_jobs.find(token);
            if (it == m_jobs.end()) return;
            job = it.value();
            job->cancelled.store(true, std::memory_order_release);
            if (!job->running && !job->localRendering) {
                m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), job), m_pending.end());
                m_jobs.erase(it);
            }
        }
        if (job->running && job->processSubmitted) sendFrame(makeFrame(kCancel, token));
        if (!job->localRendering) QObject::disconnect(job->receiverDestroyed);
        maybeShutdownProcess();
    }

    bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) {
        std::lock_guard lock(m_mutex);
        auto it = m_jobs.find(token);
        if (it == m_jobs.end() || it.value()->running || it.value()->cancelled.load()) return false;
        it.value()->request.priority = priority;
        return true;
    }

    bool setRenderFilteredImage(RequestToken token, bool enabled, const QColor& backgroundColor) {
        std::lock_guard lock(m_mutex);
        auto it = m_jobs.find(token);
        if (it == m_jobs.end() || it.value()->request.renderOnly || it.value()->localRendering ||
            it.value()->cancelled.load()) return false;
        it.value()->request.renderFilteredImage = enabled;
        it.value()->request.backgroundColor = enabled ? backgroundColor : QColor();
        return true;
    }

    void setBackendPreference(ScreenshotOcrBackendPreference preference) {
        bool restart = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_backendPreference == preference) return;
            m_backendPreference = preference;
            restart = m_process != nullptr && !m_stopping;
            if (restart) m_configurationDirty = true;
        }
        if (restart) maybeShutdownProcess();
    }

    void setProxyUrl(const QString& proxyUrl) {
        if (m_proxyUrl == proxyUrl) return;
        m_proxyUrl = proxyUrl;
        if (m_assetManager != nullptr) m_assetManager->setProxyUrl(proxyUrl);
    }

    int liveWorkerCount() const {
        std::lock_guard lock(m_mutex);
        return m_process != nullptr && m_process->state() != QProcess::NotRunning
                   ? std::min(m_workerLimit, m_runningCount + static_cast<int>(m_pending.size())) : 0;
    }

    bool modelFilesReady() const { return assetsReady(); }
    ScreenshotOcrAssetStatus assetStatus() const { return m_assetStatus; }

  private:
    struct Job {
        RequestToken token = 0;
        ScreenshotOcrRequest request;
        QPointer<QObject> receiver;
        Completion completion;
        QMetaObject::Connection receiverDestroyed;
        std::atomic_bool cancelled{false};
        bool running = false;
        bool processSubmitted = false;
        bool localRendering = false;
        int slot = -1;
    };

    std::shared_ptr<Job> makeJob(RequestToken token, ScreenshotOcrRequest request, QObject* receiver,
                                 Completion completion) {
        auto job = std::make_shared<Job>();
        job->token = token; job->request = std::move(request); job->receiver = receiver; job->completion = std::move(completion);
        QPointer<ScreenshotOcrRecognitionService> service(m_owner);
        job->receiverDestroyed = QObject::connect(receiver, &QObject::destroyed, m_owner,
                                                  [service, token]() { if (service != nullptr) service->cancel(token); });
        return job;
    }

    bool ensureProcess() {
        std::lock_guard lock(m_mutex);
        if (!assetsReady()) return false;
        if (m_process != nullptr && m_process->state() != QProcess::NotRunning && !m_shuttingDown) return true;
        if (m_shuttingDown) {
            // A shutdown frame is already queued. Keep the request pending;
            // the finished handler will release the old process and the next
            // enqueue path will start a fresh child.
            return false;
        }
        if (m_stopping) return false;
        m_shmFile = std::make_unique<QTemporaryFile>();
        m_shmFile->setAutoRemove(true);
        if (!m_shmFile->open()) return false;
        m_slotBytes = kSlotHeaderBytes + kMaximumImageBytes;
        const qint64 size = static_cast<qint64>(m_slotBytes * m_slots.size());
        if (!m_shmFile->resize(size)) return false;
        m_shmMapping = m_shmFile->map(0, size);
        if (m_shmMapping == nullptr) return false;
        // Keep the mapping alive but release the file handle so the child can
        // reopen the temporary backing file on Windows without sharing locks.
        m_shmFile->close();
        m_readBuffer.clear();
        m_slotSequences.assign(m_slots.size(), 0);
        for (int index = 0; index < static_cast<int>(m_slots.size()); ++index) {
            uchar* header = m_shmMapping + index * m_slotBytes;
            std::memset(header, 0, static_cast<size_t>(kSlotHeaderBytes));
        }
        m_process = std::make_unique<QProcess>(m_owner);
        connect(m_process.get(), &QProcess::readyReadStandardOutput, m_owner, [this]() { readProcessOutput(); });
        connect(m_process.get(), &QProcess::readyReadStandardError, m_owner,
                [this]() { if (m_process != nullptr) m_process->readAllStandardError(); });
        connect(m_process.get(), qOverload<int, QProcess::ExitStatus>(&QProcess::finished), m_owner,
                [this](int, QProcess::ExitStatus) {
                    bool shuttingDown = false;
                    {
                        std::lock_guard lock(m_mutex);
                        shuttingDown = m_shuttingDown;
                    }
                    if (shuttingDown) finishShutdown();
                    else processFailed();
                });
        m_process->setProcessChannelMode(QProcess::SeparateChannels);
        m_process->setWorkingDirectory(m_assets.runtimeDirectory);
        m_process->start(m_assets.processPath);
        if (!m_process->waitForStarted(5000)) { m_process.reset(); return false; }
        QByteArray payload;
        appendU32(payload, static_cast<quint32>(m_workerLimit));
        appendU8(payload, m_backendPreference == ScreenshotOcrBackendPreference::DirectMl ? 1 : 0);
        appendString(payload, m_assets.detectorModelPath);
        appendString(payload, m_assets.recognizerModelPath);
        appendString(payload, m_assets.dictionaryPath);
        appendString(payload, m_assets.stateDirectory);
        appendString(payload, m_shmFile->fileName());
        appendU64(payload, static_cast<quint64>(m_slotBytes));
        appendU32(payload, static_cast<quint32>(m_slots.size()));
        sendFrame(makeFrame(kHello, 0, payload));
        m_ready = false; m_shuttingDown = false;
        const quint64 generation = ++m_processGeneration;
        QTimer::singleShot(5000, m_owner, [this, generation]() {
            bool timedOut = false;
            {
                std::lock_guard lock(m_mutex);
                timedOut = m_process != nullptr && !m_ready && generation == m_processGeneration;
            }
            if (timedOut) processFailed();
        });
        return true;
    }

    void sendFrame(const QByteArray& frame) {
        if (m_process != nullptr && m_process->state() != QProcess::NotRunning) m_process->write(frame);
    }

    int acquireSlot() const {
        for (int index = 0; index < static_cast<int>(m_slots.size()); ++index) if (!m_slots[index]) return index;
        return -1;
    }

    std::shared_ptr<Job> nextPending() {
        auto choose = [&](ScreenshotOcrRequestPriority priority) {
            auto it = std::find_if(m_pending.begin(), m_pending.end(), [priority](const auto& job) {
                return job->request.priority == priority && !job->cancelled.load();
            });
            if (it == m_pending.end()) return std::shared_ptr<Job>();
            auto job = *it; m_pending.erase(it); return job;
        };
        auto job = choose(ScreenshotOcrRequestPriority::Interactive);
        return job != nullptr ? job : choose(ScreenshotOcrRequestPriority::Prefetch);
    }

    void flushPending() {
        std::lock_guard lock(m_mutex);
        if (!m_ready || m_configurationDirty) return;
        while (true) {
            const int slot = acquireSlot();
            if (slot < 0) return;
            auto job = nextPending();
            if (job == nullptr) return;
            QImage image = job->request.image;
            if (image.format() != QImage::Format_RGBA8888) image = image.convertToFormat(QImage::Format_RGBA8888);
            if (image.isNull() || image.sizeInBytes() > static_cast<qsizetype>(m_slotBytes - kSlotHeaderBytes)) {
                failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed"));
                continue;
            }
            uchar* destination = m_shmMapping + slot * m_slotBytes + kSlotHeaderBytes;
            for (int row = 0; row < image.height(); ++row) std::memcpy(destination + row * image.bytesPerLine(), image.constScanLine(row), image.bytesPerLine());
            uchar* header = m_shmMapping + slot * m_slotBytes;
            const quint64 sequence = ++m_slotSequences[slot];
            writeU64(header + kSlotSequenceOffset, sequence);
            writeU32(header + kSlotStateOffset, kSlotFree);
            writeU32(header + kSlotWidthOffset, static_cast<quint32>(image.width()));
            writeU32(header + kSlotHeightOffset, static_cast<quint32>(image.height()));
            writeU32(header + kSlotStrideOffset, static_cast<quint32>(image.bytesPerLine()));
            writeU32(header + kSlotBytesOffset, static_cast<quint32>(image.sizeInBytes()));
            writeU32(header + kSlotMagicOffset, kSlotMagic);
            m_slots[slot] = job; job->slot = slot; job->running = true; job->processSubmitted = true; ++m_runningCount;
            QByteArray payload;
            appendU32(payload, static_cast<quint32>(slot)); appendU32(payload, static_cast<quint32>(image.width()));
            appendU32(payload, static_cast<quint32>(image.height())); appendU32(payload, static_cast<quint32>(image.bytesPerLine()));
            appendU64(payload, sequence);
            appendU8(payload, job->request.priority == ScreenshotOcrRequestPriority::Interactive ? 0 : 1);
            std::atomic_thread_fence(std::memory_order_release);
            writeU32(header + kSlotStateOffset, kSlotReady);
            sendFrame(makeFrame(kSubmit, job->token, payload));
        }
    }

    void readProcessOutput() {
        if (m_process == nullptr) return;
        m_readBuffer.append(m_process->readAllStandardOutput());
        while (m_readBuffer.size() >= 20) {
            const QByteArray magicBytes("SOCR", 4);
            const qsizetype magicPosition = m_readBuffer.indexOf(magicBytes);
            if (magicPosition < 0) {
                if (m_readBuffer.size() > 3) m_readBuffer.remove(0, m_readBuffer.size() - 3);
                return;
            }
            if (magicPosition > 0) m_readBuffer.remove(0, magicPosition);
            if (m_readBuffer.size() < 20) return;
            const quint32 magic = static_cast<quint32>(static_cast<quint8>(m_readBuffer.at(0))) |
                                  (static_cast<quint32>(static_cast<quint8>(m_readBuffer.at(1))) << 8) |
                                  (static_cast<quint32>(static_cast<quint8>(m_readBuffer.at(2))) << 16) |
                                  (static_cast<quint32>(static_cast<quint8>(m_readBuffer.at(3))) << 24);
            const quint16 version = static_cast<quint16>(static_cast<quint8>(m_readBuffer.at(4))) |
                                    (static_cast<quint16>(static_cast<quint8>(m_readBuffer.at(5))) << 8);
            const quint16 kind = static_cast<quint16>(static_cast<quint8>(m_readBuffer.at(6))) |
                                 (static_cast<quint16>(static_cast<quint8>(m_readBuffer.at(7))) << 8);
            qsizetype offset = 8; quint64 id = 0; quint32 length = 0;
            if (!takeU64(m_readBuffer, offset, &id) || !takeU32(m_readBuffer, offset, &length) ||
                magic != kProtocolMagic || version != kProtocolVersion || length > static_cast<quint32>(kMaximumFrameBytes)) {
                processFailed(); return;
            }
            if (m_readBuffer.size() < 20 + static_cast<qsizetype>(length)) return;
            const QByteArray payload = m_readBuffer.mid(20, static_cast<qsizetype>(length));
            m_readBuffer.remove(0, 20 + static_cast<qsizetype>(length));
            if (kind == kReady) handleReady(payload);
            else if (kind == kComplete) handleComplete(id, payload);
            else if (kind == kShutdownAck) finishShutdown();
        }
    }

    void handleReady(const QByteArray& payload) {
        qsizetype offset = 0; quint8 ok = 0, directMl = 0; QString provider, runtimeVersion;
        quint32 protocolVersion = 0;
        if (!takeU8(payload, offset, &ok) || !takeU8(payload, offset, &directMl) ||
            !takeString(payload, offset, &provider) ||
            !takeString(payload, offset, &runtimeVersion) ||
            !takeU32(payload, offset, &protocolVersion) || offset != payload.size() || ok == 0 ||
            runtimeVersion != QString::fromLatin1(kRuntimeVersion) ||
            protocolVersion != kProtocolVersion) {
            processFailed();
            return;
        }
        Q_UNUSED(directMl); Q_UNUSED(provider);
        m_ready = true;
        flushPending();
        maybeShutdownProcess();
    }

    void handleComplete(quint64 id, const QByteArray& payload) {
        std::shared_ptr<Job> job; ScreenshotOcrRecognitionResult result;
        {
            std::lock_guard lock(m_mutex);
            auto it = m_jobs.find(id); if (it == m_jobs.end()) return; job = it.value();
            qsizetype offset = 0; quint8 status = 0;
            if (!takeU8(payload, offset, &status)) { failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed")); return; }
            if (status == 0) {
                QString error; takeString(payload, offset, &error);
                result.error = error.isEmpty() ? QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed") : error;
            } else if (status == 1) {
                QString ignored; quint32 lineCount = 0;
                if (!takeString(payload, offset, &ignored) || !takeU32(payload, offset, &lineCount) || lineCount > 100000) { failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed")); return; }
                result.presentation = std::make_shared<ScreenshotOcrPresentation>(); result.presentation->selection = job->request.canvasRect.toAlignedRect(); result.presentation->lines.reserve(static_cast<qsizetype>(lineCount));
                for (quint32 index = 0; index < lineCount; ++index) {
                    QString text; float confidence = 0.0F; float points[8]{};
                    if (!takeString(payload, offset, &text) || !takeF32(payload, offset, &confidence)) { failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed")); return; }
                    for (float& point : points) if (!takeF32(payload, offset, &point)) { failJobLocked(job, QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed")); return; }
                    ScreenshotOcrLine line; line.text = std::move(text); line.confidence = confidence; line.quad = quadFromValues(points, job->request.canvasRect, job->request.image.size()); line.direction = textDirectionForQuad(line.quad); result.presentation->lines.push_back(std::move(line));
                }
                result.presentation->prepareForRendering();
            } else {
                result.error = QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed");
            }
            if (job->slot >= 0) {
                uchar* header = m_shmMapping + job->slot * m_slotBytes;
                std::atomic_thread_fence(std::memory_order_release);
                writeU32(header + kSlotStateOffset, kSlotFree);
                m_slots[job->slot].reset();
            }
            job->slot = -1; job->running = false; job->processSubmitted = false; --m_runningCount;
            const bool renderFiltered = !job->cancelled.load() && result.error.isEmpty() &&
                                         result.presentation != nullptr && job->request.renderFilteredImage;
            if (renderFiltered) {
                job->localRendering = true;
                ++m_localRenderingCount;
            } else {
                m_jobs.remove(id);
            }
        }
        if (!job->localRendering) QObject::disconnect(job->receiverDestroyed);
        if (!job->cancelled.load() && result.error.isEmpty() && result.presentation != nullptr && job->request.renderFilteredImage) {
            const QImage source = job->request.image; const QRectF canvasRect = job->request.canvasRect; const QColor background = job->request.backgroundColor;
            const QPointer<ScreenshotOcrRecognitionService> service(m_owner);
            const auto alive = m_alive;
            m_localPool.start(QRunnable::create([service, job, alive, result = std::move(result), source, canvasRect, background]() mutable {
                SnowCanvasRegionFilterScratch scratch;
                if (alive->load(std::memory_order_acquire)) {
                    result.filteredImage = renderFilteredImage(source, canvasRect, result.presentation, background, &scratch, &result.filteredImageCanvasRect);
                }
                QMetaObject::invokeMethod(service, [service, job, alive, result = std::move(result)]() mutable {
                    if (alive->load(std::memory_order_acquire) && service != nullptr &&
                        service->m_impl != nullptr) {
                        service->m_impl->finishLocalJob(job, std::move(result));
                    }
                }, Qt::QueuedConnection);
            }));
        } else {
            deliver(job, std::move(result));
        }
        flushPending(); maybeShutdownProcess();
    }

    void deliver(const std::shared_ptr<Job>& job, ScreenshotOcrRecognitionResult result) {
        if (!job->cancelled.load() && job->receiver != nullptr && job->completion) job->completion(std::move(result));
    }

    void finishLocalJob(const std::shared_ptr<Job>& job, ScreenshotOcrRecognitionResult result) {
        {
            std::lock_guard lock(m_mutex);
            auto it = m_jobs.find(job->token);
            if (it == m_jobs.end()) return;
            m_jobs.erase(it);
            job->localRendering = false;
            if (m_localRenderingCount > 0) --m_localRenderingCount;
        }
        QObject::disconnect(job->receiverDestroyed); deliver(job, std::move(result));
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
        m_pending.erase(std::remove(m_pending.begin(), m_pending.end(), job), m_pending.end());
        if (job->slot >= 0 && m_shmMapping != nullptr) {
            uchar* header = m_shmMapping + job->slot * m_slotBytes;
            std::atomic_thread_fence(std::memory_order_release);
            writeU32(header + kSlotStateOffset, kSlotFree);
            m_slots[job->slot].reset();
            job->slot = -1;
            job->running = false;
            job->processSubmitted = false;
            if (m_runningCount > 0) --m_runningCount;
        }
        job->localRendering = false;
        m_jobs.remove(job->token); QObject::disconnect(job->receiverDestroyed);
        if (!job->cancelled.load() && job->receiver != nullptr && job->completion) {
            ScreenshotOcrRecognitionResult result; result.error = error;
            QMetaObject::invokeMethod(m_owner, [job, result = std::move(result)]() mutable { if (!job->cancelled.load() && job->receiver != nullptr && job->completion) job->completion(std::move(result)); }, Qt::QueuedConnection);
        }
    }

    void processFailed() {
        std::vector<std::shared_ptr<Job>> failed;
        std::unique_ptr<QProcess> process;
        { std::lock_guard lock(m_mutex); if (m_process == nullptr || m_stopping || m_shuttingDown) return;
            for (auto it = m_jobs.begin(); it != m_jobs.end();) {
                const auto job = it.value();
                if (!job->localRendering) { failed.push_back(job); it = m_jobs.erase(it); }
                else { ++it; }
            }
            m_pending.clear(); m_runningCount = 0; m_slots.assign(m_slots.size(), {}); m_ready = false; m_shuttingDown = false; m_configurationDirty = false; process = std::move(m_process); m_shmMapping = nullptr; m_shmFile.reset(); }
        if (process != nullptr && process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
        for (const auto& job : failed) { QObject::disconnect(job->receiverDestroyed); if (!job->cancelled.load() && job->receiver != nullptr && job->completion) { ScreenshotOcrRecognitionResult result; result.error = QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed"); job->completion(std::move(result)); } }
    }

    void failPendingForAssetError() {
        std::vector<std::shared_ptr<Job>> failed;
        {
            std::lock_guard lock(m_mutex);
            failed = m_pending;
            for (const auto& job : failed) m_jobs.remove(job->token);
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

    bool assetsReady() const { return m_assets.valid(); }

    void maybeShutdownProcess() {
        std::lock_guard lock(m_mutex);
        if (m_process == nullptr || !m_ready || m_shuttingDown || m_runningCount != 0 ||
            m_localRenderingCount != 0 || (!m_pending.empty() && !m_configurationDirty)) {
            return;
        }
        m_shuttingDown = true;
        sendFrame(makeFrame(kShutdown, 0));
    }

    void finishShutdown() {
        std::unique_ptr<QProcess> process;
        { std::lock_guard lock(m_mutex); if (m_process == nullptr) return; process = std::move(m_process); m_shmMapping = nullptr; m_shmFile.reset(); m_ready = false; m_shuttingDown = false; m_configurationDirty = false; }
        process->closeWriteChannel();
        if (!process->waitForFinished(1000)) process->kill();
        bool restart = false;
        {
            std::lock_guard lock(m_mutex);
            restart = !m_pending.empty() && !m_stopping;
        }
        if (restart && ensureProcess()) flushPending();
    }

    void shutdown() {
        std::unique_ptr<QProcess> process;
        m_alive->store(false, std::memory_order_release);
        { std::lock_guard lock(m_mutex); m_stopping = true; for (auto it = m_jobs.begin(); it != m_jobs.end(); ++it) { it.value()->cancelled.store(true, std::memory_order_release); QObject::disconnect(it.value()->receiverDestroyed); } m_jobs.clear(); m_pending.clear(); m_localRenderingCount = 0; if (m_process != nullptr) { process = std::move(m_process); } m_shmMapping = nullptr; m_shmFile.reset(); }
        m_localPool.waitForDone();
        if (process != nullptr) {
            if (process->state() != QProcess::NotRunning) { process->write(makeFrame(kShutdown, 0)); process->closeWriteChannel(); if (!process->waitForFinished(1000)) process->kill(); }
            process.reset();
        }
    }

    ScreenshotOcrRecognitionService* m_owner = nullptr;
    const int m_workerLimit;
    QString m_proxyUrl;
    ScreenshotOcrResolvedAssets m_assets;
    ScreenshotOcrAssetStatus m_assetStatus;
    std::unique_ptr<ScreenshotOcrAssets> m_assetManager;
    mutable std::mutex m_mutex;
    QHash<RequestToken, std::shared_ptr<Job>> m_jobs;
    std::vector<std::shared_ptr<Job>> m_pending, m_slots;
    QThreadPool m_localPool;
    std::shared_ptr<std::atomic_bool> m_alive = std::make_shared<std::atomic_bool>(true);
    std::vector<quint64> m_slotSequences;
    std::unique_ptr<QProcess> m_process;
    std::unique_ptr<QTemporaryFile> m_shmFile;
    uchar* m_shmMapping = nullptr;
    qsizetype m_slotBytes = 0;
    int m_runningCount = 0, m_localRenderingCount = 0;
    ScreenshotOcrBackendPreference m_backendPreference = ScreenshotOcrBackendPreference::Cpu;
    QByteArray m_readBuffer;
    bool m_ready = false, m_shuttingDown = false, m_stopping = false;
    bool m_configurationDirty = false;
    quint64 m_processGeneration = 0;
};

ScreenshotOcrRecognitionService::ScreenshotOcrRecognitionService(QObject* parent)
    : ScreenshotOcrRecognitionService(Options{}, ScreenshotOcrBackendPreference::Cpu, parent) {}
ScreenshotOcrRecognitionService::ScreenshotOcrRecognitionService(const Options& options, ScreenshotOcrBackendPreference preference, QObject* parent)
    : ScreenshotOcrRecognitionPort(parent), m_impl(std::make_unique<Impl>(this, options, preference)) {}
ScreenshotOcrRecognitionService::~ScreenshotOcrRecognitionService() = default;

ScreenshotOcrRecognitionPort::RequestToken ScreenshotOcrRecognitionService::recognize(ScreenshotOcrRequest request, QObject* receiver, Completion completion) {
    if (request.image.isNull() || !request.canvasRect.isValid() || request.canvasRect.isEmpty() || receiver == nullptr || !completion || m_impl == nullptr) return 0;
    do { ++m_nextToken; } while (m_nextToken == 0);
    return m_impl->enqueue(m_nextToken, std::move(request), receiver, std::move(completion));
}
ScreenshotOcrRecognitionPort::RequestToken ScreenshotOcrRecognitionService::render(ScreenshotOcrRequest request, QObject* receiver, Completion completion) {
    if (request.image.isNull() || !request.canvasRect.isValid() || request.canvasRect.isEmpty() || request.presentation == nullptr || receiver == nullptr || !completion || m_impl == nullptr) return 0;
    request.renderOnly = true; request.renderFilteredImage = false;
    do { ++m_nextToken; } while (m_nextToken == 0);
    return m_impl->render(m_nextToken, std::move(request), receiver, std::move(completion));
}
bool ScreenshotOcrRecognitionService::setRenderFilteredImage(RequestToken token, bool enabled, const QColor& backgroundColor) { return m_impl != nullptr && token != 0 && m_impl->setRenderFilteredImage(token, enabled, backgroundColor); }
void ScreenshotOcrRecognitionService::cancel(RequestToken token) { if (m_impl != nullptr && token != 0) m_impl->cancel(token); }
bool ScreenshotOcrRecognitionService::reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) { return m_impl != nullptr && token != 0 && m_impl->reprioritize(token, priority); }
bool ScreenshotOcrRecognitionService::modelFilesReady() const { return m_impl != nullptr && m_impl->modelFilesReady(); }
ScreenshotOcrAssetStatus ScreenshotOcrRecognitionService::assetStatus() const { return m_impl != nullptr ? m_impl->assetStatus() : ScreenshotOcrAssetStatus{}; }
void ScreenshotOcrRecognitionService::setBackendPreference(ScreenshotOcrBackendPreference preference) { if (m_impl != nullptr) m_impl->setBackendPreference(preference); }
void ScreenshotOcrRecognitionService::setProxyUrl(const QString& proxyUrl) { if (m_impl != nullptr) m_impl->setProxyUrl(proxyUrl); }
int ScreenshotOcrRecognitionService::liveWorkerCount() const { return m_impl != nullptr ? m_impl->liveWorkerCount() : 0; }
