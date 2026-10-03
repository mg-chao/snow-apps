#include "snow_shot/app/edition.h"
#include "snow_shot/presentation/editionfeatures.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
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

void settingsExcludeRemovedFeatures() {
    namespace settings = snow_shot::presentation::settings;
    const auto& registry = settings::builtInSettingsRegistry();
    const auto& catalog = registry.catalog();
    require(catalog.validationErrors().isEmpty(), "Mini settings catalog must validate");
    for (const QString& page : {QStringLiteral("translation"), QStringLiteral("api-configuration"),
                                QStringLiteral("extended-features")}) {
        require(catalog.page(page) == nullptr, "removed settings pages must be unavailable");
    }
    require(
        catalog.page(QStringLiteral("text-recognition-translation"))->title.translated() ==
                QStringLiteral("Text recognition") &&
            catalog.section(QStringLiteral("connections-services"), QStringLiteral("network")) &&
            !catalog.section(QStringLiteral("connections-services"), QStringLiteral("ai-model")) &&
            !catalog.section(QStringLiteral("text-recognition-translation"),
                             QStringLiteral("translation")),
        "Mini keeps OCR and network settings without advertising unavailable providers");
    require(catalog.itemForShortcut(snow_shot::presentation::GlobalShortcutAction::ScreenshotOcr) !=
                nullptr,
            "manual text recognition shortcut must remain available");
    require(catalog.itemForShortcut(
                snow_shot::presentation::GlobalShortcutAction::ScreenshotTranslation) == nullptr &&
                catalog.itemForShortcut(
                    snow_shot::presentation::GlobalShortcutAction::TranslateSelectedText) ==
                    nullptr,
            "translation shortcuts must not be exposed");
    const settings::SettingsSearchIndex index(registry);
    require(index.search(QStringLiteral("API Key")).isEmpty(),
            "Mini search must not advertise credentials for unavailable online providers");
    for (const auto& entry : index.entries()) {
        const auto* item = catalog.item(entry.location);
        require(
            item == nullptr || item->configurationKey.isEmpty() ||
                snow_shot::presentation::editionConfigurationKeyAvailable(item->configurationKey),
            "search must not expose removed configuration keys");
    }
    for (const auto& group : settings::builtInTrayCommandManifest().groups) {
        for (const auto& option : group.options) {
            require(option.shortcutAction !=
                            snow_shot::presentation::GlobalShortcutAction::ScreenshotTranslation &&
                        option.shortcutAction !=
                            snow_shot::presentation::GlobalShortcutAction::TranslateSelectedText,
                    "tray customization must exclude translation actions");
        }
    }
}

void toolbarDefaultsAndImports() {
    namespace layout = snow_shot::presentation::toolbar_layout;
    namespace storage = snow_shot::storage;
    for (const auto kind : {storage::ScreenshotToolbarLayoutKind::ActionTools,
                            storage::ScreenshotToolbarLayoutKind::PinnedActionTools}) {
        const auto defaults = layout::normalizedLayout({}, kind);
        require(defaults.hidden.contains(QStringLiteral("text-recognition")),
                "fresh screenshot and pinned toolbars must hide text recognition");
        require(layout::defaultOrder(kind).contains(QStringLiteral("text-recognition")),
                "manual OCR must remain configurable");
        auto enabled =
            layout::moveItemToPosition(defaults, kind, QStringLiteral("text-recognition"), 0);
        require(!enabled.hidden.contains(QStringLiteral("text-recognition")) &&
                    layout::normalizedLayout(enabled, kind)
                        .positions.constFirst()
                        .contains(QStringLiteral("text-recognition")),
                "explicit OCR visibility must survive normalization");
        storage::ScreenshotToolbarLayout imported;
        imported.positions = {
            {QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
             QStringLiteral("latex-recognition"), QStringLiteral("convert-to-markdown"),
             QStringLiteral("convert-to-html"), QStringLiteral("text-translation")},
            {QStringLiteral("save-as-file")}};
        const auto normalized = layout::normalizedLayout(imported, kind);
        for (const auto& position : normalized.positions)
            for (const auto& id : position)
                require(snow_shot::presentation::editionActionToolAvailable(id),
                        "imported full-edition layouts must discard removed tools");
        require(normalized.hidden.contains(QStringLiteral("text-recognition")),
                "partial imported layouts must preserve hidden default OCR");
    }
    for (const QString& key : {QStringLiteral("screenshot_toolbar/action_tools_layout"),
                               QStringLiteral("pin_to_screen/action_tools_layout")}) {
        const auto value = storage::ConfigurationSchema::defaultValue(key).toObject();
        require(value.value(QStringLiteral("hidden"))
                    .toArray()
                    .contains(QStringLiteral("text-recognition")),
                "stored defaults must agree with toolbar normalization");
        require(storage::ConfigurationSchema::normalize(key, value).valid,
                "Mini stored toolbar defaults must normalize");
    }
    require(!storage::ConfigurationSchema::defaultValue(
                 QStringLiteral("pin_to_screen/automatic_text_recognition"))
                 .toBool(),
            "pin automatic recognition must default off");
    for (const QString& key : {QStringLiteral("api_configuration/server_url"),
                               QStringLiteral("screenshot/auto_recognize_qr_code"),
                               QStringLiteral("screenshot_shortcuts/table_recognition"),
                               QStringLiteral("screenshot_shortcuts/text_translation"),
                               QStringLiteral("screenshot_conversion/vision_model"),
                               QStringLiteral("screenshot_toolbar/table_qr_tool"),
                               QStringLiteral("interface/translation_window_size"),
                               QStringLiteral("extended_features/translation_page_enabled")}) {
        require(!storage::ConfigurationSchema::contains(key) &&
                    !storage::ConfigurationSchema::normalize(key, true).valid,
                "unavailable settings must reject imports and writes");
    }
}

