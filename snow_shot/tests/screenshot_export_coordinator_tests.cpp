#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotexportcache.h"
#include "snow_shot/runtime/runtimeactivitytracker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSet>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool processUntil(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

void exportCacheBoundsAggregateRetention() {
    ScreenshotExportCache cache(8);
    auto held = std::make_shared<int>(11);
    std::weak_ptr<int> oldest;
    std::weak_ptr<int> newest;
    for (int index = 0; index < 100; ++index) {
        auto value = std::make_shared<int>(index);
        oldest = newest;
        newest = value;
        cache.retain(held, 4);
        cache.retain(value, 4);
        cache.touch(held.get());
        require(oldest.expired() && cache.retainedBytes() == 8,
                "aggregate export cache evicts LRU entries across many owners");
    }
    cache.retain(held, 9);
    require(cache.retainedBytes() == 4 && *held == 11,
            "oversized output is not retained and consumers survive eviction");
    cache.clear();
    require(newest.expired() && cache.retainedBytes() == 0 && *held == 11,
            "pressure trimming releases optional storage without invalidating consumers");
    cache.retain(held, 4);
    cache.remove(held.get());
    require(cache.retainedBytes() == 0, "owner invalidation releases retained output immediately");
}

void workingBytesGateJobsWithoutOccupyingWorkers() {
    ScreenshotExportCoordinator coordinator(5000, qint64{8});
    QObject receiver;
    auto gate = std::make_shared<QSemaphore>();
    const auto release = qScopeGuard([&] { gate->release(4); });
    std::atomic_int firstStarted{0};
    std::atomic_int smallStarted{0};
    int completions = 0;
    const auto first = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [gate, &firstStarted](const auto&) {
            ++firstStarted;
            gate->acquire();
            return ScreenshotExportTaskResult{};
        },
        [&](auto) { ++completions; }, 6);
    require(first.isValid() && processUntil([&] { return firstStarted.load() == 1; }),
            "the first byte-accounted job starts");
    auto payload = std::make_shared<int>(42);
    const std::weak_ptr<int> waitingPayload = payload;
    const auto large = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [payload](const auto&) {
            require(false, "cancelled byte-waiting work must not execute");
            return ScreenshotExportTaskResult{};
        },
        [&](auto result) {
            require(result.failureStage == ScreenshotExportFailureStage::Cancelled,
                    "byte-waiting cancellation delivers the normal result");
            ++completions;
        },
        6);
    payload.reset();
    const auto small = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [gate, &smallStarted](const auto&) {
            ++smallStarted;
            gate->acquire();
            return ScreenshotExportTaskResult{};
        },
        [&](auto) { ++completions; }, 1);
    require(large.isValid() && small.isValid() && coordinator.activeWorkingBytes() == 6 &&
                coordinator.pendingJobCount() == 3,
            "a large head request waits fairly without admitting smaller followers");
    large.cancel();
    require(waitingPayload.expired(), "cancel releases byte-waiting captures immediately");
    if (QThread::idealThreadCount() > 1) {
        require(processUntil([&] { return smallStarted.load() == 1; }) &&
                    coordinator.activeWorkingBytes() == 7,
                "cancelling the blocked head allows compatible work to use the idle worker");
    }
    gate->release(2);
    require(processUntil([&] { return completions == 3; }) && coordinator.activeWorkingBytes() == 0,
            "completion releases every working-byte admission");

    const auto oversized = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Background,
        [gate](const auto&) {
            gate->acquire();
            return ScreenshotExportTaskResult{};
        },
        [&](auto) { ++completions; }, 9);
    const auto follower = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [](const auto&) { return ScreenshotExportTaskResult{}; }, [&](auto) { ++completions; });
    require(oversized.isValid() && follower.isValid() && coordinator.activeWorkingBytes() == 9,
            "oversized work runs alone without rejecting supported large exports");
    gate->release();
    require(processUntil([&] { return completions == 5; }) && coordinator.activeWorkingBytes() == 0,
            "an oversized job releases admission for its follower");
}

