#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "theme/theme_manager.h"
#include "widgets/color_picker.h"

#include <QApplication>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace settings = snow_shot::presentation::settings;
namespace styles = snow_shot::presentation::styles;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void settingsColorsCommitOnPopupClose(QApplication& application) {
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    auto& theme = styles::ThemeManager::instance();
    int tested = 0;
    for (const auto& field : registry.fields()) {
        if (field.kind != settings::SettingsFieldKind::Color) {
            continue;
        }
        std::cout << "Checking " << field.id.toStdString() << '\n';
        const auto& definition =
            std::get<settings::SettingsColorDefinition>(field.definition->payload);
        SettingsPageWidget page(registry, field.pageId, session);
        page.resize(880, 760);
        page.show();
        application.processEvents();
        auto* row = page.findChild<QWidget*>(
            settings::generatedObjectName(QStringLiteral("settings-item"), field.id));
        auto* picker = row ? row->findChild<adqt::widgets::AdColorPicker*>() : nullptr;
        require(picker != nullptr, "color setting creates a picker");
        const QColor original = backend.colorValue(definition.binding);
        const QColor originalPrimary = adqt::theme::ThemeManager::instance().config().primary;
        int writes = 0;
        int themeChanges = 0;
        QObject::connect(&storage::ApplicationStorage::instance().configuration(),
                         &storage::ConfigurationStore::valueChanged, &page,
                         [&writes, &field](const QString& key) {
                             if (key == field.configurationKey) {
                                 ++writes;
                             }
                         });
        QObject::connect(&theme, &styles::ThemeManager::themeChanged, &page,
                         [&themeChanges] { ++themeChanges; });
        picker->setPopupVisible(true);
        application.processEvents();
        require(picker->popupVisible(), "color popup opens offscreen");
        QColor finalColor;
        for (int step = 0; step < 3; ++step) {
            finalColor = QColor(31 + step * 30, 97, 183,
                                definition.alphaChannelEnabled ? 100 + step * 30 : 255);
            picker->commitValue(adqt::widgets::AdColorValue::solid(finalColor));
            application.processEvents();
            require(picker->value().solidColor == finalColor,
                    "picker previews the current edit locally");
            require(backend.colorValue(definition.binding) == original && writes == 0,
                    "intermediate popup edits must not persist settings");
            require(themeChanges == 0 &&
                        adqt::theme::ThemeManager::instance().config().primary == originalPrimary,
                    "intermediate popup edits must not rebuild the application theme");
        }
        picker->setPopupVisible(false);
        application.processEvents();
        require(backend.colorValue(definition.binding) == finalColor && writes == 1,
                "closing the popup persists only its final color, including alpha");
        const bool primary =
            definition.binding == settings::SettingsColorBinding::ThemePrimaryColor;
        require(themeChanges == (primary ? 1 : 0), "theme updates only once after completion");
        if (primary) {
            require(adqt::theme::ThemeManager::instance().config().primary == finalColor,
                    "completed primary color is applied to the application theme");
        }
        picker->setPopupVisible(true);
        application.processEvents();
        picker->setPopupVisible(false);
        application.processEvents();
        require(writes == 1 && themeChanges == (primary ? 1 : 0),
                "opening and closing without editing does not apply another change");
        ++tested;
    }
    require(tested == 7, "cover all seven settings color pickers");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated settings storage");
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize settings storage");
    snow_shot::presentation::LanguageManager::instance().initialize();
    styles::ThemeManager::instance().initialize(application);
    settingsColorsCommitOnPopupClose(application);
    appStorage.shutdown();
    return 0;
}
