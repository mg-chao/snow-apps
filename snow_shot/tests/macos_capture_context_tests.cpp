#include "snow_shot/platform/macos/focusedfullscreenwindow.h"
#include "snow_shot/platform/macos/selectedtextcapturebackend.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QSemaphore>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>

namespace {
using namespace snow_shot::platform::macos;
using namespace snow_shot::presentation;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void fullscreenTracksTheFrontWindowOnEachDisplay() {
    const QList<QRectF> displays{{0, 0, 1512, 982}, {-1920, -120, 1920, 1080}};
    const VisibleWindow main{20, 0, displays[0]};
    const VisibleWindow external{20, 0, displays[1]};
    const VisibleWindow ordinary{20, 0, {100, 100, 800, 600}};
    require(focusedWindowFillsDisplay(20, 10, {main}, displays),
            "a front window covering the main display must suppress global shortcuts");
    require(focusedWindowFillsDisplay(20, 10, {external}, displays),
            "a fullscreen window on a display with negative coordinates must be recognized");
    require(!focusedWindowFillsDisplay(20, 10, {ordinary, main}, displays),
            "a covered fullscreen window from the same application must not suppress keys");
    require(!focusedWindowFillsDisplay(30, 10, {main}, displays) &&
                !focusedWindowFillsDisplay(10, 10, {{10, 0, displays[0]}}, displays) &&
                !focusedWindowFillsDisplay(0, 10, {main}, displays),
            "background applications, our capture overlays, and absent focus must be ignored");
    require(focusedWindowFillsDisplay(20, 10, {{20, 25, {0, 0, 100, 24}}, main}, displays),
            "menu and overlay layers must not replace the normal front window");
    require(!focusedWindowFillsDisplay(20, 10, {{20, 0, {0, 24, 1512, 958}}}, displays) &&
                !focusedWindowFillsDisplay(20, 10, {{20, 0, {0, 0, 1512, 900}}}, displays),
            "maximized windows leaving the menu bar or Dock visible must not count as fullscreen");
    require(focusedWindowFillsDisplay(20, 10, {{20, 0, {0.5, 0.5, 1511, 981}}}, displays) &&
                !focusedWindowFillsDisplay(20, 10, {{20, 0, {2, 0, 1510, 982}}}, displays),
            "only one Quartz coordinate of frame rounding is tolerated");
    require(!focusedWindowFillsDisplay(20, 10, {}, displays) &&
                !focusedWindowFillsDisplay(20, 10, {main}, {}) &&
                !focusedWindowFillsDisplay(20, 10, {{20, 0, {}}}, displays),
            "missing window or display information must leave shortcuts enabled");
}

SelectedTextCaptureResult completedResult(SelectedTextCaptureBackend& backend) {
    SelectedTextCaptureResult result;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(1);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        result = backend.poll();
        if (result.status != SelectedTextStatus::Pending) {
            loop.quit();
        }
    });
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    require(result.status != SelectedTextStatus::Pending,
            "a worker result must become available without blocking the event loop");
    return result;
}

void permissionAndMissingSelectionCanBeRetried() {
    bool trusted = false;
    qint64 process = 22;
    int foregroundReads = 0;
    std::atomic_int textReads = 0;
    auto backend = createSelectedTextCaptureBackend(
        {[&] { return trusted; },
         [&] {
             ++foregroundReads;
             return process;
         },
         [&](qint64) {
             ++textReads;
             return SelectedTextCaptureResult{SelectedTextStatus::Selected, QStringLiteral("ok")};
         }});
    require(backend->start().status == SelectedTextStatus::PermissionDenied &&
                foregroundReads == 0 && textReads == 0,
            "missing Accessibility permission must report its reason without reading another app");
    trusted = true;
    process = 0;
    require(backend->start().status == SelectedTextStatus::NoSelection && textReads == 0,
            "an absent foreground process must not start an AX read");
    process = 22;
    require(backend->start().status == SelectedTextStatus::Pending &&
                completedResult(*backend).text == QStringLiteral("ok"),
            "explicit retry after granting permission must work without restarting the app");
}

struct ReadGate {
    QSemaphore started;
    QSemaphore release;
    QSemaphore finished;
};

