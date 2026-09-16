#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QFile>
#include <QJsonDocument>
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

void resizeModeDefaultsPersistsAndResets(const QString& configurationPath) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    constexpr auto binding = settings::SettingsSelectBinding::ScreenshotSelectionResizeMode;
    require(backend.selectValue(binding) == QStringLiteral("follow_mouse_movement"),
            "selection resize mode must default to following mouse movement");
    require(backend.applySelectValue(binding, QStringLiteral("follow_mouse_position")) &&
                backend.selectValue(binding) == QStringLiteral("follow_mouse_position") &&
                storage::ScreenshotSettings().selectionResizeMode() ==
                    QStringLiteral("follow_mouse_position"),
            "the settings backend must apply and read the follow-position resize mode");
    require(!backend.applySelectValue(binding, QStringLiteral("unknown")) &&
                backend.selectValue(binding) == QStringLiteral("follow_mouse_position"),
            "invalid resize mode choices must preserve the accepted value");
    require(storage::ApplicationStorage::instance().configuration().flushNow().success,
            "selection resize mode must be flushable");
    storage::ConfigurationStore reloaded(configurationPath, true, true, 60000);
    require(reloaded.value(QStringLiteral("screenshot/selection_resize_mode")) ==
                QStringLiteral("follow_mouse_position"),
            "selection resize mode must survive a configuration reload");
    require(backend.resetSection(settings::SettingsSectionReset::ScreenshotSettings) &&
                backend.selectValue(binding) == QStringLiteral("follow_mouse_movement"),
            "resetting the Screenshot function settings must restore following mouse movement");
    const auto invalid = storage::ConfigurationSchema::normalize(
        QStringLiteral("screenshot/selection_resize_mode"), QStringLiteral("unknown"));
    require(!invalid.valid, "the schema must reject unsupported selection resize modes");

    const QString invalidPath = configurationPath + QStringLiteral(".invalid");
    QFile invalidFile(invalidPath);
    require(invalidFile.open(QIODevice::WriteOnly), "failed to create invalid configuration");
    const QByteArray invalidDocument =
        QJsonDocument(QJsonObject{{QStringLiteral("screenshot"),
                                   QJsonObject{{QStringLiteral("selection_resize_mode"),
                                                QStringLiteral("unknown")}}}})
            .toJson();
    require(invalidFile.write(invalidDocument) == invalidDocument.size(),
            "failed to write invalid configuration");
    invalidFile.close();
    storage::ConfigurationStore repaired(invalidPath, true, true, 60000);
    require(repaired.value(QStringLiteral("screenshot/selection_resize_mode")) ==
                QStringLiteral("follow_mouse_movement"),
            "invalid stored resize modes must fall back to following mouse movement");
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
    resizeModeDefaultsPersistsAndResets(temporary.filePath(QStringLiteral("data/config.json")));
    applicationStorage.shutdown();
    return 0;
}
