#include "shell_context_menu_test_registry.h"
#include "snow_shot/app/distribution.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/platform/windows/packagedcontextmenupreference.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
using namespace shell_context_menu_tests;
namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

const QString kKey = QStringLiteral("system/context_menu_mode");
const QString kId = QStringLiteral("system.context-menu-mode");
const QString kShortcutKey = QStringLiteral("context_menu_shortcuts/edit_with_snow_shot");
const QString kShortcutId = QStringLiteral("edit_with_snow_shot");
constexpr auto kScope = settings::SettingsLocalShortcutScope::WindowsContextMenu;
constexpr auto kBinding = settings::SettingsSelectBinding::ContextMenuMode;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void catalogAndDefaults(const QString& configurationPath) {
    const auto* schema = storage::ConfigurationSchema::entry(kKey);
    require(schema && schema->defaultValue == QStringLiteral("image_files") &&
                schema->valueKind == storage::ConfigurationValueKind::String &&
                schema->allowedStringValues ==
                    QStringList{QStringLiteral("image_files"), QStringLiteral("disabled")},
            "shared schema defaults to Image and document files with two exact string values");
    const auto& registry = settings::builtInSettingsRegistry();
    require(registry.isValid(), "the context menu setting preserves valid catalog bindings");
    const auto* field = registry.field(kId);
    const settings::SettingsSearchIndex search(registry);
    const auto* shortcutField = registry.fieldForLocalShortcut(kScope, kShortcutId);
    const auto* shortcutSchema = storage::ConfigurationSchema::entry(kShortcutKey);
    require(shortcutSchema && shortcutSchema->defaultValue == QJsonArray{} &&
                shortcutSchema->valueKind == storage::ConfigurationValueKind::ShortcutList &&
                shortcutSchema->maximumListItems == 1,
            "menu access keys default to an empty shortcut list with one allowed assignment");
#ifdef Q_OS_WIN
    require(
        shortcutField && shortcutField->pageId == QStringLiteral("shortcuts-mouse") &&
            shortcutField->sectionId == QStringLiteral("windows-context-menu-shortcuts") &&
            shortcutField->configurationKey == kShortcutKey &&
            shortcutField->reset == settings::SettingsSectionReset::WindowsContextMenuShortcuts &&
            std::get<settings::SettingsLocalShortcutDefinition>(shortcutField->definition->payload)
                    .maximumShortcutCount == 1 &&
            shortcutField->definition->title.translated() ==
                (snow_shot::app::edition::isMini ? QStringLiteral("Edit with Snow Shot Mini")
                                                 : QStringLiteral("Edit with Snow Shot")) &&
            registry.catalog().page(shortcutField->pageId)->sections.last().id ==
                shortcutField->sectionId &&
            !search.search(QStringLiteral("Edit with Snow Shot")).isEmpty(),
        "Windows exposes the searchable edition-aware recorder in the final shortcut category");
    require(field && field->configurationKey == kKey &&
                field->pageId == QStringLiteral("general") &&
                field->sectionId == QStringLiteral("system-general") &&
                field->reset == settings::SettingsSectionReset::SystemGeneral &&
                field->defaultValue == QStringLiteral("image_files") &&
                registry.fieldForSelect(kBinding) == field &&
                field->definition->title.translated() == QStringLiteral("Windows context menu"),
            "Windows exposes the dropdown in General's system integration section");
    const auto& select = std::get<settings::SettingsSelectDefinition>(field->definition->payload);
    require(select.source == settings::SettingsSelectSource::Fixed && select.options.size() == 2 &&
                select.options[0].value == QStringLiteral("image_files") &&
                select.options[0].label.translated() ==
                    QStringLiteral("Image and document files") &&
                select.options[1].value == QStringLiteral("disabled") &&
                select.options[1].label.translated() == QStringLiteral("Disabled") &&
                !search.search(QStringLiteral("Windows context menu")).isEmpty(),
            "dropdown uses the requested option order, labels and searchable title");
    const auto* section = registry.catalog().section(field->pageId, field->sectionId);
    require(section && section->items[3].id == kId &&
                section->items[2].id == QStringLiteral("system.restart-as-administrator"),
            "context menu dropdown follows the adjacent administrator startup controls");
#else
    require(!shortcutField && !field && !registry.fieldForSelect(kBinding) &&
                search.search(QStringLiteral("Windows context menu")).isEmpty(),
            "other platforms omit the Windows context menu control and search entry");
#endif
    QFile file(configurationPath);
    require(file.open(QIODevice::WriteOnly), "create an existing configuration fixture");
    const QJsonObject document{
        {QStringLiteral("storage"), QJsonObject{{QStringLiteral("schema_version"),
                                                 storage::ConfigurationSchema::currentVersion()}}},
        {QStringLiteral("system"),
         QJsonObject{{QStringLiteral("application_priority"), QStringLiteral("normal")}}},
    };
    const auto bytes = QJsonDocument(document).toJson();
    require(file.write(bytes) == bytes.size(),
            "write a configuration without a context menu preference");
    file.close();
    storage::ConfigurationStore store(configurationPath, true, true, 60000);
    require(store.value(kKey) == QStringLiteral("image_files") &&
                store.value(kShortcutKey) == QJsonArray{} &&
                store.value(QStringLiteral("system/application_priority")) ==
                    QStringLiteral("normal"),
            "old configurations acquire Image and document files without changing existing "
            "preferences");
    require(store.applySnapshot({{kKey, QStringLiteral("disabled")}}) &&
                store.value(kKey) == QStringLiteral("disabled") && store.applySnapshot({}) &&
                store.value(kKey) == QStringLiteral("image_files") &&
                store.applySnapshot({{kKey, QStringLiteral("unknown")}}) &&
                store.value(kKey) == QStringLiteral("image_files"),
            "imports preserve Disabled and default missing or invalid preferences to Image and "
            "document files");
    const auto revision = store.revision();
    require(!store.setValue(kKey, false) && !store.setValue(kKey, QStringLiteral("unknown")) &&
                store.revision() == revision,
            "direct writes reject non-string and unsupported context menu preferences");
}

