#include "platform/windows/windowcursorrefresh.h"
#include "snow_shot/platform/windows/windowchrome.h"

#include <QApplication>

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
        require(isUnderlyingCursorUpdate(event, OBJID_CURSOR, caller, caller, caller),
                "a same-thread underlying window can legitimately choose its own cursor");
        require(!isUnderlyingCursorUpdate(event, OBJID_CLIENT, compositor, caller, target) &&
                    !isUnderlyingCursorUpdate(event, OBJID_CURSOR, compositor, caller, 0),
                "non-cursor events and missing targets must not complete cursor preparation");
    }
    require(!isUnderlyingCursorUpdate(EVENT_OBJECT_FOCUS, OBJID_CURSOR, target, caller, target),
            "unrelated cursor-object events must not complete preparation");
}

} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    cancelledCursorPreparationDoesNotCallItsOwner();
    unavailableCursorPreparationReportsFailureOnce();
    desktopCursorUpdatesDoNotRequireTheWindowOwnerThread();
    return 0;
}
