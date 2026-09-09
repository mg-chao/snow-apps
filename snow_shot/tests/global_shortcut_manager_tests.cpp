#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QCoreApplication>
#include <QKeySequence>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
using namespace snow_shot::presentation;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void functionKeysAreValidatedIndependentlyOfSystemAvailability() {
    GlobalShortcutManager manager;
    require(
        manager.validateShortcut(QStringLiteral("Meta+Shift+F12")).supported,
        "Windows+Shift+F12 must reach native registration instead of being rejected as invalid");
    for (int functionKey = 1; functionKey <= 24; ++functionKey) {
        for (int mask = 0; mask < 16; ++mask) {
            Qt::KeyboardModifiers modifiers;
            if ((mask & 1) != 0) {
                modifiers |= Qt::ControlModifier;
            }
            if ((mask & 2) != 0) {
                modifiers |= Qt::AltModifier;
            }
            if ((mask & 4) != 0) {
                modifiers |= Qt::ShiftModifier;
            }
            if ((mask & 8) != 0) {
                modifiers |= Qt::MetaModifier;
            }
            const auto key = static_cast<Qt::Key>(Qt::Key_F1 + functionKey - 1);
            const QString shortcut =
                QKeySequence(QKeyCombination(modifiers, key)).toString(QKeySequence::PortableText);
            const auto result = manager.validateShortcut(shortcut);
            require(
                result.supported && result.failureReason == GlobalShortcutFailureReason::None &&
                    result.shortcut == shortcut,
                "all native function keys and modifier combinations must pass syntax validation");
        }
    }
    for (const QString& shortcut : {QStringLiteral("F25"), QStringLiteral("Ctrl"),
                                    QStringLiteral("Ctrl+K, Ctrl+C"), QStringLiteral("Num+F12")}) {
        const auto result = manager.validateShortcut(shortcut);
        require(!result.supported &&
                    result.failureReason == GlobalShortcutFailureReason::InvalidShortcut,
                "unmappable keys, modifier-only input and chords must remain invalid");
    }
}

void nativeRegistrationMatchesWindowsAvailability() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "test storage directory must be available");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    static_cast<void>(
        storage.initialize({temporary.filePath(QStringLiteral("bin")), temporary.path()}));
    require(storage.isInitialized(), "test storage must initialize");
    {
        GlobalShortcutManager manager;
        for (const UINT modifiers : {MOD_WIN | MOD_SHIFT, MOD_ALT, MOD_CONTROL,
                                     MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN}) {
            constexpr int probeId = 0x6A01;
            SetLastError(ERROR_SUCCESS);
            const bool available =
                RegisterHotKey(nullptr, probeId, modifiers | MOD_NOREPEAT, VK_F12) != FALSE;
            const DWORD error = GetLastError();
            if (available) {
                require(UnregisterHotKey(nullptr, probeId) != FALSE,
                        "probe must release its hotkey");
            }
            Qt::KeyboardModifiers qtModifiers;
            if ((modifiers & MOD_WIN) != 0) {
                qtModifiers |= Qt::MetaModifier;
            }
            if ((modifiers & MOD_SHIFT) != 0) {
                qtModifiers |= Qt::ShiftModifier;
            }
            if ((modifiers & MOD_CONTROL) != 0) {
                qtModifiers |= Qt::ControlModifier;
            }
            if ((modifiers & MOD_ALT) != 0) {
                qtModifiers |= Qt::AltModifier;
            }
            const QString shortcut = QKeySequence(QKeyCombination(qtModifiers, Qt::Key_F12))
                                         .toString(QKeySequence::PortableText);
            manager.setShortcuts(GlobalShortcutAction::Screenshot, {shortcut});
            const auto state = manager.state(GlobalShortcutAction::Screenshot);
            require(state.bindings.size() == 1, "the requested shortcut must have one result");
            const auto& binding = state.bindings.front();
            std::cout << shortcut.toStdString() << ": registered=" << binding.registered
                      << " nativeError=" << binding.nativeErrorCode << '\n';
            require(binding.registered == available && binding.nativeErrorCode == error,
                    "registration must agree with the direct Windows API probe");
            const auto expectedReason = available ? GlobalShortcutFailureReason::None
                                        : error == ERROR_HOTKEY_ALREADY_REGISTERED
                                            ? GlobalShortcutFailureReason::AlreadyInUse
                                            : GlobalShortcutFailureReason::SystemError;
            require(binding.failureReason == expectedReason,
                    "native conflicts must not be misreported as invalid syntax");
            manager.setShortcuts(GlobalShortcutAction::Screenshot, {});
        }
    }
    storage.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    functionKeysAreValidatedIndependentlyOfSystemAvailability();
    if (app.arguments().contains(QStringLiteral("--native-registration-smoke"))) {
        nativeRegistrationMatchesWindowsAvailability();
    }
    return 0;
}
