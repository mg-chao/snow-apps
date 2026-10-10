#include "platform/windows/windowcursorrefresh.h"
#include "snow_shot/platform/windows/windowchrome.h"

#include <QApplication>
#include <QEvent>

#include <cstdlib>
#include <iostream>

#include <qt_windows.h>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void cancelledCursorPreparationDoesNotCallItsOwner() {
    auto context = std::make_unique<QObject>();
    snow_shot::platform::windows::CursorRefresh refresh(context.get());
    context.reset();
    bool called = false;
    refresh.refresh([&](bool) { called = true; });
    QCoreApplication::processEvents();
    require(!called, "destroying the callback context must cancel cursor preparation");
}

void unavailableCursorPreparationReportsFailureOnce() {
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        return;
    }
    QObject context;
    snow_shot::platform::windows::CursorRefresh refresh(&context);
    int callbacks = 0;
    refresh.refresh([&](bool ready) {
        require(!ready, "an unavailable native cursor refresh must report failure");
        ++callbacks;
    });
    QCoreApplication::processEvents();
    require(callbacks == 1, "failed cursor preparation must complete exactly once");
}

void desktopCursorUpdatesDoNotRequireTheWindowOwnerThread() {
    using snow_shot::platform::windows::detail::isUnderlyingCursorUpdate;
    constexpr quint32 caller = 11;
    constexpr quint32 target = 22;
    constexpr quint32 compositor = 33;
    for (const auto event : {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE}) {
        require(isUnderlyingCursorUpdate(event, OBJID_CURSOR, compositor, caller, target),
                "DWM cursor updates must be accepted even though DWM does not own the target");
        require(isUnderlyingCursorUpdate(event, OBJID_CURSOR, target, caller, target),
                "direct application cursor updates must also be accepted");
        require(!isUnderlyingCursorUpdate(event, OBJID_CURSOR, caller, caller, target),
                "our outgoing cursor change must not complete another application's refresh");
        require(!isUnderlyingCursorUpdate(event, OBJID_CURSOR, caller, caller, caller) &&
                    !isUnderlyingCursorUpdate(event, OBJID_CURSOR, compositor, caller, caller),
                "local cursor notifications cannot acknowledge native mouse dispatch");
        require(!isUnderlyingCursorUpdate(event, OBJID_CLIENT, compositor, caller, target) &&
                    !isUnderlyingCursorUpdate(event, OBJID_CURSOR, compositor, caller, 0),
                "non-cursor events and missing targets must not complete cursor preparation");
    }
    require(!isUnderlyingCursorUpdate(EVENT_OBJECT_FOCUS, OBJID_CURSOR, target, caller, target),
            "unrelated cursor-object events must not complete preparation");
}

using snow_shot::platform::windows::detail::CursorRefreshOperation;
using snow_shot::platform::windows::detail::CursorRefreshTarget;

struct RefreshFixture {
    QObject context;
    CursorRefreshTarget target{42, 11, QPoint(100, 100)};
    int refreshes = 0;
    int flushes = 0;
    int disarms = 0;
    int completions = 0;
    bool localRefresh = false;
    bool refreshSucceeds = true;
    bool flushSucceeds = true;
    bool moveDuringFlush = false;
    bool ready = false;
    CursorRefreshOperation operation{&context,
                                     {11, [&] { return target; },
                                      [&](bool local) {
                                          ++refreshes;
                                          localRefresh = local;
                                          return refreshSucceeds;
                                      },
                                      [&] {
                                          ++flushes;
                                          if (std::exchange(moveDuringFlush, false))
                                              ++target.position.rx();
                                          return flushSucceeds;
                                      },
                                      [&] { ++disarms; }}};

    void start() {
        operation.start([&](bool succeeded) {
            ready = succeeded;
            ++completions;
        });
    }
    static void dispatchQueuedCompletion() {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }
};

void localMouseDispatchCompletesWithoutCursorChanges() {
    // A main/settings window can keep its cached cursor (including an intentionally
    // hidden cursor). No WinEvent is required to acknowledge its stationary input.
    for (int iteration = 0; iteration < 3; ++iteration) {
        RefreshFixture fixture;
        fixture.operation.mouseDispatched(fixture.target.window, fixture.target.position);
        fixture.start();
        require(fixture.localRefresh && fixture.refreshes == 1,
                "local refresh must preserve the existing cursor instead of forcing a change");
        for (const auto event : {EVENT_OBJECT_HIDE, EVENT_OBJECT_SHOW, EVENT_OBJECT_NAMECHANGE}) {
            fixture.operation.cursorChanged(event, OBJID_CURSOR, 11);
            fixture.operation.cursorChanged(event, OBJID_CURSOR, 33);
        }
        fixture.operation.mouseDispatched(99, fixture.target.position);
        fixture.operation.mouseDispatched(fixture.target.window,
                                          fixture.target.position - QPoint(1, 0));
        fixture.dispatchQueuedCompletion();
        require(fixture.completions == 0,
                "outgoing cursor changes, old input and other windows must not start capture");
        fixture.operation.mouseDispatched(fixture.target.window, fixture.target.position);
        require(fixture.completions == 0 && fixture.flushes == 0,
                "capture must wait until native dispatch and Qt child cursor selection finish");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        require(fixture.completions == 0,
                "the first posted batch can precede Qt's queued child-widget enter event");
        fixture.dispatchQueuedCompletion();
        require(fixture.ready && fixture.completions == 1 && fixture.flushes == 1 &&
                    fixture.disarms == 1,
                "unchanged local cursors must complete exactly once after dispatch");
        fixture.operation.mouseDispatched(fixture.target.window, fixture.target.position);
        fixture.dispatchQueuedCompletion();
        require(fixture.completions == 1, "later input must not recapture twice");
    }
}

