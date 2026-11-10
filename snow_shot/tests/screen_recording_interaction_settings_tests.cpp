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
    storage::RecordingSettings recording;
    const QString followKey = QStringLiteral("screen_recording/follow_mouse");
    require(!recording.followMouse() &&
                !storage::ConfigurationSchema::defaultValue(followKey).toBool(),
            "follow mouse must default to disabled");
    require(!applicationStorage.configuration().setValue(followKey, QStringLiteral("true")) &&
                recording.setFollowMouse(true) && recording.followMouse(),
            "follow mouse accepts and persists only boolean values");
    const QStringList keyboardKeys{QStringLiteral("screen_recording/record_number_keys"),
                                   QStringLiteral("screen_recording/record_letters_and_symbols"),
                                   QStringLiteral("screen_recording/record_control_keys"),
                                   QStringLiteral("screen_recording/preserve_keyboard_shortcuts")};
    for (const auto& keyboardKey : keyboardKeys) {
        require(storage::ConfigurationSchema::defaultValue(keyboardKey).toBool() &&
                    applicationStorage.configuration().value(keyboardKey).toBool(),
                "keyboard categories and shortcut preservation must default to enabled");
        require(!applicationStorage.configuration().setValue(keyboardKey, QStringLiteral("false")),
                "keyboard filters reject non-boolean settings");
    }
    require(recording.setRecordNumberKeys(false) && recording.setRecordLettersAndSymbols(false) &&
                recording.setRecordControlKeys(false) &&
                recording.setPreserveKeyboardShortcuts(false),
            "keyboard category preferences must persist");
    require(!recording.webcamEnabled() && recording.webcamSize() == 150 &&
                recording.webcamShape() == QStringLiteral("square") &&
                recording.webcamOffsetLeft() == 48 && recording.webcamOffsetBottom() == 48 &&
                recording.webcamOrientation() == 0,
            "webcam defaults must be disabled and a 150 px square");
    require(recording.setWebcamEnabled(true) && recording.setWebcamLayout(500, QPoint(320, 240)) &&
                recording.setWebcamShape(QStringLiteral("circle")) &&
                recording.setWebcamOrientation(7),
            "webcam settings must persist");
    require(!recording.setWebcamSize(49) && !recording.setWebcamSize(501) &&
                !recording.setWebcamLayout(200, QPoint(-1, 0)) &&
                !recording.setWebcamLayout(200, QPoint(0, 32769)) &&
                !recording.setWebcamShape(QStringLiteral("oval")) &&
                !recording.setWebcamOrientation(-1) && !recording.setWebcamOrientation(8),
            "webcam settings reject invalid bounds and shapes");
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto promptsBinding =
        settings::SettingsSwitchBinding::ScreenRecordingShowStatusPrompts;
    const QString promptsKey = QStringLiteral("screen_recording/show_status_prompts");
    require(backend.switchEnabled(promptsBinding) && backend.switchValue(promptsBinding) &&
                recording.showStatusPrompts() &&
                storage::ConfigurationSchema::defaultValue(promptsKey).toBool(),
            "recording status prompts must default to enabled");
    require(backend.applySwitchValue(promptsBinding, false) &&
                !backend.switchValue(promptsBinding) && !recording.showStatusPrompts(),
            "recording status prompts must be disabled through the shared settings backend");
    require(!applicationStorage.configuration().setValue(promptsKey, QStringLiteral("false")) &&
                !recording.showStatusPrompts(),
            "recording status prompts must reject non-boolean preferences");
    constexpr auto autoHideBinding =
        settings::SettingsSwitchBinding::ScreenRecordingAutoHideToolbar;
    const QString autoHideKey = QStringLiteral("screen_recording/auto_hide_toolbar");
    require(backend.switchEnabled(autoHideBinding) && !backend.switchValue(autoHideBinding) &&
                !storage::RecordingSettings().autoHideToolbar() &&
                !storage::ConfigurationSchema::defaultValue(autoHideKey).toBool(),
            "recording toolbar auto-hide must default to false");
    require(backend.applySwitchValue(autoHideBinding, true) &&
                backend.switchValue(autoHideBinding) &&
                storage::RecordingSettings().autoHideToolbar(),
            "toolbar auto-hide must apply through the shared settings backend");
    require(!applicationStorage.configuration().setValue(autoHideKey, QStringLiteral("true")) &&
                storage::RecordingSettings().autoHideToolbar(),
            "toolbar auto-hide must reject non-boolean configuration values");
    constexpr auto binding =
        settings::SettingsSwitchBinding::ScreenRecordingAutoExitAfterRecordingEnds;
    const QString key = QStringLiteral("screen_recording/auto_exit_after_recording_ends");
    constexpr auto notifyBinding =
        settings::SettingsSwitchBinding::ScreenRecordingNotifyAfterExportCompletes;
    const QString notifyKey = QStringLiteral("screen_recording/notify_after_export_completes");
    constexpr auto deleteBinding =
        settings::SettingsSwitchBinding::ScreenRecordingDeleteFilesOnRecordingClose;
    const QString deleteKey = QStringLiteral("screen_recording/delete_files_on_recording_close");
    require(backend.switchEnabled(deleteBinding) && !backend.switchValue(deleteBinding) &&
                !storage::RecordingSettings().deleteFilesOnRecordingClose() &&
                !storage::ConfigurationSchema::defaultValue(deleteKey).toBool(),
            "deleting files on recording close must default to false");
    require(backend.applySwitchValue(deleteBinding, true) && backend.switchValue(deleteBinding) &&
                storage::RecordingSettings().deleteFilesOnRecordingClose(),
            "the delete-on-close setting must apply through the shared settings backend");
    require(backend.switchEnabled(notifyBinding) && !backend.switchValue(notifyBinding) &&
                !storage::RecordingSettings().notifyAfterExportCompletes() &&
                !storage::ConfigurationSchema::defaultValue(notifyKey).toBool(),
            "export completion notifications must default to false");
    require(backend.applySwitchValue(notifyBinding, true) && backend.switchValue(notifyBinding) &&
                storage::RecordingSettings().notifyAfterExportCompletes(),
            "the notification setting must apply through the shared settings backend");
    require(backend.switchEnabled(binding) && !backend.switchValue(binding) &&
                !storage::RecordingSettings().autoExitAfterRecordingEnds() &&
                !storage::ConfigurationSchema::defaultValue(key).toBool(),
            "auto exit after recording must default to false");
    require(backend.applySwitchValue(binding, true) && backend.switchValue(binding) &&
                storage::RecordingSettings().autoExitAfterRecordingEnds(),
            "the settings switch must apply and read the recording auto-exit preference");
    require(storage::RecordingSettings().setMouseHighlightRadius(96),
            "a custom highlight radius must be accepted");
    require(applicationStorage.configuration().flushNow().success,
            "recording auto-exit preference must be flushable");
    storage::ConfigurationStore reloaded(temporary.filePath(QStringLiteral("data/config.json")),
                                         true, true, 60000);
    require(reloaded.value(followKey).toBool(), "follow mouse survives configuration reload");
    require(reloaded.value(QStringLiteral("screen_recording/enable_webcam")).toBool() &&
                reloaded.value(QStringLiteral("screen_recording/webcam_size")).toInt() == 500 &&
                reloaded.value(QStringLiteral("screen_recording/webcam_shape")).toString() ==
                    QStringLiteral("circle") &&
                reloaded.value(QStringLiteral("screen_recording/webcam_offset_left")).toInt() ==
                    320 &&
                reloaded.value(QStringLiteral("screen_recording/webcam_offset_bottom")).toInt() ==
                    240 &&
                reloaded.value(QStringLiteral("screen_recording/webcam_orientation")).toInt() == 7,
            "webcam values survive a configuration reload");
    require(reloaded.value(key).toBool(), "recording auto-exit must survive configuration reload");
    for (const auto& keyboardKey : keyboardKeys) {
        require(!reloaded.value(keyboardKey).toBool(),
                "unchecked keyboard options must survive configuration reload");
    }
    require(reloaded.value(autoHideKey).toBool(),
            "recording toolbar auto-hide must survive configuration reload");
    require(!reloaded.value(promptsKey).toBool(),
            "disabled recording status prompts must survive configuration reload");
    require(reloaded.value(notifyKey).toBool(),
            "export notifications must survive configuration reload");
    require(reloaded.value(deleteKey).toBool(),
            "deleting files on recording close must survive configuration reload");
    require(reloaded.value(QStringLiteral("screen_recording/mouse_highlight_radius")).toInt() == 96,
            "custom highlight radius must survive configuration reload");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenRecordingActionToolbar) &&
                storage::RecordingSettings().mouseHighlightRadius() == 96 &&
                backend.switchValue(binding) && backend.switchValue(notifyBinding) &&
                backend.switchValue(deleteBinding) && backend.switchValue(autoHideBinding),
            "Action Toolbar reset must preserve the Interaction preference");
    require(recording.recordNumberKeys() && recording.recordLettersAndSymbols() &&
                recording.recordControlKeys() && recording.preserveKeyboardShortcuts(),
            "Action Toolbar reset must restore all keyboard options to checked");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenRecordingCapture) &&
                backend.switchValue(autoHideBinding) && !backend.switchValue(promptsBinding),
            "recording capture reset must preserve toolbar auto-hide");
    require(storage::RecordingSettings().setCaptureToolbarInRecording(false) &&
                backend.resetSection(settings::SettingsSectionReset::ScreenRecordingInteraction) &&
                !backend.switchValue(binding) &&
                !storage::RecordingSettings().autoExitAfterRecordingEnds() &&
                !backend.switchValue(notifyBinding) &&
                !storage::RecordingSettings().notifyAfterExportCompletes() &&
                !backend.switchValue(deleteBinding) &&
                !storage::RecordingSettings().deleteFilesOnRecordingClose() &&
                !backend.switchValue(autoHideBinding) &&
                !storage::RecordingSettings().autoHideToolbar() &&
                backend.switchValue(promptsBinding) && recording.showStatusPrompts() &&
                storage::RecordingSettings().mouseHighlightRadius() == 96 &&
                !storage::RecordingSettings().captureToolbarInRecording(),
            "Interaction reset must restore its behavior defaults and preserve capture settings");
    require(storage::RecordingSettings().setAutoHideToolbar(true) &&
                backend.switchValue(autoHideBinding) &&
                storage::RecordingSettings().setAutoHideToolbar(false) &&
                !backend.switchValue(autoHideBinding),
            "external toolbar auto-hide writes must be reflected by the settings backend");
    require(recording.setRecordNumberKeys(false) && recording.setRecordLettersAndSymbols(false) &&
                recording.setRecordControlKeys(false) &&
                recording.setPreserveKeyboardShortcuts(false),
            "prepare unchecked filters before resetting recording settings");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenRecording) &&
                recording.recordNumberKeys() && recording.recordLettersAndSymbols() &&
                recording.recordControlKeys() && recording.preserveKeyboardShortcuts() &&
                storage::RecordingSettings().mouseHighlightRadius() == 41,
            "recording settings reset must restore the original highlight radius");
    require(!recording.followMouse(), "recording reset restores follow mouse to disabled");
    require(!recording.webcamEnabled() && recording.webcamSize() == 150 &&
                recording.webcamShape() == QStringLiteral("square") &&
                recording.webcamOffsetLeft() == 48 && recording.webcamOffsetBottom() == 48 &&
                recording.webcamOrientation() == 0,
            "recording reset restores webcam defaults");
    applicationStorage.shutdown();
    return 0;
}
