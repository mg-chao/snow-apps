#ifndef SNOW_SHOT_PLATFORM_WINDOWS_PRINTSCREENSHORTCUTRECORDER_H
#define SNOW_SHOT_PLATFORM_WINDOWS_PRINTSCREENSHORTCUTRECORDER_H

#include <Qt>

#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

class QKeyEvent;
class QWidget;

namespace snow_shot::platform::windows {

#ifdef Q_OS_WIN
struct PrintScreenHookApi {
    decltype(&SetWindowsHookExW) installHook = &SetWindowsHookExW;
    decltype(&UnhookWindowsHookEx) removeHook = &UnhookWindowsHookEx;
    decltype(&CallNextHookEx) nextHook = &CallNextHookEx;
    decltype(&GetForegroundWindow) foregroundWindow = &GetForegroundWindow;
    decltype(&GetAsyncKeyState) asyncKeyState = &GetAsyncKeyState;
    std::function<bool()> supported;
};
#endif

class PrintScreenShortcutRecorder final {
  public:
    using Handler = std::function<void(Qt::KeyboardModifiers)>;

    PrintScreenShortcutRecorder(QWidget& target, Handler handler);
#ifdef Q_OS_WIN
    PrintScreenShortcutRecorder(QWidget& target, Handler handler, PrintScreenHookApi api);
#endif
    ~PrintScreenShortcutRecorder();

    bool handleKeyEvent(const QKeyEvent& event);
    void cancelPendingCapture();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::platform::windows

#endif // SNOW_SHOT_PLATFORM_WINDOWS_PRINTSCREENSHORTCUTRECORDER_H
