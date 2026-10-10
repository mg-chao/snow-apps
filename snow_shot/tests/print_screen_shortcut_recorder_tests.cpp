#include "snow_shot/platform/windows/printscreenshortcutrecorder.h"

#include <QApplication>
#include <QKeyEvent>
#include <QSemaphore>
#include <QThread>
#include <QWidget>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>

using snow_shot::platform::windows::PrintScreenHookApi;
using snow_shot::platform::windows::PrintScreenShortcutRecorder;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct KeyboardInput {
    static inline KeyboardInput* current = nullptr;
    HOOKPROC callback = nullptr;
    QObject* driver = nullptr;
    DWORD installThread = 0;
    DWORD removeThread = 0;
    int removals = 0;
    bool failInstallation = false;
    bool initiallyPressed = false;
    std::atomic<HWND> foreground{nullptr};
    std::atomic<int> modifiers{0};
    QSemaphore completed;

    KeyboardInput() {
        current = this;
    }

    static HHOOK WINAPI install(int type, HOOKPROC callback, HINSTANCE, DWORD thread) {
        require(type == WH_KEYBOARD_LL && thread == 0, "the recorder must observe global input");
        current->installThread = GetCurrentThreadId();
        current->callback = callback;
        if (current->failInstallation) {
            return nullptr;
        }
        current->driver = new QObject;
        return reinterpret_cast<HHOOK>(quintptr{1});
    }

    static BOOL WINAPI remove(HHOOK) {
        current->removeThread = GetCurrentThreadId();
        ++current->removals;
        delete current->driver;
        current->driver = nullptr;
        return TRUE;
    }

    static LRESULT WINAPI next(HHOOK, int, WPARAM, LPARAM) {
        return 17;
    }
    static HWND WINAPI foregroundWindow() {
        return current->foreground.load();
    }
    static SHORT WINAPI asyncKeyState(int key) {
        const auto modifiers = Qt::KeyboardModifiers(current->modifiers.load());
        const bool down = (key == VK_SNAPSHOT && current->initiallyPressed) ||
                          (key == VK_CONTROL && modifiers.testFlag(Qt::ControlModifier)) ||
                          (key == VK_SHIFT && modifiers.testFlag(Qt::ShiftModifier)) ||
                          (key == VK_MENU && modifiers.testFlag(Qt::AltModifier)) ||
                          (key == VK_RWIN && modifiers.testFlag(Qt::MetaModifier));
        return down ? static_cast<SHORT>(0x8000) : 0;
    }

    PrintScreenHookApi api() {
        return {install, remove, next, foregroundWindow, asyncKeyState, [] { return true; }};
    }

    LRESULT key(WPARAM message, DWORD key = VK_SNAPSHOT, DWORD flags = 0, DWORD time = 500,
                int code = HC_ACTION) const {
        KBDLLHOOKSTRUCT input{};
        input.vkCode = key;
        input.flags = flags;
        input.time = time;
        return callback(code, message, reinterpret_cast<LPARAM>(&input));
    }

    void run(std::function<void()> input) {
        QMetaObject::invokeMethod(
            driver,
            [this, input = std::move(input)] {
                input();
                completed.release();
            },
            Qt::QueuedConnection);
        // Deliberately do not pump the GUI. Input must finish on its own thread.
        require(completed.tryAcquire(1, 1000), "keyboard input must not wait for the GUI");
    }
};

