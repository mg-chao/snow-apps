#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <exception>
#include <new>
#include <utility>
#include <vector>

namespace {
constexpr int kMaximumPendingJobs = 16;
constexpr int kDefaultShutdownTimeoutMilliseconds = 5000;
constexpr qint64 kMaximumWorkingBytes = qint64{512} * 1024 * 1024;
// Reuse workers for a burst of screenshots, then release their stacks and
// thread-local native resources while the application is idle.
constexpr int kWorkerIdleTimeoutMilliseconds = 5000;

ScreenshotExportTaskResult cancelledResult() {
    return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                               QStringLiteral("The export was cancelled"));
}
} // namespace

ScreenshotExportTaskResult ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage stage,
                                                               QString errorMessage) {
    ScreenshotExportTaskResult result;
    result.failureStage = stage;
    result.error = std::move(errorMessage);
    return result;
}

ScreenshotExportCancellation::ScreenshotExportCancellation(
    std::shared_ptr<std::atomic_bool> cancelled)
    : m_cancelled(std::move(cancelled)) {}

bool ScreenshotExportCancellation::isCancellationRequested() const {
    return m_cancelled == nullptr || m_cancelled->load(std::memory_order_acquire);
}

ScreenshotExportJobHandle::ScreenshotExportJobHandle(std::shared_ptr<std::atomic_bool> cancelled,
                                                     std::function<void()> cancelQueued,
                                                     std::function<void()> promoteQueued)
    : m_cancelled(std::move(cancelled)), m_cancelQueued(std::move(cancelQueued)),
      m_promoteQueued(std::move(promoteQueued)) {}

void ScreenshotExportJobHandle::promoteToForeground() const {
    if (m_promoteQueued)
        m_promoteQueued();
}

void ScreenshotExportJobHandle::cancel() const {
    if (m_cancelled != nullptr) {
        m_cancelled->store(true, std::memory_order_release);
        m_cancelQueued();
    }
}

bool ScreenshotExportJobHandle::isValid() const {
    return m_cancelled != nullptr;
}

bool ScreenshotExportJobHandle::isCancellationRequested() const {
    return !isValid() || m_cancelled->load(std::memory_order_acquire);
}

struct ScreenshotExportCoordinator::Impl final {
    struct Job {
        snow_shot::runtime::RuntimeActivityLease activity =
            snow_shot::runtime::RuntimeActivityTracker::shared().acquire();
        std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
        // Protected by mutex until the worker claims it or cancellation removes it.
        QRunnable* queuedRunnable = nullptr;
        bool foreground = false;
        bool admitted = false;
        qint64 workingBytes = 0;
        Completion complete;
    };

    Impl(int shutdownTimeoutMsValue, qint64 maximumBytes)
        : shutdownTimeoutMs(shutdownTimeoutMsValue), maximumWorkingBytes(maximumBytes) {
        const int ideal = QThread::idealThreadCount();
        pool->setMaxThreadCount(std::clamp(ideal, 1, 2));
        pool->setExpiryTimeout(kWorkerIdleTimeoutMilliseconds);
        pool->setObjectName(QStringLiteral("snow-shot-export"));
    }

    // Never occupy a worker while waiting for bytes. FIFO within each priority also prevents
    // a large head request from being starved by a stream of smaller requests behind it.
    void dispatchLocked() {
        if (!pool || shuttingDown.load())
            return;
        while (activeJobs < pool->maxThreadCount() && !waiting.empty()) {
            auto next = std::find_if(waiting.begin(), waiting.end(),
                                     [](const auto& job) { return job->foreground; });
            if (next == waiting.end())
                next = waiting.begin();
            const auto job = *next;
            if (activeJobs > 0 && (activeBytes > maximumWorkingBytes ||
                                   job->workingBytes > maximumWorkingBytes - activeBytes))
                return;
            waiting.erase(next);
            job->admitted = true;
            ++activeJobs;
            activeBytes += job->workingBytes;
            pool->start(job->queuedRunnable, job->foreground ? 1 : -1);
        }
    }

    void releaseLocked(const std::shared_ptr<Job>& job) {
        if (job && job->admitted) {
            job->admitted = false;
            --activeJobs;
            activeBytes -= job->workingBytes;
        }
    }

