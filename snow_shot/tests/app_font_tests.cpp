#include "snow_shot/presentation/fontfamilies.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"
#include "theme/theme_manager.h"

#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QMenu>
#include <QTemporaryDir>

#include <algorithm>
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
#ifdef Q_OS_WIN
    const QDir fonts(qEnvironmentVariable("WINDIR") + QStringLiteral("/Fonts"));
    require(QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("segoeui.ttf"))) >= 0 &&
                QFontDatabase::addApplicationFont(fonts.filePath(QStringLiteral("msyh.ttc"))) >= 0,
            "load deterministic UI fonts for offscreen checks");
#endif
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated font storage");
    namespace presentation = snow_shot::presentation;
    namespace storage = snow_shot::storage;
    namespace settings = presentation::settings;
    namespace styles = presentation::styles;
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize font storage");
    presentation::LanguageManager::instance().initialize();
    const storage::InterfaceSettings preferences;
    require(preferences.appFontFamily().isEmpty(), "system font is the default");
    const QFont originalFont = QApplication::font();
    const QStringList families = presentation::applicationFontFamilies();
    require(!families.isEmpty(), "font families are available");
    require(std::is_sorted(families.cbegin(), families.cend(),
                           [](const auto& a, const auto& b) {
                               return QString::compare(a, b, Qt::CaseInsensitive) < 0;
                           }),
            "font choices are sorted");
    const QString family = families.first();
    require(preferences.setAppFontFamily(family), "save startup font");
    auto& theme = styles::ThemeManager::instance();
    theme.initialize(application);
    require(theme.appFontFamily() == family && QApplication::font().family() == family,
            "startup applies the saved family");
    presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    const auto* field = registry.field(QStringLiteral("interface.app-font"));
    require(field != nullptr && field->pageId == QStringLiteral("general-appearance") &&
                field->definition->configurationKey == QStringLiteral("interface/app_font"),
            "app font is registered in interface settings");
    const auto* page = registry.catalog().page(QStringLiteral("general-appearance"));
    const auto& general = page->sections.first();
    require(general.items.last().id == field->id &&
                general.items.at(general.items.size() - 2).id ==
                    QStringLiteral("interface.theme-primary-color"),
            "app font follows theme controls in Appearance");
    QLabel existing(QStringLiteral("Existing label"));
    QMenu menu;
    const auto binding = settings::SettingsSelectBinding::AppFont;
    const QString next = families.last();
    require(backend.applySelectValue(binding, next) && preferences.appFontFamily() == next &&
                backend.selectValue(binding).toString() == next,
            "editing font persists and updates runtime state");
    QCoreApplication::processEvents();
    QLabel created(QStringLiteral("New label"));
    require(existing.font().family() == next && created.font().family() == next,
            "existing and new widgets use the selected family");
    require(menu.font().family() == next, "native popup fonts follow the selected family");
    require(QApplication::font().pointSizeF() == originalFont.pointSizeF() &&
                QApplication::font().weight() == originalFont.weight() &&
                QApplication::font().hintingPreference() == QFont::PreferNoHinting,
            "changing family preserves size, weight and smooth rendering");
    require(appStorage.flushNow().success, "flush font preferences");
    const storage::ConfigurationStore reloaded(
        QDir(appStorage.configurationDirectory()).filePath(QStringLiteral("config.json")), true,
        false);
    require(reloaded.value(QStringLiteral("interface/app_font")).toString() == next,
            "font preference survives reload");
    for (auto mode :
         {styles::ThemeMode::Dark, styles::ThemeMode::Light, styles::ThemeMode::FollowSystem}) {
        theme.setThemeMode(mode);
        theme.setThemePreset(styles::ThemePreset::Compact);
        theme.setThemePreset(styles::ThemePreset::Default);
        require(theme.setThemePrimaryColor(QColor("#13c2c2")), "change primary color");
        require(theme.appFontFamily() == next && QApplication::font().family() == next &&
                    adqt::theme::ThemeManager::instance().config().appFont.family() == next,
                "theme changes preserve the font family");
    }
    const auto previous = appStorage.configuration().snapshot();
    auto imported = previous;
    const QString unavailable = QStringLiteral("SnowShot Missing Font Test Family");
    imported.insert(QStringLiteral("interface/app_font"), unavailable);
    require(backend.importConfigurationSnapshot(imported, 3) &&
                theme.appFontFamily() == unavailable,
            "configuration import applies unavailable font families using Qt fallback");
    const auto options = backend.dynamicSelectOptions(binding);
    require(std::any_of(options.cbegin(), options.cend(),
                        [&](const auto& option) { return option.value.toString() == unavailable; }),
            "unavailable saved font remains an option");
    require(backend.importConfigurationSnapshot(previous, 3) && theme.appFontFamily() == next,
            "restoring a snapshot restores the live font");
    imported.insert(QStringLiteral("interface/app_font"), 42);
    require(backend.importConfigurationSnapshot(imported, 3) && theme.appFontFamily().isEmpty() &&
                preferences.appFontFamily().isEmpty(),
            "invalid imported font values follow the schema salvage rule and restore default");
    require(backend.importConfigurationSnapshot(previous, 3), "restore the font after salvage");
    require(!backend.importConfigurationSnapshot(imported, 999) && theme.appFontFamily() == next &&
                preferences.appFontFamily() == next,
            "rejected import leaves live and stored font unchanged");
    const bool reset = backend.resetSection(settings::SettingsSectionReset::GeneralSettings);
    require(reset && theme.appFontFamily().isEmpty() && preferences.appFontFamily().isEmpty(),
            "General reset restores System default");
    QCoreApplication::processEvents();
    require(QApplication::font().family() == originalFont.family() &&
                existing.font().family() == originalFont.family(),
            "System default restores the original platform family");
    appStorage.shutdown();
    return 0;
}