void isolatedStorageAndPinnedRecognitionOptIn() {
    namespace storage = snow_shot::storage;
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated Mini storage");
    const QString executable = temporary.filePath(QStringLiteral("bin"));
    const QString fallback = temporary.filePath(QStringLiteral("mini-app-data"));
    require(QDir().mkpath(executable), "create Mini storage executable directory");
    const auto writeMarker = [&executable](const QString& name, const QByteArray& value) {
        QFile marker(QDir(executable).filePath(name));
        require(marker.open(QIODevice::WriteOnly), "create isolated portable marker");
        require(marker.write(value) == value.size(), "write isolated portable marker");
    };
    const storage::StorageInitializationOptions options{executable, fallback, 60000};
    writeMarker(QStringLiteral("__data_directory"), QByteArrayLiteral("full-portable"));
    const auto withoutMiniMarker = storage::ApplicationStorage::resolveDirectory(options);
    require(withoutMiniMarker.effectiveDirectory == QDir::cleanPath(fallback) &&
                withoutMiniMarker.mode == storage::StorageMode::ApplicationData,
            "Mini must ignore the Full edition portable marker");
    writeMarker(snow_shot::app::edition::portableMarkerName(), QByteArrayLiteral("mini-portable"));
    const QString miniPortable = QDir(executable).filePath(QStringLiteral("mini-portable"));
    const auto withMiniMarker = storage::ApplicationStorage::resolveDirectory(options);
    require(withMiniMarker.effectiveDirectory == miniPortable &&
                withMiniMarker.mode == storage::StorageMode::Portable,
            "Mini must resolve its own portable marker independently");

    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize(options).success, "initialize independent Mini portable storage");
    require(appStorage.configurationDirectory() == miniPortable,
            "Mini configuration must be stored in its own portable directory");
    const storage::PinToScreenSettings pinnedSettings;
    require(!pinnedSettings.automaticTextRecognition(),
            "fresh Mini pin settings must disable automatic OCR");
    require(pinnedSettings.setAutomaticTextRecognition(true) &&
                pinnedSettings.automaticTextRecognition(),
            "Mini users must be able to opt into automatic pinned OCR");
    const storage::ScreenshotToolbarSettings toolbarSettings;
    for (const auto kind : {storage::ScreenshotToolbarLayoutKind::ActionTools,
                            storage::ScreenshotToolbarLayoutKind::PinnedActionTools}) {
        const auto defaults = toolbarSettings.layout(kind);
        require(defaults.hidden.contains(QStringLiteral("text-recognition")),
                "fresh stored screenshot and pinned layouts must hide manual OCR");
        const auto enabled = snow_shot::presentation::toolbar_layout::moveItemToPosition(
            defaults, kind, QStringLiteral("text-recognition"), 0);
        require(toolbarSettings.setLayout(kind, enabled),
                "Mini must accept explicit manual OCR toolbar visibility");
    }
    require(appStorage.flushNow().success, "persist automatic pinned OCR opt-in");
    appStorage.shutdown();
    require(appStorage.initialize(options).success && pinnedSettings.automaticTextRecognition(),
            "automatic pinned OCR opt-in must survive a Mini storage restart");
    for (const auto kind : {storage::ScreenshotToolbarLayoutKind::ActionTools,
                            storage::ScreenshotToolbarLayoutKind::PinnedActionTools}) {
        const auto enabled = toolbarSettings.layout(kind);
        require(!enabled.hidden.contains(QStringLiteral("text-recognition")) &&
                    enabled.positions.constFirst().contains(QStringLiteral("text-recognition")),
                "manual OCR toolbar opt-in must survive a Mini storage restart");
    }
    require(pinnedSettings.setAutomaticTextRecognition(false) &&
                !pinnedSettings.automaticTextRecognition(),
            "Mini users must be able to disable automatic pinned OCR again");
    appStorage.shutdown();
    require(!QDir(executable).exists(QStringLiteral("full-portable")),
            "Mini storage must not create the Full edition portable directory");
}

