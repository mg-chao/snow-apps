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
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated settings directory must exist");
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "isolated storage must initialize");
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding =
        settings::SettingsSwitchBinding::ScreenRecordingAutoExitAfterRecordingEnds;
    const QString key = QStringLiteral("screen_recording/auto_exit_after_recording_ends");
    require(backend.switchEnabled(binding) && !backend.switchValue(binding) &&
                !storage::RecordingSettings().autoExitAfterRecordingEnds() &&
                !storage::ConfigurationSchema::defaultValue(key).toBool(),
            "auto exit after recording must default to false");
    require(backend.applySwitchValue(binding, true) && backend.switchValue(binding) &&
                storage::RecordingSettings().autoExitAfterRecordingEnds(),
            "the settings switch must apply and read the recording auto-exit preference");
    require(applicationStorage.configuration().flushNow().success,
            "recording auto-exit preference must be flushable");
    storage::ConfigurationStore reloaded(temporary.filePath(QStringLiteral("data/config.json")),
                                         true, true, 60000);
    require(reloaded.value(key).toBool(), "recording auto-exit must survive configuration reload");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenRecordingActionToolbar) &&
                backend.switchValue(binding),
            "Action Toolbar reset must preserve the Interaction preference");
    require(storage::RecordingSettings().setCaptureToolbarInRecording(false) &&
                backend.resetSection(settings::SettingsSectionReset::ScreenRecordingInteraction) &&
                !backend.switchValue(binding) &&
                !storage::RecordingSettings().autoExitAfterRecordingEnds() &&
                !storage::RecordingSettings().captureToolbarInRecording(),
            "Interaction reset must restore auto-exit to false and preserve capture settings");
    applicationStorage.shutdown();
    return 0;
}