void completionRunsOnceOnGuiThread() {
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    QThread* const guiThread = QThread::currentThread();
    QThread* workThread = nullptr;
    int completionCount = 0;
    bool completionSucceeded = false;
    const ScreenshotExportJobHandle handle = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [&workThread](const ScreenshotExportCancellation&) {
            workThread = QThread::currentThread();
            return ScreenshotExportTaskResult{};
        },
        [&completionCount, &completionSucceeded, guiThread](ScreenshotExportTaskResult result) {
            ++completionCount;
            completionSucceeded = result.succeeded() && QThread::currentThread() == guiThread;
        });
    require(handle.isValid(), "coordinator success job was not admitted");
    require(processUntil([&completionCount]() { return completionCount == 1; }),
            "coordinator success job did not complete");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(completionCount == 1 && completionSucceeded && workThread != guiThread,
            "coordinator completion was not exactly once on the GUI thread");
}

void cancellationPropagates() {
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    std::atomic_bool entered{false};
    int completionCount = 0;
    ScreenshotExportFailureStage stage = ScreenshotExportFailureStage::None;
    const ScreenshotExportJobHandle handle = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [&entered](const ScreenshotExportCancellation& cancellation) {
            entered.store(true, std::memory_order_release);
            while (!cancellation.isCancellationRequested()) {
                QThread::msleep(1);
            }
            return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                                       QStringLiteral("cancelled"));
        },
        [&completionCount, &stage](ScreenshotExportTaskResult result) {
            ++completionCount;
            stage = result.failureStage;
        });
    require(handle.isValid() &&
                processUntil([&entered]() { return entered.load(std::memory_order_acquire); }),
            "coordinator cancellation job did not start");
    handle.cancel();
    require(processUntil([&completionCount]() { return completionCount == 1; }),
            "coordinator cancellation did not complete");
    require(stage == ScreenshotExportFailureStage::Cancelled,
            "coordinator cancellation stage was lost");
}

void destroyedReceiverSuppressesCompletion() {
    ScreenshotExportCoordinator coordinator;
    auto* receiver = new QObject();
    std::atomic_bool release{false};
    int completionCount = 0;
    require(coordinator
                .submit(
                    receiver, ScreenshotExportCoordinator::Priority::Background,
                    [&release](const ScreenshotExportCancellation&) {
                        while (!release.load(std::memory_order_acquire)) {
                            QThread::msleep(1);
                        }
                        return ScreenshotExportTaskResult{};
                    },
                    [&completionCount](ScreenshotExportTaskResult) { ++completionCount; })
                .isValid(),
            "receiver-lifetime job was not admitted");
    delete receiver;
    release.store(true, std::memory_order_release);
    require(processUntil([&coordinator]() { return coordinator.pendingJobCount() == 0; }),
            "receiver-lifetime job did not drain");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(completionCount == 0, "destroyed receiver received an export completion");
}

void queueAndWorkerBoundsAreEnforced() {
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    std::atomic_bool release{false};
    std::atomic_int running{0};
    std::atomic_int peak{0};
    int completionCount = 0;
    std::vector<ScreenshotExportJobHandle> handles;
    handles.reserve(16);
    for (int index = 0; index < 16; ++index) {
        handles.push_back(coordinator.submit(
            &receiver, ScreenshotExportCoordinator::Priority::Background,
            [&release, &running, &peak](const ScreenshotExportCancellation&) {
                const int active = running.fetch_add(1, std::memory_order_acq_rel) + 1;
                int observed = peak.load(std::memory_order_acquire);
                while (active > observed &&
                       !peak.compare_exchange_weak(observed, active, std::memory_order_acq_rel)) {
                }
                while (!release.load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
                running.fetch_sub(1, std::memory_order_acq_rel);
                return ScreenshotExportTaskResult{};
            },
            [&completionCount](ScreenshotExportTaskResult) { ++completionCount; }));
        require(handles.back().isValid(), "coordinator rejected a job below its queue bound");
    }
    const ScreenshotExportJobHandle overflow = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [](const ScreenshotExportCancellation&) { return ScreenshotExportTaskResult{}; },
        [](ScreenshotExportTaskResult) {});
    require(!overflow.isValid() && coordinator.pendingJobCount() == 16,
            "coordinator did not enforce its sixteen-job admission bound");
    require(processUntil([&peak]() { return peak.load(std::memory_order_acquire) > 0; }),
            "bounded coordinator jobs did not start");
    release.store(true, std::memory_order_release);
    require(processUntil([&completionCount]() { return completionCount == 16; }),
            "bounded coordinator jobs did not all complete");
    require(peak.load(std::memory_order_acquire) <= 2,
            "coordinator exceeded its two-worker concurrency bound");
}

