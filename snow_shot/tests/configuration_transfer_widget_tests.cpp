#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/presentation/globalshortcutmanager.h"

#include "antd_icons.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QStringList>
#include <QTemporaryDir>
#include <QTranslator>
#include <QWidget>

#include <iostream>

namespace presentation = snow_shot::presentation;
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void flushEvents() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class SettingsCatalogButtonTranslator final : public QTranslator {
  public:
    QString translate(const char* context, const char* sourceText, const char*,
                      int) const override {
        if (QLatin1String(context) == QLatin1String("SettingsCatalog") &&
            QLatin1String(sourceText) == QLatin1String("Export")) {
            return QStringLiteral("Translated export button");
        }
        return {};
    }
};

class RecordingSettingsBackend final : public settings::SettingsBackend {
  public:
    QVariant selectValue(settings::SettingsSelectBinding) const override {
        return {};
    }
    QVector<settings::SettingsRuntimeOption>
    dynamicSelectOptions(settings::SettingsSelectBinding) const override {
        return {};
    }
    bool applySelectValue(settings::SettingsSelectBinding, const QVariant&) override {
        return false;
    }
    bool switchValue(settings::SettingsSwitchBinding) const override {
        return false;
    }
    bool switchEnabled(settings::SettingsSwitchBinding) const override {
        return true;
    }
    bool applySwitchValue(settings::SettingsSwitchBinding, bool) override {
        return false;
    }
    QVariantList multiSelectValue(settings::SettingsMultiSelectBinding) const override {
        return {};
    }
    bool applyMultiSelectValue(settings::SettingsMultiSelectBinding, const QVariantList&) override {
        return false;
    }
    int integerValue(settings::SettingsIntegerBinding) const override {
        return 0;
    }
    bool applyIntegerValue(settings::SettingsIntegerBinding, int) override {
        return false;
    }
    int sliderValue(settings::SettingsSliderBinding) const override {
        return 0;
    }
    bool applySliderValue(settings::SettingsSliderBinding, int) override {
        return false;
    }
    QColor colorValue(settings::SettingsColorBinding) const override {
        return {};
    }
    bool applyColorValue(settings::SettingsColorBinding, const QColor&) override {
        return false;
    }
    QVariant radioValue(settings::SettingsRadioBinding) const override {
        return {};
    }
    bool applyRadioValue(settings::SettingsRadioBinding, const QVariant&) override {
        return false;
    }
    QString filePathValue(settings::SettingsFilePathBinding) const override {
        return {};
    }
    bool applyFilePathValue(settings::SettingsFilePathBinding, const QString&) override {
        return false;
    }
    QString directoryPathValue(settings::SettingsDirectoryPathBinding) const override {
        return {};
    }
    bool applyDirectoryPathValue(settings::SettingsDirectoryPathBinding, const QString&) override {
        return false;
    }
    QString textValue(settings::SettingsTextBinding) const override {
        return {};
    }
    bool applyTextValue(settings::SettingsTextBinding, const QString&) override {
        return false;
    }
    storage::ScreenshotToolbarLayout
    toolbarLayout(storage::ScreenshotToolbarLayoutKind) const override {
        return {};
    }
    bool applyToolbarLayout(storage::ScreenshotToolbarLayoutKind,
                            const storage::ScreenshotToolbarLayout&) override {
        return false;
    }
    presentation::GlobalShortcutRegistrationState
    shortcutState(presentation::GlobalShortcutAction) const override {
        return {};
    }
    presentation::GlobalShortcutValidationResult
    validateShortcut(presentation::GlobalShortcutAction,
                     const snow_shot::shortcuts::ShortcutBinding&) const override {
        return {};
    }
    bool applyShortcuts(presentation::GlobalShortcutAction,
                        const snow_shot::shortcuts::ShortcutBindingList&) override {
        return false;
    }
    snow_shot::shortcuts::ShortcutBindingList localShortcuts(settings::SettingsLocalShortcutScope,
                                                             const QString&) const override {
        return {};
    }
    presentation::GlobalShortcutValidationResult
    validateLocalShortcut(settings::SettingsLocalShortcutScope, const QString&,
                          const snow_shot::shortcuts::ShortcutBinding&) const override {
        return {};
    }
    bool applyLocalShortcuts(settings::SettingsLocalShortcutScope, const QString&,
                             const snow_shot::shortcuts::ShortcutBindingList&) override {
        return false;
    }
    settings::SettingsActionState
    actionState(settings::SettingsActionBinding binding) const override {
        if ((binding == settings::SettingsActionBinding::ExportConfiguration ||
             binding == settings::SettingsActionBinding::ImportConfiguration) &&
            m_configurationBusy) {
            return {false, true};
        }
        return {true, false};
    }
    bool triggerAction(settings::SettingsActionBinding binding,
                       const QString& filePath = {}) override {
        m_triggeredActions.push_back(binding);
        if (!filePath.isEmpty()) {
            m_importPaths.push_back(filePath);
        }
        return true;
    }
    storage::StorageStatus storageStatus() const override {
        storage::StorageStatus status;
        status.writeAvailable = true;
        status.effectiveMode = storage::StorageMode::ApplicationData;
        status.effectiveDirectory = QStringLiteral("C:/configuration-transfer-tests");
        return status;
    }
    bool resetSection(settings::SettingsSectionReset) override {
        return false;
    }