    void finish(const std::shared_ptr<Job>& job) {
        QMutexLocker lock(&mutex);
        releaseLocked(job);
        dispatchLocked();
    }

    bool reservePending() {
        int observed = pending.load(std::memory_order_acquire);
        while (observed < kMaximumPendingJobs) {
            if (pending.compare_exchange_weak(observed, observed + 1, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    void cancelQueued(const std::shared_ptr<Job>& job, bool notify = true) {
        QRunnable* removed = nullptr;
        {
            QMutexLocker lock(&mutex);
            if (!pool || !job->queuedRunnable)
                return;
            if (job->admitted) {
                if (!pool->tryTake(job->queuedRunnable))
                    return;
                releaseLocked(job);
            } else {
                std::erase(waiting, job);
            }
            removed = std::exchange(job->queuedRunnable, nullptr);
            dispatchLocked();
        }
        // The worker clears queuedRunnable under the same lock before running.
        // Thus tryTake never receives an auto-deleted pointer (or an ABA reuse).
        if (notify)
            job->complete(cancelledResult());
        else
            pending.fetch_sub(1, std::memory_order_acq_rel);
        delete removed;
    }

    void promoteQueued(const std::shared_ptr<Job>& job) {
        QMutexLocker lock(&mutex);
        if (!pool || shuttingDown.load() || job->cancelled->load() || job->foreground ||
            !job->queuedRunnable)
            return;
        if (!job->admitted) {
            job->foreground = true;
            std::erase(waiting, job);
            waiting.push_back(job);
            dispatchLocked();
            return;
        }
        if (!pool->tryTake(job->queuedRunnable))
            return;
        // The same mutex protects the worker's claim and queued cancellation. Requeueing keeps
        // the job's admission, resource leases, cancellation token and exactly-once completion.
        job->foreground = true;
        pool->start(job->queuedRunnable, 1);
    }

    const int shutdownTimeoutMs;
    const qint64 maximumWorkingBytes;
    qint64 activeBytes = 0;
    int activeJobs = 0;
    std::unique_ptr<QThreadPool> pool = std::make_unique<QThreadPool>();
    std::atomic_int pending{0};
    std::atomic_bool shuttingDown{false};
    QMutex mutex;
    std::vector<std::weak_ptr<Job>> jobs;
    std::vector<std::shared_ptr<Job>> waiting;
    QList<QPointer<ScreenshotExportQueue>> queues;
};

ScreenshotExportCoordinator::ScreenshotExportCoordinator(QObject* parent)
    : ScreenshotExportCoordinator(kDefaultShutdownTimeoutMilliseconds, parent) {}

ScreenshotExportCoordinator::ScreenshotExportCoordinator(int shutdownTimeoutMilliseconds,
                                                         QObject* parent)
    : ScreenshotExportCoordinator(shutdownTimeoutMilliseconds, kMaximumWorkingBytes, parent) {}

ScreenshotExportCoordinator::ScreenshotExportCoordinator(int shutdownTimeoutMilliseconds,
                                                         qint64 maximumWorkingBytes,
                                                         QObject* parent)
    : QObject(parent), m_impl(std::make_shared<Impl>(std::max(1, shutdownTimeoutMilliseconds),
                                                     std::max(qint64{1}, maximumWorkingBytes))) {
    assertOwnerThread();
}

void ScreenshotExportCoordinator::assertOwnerThread() const {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!QCoreApplication::instance() || thread() == QCoreApplication::instance()->thread());
}

ScreenshotExportCoordinator::~ScreenshotExportCoordinator() {
    shutdown();
}

ScreenshotExportCoordinator& ScreenshotExportCoordinator::shared() {
    static ScreenshotExportCoordinator* coordinator = []() {
        auto* instance = new ScreenshotExportCoordinator(QCoreApplication::instance());
        return instance;
    }();
    return *coordinator;
}

ScreenshotExportJobHandle ScreenshotExportCoordinator::submit(QObject* receiver, Priority priority,
                                                              Work work, Completion completion,
                                                              qint64 estimatedWorkingBytes) {
    assertOwnerThread();
    if (receiver == nullptr || !work || !completion || estimatedWorkingBytes < 0 ||
        m_impl->shuttingDown.load(std::memory_order_acquire) || !m_impl->reservePending()) {
        return {};
    }

    const auto state = m_impl;
    auto job = std::make_shared<Impl::Job>();
    job->workingBytes = estimatedWorkingBytes;
    auto terminal = std::make_shared<std::atomic_bool>(false);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<ScreenshotExportCoordinator> guardedCoordinator(this);
    job->complete = [state, weakJob = std::weak_ptr(job), guardedCoordinator, guardedReceiver,
                     terminal, activity = job->activity, completion = std::move(completion)](
                        ScreenshotExportTaskResult result) mutable {
        state->pending.fetch_sub(1, std::memory_order_acq_rel);
        const auto finishedJob = weakJob.lock();
        if (guardedCoordinator.isNull()) {
            state->finish(finishedJob);
            return;
        }
        auto sharedResult = std::make_shared<ScreenshotExportTaskResult>(std::move(result));
        static_cast<void>(QMetaObject::invokeMethod(
            guardedCoordinator,
            [state, finishedJob, guardedCoordinator, guardedReceiver, terminal, sharedResult,
             activity, completion = std::move(completion)]() mutable {
                // The output becomes consumer-owned at delivery. Release before user callbacks,
                // which may open a native modal dialog or synchronously enqueue dependent work.
                state->finish(finishedJob);
                static_cast<void>(activity);
                if (!terminal->exchange(true, std::memory_order_acq_rel) && guardedReceiver)
                    completion(std::move(*sharedResult));
                // A freed slot also wakes queues whose own first job has not yet been admitted.
                if (guardedCoordinator)
                    guardedCoordinator->dispatchQueues();
            },
            Qt::QueuedConnection));
    };
    auto runnable = QRunnable::create([state, job, work = std::move(work)]() mutable {
        snow_shot::platform::applyApplicationQoSToCurrentThread();
        {
            QMutexLocker lock(&state->mutex);
            job->queuedRunnable = nullptr;
        }
        ScreenshotExportTaskResult result;
        const ScreenshotExportCancellation token(job->cancelled);
        if (token.isCancellationRequested()) {
            result = cancelledResult();
        } else {
            try {
                result = work(token);
            } catch (const std::bad_alloc&) {
                result = ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Internal,
                    QStringLiteral("The export ran out of memory"));
            } catch (const std::exception& error) {
                result = ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Internal,
                                                             QString::fromUtf8(error.what()));
            } catch (...) {
                result = ScreenshotExportTaskResult::failure(
                    ScreenshotExportFailureStage::Internal,
                    QStringLiteral("The export failed unexpectedly"));
            }
        }

        job->complete(std::move(result));
    });
    runnable->setAutoDelete(true);
    {
        QMutexLocker lock(&state->mutex);
        state->jobs.erase(std::remove_if(state->jobs.begin(), state->jobs.end(),
                                         [](const auto& value) { return value.expired(); }),
                          state->jobs.end());
        state->jobs.push_back(job);
        job->queuedRunnable = runnable;
        job->foreground = priority == Priority::Foreground;
        state->waiting.push_back(job);
        state->dispatchLocked();
    }
    const std::weak_ptr<Impl> weakState = state;
    const std::weak_ptr<Impl::Job> weakJob = job;
    return ScreenshotExportJobHandle(
        job->cancelled,
        [weakState, weakJob] {
            if (const auto activeState = weakState.lock())
                if (const auto activeJob = weakJob.lock())
                    activeState->cancelQueued(activeJob);
        },
        [weakState, weakJob] {
            if (const auto activeState = weakState.lock())
                if (const auto activeJob = weakJob.lock())
                    activeState->promoteQueued(activeJob);
        });
}

int ScreenshotExportCoordinator::pendingJobCount() const {
    return m_impl->pending.load(std::memory_order_acquire);
}

qint64 ScreenshotExportCoordinator::activeWorkingBytes() const {
    QMutexLocker lock(&m_impl->mutex);
    return m_impl->activeBytes;
}

bool ScreenshotExportCoordinator::acceptingSubmissions() const {
    return !m_impl->shuttingDown.load(std::memory_order_acquire);
}

void ScreenshotExportCoordinator::dispatchQueues() {
    assertOwnerThread();
    const auto queues = m_impl->queues;
    for (const auto& queue : queues)
        if (queue)
            queue->dispatch();
}

void ScreenshotExportCoordinator::shutdown() {
    assertOwnerThread();
    if (m_impl == nullptr || m_impl->shuttingDown.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const auto queues = m_impl->queues;
    for (const auto& queue : queues)
        if (queue)
            queue->cancel();
    std::vector<std::shared_ptr<Impl::Job>> jobs;
    {
        QMutexLocker lock(&m_impl->mutex);
        for (const auto& weak : m_impl->jobs) {
            if (const auto job = weak.lock()) {
                job->cancelled->store(true, std::memory_order_release);
                jobs.push_back(job);
            }
        }
        m_impl->jobs.clear();
    }
    // Deleting the pool would wait for stragglers unconditionally; orphan it
    // past the deadline so in-flight runnables finish against their QPointer
    // guards instead of stalling teardown.
    const bool drained = m_impl->pool->waitForDone(m_impl->shutdownTimeoutMs);
    // Byte-waiting jobs are outside the pool. Preserve normal shutdown's cancellation delivery,
    // but suppress abandoned queued work just as for the original pool-owned queue.
    for (const auto& job : jobs)
        m_impl->cancelQueued(job, drained);
    if (!drained) {
        qWarning("Screenshot export workers did not stop within %d milliseconds; abandoning them",
                 m_impl->shutdownTimeoutMs);
        QMutexLocker lock(&m_impl->mutex);
        // An expiry timeout of 0 makes a worker exit the next time it parks (QThreadPool
        // re-reads the timeout on every park). Workers already parked under the
        // ordinary finite timeout also retire without needing throwaway jobs.
        m_impl->pool->setExpiryTimeout(0);
        static_cast<void>(m_impl->pool.release());
    }
}

ScreenshotExportQueue::ScreenshotExportQueue(ScreenshotExportCoordinator& coordinator,
                                             QObject* parent)
    : QObject(parent), m_coordinator(&coordinator) {
    coordinator.assertOwnerThread();
    Q_ASSERT(QThread::currentThread() == thread());
    coordinator.m_impl->queues.append(this);
}

ScreenshotExportQueue::~ScreenshotExportQueue() {
    Q_ASSERT(QThread::currentThread() == thread());
    cancel();
    if (m_coordinator)
        m_coordinator->m_impl->queues.removeAll(this);
}

bool ScreenshotExportQueue::enqueue(ScreenshotExportCoordinator::Priority priority,
                                    ScreenshotExportCoordinator::Work work,
                                    ScreenshotExportCoordinator::Completion completion,
                                    qint64 estimatedWorkingBytes) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (!m_coordinator || !m_coordinator->acceptingSubmissions() || !work || !completion ||
        estimatedWorkingBytes < 0)
        return false;
    m_requests.enqueue({priority, std::move(work), std::move(completion), estimatedWorkingBytes});
    dispatch();
    return true;
}

void ScreenshotExportQueue::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    ++m_generation;
    m_requests.clear();
    const auto jobs = std::exchange(m_jobs, {});
    for (const auto& job : jobs)
        job.cancel();
}

bool ScreenshotExportQueue::active() const {
    Q_ASSERT(QThread::currentThread() == thread());
    return !m_requests.isEmpty() || !m_jobs.isEmpty();
}

void ScreenshotExportQueue::dispatch() {
    Q_ASSERT(QThread::currentThread() == thread());
    constexpr qsizetype maximumConcurrentJobs = 2;
    while (m_coordinator && m_coordinator->acceptingSubmissions() &&
           m_jobs.size() < maximumConcurrentJobs && !m_requests.isEmpty()) {
        const auto& request = m_requests.head();
        const quint64 id = ++m_nextId;
        auto job = m_coordinator->submit(
            this, request.priority, request.work,
            [this, generation = m_generation, id,
             completion = request.completion](ScreenshotExportTaskResult result) mutable {
                if (generation != m_generation)
                    return;
                m_jobs.remove(id);
                const QPointer<ScreenshotExportQueue> guard(this);
                completion(std::move(result));
                if (guard)
                    dispatch();
            },
            request.estimatedWorkingBytes);
        if (!job.isValid())
            return;
        m_requests.dequeue();
        m_jobs.insert(id, std::move(job));
    }
}
