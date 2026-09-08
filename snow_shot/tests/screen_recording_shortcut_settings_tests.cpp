#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated recording shortcut storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create recording shortcut executable directory");
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success,
            "initialize recording shortcut storage");
    const storage::ScreenRecordingShortcutSettings recording;
    const auto defaults = recording.allShortcuts();
    const QString exportId = QStringLiteral("export");
    const QStringList custom = {QStringLiteral("F12"), QStringLiteral("Ctrl+F12")};
    {
        snow_shot::presentation::GlobalShortcutManager globalShortcuts;
        settings::BuiltInSettingsBackend backend(globalShortcuts);
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        const auto scope = settings::SettingsLocalShortcutScope::ScreenRecording;
        for (auto it = defaults.cbegin(); it != defaults.cend(); ++it) {
            require(
                session.localShortcuts(scope, it.key()) == it.value() &&
                    backend.validateLocalShortcut(scope, it.key(), it.value().first()).supported,
                "settings must expose and accept all defaults, including Ctrl+C and Esc");
        }
        require(session.applyLocalShortcuts(scope, exportId, custom) &&
                    recording.shortcuts(exportId) == custom &&
                    session.localShortcuts(scope, exportId) == custom,
                "settings edits must update storage and runtime state");
        const auto duplicate =
            backend.validateLocalShortcut(scope, exportId, QStringLiteral("Ctrl+S"));
        require(!duplicate.supported &&
                    duplicate.failureReason ==
                        snow_shot::presentation::GlobalShortcutFailureReason::AlreadyInUse &&
                    !session.applyLocalShortcuts(scope, exportId, {QStringLiteral("Ctrl+S")}) &&
                    recording.shortcuts(exportId) == custom,
                "duplicate recording keys must be rejected without changing saved settings");
        require(
            !backend.validateLocalShortcut(scope, exportId, QStringLiteral("Ctrl+K, Ctrl+C"))
                    .supported &&
                !backend
                     .validateLocalShortcut(scope, QStringLiteral("unknown"), QStringLiteral("F12"))
                     .supported &&
                !recording.setShortcuts(QStringLiteral("unknown"), custom),
            "unknown actions and multi-step sequences must be rejected");
        auto conflicting = recording.allShortcuts();
        conflicting.insert(QStringLiteral("end_recording"), custom);
        require(!recording.setAllShortcutsAtomic(conflicting) &&
                    recording.shortcuts(exportId) == custom &&
                    recording.shortcuts(QStringLiteral("end_recording")) ==
                        defaults.value(QStringLiteral("end_recording")),
                "adapter must reject duplicate keys atomically");
        conflicting.remove(exportId);
        require(!recording.setAllShortcutsAtomic(conflicting),
                "atomic updates require every action");
        require(session.applyLocalShortcuts(scope, QStringLiteral("end_recording"), {}) &&
                    recording.shortcuts(QStringLiteral("end_recording")).isEmpty(),
                "settings must support disabling shortcuts");
        const auto screenshotBefore = storage::ScreenshotShortcutSettings().allShortcuts();
        const auto drawingBefore = storage::DrawingShortcutSettings().allShortcuts();
        require(session.reset(settings::SettingsSectionReset::ScreenRecordingShortcuts) &&
                    recording.allShortcuts() == defaults &&
                    session.localShortcuts(scope, exportId) == defaults.value(exportId) &&
                    storage::ScreenshotShortcutSettings().allShortcuts() == screenshotBefore &&
                    storage::DrawingShortcutSettings().allShortcuts() == drawingBefore,
                "recording reset must restore defaults without changing other shortcut categories");
        require(session.applyLocalShortcuts(scope, exportId, custom) &&
                    session.applyLocalShortcuts(scope, QStringLiteral("end_recording"), {}),
                "prepare custom and empty bindings for persistence verification");
    }
    require(applicationStorage.flushNow().success, "save recording shortcut configuration");
    applicationStorage.shutdown();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success &&
                recording.shortcuts(exportId) == custom &&
                recording.shortcuts(QStringLiteral("end_recording")).isEmpty() &&
                recording.shortcuts(QStringLiteral("toggle_recording")) ==
                    defaults.value(QStringLiteral("toggle_recording")),
            "custom, disabled, and default bindings must survive restart");
    applicationStorage.shutdown();
    return 0;
}