    void setConfigurationBusy(bool busy) {
        m_configurationBusy = busy;
        emit synchronized();
    }

    const QVector<settings::SettingsActionBinding>& triggeredActions() const {
        return m_triggeredActions;
    }
    const QStringList& importPaths() const {
        return m_importPaths;
    }

  private:
    QVector<settings::SettingsActionBinding> m_triggeredActions;
    QStringList m_importPaths;
    bool m_configurationBusy = false;
};

void configurationItemsRenderAsButtons() {
    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    RecordingSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("storage-and-privacy"), session);
    page.resize(960, 480);

    QWidget* const exportRow =
        page.findChild<QWidget*>(QStringLiteral("settings-item-configuration-export"));
    require(exportRow != nullptr, "the export configuration row must be rendered");
    QWidget* const importRow =
        page.findChild<QWidget*>(QStringLiteral("settings-item-configuration-import"));
    require(importRow != nullptr, "the import configuration row must be rendered");

    auto* const exportButton = page.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("settings-control-configuration-export"));
    require(exportButton != nullptr, "export configuration must render an action button");
    require(exportButton->text() == QStringLiteral("Export"),
            "export configuration must carry its button text");
    require(exportButton->toolTip().isEmpty(),
            "action buttons must not carry tooltips without a backend hint");
    require(exportButton->accessibleName() == QStringLiteral("Export configuration"),
            "export configuration must expose the item title as the accessible name");
    require(adqt::icons::describeIcon(exportButton->iconRef()).key.pack ==
                    QStringLiteral("snow-shot") &&
                adqt::icons::describeIcon(exportButton->iconRef()).key.name ==
                    QStringLiteral("export-configuration"),
            "export configuration must use the package icon");

    auto* const importButton = page.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("settings-control-configuration-import"));
    require(importButton != nullptr, "import configuration must render an action button");
    require(importButton->text() == QStringLiteral("Import"),
            "import configuration must carry its button text");
    require(importButton->toolTip().isEmpty(),
            "action buttons must not carry tooltips without a backend hint");
    require(importButton->accessibleName() == QStringLiteral("Import configuration"),
            "import configuration must expose the item title as the accessible name");
    require(adqt::icons::describeIcon(importButton->iconRef()).key.pack ==
                    QStringLiteral("snow-shot") &&
                adqt::icons::describeIcon(importButton->iconRef()).key.name ==
                    QStringLiteral("import-configuration"),
            "import configuration must use the import-config icon");

    exportButton->click();
    require(backend.triggeredActions().size() == 1 &&
                backend.triggeredActions().front() ==
                    settings::SettingsActionBinding::ExportConfiguration,
            "clicking export configuration must trigger the backend action");
}

void configurationBusyStateDisablesBothButtons() {
    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    RecordingSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("storage-and-privacy"), session);

    auto* const exportButton = page.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("settings-control-configuration-export"));
    auto* const importButton = page.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("settings-control-configuration-import"));
    require(exportButton != nullptr && importButton != nullptr,
            "configuration buttons must exist for busy-state checks");
    require(exportButton->isEnabled() && importButton->isEnabled(),
            "configuration buttons must start enabled");

    backend.setConfigurationBusy(true);
    flushEvents();
    require(!exportButton->isEnabled() && !importButton->isEnabled(),
            "an in-flight configuration transfer must disable both buttons");

    backend.setConfigurationBusy(false);
    flushEvents();
    require(exportButton->isEnabled() && importButton->isEnabled(),
            "finishing a configuration transfer must re-enable both buttons");
}

void buttonTextsRetranslate() {
    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    RecordingSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("storage-and-privacy"), session);

    auto* const exportButton = page.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("settings-control-configuration-export"));
    require(exportButton != nullptr, "export configuration must render an action button");

    SettingsCatalogButtonTranslator translator;
    QCoreApplication::installTranslator(&translator);
    page.retranslateUi();
    require(exportButton->text() == QStringLiteral("Translated export button"),
            "button texts must follow language changes");
    QCoreApplication::removeTranslator(&translator);
    page.retranslateUi();
    require(exportButton->text() == QStringLiteral("Export"),
            "removing the translator must restore the source button text");
}

