// SPDX-License-Identifier: GPL-3.0-or-later
#include "presentation/services/screenshotdeferredpresentation_p.h"

#include <QCoreApplication>
#include <QEventLoop>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
void dispatch() {
    for (int i = 0; i != 12; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents);
}

void queuedWorkWaitsForResume() {
    QObject context;
    std::vector<int> calls;
    ScreenshotDeferredPresentation work(
        context,
        {[&] { calls.push_back(1); }, [&] { calls.push_back(2); }, [&] { calls.push_back(3); }});
    work.requestPrewarm();
    work.requestRestore();
    require(calls.empty(), "presentation work is asynchronous");
    work.setSuspended(true);
    dispatch();
    require(calls.empty(), "suspension acquired after enqueue closes delayed presentation");
    work.requestRestore();
    work.requestPrewarm();
    dispatch();
    require(calls.empty(), "new requests while paused retain intent without executing");
    work.setSuspended(false);
    work.setSuspended(false);
    dispatch();
    require(calls == std::vector<int>({1, 2, 3}),
            "resume coalesces equivalent pending work and preserves prewarm-before-restore");
    work.requestRestore();
    dispatch();
    require(calls == std::vector<int>({1, 2, 3, 3}),
            "a later group restore is a new request after the prior request completed");
}

void reentrantPauseRetainsOnlyUnconsumedWork() {
    QObject context;
    std::vector<int> calls;
    std::unique_ptr<ScreenshotDeferredPresentation> work;
    work = std::make_unique<ScreenshotDeferredPresentation>(
        context, ScreenshotDeferredPresentation::Actions{
                     [&] {
                         calls.push_back(1);
                         work->setSuspended(true);
                         work->requestRestore();
                         work->setSuspended(false);
                         dispatch();
                         require(calls == std::vector<int>{1},
                                 "nested events cannot overlap a presentation step");
                         work->setSuspended(true);
                     },
                     [&] { calls.push_back(2); }, [&] { calls.push_back(3); }});
    work->requestPrewarm();
    dispatch();
    require(calls == std::vector<int>{1},
            "pause inside capture prewarm prevents pin prewarm and restoration");
    work->setSuspended(false);
    dispatch();
    require(calls == std::vector<int>({1, 2, 3}),
            "resume does not repeat an already completed native prewarm");
}

void terminalAndOwnerLossCancelWork() {
    QObject context;
    int calls = 0;
    ScreenshotDeferredPresentation stopped(context, {{}, {}, [&] { ++calls; }});
    stopped.requestRestore();
    stopped.stop();
    stopped.setSuspended(false);
    stopped.requestRestore();
    dispatch();
    require(calls == 0, "terminal cancellation cannot be reopened by resume or new requests");

    std::unique_ptr<ScreenshotDeferredPresentation> work;
    work = std::make_unique<ScreenshotDeferredPresentation>(
        context, ScreenshotDeferredPresentation::Actions{[&] { work.reset(); }, [&] { ++calls; },
                                                         [&] { ++calls; }});
    work->requestPrewarm();
    work->requestRestore();
    dispatch();
    require(!work && calls == 0, "callback owner loss cancels all remaining steps");

    auto transient = std::make_unique<QObject>();
    ScreenshotDeferredPresentation orphaned(*transient, {{}, {}, [&] { ++calls; }});
    orphaned.requestRestore();
    transient.reset();
    dispatch();
    orphaned.setSuspended(false);
    require(calls == 0, "Qt context destruction prevents delivery to expired presentation state");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    queuedWorkWaitsForResume();
    reentrantPauseRetainsOnlyUnconsumedWork();
    terminalAndOwnerLossCancelWork();
    return EXIT_SUCCESS;
}