void accessKeySchemaValidation() {
    using snow_shot::shortcuts::ShortcutBinding;
    using snow_shot::shortcuts::shortcutBindingsToJson;
    ShortcutBinding physical{QStringLiteral("E")};
    physical.physicalKeys.insert(snow_shot::shortcuts::ShortcutPlatform::MacOS, 14);
    for (const auto& value : {QJsonArray{QStringLiteral("e")}, shortcutBindingsToJson({physical}),
                              QJsonArray{QStringLiteral("7")}, QJsonArray{}}) {
        const auto normalized = storage::ConfigurationSchema::normalize(kShortcutKey, value);
        require(normalized.valid, "valid logical access keys normalize successfully");
        const auto keys = snow_shot::shortcuts::shortcutBindingsFromJson(normalized.value, false);
        require(keys.isEmpty() || (keys.size() == 1 && keys.first().physicalKeys.isEmpty() &&
                                   (keys.first().portableText == QStringLiteral("E") ||
                                    keys.first().portableText == QStringLiteral("7"))),
                "normalization uppercases letters and strips physical keyboard metadata");
    }
    for (const auto& value :
         {QJsonValue(QStringLiteral("E")), QJsonValue(QJsonArray{42}),
          QJsonValue(QJsonArray{QStringLiteral("Ctrl+E")}),
          QJsonValue(QJsonArray{QStringLiteral("Shift+E")}),
          QJsonValue(QJsonArray{QStringLiteral("Alt+E")}),
          QJsonValue(QJsonArray{QStringLiteral("Meta+E")}),
          QJsonValue(QJsonArray{QStringLiteral("F1")}), QJsonValue(QJsonArray{QStringLiteral(";")}),
          QJsonValue(QJsonArray{QStringLiteral("雪")}),
          QJsonValue(QJsonArray{QStringLiteral("E"), QStringLiteral("S")})})
        require(!storage::ConfigurationSchema::normalize(kShortcutKey, value).valid,
                "schema rejects malformed, modified, non-alphanumeric and multiple access keys");
}

