#include "snow_shot/presentation/screenshotocrrecognitionservice.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"

#include "screenshotocrworkerentry.h"
#include "screenshotocrworkerprotocol.h"
#include "screenshotrecognitionworkerprocess.h"

#include "snow_ocr_c.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QHash>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <Windows.h>
#endif

namespace {
namespace protocol = snow_shot::presentation::ocr_worker_protocol;

QString recognitionFailure() {
    return QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed");
}

QPolygonF quadFromPoints(const float* points, const QRectF& canvasRect, const QSize& imageSize) {
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
    constexpr qreal kVerticalAspectRatio = 1.5;
    if (quad.size() != 4) {
        return ScreenshotOcrTextDirection::Horizontal;
    }
    const qreal width =
        (std::max)(edgeLength(quad.at(0), quad.at(1)), edgeLength(quad.at(3), quad.at(2)));
    const qreal height =
        (std::max)(edgeLength(quad.at(0), quad.at(3)), edgeLength(quad.at(1), quad.at(2)));
    return height >= width * kVerticalAspectRatio ? ScreenshotOcrTextDirection::Vertical
                                                  : ScreenshotOcrTextDirection::Horizontal;
}

QString workerExecutable() {
    const QString configured = qEnvironmentVariable("SNOW_SHOT_OCR_WORKER_EXECUTABLE").trimmed();
    return configured.isEmpty() ? QCoreApplication::applicationFilePath() : configured;
}

using ProcessIoResult = snow_shot::presentation::RecognitionWorkerIoResult;
using WorkerProcess = snow_shot::presentation::RecognitionWorkerProcess;

uchar* allocateImageBuffer(std::size_t length) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    return static_cast<uchar*>(
        VirtualAlloc(nullptr, length, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
    return new (std::nothrow) uchar[length];
#endif
}

void releaseImageBuffer(void* context) {
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (context != nullptr) {
        static_cast<void>(VirtualFree(context, 0, MEM_RELEASE));
    }
#else
    delete[] static_cast<uchar*>(context);
#endif
}

struct ImageBufferDeleter {
    void operator()(uchar* buffer) const {
        releaseImageBuffer(buffer);
    }
};

struct ParsedWorkerResponse {
    ScreenshotOcrRecognitionResult result;
    ProcessIoResult io = ProcessIoResult::Failed;
};

ParsedWorkerResponse parseWorkerResponse(WorkerProcess& process,
                                         const std::atomic_bool& cancelled,
                                         const QRectF& canvasRect, const QSize& sourceSize) {
    protocol::ResponseHeader header;
    ProcessIoResult io = process.readExact(cancelled, &header, sizeof(header));
    if (io != ProcessIoResult::Complete) {
        return {{}, io};
    }
    if (
        header.magic != protocol::kResponseMagic || header.version != protocol::kVersion) {
        return {};
    }
    if (header.status == protocol::kStatusFailure) {
        if (header.width != 0 || header.height != 0 || header.strideBytes != 0 ||
            header.rgbaLength != 0 || header.lineCount != 0 ||
            header.errorLength > protocol::kMaximumTextBytes ||
            header.errorLength >
                static_cast<std::uint64_t>((std::numeric_limits<qsizetype>::max)())) {
            return {};
        }
        QByteArray errorBytes(static_cast<qsizetype>(header.errorLength), Qt::Uninitialized);
        io = process.readExact(cancelled, errorBytes.data(), errorBytes.size());
        if (io != ProcessIoResult::Complete) {
            return {{}, io};
        }
        const QString error = QString::fromUtf8(errorBytes);
        return {{nullptr, error.isEmpty() ? recognitionFailure() : error},
                ProcessIoResult::Complete};
    }
    if (header.status != protocol::kStatusSuccess || header.errorLength != 0 ||
        header.width != static_cast<std::uint32_t>(sourceSize.width()) ||
        header.height != static_cast<std::uint32_t>(sourceSize.height()) ||
        header.width > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
        header.height > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
        header.strideBytes > static_cast<std::uint32_t>((std::numeric_limits<int>::max)()) ||
        header.strideBytes < static_cast<std::uint64_t>(header.width) * 4 ||
        header.rgbaLength != static_cast<std::uint64_t>(header.strideBytes) * header.height ||
        header.rgbaLength > protocol::kMaximumImageBytes ||
        header.lineCount > protocol::kMaximumLineCount ||
        header.rgbaLength >
            static_cast<std::uint64_t>((std::numeric_limits<qsizetype>::max)()) ||
        header.lineCount >
            static_cast<std::uint64_t>((std::numeric_limits<qsizetype>::max)())) {
        return {};
    }

    const qsizetype imageLength = static_cast<qsizetype>(header.rgbaLength);
    std::unique_ptr<uchar, ImageBufferDeleter> imageBuffer(
        allocateImageBuffer(static_cast<std::size_t>(imageLength)));
    if (imageBuffer == nullptr) {
        return {};
    }
    io = process.readExact(cancelled, imageBuffer.get(), imageLength);
    if (io != ProcessIoResult::Complete) {
        return {{}, io};
    }
    QImage filledImage(static_cast<const uchar*>(imageBuffer.get()), static_cast<int>(header.width),
                       static_cast<int>(header.height),
                       static_cast<qsizetype>(header.strideBytes), QImage::Format_RGBA8888,
                       &releaseImageBuffer, imageBuffer.get());
    if (filledImage.isNull()) {
        return {};
    }
    imageBuffer.release();

    ScreenshotOcrRecognitionResult output;
    output.presentation = std::make_shared<ScreenshotOcrPresentation>();
    output.presentation->selection = canvasRect.toAlignedRect();
    output.presentation->filledImage = std::move(filledImage);
    output.presentation->lines.reserve(static_cast<qsizetype>(header.lineCount));
    std::uint64_t totalTextBytes = 0;
    for (std::uint64_t lineIndex = 0; lineIndex < header.lineCount; ++lineIndex) {
        protocol::LineHeader lineHeader;
        io = process.readExact(cancelled, &lineHeader, sizeof(lineHeader));
        if (io != ProcessIoResult::Complete) {
            return {{}, io};
        }
        if (
            lineHeader.textLength > protocol::kMaximumTextBytes - totalTextBytes ||
            lineHeader.textLength >
                static_cast<std::uint64_t>((std::numeric_limits<qsizetype>::max)()) ||
            !std::isfinite(lineHeader.confidence) ||
            !std::all_of(std::begin(lineHeader.points), std::end(lineHeader.points),
                         [](float point) { return std::isfinite(point); })) {
            return {};
        }
        totalTextBytes += lineHeader.textLength;
        QByteArray textBytes(static_cast<qsizetype>(lineHeader.textLength), Qt::Uninitialized);
        io = process.readExact(cancelled, textBytes.data(), textBytes.size());
        if (io != ProcessIoResult::Complete) {
            return {{}, io};
        }
        ScreenshotOcrLine line;
        line.text = QString::fromUtf8(textBytes);
        line.confidence = static_cast<qreal>(lineHeader.confidence);
        line.foreground = QColor(lineHeader.foreground[0], lineHeader.foreground[1],
                                 lineHeader.foreground[2], lineHeader.foreground[3]);
        line.quad = quadFromPoints(lineHeader.points, canvasRect, sourceSize);
        line.direction = textDirectionForQuad(line.quad);
        output.presentation->lines.push_back(std::move(line));
    }
    output.presentation->prepareForRendering();
    return {std::move(output), ProcessIoResult::Complete};
}

ScreenshotOcrRecognitionResult runRecognition(const std::atomic_bool& cancelled,
                                              ScreenshotOcrBackendPreference preference,
                                              QImage source, const QRectF& canvasRect) {
    if (source.format() != QImage::Format_RGBA8888) {
        source = source.convertToFormat(QImage::Format_RGBA8888);
    }
    if (source.isNull() || source.sizeInBytes() <= 0 ||
        static_cast<std::uint64_t>(source.sizeInBytes()) > protocol::kMaximumImageBytes ||
        static_cast<std::uint64_t>(source.bytesPerLine()) * source.height() !=
            static_cast<std::uint64_t>(source.sizeInBytes())) {
        return {nullptr, recognitionFailure()};
    }

    protocol::RequestHeader request;
    request.width = static_cast<std::uint32_t>(source.width());
    request.height = static_cast<std::uint32_t>(source.height());
    request.strideBytes = static_cast<std::uint32_t>(source.bytesPerLine());
    request.useDirectMl =
        static_cast<std::uint8_t>(preference == ScreenshotOcrBackendPreference::DirectMl ? 1 : 0);
    request.rgbaLength = static_cast<std::uint64_t>(source.sizeInBytes());

    std::unique_ptr<WorkerProcess> process = WorkerProcess::start(
        workerExecutable(),
        QString::fromLatin1(snow_shot::presentation::kScreenshotOcrWorkerArgument));
    if (process == nullptr) {
        return {nullptr, recognitionFailure()};
    }
    ProcessIoResult io = process->writeExact(cancelled, &request, sizeof(request));
    if (io == ProcessIoResult::Complete) {
        io = process->writeExact(cancelled, source.constBits(),
                                 static_cast<std::uint64_t>(source.sizeInBytes()));
    }
    process->closeInput();
    if (io != ProcessIoResult::Complete) {
        process->terminate();
        return io == ProcessIoResult::Cancelled ? ScreenshotOcrRecognitionResult{}
                                                : ScreenshotOcrRecognitionResult{
                                                      nullptr, recognitionFailure()};
    }

    ParsedWorkerResponse response =
        parseWorkerResponse(*process, cancelled, canvasRect, source.size());
    if (response.io != ProcessIoResult::Complete) {
        process->terminate();
        return response.io == ProcessIoResult::Cancelled ? ScreenshotOcrRecognitionResult{}
                                                         : ScreenshotOcrRecognitionResult{
                                                               nullptr, recognitionFailure()};
    }
    io = process->waitForFinished(cancelled);
    if (io != ProcessIoResult::Complete) {
        process->terminate();
        return io == ProcessIoResult::Cancelled ? ScreenshotOcrRecognitionResult{}
                                                : ScreenshotOcrRecognitionResult{
                                                      nullptr, recognitionFailure()};
    }
    if (!process->exitedSuccessfullyWithoutOutput()) {
        const QString workerError = process->errorString();
        return {nullptr, workerError.isEmpty() ? recognitionFailure() : workerError};
    }
    return response.result;
}
} // namespace

class ScreenshotOcrRecognitionService::Impl final {
  public:
    Impl(ScreenshotOcrRecognitionService* owner, const Options& options,
         ScreenshotOcrBackendPreference preference)
        : m_owner(owner), m_workerLimit(std::clamp(options.workerCount, 1, 2)),
          m_backendPreference(preference) {
        m_workers.reserve(static_cast<std::size_t>(m_workerLimit));
    }

