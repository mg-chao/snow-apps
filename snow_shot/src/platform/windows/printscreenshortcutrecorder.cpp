#include "snow_shot/platform/windows/printscreenshortcutrecorder.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPointer>
#include <QThread>
#include <QWidget>

#include <atomic>
#include <utility>
#include <optional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace snow_shot::platform::windows {

class PrintScreenShortcutRecorder::Impl final : public QObject, public QAbstractNativeEventFilter {
  public:
#ifdef Q_OS_WIN
    Impl(QWidget& targetWidget, Handler recordHandler, PrintScreenHookApi hookApi)
        : api(std::move(hookApi)), hookSupported(api.supported ? api.supported()
                                                               : QGuiApplication::platformName() ==
                                                                     QStringLiteral("windows")),
          target(&targetWidget), window(targetWidget.window()), handler(std::move(recordHandler)) {
#else
    Impl(QWidget& targetWidget, Handler recordHandler)
        : target(&targetWidget), window(targetWidget.window()), handler(std::move(recordHandler)) {
#endif
        target->installEventFilter(this);
        if (window != target) {
            window->installEventFilter(this);
            windowDestroyedConnection =
                connect(window, &QObject::destroyed, this, [this] { stopHook(); });
        }
        connect(target, &QObject::destroyed, this, [this] { stopHook(); });
        QCoreApplication::instance()->installNativeEventFilter(this);
        startHook();
    }

    ~Impl() override {
        stopHook();
        QCoreApplication::instance()->removeNativeEventFilter(this);
    }

    bool isActive() const {
        if (target == nullptr || window == nullptr || !target->isVisible()) {
            return false;
        }
#ifdef Q_OS_WIN
        if (hookSupported) {
            return api.foregroundWindow() == reinterpret_cast<HWND>(window->winId());
        }
#endif
        return window->isActiveWindow();
    }

    void record(bool pressed, bool autoRepeat, Qt::KeyboardModifiers modifiers,
                std::optional<quint32> nativeTimestamp = std::nullopt) {
        if (!isActive() || autoRepeat) {
            return;
        }
        if (pressed) {
            if (printPressed) {
                return;
            }
            printPressed = true;
        } else if (std::exchange(printPressed, false)) {
            return;
        }
        queueCapture(modifiers, nativeTimestamp ? *nativeTimestamp : noNativeTimestamp);
    }

    void queueCapture(Qt::KeyboardModifiers modifiers, quint64 timestamp) {
        nativeCaptureTimestamp.store(timestamp);
        // Shared by the GUI fallback and hook thread. Only publish a snapshot;
        // validation and widget changes always run through the GUI queue.
        const quint64 captureGeneration = ++generation;
        QMetaObject::invokeMethod(
            this,
            [this, captureGeneration, modifiers]() {
                if (captureGeneration == generation.load() && isActive()) {
                    const auto callback = handler;
                    callback(modifiers);
                }
            },
            Qt::QueuedConnection);
    }

    void refreshWindow() {
        QWidget* const scope = target ? target->window() : nullptr;
        if (window == scope) {
            return;
        }
        stopHook();
        if (window && window != target) {
            window->removeEventFilter(this);
        }
        disconnect(windowDestroyedConnection);
        window = scope;
        if (window && window != target) {
            window->installEventFilter(this);
            windowDestroyedConnection =
                connect(window, &QObject::destroyed, this, [this] { stopHook(); });
        }
        ++generation;
        printPressed = false;
        nativeCaptureTimestamp.store(noNativeTimestamp);
#ifdef Q_OS_WIN
        ++scopeGeneration;
#endif
    }

    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
        case QEvent::WindowDeactivate:
            if (QGuiApplication::platformName() != QStringLiteral("windows") || !isActive()) {
                ++generation;
                printPressed = false;
                nativeCaptureTimestamp.store(noNativeTimestamp);
#ifdef Q_OS_WIN
                ++scopeGeneration;
#endif
            }
            break;
        case QEvent::Hide:
            ++generation;
            printPressed = false;
            nativeCaptureTimestamp.store(noNativeTimestamp);
            stopHook();
            break;
        case QEvent::WindowActivate:
        case QEvent::Show:
            refreshWindow();
            startHook();
            break;
        case QEvent::ParentChange:
            if (watched == target) {
                refreshWindow();
            }
            break;
#ifdef Q_OS_WIN
        case QEvent::WinIdChange:
            if (hookSupported && window) {
                captureWindow.store(reinterpret_cast<HWND>(window->internalWinId()));
            }
            break;
#endif
        default:
            break;
        }
        return false;
    }

    bool nativeEventFilter(const QByteArray&, void* message, qintptr* result) override {
#ifdef Q_OS_WIN
        if (message == nullptr || !isActive()) {
            return false;
        }
        const auto& native = *static_cast<const MSG*>(message);
        if (native.hwnd != reinterpret_cast<HWND>(window->winId()) ||
            native.wParam != VK_SNAPSHOT) {
            return false;
        }
        const bool pressed = native.message == WM_KEYDOWN || native.message == WM_SYSKEYDOWN;
        if (!pressed && native.message != WM_KEYUP && native.message != WM_SYSKEYUP) {
            return false;
        }

        // Qt's Windows key mapper drops non-modifier releases without a preceding press:
        // qtbase/src/plugins/platforms/windows/qwindowskeymapper.cpp (KEYUP handling).
        auto modifiers = nativeModifiers(false);
        if ((native.lParam & (LPARAM{1} << 29)) != 0) {
            modifiers |= Qt::AltModifier;
        }
        record(pressed, pressed && (native.lParam & (LPARAM{1} << 30)) != 0, modifiers,
               native.time);
        if (result != nullptr) {
            *result = 0;
        }
        return true;
#else
        Q_UNUSED(message);
        Q_UNUSED(result);
        return false;
#endif
    }

    void startHook() {
#ifdef Q_OS_WIN
        // Keep the hook for the recording session: Windows can deliver input before
        // Qt processes WindowActivate. Only the native foreground window may capture.
        if (worker != nullptr || target == nullptr || window == nullptr || !target->isVisible() ||
            !hookSupported) {
            return;
        }
        // Publish only native state. The hook must never access QWidget or wait
        // for the GUI thread, even while it is creating or validating a modal.
        captureWindow.store(reinterpret_cast<HWND>(window->winId()));
        worker = new QObject;
        worker->moveToThread(&thread);
        connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        connect(&thread, &QThread::started, worker, [this] {
            active = this;
            hookPrintPressed = (api.asyncKeyState(VK_SNAPSHOT) & 0x8000) != 0;
            hookScopeGeneration = scopeGeneration.load();
            hook = api.installHook(WH_KEYBOARD_LL, keyboardHook, GetModuleHandleW(nullptr), 0);
            if (!hook) {
                qWarning() << "Print Screen recorder keyboard hook failed:" << GetLastError();
            }
        });
        thread.setObjectName(QStringLiteral("PrintScreenShortcutHook"));
        thread.start();
        // Establish interception before the recording session accepts input.
        QMetaObject::invokeMethod(worker, []{}, Qt::BlockingQueuedConnection);
#endif
    }

    void stopHook() {
#ifdef Q_OS_WIN
        captureWindow.store(nullptr);
        if (worker != nullptr) {
            QMetaObject::invokeMethod(
                worker,
                [this] {
                    if (hook) {
                        api.removeHook(hook);
                    }
                    hook = nullptr;
                    active = nullptr;
                },
                Qt::BlockingQueuedConnection);
            thread.quit();
            thread.wait();
            worker = nullptr;
        }
#endif
    }