void exportQueueDrainsLargeSelectionsAfterContention() {
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    auto externalGate = std::make_shared<QSemaphore>();
    auto decodeGate = std::make_shared<QSemaphore>();
    auto started = std::make_shared<std::atomic_int>(0);
    const auto release = qScopeGuard([&] {
        externalGate->release(16);
        decodeGate->release(50);
    });
    int externalCompletions = 0;
    for (int index = 0; index < 16; ++index) {
        require(coordinator
                    .submit(
                        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
                        [externalGate](const auto&) {
                            externalGate->acquire();
                            return ScreenshotExportTaskResult{};
                        },
                        [&](auto) { ++externalCompletions; })
                    .isValid(),
                "fill shared coordinator capacity before the batch");
    }
    ScreenshotExportQueue queue(coordinator);
    const int workerCount = std::clamp(QThread::idealThreadCount(), 1, 2);
    QSet<int> completed;
    int failures = 0;
    QThread* const guiThread = QThread::currentThread();
    for (int index = 0; index < 50; ++index) {
        require(queue.enqueue(
                    ScreenshotExportCoordinator::Priority::Foreground,
                    [decodeGate, started, index](const auto&) {
                        started->fetch_add(1);
                        decodeGate->acquire();
                        ScreenshotExportTaskResult result;
                        result.savedPath = QString::number(index);
                        if (index == 17)
                            result.failureStage = ScreenshotExportFailureStage::Source;
                        return result;
                    },
                    [&](ScreenshotExportTaskResult result) {
                        require(QThread::currentThread() == guiThread,
                                "queued pin completions stay on the GUI thread");
                        const int index = result.savedPath.toInt();
                        require(!completed.contains(index), "each queued pin completes once");
                        completed.insert(index);
                        if (!result.succeeded())
                            ++failures;
                    }),
                "a saturated coordinator must retain all fifty selected pin requests");
    }
    require(queue.active() && coordinator.pendingJobCount() == 16 && started->load() == 0,
            "waiting selections do not bypass shared admission or start decoding");
    externalGate->release(16);
    require(
        processUntil([&] { return externalCompletions == 16 && started->load() == workerCount; }),
        "freeing shared capacity wakes a queue with no admitted jobs");
    require(coordinator.pendingJobCount() == 2,
            "a large batch admits only two blocked decodes at a time");
    decodeGate->release(50);
    require(processUntil([&] { return completed.size() == 50 && !queue.active(); }),
            "the complete selection drains after contention and individual decode failures");
    require(failures == 1 && coordinator.pendingJobCount() == 0,
            "failed records report once without dropping subsequent selected pins");
}