void fullArchiveImportPreservesSupportedChoices() {
    namespace storage = snow_shot::storage;
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated configuration archive fixture");
    const QString executable = temporary.filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "create archive fixture executable directory");
    const storage::StorageInitializationOptions options{
        executable, temporary.filePath(QStringLiteral("mini-app-data")), 60000};
    auto& appStorage = storage::ApplicationStorage::instance();
    require(appStorage.initialize(options).success, "initialize isolated archive import storage");
    const auto original = appStorage.configuration().snapshot();
    const QJsonObject fullLayout{
        {QStringLiteral("positions"),
         QJsonArray{
             QJsonArray{QStringLiteral("text-recognition"), QStringLiteral("table-recognition"),
                        QStringLiteral("barcode-recognition"),
                        QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")},
             QJsonArray{QStringLiteral("save-as-file")}}},
        {QStringLiteral("hidden"),
         QJsonArray{QStringLiteral("latex-recognition"), QStringLiteral("text-translation")}}};
    const QMap<QString, QJsonValue> unavailable{
        {QStringLiteral("api_configuration/server_url"), QStringLiteral("https://example.test")},
        {QStringLiteral("api_configuration/custom_models"), QJsonArray()},
        {QStringLiteral("api_configuration/text_translation"), QJsonArray()},
        {QStringLiteral("screenshot_translation/source_language"), QStringLiteral("auto")},
        {QStringLiteral("global_shortcuts/screenshot_translation"),
         QJsonArray{QStringLiteral("Ctrl+Alt+T")}},
        {QStringLiteral("global_mouse/screenshot_translation"), QJsonObject()},
        {QStringLiteral("screenshot/auto_recognize_qr_code"), true},
        {QStringLiteral("screenshot_shortcuts/table_recognition"),
         QJsonArray{QStringLiteral("Ctrl+Alt+1")}},
        {QStringLiteral("screenshot_shortcuts/qr_code_recognition"),
         QJsonArray{QStringLiteral("Ctrl+Alt+2")}},
        {QStringLiteral("screenshot_shortcuts/text_translation"),
         QJsonArray{QStringLiteral("Ctrl+Alt+3")}},
        {QStringLiteral("screenshot_conversion/vision_model"), QStringLiteral("full-only-model")},
        {QStringLiteral("screenshot_toolbar/table_qr_tool"), QStringLiteral("table-recognition")},
        {QStringLiteral("interface/translation_window_size"), QJsonObject()},
        {QStringLiteral("extended_features/translation_page_enabled"), true}};
    QMap<QString, QJsonValue> fullSnapshot = unavailable;
    fullSnapshot.insert(QStringLiteral("interface/theme_mode"), QStringLiteral("dark"));
    fullSnapshot.insert(QStringLiteral("pin_to_screen/automatic_text_recognition"), true);
    fullSnapshot.insert(QStringLiteral("screenshot_toolbar/action_tools_layout"), fullLayout);
    fullSnapshot.insert(QStringLiteral("pin_to_screen/action_tools_layout"), fullLayout);
    const QString archivePath = temporary.filePath(QStringLiteral("full-settings.snowshot"));
    require(storage::ConfigurationArchive::write(
                archivePath, fullSnapshot, storage::ConfigurationStore::currentSchemaVersion())
                .isEmpty(),
            "write Full-like archive through the public snapshot API");
    const auto imported = storage::ConfigurationArchive::read(archivePath);
    require(imported.isValid() && imported.values.size() == 4,
            "archive read must keep only the supported Full settings");
    require(appStorage.configuration().snapshot() == original,
            "reading an archive must not mutate live Mini settings");
    for (auto it = unavailable.cbegin(); it != unavailable.cend(); ++it) {
        require(!imported.values.contains(it.key()),
                "archive read must remove unsupported API, translation, QR and table keys");
    }
    for (const QString& key : {QStringLiteral("screenshot_toolbar/action_tools_layout"),
                               QStringLiteral("pin_to_screen/action_tools_layout")}) {
        const auto layout = imported.values.value(key).toObject();
        const auto hidden = layout.value(QStringLiteral("hidden")).toArray();
        require(!hidden.contains(QStringLiteral("text-recognition")),
                "archive normalization must preserve explicit OCR visibility");
        require(layout.value(QStringLiteral("positions"))
                    .toArray()
                    .first()
                    .toArray()
                    .contains(QStringLiteral("text-recognition")),
                "archive normalization must retain the visible OCR tool");
        for (const auto& position : layout.value(QStringLiteral("positions")).toArray()) {
            for (const auto& id : position.toArray()) {
                require(snow_shot::presentation::editionActionToolAvailable(id.toString()),
                        "archive normalization must remove unavailable visible tools");
            }
        }
        for (const auto& id : hidden) {
            require(snow_shot::presentation::editionActionToolAvailable(id.toString()),
                    "archive normalization must remove unavailable hidden tools");
        }
    }
    require(appStorage.configuration().applySnapshot(imported.values, imported.schemaVersion),
            "apply normalized Full archive into Mini storage");
    const storage::ScreenshotToolbarSettings toolbarSettings;
    const storage::PinToScreenSettings pinSettings;
    const auto requireImportedChoices = [&] {
        require(appStorage.configuration().value(QStringLiteral("interface/theme_mode")) ==
                        QJsonValue(QStringLiteral("dark")) &&
                    pinSettings.automaticTextRecognition(),
                "Mini import must preserve supported theme and automatic OCR choices");
        for (const auto kind : {storage::ScreenshotToolbarLayoutKind::ActionTools,
                                storage::ScreenshotToolbarLayoutKind::PinnedActionTools}) {
            const auto layout = toolbarSettings.layout(kind);
            require(!layout.hidden.contains(QStringLiteral("text-recognition")) &&
                        layout.positions.constFirst().contains(QStringLiteral("text-recognition")),
                    "applied Full archive must preserve explicit manual OCR visibility");
        }
        for (auto it = unavailable.cbegin(); it != unavailable.cend(); ++it) {
            require(!appStorage.configuration().snapshot().contains(it.key()),
                    "applied Mini storage must exclude unsupported Full settings");
        }
    };
    requireImportedChoices();
    require(appStorage.flushNow().success, "persist imported Mini configuration");
    appStorage.shutdown();
    require(appStorage.initialize(options).success, "reload imported Mini configuration");
    requireImportedChoices();
    appStorage.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    require(snow_shot::app::edition::isMini, "this test must be compiled as Mini");
    require(snow_shot::app::edition::productName() == QStringLiteral("Snow Shot Mini") &&
                snow_shot::app::edition::applicationName() == QStringLiteral("snow_shot_mini") &&
                snow_shot::app::edition::portableMarkerName() ==
                    QStringLiteral("__mini_data_directory"),
            "Mini must use independent product and portable storage identities");
    settingsExcludeRemovedFeatures();
    toolbarDefaultsAndImports();
    isolatedStorageAndPinnedRecognitionOptIn();
    fullArchiveImportPreservesSupportedChoices();
    return 0;
}
