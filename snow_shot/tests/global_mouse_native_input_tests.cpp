#include "../src/platform/windows/globalmousebackend_p.h"

#include <QAbstractEventDispatcher>
#include <QSemaphore>
#include <QTimer>
#include <array>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using namespace snow_shot::presentation;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct NativeInput {
    static inline NativeInput* instance = nullptr;
    HOOKPROC mouse = nullptr;
    HOOKPROC keyboard = nullptr;
    std::array<bool, 256> down{};
    std::vector<INPUT> sent;
    std::vector<GlobalMouseDragEvent> events;
    std::function<void()> scenario;
    bool injectionBlocked = false;
    bool mouseInstallBlocked = false;
    int mouseInstalls = 0;
    int mouseRemovals = 0;
    int queries = 0;
    int failures = 0;
    QSemaphore completed;
    GlobalMouseBackend* backend = nullptr;
    std::function<void()> afterMouseInstall;
    GlobalMouseConfiguration configuration{{{settings::SettingsGlobalMouseAction::ScreenshotCopy,
                                             Qt::MetaModifier | Qt::AltModifier, Qt::LeftButton}},
                                           true};

    static HHOOK WINAPI install(int kind, HOOKPROC callback, HINSTANCE, DWORD) {
        if (kind == WH_MOUSE_LL) {
            ++instance->mouseInstalls;
            if (instance->mouseInstallBlocked) {
                SetLastError(ERROR_ACCESS_DENIED);
                return nullptr;
            }
            instance->mouse = callback;
            if (instance->afterMouseInstall) {
                instance->afterMouseInstall();
            }
        } else {
            instance->keyboard = callback;
            QTimer::singleShot(0, QAbstractEventDispatcher::instance(), [] {
                instance->scenario();
                instance->completed.release();
            });
        }
        return reinterpret_cast<HHOOK>(static_cast<INT_PTR>(kind));
    }

    static BOOL WINAPI remove(HHOOK hook) {
        if (hook == reinterpret_cast<HHOOK>(static_cast<INT_PTR>(WH_MOUSE_LL))) {
            instance->mouse = nullptr;
            ++instance->mouseRemovals;
        } else {
            instance->keyboard = nullptr;
        }
        return TRUE;
    }

    static SHORT WINAPI keyState(int key) {
        ++instance->queries;
        if (key == VK_MENU) {
            return static_cast<SHORT>(keyState(VK_LMENU) | keyState(VK_RMENU));
        }
        return instance->down.at(static_cast<size_t>(key)) ? static_cast<SHORT>(0x8000) : 0;
    }

    static BOOL WINAPI cursorPosition(LPPOINT point) {
        *point = {20, 30};
        return TRUE;
    }

    static LRESULT WINAPI next(HHOOK, int, WPARAM, LPARAM) {
        return 0;
    }

    static UINT WINAPI send(UINT count, LPINPUT inputs, int size) {
        require(size == sizeof(INPUT), "native input size must be correct");
        if (instance->injectionBlocked) {
            return 0;
        }
        for (UINT index = 0; index < count; ++index) {
            const INPUT input = inputs[index];
            instance->sent.push_back(input);
            require(instance->key(input.ki.wVk, (input.ki.dwFlags & KEYEVENTF_KEYUP) != 0,
                                  LLKHF_INJECTED) == 0,
                    "injected input must pass through without recursive masking");
        }
        return count;
    }

    LRESULT key(DWORD code, bool up, DWORD flags = 0) {
        KBDLLHOOKSTRUCT input{};
        input.vkCode = code;
        input.scanCode = 42;
        input.flags = flags | (up ? LLKHF_UP : 0);
        if (code == VK_LWIN || code == VK_RWIN || code == VK_RMENU) {
            input.flags |= LLKHF_EXTENDED;
        }
        const bool alt = code == VK_LMENU || code == VK_RMENU;
        const WPARAM message =
            alt ? (up ? WM_SYSKEYUP : WM_SYSKEYDOWN) : (up ? WM_KEYUP : WM_KEYDOWN);
        const auto result = keyboard(HC_ACTION, message, reinterpret_cast<LPARAM>(&input));
        if (result == 0) {
            down.at(code) = !up;
        }
        return result;
    }

    LRESULT button(bool up) {
        MSLLHOOKSTRUCT input{};
        input.pt = {20, 30};
        const auto result = mouse ? mouse(HC_ACTION, up ? WM_LBUTTONUP : WM_LBUTTONDOWN,
                                          reinterpret_cast<LPARAM>(&input))
                                  : 0;
        if (result == 0) {
            down[VK_LBUTTON] = !up;
        }
        return result;
    }

    LRESULT move(int x, DWORD flags = 0) {
        MSLLHOOKSTRUCT input{};
        input.pt = {x, 30};
        input.flags = flags;
        return mouse ? mouse(HC_ACTION, WM_MOUSEMOVE, reinterpret_cast<LPARAM>(&input)) : 0;
    }

    void run(int iterations = 1) {
        instance = this;
        detail::GlobalMouseNativeApi api{
            install, remove, keyState, send, next, [] { return true; }, cursorPosition};
        auto ownedBackend = detail::createGlobalMouseBackend(std::move(api));
        backend = ownedBackend.get();
        backend->configure(configuration);
        for (int iteration = 0; iteration < iterations; ++iteration) {
            backend->start([this](const auto& event) { events.push_back(event); },
                           [this](quint32) { ++failures; });
            require(completed.tryAcquire(1, 10000), "hook-thread scenario must complete");
            backend->stop();
        }
        instance = nullptr;
    }
};
} // namespace