void exportQueueCancellationAndLifetime() {
    ScreenshotExportCoordinator coordinator;
    const int workerCount = std::clamp(QThread::idealThreadCount(), 1, 2);
    auto entered = std::make_shared<std::atomic_int>(0);
    auto queue = std::make_unique<ScreenshotExportQueue>(coordinator);
    int completions = 0;
    const auto blocking = [entered](const ScreenshotExportCancellation& token) {
        entered->fetch_add(1);
        while (!token.isCancellationRequested())
            QThread::msleep(1);
        return ScreenshotExportTaskResult{};
    };
    for (int index = 0; index < 50; ++index)
        require(queue->enqueue(ScreenshotExportCoordinator::Priority::Foreground, blocking,
                               [&](auto) { ++completions; }),
                "admit cancellable batch");
    require(processUntil([&] { return entered->load() == workerCount; }), "start batch decodes");
    queue->cancel();
    require(!queue->active(), "cancel clears waiting and admitted requests immediately");
    require(queue->enqueue(
                ScreenshotExportCoordinator::Priority::Foreground,
                [](const auto&) { return ScreenshotExportTaskResult{}; },
                [&](auto) { ++completions; }),
            "a later selection may reuse a cancelled queue");
    require(processUntil([&] {
                return completions == 1 && !queue->active() && coordinator.pendingJobCount() == 0;
            }),
            "cancelled generations cannot deliver callbacks into the next selection");
    const int cancelledStarted = entered->load();
    require(cancelledStarted <= 2, "cancel never decodes the unsubmitted batch tail");
    require(queue->enqueue(ScreenshotExportCoordinator::Priority::Foreground, blocking,
                           [&](auto) { ++completions; }),
            "admit lifetime fixture");
    require(processUntil([&] { return entered->load() == cancelledStarted + 1; }),
            "start lifetime fixture");
    queue.reset();
    require(processUntil([&] { return coordinator.pendingJobCount() == 0; }),
            "destroying the queue cancels in-flight work");
    QCoreApplication::processEvents();
    require(completions == 1, "destroyed queues never deliver completions");

    ScreenshotExportQueue stopped(coordinator);
    for (int index = 0; index < 50; ++index)
        require(stopped.enqueue(ScreenshotExportCoordinator::Priority::Foreground, blocking,
                                [&](auto) { ++completions; }),
                "admit shutdown batch");
    coordinator.shutdown();
    require(!stopped.active() && coordinator.pendingJobCount() == 0 &&
                !stopped.enqueue(ScreenshotExportCoordinator::Priority::Foreground, blocking,
                                 [&](auto) { ++completions; }),
            "shutdown discards every waiting request and rejects future selections");
    QCoreApplication::processEvents();
    require(completions == 1, "shutdown suppresses queued pin completions");
}

void exportQueueCompletionMayReplaceOrDeleteTheQueue() {
    ScreenshotExportCoordinator coordinator;
    auto queue = std::make_unique<ScreenshotExportQueue>(coordinator);
    int completions = 0;
    require(queue->enqueue(
                ScreenshotExportCoordinator::Priority::Foreground,
                [](const auto&) { return ScreenshotExportTaskResult{}; },
                [&](auto) {
                    ++completions;
                    queue->cancel();
                    require(queue->enqueue(
                                ScreenshotExportCoordinator::Priority::Foreground,
                                [](const auto&) { return ScreenshotExportTaskResult{}; },
                                [&](auto) {
                                    ++completions;
                                    queue.reset();
                                }),
                            "completion may enqueue a replacement selection");
                }),
            "admit reentrant completion");
    require(processUntil([&] { return completions == 2 && !queue; }),
            "completion can replace and delete its queue safely");
}

