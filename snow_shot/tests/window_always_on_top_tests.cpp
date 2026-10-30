#include "snow_shot/platform/windows/windowalwaysontop.h"

#include <QApplication>
#include <QWidget>
#include <qt_windows.h>

#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>

namespace {
using namespace snow_shot::platform::windows;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Fixture {
    std::optional<QPoint> cursor = QPoint(-1200, 250);
    WId hit = 11;
    WId root = 22;
    bool eligible = true;
    std::optional<bool> topmost = false;
    bool writeSucceeds = true;
    int reads = 0;
    int writes = 0;
    WindowAlwaysOnTopAccess access{
        [this] { return cursor; },
        [this](const QPoint& point) {
            require(point == QPoint(-1200, 250), "hit testing uses the native cursor coordinates");
            return hit;
        },
        [this](WId id) {
            require(id == hit, "the hit child is resolved to its top-level ancestor");
            return root;
        },
        [this](WId id) {
            require(id == root, "eligibility is checked on the top-level window");
            return eligible;
        },
        [this](WId id) {
            require(id == root, "read the topmost state of the top-level window");
            ++reads;
            return topmost;
        },
        [this](WId id, bool enabled, quint32 flags) {
            require(id == root && topmost && enabled != *topmost,
                    "toggle the actual top-level native state");
            require((flags & SWP_ASYNCWINDOWPOS) != 0,
                    "toggling must not wait for another window's thread");
            require((flags & ~SWP_ASYNCWINDOWPOS) == (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
                    "toggling preserves geometry and focus");
            ++writes;
            if (writeSucceeds)
                topmost = enabled;
            return writeSucceeds;
        }};
};

void togglesBothDirections() {
    Fixture fixture;
    require(toggleWindowUnderCursorAlwaysOnTop(fixture.access) ==
                    WindowAlwaysOnTopResult::ChangeRequested &&
                fixture.topmost == true,
            "a normal window becomes topmost");
    require(toggleWindowUnderCursorAlwaysOnTop(fixture.access) ==
                    WindowAlwaysOnTopResult::ChangeRequested &&
                fixture.topmost == false && fixture.writes == 2,
            "a second invocation removes topmost");
}

void invalidTargetsAreIgnored() {
    for (int scenario = 0; scenario != 3; ++scenario) {
        Fixture fixture;
        if (scenario == 0)
            fixture.hit = 0;
        else if (scenario == 1)
            fixture.root = 0;
        else
            fixture.eligible = false;
        int localCalls = 0;
        require(toggleWindowUnderCursorAlwaysOnTop(fixture.access,
                                                   [&](WId) -> std::optional<bool> {
                                                       ++localCalls;
                                                       return true;
                                                   }) == WindowAlwaysOnTopResult::Ignored &&
                    fixture.reads == 0 && fixture.writes == 0 && localCalls == 0,
                "missing windows and excluded shell/transient targets are ignored");
    }
}

void shellAndTransientSurfacesAreExcluded() {
    for (const auto& className :
         {QStringLiteral("Progman"), QStringLiteral("WorkerW"), QStringLiteral("Shell_TrayWnd"),
          QStringLiteral("Shell_SecondaryTrayWnd"), QStringLiteral("#32768"),
          QStringLiteral("tooltips_class32"), QString()}) {
        require(!detail::isWindowAlwaysOnTopTarget(className, Qt::Window),
                "desktop, taskbar, menu, tooltip, and unknown native classes are excluded");
    }
    require(
        detail::isWindowAlwaysOnTopTarget(QStringLiteral("ApplicationWindow"), Qt::Window) &&
            detail::isWindowAlwaysOnTopTarget(QStringLiteral("ApplicationWindow"), Qt::Dialog) &&
            !detail::isWindowAlwaysOnTopTarget(QStringLiteral("ApplicationWindow"), Qt::Popup) &&
            !detail::isWindowAlwaysOnTopTarget(QStringLiteral("ApplicationWindow"), Qt::ToolTip),
        "normal application windows and dialogs are eligible, Qt popups and tooltips are not");
}

void nativeFailuresDoNotReportSuccess() {
    for (int scenario = 0; scenario != 6; ++scenario) {
        Fixture fixture;
        if (scenario == 0)
            fixture.cursor.reset();
        else if (scenario == 1)
            fixture.topmost.reset();
        else if (scenario == 2)
            fixture.writeSucceeds = false;
        else if (scenario == 3)
            fixture.access.rootWindow = {};
        else if (scenario == 4)
            fixture.access.alwaysOnTop = {};
        else
            fixture.access.setAlwaysOnTop = {};
        require(toggleWindowUnderCursorAlwaysOnTop(fixture.access) ==
                    WindowAlwaysOnTopResult::Failed,
                "cursor, style, write, and unavailable backend failures are reported");
        require(fixture.writes == (scenario == 2 ? 1 : 0), "failure never triggers another write");
    }
}

void localWindowsUseTheirStatefulCommand() {
    for (bool succeeds : {false, true}) {
        Fixture fixture;
        fixture.access.alwaysOnTop = {};
        fixture.access.setAlwaysOnTop = {};
        int calls = 0;
        const auto result =
            toggleWindowUnderCursorAlwaysOnTop(fixture.access, [&](WId id) -> std::optional<bool> {
                require(id == fixture.root, "the local command receives the resolved surface");
                ++calls;
                return succeeds;
            });
        require(
            calls == 1 && fixture.reads == 0 && fixture.writes == 0 &&
                result == (succeeds ? WindowAlwaysOnTopResult::ChangeRequested
                                    : WindowAlwaysOnTopResult::Failed),
            "local stateful commands use their own state without consulting the native backend");
    }
    Fixture fixture;
    require(toggleWindowUnderCursorAlwaysOnTop(
                fixture.access, [](WId) -> std::optional<bool> { return std::nullopt; }) ==
                    WindowAlwaysOnTopResult::ChangeRequested &&
                fixture.reads == 1 && fixture.writes == 1,
            "other windows use the native setter");
}

void localQtFlagsFollowSuccessfulNativeChanges() {
    QWidget window;
    window.setGeometry(50, 60, 200, 100);
    window.show();
    QApplication::processEvents();
    const auto geometry = window.geometry();
    const auto id = window.winId();
    Fixture fixture;
    fixture.root = id;
    for (bool enabled : {true, false}) {
        require(toggleWindowUnderCursorAlwaysOnTop(fixture.access) ==
                        WindowAlwaysOnTopResult::ChangeRequested &&
                    window.windowFlags().testFlag(Qt::WindowStaysOnTopHint) == enabled &&
                    window.winId() == id && window.isVisible() && window.geometry() == geometry,
                "local Qt flags track native changes without recreating or hiding the window");
    }
    fixture.writeSucceeds = false;
    require(toggleWindowUnderCursorAlwaysOnTop(fixture.access) == WindowAlwaysOnTopResult::Failed &&
                !window.windowFlags().testFlag(Qt::WindowStaysOnTopHint),
            "failed native changes leave the local Qt flags unchanged");
}

void nativeChangesDoNotWaitForTheWindowThread() {
    // The hidden fixture's owner cannot pump messages until the caller returns.
    // No desktop hit testing, cursor movement, activation, or timing threshold is needed.
    std::promise<HWND> created;
    std::promise<void> resume;
    std::promise<bool> applied;
    auto resumeRequested = resume.get_future();
    std::thread owner([&] {
        WNDCLASSW windowClass{};
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"SnowShotAlwaysOnTopTest";
        windowClass.lpfnWndProc = [](HWND window, UINT message, WPARAM wParam,
                                     LPARAM lParam) -> LRESULT {
            if (message == WM_WINDOWPOSCHANGED &&
                (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
                PostQuitMessage(0);
            return DefWindowProcW(window, message, wParam, lParam);
        };
        require(RegisterClassW(&windowClass) != 0, "register the native fixture's window class");
        const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                            windowClass.lpszClassName, L"", WS_POPUP, 0, 0, 100,
                                            100, nullptr, nullptr, windowClass.hInstance, nullptr);
        require(window != nullptr, "create a hidden native window on a separate thread");
        created.set_value(window);
        resumeRequested.wait();
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
            DispatchMessageW(&message);
        applied.set_value((GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
        require(DestroyWindow(window) != FALSE, "destroy the native fixture on its owning thread");
        require(UnregisterClassW(windowClass.lpszClassName, windowClass.hInstance) != FALSE,
                "unregister the native fixture's window class");
    });

    Fixture fixture;
    fixture.root = reinterpret_cast<WId>(created.get_future().get());
    fixture.access.setAlwaysOnTop = [](WId id, bool enabled, quint32 flags) {
        // Fail before entering a synchronous OS call if the nonblocking contract regresses.
        require((flags & SWP_ASYNCWINDOWPOS) != 0, "foreign updates must be asynchronous");
        return SetWindowPos(reinterpret_cast<HWND>(id), enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0,
                            0, 0, 0, flags) != FALSE;
    };
    const auto result = toggleWindowUnderCursorAlwaysOnTop(fixture.access);
    require(result == WindowAlwaysOnTopResult::ChangeRequested,
            "the paused window's native update is accepted");
    resume.set_value();
    const bool topmost = applied.get_future().get();
    owner.join();
    require(result == WindowAlwaysOnTopResult::ChangeRequested && topmost,
            "a foreign update returns before its owner resumes and is applied when it pumps");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    togglesBothDirections();
    invalidTargetsAreIgnored();
    shellAndTransientSurfacesAreExcluded();
    nativeFailuresDoNotReportSuccess();
    localWindowsUseTheirStatefulCommand();
    localQtFlagsFollowSuccessfulNativeChanges();
    nativeChangesDoNotWaitForTheWindowThread();
    require(toggleWindowUnderCursorAlwaysOnTop() == WindowAlwaysOnTopResult::Failed,
            "the native helper cannot manipulate the real desktop from an offscreen session");
    return 0;
}
