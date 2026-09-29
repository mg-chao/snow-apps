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
            "the English catalog should be active after canonicalizing the locale");
    require(adqt::locale::LocaleManager::instance().locale().name() == QStringLiteral("en_US"),
            "Ant Design Qt should follow the English application locale");

    require(manager.setLanguage(QStringLiteral("zh_CN")), "Simplified Chinese should load");
    require(QCoreApplication::translate("SettingsCatalog", "When pinning duplicate content") ==
                    QString::fromUtf8("固定重复内容时") &&
                QCoreApplication::translate("SettingsCatalog", "Shake Window") ==
                    QString::fromUtf8("晃动窗口") &&
                QCoreApplication::translate("SettingsCatalog", "Repeat Action") ==
                    QString::fromUtf8("重复执行"),
            "language changes translate duplicate pin settings and option labels");
    require(QCoreApplication::translate("ScreenshotPinnedWindow", "Decrease 10%") ==
                    QString::fromUtf8("减少 10%") &&
                QCoreApplication::translate("SettingsCatalog", "Increase scale by 10%") ==
                    QString::fromUtf8("缩放比例增加 10%"),
            "Simplified Chinese must translate pinned image adjustment commands");
    require(manager.languagePreference() == QStringLiteral("zh_CN") &&
                manager.currentLocale().name() == QStringLiteral("zh_CN"),
            "the selected locale should become active immediately");
    require(adqt::locale::LocaleManager::instance().locale().name() == QStringLiteral("zh_CN"),
            "Ant Design Qt should follow Simplified Chinese");
    require(settings.language() == QStringLiteral("zh_CN"),
            "a selected locale should persist immediately");
    require(QCoreApplication::translate("PinnedWindowManagementPageWidget",
                                        "Pin to Screen Management") ==
                QString::fromUtf8("固定到屏幕管理"),
            "Simplified Chinese should use Pin to Screen terminology in management");
    require(QCoreApplication::translate("SettingsCatalog", "Pin to Screen Management") ==
                QString::fromUtf8("固定到屏幕管理"),
            "Simplified Chinese settings should use the same management title");
    require(QCoreApplication::translate("PinnedWindowManagementPageWidget", "No pinned windows") ==
                QString::fromUtf8("暂无固定到屏幕窗口"),
            "Simplified Chinese management should use the same window terminology");

    require(manager.setLanguage(QStringLiteral("zh_TW")), "Traditional Chinese should load");
    require(QCoreApplication::translate("SettingsCatalog", "When pinning duplicate content") ==
                    QString::fromUtf8("固定重複內容時") &&
                QCoreApplication::translate("SettingsCatalog", "Shake Window") ==
                    QString::fromUtf8("晃動視窗") &&
                QCoreApplication::translate("SettingsCatalog", "Repeat Action") ==
                    QString::fromUtf8("重複執行"),
            "language changes translate duplicate pin settings and option labels");
    require(QCoreApplication::translate("ScreenshotPinnedWindow", "Decrease 10%") ==
                    QString::fromUtf8("減少 10%") &&
                QCoreApplication::translate("SettingsCatalog", "Increase scale by 10%") ==
                    QString::fromUtf8("縮放比例增加 10%"),
            "Traditional Chinese must translate pinned image adjustment commands");
    require(QCoreApplication::translate("PinnedWindowManagementPageWidget",
                                        "Pin to Screen Management") ==
                QString::fromUtf8("固定到螢幕管理"),
            "Traditional Chinese should use Pin to Screen terminology in management");
    require(QCoreApplication::translate("SettingsCatalog", "Pin to Screen Management") ==
                QString::fromUtf8("固定到螢幕管理"),
            "Traditional Chinese settings should use the same management title");
    require(QCoreApplication::translate("PinnedWindowManagementPageWidget", "No pinned windows") ==
                QString::fromUtf8("尚無固定到螢幕視窗"),
            "Traditional Chinese management should use the same window terminology");

    require(manager.setLanguage(QStringLiteral("system")),
            "Follow system should be a persistent preference");
    require(manager.languagePreference() == QStringLiteral("system"),
            "Follow system should remain the stored preference");
    require(settings.language() == QStringLiteral("system"),
            "Follow system should persist as system");
    require(manager.currentLocale().language() != QLocale::AnyLanguage,
            "Follow system should resolve to a valid bundled locale");

    const QString preferenceBeforeFailure = manager.languagePreference();
    const QString settingBeforeFailure = settings.language();
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
