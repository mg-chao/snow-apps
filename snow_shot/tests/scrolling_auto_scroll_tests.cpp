#include "presentation/capture/screenshotscrollingautoscroller.h"
#include "snow_shot/platform/windows/scrollinput.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QProcess>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#include <windowsx.h>
#endif

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

#if defined(Q_OS_WIN)
const QRect nativeSelection(-30000, -30000, 200, 200);

LRESULT CALLBACK scrollTargetProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static int steps = 0;
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        const bool expectedAxis = steps == 0 ? message == WM_MOUSEWHEEL : message == WM_MOUSEHWHEEL;
        const int expectedDelta = steps == 0 ? -120 : 120;
        const QPoint position(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        if (!expectedAxis || GET_WHEEL_DELTA_WPARAM(wParam) != expectedDelta ||
            position != nativeSelection.center() || GET_KEYSTATE_WPARAM(wParam) != 0 ||
            GetParent(window) == nullptr) {
            PostQuitMessage(1);
        } else if (++steps == 2) {
            PostQuitMessage(0);
        }
        return 0;
    }
    if (message == WM_TIMER) {
        PostQuitMessage(2);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int runNativeTarget() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = scrollTargetProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"SnowAutoScrollTestTarget";
    require(RegisterClassW(&windowClass) != 0, "native target class must register");
    const HWND window =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, windowClass.lpszClassName, L"",
                        WS_POPUP, nativeSelection.x(), nativeSelection.y(), nativeSelection.width(),
                        nativeSelection.height(), nullptr, nullptr, instance, nullptr);
    require(window != nullptr, "offscreen native target must be created");
    const HWND child = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0,
                                       0, 200, 200, window, nullptr, instance, nullptr);
    require(child != nullptr, "native scrollable child must be created");
    ShowWindow(window, SW_SHOWNOACTIVATE);
    SetTimer(window, 1, 10000, nullptr);
    std::cout << "ready" << std::endl;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    DestroyWindow(window);
    return static_cast<int>(message.wParam);
}

void nativeWheelMessagesReachTheSelection() {
    QProcess target;
    target.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--native-target")});
    require(target.waitForStarted(10000), "native target process must start");
    require(target.waitForReadyRead(10000) && target.readAllStandardOutput().contains("ready"),
            "native target must be ready before scrolling");
    snow_shot::platform::windows::sendScrollingWheelStep(nativeSelection, QPoint(0, -120));
    snow_shot::platform::windows::sendScrollingWheelStep(nativeSelection, QPoint(120, 0));
    require(target.waitForFinished(10000) && target.exitStatus() == QProcess::NormalExit &&
                target.exitCode() == 0,
            "native child must receive vertical/down and horizontal/right with signed coordinates");
}
#endif
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
#if defined(Q_OS_WIN)
    if (application.arguments().contains(QStringLiteral("--native-target"))) {
        return runNativeTarget();
    }
    nativeWheelMessagesReachTheSelection();
#endif
    using Mode = ScreenshotScrollingRecognitionMode;
    int steps = 0;
    QRect target;
    QPoint delta;
    snow_shot::capture_detail::ScreenshotScrollingAutoScroller scroller(
        [&](const QRect& selection, const QPoint& wheelDelta) {
            ++steps;
            target = selection;
            delta = wheelDelta;
        });
    auto* timer = scroller.findChild<QTimer*>();
    require(timer && timer->interval() == 200 && timer->timerType() == Qt::PreciseTimer,
            "auto-scroll must use a precise 200 ms timer");
    const auto tick = [&]() {
        require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
                "timer timeout must be invokable without wall-clock waits");
    };
    scroller.setEnabled(true);
    tick();
    require(steps == 0 && !timer->isActive(), "inactive captures must not scroll");
    const QRect selection(-1400, -300, 800, 600);
    scroller.start(selection, Mode::Vertical);
    tick();
    require(steps == 0, "new sessions must start disabled");
    scroller.setEnabled(true);
    require(steps == 0 && timer->isActive(), "first step must wait for the timer");
    tick();
    require(steps == 1 && target == selection && delta == QPoint(0, -120),
            "vertical ticks must send one downward wheel notch to the physical selection");
    tick();
    require(steps == 2, "each timeout must scroll exactly once");
    scroller.setMode(Mode::Horizontal);
    tick();
    require(steps == 3 && delta == QPoint(120, 0),
            "horizontal ticks must send one rightward horizontal wheel notch");
    scroller.setEnabled(false);
    tick();
    require(steps == 3 && !timer->isActive(), "deactivation must stop pending ticks");
    scroller.setEnabled(true);
    scroller.setPaused(true);
    tick();
    require(steps == 3 && !timer->isActive(), "export pause must suppress scrolling");
    scroller.setPaused(false);
    tick();
    require(steps == 4, "export cancellation must resume enabled auto-scroll");
    scroller.setPaused(true);
    scroller.setEnabled(false);
    scroller.setPaused(false);
    tick();
    require(steps == 4, "resuming must not reactivate a disabled toggle");
    scroller.setEnabled(true);
    scroller.stop();
    tick();
    require(steps == 4 && !timer->isActive(), "capture termination must stop scrolling");
    scroller.start(selection, Mode::Horizontal);
    tick();
    require(steps == 4, "capture restart must not restore prior activation");
    return 0;
}