void settingsTransactions(const QString& configurationDirectory, const QString& blockedDirectory) {
    Registry registry;
    windows::ShellContextMenuRegistration registration(registry.options());
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    int startupApplications = 0;
    const auto applyStartup = [&](bool enabled, bool elevated) {
        ++startupApplications;
        const bool saved =
            configuration.setValues({{QStringLiteral("system/auto_start_at_boot"), enabled},
                                     {QStringLiteral("system/launch_as_administrator"), elevated}});
        return windows::AdministratorResult{saved, false, {}};
    };
    settings::BuiltInSettingsBackend backend(shortcuts, nullptr, nullptr, nullptr, nullptr,
                                             &registration, applyStartup);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
#ifdef Q_OS_WIN
    require(backend.selectValue(kBinding) == QStringLiteral("image_files") &&
                session.state(kId).acceptedValue == QStringLiteral("image_files") &&
                registration.setEnabled(true) && registry.keys.size() == 2,
            "first launch applies the default Image and document files mode");
    require(
        session.applySelectValue(kBinding, QStringLiteral("disabled")) && registry.keys.isEmpty() &&
            configuration.value(kKey) == QStringLiteral("disabled") && !session.hasDirtyFields(),
        "selecting Disabled immediately removes native entries and commits the preference");
    storage::ConfigurationStore persisted(configurationDirectory + QStringLiteral("/config.json"),
                                          true, true, 60000);
    require(persisted.value(kKey) == QStringLiteral("disabled"),
            "Disabled survives an immediate configuration reload");
    const auto revision = configuration.revision();
    require(!backend.applySelectValue(kBinding, false) &&
                !backend.applySelectValue(kBinding, QStringLiteral("unknown")) &&
                configuration.revision() == revision && registry.keys.isEmpty(),
            "invalid settings values have no configuration or registry side effects");
    registry.writesBeforeFailure = 1;
    QString operationError;
    QObject::connect(&backend, &settings::SettingsBackend::operationMessage, &session,
                     [&](const QString& error, bool) { operationError = error; });
    require(!backend.applySelectValue(kBinding, QStringLiteral("image_files")) &&
                configuration.value(kKey) == QStringLiteral("disabled") &&
                registry.keys.isEmpty() &&
                operationError == QStringLiteral("injected write failure"),
            "native failure rolls back entries and preserves the committed dropdown preference");
    require(session.applySelectValue(kBinding, QStringLiteral("image_files")) &&
                registry.keys.size() == 2,
            "the mode can be retried after a failed native registration");
    const auto enabled = registry.keys;
    configuration.relocate(blockedDirectory);
    require(!backend.applySelectValue(kBinding, QStringLiteral("disabled")) &&
                configuration.value(kKey) == QStringLiteral("image_files") &&
                registry.keys == enabled,
            "failed preference flush restores native entries and the previous in-memory value");
    configuration.relocate(configurationDirectory);
    require(configuration.flushNow().success, "persist recovered context menu preference");
    auto imported = configuration.snapshot();
    imported.insert(kKey, QStringLiteral("disabled"));
    require(backend.importConfigurationSnapshot(imported,
                                                storage::ConfigurationSchema::currentVersion()) &&
                registry.keys.isEmpty() && configuration.value(kKey) == QStringLiteral("disabled"),
            "importing Disabled immediately removes owned context menu entries");
    imported.remove(kKey);
    require(backend.importConfigurationSnapshot(imported,
                                                storage::ConfigurationSchema::currentVersion()) &&
                registry.keys.size() == 2 &&
                configuration.value(kKey) == QStringLiteral("image_files"),
            "an omitted imported preference immediately restores the default native mode");
    registry.writesBeforeFailure = 0;
    imported = configuration.snapshot();
    imported.insert(kKey, QStringLiteral("disabled"));
    require(!backend.importConfigurationSnapshot(imported,
                                                 storage::ConfigurationSchema::currentVersion()) &&
                registry.keys.size() == 2 &&
                configuration.value(kKey) == QStringLiteral("image_files"),
            "failed native import retains the stored preference from before import");
    require(session.applySelectValue(kBinding, QStringLiteral("disabled")) &&
                session.reset(settings::SettingsSectionReset::SystemGeneral) &&
                registry.keys.size() == 2 &&
                configuration.value(kKey) == QStringLiteral("image_files") &&
                startupApplications == 1,
            "resetting Startup and updates restores the default native menu through isolated "
            "startup injection");
    require(session.applySelectValue(kBinding, QStringLiteral("disabled")),
            "prepare a rejected reset");
    configuration.suspendWrites(true);
    const auto before = configuration.snapshot();
    require(!backend.resetSection(settings::SettingsSectionReset::SystemGeneral) &&
                configuration.snapshot() == before && registry.keys.isEmpty(),
            "a rejected reset cannot leave a new registry entry or changed preference");
    configuration.suspendWrites(false);
    require(backend.applySelectValue(kBinding, QStringLiteral("image_files")),
            "prepare translated native labels");
    auto& language = snow_shot::presentation::LanguageManager::instance();
    require(language.setLanguage(QStringLiteral("en_US")), "select English language");
    const auto png = Registry::path(QStringLiteral("png"));
    const QString english = stringText(registry.keys[png].values.value({}));
    for (const auto& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        require(language.setLanguage(locale) &&
                    stringText(registry.keys[png].values.value({})) != english,
                "language changes immediately retranslate an existing context menu label");
    }
    require(language.setLanguage(QStringLiteral("en_US")), "restore English language");
#else
    Q_UNUSED(configurationDirectory);
    Q_UNUSED(blockedDirectory);
    const auto before = configuration.snapshot();
    require(!backend.applySelectValue(kBinding, QStringLiteral("disabled")) &&
                configuration.snapshot() == before && registry.keys.isEmpty(),
            "other platforms reject context menu changes without side effects");
#endif
}