void queuedPromotionPreservesOwnershipAndPriority() {
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    struct State {
        QSemaphore blockers;
        QSemaphore promoted;
        std::atomic_int entered{0};
        std::atomic_int backgroundRuns{0};
        std::atomic_int promotedRuns{0};
        std::atomic_int cancelledRuns{0};
    };
    auto state = std::make_shared<State>();
    const int workers = std::clamp(QThread::idealThreadCount(), 1, 2);
    const auto unblock = qScopeGuard([state, workers] {
        state->blockers.release(workers);
        state->promoted.release();
    });
    for (int i = 0; i < workers; ++i)
        require(coordinator
                    .submit(
                        &receiver, ScreenshotExportCoordinator::Priority::Background,
                        [state](const ScreenshotExportCancellation&) {
                            ++state->entered;
                            state->blockers.acquire();
                            return ScreenshotExportTaskResult{};
                        },
                        [](ScreenshotExportTaskResult) {})
                    .isValid(),
                "promotion fixture occupies all workers");
    require(processUntil([&] { return state->entered.load() == workers; }),
            "promotion workers have started");
    int completions = 0;
    require(coordinator
                .submit(
                    &receiver, ScreenshotExportCoordinator::Priority::Background,
                    [state](const ScreenshotExportCancellation&) {
                        ++state->backgroundRuns;
                        return ScreenshotExportTaskResult{};
                    },
                    [&](ScreenshotExportTaskResult result) {
                        require(result.succeeded(), "background work still succeeds");
                        ++completions;
                    })
                .isValid(),
            "older background work is queued");
    const auto promoted = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Background,
        [state](const ScreenshotExportCancellation&) {
            ++state->promotedRuns;
            state->promoted.acquire();
            return ScreenshotExportTaskResult{};
        },
        [&](ScreenshotExportTaskResult result) {
            require(result.succeeded(), "promoted work succeeds");
            ++completions;
        });
    require(promoted.isValid(), "promotion candidate is queued");
    const int pending = coordinator.pendingJobCount();
    promoted.promoteToForeground();
    promoted.promoteToForeground();
    require(coordinator.pendingJobCount() == pending, "promotion retains one admission");
    state->blockers.release();
    require(processUntil([&] { return state->promotedRuns.load() == 1; }) &&
                state->backgroundRuns.load() == 0,
            "a promoted job precedes older background jobs when a worker becomes free");
    promoted.promoteToForeground();
    bool cancelled = false;
    const auto cancelledJob = coordinator.submit(
        &receiver, ScreenshotExportCoordinator::Priority::Background,
        [state](const ScreenshotExportCancellation&) {
            ++state->cancelledRuns;
            return ScreenshotExportTaskResult{};
        },
        [&](ScreenshotExportTaskResult result) {
            require(!cancelled && result.failureStage == ScreenshotExportFailureStage::Cancelled,
                    "promoted cancellation completes exactly once");
            cancelled = true;
        });
    require(cancelledJob.isValid(), "queued cancellation candidate is admitted");
    cancelledJob.promoteToForeground();
    cancelledJob.cancel();
    cancelledJob.promoteToForeground();
    require(processUntil([&] { return cancelled; }), "promoted queued cancellation completes");
    state->blockers.release(workers);
    state->promoted.release();
    require(processUntil([&] { return completions == 2 && coordinator.pendingJobCount() == 0; }) &&
                state->backgroundRuns.load() == 1 && state->promotedRuns.load() == 1 &&
                state->cancelledRuns.load() == 0,
            "promotion neither duplicates jobs nor loses cancellation");
    coordinator.shutdown();
    promoted.promoteToForeground();
    ScreenshotExportJobHandle{}.promoteToForeground();
}

void queuedCancellationImmediatelyReleasesCapacity() {
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    const int workerCount = std::clamp(QThread::idealThreadCount(), 1, 2);
    auto released = std::make_shared<std::atomic_bool>(false);
    auto entered = std::make_shared<std::atomic_int>(0);
    const auto unblock = qScopeGuard([released] { released->store(true); });
    for (int index = 0; index < workerCount; ++index) {
        require(coordinator
                    .submit(
                        &receiver, ScreenshotExportCoordinator::Priority::Foreground,
                        [released, entered](const ScreenshotExportCancellation&) {
                            ++*entered;
                            while (!released->load())
                                QThread::msleep(1);
                            return ScreenshotExportTaskResult{};
                        },
                        [](ScreenshotExportTaskResult) {})
                    .isValid(),
                "queue cancellation workers must be admitted");
    }
    require(processUntil([&] { return entered->load() == workerCount; }),
            "queue cancellation workers must be occupied");
    int completions = 0;
    int executions = 0;
    QThread* const guiThread = QThread::currentThread();
    for (int index = 0; index < 40; ++index) {
        auto payload = std::make_shared<int>(index);
        std::weak_ptr<int> retainedPayload = payload;
        const auto job = coordinator.submit(
            &receiver, ScreenshotExportCoordinator::Priority::Foreground,
            [payload, &executions](const ScreenshotExportCancellation&) {
                ++executions;
                return ScreenshotExportTaskResult{};
            },
            [&](ScreenshotExportTaskResult result) {
                require(result.failureStage == ScreenshotExportFailureStage::Cancelled &&
                            QThread::currentThread() == guiThread,
                        "queued cancellation must deliver one GUI-thread cancellation");
                ++completions;
            });
        payload.reset();
        require(job.isValid(), "superseded jobs must not exhaust queue capacity");
        std::thread cancel([job] {
            job.cancel();
            job.cancel();
        });
        cancel.join();
        require(job.isCancellationRequested() && coordinator.pendingJobCount() == workerCount &&
                    retainedPayload.expired() && completions == index,
                "cancellation must free the queued slot and work payload before returning");
        require(processUntil([&] { return completions == index + 1; }),
                "queued cancellation completion must remain asynchronous");
    }
    require(executions == 0, "cancelled queued work must never execute");
    bool completed = false;
    require(
        coordinator
            .submit(
                &receiver, ScreenshotExportCoordinator::Priority::Foreground,
                [](const ScreenshotExportCancellation&) { return ScreenshotExportTaskResult{}; },
                [&](ScreenshotExportTaskResult result) { completed = result.succeeded(); })
            .isValid(),
        "the latest job must remain admissible after repeated cancellations");
    released->store(true);
    require(processUntil([&] { return completed && coordinator.pendingJobCount() == 0; }),
            "latest work must complete after the workers are released");
    QCoreApplication::processEvents();
    require(completions == 40, "cancelled jobs must not complete again when workers resume");
}