#ifdef Q_OS_WIN
    Qt::KeyboardModifiers nativeModifiers(bool asynchronous) const {
        const auto down = [this, asynchronous](int key) {
            return ((asynchronous ? api.asyncKeyState(key) : GetKeyState(key)) & 0x8000) != 0;
        };
        Qt::KeyboardModifiers modifiers;
        if (down(VK_CONTROL)) {
            modifiers |= Qt::ControlModifier;
        }
        if (down(VK_SHIFT)) {
            modifiers |= Qt::ShiftModifier;
        }
        if (down(VK_MENU)) {
            modifiers |= Qt::AltModifier;
        }
        if (down(VK_LWIN) || down(VK_RWIN)) {
            modifiers |= Qt::MetaModifier;
        }
        return modifiers;
    }

    static LRESULT CALLBACK keyboardHook(int code, WPARAM message, LPARAM data) {
        Impl* const recorder = active;
        if (code == HC_ACTION && recorder != nullptr) {
            const auto scope = recorder->scopeGeneration.load();
            if (recorder->hookScopeGeneration != scope) {
                recorder->hookPrintPressed = false;
                recorder->hookScopeGeneration = scope;
            }
            const HWND owner = recorder->captureWindow.load();
            if (!owner || recorder->api.foregroundWindow() != owner) {
                ++recorder->generation;
                recorder->hookPrintPressed = false;
                recorder->nativeCaptureTimestamp.store(noNativeTimestamp);
                return recorder->api.nextHook(nullptr, code, message, data);
            }
            const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            if (key.vkCode == VK_SNAPSHOT &&
                (pressed || message == WM_KEYUP || message == WM_SYSKEYUP)) {
                // Print Screen does not change modifier state; sample the modifiers now,
                // before a queued UI callback can observe their subsequent release.
                auto modifiers = recorder->nativeModifiers(true);
                if ((key.flags & LLKHF_ALTDOWN) != 0) {
                    modifiers |= Qt::AltModifier;
                }
                const bool capture = pressed ? !std::exchange(recorder->hookPrintPressed, true)
                                             : !std::exchange(recorder->hookPrintPressed, false);
                if (capture) {
                    recorder->queueCapture(modifiers, key.time);
                }
                return 1;
            }
        }
        return recorder ? recorder->api.nextHook(nullptr, code, message, data)
                        : CallNextHookEx(nullptr, code, message, data);
    }

    static thread_local Impl* active;
    PrintScreenHookApi api;
    const bool hookSupported;
    QThread thread;
    QObject* worker = nullptr;
    HHOOK hook = nullptr;
    std::atomic<HWND> captureWindow{nullptr};
    std::atomic<quint64> scopeGeneration{0};
    quint64 hookScopeGeneration = 0;
    bool hookPrintPressed = false;