void globalMouseNativeActivationKeyTests() {
    for (const DWORD windowsKey : {VK_LWIN, VK_RWIN}) {
        for (const DWORD altKey : {VK_LMENU, VK_RMENU}) {
            for (const int ending : {0, 1, 2}) {
                NativeInput input;
                input.scenario = [&] {
                    require(input.key(windowsKey, false) == 0 && input.key(altKey, false) == 0,
                            "activation key presses must preserve ordinary shortcuts");
                    require(input.button(false) == 1 && input.events.size() == 1,
                            "the configured physical gesture must activate and consume the mouse");
                    if (ending == 1) {
                        require(input.button(true) == 1, "the mouse release must be consumed");
                    } else if (ending == 2) {
                        require(input.key(VK_ESCAPE, false) == 1 && input.key(VK_ESCAPE, true) == 1,
                                "Escape must cancel the gesture with a balanced consumed pair");
                    }
                    for (const DWORD key : {windowsKey, altKey}) {
                        const auto sentBefore = input.sent.size();
                        require(
                            input.key(key, false) == 1,
                            "used activation key repeats must be consumed after finish or cancel");
                        require(input.key(key, true) == 1,
                                "used Windows and Alt releases must not reach the OS unmasked");
                        require(input.sent.size() == sentBefore + 3,
                                "release must inject a mask pair and a balanced modifier key-up");
                        const auto& release = input.sent.back();
                        require(input.sent[sentBefore].ki.wVk == 0xE8 &&
                                    input.sent[sentBefore].ki.dwFlags == 0 &&
                                    input.sent[sentBefore + 1].ki.wVk == 0xE8 &&
                                    input.sent[sentBefore + 1].ki.dwFlags == KEYEVENTF_KEYUP,
                                "the mask key must precede the balanced modifier release");
                        require(
                            release.ki.wVk == key && release.ki.wScan == 42 &&
                                (release.ki.dwFlags & KEYEVENTF_KEYUP) != 0 && !input.down[key],
                            "the forwarded press must be balanced without leaving the key stuck");
                        require(((release.ki.dwFlags & KEYEVENTF_EXTENDEDKEY) != 0) ==
                                    (key != VK_LMENU),
                                "replacement releases must preserve extended key identity");
                        require(input.key(key, false) == 0 && input.key(key, true) == 0,
                                "a later standalone activation key must work normally");
                    }
                };
                input.run();
            }
        }
    }

    for (const auto modifier : {Qt::MetaModifier, Qt::AltModifier}) {
        NativeInput input;
        input.configuration.bindings[0].modifiers = modifier;
        const DWORD left = modifier == Qt::MetaModifier ? VK_LWIN : VK_LMENU;
        const DWORD right = modifier == Qt::MetaModifier ? VK_RWIN : VK_RMENU;
        input.scenario = [&] {
            require(input.key(left, false) == 0 && input.key(left, true) == 0 && input.sent.empty(),
                    "standalone Windows and Alt must pass through without masking");
            require(input.key(left, false) == 0 && input.key(right, false) == 0 &&
                        input.button(false) == 1,
                    "both physical sides must activate a single-modifier binding");
            require(input.key('A', false) == 0 && input.key('A', true) == 0,
                    "unrelated keys must not be consumed");
            require(input.key(left, true, LLKHF_INJECTED) == 0,
                    "injected releases must not steal physical key ownership");
            require(input.key(left, true) == 1 && input.key(right, true) == 1,
                    "both physical activation keys must retain independent ownership");
        };
        input.run();
    }

    for (const bool captureAvailable : {false, true}) {
        NativeInput input;
        input.configuration.bindings[0].modifiers = Qt::MetaModifier;
        input.configuration.captureAvailable = captureAvailable;
        input.scenario = [&] {
            require(input.key(VK_LWIN, false) == 0, "initial key-down must pass through");
            require(input.button(false) == (captureAvailable ? 1 : 0),
                    "capture availability must determine mouse consumption");
            input.injectionBlocked = true;
            require(
                input.key(VK_LWIN, true) == 0 && !input.down[VK_LWIN],
                "unclaimed keys and failed release injection must forward the physical release");
            require(input.key(VK_LWIN, false) == 0 && input.key(VK_LWIN, true) == 0,
                    "failed injection must not leave stale ownership");
        };
        input.run();
    }
}

