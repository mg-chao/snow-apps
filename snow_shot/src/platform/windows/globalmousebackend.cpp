#include "snow_shot/presentation/globalmousemanager.h"

#include <QGuiApplication>
#include <QThread>
#include <algorithm>
#include <array>
#include <utility>

#ifdef Q_OS_WIN
#include "globalmousebackend_p.h"
#endif

namespace snow_shot::presentation {
namespace {
class NativeGlobalMouseBackend final : public GlobalMouseBackend {
  public:
#ifdef Q_OS_WIN
    explicit NativeGlobalMouseBackend(detail::GlobalMouseNativeApi nativeApi = {})
        : api(std::move(nativeApi)) {}
#endif
    ~NativeGlobalMouseBackend() override {
        stop();
    }

    void start(Handler handler, FailureHandler failure) override {
        if (worker != nullptr) {
            return;
        }
        onEvent = std::move(handler);
        onFailure = std::move(failure);
#ifdef Q_OS_WIN
        if (!(api.supported ? api.supported()
                            : QGuiApplication::platformName() == QStringLiteral("windows"))) {
            onFailure(ERROR_NOT_SUPPORTED);
            return;
        }
        worker = new QObject;
        worker->moveToThread(&thread);
        QObject::connect(&thread, &QThread::finished, worker, &QObject::deleteLater);
        thread.start();
        QMetaObject::invokeMethod(
            worker,
            [this]() {
                current = this;
                keyboardHook =
                    api.installHook(WH_KEYBOARD_LL, keyboardCallback, GetModuleHandleW(nullptr), 0);
                if (keyboardHook == nullptr) {
                    const DWORD error = GetLastError();
                    unhook();
                    onFailure(error);
                    return;
                }
                sampleModifiers();
                syncMouseHook();
            },
            Qt::QueuedConnection);
#else
        onFailure(0);
#endif
    }