void portableSettingsTransactions(const QString& root) {
    const QString executable = QDir(root).filePath(QStringLiteral("portable-bin"));
    require(QDir().mkpath(executable), "create an isolated portable executable directory");
    QFile marker(QDir(executable).filePath(snow_shot::app::edition::portableMarkerName()));
    require(marker.open(QIODevice::WriteOnly) && marker.write("data") == 4,
            "create the edition-specific portable marker");
    marker.close();
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(
        applicationStorage
                .initialize({executable, QDir(root).filePath(QStringLiteral("fallback")), 60000})
                .success &&
            applicationStorage.status().effectiveMode == storage::StorageMode::Portable,
        "initialize portable settings through the storage marker");
    auto& configuration = applicationStorage.configuration();
    const QString enabledKey = QStringLiteral("system/auto_start_at_boot");
    Registry registry;
    windows::ShellContextMenuRegistration registration(registry.options());
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(
        shortcuts, nullptr, nullptr, nullptr, nullptr, &registration,
        [&](bool enabled, bool elevated) {
            return windows::AdministratorResult{
                configuration.setValues(
                    {{enabledKey, enabled},
                     {QStringLiteral("system/launch_as_administrator"), elevated}}),
                false,
                {}};
        });
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    const auto startupBinding = settings::SettingsSwitchBinding::AutoStartAtBoot;
    require(!backend.switchValue(startupBinding) &&
                session.state(kId).acceptedValue == QStringLiteral("disabled") &&
                registration.setEnabled(false) && registry.keys.isEmpty(),
            "portable settings expose both integrations as disabled on first launch");
    const auto enableIntegrations = [&] {
        return session.applySwitchValue(startupBinding, true) &&
               session.applySelectValue(kBinding, QStringLiteral("image_files")) &&
               backend.switchValue(startupBinding) && registry.keys.size() == 2;
    };
    require(enableIntegrations(), "portable settings allow explicitly enabling both integrations");
    require(session.reset(settings::SettingsSectionReset::SystemGeneral) &&
                !backend.switchValue(startupBinding) && registry.keys.isEmpty() &&
                session.state(kId).acceptedValue == QStringLiteral("disabled") &&
                !session.hasDirtyFields(),
            "portable General reset disables both native integrations and accepts the defaults");
    require(enableIntegrations(), "prepare enabled integrations for a portable import");
    auto imported = configuration.snapshot();
    imported.remove(enabledKey);
    imported.remove(kKey);
    require(backend.importConfigurationSnapshot(imported,
                                                storage::ConfigurationSchema::currentVersion()) &&
                !backend.switchValue(startupBinding) && registry.keys.isEmpty() &&
                configuration.value(kKey) == QStringLiteral("disabled"),
            "omitted imported preferences restore disabled portable native integrations");
    applicationStorage.shutdown();
}

void accessKeyTransactions(const QString& configurationDirectory, const QString& blockedDirectory) {
    using snow_shot::shortcuts::ShortcutBinding;
    using snow_shot::shortcuts::ShortcutBindingList;
    using snow_shot::shortcuts::shortcutBindingsToJson;
    Registry registry;
    windows::ShellContextMenuRegistration registration(registry.options());
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts, nullptr, nullptr, nullptr, nullptr,
                                             &registration);
    settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
    auto& configuration = storage::ApplicationStorage::instance().configuration();