void shutdownCancelsAndDrains() {
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    std::atomic_bool entered{false};
    int completionCount = 0;
    ScreenshotExportFailureStage stage = ScreenshotExportFailureStage::None;
    require(coordinator
                .submit(
                    &receiver, ScreenshotExportCoordinator::Priority::Foreground,
                    [&entered](const ScreenshotExportCancellation& cancellation) {
                        entered.store(true, std::memory_order_release);
                        while (!cancellation.isCancellationRequested()) {
                            QThread::msleep(1);
                        }
                        return ScreenshotExportTaskResult::failure(
                            ScreenshotExportFailureStage::Cancelled, QStringLiteral("shutdown"));
                    },
                    [&completionCount, &stage](ScreenshotExportTaskResult result) {
                        ++completionCount;
                        stage = result.failureStage;
                    })
                .isValid(),
            "shutdown job was not admitted");
    require(processUntil([&entered]() { return entered.load(std::memory_order_acquire); }),
            "shutdown job did not start");
    coordinator.shutdown();
    require(coordinator.pendingJobCount() == 0,
            "coordinator shutdown returned before its jobs drained");
    require(processUntil([&completionCount]() { return completionCount == 1; }),
            "shutdown completion was not delivered");
    require(stage == ScreenshotExportFailureStage::Cancelled &&
                !coordinator
                     .submit(
                         &receiver, ScreenshotExportCoordinator::Priority::Foreground,
                         [](const ScreenshotExportCancellation&) {
                             return ScreenshotExportTaskResult{};
                         },
                         [](ScreenshotExportTaskResult) {})
                     .isValid(),
            "coordinator admitted work after shutdown");
}

void idleWorkersRetireAndLaterWorkRestarts() {
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    QThread* workerThread = nullptr;
    int completionCount = 0;
    const auto submit = [&]() {
        return coordinator.submit(
            &receiver, ScreenshotExportCoordinator::Priority::Foreground,
            [&workerThread](const ScreenshotExportCancellation&) {
                workerThread = QThread::currentThread();
                return ScreenshotExportTaskResult{};
            },
            [&completionCount](ScreenshotExportTaskResult result) {
                require(result.succeeded(), "restarted export work must succeed");
                ++completionCount;
            });
    };
    require(submit().isValid() && processUntil([&]() { return completionCount == 1; }),
            "initial export work must complete");
    require(workerThread != nullptr &&
                processUntil([&]() { return workerThread->isFinished(); }, 10000),
            "idle export workers must retire without shutting down the coordinator");
    require(coordinator.pendingJobCount() == 0,
            "retiring idle workers must leave no pending exports");
    require(submit().isValid() && processUntil([&]() { return completionCount == 2; }),
            "export work must restart after idle worker retirement");
}