void sessionDelegatesConfigurationImports() {
    const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
    RecordingSettingsBackend backend;
    settings::SettingsRuntimeSession session(registry, backend);

    const QString archivePath = QStringLiteral("C:/configuration-transfer-tests/export.zip");
    require(
        session.triggerAction(settings::SettingsActionBinding::ImportConfiguration, archivePath),
        "importing a configuration must be delegated to the backend");
    require(backend.importPaths() == QStringList{archivePath},
            "the session must forward the selected archive path unchanged");
}

void realBackendImportReplacesConfiguration() {
    presentation::GlobalShortcutManager shortcutManager;
    settings::BuiltInSettingsBackend backend(shortcutManager);

    QString archivedKey;
    QJsonValue archivedValue;
    QString replacedKey;
    QJsonValue replacedValue;
    for (const storage::ConfigurationSchemaEntry& entry : storage::ConfigurationSchema::entries()) {
        if (entry.key == QStringLiteral("storage/schema_version") ||
            entry.valueKind != storage::ConfigurationValueKind::Boolean) {
            continue;
        }
        const QJsonValue flipped = !entry.defaultValue.toBool();
        if (archivedKey.isEmpty()) {
            archivedKey = entry.key;
            archivedValue = flipped;
        } else {
            replacedKey = entry.key;
            replacedValue = flipped;
            break;
        }
    }
    require(!archivedKey.isEmpty() && !replacedKey.isEmpty(),
            "the schema must expose at least two boolean settings");

    storage::ConfigurationStore& store = storage::ApplicationStorage::instance().configuration();
    require(store.setValue(archivedKey, archivedValue) &&
                store.setValue(replacedKey, replacedValue),
            "seeding local settings must succeed");

    QTemporaryDir temporary;
    require(temporary.isValid(), "temporary directory unavailable for import fixtures");
    const QString archivePath = temporary.filePath(QStringLiteral("partial.zip"));
    require(
        storage::ConfigurationArchive::write(archivePath, {{archivedKey, archivedValue}},
                                             storage::ConfigurationStore::currentSchemaVersion())
            .isEmpty(),
        "writing a partial configuration archive must succeed");

    require(
        backend.triggerAction(settings::SettingsActionBinding::ImportConfiguration, archivePath),
        "the built-in backend must accept a valid archive");
    require(store.value(archivedKey) == archivedValue,
            "values carried by the archive must be applied");
    require(store.value(replacedKey) == storage::ConfigurationSchema::defaultValue(replacedKey),
            "settings absent from the archive must revert to schema defaults");
    require(store.value(QStringLiteral("storage/schema_version")).toInt() ==
                storage::ConfigurationStore::currentSchemaVersion(),
            "the store-managed schema version must survive imports");
    require(backend.actionState(settings::SettingsActionBinding::ImportConfiguration).enabled,
            "finishing an import must release the busy state");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("configuration-transfer-widget-tests"));

    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "temporary storage directory should be available");
    static_cast<void>(storage::ApplicationStorage::instance().initialize(
        {storageDirectory.path(), storageDirectory.path(), 8000}));

    {
        const QString previewDirectory = qEnvironmentVariable("SNOW_CONFIG_TEST_SCREENSHOT_DIR");
        if (!previewDirectory.isEmpty()) {
            const settings::SettingsRegistry& registry = settings::builtInSettingsRegistry();
            RecordingSettingsBackend backend;
            settings::SettingsRuntimeSession session(registry, backend);
            SettingsPageWidget page(registry, QStringLiteral("storage-and-privacy"), session);
            page.resize(960, 1600);
            page.show();
            flushEvents();
            QWidget* const exportRow =
                page.findChild<QWidget*>(QStringLiteral("settings-item-configuration-export"));
            require(exportRow != nullptr && QDir().mkpath(previewDirectory),
                    "preview fixture unavailable");
            exportRow->grab().save(
                QDir(previewDirectory).filePath(QStringLiteral("config-export-row.png")));
            exportRow->window()->grab().save(
                QDir(previewDirectory).filePath(QStringLiteral("config-export-page.png")));
            page.hide();
        }
    }

    configurationItemsRenderAsButtons();
    configurationBusyStateDisablesBothButtons();
    buttonTextsRetranslate();
    sessionDelegatesConfigurationImports();
    realBackendImportReplacesConfiguration();

    storage::ApplicationStorage::instance().shutdown();
    return 0;
}
