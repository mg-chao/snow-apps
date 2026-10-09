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
                                                     std::function<void()> cancelQueued)
    : m_cancelled(std::move(cancelled)), m_cancelQueued(std::move(cancelQueued)) {}

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
        Completion complete;
    };

    explicit Impl(int shutdownTimeoutMsValue) : shutdownTimeoutMs(shutdownTimeoutMsValue) {
        const int ideal = QThread::idealThreadCount();
        pool->setMaxThreadCount(std::clamp(ideal, 1, 2));
        pool->setExpiryTimeout(kWorkerIdleTimeoutMilliseconds);
        pool->setObjectName(QStringLiteral("snow-shot-export"));
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
            if (!pool || !job->queuedRunnable || !pool->tryTake(job->queuedRunnable))
                return;
            removed = std::exchange(job->queuedRunnable, nullptr);
        }
        // The worker clears queuedRunnable under the same lock before running.
        // Thus tryTake never receives an auto-deleted pointer (or an ABA reuse).
        if (notify)
            job->complete(cancelledResult());
        else
            pending.fetch_sub(1, std::memory_order_acq_rel);
        delete removed;
    }

    const int shutdownTimeoutMs;
    std::unique_ptr<QThreadPool> pool = std::make_unique<QThreadPool>();
    std::atomic_int pending{0};
    std::atomic_bool shuttingDown{false};
    QMutex mutex;
    std::vector<std::weak_ptr<Job>> jobs;
};

ScreenshotExportCoordinator::ScreenshotExportCoordinator(QObject* parent)
    : ScreenshotExportCoordinator(kDefaultShutdownTimeoutMilliseconds, parent) {}

ScreenshotExportCoordinator::ScreenshotExportCoordinator(int shutdownTimeoutMilliseconds,
                                                         QObject* parent)
    : QObject(parent), m_impl(std::make_shared<Impl>(std::max(1, shutdownTimeoutMilliseconds))) {}

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
                                                              Work work, Completion completion) {
    if (receiver == nullptr || !work || !completion ||
        m_impl->shuttingDown.load(std::memory_order_acquire) || !m_impl->reservePending()) {
        return {};
    }

    const auto state = m_impl;
    auto job = std::make_shared<Impl::Job>();
    auto terminal = std::make_shared<std::atomic_bool>(false);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<ScreenshotExportCoordinator> guardedCoordinator(this);
    job->complete = [state, guardedCoordinator, guardedReceiver, terminal, activity = job->activity,
                     completion =
                         std::move(completion)](ScreenshotExportTaskResult result) mutable {
        state->pending.fetch_sub(1, std::memory_order_acq_rel);
        if (guardedCoordinator.isNull())
            return;
        auto sharedResult = std::make_shared<ScreenshotExportTaskResult>(std::move(result));
        static_cast<void>(QMetaObject::invokeMethod(
            guardedCoordinator,
            [guardedReceiver, terminal, sharedResult, activity,
             completion = std::move(completion)]() mutable {
                static_cast<void>(activity);
                if (terminal->exchange(true, std::memory_order_acq_rel) || guardedReceiver.isNull())
                    return;
                completion(std::move(*sharedResult));
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
        state->pool->start(runnable, priority == Priority::Foreground ? 1 : -1);
    }
    return ScreenshotExportJobHandle(job->cancelled, [weakState = std::weak_ptr<Impl>(state),
                                                      weakJob = std::weak_ptr<Impl::Job>(job)] {
        const auto activeState = weakState.lock();
        const auto activeJob = weakJob.lock();
        if (activeState && activeJob)
            activeState->cancelQueued(activeJob);
    });
}

int ScreenshotExportCoordinator::pendingJobCount() const {
    return m_impl->pending.load(std::memory_order_acquire);
}

void ScreenshotExportCoordinator::shutdown() {
    if (m_impl == nullptr || m_impl->shuttingDown.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
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
    if (!m_impl->pool->waitForDone(m_impl->shutdownTimeoutMs)) {
        qWarning("Screenshot export workers did not stop within %d milliseconds; abandoning them",
                 m_impl->shutdownTimeoutMs);
        // Retire what the deadline could not, without delivering abandoned work.
        for (const auto& job : jobs)
            m_impl->cancelQueued(job, false);
        QMutexLocker lock(&m_impl->mutex);
        // An expiry timeout of 0 makes a worker exit the next time it parks (QThreadPool
        // re-reads the timeout on every park). Workers already parked under the
        // ordinary finite timeout also retire without needing throwaway jobs.
        m_impl->pool->setExpiryTimeout(0);
        static_cast<void>(m_impl->pool.release());
    }
}
