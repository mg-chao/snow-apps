#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/shortcuts/shortcutbinding.h"

#include <QApplication>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;
namespace shortcuts = snow_shot::shortcuts;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void snapShortcutPersistsValidatesAndResets(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager manager;
    settings::BuiltInSettingsBackend backend(manager);
    constexpr auto scope = settings::SettingsLocalShortcutScope::Screenshot;
    const QString id = QStringLiteral("selection_aspect_ratio_snap");
    const QString key = QStringLiteral("screenshot_shortcuts/") + id;
    const auto defaults = backend.localShortcuts(scope, id);
    require(defaults.size() == 1 && defaults.constFirst().portableText == QStringLiteral("Q"),
            "the settings backend must expose Q as the default snap shortcut");
    const shortcuts::ShortcutBindingList custom{QStringLiteral("G"), QStringLiteral("Ctrl+Alt+G")};
    require(backend.applyLocalShortcuts(scope, id, custom) &&
                backend.localShortcuts(scope, id) == custom,
            "the settings backend must accept two custom snap shortcuts");
    require(!backend.validateLocalShortcut(scope, id, {QStringLiteral("C")}).supported &&
                !backend.applyLocalShortcuts(scope, id, {QStringLiteral("C")}) &&
                backend.localShortcuts(scope, id) == custom,
            "a conflicting copy-color shortcut must be rejected without changing snap settings");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "custom snap shortcuts must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(shortcuts::shortcutBindingsFromJson(reloaded.value(key), true) == custom,
            "custom snap shortcuts must survive configuration reload");
    require(backend.applyLocalShortcuts(scope, id, {}) &&
                backend.localShortcuts(scope, id).isEmpty(),
            "clearing the snap shortcut must disable it");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotEditorShortcuts) &&
                backend.localShortcuts(scope, id) == defaults,
            "resetting screenshot editor shortcuts must restore Q");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create isolated settings directory");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "failed to initialize isolated snap settings");
    snapShortcutPersistsValidatesAndResets(temporary.filePath(QStringLiteral("data/config.json")));
    appStorage.shutdown();
    return 0;
}