void captureSnapshotsFocusAndNeverWaitsForTheWorker() {
    auto gate = std::make_shared<ReadGate>();
    const auto mainThread = QThread::currentThreadId();
    qint64 foreground = 25;
    std::atomic<qint64> observed = 0;
    std::atomic_bool workerThread = false;
    const QString text = QString::fromUtf8("  selected \xe4\xb8\xad\xe6\x96\x87\ntext  ");
    auto backend = createSelectedTextCaptureBackend(
        {[] { return true; }, [&] { return foreground; },
         [&, gate](qint64 process) {
             observed = process;
             workerThread = QThread::currentThreadId() != mainThread;
             gate->started.release();
             gate->release.acquire();
             gate->finished.release();
             return SelectedTextCaptureResult{SelectedTextStatus::Selected, text};
         }});
    require(backend->start().status == SelectedTextStatus::Pending,
            "capture submission must return before the AX read finishes");
    foreground = 99;
    require(gate->started.tryAcquire(1, 3000), "the worker must start");
    require(observed == 25 && workerThread && backend->start().status == SelectedTextStatus::Busy &&
                backend->poll().status == SelectedTextStatus::Pending,
            "read the original application in a worker and reject duplicate submissions");
    gate->release.release();
    const auto result = completedResult(*backend);
    require(result.status == SelectedTextStatus::Selected && result.text == text,
            "selected Unicode text, whitespace and line breaks must remain unchanged");
}

void cancellationAndDestructionDiscardLateResults() {
    for (const bool destroy : {false, true}) {
        auto gate = std::make_shared<ReadGate>();
        std::atomic_int reads = 0;
        auto backend = createSelectedTextCaptureBackend(
            {[] { return true; }, [] { return 25; },
             [&, gate](qint64) {
                 if (++reads == 1) {
                     gate->started.release();
                     gate->release.acquire();
                     gate->finished.release();
                     return SelectedTextCaptureResult{SelectedTextStatus::Selected,
                                                      QStringLiteral("cancelled")};
                 }
                 return SelectedTextCaptureResult{SelectedTextStatus::Selected,
                                                  QStringLiteral("new request")};
             }});
        require(backend->start().status == SelectedTextStatus::Pending &&
                    gate->started.tryAcquire(1, 3000),
                "the request must enter a blocked worker before cancellation");
        if (destroy) {
            backend.reset();
        } else {
            backend->cancel();
            backend->cancel();
        }
        gate->release.release();
        require(gate->finished.tryAcquire(1, 3000),
                "shutdown must not wait for a pending AX read or access the deleted backend");
        if (!destroy) {
            require(backend->poll().status == SelectedTextStatus::NoSelection &&
                        backend->start().status == SelectedTextStatus::Pending &&
                        completedResult(*backend).text == QStringLiteral("new request"),
                    "a late cancelled completion must never replace a subsequent request");
        }
    }
}

void workerFailuresKeepTheirReason() {
    for (const auto status : {SelectedTextStatus::NoSelection, SelectedTextStatus::Unsupported,
                              SelectedTextStatus::PermissionDenied, SelectedTextStatus::TimedOut,
                              SelectedTextStatus::Failed}) {
        auto backend = createSelectedTextCaptureBackend(
            {[] { return true; }, [] { return 25; },
             [=](qint64) { return SelectedTextCaptureResult{status, {}}; }});
        require(backend->start().status == SelectedTextStatus::Pending &&
                    completedResult(*backend).status == status,
                "AX permission, unsupported app, timeout and empty selection must stay distinct");
    }
    auto backend =
        createSelectedTextCaptureBackend({[] { return true; }, [] { return 25; },
                                          [](qint64) -> SelectedTextCaptureResult { throw 1; }});
    require(backend->start().status == SelectedTextStatus::Pending &&
                completedResult(*backend).status == SelectedTextStatus::Failed,
            "an exceptional worker failure must complete instead of leaving capture pending");

    backend =
        createSelectedTextCaptureBackend({[] { return true; }, [] { return 25; },
                                          [](qint64) { return SelectedTextCaptureResult{}; }});
    require(backend->start().status == SelectedTextStatus::Pending &&
                completedResult(*backend).status == SelectedTextStatus::TimedOut,
            "a read that never reports completion must reach the bounded capture deadline");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    fullscreenTracksTheFrontWindowOnEachDisplay();
    permissionAndMissingSelectionCanBeRetried();
    captureSnapshotsFocusAndNeverWaitsForTheWorker();
    cancellationAndDestructionDiscardLateResults();
    workerFailuresKeepTheirReason();
    return 0;
}