    ~Impl() {
        shutdown();
    }

    RequestToken enqueue(RequestToken token, ScreenshotOcrRequest request, QObject* receiver,
                         Completion completion) {
        auto job = std::make_shared<Job>();
        job->token = token;
        job->request = std::move(request);
        job->receiver = receiver;
        job->completion = std::move(completion);
        QPointer<ScreenshotOcrRecognitionService> service(m_owner);
        job->receiverDestroyed =
            QObject::connect(receiver, &QObject::destroyed, m_owner, [service, token]() {
                if (service != nullptr) {
                    service->cancel(token);
                }
            });

        bool accepted = false;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                QObject::disconnect(job->receiverDestroyed);
                return 0;
            }
            m_requests.insert(token, job);
            queueFor(job->request.priority).push_back(job);
            startWorkersForDemandLocked();
            accepted = m_liveWorkerCount > 0;
            if (!accepted) {
                eraseQueuedJob(job);
                m_requests.remove(token);
            }
        }
        if (!accepted) {
            QObject::disconnect(job->receiverDestroyed);
            return 0;
        }
        return token;
    }

    void cancel(RequestToken token) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard lock(m_mutex);
            const auto request = m_requests.find(token);
            if (request == m_requests.end()) {
                return;
            }
            job = request.value();
            job->cancelled.store(true, std::memory_order_release);
            if (!job->running) {
                eraseQueuedJob(job);
                m_requests.erase(request);
            }
        }
        QObject::disconnect(job->receiverDestroyed);
    }

    bool reprioritize(RequestToken token, ScreenshotOcrRequestPriority priority) {
        std::lock_guard lock(m_mutex);
        const auto request = m_requests.find(token);
        if (request == m_requests.end() || request.value()->running ||
            request.value()->cancelled.load(std::memory_order_acquire)) {
            return false;
        }
        const std::shared_ptr<Job>& job = request.value();
        if (job->request.priority == priority) {
            return true;
        }
        eraseQueuedJob(job);
        job->request.priority = priority;
        queueFor(priority).push_back(job);
        return true;
    }

    void setBackendPreference(ScreenshotOcrBackendPreference preference) {
        {
            std::lock_guard lock(m_mutex);
            if (m_backendPreference == preference) {
                return;
            }
            m_backendPreference = preference;
        }
    }

    [[nodiscard]] int liveWorkerCount() const {
        std::lock_guard lock(m_mutex);
        return m_liveWorkerCount;
    }

    bool releaseRetainedIdleResources(std::function<void(bool released)> completion) {
        if (!completion) {
            return false;
        }
        m_idleReleaseCompletions.push_back(std::move(completion));
        const QPointer<ScreenshotOcrRecognitionService> service(m_owner);
        return QMetaObject::invokeMethod(
            m_owner,
            [service]() {
                if (service != nullptr && service->m_impl != nullptr) {
                    service->m_impl->notifyIdleReleaseCompletionsIfIdle();
                }
            },
            Qt::QueuedConnection);
    }

  private:
    using RequestToken = ScreenshotOcrRecognitionPort::RequestToken;
    using Completion = ScreenshotOcrRecognitionPort::Completion;
    struct Job {
        RequestToken token = 0;
        ScreenshotOcrRequest request;
        QPointer<QObject> receiver;
        Completion completion;
        QMetaObject::Connection receiverDestroyed;
        std::atomic_bool cancelled{false};
        bool running = false;
    };

    struct WorkerSlot {
        std::thread thread;
        bool finished = false;
    };

    using Queue = std::deque<std::shared_ptr<Job>>;

    Queue& queueFor(ScreenshotOcrRequestPriority priority) {
        return priority == ScreenshotOcrRequestPriority::Interactive ? m_interactiveQueue
                                                                     : m_prefetchQueue;
    }

    void eraseQueuedJob(const std::shared_ptr<Job>& job) {
        Queue& queue = queueFor(job->request.priority);
        const auto position = std::find(queue.begin(), queue.end(), job);
        if (position != queue.end()) {
            queue.erase(position);
        }
    }

    std::shared_ptr<Job> takeNextJob() {
        Queue& queue = !m_interactiveQueue.empty() ? m_interactiveQueue : m_prefetchQueue;
        if (queue.empty()) {
            return {};
        }
        std::shared_ptr<Job> job = std::move(queue.front());
        queue.pop_front();
        job->running = true;
        ++m_activeWorkerCount;
        return job;
    }

    [[nodiscard]] std::size_t queuedJobCountLocked() const {
        return m_interactiveQueue.size() + m_prefetchQueue.size();
    }

    void reapFinishedWorkersLocked() {
        auto worker = m_workers.begin();
        while (worker != m_workers.end()) {
            if (!(*worker)->finished) {
                ++worker;
                continue;
            }
            if ((*worker)->thread.get_id() == std::this_thread::get_id()) {
                ++worker;
                continue;
            }
            if ((*worker)->thread.joinable()) {
                (*worker)->thread.join();
            }
            worker = m_workers.erase(worker);
        }
    }

    [[nodiscard]] bool spawnWorkerLocked() {
        reapFinishedWorkersLocked();
        auto slot = std::make_unique<WorkerSlot>();
        WorkerSlot* const worker = slot.get();
        m_workers.push_back(std::move(slot));
        try {
            worker->thread = std::thread([this, worker]() { workerLoop(worker); });
        } catch (const std::system_error& error) {
            qWarning() << "Unable to create OCR worker thread:" << error.what();
            m_workers.pop_back();
            return false;
        }
        ++m_liveWorkerCount;
        return true;
    }

    void startWorkersForDemandLocked() {
        const std::size_t demand =
            (std::min)(static_cast<std::size_t>(m_workerLimit),
                       queuedJobCountLocked() + static_cast<std::size_t>(m_activeWorkerCount));
        while (static_cast<std::size_t>(m_liveWorkerCount) < demand) {
            if (!spawnWorkerLocked()) {
                break;
            }
        }
    }

    void finishWorkerLocked(WorkerSlot* worker) {
        --m_liveWorkerCount;
        worker->finished = true;
    }

    void queueIdleReleaseCheck() {
        const QPointer<ScreenshotOcrRecognitionService> service(m_owner);
        static_cast<void>(QMetaObject::invokeMethod(
            m_owner,
            [service]() {
                if (service != nullptr && service->m_impl != nullptr) {
                    service->m_impl->notifyIdleReleaseCompletionsIfIdle();
                }
            },
            Qt::QueuedConnection));
    }

    void workerLoop(WorkerSlot* worker) {
        while (true) {
            std::shared_ptr<Job> job;
            ScreenshotOcrBackendPreference preference = ScreenshotOcrBackendPreference::Cpu;
            {
                std::lock_guard lock(m_mutex);
                if (m_stopping) {
                    finishWorkerLocked(worker);
                    queueIdleReleaseCheck();
                    return;
                }
                job = takeNextJob();
                if (job == nullptr) {
                    finishWorkerLocked(worker);
                    queueIdleReleaseCheck();
                    return;
                }
                preference = m_backendPreference;
            }

            ScreenshotOcrRecognitionResult result;
            if (!job->cancelled.load(std::memory_order_acquire)) {
                result = runRecognition(job->cancelled, preference, std::move(job->request.image),
                                        job->request.canvasRect);
            }

            bool keepWorker = false;
            {
                std::lock_guard lock(m_mutex);
                --m_activeWorkerCount;
                keepWorker = !m_stopping && queuedJobCountLocked() > 0;
                if (!keepWorker) {
                    finishWorkerLocked(worker);
                }
            }

            QPointer<ScreenshotOcrRecognitionService> service(m_owner);
            QMetaObject::invokeMethod(
                m_owner,
                [service, job = std::move(job), result = std::move(result)]() mutable {
                    if (service != nullptr && service->m_impl != nullptr) {
                        service->m_impl->finishJob(std::move(job), std::move(result));
                    }
                },
                Qt::QueuedConnection);

            if (!keepWorker) {
                return;
            }
        }
    }

    void finishJob(std::shared_ptr<Job> job, ScreenshotOcrRecognitionResult result) {
        bool deliver = false;
        {
            std::lock_guard lock(m_mutex);
            reapFinishedWorkersLocked();
            const auto request = m_requests.find(job->token);
            if (request != m_requests.end() && request.value() == job) {
                m_requests.erase(request);
                deliver = true;
            }
        }

        if (deliver) {
            QObject::disconnect(job->receiverDestroyed);
            if (!job->cancelled.load(std::memory_order_acquire) && job->receiver != nullptr &&
                job->completion) {
                job->completion(std::move(result));
            }
        }
        notifyIdleReleaseCompletionsIfIdle();
    }

    void notifyIdleReleaseCompletionsIfIdle() {
        {
            std::lock_guard lock(m_mutex);
            reapFinishedWorkersLocked();
            if (m_liveWorkerCount > 0 || m_activeWorkerCount > 0 || queuedJobCountLocked() > 0 ||
                !m_requests.isEmpty()) {
                return;
            }
        }
        std::vector<std::function<void(bool)>> completions =
            std::exchange(m_idleReleaseCompletions, {});
        for (auto& completion : completions) {
            completion(true);
        }
    }

    void shutdown() {
        m_idleReleaseCompletions.clear();
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                return;
            }
            m_stopping = true;
            for (auto request = m_requests.begin(); request != m_requests.end(); ++request) {
                request.value()->cancelled.store(true, std::memory_order_release);
                QObject::disconnect(request.value()->receiverDestroyed);
            }
            m_interactiveQueue.clear();
            m_prefetchQueue.clear();
            m_requests.clear();
        }
        for (const std::unique_ptr<WorkerSlot>& worker : m_workers) {
            if (worker->thread.joinable()) {
                worker->thread.join();
            }
        }
        m_workers.clear();
    }

    ScreenshotOcrRecognitionService* m_owner = nullptr;
    const int m_workerLimit;
    mutable std::mutex m_mutex;
    Queue m_interactiveQueue;
    Queue m_prefetchQueue;
    QHash<RequestToken, std::shared_ptr<Job>> m_requests;
    std::vector<std::unique_ptr<WorkerSlot>> m_workers;
    std::vector<std::function<void(bool)>> m_idleReleaseCompletions;
    int m_liveWorkerCount = 0;
    int m_activeWorkerCount = 0;
    ScreenshotOcrBackendPreference m_backendPreference = ScreenshotOcrBackendPreference::Cpu;
    bool m_stopping = false;
};

