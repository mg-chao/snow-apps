#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "locale/locale.h"

#include <QApplication>
#include <QCoreApplication>
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
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("language_manager_tests"));

    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory should be available");
    static_cast<void>(snow_shot::storage::ApplicationStorage::instance().initialize(
        {storageDirectory.path(), storageDirectory.path(), 8000}));
    snow_shot::storage::InterfaceSettings settings;
    settings.setLanguage(QStringLiteral("en"));

    auto& manager = snow_shot::presentation::LanguageManager::instance();
    manager.initialize();

    const auto catalogs = manager.availableLanguages();
    require(catalogs.size() == 3, "the three embedded catalogs should be discovered");
    require(catalogs.at(0).localeName == QStringLiteral("en_US"),
            "the source catalog should sort first");
    require(catalogs.at(1).localeName == QStringLiteral("zh_CN") &&
                catalogs.at(2).localeName == QStringLiteral("zh_TW"),
            "remaining catalogs should sort by canonical locale name");
    require(catalogs.at(0).nativeName == QStringLiteral("English"),
            "English should expose its catalog native name");
    require(catalogs.at(1).nativeName == QString::fromUtf8("简体中文"),
            "Simplified Chinese should expose its catalog native name");
    require(catalogs.at(2).nativeName == QString::fromUtf8("繁體中文"),
            "Traditional Chinese should expose its catalog native name");
    require(manager.languagePreference() == QStringLiteral("en_US"),
            "English should use the canonical en_US preference");
    require(settings.language() == QStringLiteral("en_US"),
            "the canonical language preference should persist");
    require(manager.currentLocale().name() == QStringLiteral("en_US"),
            "the English catalog should be active after migration");
    require(adqt::locale::LocaleManager::instance().locale().name() == QStringLiteral("en_US"),
            "Ant Design Qt should follow the English application locale");

    require(manager.setLanguage(QStringLiteral("zh_CN")), "Simplified Chinese should load");
    require(manager.languagePreference() == QStringLiteral("zh_CN") &&
                manager.currentLocale().name() == QStringLiteral("zh_CN"),
            "the selected locale should become active immediately");
    require(adqt::locale::LocaleManager::instance().locale().name() == QStringLiteral("zh_CN"),
            "Ant Design Qt should follow Simplified Chinese");
    require(settings.language() == QStringLiteral("zh_CN"),
            "a selected locale should persist immediately");

    require(manager.setLanguage(QStringLiteral("system")),
            "Follow system should be a persistent preference");
    require(manager.languagePreference() == QStringLiteral("system"),
            "Follow system should remain the stored preference");
    require(settings.language() == QStringLiteral("system"),
            "Follow system should persist as system");
    require(manager.currentLocale().language() != QLocale::AnyLanguage,
            "Follow system should resolve to a valid bundled locale");

    const QString preferenceBeforeFailure = manager.languagePreference();
    const QString settingBeforeFailure =
        settings.language();
    require(!manager.setLanguage(QStringLiteral("fr_FR")),
            "an unavailable locale should fail transactionally");
    require(manager.languagePreference() == preferenceBeforeFailure,
            "a failed language change should preserve the active preference");
    require(adqt::locale::LocaleManager::instance().locale().name() ==
                manager.currentLocale().name(),
            "a failed language change should preserve the Ant Design locale");
    require(settings.language() == settingBeforeFailure,
            "a failed language change should preserve persisted settings");

    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