void hookInputAndLifecycleRemainIndependentOfTheGui() {
    QWidget target;
    target.show();
    QApplication::processEvents();
    KeyboardInput input;
    input.foreground.store(reinterpret_cast<HWND>(target.winId()));
    QList<Qt::KeyboardModifiers> captures;
    const auto handler = [&](Qt::KeyboardModifiers modifiers) {
        require(QThread::currentThread() == qApp->thread(), "capture must be delivered on the GUI");
        captures.append(modifiers);
    };
    auto recorder = std::make_unique<PrintScreenShortcutRecorder>(target, handler, input.api());
    input.run([&] {
        require(input.key(WM_KEYDOWN, 'A', 0, 0, -1) == 17 && input.key(WM_KEYDOWN, 'A') == 17,
                "negative hook codes and ordinary keys must pass through");
        input.modifiers.store(Qt::ControlModifier | Qt::ShiftModifier);
        require(input.key(WM_KEYDOWN) == 1 && input.key(WM_KEYDOWN) == 1,
                "Print Screen presses and repeats must suppress existing hotkeys");
        input.modifiers.store(0);
        require(input.key(WM_KEYUP) == 1, "balanced releases must remain intercepted");
    });
    require(captures.isEmpty(), "the hook must only queue GUI work");
    require(input.installThread != GetCurrentThreadId(), "hook installation must leave the GUI");
    QKeyEvent olderModifier(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier);
    olderModifier.setTimestamp(499);
    require(recorder->handleKeyEvent(olderModifier), "older Qt input must not replace the chord");
    QApplication::processEvents();
    require(captures == QList<Qt::KeyboardModifiers>{Qt::ControlModifier | Qt::ShiftModifier},
            "repeats and early modifier release must preserve the press snapshot");

    input.run([&] {
        input.modifiers.store(Qt::MetaModifier);
        input.key(WM_KEYUP, VK_SNAPSHOT, LLKHF_ALTDOWN);
    });
    recorder->cancelPendingCapture();
    QApplication::processEvents();
    require(captures.size() == 1, "later GUI recording must cancel pending native captures");
    input.run([&] {
        input.modifiers.store(Qt::ControlModifier);
        input.key(WM_KEYUP);
        input.modifiers.store(Qt::MetaModifier | Qt::ShiftModifier);
        input.key(WM_SYSKEYUP, VK_SNAPSHOT, LLKHF_ALTDOWN);
    });
    QApplication::processEvents();
    require(captures.size() == 2 &&
                captures.last() == (Qt::MetaModifier | Qt::ShiftModifier | Qt::AltModifier),
            "release-only input must preserve Alt context and the latest capture must win");

    input.run([&] {
        input.key(WM_KEYUP);
        input.foreground.store(nullptr);
        require(input.key(WM_KEYUP) == 17, "background Print Screen input must pass through");
    });
    input.foreground.store(reinterpret_cast<HWND>(target.winId()));
    QApplication::processEvents();
    require(captures.size() == 2, "foreground loss must invalidate earlier queued captures");
    input.run([&] { input.key(WM_KEYUP); });
    target.hide();
    QApplication::processEvents();
    require(captures.size() == 2 && input.removals == 1 &&
                input.installThread == input.removeThread,
            "hiding must cancel delivery and unhook on the installing thread");
    target.show();
    QApplication::processEvents();
    input.run([&] { input.key(WM_KEYUP); });
    recorder.reset();
    QApplication::processEvents();
    require(captures.size() == 2 && input.removals == 2,
            "retiring the recorder must discard all queued GUI captures");

    input.initiallyPressed = true;
    recorder = std::make_unique<PrintScreenShortcutRecorder>(target, handler, input.api());
    input.run([&] { input.key(WM_KEYUP); });
    QApplication::processEvents();
    require(captures.size() == 2, "releasing a key held before recording must not create a chord");
    recorder.reset();

    auto destroyedTarget = std::make_unique<QWidget>();
    destroyedTarget->show();
    input.initiallyPressed = false;
    input.foreground.store(reinterpret_cast<HWND>(destroyedTarget->winId()));
    recorder =
        std::make_unique<PrintScreenShortcutRecorder>(*destroyedTarget, handler, input.api());
    input.run([&] { input.key(WM_KEYUP); });
    destroyedTarget.reset();
    QApplication::processEvents();
    require(captures.size() == 2 && input.removals == 4,
            "destroying the target must remove interception and pending captures");
    recorder.reset();

    input.failInstallation = true;
    input.foreground.store(reinterpret_cast<HWND>(target.winId()));
    recorder = std::make_unique<PrintScreenShortcutRecorder>(target, handler, input.api());
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Print, Qt::ControlModifier);
    recorder->handleKeyEvent(release);
    QApplication::processEvents();
    require(captures.size() == 3 && captures.last() == Qt::ControlModifier,
            "hook installation failure must retain normal Qt capture");
    recorder.reset();
    require(input.removals == 4, "failed installation must not unhook an invalid handle");

    input.failInstallation = false;
    recorder = std::make_unique<PrintScreenShortcutRecorder>(
        target,
        [&](Qt::KeyboardModifiers modifiers) {
            handler(modifiers);
            recorder.reset();
        },
        input.api());
    input.run([&] { input.key(WM_KEYUP); });
    QApplication::processEvents();
    require(!recorder && captures.size() == 4 && input.removals == 5,
            "a capture handler must be able to retire its recorder safely");
}

void reparentingMovesTheNativeCaptureScope() {
    auto previousOwner = std::make_unique<QWidget>();
    QWidget nextOwner;
    QWidget target(previousOwner.get());
    previousOwner->show();
    target.show();
    QApplication::processEvents();
    const auto previousHandle = reinterpret_cast<HWND>(previousOwner->winId());
    KeyboardInput input;
    input.foreground.store(previousHandle);
    int captures = 0;
    PrintScreenShortcutRecorder recorder(
        target, [&](Qt::KeyboardModifiers) { ++captures; }, input.api());
    input.run([&] { input.key(WM_KEYUP); });
    target.setParent(&nextOwner);
    const auto nextHandle = reinterpret_cast<HWND>(nextOwner.winId());
    input.foreground.store(nextHandle);
    nextOwner.show();
    target.show();
    previousOwner.reset();
    QApplication::processEvents();
    require(captures == 0 && input.removals == 1 && input.driver,
            "reparenting must cancel old captures and detach the old owner's teardown callback");
    input.run([&] {
        input.foreground.store(previousHandle);
        require(input.key(WM_KEYUP) == 17, "the former owner must no longer intercept input");
        input.foreground.store(nextHandle);
        require(input.key(WM_KEYUP) == 1, "the new owner must own Print Screen interception");
    });
    QApplication::processEvents();
    require(captures == 1, "the reparented recorder must deliver only the new session's capture");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    hookInputAndLifecycleRemainIndependentOfTheGui();
    reparentingMovesTheNativeCaptureScope();
    return 0;
}
