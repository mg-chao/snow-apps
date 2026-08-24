#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/presentation/screenshotasyncactivitytracker.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QObject>
#include <QThread>
#include <QTimer>

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

bool waitWithoutProcessingEvents(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QThread::msleep(1);
    }
    return predicate();
}

void completionRunsOnceOnGuiThread() {
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 0,
            "the coordinator success test started with retained async activity");
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
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 1,
            "an admitted coordinator job did not acquire an activity lease");
    require(processUntil([&completionCount]() { return completionCount == 1; }),
            "coordinator success job did not complete");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(completionCount == 1 && completionSucceeded && workThread != guiThread,
            "coordinator completion was not exactly once on the GUI thread");
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 0,
            "the coordinator activity lease outlived terminal result disposal");
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
    require(waitWithoutProcessingEvents(
                [&coordinator]() { return coordinator.pendingJobCount() == 0; }),
            "receiver-lifetime job did not drain");
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 1,
            "worker completion released activity before GUI-terminal result disposal");
    require(processUntil([]() {
                return ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 0;
            }),
            "destroyed-receiver activity did not release on the GUI terminal turn");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(completionCount == 0, "destroyed receiver received an export completion");
}

void overlappingJobsPublishOneIdleTransition() {
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 0,
            "the overlapping-job test started with retained async activity");
    ScreenshotExportCoordinator coordinator;
    QObject receiver;
    QObject idleObserver;
    std::atomic_bool release{false};
    int completionCount = 0;
    int idleCount = 0;
    ScreenshotAsyncActivityTracker::shared().observeIdle(&idleObserver,
                                                         [&idleCount]() { ++idleCount; });
    const auto submit = [&]() {
        return coordinator.submit(
            &receiver, ScreenshotExportCoordinator::Priority::Foreground,
            [&release](const ScreenshotExportCancellation&) {
                while (!release.load(std::memory_order_acquire)) {
                    QThread::msleep(1);
                }
                return ScreenshotExportTaskResult{};
            },
            [&completionCount](ScreenshotExportTaskResult) { ++completionCount; });
    };
    const ScreenshotExportJobHandle first = submit();
    const ScreenshotExportJobHandle second = submit();
    require(first.isValid() && second.isValid() &&
                ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 2,
            "overlapping coordinator jobs did not hold independent activity leases");

    release.store(true, std::memory_order_release);
    require(processUntil([&]() {
                return completionCount == 2 && idleCount == 1 &&
                       ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 0;
            }),
            "overlapping coordinator jobs did not publish one terminal idle transition");
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    require(idleCount == 1, "the async activity tracker published duplicate idle transitions");
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
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 1,
            "the clipboard retry did not acquire an async activity lease");
    handle.cancel();
    require(processUntil([&completionCount]() { return completionCount == 1; }),
            "cancelled clipboard commit did not complete");
    require(result.failure == ScreenshotClipboardCommitFailure::Cancelled && result.attempts == 0,
            "clipboard cancellation did not precede its first publication attempt");
    require(ScreenshotAsyncActivityTracker::shared().activeActivityCount() == 0,
            "the cancelled clipboard retry retained its activity lease");
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

    const bool opened = OpenClipboard(nullptr) != FALSE;
    const HANDLE clipboardImage = opened ? GetClipboardData(CF_DIBV5) : nullptr;
    const auto* header = clipboardImage != nullptr
                             ? static_cast<const BITMAPV5HEADER*>(GlobalLock(clipboardImage))
                             : nullptr;
    const bool validHeader = header != nullptr &&
                             header->bV5Size == sizeof(BITMAPV5HEADER) &&
                             header->bV5Width == image.width() &&
                             header->bV5Height == -image.height() &&
                             header->bV5BitCount == 32 &&
                             header->bV5Compression == BI_BITFIELDS &&
                             header->bV5AlphaMask == 0xff000000;
    if (header != nullptr) {
        GlobalUnlock(clipboardImage);
    }
    if (opened) {
        CloseClipboard();
    }
    require(opened, "clipboard could not be reopened after the publisher exited");
    require(clipboardImage != nullptr,
            "clipboard image was unavailable after the publisher exited");
    require(validHeader,
            "clipboard image metadata changed after the publisher exited");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        completionRunsOnceOnGuiThread();
        cancellationPropagates();
        destroyedReceiverSuppressesCompletion();
        overlappingJobsPublishOneIdleTransition();
        queueAndWorkerBoundsAreEnforced();
        shutdownCancelsAndDrains();
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
