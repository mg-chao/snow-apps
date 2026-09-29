#pragma once
#ifdef Q_OS_WIN
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <QCursor>
#include <qt_windows.h>
#include <stdexcept>
namespace {
class NativeGroupDesktopUnavailable : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
void sendGroupKey(WORD key, bool down) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = key;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    if (SendInput(1, &input, sizeof(input)) != 1)
        throw std::runtime_error("SendInput failed");
}
template <class Predicate> bool awaitGroupNative(Predicate predicate) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000) {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        QThread::msleep(2);
    }
    return false;
}
void nativeSwitcherChecks() {
    QProcess host;
    const QPoint originalCursor = QCursor::pos();
    struct Cleanup {
        QProcess& host;
        QPoint cursor;
        bool restoreCursor = true;
        ~Cleanup() {
            for (WORD key : {WORD(VK_F20), WORD(VK_CONTROL), WORD(VK_MENU), WORD(VK_SHIFT)}) {
                INPUT input{};
                input.type = INPUT_KEYBOARD;
                input.ki.wVk = key;
                input.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &input, sizeof(input));
            }
            if (restoreCursor)
                QCursor::setPos(cursor);
            host.terminate();
            if (!host.waitForFinished(3000)) {
                host.kill();
                host.waitForFinished(3000);
            }
        }
    } cleanup{host, originalCursor};
    const auto check = [](bool value, const char* message) {
        if (!value)
            throw std::runtime_error(message);
    };
    host.start(
        QCoreApplication::applicationFilePath(),
        {QStringLiteral("--native-host"), QStringLiteral("-platform"), QStringLiteral("windows")});
    check(host.waitForStarted(5000) && host.waitForReadyRead(5000), "native focus host starts");
    const auto hostId = host.readLine().trimmed().toULongLong();
    HWND hostWindow = reinterpret_cast<HWND>(static_cast<quintptr>(hostId));
    check(IsWindow(hostWindow), "native focus host exposes HWND");
    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD currentThread = GetCurrentThreadId();
    const bool attached = foregroundThread != currentThread &&
                          AttachThreadInput(currentThread, foregroundThread, TRUE);
    SetForegroundWindow(hostWindow);
    if (attached)
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    SetWindowPos(hostWindow, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    RECT hostRect{};
    check(GetWindowRect(hostWindow, &hostRect), "host bounds available");
    SetCursorPos((hostRect.left + hostRect.right) / 2, (hostRect.top + hostRect.bottom) / 2);
    INPUT focusClick[2]{};
    focusClick[0].type = INPUT_MOUSE;
    focusClick[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    focusClick[1].type = INPUT_MOUSE;
    focusClick[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    check(SendInput(2, focusClick, sizeof(INPUT)) == 2, "focus external host with a native click");
    if (!awaitGroupNative([&] { return GetForegroundWindow() == hostWindow; })) {
        POINT cursor{};
        GetCursorPos(&cursor);
        std::cerr << "host=" << hostWindow << " visible=" << IsWindowVisible(hostWindow)
                  << " foreground=" << GetForegroundWindow()
                  << " at-cursor=" << WindowFromPoint(cursor) << " rect=" << hostRect.left << ","
                  << hostRect.top << "," << hostRect.right << "," << hostRect.bottom
                  << " root=" << GetAncestor(hostWindow, GA_ROOT) << " cursor=" << cursor.x << ","
                  << cursor.y << '\n';
        cleanup.restoreCursor = cursor.x == (hostRect.left + hostRect.right) / 2 &&
                                cursor.y == (hostRect.top + hostRect.bottom) / 2;
        throw NativeGroupDesktopUnavailable("Native test requires an idle interactive desktop; "
                                            "external focus could not be acquired.");
    }
    Fixture fixture;
    GlobalShortcutManager shortcuts;
    shortcuts.initialize();
    check(shortcuts.setShortcuts(GlobalShortcutAction::SwitchWindowGroup,
                                 {QStringLiteral("Ctrl+Alt+Shift+F20")}) &&
              shortcuts.state(GlobalShortcutAction::SwitchWindowGroup).status ==
                  GlobalShortcutStatus::Registered,
          "native shortcut registers");
    WindowGroupSwitcherController picker(shortcuts, fixture.groups);
    QObject::connect(&shortcuts, &GlobalShortcutManager::bindingActivated, &picker,
                     [&](GlobalShortcutAction action, int id) {
                         if (action == GlobalShortcutAction::SwitchWindowGroup)
                             picker.activateShortcut(id);
                     });
    for (QScreen* screen : QGuiApplication::screens()) {
        check(fixture.groups.setActiveGroup(QStringLiteral("default")), "reset native group");
        QCursor::setPos(screen->availableGeometry().center());
        for (WORD key : {WORD(VK_CONTROL), WORD(VK_MENU), WORD(VK_SHIFT), WORD(VK_F20)})
            sendGroupKey(key, true);
        check(awaitGroupNative([&] { return picker.isVisible(); }), "native hotkey opens popup");
        check(picker.selectedGroupId() == fixture.alpha &&
                  fixture.groups.activeGroupId() == QStringLiteral("default"),
              "native activation only previews");
        check(screen->availableGeometry().contains(picker.popup()->geometry()),
              "popup uses cursor monitor and its scale");
        check(GetForegroundWindow() == hostWindow,
              "showing popup preserves external keyboard focus");
        HWND popupWindow = reinterpret_cast<HWND>(picker.popup()->winId());
        check((GetWindowLongPtrW(popupWindow, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
              "popup has native topmost style");
        bool above = false;
        for (HWND window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT)) {
            if (window == popupWindow) {
                above = true;
                break;
            }
            if (window == hostWindow)
                break;
        }
        check(above, "popup stacks above existing topmost host");
        // Repeated keydown is a held key, not a new activation.
        sendGroupKey(VK_F20, true);
        QThread::msleep(25);
        QCoreApplication::processEvents();
        check(picker.selectedGroupId() == fixture.alpha, "native auto-repeat is suppressed");
        sendGroupKey(VK_F20, false);
        QThread::msleep(25);
        QCoreApplication::processEvents();
        check(picker.isVisible(), "main key release alone keeps popup open");
        sendGroupKey(VK_F20, true);
        check(awaitGroupNative([&] { return picker.selectedGroupId() == fixture.beta; }),
              "fresh native key press cycles");
        sendGroupKey(VK_F20, false);
        sendGroupKey(VK_CONTROL, false);
        sendGroupKey(VK_MENU, false);
        QThread::msleep(25);
        QCoreApplication::processEvents();
        check(picker.isVisible(), "last modifier alone delays commit");
        sendGroupKey(VK_SHIFT, false);
        check(awaitGroupNative([&] { return !picker.isVisible(); }) &&
                  fixture.groups.activeGroupId() == fixture.beta,
              "final native release commits");
        check(GetForegroundWindow() == hostWindow, "confirmation preserves external focus");
        picker.openPicker();
        QCoreApplication::processEvents();
        auto* list = picker.popup()->findChild<QListView*>();
        const auto point =
            list->viewport()->mapToGlobal(list->visualRect(list->model()->index(1, 0)).center());
        // Deliver through the native window procedure without racing desktop pointer movement.
        popupWindow = reinterpret_cast<HWND>(picker.popup()->winId());
        const QPoint local = picker.popup()->mapFromGlobal(point);
        const qreal scale = picker.popup()->devicePixelRatioF();
        const LPARAM position = MAKELPARAM(qRound(local.x() * scale), qRound(local.y() * scale));
        check(SendMessageW(popupWindow, WM_MOUSEACTIVATE, reinterpret_cast<WPARAM>(hostWindow),
                           MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN)) == MA_NOACTIVATE,
              "native click requests no keyboard activation");
        check(PostMessageW(popupWindow, WM_LBUTTONDOWN, MK_LBUTTON, position) &&
                  PostMessageW(popupWindow, WM_LBUTTONUP, 0, position),
              "send native group click");
        check(awaitGroupNative([&] { return !picker.isVisible(); }) &&
                  fixture.groups.activeGroupId() == fixture.alpha,
              "native mouse click commits clicked group");
        check(GetForegroundWindow() == hostWindow, "clicking popup does not take keyboard focus");
    }
}
} // namespace
#endif
