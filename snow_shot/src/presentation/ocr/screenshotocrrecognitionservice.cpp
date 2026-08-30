#include "snow_shot/presentation/screenshotocrrecognitionservice.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"

#include "snow_ocr_c.h"

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
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {
struct OcrEngineDeleter {
    void operator()(SnowOcrEngine* engine) const {
        snow_ocr_engine_destroy(engine);
    }
};

struct OcrResultDeleter {
    void operator()(SnowOcrResult* result) const {
        snow_ocr_result_destroy(result);
    }
};

using OcrEngineHandle = std::unique_ptr<SnowOcrEngine, OcrEngineDeleter>;
using OcrResultHandle = std::unique_ptr<SnowOcrResult, OcrResultDeleter>;

QString lastOcrError() {
    const char* message = snow_ocr_last_error_message();
    if (message == nullptr || *message == '\0') {
        return QCoreApplication::translate("ScreenshotOcrController", "Text recognition failed");
    }
    return QString::fromUtf8(message);
}

QPolygonF quadFromFfi(const SnowOcrQuad& quad, const QRectF& canvasRect, const QSize& imageSize) {
    const qreal scaleX = imageSize.width() > 0 ? canvasRect.width() / imageSize.width() : 1.0;
    const qreal scaleY = imageSize.height() > 0 ? canvasRect.height() / imageSize.height() : 1.0;
    QPolygonF polygon;
    polygon.reserve(4);
    for (int index = 0; index < 4; ++index) {
        polygon.push_back(
            QPointF(canvasRect.left() + static_cast<qreal>(quad.points[index * 2]) * scaleX,
                    canvasRect.top() + static_cast<qreal>(quad.points[index * 2 + 1]) * scaleY));
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
        std::max(edgeLength(quad.at(0), quad.at(1)), edgeLength(quad.at(3), quad.at(2)));
    const qreal height =
        std::max(edgeLength(quad.at(0), quad.at(3)), edgeLength(quad.at(1), quad.at(2)));
    return height >= width * kVerticalAspectRatio ? ScreenshotOcrTextDirection::Vertical
                                                  : ScreenshotOcrTextDirection::Horizontal;
}

OcrEngineHandle createEngine(ScreenshotOcrBackendPreference preference) {
    SnowOcrRuntimeInfoV1 runtimeInfo{
        static_cast<std::uint32_t>(sizeof(SnowOcrRuntimeInfoV1)), 0};
    const std::uint32_t physicalCores =
        snow_ocr_runtime_info_v1(&runtimeInfo) != 0 ? runtimeInfo.physical_core_count : 1;
    const std::uint32_t threadBudget = (std::max)(1u, physicalCores / 2u);
    const bool preferDirectMl = preference == ScreenshotOcrBackendPreference::DirectMl;
    const SnowOcrEngineConfigV2 config{
        static_cast<std::uint32_t>(sizeof(SnowOcrEngineConfigV2)),
        threadBudget,
        1u,
        threadBudget,
        1,
        static_cast<std::uint8_t>(preferDirectMl ? 1 : 0),
        {0, 0},
    };
    return OcrEngineHandle(snow_ocr_engine_create_with_config_v2(&config));
}

ScreenshotOcrRecognitionResult runRecognition(OcrEngineHandle& engine, QImage source,
                                              const QRectF& canvasRect) {
    if (engine == nullptr) {
        return {nullptr, lastOcrError()};
    }
    if (source.format() != QImage::Format_RGBA8888) {
        source = source.convertToFormat(QImage::Format_RGBA8888);
    }
    if (source.isNull()) {
        return {nullptr, QCoreApplication::translate("ScreenshotOcrController",
                                                     "Text recognition failed")};
    }

    const SnowOcrRequestV1 request{
        static_cast<std::uint32_t>(sizeof(SnowOcrRequestV1)),
        static_cast<std::uint32_t>(source.width()),
        static_cast<std::uint32_t>(source.height()),
        static_cast<std::uint32_t>(source.bytesPerLine()),
        source.constBits(),
        static_cast<std::size_t>(source.sizeInBytes()),
    };
    OcrResultHandle result(snow_ocr_engine_recognize_rgba(engine.get(), &request));
    if (result == nullptr) {
        return {nullptr, lastOcrError()};
    }

    ScreenshotOcrRecognitionResult output;
    output.presentation = std::make_shared<ScreenshotOcrPresentation>();
    output.presentation->selection = canvasRect.toAlignedRect();
    const std::size_t lineCount = snow_ocr_result_line_count(result.get());
    if (lineCount > static_cast<std::size_t>((std::numeric_limits<qsizetype>::max)())) {
        return {nullptr, QCoreApplication::translate("ScreenshotOcrController",
                                                     "Text recognition failed")};
    }
    output.presentation->lines.reserve(static_cast<qsizetype>(lineCount));
    for (std::size_t lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
        SnowOcrLineInfoV1 lineInfo{};
        lineInfo.struct_size = static_cast<std::uint32_t>(sizeof(SnowOcrLineInfoV1));
        if (snow_ocr_result_line(result.get(), lineIndex, &lineInfo) == 0 ||
            (lineInfo.text_len > 0 && lineInfo.text_utf8 == nullptr) ||
            lineInfo.text_len >
                static_cast<std::size_t>((std::numeric_limits<qsizetype>::max)())) {
            return {nullptr, lastOcrError()};
        }
        ScreenshotOcrLine line;
        line.text = QString::fromUtf8(reinterpret_cast<const char*>(lineInfo.text_utf8),
                                      static_cast<qsizetype>(lineInfo.text_len));
        line.confidence = static_cast<qreal>(lineInfo.confidence);
        line.quad = quadFromFfi(lineInfo.quad, canvasRect, source.size());
        line.direction = textDirectionForQuad(line.quad);
        output.presentation->lines.push_back(std::move(line));
    }
    output.presentation->prepareForRendering();
    return output;
}
} // namespace

class ScreenshotOcrRecognitionService::Impl final {
  public:
    Impl(ScreenshotOcrRecognitionService* owner, const Options& options,
         ScreenshotOcrBackendPreference preference)
        : m_owner(owner),
          m_workerLimit(std::clamp(options.workerCount, 1, 2)),
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
        job->receiverDestroyed = QObject::connect(
            receiver, &QObject::destroyed, m_owner, [service, token]() {
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
            ++m_backendGeneration;
        }
    }

    [[nodiscard]] int liveWorkerCount() const {
        std::lock_guard lock(m_mutex);
        return m_liveWorkerCount;
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

    void workerLoop(WorkerSlot* worker) {
        OcrEngineHandle engine;
        quint64 engineGeneration = 0;

        while (true) {
            std::shared_ptr<Job> job;
            ScreenshotOcrBackendPreference preference = ScreenshotOcrBackendPreference::Cpu;
            quint64 generation = 0;
            bool retireEngine = false;
            {
                std::lock_guard lock(m_mutex);
                if (m_stopping) {
                    finishWorkerLocked(worker);
                    return;
                }
                if (m_backendGeneration != engineGeneration) {
                    engineGeneration = m_backendGeneration;
                    retireEngine = true;
                }
                job = takeNextJob();
                if (job == nullptr) {
                    engine.reset();
                    finishWorkerLocked(worker);
                    return;
                }
                preference = m_backendPreference;
                generation = m_backendGeneration;
            }

            if (retireEngine) {
                engine.reset();
            }
            if (job == nullptr) {
                continue;
            }

            ScreenshotOcrRecognitionResult result;
            if (!job->cancelled.load(std::memory_order_acquire)) {
                if (engine == nullptr || engineGeneration != generation) {
                    engine.reset();
                    engine = createEngine(preference);
                    engineGeneration = generation;
                }
                result =
                    runRecognition(engine, std::move(job->request.image), job->request.canvasRect);
            }
            {
                std::lock_guard lock(m_mutex);
                --m_activeWorkerCount;
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
        }
        engine.reset();
    }

    void finishJob(std::shared_ptr<Job> job, ScreenshotOcrRecognitionResult result) {
        {
            std::lock_guard lock(m_mutex);
            const auto request = m_requests.find(job->token);
            if (request == m_requests.end() || request.value() != job) {
                return;
            }
            m_requests.erase(request);
        }

        QObject::disconnect(job->receiverDestroyed);
        if (!job->cancelled.load(std::memory_order_acquire) && job->receiver != nullptr &&
            job->completion) {
            job->completion(std::move(result));
        }
    }

    void shutdown() {
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
    int m_liveWorkerCount = 0;
    int m_activeWorkerCount = 0;
    ScreenshotOcrBackendPreference m_backendPreference = ScreenshotOcrBackendPreference::Cpu;
    quint64 m_backendGeneration = 1;
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

int ScreenshotOcrRecognitionService::liveWorkerCount() const {
    return m_impl != nullptr ? m_impl->liveWorkerCount() : 0;
}
