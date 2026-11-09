#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
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
void testSettings(const QString& path) {
    const auto& registry = settings::builtInSettingsRegistry();
    const auto binding = settings::SettingsSwitchBinding::DrawingSyncToolColors;
    const auto* field = registry.fieldForSwitch(binding);
    require(field && registry.validationErrors().isEmpty() && !field->defaultValue.toBool() &&
                field->configurationKey == QStringLiteral("drawing/sync_tool_colors"),
            "sync is a valid switch with a disabled default");
    bool found = false;
    for (const auto& page : registry.pages()) {
        for (const auto& section : page.sections) {
            for (qsizetype i = 1; i < section.items.size(); ++i) {
                if (section.items[i].id == QStringLiteral("drawing.stroke-color-presets"))
                    found = section.items[i - 1].id == field->id;
            }
        }
    }
    require(found, "sync is directly above Stroke color presets");
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const storage::DrawingSettings drawing;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const QString colorKey = QStringLiteral("drawing/shared_tool_color");
    const QString shapeKey = QStringLiteral("drawing/shape_style");
    const QString textKey = QStringLiteral("drawing/text_style");
    const QString arrowKey = QStringLiteral("drawing/arrow_style");
    require(!backend.switchValue(binding) && !drawing.syncToolColors() &&
                configuration.value(colorKey).toString().isEmpty(),
            "existing and new configurations begin with independent colors");
    const QString initial = QStringLiteral("#12345678");
    require(configuration.setValues(
                {{shapeKey,
                  QJsonObject{{"stroke", initial}, {"fill", "#ABCDEF00"}, {"stroke_width", 7}}},
                 {textKey, QJsonObject{{"color", "#AABBCCDD"}, {"font_size", 41}}},
                 {arrowKey,
                  QJsonObject{{"stroke", "#11223344"}, {"arrow_outline_color", "#ABCD1234"}}}}),
            "seed independent creation styles");
    const auto palettes = storage::ScreenshotColorPresetSettings().strokeColors();
    const auto revision = configuration.revision();
    require(backend.applySwitchValue(binding, true) && drawing.syncToolColors() &&
                configuration.value(colorKey).toString() == initial &&
                configuration.revision() == revision + 1,
            "first enable atomically seeds the shared RGBA from Shape stroke");
    require(
        !configuration.value(shapeKey).toObject().contains(QStringLiteral("stroke")) &&
            !configuration.value(shapeKey).toObject().contains(QStringLiteral("fill")) &&
            !configuration.value(textKey).toObject().contains(QStringLiteral("color")) &&
            configuration.value(shapeKey).toObject().value(QStringLiteral("stroke_width")) == 7 &&
            configuration.value(arrowKey).toObject().value(QStringLiteral("arrow_outline_color")) ==
                QStringLiteral("#ABCD1234"),
        "enabled styles store only nonshared appearance properties");
    require(configuration.flushNow().success, "flush shared preferences");
    storage::ConfigurationStore reloaded(path, true, true, 60000);
    require(reloaded.value(QStringLiteral("drawing/sync_tool_colors")).toBool() &&
                reloaded.value(colorKey).toString() == initial,
            "sync and alpha survive a storage reload");
    require(!configuration.setValue(colorKey, QStringLiteral("#123456")) &&
                !configuration.setValue(colorKey, 42) &&
                configuration.value(colorKey).toString() == initial,
            "invalid shared colors are rejected without writes");
    require(backend.applySwitchValue(binding, false) &&
                configuration.value(shapeKey).toObject().value(QStringLiteral("fill")) == initial &&
                configuration.value(textKey).toObject().value(QStringLiteral("color")) == initial &&
                configuration.value(textKey).toObject().value(QStringLiteral("stroke")) == initial,
            "disable materializes the shared colors instead of restoring old independent colors");
    require(configuration.setValue(shapeKey, QJsonObject{{"stroke", "#FFFFFFFF"}}) &&
                backend.applySwitchValue(binding, true) &&
                configuration.value(colorKey).toString() == initial,
            "re-enable reuses the remembered shared color");
    require(backend.resetSection(settings::SettingsSectionReset::DrawingQuickSelection) &&
                !drawing.syncToolColors() && configuration.value(colorKey).toString().isEmpty() &&
                configuration.value(shapeKey).toObject().value(QStringLiteral("stroke")) ==
                    initial &&
                storage::ScreenshotColorPresetSettings().strokeColors() == palettes,
            "reset disables sync, clears remembered shared color, and keeps current tool colors");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated settings directory");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage
                .initialize({temporary.filePath(QStringLiteral("bin")),
                             temporary.filePath(QStringLiteral("data")), 60000})
                .success,
            "initialize isolated storage");
    testSettings(temporary.filePath(QStringLiteral("data/config.json")));
    storage.shutdown();
    return 0;
}
