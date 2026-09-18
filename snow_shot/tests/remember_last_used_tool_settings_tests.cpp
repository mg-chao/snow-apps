#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void rememberLastUsedToolDefaultsPersistsAndResets(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding = settings::SettingsSwitchBinding::DrawingRememberLastUsedTool;
    require(backend.switchEnabled(binding) && !backend.switchValue(binding) &&
                !storage::DrawingSettings().rememberLastUsedTool() &&
                !storage::ConfigurationSchema::defaultValue(
                    QStringLiteral("drawing/remember_last_used_tool"))
                     .toBool(),
            "remember last used tool must default to disabled");
    require(backend.applySwitchValue(binding, true) && backend.switchValue(binding) &&
                storage::DrawingSettings().rememberLastUsedTool(),
            "the settings backend must apply and read the remembered tool preference");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "the remembered tool preference must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(reloaded.value(QStringLiteral("drawing/remember_last_used_tool")).toBool(),
            "the remembered tool preference must survive a configuration reload");
    require(storage::ScreenshotToolbarSettings().lastDrawingTool().isEmpty(),
            "no drawing tool is remembered before any tool activation");
    require(storage::ScreenshotToolbarSettings().setLastDrawingTool(QStringLiteral("shape")) &&
                storage::ScreenshotToolbarSettings().lastDrawingTool() ==
                    QStringLiteral("shape"),
            "the remembered drawing tool must round-trip through the toolbar settings");
    require(backend.resetSection(settings::SettingsSectionReset::DrawingQuickSelection) &&
                !backend.switchValue(binding) &&
                !storage::DrawingSettings().rememberLastUsedTool(),
            "resetting the Drawing function settings must restore the default disabled switch");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create isolated settings directory");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    static_cast<void>(
        applicationStorage.initialize({temporary.filePath(QStringLiteral("bin")),
                                       temporary.filePath(QStringLiteral("data")), 60000}));
    rememberLastUsedToolDefaultsPersistsAndResets(
        temporary.filePath(QStringLiteral("data/config.json")));
    applicationStorage.shutdown();
    return 0;
}