#endif
    QPointer<QWidget> target;
    QPointer<QWidget> window;
    QMetaObject::Connection windowDestroyedConnection;
    Handler handler;
    static constexpr quint64 noNativeTimestamp = quint64{1} << 32;
    std::atomic<quint64> nativeCaptureTimestamp{noNativeTimestamp};
    std::atomic<quint64> generation{0};
    bool printPressed = false;
};

#ifdef Q_OS_WIN
thread_local PrintScreenShortcutRecorder::Impl* PrintScreenShortcutRecorder::Impl::active = nullptr;
#endif

#ifdef Q_OS_WIN
PrintScreenShortcutRecorder::PrintScreenShortcutRecorder(QWidget& target, Handler handler)
    : PrintScreenShortcutRecorder(target, std::move(handler), PrintScreenHookApi{}) {}

PrintScreenShortcutRecorder::PrintScreenShortcutRecorder(QWidget& target, Handler handler,
                                                         PrintScreenHookApi api)
    : m_impl(std::make_unique<Impl>(target, std::move(handler), std::move(api))) {}
#else
PrintScreenShortcutRecorder::PrintScreenShortcutRecorder(QWidget& target, Handler handler)
    : m_impl(std::make_unique<Impl>(target, std::move(handler))) {}
#endif

PrintScreenShortcutRecorder::~PrintScreenShortcutRecorder() = default;

bool PrintScreenShortcutRecorder::handleKeyEvent(const QKeyEvent& event) {
    if (event.key() != Qt::Key_Print && event.key() != Qt::Key_SysReq) {
        const quint64 timestamp = m_impl->nativeCaptureTimestamp.load();
        if (event.type() == QEvent::KeyPress && event.timestamp() != 0 &&
            timestamp != Impl::noNativeTimestamp) {
            const quint32 elapsed =
                static_cast<quint32>(event.timestamp()) - static_cast<quint32>(timestamp);
            const bool modifier = event.key() == Qt::Key_Control || event.key() == Qt::Key_Shift ||
                                  event.key() == Qt::Key_Alt || event.key() == Qt::Key_Meta ||
                                  event.key() == Qt::Key_AltGr || event.key() == Qt::Key_Super_L ||
                                  event.key() == Qt::Key_Super_R;
            // Hook notifications precede Qt's queued key events. Do not let an older
            // modifier press clear the captured chord, including within the same millisecond.
            return elapsed > 0x7FFFFFFFU || (elapsed == 0 && modifier);
        }
        return false;
    }
    m_impl->record(event.type() == QEvent::KeyPress, event.isAutoRepeat(), event.modifiers());
    return true;
}

void PrintScreenShortcutRecorder::cancelPendingCapture() {
    ++m_impl->generation;
}

} // namespace snow_shot::platform::windows
