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
    require(pinned.shortcuts(QStringLiteral("show_shadow")) ==
                snow_shot::shortcuts::ShortcutBindingList{QStringLiteral("Y")},
            "show shadow must default to Y");
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
                                  QStringLiteral("show_shadow"), QStringLiteral("toggle_lock"),
                                  QStringLiteral("print")}) {
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
        constexpr auto borderDefault = settings::SettingsSwitchBinding::PinShowBorderByDefault;
        constexpr auto shadowDefault = settings::SettingsSwitchBinding::PinShowShadowByDefault;
        require(session.switchValue(borderDefault) && !session.switchValue(shadowDefault) &&
                    session.applySwitchValue(borderDefault, false) &&
                    session.applySwitchValue(shadowDefault, true) &&
                    !storage::PinToScreenSettings().showBorderByDefault() &&
                    storage::PinToScreenSettings().showShadowByDefault(),
                "border and shadow creation defaults must be independently configurable");
        struct ShadowColorFixture {
            settings::SettingsColorBinding binding;
            QColor defaultColor;
        };
        const ShadowColorFixture shadowColors[] = {
            {settings::SettingsColorBinding::PinShadowColor, QColor(0xbf, 0xbf, 0xbf)},
            {settings::SettingsColorBinding::PinShadowActiveColor, QColor(105, 177, 255)},
            {settings::SettingsColorBinding::PinLockedShadowColor, QColor(250, 173, 20)},
        };
        for (const auto& fixture : shadowColors) {
            require(session.colorValue(fixture.binding) == fixture.defaultColor &&
                        session.applyColorValue(fixture.binding, QColor(170, 90, 20, 128)) &&
                        session.colorValue(fixture.binding) == QColor(170, 90, 20, 128) &&
                        !session.applyColorValue(fixture.binding, QColor()),
                    "each shadow state must expose editable alpha and reject invalid colors");
        }
        require(session.reset(settings::SettingsSectionReset::PinToScreen) &&
                    session.switchValue(borderDefault) && !session.switchValue(shadowDefault),
                "window interface reset must restore border and shadow creation defaults");
        for (const auto& fixture : shadowColors)
            require(session.colorValue(fixture.binding) == fixture.defaultColor,
                    "window interface reset must restore every shadow color");
        require(session.colorValue(lockedColor) == QColor(250, 173, 20) &&
                    session.applyColorValue(lockedColor, QColor(170, 90, 20, 128)) &&
                    storage::PinToScreenSettings().lockedBorderColor() ==
                        QColor(170, 90, 20, 128) &&
                    session.reset(settings::SettingsSectionReset::PinToScreen) &&
                    session.colorValue(lockedColor) == QColor(250, 173, 20) &&
                    !session.applyColorValue(lockedColor, QColor()),
                "locked color must support custom alpha, reset, and invalid-color rejection");
        require(session.applyColorValue(lockedColor, QColor(160, 100, 30)), "save locked color");
        require(session.applySwitchValue(borderDefault, false) &&
                    session.applySwitchValue(shadowDefault, true) &&
                    session.applyColorValue(settings::SettingsColorBinding::PinShadowColor,
                                            QColor(30, 40, 50, 60)) &&
                    session.applyColorValue(settings::SettingsColorBinding::PinShadowActiveColor,
                                            QColor(70, 80, 90, 100)) &&
                    session.applyColorValue(settings::SettingsColorBinding::PinLockedShadowColor,
                                            QColor(110, 120, 130, 140)),
                "save independent pinned appearance preferences");
        require(session.applyLocalShortcuts(scope, QStringLiteral("always_on_top"),
                                            {QStringLiteral("Ctrl+Alt+T")}) &&
                    session.applyLocalShortcuts(scope, QStringLiteral("show_shadow"),
                                                {QStringLiteral("Ctrl+Alt+Y")}) &&
                    session.applyLocalShortcuts(scope, QStringLiteral("show_border"), {}),
                "prepare custom and disabled window management shortcuts");
    }
    require(applicationStorage.flushNow().success, "save pinned shortcut configuration");
    applicationStorage.shutdown();
    require(applicationStorage.initialize({executable, temporary.path(), 60000}).success &&
                pinned.shortcuts(QStringLiteral("always_on_top")) ==
                    snow_shot::shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+Alt+T")} &&
                pinned.shortcuts(QStringLiteral("show_shadow")) ==
                    snow_shot::shortcuts::ShortcutBindingList{QStringLiteral("Ctrl+Alt+Y")} &&
                pinned.shortcuts(QStringLiteral("show_border")).isEmpty() &&
                storage::PinToScreenSettings().lockedBorderColor() == QColor(160, 100, 30) &&
                !storage::PinToScreenSettings().showBorderByDefault() &&
                storage::PinToScreenSettings().showShadowByDefault() &&
                storage::PinToScreenSettings().shadowColor() == QColor(30, 40, 50, 60) &&
                storage::PinToScreenSettings().shadowActiveColor() == QColor(70, 80, 90, 100) &&
                storage::PinToScreenSettings().lockedShadowColor() == QColor(110, 120, 130, 140),
            "custom and disabled window management shortcuts must survive restart");
    applicationStorage.shutdown();
    return 0;
}
