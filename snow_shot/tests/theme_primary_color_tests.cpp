#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingscatalog.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QDir>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated theme storage");
    const QString executable = QDir(temporary.path()).filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create executable directory");
    namespace storage = snow_shot::storage;
    namespace settings = snow_shot::presentation::settings;
    namespace styles = snow_shot::presentation::styles;
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize({executable, temporary.path(), 60000}).success,
            "initialize theme storage");
    snow_shot::presentation::LanguageManager::instance().initialize();
    const storage::InterfaceSettings preferences;
    require(preferences.themePrimaryColor() == QColor("#1677ff"), "default primary is blue");
    const QColor saved("#722ed1");
    require(preferences.setThemePrimaryColor(saved), "save primary before theme initialization");
    auto& theme = styles::ThemeManager::instance();
    theme.initialize(application);
    require(adqt::theme::ThemeManager::instance().config().primary == saved,
            "startup applies saved primary color");
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto binding = settings::SettingsColorBinding::ThemePrimaryColor;
    const QColor custom("#13c2c2");
    int changes = 0;
    QObject::connect(&theme, &styles::ThemeManager::themeChanged, &application,
                     [&changes] { ++changes; });
    require(backend.applyColorValue(binding, custom) && backend.colorValue(binding) == custom &&
                preferences.themePrimaryColor() == custom && changes == 1 &&
                adqt::theme::ThemeManager::instance().config().primary == custom,
            "editing primary persists and updates the live theme");
    require(appStorage.flushNow().success, "flush custom theme preference");
    const storage::ConfigurationStore reloaded(
        QDir(appStorage.configurationDirectory()).filePath(QStringLiteral("config.json")), true,
        false);
    require(storage::colorFromRgbaString(
                reloaded.value(QStringLiteral("interface/theme_primary_color")).toString()) ==
                custom,
            "custom primary color survives a configuration reload");
    require(!backend.applyColorValue(binding, QColor()) &&
                preferences.themePrimaryColor() == custom,
            "invalid colors leave the preference unchanged");
    for (auto mode :
         {styles::ThemeMode::Dark, styles::ThemeMode::Light, styles::ThemeMode::FollowSystem}) {
        theme.setThemeMode(mode);
        require(adqt::theme::ThemeManager::instance().config().primary == custom,
                "appearance changes preserve primary color");
    }
    require(backend.resetSection(settings::SettingsSectionReset::GeneralSettings) &&
                preferences.themePrimaryColor() == QColor("#1677ff") &&
                adqt::theme::ThemeManager::instance().config().primary == QColor("#1677ff"),
            "general reset restores the default primary color immediately");
    require(appStorage.flushNow().success, "flush theme preferences");
    appStorage.shutdown();
    return 0;
}