    void stop() override {
        if (worker == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            worker,
            [this]() {
#ifdef Q_OS_WIN
                unhook();
#endif
            },
            Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
        worker = nullptr;
        onEvent = {};
        onFailure = {};
    }

    void configure(const GlobalMouseConfiguration& value) override {
        if (worker == nullptr) {
            configuration = value;
            return;
        }
        onWorker([this, value]() {
            configuration = value;
#ifdef Q_OS_WIN
            sampleModifiers();
            syncMouseHook();
#endif
        });
    }

    void cancel(quint64 id) override {
        if (worker != nullptr) {
            onWorker([this, id]() {
                gesture.cancel(id);
#ifdef Q_OS_WIN
                syncMouseHook();
#endif
            });
        }
    }

    void beginButtonDrag(settings::SettingsGlobalMouseAction action) override {
#ifdef Q_OS_WIN
        POINT cursor;
        if (worker == nullptr || !api.cursorPosition(&cursor) ||
            (api.keyState(VK_LBUTTON) & 0x8000) == 0) {
            return;
        }
        const QPoint start(cursor.x, cursor.y);
        onWorker([this, action, start]() {
            if (keyboardHook == nullptr || !configuration.captureAvailable || gesture.pending() ||
                gesture.needsMouseInput()) {
                return;
            }
            if (!ensureMouseHook()) {
                return;
            }
            const auto result = gesture.beginButtonDrag(action, start);
            if (!result.event || !onEvent) {
                return;
            }
            onEvent(*result.event);
            // A quick release may precede this queued request on the hook thread.
            if ((api.keyState(VK_LBUTTON) & 0x8000) == 0) {
                POINT end;
                const QPoint position = api.cursorPosition(&end) ? QPoint(end.x, end.y) : start;
                static_cast<void>(
                    handle({GlobalMouseInput::Kind::Release, position, Qt::LeftButton}));
            }
        });
#else
        Q_UNUSED(action)
#endif
    }

  private:
    template <typename Callback> void onWorker(Callback callback) {
        if (QThread::currentThread() == &thread) {
            callback();
        } else {
            QMetaObject::invokeMethod(worker, std::move(callback), Qt::QueuedConnection);
        }
    }
#ifdef Q_OS_WIN
    static constexpr std::array modifierKeys{VK_LCONTROL, VK_RCONTROL, VK_LSHIFT, VK_RSHIFT,
                                             VK_LMENU,    VK_RMENU,    VK_LWIN,   VK_RWIN};

    void sampleModifiers() {
        for (const int key : modifierKeys) {
            modifierDown[key] = (api.keyState(key) & 0x8000) != 0;
        }
    }

    bool activationEligible() const {
        return configuration.captureAvailable && !gesture.pending() &&
               std::any_of(
                   configuration.bindings.cbegin(), configuration.bindings.cend(),
                   [this](const auto& binding) { return binding.modifiers == modifiers(); });
    }

    bool ensureMouseHook() {
        if (mouseHook == nullptr) {
            mouseHook = api.installHook(WH_MOUSE_LL, mouseCallback, GetModuleHandleW(nullptr), 0);
            if (mouseHook == nullptr) {
                onFailure(GetLastError());
                return false;
            }
        }
        return true;
    }

    void syncMouseHook() {
        if (keyboardHook == nullptr) {
            return;
        }
        const bool needed = gesture.needsMouseInput() || activationEligible();
        if (!needed) {
            if (mouseHook != nullptr) {
                api.removeHook(mouseHook);
                mouseHook = nullptr;
            }
            mouseHookAttempted = false;
        } else if (!mouseHookAttempted) {
            mouseHookAttempted = true;
            static_cast<void>(ensureMouseHook());
        }
    }

    void unhook() {
        if (mouseHook != nullptr) {
            api.removeHook(mouseHook);
            mouseHook = nullptr;
        }
        if (keyboardHook != nullptr) {
            api.removeHook(keyboardHook);
            keyboardHook = nullptr;
        }
        current = nullptr;
        consumedActivationKeys.fill(false);
        modifierDown.fill(false);
        mouseHookAttempted = false;
        escapeConsumed = false;
        gesture.reset();
    }

    Qt::KeyboardModifiers modifiers() const {
        Qt::KeyboardModifiers result;
        if (modifierDown[VK_LCONTROL] || modifierDown[VK_RCONTROL])
            result |= Qt::ControlModifier;
        if (modifierDown[VK_LSHIFT] || modifierDown[VK_RSHIFT])
            result |= Qt::ShiftModifier;
        if (modifierDown[VK_LMENU] || modifierDown[VK_RMENU])
            result |= Qt::AltModifier;
        if (modifierDown[VK_LWIN] || modifierDown[VK_RWIN])
            result |= Qt::MetaModifier;
        return result;
    }

    bool maskActivationKey(const KBDLLHOOKSTRUCT* release = nullptr) {
        // VK 0xE8 is an unassigned menu-mask key, with no application command.
        INPUT inputs[3]{};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = 0xE8;
        inputs[1] = inputs[0];
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        UINT count = 2;
        if (release != nullptr) {
            inputs[2].type = INPUT_KEYBOARD;
            inputs[2].ki.wVk = static_cast<WORD>(release->vkCode);
            inputs[2].ki.wScan = static_cast<WORD>(release->scanCode);
            inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
            if ((release->flags & LLKHF_EXTENDED) != 0) {
                inputs[2].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            count = 3;
        }
        return api.sendInput(count, inputs, sizeof(INPUT)) == count;
    }

    bool handle(const GlobalMouseInput& input) {
        const auto result = gesture.handle(input, configuration);
        if (result.maskActivationKey) {
            for (const int key : {VK_LWIN, VK_RWIN, VK_LMENU, VK_RMENU}) {
                if (modifierDown[key]) {
                    consumedActivationKeys[key] = true;
                }
            }
            static_cast<void>(maskActivationKey());
        }
        if (result.event && onEvent) {
            onEvent(*result.event);
        }
        if (input.kind != GlobalMouseInput::Kind::Move) {
            syncMouseHook();
        }
        return result.consumed;
    }

    static LRESULT CALLBACK mouseCallback(int code, WPARAM message, LPARAM data) {
        if (code != HC_ACTION || current == nullptr) {
            return current ? current->api.nextHook(nullptr, code, message, data)
                           : CallNextHookEx(nullptr, code, message, data);
        }
        const auto& native = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
        if ((native.flags & LLMHF_INJECTED) != 0 ||
            (message == WM_MOUSEMOVE && !current->gesture.active())) {
            return current->api.nextHook(nullptr, code, message, data);
        }
        GlobalMouseInput input;
        input.position = QPoint(native.pt.x, native.pt.y);
        using Kind = GlobalMouseInput::Kind;
        switch (message) {
        case WM_MOUSEMOVE:
            input.kind = Kind::Move;
            break;
        case WM_LBUTTONDOWN:
            input.kind = Kind::Press;
            input.button = Qt::LeftButton;
            break;
        case WM_LBUTTONUP:
            input.kind = Kind::Release;
            input.button = Qt::LeftButton;
            break;
        case WM_RBUTTONDOWN:
            input.kind = Kind::Press;
            input.button = Qt::RightButton;
            break;
        case WM_RBUTTONUP:
            input.kind = Kind::Release;
            input.button = Qt::RightButton;
            break;
        case WM_MBUTTONDOWN:
            input.kind = Kind::Press;
            input.button = Qt::MiddleButton;
            break;
        case WM_MBUTTONUP:
            input.kind = Kind::Release;
            input.button = Qt::MiddleButton;
            break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP:
            input.kind = message == WM_XBUTTONDOWN ? Kind::Press : Kind::Release;
            input.button =
                HIWORD(native.mouseData) == XBUTTON1 ? Qt::BackButton : Qt::ForwardButton;
            break;
        default:
            break;
        }
        if (input.kind == Kind::Press && !current->gesture.needsMouseInput()) {
            // Releases on another desktop may bypass this hook. Validate only a
            // candidate press, keeping all movement free of asynchronous key queries.
            current->sampleModifiers();
            if (!current->activationEligible()) {
                current->syncMouseHook();
                return current->api.nextHook(nullptr, code, message, data);
            }
            input.modifiers = current->modifiers();
            for (const auto& [key, button] :
                 {std::pair{VK_LBUTTON, Qt::LeftButton}, std::pair{VK_RBUTTON, Qt::RightButton},
                  std::pair{VK_MBUTTON, Qt::MiddleButton}, std::pair{VK_XBUTTON1, Qt::BackButton},
                  std::pair {
                      VK_XBUTTON2,
                      Qt::ForwardButton
                  }}) {
                if ((current->api.keyState(key) & 0x8000) != 0)
                    input.heldButtons |= button;
            }
        }
        return current->handle(input) ? 1 : current->api.nextHook(nullptr, code, message, data);
    }

    static LRESULT CALLBACK keyboardCallback(int code, WPARAM message, LPARAM data) {
        if (code == HC_ACTION && current != nullptr) {
            const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
            if (std::find(modifierKeys.cbegin(), modifierKeys.cend(),
                          static_cast<int>(key.vkCode)) != modifierKeys.cend()) {
                // Low-level callbacks precede the asynchronous key-state update.
                current->modifierDown[key.vkCode] = (key.flags & LLKHF_UP) == 0;
                current->syncMouseHook();
            }
            if ((key.flags & LLKHF_INJECTED) == 0 &&
                key.vkCode < current->consumedActivationKeys.size() &&
                current->consumedActivationKeys[key.vkCode]) {
                if (message == WM_KEYUP || message == WM_SYSKEYUP) {
                    current->consumedActivationKeys[key.vkCode] = false;
                    // Keep the mask and balanced release in one input batch. Ownership
                    // outlives the drag, including capture failure and cancellation.
                    if (current->maskActivationKey(&key)) {
                        return 1;
                    }
                    // If injection is blocked, forward the real release to avoid a stuck key.
                } else if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) {
                    return 1;
                }
            }
            if (key.vkCode == VK_ESCAPE && (key.flags & LLKHF_INJECTED) == 0) {
                if (message == WM_KEYUP || message == WM_SYSKEYUP) {
                    if (std::exchange(current->escapeConsumed, false))
                        return 1;
                } else if (current->gesture.pending()) {
                    GlobalMouseInput input;
                    input.kind = GlobalMouseInput::Kind::Cancel;
                    current->escapeConsumed = current->handle(input);
                    if (current->escapeConsumed)
                        return 1;
                } else if (current->escapeConsumed) {
                    return 1;
                }
            }
        }
        return current ? current->api.nextHook(nullptr, code, message, data)
                       : CallNextHookEx(nullptr, code, message, data);
    }

    static thread_local NativeGlobalMouseBackend* current;
    detail::GlobalMouseNativeApi api;
    std::array<bool, 256> consumedActivationKeys{};
    std::array<bool, 256> modifierDown{};
    HHOOK mouseHook = nullptr;
    HHOOK keyboardHook = nullptr;
    bool escapeConsumed = false;
    bool mouseHookAttempted = false;
#endif
    QThread thread;
    QObject* worker = nullptr;
    GlobalMouseConfiguration configuration;
    GlobalMouseGesture gesture;
    Handler onEvent;
    FailureHandler onFailure;
};

#ifdef Q_OS_WIN
thread_local NativeGlobalMouseBackend* NativeGlobalMouseBackend::current = nullptr;
#endif
} // namespace

#ifdef Q_OS_WIN
std::unique_ptr<GlobalMouseBackend>
detail::createGlobalMouseBackend(detail::GlobalMouseNativeApi api) {
    return std::make_unique<NativeGlobalMouseBackend>(std::move(api));
}
#endif

std::unique_ptr<GlobalMouseBackend> createGlobalMouseBackend() {
    return std::make_unique<NativeGlobalMouseBackend>();
}
} // namespace snow_shot::presentation