void shutdownAbandonsAWorkerThatIgnoresCancellation() {
    const auto idleCount =
        snow_shot::runtime::RuntimeActivityTracker::shared().snapshot().activeCount;
    ScreenshotExportCoordinator coordinator(150);
    QObject receiver;
    std::atomic_bool entered{false};
    std::atomic_bool release{false};
    int completionCount = 0;
    QThread* workerThread = nullptr;
    require(coordinator
                .submit(
                    &receiver, ScreenshotExportCoordinator::Priority::Foreground,
                    [&entered, &release, &workerThread](const ScreenshotExportCancellation&) {
                        workerThread = QThread::currentThread();
                        entered.store(true, std::memory_order_release);
                        while (!release.load(std::memory_order_acquire)) {
                            QThread::msleep(2);
                        }
                        return ScreenshotExportTaskResult{};
                    },
                    [&completionCount](ScreenshotExportTaskResult) { ++completionCount; })
                .isValid(),
            "shutdown abandon job was not admitted");
    require(processUntil([&entered]() { return entered.load(std::memory_order_acquire); }),
            "shutdown abandon job did not start");
    coordinator.shutdown();
    require(!release.load(std::memory_order_acquire) && workerThread != nullptr &&
                !workerThread->isFinished(),
            "shutdown must return while the cancellation-ignoring worker remains gated");
    require(snow_shot::runtime::RuntimeActivityTracker::shared().snapshot().activeCount > idleCount,
            "an abandoned running export must keep memory trimming blocked");
    release.store(true, std::memory_order_release);
    require(processUntil([&coordinator]() { return coordinator.pendingJobCount() == 0; }),
            "the abandoned export worker did not finish after release");
    // The abandoned pool retires the worker instead of parking it forever on
    // its disabled expiry timeout.
    require(workerThread != nullptr &&
                processUntil([workerThread]() { return workerThread->isFinished(); }),
            "the abandoned export worker thread did not retire after finishing");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(completionCount == 1, "the abandoned worker's completion was not delivered");
    require(
        processUntil([idleCount] {
            return snow_shot::runtime::RuntimeActivityTracker::shared().snapshot().activeCount ==
                   idleCount;
        }),
        "settled abandoned work must release its memory activity reservation");
    coordinator.shutdown();
    QCoreApplication::processEvents();
    require(coordinator.pendingJobCount() == 0 && completionCount == 1 &&
                workerThread->isFinished(),
            "repeated shutdown must preserve drained work and its single completion");
}

void shutdownDropsQueuedWorkWhenAbandoned() {
    ScreenshotExportCoordinator coordinator(150);
    QObject receiver;
    const int workerCount = std::clamp(QThread::idealThreadCount(), 1, 2);
    std::atomic_int entered{0};
    std::atomic_bool release{false};
    int blockerCompletions = 0;
    int stragglerCompletions = 0;
    const auto blocker = [&entered, &release](const ScreenshotExportCancellation&) {
        entered.fetch_add(1, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) {
            QThread::msleep(2);
        }
        return ScreenshotExportTaskResult{};
    };
    const auto countBlockerCompletion = [&blockerCompletions](ScreenshotExportTaskResult) {
        ++blockerCompletions;
    };
    require(coordinator
                .submit(&receiver, ScreenshotExportCoordinator::Priority::Foreground, blocker,
                        countBlockerCompletion)
                .isValid(),
            "shutdown straggler blocker was not admitted");
    require(processUntil([&entered]() { return entered.load(std::memory_order_acquire) >= 1; }),
            "shutdown straggler first blocker did not start");
    // A second blocker keeps every pool worker wedged so the straggler below is
    // guaranteed to stay queued, never started.
    static_cast<void>(coordinator.submit(&receiver,
                                         ScreenshotExportCoordinator::Priority::Foreground, blocker,
                                         countBlockerCompletion));
    require(processUntil([workerCount, &entered]() {
                return entered.load(std::memory_order_acquire) == workerCount;
            }),
            "shutdown straggler blockers did not occupy the workers");
    require(
        coordinator
            .submit(
                &receiver, ScreenshotExportCoordinator::Priority::Background,
                [](const ScreenshotExportCancellation&) { return ScreenshotExportTaskResult{}; },
                [&stragglerCompletions](ScreenshotExportTaskResult) { ++stragglerCompletions; })
            .isValid(),
        "shutdown straggler job was not admitted");
    coordinator.shutdown();
    release.store(true, std::memory_order_release);
    require(processUntil(
                [workerCount, &blockerCompletions]() { return blockerCompletions == workerCount; }),
            "shutdown straggler blockers did not finish after release");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(stragglerCompletions == 0, "the abandoned pool executed queued work after shutdown");
}

