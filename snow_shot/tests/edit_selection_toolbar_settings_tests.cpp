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

void editSelectionToolbarDefaultsPersistsAndResets(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding = settings::SettingsSwitchBinding::ShowEditSelectionToolbar;
    const QString key = QStringLiteral("screenshot_ui/show_edit_selection_toolbar");
    const storage::ScreenshotUiSettings screenshot;
    require(backend.switchEnabled(binding) && backend.switchValue(binding) &&
                screenshot.showEditSelectionToolbar() &&
                storage::ConfigurationSchema::defaultValue(key).toBool(),
            "the edit selection toolbar must default to shown");
    require(backend.applySwitchValue(binding, false) && !backend.switchValue(binding) &&
                !screenshot.showEditSelectionToolbar(),
            "the settings backend must apply and read the disabled toolbar preference");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "the toolbar preference must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(!reloaded.value(key).toBool(true),
            "the disabled toolbar preference must survive a configuration reload");
    require(!storage::ConfigurationSchema::normalize(key, QStringLiteral("invalid")).valid,
            "the toolbar preference must reject non-boolean values");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotInterfaceSettings) &&
                backend.switchValue(binding) && screenshot.showEditSelectionToolbar(),
            "resetting screenshot interface settings must restore the shown toolbar");
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
    editSelectionToolbarDefaultsPersistsAndResets(
        temporary.filePath(QStringLiteral("data/config.json")));
    applicationStorage.shutdown();
    return 0;
}
