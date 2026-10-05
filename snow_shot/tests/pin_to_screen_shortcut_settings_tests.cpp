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
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    QTemporaryDir temporary;
    require(temporary.isValid(), "create pinned shortcut settings directory");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success,
            "initialize pinned shortcut settings storage");
    const storage::PinToScreenShortcutSettings pinned;
    const auto defaults = pinned.allShortcuts();
    require(pinned.shortcuts(QStringLiteral("toggle_lock")) ==
                snow_shot::shortcuts::ShortcutBindingList{QStringLiteral("L")},
            "lock must default to L");
    const auto screenshotBefore = storage::ScreenshotShortcutSettings().allShortcuts();
    const auto drawingBefore = storage::DrawingShortcutSettings().allShortcuts();
    require(defaults.value(QStringLiteral("print")) ==
                    snow_shot::shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+P")} &&
                screenshotBefore.value(QStringLiteral("print")) ==
                    defaults.value(QStringLiteral("print")),
            "screenshot and pin print shortcuts must default to Ctrl+P");
    {
        snow_shot::presentation::GlobalShortcutManager globalShortcuts;
        settings::BuiltInSettingsBackend backend(globalShortcuts);
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        constexpr auto scope = settings::SettingsLocalShortcutScope::PinToScreen;
        for (const QString& id : {QStringLiteral("always_on_top"), QStringLiteral("show_border"),
                                  QStringLiteral("toggle_lock"), QStringLiteral("print")}) {
            require(session.localShortcuts(scope, id) == defaults.value(id),
                    "settings must expose default window management shortcuts");
            require(
                session.applyLocalShortcuts(scope, id, {QStringLiteral("Ctrl+Alt+F12")}) &&
                    pinned.shortcuts(id) ==
                        snow_shot::shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+Alt+F12")},
                "settings must accept custom window management shortcuts");
            const QString other = id == QStringLiteral("always_on_top")
                                      ? QStringLiteral("show_border")
                                      : QStringLiteral("always_on_top");
            require(!session.applyLocalShortcuts(scope, other, {QStringLiteral("Ctrl+Alt+F12")}) &&
                        pinned.shortcuts(other) == defaults.value(other),
                    "window management shortcuts must reject conflicting keys");
            require(session.applyLocalShortcuts(scope, id, {}) && pinned.shortcuts(id).isEmpty(),
                    "settings must support disabling window management shortcuts");
            require(
                session.reset(settings::SettingsSectionReset::PinToScreenShortcuts) &&
                    pinned.allShortcuts() == defaults &&
                    session.localShortcuts(scope, id) == defaults.value(id) &&
                    storage::ScreenshotShortcutSettings().allShortcuts() == screenshotBefore &&
                    storage::DrawingShortcutSettings().allShortcuts() == drawingBefore,
                "pinned shortcut reset must restore the complete map and preserve other scopes");
        }
        constexpr auto lockedColor = settings::SettingsColorBinding::PinLockedBorderColor;
        require(session.colorValue(lockedColor) == QColor(250, 173, 20) &&
                    session.applyColorValue(lockedColor, QColor(170, 90, 20, 128)) &&
                    storage::PinToScreenSettings().lockedBorderColor() ==
                        QColor(170, 90, 20, 128) &&
                    session.reset(settings::SettingsSectionReset::PinToScreen) &&
                    session.colorValue(lockedColor) == QColor(250, 173, 20) &&
                    !session.applyColorValue(lockedColor, QColor()),
                "locked color must support custom alpha, reset, and invalid-color rejection");
        require(session.applyColorValue(lockedColor, QColor(160, 100, 30)), "save locked color");
        require(session.applyLocalShortcuts(scope, QStringLiteral("always_on_top"),
                                            {QStringLiteral("Ctrl+Alt+T")}) &&
                    session.applyLocalShortcuts(scope, QStringLiteral("show_border"), {}),
                "prepare custom and disabled window management shortcuts");
    }
    require(applicationStorage.flushNow().success, "save pinned shortcut configuration");
    applicationStorage.shutdown();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success &&
                pinned.shortcuts(QStringLiteral("always_on_top")) ==
                    snow_shot::shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+Alt+T")} &&
                pinned.shortcuts(QStringLiteral("show_border")).isEmpty() &&
                storage::PinToScreenSettings().lockedBorderColor() == QColor(160, 100, 30),
            "custom and disabled window management shortcuts must survive restart");
    applicationStorage.shutdown();
    return 0;
}
