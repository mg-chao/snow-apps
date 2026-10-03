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
#include <QFontDatabase>
#include <QJsonDocument>
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

void requireUpdateAppearance() {
    const auto& resolved = adqt::theme::ThemeManager::instance().globalResolvedTheme();
    const auto& colors = resolved.theme.palette;
    const auto& metrics = resolved.theme.metrics;
    const QJsonObject appearance =
        snow_shot::presentation::styles::ThemeManager::instance().updateProgressAppearance();
    require(appearance.size() == 16, "updater appearance contains the complete token contract");
    const bool dark = resolved.config.scheme == adqt::theme::ThemeScheme::Dark;
    require(appearance.value(QStringLiteral("background")).toInt() ==
                    (dark ? 0x1f1f1f : 0xffffff) &&
                appearance.value(QStringLiteral("text")).toInt() == (dark ? 0xdddddd : 0x1f1f1f) &&
                appearance.value(QStringLiteral("textSecondary")).toInt() ==
                    (dark ? 0xb1b1b1 : 0x595959) &&
                appearance.value(QStringLiteral("textTertiary")).toInt() ==
                    (dark ? 0x848484 : 0x8c8c8c) &&
                appearance.value(QStringLiteral("fillSecondary")).toInt() ==
                    (dark ? 0x3a3a3a : 0xf0f0f0) &&
                appearance.value(QStringLiteral("fillTertiary")).toInt() ==
                    (dark ? 0x313131 : 0xf5f5f5),
            "updater flattens translucent tokens over its elevated surface");
    const auto rgb = [](const QColor& color) { return static_cast<int>(color.rgb() & 0xffffffU); };
    require(appearance.value(QStringLiteral("primary")).toInt() == rgb(colors.colorPrimary) &&
                appearance.value(QStringLiteral("primaryBackground")).toInt() ==
                    rgb(colors.colorPrimaryBg) &&
                appearance.value(QStringLiteral("border")).toInt() ==
                    (dark ? 0x303030 : 0xf0f0f0) &&
                appearance.value(QStringLiteral("success")).toInt() == rgb(colors.colorSuccess) &&
                appearance.value(QStringLiteral("error")).toInt() == rgb(colors.colorError),
            "updater inherits resolved accent and semantic colors");
    require(appearance.value(QStringLiteral("fontFamily")).toString() ==
                    QApplication::font().family() &&
                appearance.value(QStringLiteral("fontSize")).toInt() == qRound(metrics.fontSize) &&
                appearance.value(QStringLiteral("smallFontSize")).toInt() ==
                    qRound(metrics.fontSizeSM) &&
                appearance.value(QStringLiteral("borderRadius")).toInt() ==
                    qRound(metrics.borderRadiusLG) &&
                appearance.value(QStringLiteral("motion")).toBool() == resolved.config.motion,
            "updater inherits application typography, popup radius, and motion preference");
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
    requireUpdateAppearance();
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
    requireUpdateAppearance();
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
        requireUpdateAppearance();
    }
    const QString customFont = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    require(theme.setAppFontFamily(customFont), "configure updater font through app preference");
    requireUpdateAppearance();
    require(theme.updateProgressAppearance().value(QStringLiteral("fontFamily")).toString() ==
                customFont,
            "updater receives the configured application font");
    require(backend.resetSection(settings::SettingsSectionReset::GeneralSettings) &&
                preferences.themePrimaryColor() == QColor("#1677ff") &&
                adqt::theme::ThemeManager::instance().config().primary == QColor("#1677ff"),
            "general reset restores the default primary color immediately");
    theme.setThemeMode(styles::ThemeMode::Light);
    requireUpdateAppearance();
    const QJsonObject defaultAppearance = theme.updateProgressAppearance();
    require(defaultAppearance.value(QStringLiteral("background")).toInt() == 0xffffff &&
                defaultAppearance.value(QStringLiteral("primary")).toInt() == 0x1677ff &&
                defaultAppearance.value(QStringLiteral("fontSize")).toInt() == 14 &&
                defaultAppearance.value(QStringLiteral("smallFontSize")).toInt() == 12 &&
                defaultAppearance.value(QStringLiteral("borderRadius")).toInt() == 8,
            "default updater matches Ant's comfortable notification and small progress tokens");
    const QFont previousFont = QApplication::font();
    const QString oversizedFamily(64 * 1024, QLatin1Char('A'));
    QFont oversizedFont = previousFont;
    oversizedFont.setFamily(oversizedFamily);
    QApplication::setFont(oversizedFont);
    const QJsonObject boundedAppearance = theme.updateProgressAppearance();
    QApplication::setFont(previousFont);
    require(boundedAppearance.value(QStringLiteral("fontFamily")).toString() ==
                    oversizedFamily.left(63) &&
                QJsonDocument(boundedAppearance).toJson(QJsonDocument::Compact).size() < 2048,
            "imported font names are bounded before updater protocol serialization");
    auto motionConfig = adqt::theme::ThemeManager::instance().config();
    motionConfig.motion = false;
    adqt::theme::ThemeManager::instance().setConfig(motionConfig);
    requireUpdateAppearance();
    require(!theme.updateProgressAppearance().value(QStringLiteral("motion")).toBool(),
            "updater respects disabled Ant motion");
    require(appStorage.flushNow().success, "flush theme preferences");
    appStorage.shutdown();
    return 0;
}