#ifdef Q_OS_WIN
    const ShortcutBindingList e{ShortcutBinding{QStringLiteral("E")}};
    const ShortcutBindingList digit{ShortcutBinding{QStringLiteral("7")}};
    const auto label = [&] {
        return stringText(
            registry.keys.value(Registry::path(QStringLiteral("png"))).values.value({}));
    };
    require(backend.localShortcuts(kScope, kShortcutId).isEmpty() && registration.setEnabled(true),
            "prepare the default unassigned menu");
    ShortcutBinding physical{QStringLiteral("e")};
    physical.physicalKeys.insert(snow_shot::shortcuts::ShortcutPlatform::MacOS, 14);
    require(session.applyLocalShortcuts(kScope, kShortcutId, {physical}) &&
                session.localShortcuts(kScope, kShortcutId) == e &&
                label().endsWith(QStringLiteral(" (&E)")) && !session.hasDirtyFields(),
            "recorder commits canonical logical keys immediately to settings and native labels");
    storage::ConfigurationStore persisted(configurationDirectory + QStringLiteral("/config.json"),
                                          true, true, 60000);
    require(persisted.value(kShortcutKey) == shortcutBindingsToJson(e),
            "access keys survive an immediate configuration reload");
    for (const auto& invalid :
         {QStringLiteral("Ctrl+E"), QStringLiteral("Shift+E"), QStringLiteral("Alt+E"),
          QStringLiteral("Meta+E"), QStringLiteral("F1"), QStringLiteral(";")}) {
        const auto before = configuration.snapshot();
        const auto nativeBefore = registry.keys;
        require(!backend.validateLocalShortcut(kScope, kShortcutId, ShortcutBinding{invalid})
                        .supported &&
                    !backend.applyLocalShortcuts(kScope, kShortcutId, {ShortcutBinding{invalid}}) &&
                    configuration.snapshot() == before && registry.keys == nativeBefore,
                "invalid recorder assignments have no configuration or registry side effects");
    }
    require(!backend.applyLocalShortcuts(kScope, kShortcutId, {e.first(), digit.first()}) &&
                !backend.applyLocalShortcuts(kScope, QStringLiteral("unknown"), e),
            "the backend rejects extra keys and unknown commands");
    const auto nativeBefore = registry.keys;
    registry.writesBeforeFailure = 1;
    require(!session.applyLocalShortcuts(kScope, kShortcutId, digit) &&
                registry.keys == nativeBefore && backend.localShortcuts(kScope, kShortcutId) == e,
            "a partial registry failure preserves the accepted access key");
    configuration.relocate(blockedDirectory);
    require(!backend.applyLocalShortcuts(kScope, kShortcutId, digit) &&
                registry.keys == nativeBefore && backend.localShortcuts(kScope, kShortcutId) == e,
            "a failed configuration flush restores native labels and the in-memory assignment");
    configuration.relocate(configurationDirectory);
    require(configuration.flushNow().success, "recover preference persistence");
    require(session.applySelectValue(kBinding, QStringLiteral("disabled")) &&
                session.applyLocalShortcuts(kScope, kShortcutId, digit) && registry.keys.isEmpty(),
            "users can save an access key while the context menu is disabled");
    auto imported = configuration.snapshot();
    imported.insert(kKey, QStringLiteral("image_files"));
    imported.insert(kShortcutKey, shortcutBindingsToJson(e));
    registry.writesBeforeFailure = 1;
    require(!backend.importConfigurationSnapshot(imported,
                                                 storage::ConfigurationSchema::currentVersion()) &&
                registry.keys.isEmpty() &&
                configuration.value(kKey) == QStringLiteral("disabled") &&
                backend.localShortcuts(kScope, kShortcutId) == digit,
            "failed imports roll back menu mode and access key together");
    require(backend.importConfigurationSnapshot(imported,
                                                storage::ConfigurationSchema::currentVersion()) &&
                label().endsWith(QStringLiteral(" (&E)")) &&
                backend.localShortcuts(kScope, kShortcutId) == e,
            "successful imports apply menu mode and access key in one native transaction");
    auto& language = snow_shot::presentation::LanguageManager::instance();
    const QString english = label();
    for (const auto& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")})
        require(language.setLanguage(locale) && label() != english &&
                    label().endsWith(QStringLiteral(" (&E)")),
                "language changes translate the label while preserving its assigned access key");
    require(language.setLanguage(QStringLiteral("en_US")), "restore English");
    registry.writesBeforeFailure = 1;
    require(!session.reset(settings::SettingsSectionReset::WindowsContextMenuShortcuts) &&
                label().endsWith(QStringLiteral(" (&E)")),
            "a failed category reset preserves the saved access key");
    require(session.reset(settings::SettingsSectionReset::WindowsContextMenuShortcuts) &&
                session.localShortcuts(kScope, kShortcutId).isEmpty() &&
                !label().contains(QLatin1Char('&')) &&
                configuration.value(kKey) == QStringLiteral("image_files"),
            "category reset clears only the shortcut and removes its menu suffix");
    require(session.applyLocalShortcuts(kScope, kShortcutId, digit) &&
                session.applyLocalShortcuts(kScope, kShortcutId, {}) &&
                !label().contains(QLatin1Char('&')),
            "clearing an assignment removes the mnemonic");
    for (const bool missing : {false, true}) {
        require(session.applyLocalShortcuts(kScope, kShortcutId, e),
                "prepare legacy or invalid import");
        imported = configuration.snapshot();
        if (missing)
            imported.remove(kShortcutKey);
        else
            imported.insert(kShortcutKey, QJsonArray{QStringLiteral("Ctrl+E")});
        require(backend.importConfigurationSnapshot(
                    imported, storage::ConfigurationSchema::currentVersion()) &&
                    backend.localShortcuts(kScope, kShortcutId).isEmpty() &&
                    !label().contains(QLatin1Char('&')),
                "missing or invalid imported access keys use the unassigned default");
    }
#else
    Q_UNUSED(configurationDirectory);
    Q_UNUSED(blockedDirectory);
    const auto before = configuration.snapshot();
    require(
        !backend.applyLocalShortcuts(kScope, kShortcutId, {ShortcutBinding{QStringLiteral("E")}}) &&
            configuration.snapshot() == before && registry.keys.isEmpty(),
        "other platforms reject native access key changes without side effects");
#endif
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "create isolated settings test storage");
    if (snow_shot::app::distribution::isStoreManaged()) {
        using snow_shot::shortcuts::ShortcutBinding;
        using snow_shot::shortcuts::ShortcutBindingList;
        using snow_shot::shortcuts::shortcutBindingsToJson;
        const ShortcutBindingList e{ShortcutBinding{QStringLiteral("E")}};
        const ShortcutBindingList digit{ShortcutBinding{QStringLiteral("7")}};
        auto& storage = storage::ApplicationStorage::instance();
        require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
                "initialize isolated Store settings");
        Registry registry;
        auto options = registry.options();
        options.packagedPreferencePath = temporary.filePath(QStringLiteral("context-menu-enabled"));
        const auto menuAccessKey = [&] {
            return windows::packaged_context_menu::readPreference(
                       options.packagedPreferencePath.toStdWString())
                .accessKey;
        };
        const auto menuEnabled = [&] {
            return windows::packaged_context_menu::isEnabled(
                options.packagedPreferencePath.toStdWString());
        };
        windows::ShellContextMenuRegistration shell(options);
        snow_shot::presentation::GlobalShortcutManager shortcuts;
        int nativeStartupChanges = 0;
        settings::BuiltInSettingsBackend backend(
            shortcuts, nullptr, nullptr, nullptr, nullptr, &shell, [&](bool, bool) {
                ++nativeStartupChanges;
                return windows::AdministratorResult{true, false, {}};
            });
        settings::SettingsRuntimeSession session(settings::builtInSettingsRegistry(), backend);
        const auto* field = settings::builtInSettingsRegistry().field(kId);
        require(field && backend.selectEnabled(kBinding) &&
                    !settings::SettingsSearchIndex(settings::builtInSettingsRegistry())
                         .search(QStringLiteral("Windows context menu"))
                         .isEmpty(),
                "Store exposes an enabled, searchable context menu setting");
        const auto* shortcutField =
            settings::builtInSettingsRegistry().fieldForLocalShortcut(kScope, kShortcutId);
        require(shortcutField && shortcutField->configurationKey == kShortcutKey &&
                    shortcutField->reset ==
                        settings::SettingsSectionReset::WindowsContextMenuShortcuts &&
                    std::get<settings::SettingsLocalShortcutDefinition>(
                        shortcutField->definition->payload)
                            .maximumShortcutCount == 1 &&
                    !settings::SettingsSearchIndex(settings::builtInSettingsRegistry())
                         .search(QStringLiteral("Edit with Snow Shot"))
                         .isEmpty(),
                "Store exposes the searchable classic menu access-key recorder and reset");
        require(session.applySelectValue(kBinding, QStringLiteral("disabled")) && !menuEnabled() &&
                    session.state(kId).acceptedValue == QStringLiteral("disabled") &&
                    !session.hasDirtyFields(),
                "Store Settings disables the Explorer command and accepts the saved value");
        storage::ConfigurationStore persisted(
            storage.configurationDirectory() + QStringLiteral("/config.json"), true, true, 60000);
        require(persisted.value(kKey) == QStringLiteral("disabled"),
                "Store context menu preference survives a configuration reload");
        require(session.applySelectValue(kBinding, QStringLiteral("image_files")) && menuEnabled(),
                "Store Settings reenables the Explorer command");
        require(!backend.applySelectValue(kBinding, false) &&
                    !backend.applySelectValue(kBinding, QStringLiteral("unknown")) && menuEnabled(),
                "Store rejects invalid context menu values");
        ShortcutBinding physical{QStringLiteral("e")};
        physical.physicalKeys.insert(snow_shot::shortcuts::ShortcutPlatform::MacOS, 14);
        require(session.applyLocalShortcuts(kScope, kShortcutId, {physical}) &&
                    session.localShortcuts(kScope, kShortcutId) == e && menuAccessKey() == L'E' &&
                    menuEnabled() && !session.hasDirtyFields(),
                "Store recorder canonicalizes and persists a logical access key for Explorer");
        storage::ConfigurationStore savedShortcut(
            storage.configurationDirectory() + QStringLiteral("/config.json"), true, true, 60000);
        require(savedShortcut.value(kShortcutKey) == shortcutBindingsToJson(e),
                "Store access keys survive a configuration reload");
        for (const auto& invalid : {QStringLiteral("Ctrl+E"), QStringLiteral("Alt+E"),
                                    QStringLiteral("F1"), QStringLiteral("&")})
            require(!backend.applyLocalShortcuts(kScope, kShortcutId, {ShortcutBinding{invalid}}) &&
                        menuAccessKey() == L'E' && backend.localShortcuts(kScope, kShortcutId) == e,
                    "Store rejects invalid menu access keys without changing the accepted key");
        require(!backend.applyLocalShortcuts(kScope, kShortcutId, {e.first(), digit.first()}) &&
                    !backend.applyLocalShortcuts(kScope, QStringLiteral("unknown"), e),
                "Store rejects multiple access keys and unknown commands");
        const auto previous = storage.configuration().snapshot();
        auto imported = previous;
        imported.insert(QStringLiteral("system/launch_as_administrator"), true);
        imported.insert(QStringLiteral("system/auto_start_at_boot"), true);
        imported.insert(kKey, QStringLiteral("disabled"));
        imported.insert(kShortcutKey, shortcutBindingsToJson(digit));
        imported.insert(QStringLiteral("updates/mode"), QStringLiteral("next_launch"));
        require(backend.importConfigurationSnapshot(
                    imported, storage::ConfigurationSchema::currentVersion()) &&
                    nativeStartupChanges == 0 && registry.writes == 0 &&
                    storage.configuration().value(kKey) == QStringLiteral("disabled") &&
                    !menuEnabled() && menuAccessKey() == L'7' &&
                    backend.localShortcuts(kScope, kShortcutId) == digit &&
                    storage.configuration().value(QStringLiteral("updates/mode")) ==
                        previous.value(QStringLiteral("updates/mode")) &&
                    !storage.configuration()
                         .value(QStringLiteral("system/launch_as_administrator"))
                         .toBool(),
                "Store imports apply context menu choices while preserving Windows-owned settings");
        require(backend.resetSection(settings::SettingsSectionReset::SystemGeneral) &&
                    nativeStartupChanges == 0 && registry.writes == 0 && menuEnabled() &&
                    menuAccessKey() == L'7' &&
                    storage.configuration().value(kKey) == QStringLiteral("image_files") &&
                    !backend.applySelectValue(settings::SettingsSelectBinding::UpdateMode,
                                              QStringLiteral("automatic")),
                "Store reset restores the menu default without changing OS-managed integrations");
        require(session.applySelectValue(kBinding, QStringLiteral("disabled")) &&
                    session.reset(settings::SettingsSectionReset::WindowsContextMenuShortcuts) &&
                    !menuEnabled() && menuAccessKey() == L'\0' &&
                    backend.localShortcuts(kScope, kShortcutId).isEmpty(),
                "Store shortcut reset clears the key without reenabling a disabled menu");
        require(session.applyLocalShortcuts(kScope, kShortcutId, e) && !menuEnabled() &&
                    menuAccessKey() == L'E' &&
                    session.applySelectValue(kBinding, QStringLiteral("image_files")) &&
                    menuEnabled() && menuAccessKey() == L'E',
                "Store can assign access keys while disabled and keeps them when reenabled");
        require(session.applyLocalShortcuts(kScope, kShortcutId, {}) && menuAccessKey() == L'\0' &&
                    menuEnabled() && session.applyLocalShortcuts(kScope, kShortcutId, e),
                "clearing a Store access key removes the mnemonic without changing menu mode");
        storage.configuration().suspendWrites(true);
        require(!session.applySelectValue(kBinding, QStringLiteral("disabled")) && menuEnabled() &&
                    storage.configuration().value(kKey) == QStringLiteral("image_files"),
                "read-only settings cannot change the Explorer preference");
        storage.configuration().suspendWrites(false);
        const auto originalDirectory = storage.configurationDirectory();
        QFile blocker(temporary.filePath(QStringLiteral("blocked")));
        require(blocker.open(QIODevice::WriteOnly), "create a configuration persistence blocker");
        blocker.close();
        storage.configuration().relocate(blocker.fileName());
        require(!session.applyLocalShortcuts(kScope, kShortcutId, digit) &&
                    menuAccessKey() == L'E' && backend.localShortcuts(kScope, kShortcutId) == e &&
                    menuEnabled() &&
                    !session.reset(settings::SettingsSectionReset::WindowsContextMenuShortcuts) &&
                    menuAccessKey() == L'E',
                "failed Store saves and resets roll back the native and accepted access key");
        auto failedImport = storage.configuration().snapshot();
        failedImport.insert(kKey, QStringLiteral("disabled"));
        failedImport.insert(kShortcutKey, shortcutBindingsToJson(digit));
        require(!backend.importConfigurationSnapshot(
                    failedImport, storage::ConfigurationSchema::currentVersion()) &&
                    menuEnabled() && menuAccessKey() == L'E' &&
                    backend.localShortcuts(kScope, kShortcutId) == e,
                "failed Store imports restore mode and access key together");
        require(!session.applySelectValue(kBinding, QStringLiteral("disabled")) && menuEnabled() &&
                    storage.configuration().value(kKey) == QStringLiteral("image_files"),
                "a failed configuration save rolls back both the Store dropdown and Explorer");
        storage.configuration().relocate(originalDirectory);
        storage.shutdown();
        return 0;
    }
    catalogAndDefaults(temporary.filePath(QStringLiteral("existing.json")));
    accessKeySchemaValidation();
    auto& applicationStorage = storage::ApplicationStorage::instance();
    require(applicationStorage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "initialize isolated application storage");
    snow_shot::presentation::LanguageManager::instance().initialize();
    require(applicationStorage.configuration().flushNow().success,
            "flush initial settings fixture");
    QFile blocker(temporary.filePath(QStringLiteral("blocked")));
    require(blocker.open(QIODevice::WriteOnly),
            "create a file that blocks configuration persistence");
    blocker.close();
    settingsTransactions(applicationStorage.configurationDirectory(), blocker.fileName());
    accessKeyTransactions(applicationStorage.configurationDirectory(), blocker.fileName());
    applicationStorage.shutdown();
    portableSettingsTransactions(temporary.path());
    std::cout << "Shell context menu settings tests passed\n";
    return 0;
}