void foreignCursorUpdatesStillWaitForTheDesktop() {
    RefreshFixture fixture;
    fixture.target.thread = 22;
    fixture.start();
    require(!fixture.localRefresh, "foreign targets must request desktop cursor handover");
    fixture.operation.mouseDispatched(fixture.target.window, fixture.target.position);
    fixture.operation.cursorChanged(EVENT_OBJECT_HIDE, OBJID_CURSOR, 11);
    fixture.dispatchQueuedCompletion();
    require(fixture.completions == 0,
            "our own outgoing cursor hide cannot acknowledge a foreign target");
    fixture.operation.cursorChanged(EVENT_OBJECT_NAMECHANGE, OBJID_CURSOR, 33);
    fixture.dispatchQueuedCompletion();
    require(fixture.ready && fixture.completions == 1,
            "a desktop cursor event from DWM must still acknowledge a foreign target");
}

void foreignCursorHandoverDuringWindowHidingIsRetained() {
    for (const bool moved : {false, true}) {
        RefreshFixture fixture;
        fixture.target.thread = 22;
        fixture.operation.cursorChanged(EVENT_OBJECT_NAMECHANGE, OBJID_CURSOR, 33);
        if (moved)
            ++fixture.target.position.rx();
        fixture.start();
        fixture.dispatchQueuedCompletion();
        require(fixture.ready && fixture.completions == 1,
                "handover during hiding must survive movement before capture preparation");
    }
}

void pointerMovementDoesNotDiscardCursorHandover() {
    for (const bool local : {false, true}) {
        for (const bool changeWindow : {false, true}) {
            RefreshFixture fixture;
            fixture.target.thread = local ? 11 : 22;
            fixture.start();
            if (local)
                fixture.operation.mouseDispatched(fixture.target.window, fixture.target.position);
            else
                fixture.operation.cursorChanged(EVENT_OBJECT_NAMECHANGE, OBJID_CURSOR, 33);
            // Moving after handover need not change the cursor shape. In particular,
            // another process's mouse dispatch cannot produce a local acknowledgement.
            ++fixture.target.position.rx();
            if (changeWindow) {
                ++fixture.target.window;
                ++fixture.target.thread;
            }
            fixture.dispatchQueuedCompletion();
            require(fixture.ready && fixture.completions == 1 && fixture.flushes == 1 &&
                        fixture.disarms == 1,
                    "capture must sample the current cursor without revalidating its old target");
            fixture.operation.cursorChanged(EVENT_OBJECT_NAMECHANGE, OBJID_CURSOR, 33);
            fixture.dispatchQueuedCompletion();
            require(fixture.completions == 1, "later cursor updates must not capture twice");
        }

        RefreshFixture fixture;
        fixture.target.thread = local ? 11 : 22;
        fixture.start();
        fixture.moveDuringFlush = true;
        if (local)
            fixture.operation.mouseDispatched(fixture.target.window, fixture.target.position);
        else
            fixture.operation.cursorChanged(EVENT_OBJECT_NAMECHANGE, OBJID_CURSOR, 33);
        fixture.dispatchQueuedCompletion();
        require(fixture.ready && fixture.completions == 1 && fixture.flushes == 1,
                "movement during composition must not discard completed cursor handover");
    }
}

void pendingPreparationCanBeCancelledAndReportsNativeFailures() {
    for (const bool failRefresh : {false, true}) {
        RefreshFixture fixture;
        fixture.refreshSucceeds = !failRefresh;
        fixture.flushSucceeds = false;
        fixture.start();
        fixture.operation.mouseDispatched(fixture.target.window, fixture.target.position);
        fixture.dispatchQueuedCompletion();
        require(!fixture.ready && fixture.completions == 1 && fixture.disarms == 1,
                "native request and composition failures must be reported once");
    }
    auto context = std::make_unique<QObject>();
    int completions = 0;
    int disarms = 0;
    auto operation = std::make_unique<CursorRefreshOperation>(
        context.get(), CursorRefreshOperation::Backend{
                           11, [] { return CursorRefreshTarget{42, 11, QPoint()}; },
                           [](bool) { return true; }, [] { return true; }, [&] { ++disarms; }});
    operation->start([&](bool) { ++completions; });
    operation->mouseDispatched(42, QPoint());
    context.reset();
    RefreshFixture::dispatchQueuedCompletion();
    require(completions == 0 && disarms == 1,
            "destroying the owner must disarm pending input and cancel queued completion");

    QObject liveContext;
    operation = std::make_unique<CursorRefreshOperation>(
        &liveContext,
        CursorRefreshOperation::Backend{11, [] { return CursorRefreshTarget{42, 11, QPoint()}; },
                                        [](bool) { return true; }, [] { return true; }, [] {}});
    operation->start([&](bool ready) {
        require(ready, "the live callback must receive successful preparation");
        ++completions;
        operation.reset();
    });
    operation->mouseDispatched(42, QPoint());
    RefreshFixture::dispatchQueuedCompletion();
    require(completions == 1 && !operation,
            "a capture callback may safely destroy its cursor preparation operation");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    cancelledCursorPreparationDoesNotCallItsOwner();
    unavailableCursorPreparationReportsFailureOnce();
    desktopCursorUpdatesDoNotRequireTheWindowOwnerThread();
    localMouseDispatchCompletesWithoutCursorChanges();
    foreignCursorUpdatesStillWaitForTheDesktop();
    foreignCursorHandoverDuringWindowHidingIsRetained();
    pointerMovementDoesNotDiscardCursorHandover();
    pendingPreparationCanBeCancelledAndReportsNativeFailures();
    return 0;
}