void clipboardCommitCancellationIsAsynchronous() {
    QObject receiver;
    QImage image(QSize(2, 2), QImage::Format_RGBA8888);
    image.fill(Qt::red);
    int completionCount = 0;
    ScreenshotClipboardCommitResult result;
    const ScreenshotClipboardCommitHandle handle = ScreenshotClipboardService::commit(
        nullptr, &receiver, ScreenshotClipboardService::prepareImage(image),
        [&completionCount, &result](ScreenshotClipboardCommitResult completed) {
            ++completionCount;
            result = completed;
        });
    require(handle.isValid() && completionCount == 0,
            "clipboard commit did not start asynchronously");
    handle.cancel();
    require(processUntil([&completionCount]() { return completionCount == 1; }),
            "cancelled clipboard commit did not complete");
    require(result.failure == ScreenshotClipboardCommitFailure::Cancelled && result.attempts == 0,
            "clipboard cancellation did not precede its first publication attempt");
}

#if defined(Q_OS_WIN) || defined(_WIN32)
void clipboardCommitRetriesTransientContention() {
    std::atomic_bool locked{false};
    std::atomic_bool release{false};
    std::thread locker([&locked, &release]() {
        bool opened = false;
        for (int attempt = 0; attempt < 500 && !opened; ++attempt) {
            opened = OpenClipboard(nullptr) != FALSE;
            if (!opened) {
                QThread::msleep(1);
            }
        }
        locked.store(opened, std::memory_order_release);
        while (opened && !release.load(std::memory_order_acquire)) {
            QThread::msleep(1);
        }
        if (opened) {
            CloseClipboard();
        }
    });
    const bool clipboardLocked =
        processUntil([&locked]() { return locked.load(std::memory_order_acquire); });
    if (!clipboardLocked) {
        release.store(true, std::memory_order_release);
        locker.join();
        require(false, "clipboard contention fixture could not lock the clipboard");
    }

    QObject receiver;
    QImage image(QSize(3, 2), QImage::Format_RGBA8888);
    image.fill(Qt::blue);
    int completionCount = 0;
    ScreenshotClipboardCommitResult result;
    const ScreenshotClipboardCommitHandle handle = ScreenshotClipboardService::commit(
        nullptr, &receiver, ScreenshotClipboardService::prepareImage(image),
        [&completionCount, &result](ScreenshotClipboardCommitResult completed) {
            ++completionCount;
            result = completed;
        });
    QTimer::singleShot(50, &receiver,
                       [&release]() { release.store(true, std::memory_order_release); });
    const bool completed =
        handle.isValid() && processUntil([&completionCount]() { return completionCount == 1; });
    release.store(true, std::memory_order_release);
    locker.join();
    require(completed, "clipboard commit did not finish after transient contention");
    require(result.succeeded() && result.attempts > 1 && result.attempts <= 5,
            "clipboard commit did not retry bounded transient contention");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        if (application.arguments().contains(QStringLiteral("--idle-workers-only"))) {
            idleWorkersRetireAndLaterWorkRestarts();
            return EXIT_SUCCESS;
        }
        if (application.arguments().contains(QStringLiteral("--queue-only"))) {
            exportQueueDrainsLargeSelectionsAfterContention();
            exportQueueCancellationAndLifetime();
            exportQueueCompletionMayReplaceOrDeleteTheQueue();
            return EXIT_SUCCESS;
        }
        completionRunsOnceOnGuiThread();
        exportCacheBoundsAggregateRetention();
        workingBytesGateJobsWithoutOccupyingWorkers();
        cancellationPropagates();
        destroyedReceiverSuppressesCompletion();
        queueAndWorkerBoundsAreEnforced();
        exportQueueDrainsLargeSelectionsAfterContention();
        exportQueueCancellationAndLifetime();
        exportQueueCompletionMayReplaceOrDeleteTheQueue();
        queuedPromotionPreservesOwnershipAndPriority();
        queuedCancellationImmediatelyReleasesCapacity();
        shutdownCancelsAndDrains();
        idleWorkersRetireAndLaterWorkRestarts();
        shutdownAbandonsAWorkerThatIgnoresCancellation();
        shutdownDropsQueuedWorkWhenAbandoned();
        if (application.arguments().contains(QStringLiteral("--scheduling-only")))
            return EXIT_SUCCESS;
        clipboardCommitCancellationIsAsynchronous();
#if defined(Q_OS_WIN) || defined(_WIN32)
        clipboardCommitRetriesTransientContention();
#endif
    } catch (const std::exception& error) {
        qCritical("%s", error.what());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
