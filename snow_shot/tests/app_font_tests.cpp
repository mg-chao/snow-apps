#include "snow_shot/presentation/fontfamilies.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationschema.h"
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
    require(preferences.appFontSizePercentage() == 100, "font size defaults to 100 percent");
    const QFont originalFont = QApplication::font();
    const int originalThemeFontSize =
        qRound(adqt::theme::ThemeManager::instance().config().fontSize);
    const auto requireFontSize = [&](const QFont& font, int percentage) {
        const qreal scale = percentage / 100.0;
        require(originalFont.pointSizeF() > 0
                    ? qFuzzyCompare(font.pointSizeF(), originalFont.pointSizeF() * scale)
                    : font.pixelSize() == qRound(originalFont.pixelSize() * scale),
                "application font scales from its original size without compounding");
    };
    const QStringList families = presentation::applicationFontFamilies();
    require(!families.isEmpty(), "font families are available");
    require(std::is_sorted(families.cbegin(), families.cend(),
                           [](const auto& a, const auto& b) {
                               return QString::compare(a, b, Qt::CaseInsensitive) < 0;
                           }),
            "font choices are sorted");
    const QString family = families.first();
    require(preferences.setAppFontFamily(family), "save startup font");
    require(preferences.setAppFontSizePercentage(150), "save startup font size");
    auto& theme = styles::ThemeManager::instance();
    theme.initialize(application);
    require(theme.appFontFamily() == family && QApplication::font().family() == family,
            "startup applies the saved family");
    require(theme.appFontSizePercentage() == 150, "startup applies the saved font size");
    requireFontSize(QApplication::font(), 150);
    presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto registry = settings::buildBuiltInSettingsRegistry();
    const auto* field = registry.field(QStringLiteral("interface.app-font"));
    require(field != nullptr && field->pageId == QStringLiteral("general-appearance") &&
                field->definition->configurationKey == QStringLiteral("interface/app_font"),
            "app font is registered in interface settings");
    const auto* page = registry.catalog().page(QStringLiteral("general-appearance"));
    const auto& general = page->sections.first();
    const auto* sizeField = registry.field(QStringLiteral("interface.app-font-size"));
    require(sizeField && general.items.last().id == sizeField->id &&
                general.items.at(general.items.size() - 2).id == field->id &&
                general.items.at(general.items.size() - 3).id ==
                    QStringLiteral("interface.theme-primary-color"),
            "font size immediately follows app font in Appearance");
    const auto* sizeSchema = storage::ConfigurationSchema::entry(sizeField->configurationKey);
    require(sizeField->kind == settings::SettingsFieldKind::Slider && sizeSchema &&
                sizeField->configurationKey ==
                    QStringLiteral("interface/app_font_size_percentage") &&
                sizeField->defaultValue.toInt() == 100 && sizeSchema->integerRange.has_value() &&
                sizeSchema->integerRange->minimum == 100 &&
                sizeSchema->integerRange->maximum == 200 && sizeSchema->integerRange->step == 1 &&
                std::get<settings::SettingsSliderDefinition>(sizeField->definition->payload)
                        .suffix.translated() == QStringLiteral("%"),
            "font size slider ranges from 100 to 200 percent with a 100 percent default");
    const auto sizeBinding = settings::SettingsSliderBinding::AppFontSize;
    require(backend.applySliderValue(sizeBinding, 100), "restore the default startup size");
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
    for (const int percentage : {101, 133, 200, 150, 100, 200}) {
        require(backend.applySliderValue(sizeBinding, percentage) &&
                    preferences.appFontSizePercentage() == percentage &&
                    backend.sliderValue(sizeBinding) == percentage,
                "font size edits persist and update runtime state");
        QCoreApplication::processEvents();
        const QLabel newLabel(QStringLiteral("New scaled label"));
        requireFontSize(QApplication::font(), percentage);
        requireFontSize(existing.font(), percentage);
        requireFontSize(newLabel.font(), percentage);
        requireFontSize(menu.font(), percentage);
        require(theme.themeColorScheme().metricAlias.fontSize ==
                        qRound(originalThemeFontSize * percentage / 100.0) &&
                    QApplication::font().family() == next,
                "theme typography and application fonts scale together while preserving family");
    }
    require(!backend.applySliderValue(sizeBinding, 99) &&
                !backend.applySliderValue(sizeBinding, 201) &&
                theme.appFontSizePercentage() == 200 && preferences.appFontSizePercentage() == 200,
            "out-of-range font sizes leave runtime and stored settings unchanged");
    require(backend.applySelectValue(binding, QString()), "select system font at an enlarged size");
    requireFontSize(QApplication::font(), 200);
    require(backend.applySelectValue(binding, next), "restore selected family at an enlarged size");
    requireFontSize(QApplication::font(), 200);
    require(appStorage.flushNow().success, "flush font preferences");
    const storage::ConfigurationStore reloaded(
        QDir(appStorage.configurationDirectory()).filePath(QStringLiteral("config.json")), true,
        false);
    require(reloaded.value(QStringLiteral("interface/app_font")).toString() == next &&
                reloaded.value(QStringLiteral("interface/app_font_size_percentage")).toInt() == 200,
            "font family and size preferences survive reload");
    for (auto mode :
         {styles::ThemeMode::Dark, styles::ThemeMode::Light, styles::ThemeMode::FollowSystem}) {
        theme.setThemeMode(mode);
        theme.setThemePreset(styles::ThemePreset::Compact);
        theme.setThemePreset(styles::ThemePreset::Default);
        require(theme.setThemePrimaryColor(QColor("#13c2c2")), "change primary color");
        require(theme.appFontFamily() == next && QApplication::font().family() == next &&
                    adqt::theme::ThemeManager::instance().config().appFont.family() == next,
                "theme changes preserve the font family");
        requireFontSize(QApplication::font(), 200);
        require(theme.themeColorScheme().metricAlias.fontSize == originalThemeFontSize * 2,
                "theme and density changes preserve the font size");
    }
    const auto previous = appStorage.configuration().snapshot();
    auto imported = previous;
    const QString unavailable = QStringLiteral("SnowShot Missing Font Test Family");
    imported.insert(QStringLiteral("interface/app_font"), unavailable);
    imported.insert(QStringLiteral("interface/app_font_size_percentage"), 175);
    require(backend.importConfigurationSnapshot(imported, 3) &&
                theme.appFontFamily() == unavailable && theme.appFontSizePercentage() == 175,
            "configuration import applies font family and size using Qt fallback");
    requireFontSize(QApplication::font(), 175);
    const auto options = backend.dynamicSelectOptions(binding);
    require(std::any_of(options.cbegin(), options.cend(),
                        [&](const auto& option) { return option.value.toString() == unavailable; }),
            "unavailable saved font remains an option");
    require(backend.importConfigurationSnapshot(previous, 3) && theme.appFontFamily() == next &&
                theme.appFontSizePercentage() == 200,
            "restoring a snapshot restores the live font and size");
    imported.insert(QStringLiteral("interface/app_font"), 42);
    imported.insert(QStringLiteral("interface/app_font_size_percentage"), 201);
    require(backend.importConfigurationSnapshot(imported, 3) && theme.appFontFamily().isEmpty() &&
                preferences.appFontFamily().isEmpty() && theme.appFontSizePercentage() == 100 &&
                preferences.appFontSizePercentage() == 100,
            "invalid imported font values follow the schema salvage rule and restore default");
    requireFontSize(QApplication::font(), 100);
    require(backend.importConfigurationSnapshot(previous, 3), "restore the font after salvage");
    require(!backend.importConfigurationSnapshot(imported, 999) && theme.appFontFamily() == next &&
                preferences.appFontFamily() == next && theme.appFontSizePercentage() == 200 &&
                preferences.appFontSizePercentage() == 200,
            "rejected import leaves live and stored font unchanged");
    const bool reset = backend.resetSection(settings::SettingsSectionReset::GeneralSettings);
    require(reset && theme.appFontFamily().isEmpty() && preferences.appFontFamily().isEmpty() &&
                theme.appFontSizePercentage() == 100 && preferences.appFontSizePercentage() == 100,
            "General reset restores System default and 100 percent font size");
    QCoreApplication::processEvents();
    require(QApplication::font().family() == originalFont.family() &&
                existing.font().family() == originalFont.family(),
            "System default restores the original platform family");
    requireFontSize(QApplication::font(), 100);
    requireFontSize(existing.font(), 100);
    appStorage.shutdown();
    return 0;
}