void globalMouseNativeStopFromWorkerThreadTests() {
    NativeInput input;
    input.scenario = [&] {
        // The scenario runs on the hook thread, so stopping must retire the
        // hooks inline instead of blocking on a queued invocation to itself.
        input.backend->stop();
        require(input.mouse == nullptr && input.keyboard == nullptr,
                "a worker-thread stop must unhook synchronously");
    };
    input.run();
    require(input.failures == 0, "a worker-thread stop reported a failure");
}

void globalMouseNativePerformanceTests() {
    {
        NativeInput input;
        input.scenario = [&] {
            require(input.mouse == nullptr, "idle startup must install only the keyboard hook");
            const int idleQueries = input.queries;
            for (int i = 0; i < 1000; ++i)
                input.move(i);
            require(input.queries == idleQueries && input.mouseInstalls == 0,
                    "ordinary desktop movement must do no Snow Shot mouse work");
            input.key(VK_LWIN, false);
            require(input.mouse == nullptr, "partial modifier matches must not arm mouse input");
            input.key(VK_LMENU, false);
            require(input.mouse != nullptr, "the last modifier must synchronously arm mouse input");
            const int armedQueries = input.queries;
            for (int i = 0; i < 1000; ++i)
                input.move(i);
            require(input.queries == armedQueries && input.events.empty(),
                    "armed movement must forward without key queries or events");
            input.down[VK_RBUTTON] = true;
            require(input.button(false) == 0 && input.events.empty(),
                    "a button held before arming must prevent taking over its drag");
            input.button(true);
            input.down[VK_RBUTTON] = false;
            require(input.button(false) == 1, "eligible press must still activate");
            const int dragQueries = input.queries;
            const auto before = input.events.size();
            input.move(50, LLMHF_INJECTED);
            require(input.events.size() == before, "injected movement must be ignored");
            for (int i = 0; i < 1000; ++i)
                input.move(i);
            require(input.queries == dragQueries && input.events.size() == before + 1000,
                    "active movement must deliver positions without querying any key state");
            input.configuration.captureAvailable = false;
            input.backend->configure(input.configuration);
            input.key(VK_LWIN, true);
            input.key(VK_LMENU, true);
            require(input.mouse != nullptr, "busy capture and released modifiers must retain drag");
            input.key(VK_ESCAPE, false);
            input.key(VK_ESCAPE, true);
            require(input.mouse != nullptr, "cancel must retain swallowed button ownership");
            require(input.button(true) == 1 && input.mouse == nullptr,
                    "the final swallowed release must disarm mouse monitoring");
        };
        input.run();
        require(input.failures == 0, "normal native lifecycle must not report failures");
    }
    for (const auto modifier : {Qt::ControlModifier, Qt::ShiftModifier}) {
        NativeInput input;
        input.configuration.bindings[0].modifiers = modifier;
        const DWORD left = modifier == Qt::ControlModifier ? VK_LCONTROL : VK_LSHIFT;
        const DWORD right = modifier == Qt::ControlModifier ? VK_RCONTROL : VK_RSHIFT;
        input.down[left] = true;
        input.scenario = [&] {
            require(input.mouse != nullptr, "startup must recognize already-held modifiers");
            input.key(right, false);
            input.key(left, true);
            require(input.mouse != nullptr, "either modifier side must keep monitoring armed");
            input.key(VK_LWIN, false);
            require(input.mouse == nullptr, "extra modifiers must disarm exact combinations");
            input.key(VK_LWIN, true);
            require(input.mouse != nullptr, "removing an extra modifier must rearm immediately");
            input.configuration.captureAvailable = false;
            input.backend->configure(input.configuration);
            require(input.mouse == nullptr, "unavailable capture must disarm idle monitoring");
            input.configuration.captureAvailable = true;
            input.backend->configure(input.configuration);
            require(input.mouse != nullptr, "configuration changes must honor held modifiers");
            input.configuration.bindings.clear();
            input.backend->configure(input.configuration);
            require(input.mouse == nullptr, "clearing bindings must remove idle mouse monitoring");
        };
        input.run();
    }
    {
        NativeInput input;
        input.configuration.bindings[0].modifiers = Qt::ControlModifier | Qt::AltModifier;
        input.scenario = [&] {
            input.key(VK_LCONTROL, false);
            input.key(VK_LMENU, false);
            require(input.mouse != nullptr, "the configured modifiers must arm monitoring");
            input.down[VK_LCONTROL] = false;
            input.down[VK_LMENU] = false;
            require(input.button(false) == 0 && input.events.empty() && input.mouse == nullptr,
                    "missed modifier releases must not turn an ordinary click into a gesture");
        };
        input.run();
    }
    {
        NativeInput input;
        input.configuration.bindings[0].modifiers = Qt::MetaModifier;
        input.mouseInstallBlocked = true;
        input.scenario = [&] {
            input.key(VK_LWIN, false);
            require(input.mouse == nullptr && input.failures == 1,
                    "failed dynamic installation must report the existing failure signal");
            input.key(VK_LWIN, false);
            require(input.failures == 1, "key repeats must not repeatedly retry a failed hook");
            input.key(VK_LWIN, true);
            input.mouseInstallBlocked = false;
            input.key(VK_LWIN, false);
            require(input.mouse != nullptr && input.button(false) == 1,
                    "the next activation edge must recover from installation failure");
            const auto id = input.events.back().id;
            input.backend->cancel(id);
            input.key(VK_LWIN, true);
            require(input.mouse != nullptr && input.button(true) == 1 && input.mouse == nullptr,
                    "controller cancellation must drain its consumed press before unhooking");
        };
        input.run();
    }
    for (const bool quickRelease : {false, true}) {
        NativeInput input;
        input.configuration.bindings.clear();
        input.scenario = [&] {
            require(input.mouse == nullptr, "unset bindings must not require a mouse hook");
            input.down[VK_LBUTTON] = true;
            if (quickRelease) {
                input.afterMouseInstall = [&] { input.down[VK_LBUTTON] = false; };
            }
            input.backend->beginButtonDrag(settings::SettingsGlobalMouseAction::ScreenshotCopy);
            if (!quickRelease) {
                require(input.mouse != nullptr && input.button(true) == 0,
                        "direct drags must temporarily monitor and forward the Qt release");
            }
            require(input.events.size() == 2 &&
                        input.events.back().kind == GlobalMouseDragEvent::Kind::Finish &&
                        input.events.back().position == QPoint(20, 30) && input.mouse == nullptr,
                    "direct drags must finish exactly and disarm even on a quick release");
        };
        input.run();
    }
    {
        NativeInput input;
        input.configuration.bindings.clear();
        input.scenario = [&] {
            input.down[VK_LBUTTON] = true;
            input.backend->beginButtonDrag(settings::SettingsGlobalMouseAction::ScreenshotCopy);
            require(input.mouse != nullptr &&
                        input.events.back().kind == GlobalMouseDragEvent::Kind::Begin,
                    "restart must reset the previous active gesture");
        };
        input.run(2);
        require(input.events.size() == 2 && input.events[1].id > input.events[0].id,
                "restarted hooks must not reuse a previous gesture identity");
    }
}
