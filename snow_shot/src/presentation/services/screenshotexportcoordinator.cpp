#include "snow_shot/presentation/screenshotexportcoordinator.h"

#include "snow_shot/presentation/screenshotasyncactivitytracker.h"

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

ScreenshotExportJobHandle::ScreenshotExportJobHandle(std::shared_ptr<std::atomic_bool> cancelled)
    : m_cancelled(std::move(cancelled)) {}

void ScreenshotExportJobHandle::cancel() const {
    if (m_cancelled != nullptr) {
        m_cancelled->store(true, std::memory_order_release);
    }
}

bool ScreenshotExportJobHandle::isValid() const {
    return m_cancelled != nullptr;
}

bool ScreenshotExportJobHandle::isCancellationRequested() const {
    return !isValid() || m_cancelled->load(std::memory_order_acquire);
}

struct ScreenshotExportCoordinator::Impl final {
    explicit Impl(ScreenshotExportCoordinator& ownerValue) : owner(ownerValue) {
        const int ideal = QThread::idealThreadCount();
        pool.setMaxThreadCount(std::clamp(ideal, 1, 2));
        pool.setExpiryTimeout(-1);
        pool.setObjectName(QStringLiteral("snow-shot-export"));
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

    void track(const std::shared_ptr<std::atomic_bool>& cancellation) {
        QMutexLocker lock(&mutex);
        cancellations.erase(std::remove_if(cancellations.begin(), cancellations.end(),
                                           [](const auto& value) { return value.expired(); }),
                            cancellations.end());
        cancellations.push_back(cancellation);
    }

    ScreenshotExportCoordinator& owner;
    QThreadPool pool;
    std::atomic_int pending{0};
    std::atomic_bool shuttingDown{false};
    QMutex mutex;
    std::vector<std::weak_ptr<std::atomic_bool>> cancellations;
};

ScreenshotExportCoordinator::ScreenshotExportCoordinator(QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(*this)) {}

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

    auto cancellation = std::make_shared<std::atomic_bool>(false);
    auto terminal = std::make_shared<std::atomic_bool>(false);
    ScreenshotAsyncActivityLease activityLease =
        ScreenshotAsyncActivityTracker::shared().acquire();
    m_impl->track(cancellation);
    const QPointer<QObject> guardedReceiver(receiver);
    const QPointer<ScreenshotExportCoordinator> guardedCoordinator(this);
    auto runnable =
        QRunnable::create([guardedCoordinator, guardedReceiver, cancellation, terminal,
                           activityLease = std::move(activityLease),
                           work = std::move(work), completion = std::move(completion)]() mutable {
            ScreenshotExportTaskResult result;
            const ScreenshotExportCancellation token(cancellation);
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
                    result = ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Internal, QString::fromUtf8(error.what()));
                } catch (...) {
                    result = ScreenshotExportTaskResult::failure(
                        ScreenshotExportFailureStage::Internal,
                        QStringLiteral("The export failed unexpectedly"));
                }
            }

            if (guardedCoordinator.isNull()) {
                return;
            }
            // Work closures commonly retain the source image. Release them before publishing the
            // terminal callback; the activity lease remains alive through result delivery.
            work = {};
            guardedCoordinator->m_impl->pending.fetch_sub(1, std::memory_order_acq_rel);
            auto sharedResult = std::make_shared<ScreenshotExportTaskResult>(std::move(result));
            static_cast<void>(QMetaObject::invokeMethod(
                guardedCoordinator,
                [guardedReceiver, terminal, sharedResult,
                 completion = std::move(completion),
                 activityLease = std::move(activityLease)]() mutable {
                    if (!terminal->exchange(true, std::memory_order_acq_rel) &&
                        !guardedReceiver.isNull()) {
                        completion(std::move(*sharedResult));
                    }
                    completion = {};
                    sharedResult.reset();
                    activityLease.reset();
                },
                Qt::QueuedConnection));
        });
    runnable->setAutoDelete(true);
    m_impl->pool.start(runnable, priority == Priority::Foreground ? 1 : -1);
    return ScreenshotExportJobHandle(std::move(cancellation));
}

int ScreenshotExportCoordinator::pendingJobCount() const {
    return m_impl->pending.load(std::memory_order_acquire);
}

void ScreenshotExportCoordinator::shutdown() {
    if (m_impl == nullptr || m_impl->shuttingDown.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    {
        QMutexLocker lock(&m_impl->mutex);
        for (const auto& weak : m_impl->cancellations) {
            if (const auto cancellation = weak.lock(); cancellation != nullptr) {
                cancellation->store(true, std::memory_order_release);
            }
        }
        m_impl->cancellations.clear();
    }
    m_impl->pool.waitForDone();
}