ScreenshotOcrRecognitionService::ScreenshotOcrRecognitionService(QObject* parent)
    : ScreenshotOcrRecognitionService(Options{}, ScreenshotOcrBackendPreference::Cpu, parent) {}

ScreenshotOcrRecognitionService::ScreenshotOcrRecognitionService(
    ScreenshotOcrBackendPreference backendPreference, QObject* parent)
    : ScreenshotOcrRecognitionService(Options{}, backendPreference, parent) {}

ScreenshotOcrRecognitionService::ScreenshotOcrRecognitionService(
    const Options& options, ScreenshotOcrBackendPreference backendPreference, QObject* parent)
    : ScreenshotOcrRecognitionPort(parent),
      m_impl(std::make_unique<Impl>(this, options, backendPreference)) {}

ScreenshotOcrRecognitionService::~ScreenshotOcrRecognitionService() = default;

ScreenshotOcrRecognitionPort::RequestToken
ScreenshotOcrRecognitionService::recognize(ScreenshotOcrRequest request, QObject* receiver,
                                           Completion completion) {
    if (request.image.isNull() || !request.canvasRect.isValid() || request.canvasRect.isEmpty() ||
        receiver == nullptr || !completion || m_impl == nullptr) {
        return 0;
    }
    do {
        ++m_nextToken;
    } while (m_nextToken == 0);
    return m_impl->enqueue(m_nextToken, std::move(request), receiver, std::move(completion));
}

void ScreenshotOcrRecognitionService::cancel(RequestToken token) {
    if (m_impl != nullptr && token != 0) {
        m_impl->cancel(token);
    }
}

bool ScreenshotOcrRecognitionService::reprioritize(RequestToken token,
                                                   ScreenshotOcrRequestPriority priority) {
    return m_impl != nullptr && token != 0 && m_impl->reprioritize(token, priority);
}

void ScreenshotOcrRecognitionService::setBackendPreference(
    ScreenshotOcrBackendPreference preference) {
    if (m_impl != nullptr) {
        m_impl->setBackendPreference(preference);
    }
}

bool ScreenshotOcrRecognitionService::prewarmRuntime() {
    return snow_ocr_runtime_initialize() != 0;
}

bool ScreenshotOcrRecognitionService::releaseRetainedIdleResources(
    std::function<void(bool released)> completion) {
    return m_impl != nullptr && m_impl->releaseRetainedIdleResources(std::move(completion));
}

int ScreenshotOcrRecognitionService::liveWorkerCount() const {
    return m_impl != nullptr ? m_impl->liveWorkerCount() : 0;
}
