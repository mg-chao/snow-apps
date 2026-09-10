#ifndef SNOW_SHOT_CLOSE_RELEASE_NATIVE_TEST_SUPPORT_H
#define SNOW_SHOT_CLOSE_RELEASE_NATIVE_TEST_SUPPORT_H

#include <QApplication>
#include <QElapsedTimer>
#include <QPointer>
#include <QKeyEvent>
#include <QProcess>
#include <QThread>
#include <QWidget>

#include <cstdlib>
#include <iostream>
#include <future>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <qt_windows.h>

namespace close_release_native_test {
constexpr UINT queryInput = WM_APP + 41;
constexpr UINT resetInput = WM_APP + 42;
constexpr UINT allowActivation = WM_APP + 43;
constexpr UINT queryLastInput = WM_APP + 44;

inline void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Action> int run(Action action) {
    try {
        action();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

inline void pump(int milliseconds) {
    QElapsedTimer timer;
    timer.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    } while (timer.elapsed() < milliseconds);
}

inline void activate(HWND window) {
    const DWORD current = GetCurrentThreadId();
    const DWORD foreground = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const bool attached = foreground && foreground != current &&
                          AttachThreadInput(current, foreground, TRUE) != FALSE;
    SetForegroundWindow(window);
    BringWindowToTop(window);
    if (attached) {
        AttachThreadInput(current, foreground, FALSE);
    }
}

inline LRESULT CALLBACK receiverProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    static int received = 0;
    static UINT lastInput = 0;
    switch (message) {
    case queryInput:
        return received;
    case queryLastInput:
        return lastInput;
    case resetInput:
        received = 0;
        return 0;
    case allowActivation:
        return AllowSetForegroundWindow(static_cast<DWORD>(wParam));
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_NCLBUTTONUP:
    case WM_NCRBUTTONUP:
    case WM_NCMBUTTONUP:
    case WM_CONTEXTMENU:
        ++received;
        lastInput = message;
        return 0;
    case WM_DESTROY:
        QCoreApplication::quit();
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

inline bool receiverRequested() {
    return qApp->arguments().contains(QStringLiteral("--close-release-receiver"));
}

inline int runReceiver() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = receiverProcedure;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"SnowCloseReleaseReceiver";
    require(RegisterClassW(&cls) != 0, "register native receiver");
    const HWND window =
        CreateWindowExW(0, cls.lpszClassName, L"Snow Shot input release test", WS_OVERLAPPEDWINDOW,
                        50, 50, 900, 700, nullptr, nullptr, cls.hInstance, nullptr);
    require(window != nullptr, "create receiver window");
    ShowWindow(window, SW_SHOW);
    std::cout << "receiver=" << reinterpret_cast<quintptr>(window) << std::endl;
    return qApp->exec();
}

class Receiver final {
  public:
    Receiver() {
        process.start(QCoreApplication::applicationFilePath(),
                      {QStringLiteral("--close-release-receiver"), QStringLiteral("-platform"),
                       QStringLiteral("windows")});
        require(process.waitForStarted(3000), "start separate input receiver process");
        QByteArray output;
        QElapsedTimer timer;
        timer.start();
        while (!window && timer.elapsed() < 5000) {
            static_cast<void>(process.waitForReadyRead(50));
            output += process.readAllStandardOutput();
            const auto start = output.indexOf("receiver=");
            if (start >= 0 && output.indexOf('\n', start) >= 0) {
                const auto value = output.mid(start + 9).split('\n').first().trimmed();
                window = reinterpret_cast<HWND>(value.toULongLong());
            }
        }
        require(window && IsWindow(window), "receiver must publish its HWND");
    }

    ~Receiver() {
        PostMessageW(window, WM_CLOSE, 0, 0);
        if (!process.waitForFinished(2000)) {
            process.kill();
            static_cast<void>(process.waitForFinished(2000));
        }
    }

    // Test-only SendInput exercises actual routing across process boundaries.
    // No input synthesis is used by the production close-on-release implementation.
    static void key(bool up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = VK_ESCAPE;
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        require(SendInput(1, &input, sizeof(input)) == 1, "send native Escape input");
    }

    static void mouse(Qt::MouseButton button, bool up) {
        INPUT input{};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags =
            button == Qt::LeftButton    ? (up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN)
            : button == Qt::RightButton ? (up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN)
                                        : (up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN);
        require(SendInput(1, &input, sizeof(input)) == 1, "send native mouse input");
    }

    void verify(QWidget& target, Qt::MouseButton button = Qt::NoButton, bool doubleClick = false,
                bool caption = false) {
        std::cerr << "Verifying native dismissal: button=" << static_cast<int>(button)
                  << " double-click=" << doubleClick << '\n';
        struct Observer final : QObject {
            QStringList events;
            Observer() {
                qApp->installEventFilter(this);
            }
            ~Observer() override {
                qApp->removeEventFilter(this);
            }
            bool eventFilter(QObject* receiver, QEvent* event) override {
                if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease ||
                    event->type() == QEvent::MouseButtonPress ||
                    event->type() == QEvent::MouseButtonRelease ||
                    event->type() == QEvent::ApplicationDeactivate) {
                    events.append(QStringLiteral("%1:%2")
                                      .arg(receiver->metaObject()->className())
                                      .arg(static_cast<int>(event->type())));
                    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
                        const auto* key = static_cast<QKeyEvent*>(event);
                        events.last() += QStringLiteral("/key=%1/repeat=%2")
                                             .arg(key->key())
                                             .arg(key->isAutoRepeat());
                    }
                }
                return false;
            }
        } observer;
        QPointer<QWidget> guarded(&target);
        const HWND targetWindow = reinterpret_cast<HWND>(target.winId());
        RECT bounds{};
        require(GetWindowRect(targetWindow, &bounds) != FALSE, "get target rectangle");
        SetWindowPos(window, HWND_NOTOPMOST, bounds.left - 50, bounds.top - 50,
                     bounds.right - bounds.left + 100, bounds.bottom - bounds.top + 100,
                     SWP_NOACTIVATE);
        AllowSetForegroundWindow(static_cast<DWORD>(process.processId()));
        activate(window);
        pump(30);
        SendMessageW(window, allowActivation, GetCurrentProcessId(), 0);
        target.activateWindow();
        target.raise();
        activate(targetWindow);
        pump(100);
        require(GetForegroundWindow() == targetWindow, "dismissal target must own foreground");
        SendMessageW(window, resetInput, 0, 0);
        if (button == Qt::NoButton) {
            key(false);
            pump(40);
            const bool visibleAfterPress = guarded && guarded->isVisible();
            key(false);
            pump(40);
            const bool visibleAfterRepeat = guarded && guarded->isVisible();
            key(true);
            require(visibleAfterPress && visibleAfterRepeat,
                    "native key down/repeat must not dismiss");
        } else {
            const int x = (bounds.left + bounds.right) / 2;
            const int y = (bounds.top + bounds.bottom) / 2;
            require(SetCursorPos(x, y) != FALSE, "position pointer over target");
            pump(30);
            if (caption) {
                require(SendMessageW(targetWindow, WM_NCHITTEST, 0, MAKELPARAM(x, y)) == HTCAPTION,
                        "pinned gesture must exercise its native caption");
            }
            const auto click = [&] {
                // A caption down can enter USER32's synchronous move loop.
                // Release from another thread so that loop cannot block its own
                // test driver. Observe visibility while the button is still held.
                auto released = std::async(std::launch::async, [=] {
                    Sleep(60);
                    const bool visible = IsWindow(targetWindow) && IsWindowVisible(targetWindow);
                    mouse(button, true);
                    return visible;
                });
                mouse(button, false);
                pump(70);
                require(released.get(), "native button down must not dismiss");
            };
            if (doubleClick) {
                click();
            }
            click();
        }
        QElapsedTimer timer;
        timer.start();
        while (guarded && guarded->isVisible() && timer.elapsed() < 5000) {
            pump(10);
        }
        if (guarded && guarded->isVisible()) {
            std::cerr << "Input trace: " << observer.events.join(QStringLiteral(", ")).toStdString()
                      << " foreground=" << GetForegroundWindow() << " target=" << targetWindow
                      << " focus="
                      << (QApplication::focusWidget()
                              ? QApplication::focusWidget()->metaObject()->className()
                              : "none")
                      << " receiver-input=" << SendMessageW(window, queryInput, 0, 0) << '\n';
        }
        require(!guarded || !guarded->isVisible(), "native release must dismiss target");
        pump(100);
        if (SendMessageW(window, queryInput, 0, 0) != 0) {
            std::cerr << "Leaked input: button=" << static_cast<int>(button)
                      << " count=" << SendMessageW(window, queryInput, 0, 0)
                      << " last=" << SendMessageW(window, queryLastInput, 0, 0)
                      << " trace=" << observer.events.join(QStringLiteral(", ")).toStdString()
                      << '\n';
        }
        require(SendMessageW(window, queryInput, 0, 0) == 0,
                "dismissal input leaked into the separate receiver process");
        activate(window);
        pump(50);
        require(GetForegroundWindow() == window, "receiver must regain foreground");
        key(false);
        key(true);
        pump(80);
        require(SendMessageW(window, queryInput, 0, 0) >= 2,
                "next independent input must reach the receiver normally");
    }

  private:
    QProcess process;
    HWND window = nullptr;
};
} // namespace close_release_native_test
#endif

#endif
