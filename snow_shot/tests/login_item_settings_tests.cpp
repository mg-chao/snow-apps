#include "snow_shot/platform/macos/loginitemservice.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsregistry.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/storage/applicationstorage.h"
#include <QApplication>
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
    QApplication app(argc, argv);
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({temporary.path(), temporary.path(), 60000}).success,
            "temporary storage");
    auto& language = snow_shot::presentation::LanguageManager::instance();
    language.initialize();
    namespace settings = snow_shot::presentation::settings;
    using namespace snow_shot::platform::macos;
    LoginItemSnapshot native{LoginItemStatus::Unregistered, {}};
    bool fail = false;
    bool save = true;
    int opened = 0;
    int changes = 0;
    LoginItemService service({[&] { return native; },
                              [&](bool enabled) -> LoginItemResult {
                                  ++changes;
                                  if (fail)
                                      return {false, QStringLiteral("native failure")};
                                  native.status = enabled ? LoginItemStatus::ApprovalRequired
                                                          : LoginItemStatus::Unregistered;
                                  return {};
                              },
                              [] { return true; }, [] { return true; }, [&](bool) { return save; },
                              [&] { ++opened; }});
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts, nullptr, nullptr, nullptr, &service);
    const auto binding = settings::SettingsSwitchBinding::AutoStartAtBoot;
    int pendingSignals = 0;
    QString error;
    QObject::connect(&backend, &settings::SettingsBackend::operationMessage, &app,
                     [&](const QString& value, bool) { error = value; });
    QObject::connect(&backend, &settings::SettingsBackend::synchronized, &app, [&] {
        if (service.pending()) {
            ++pendingSignals;
            require(backend.fieldPending(QStringLiteral("system.auto-start-at-boot")) &&
                        !backend.switchEnabled(binding) &&
                        !backend.actionState(settings::SettingsActionBinding::OpenLoginItemSettings)
                             .enabled,
                    "pending operation disables conflicting controls");
        }
    });
    require(backend.switchEnabled(binding) && !backend.switchValue(binding),
            "native unregistered state");
    require(backend.applySwitchValue(binding, true) && backend.switchValue(binding) &&
                pendingSignals > 0,
            "toggle registers through service and emits busy state");
    require(!backend.switchHint(binding).isEmpty(), "approval hint is visible");
    require(backend.triggerAction(settings::SettingsActionBinding::OpenLoginItemSettings) &&
                opened == 1,
            "settings action reaches injected native operation");
    require(language.setLanguage(QStringLiteral("en_US")), "English language");
    const QString englishHint = backend.switchHint(binding);
    for (const QString& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        require(language.setLanguage(locale), "switch translated language");
        require(backend.switchHint(binding) != englishHint &&
                    !backend.switchHint(binding).isEmpty(),
                "approval hint retranslates at runtime");
    }
    require(language.setLanguage(QStringLiteral("en_US")), "restore English");
    fail = true;
    require(!backend.applySwitchValue(binding, false) && backend.switchValue(binding) &&
                error == u"native failure",
            "native failure surfaces without falsely changing toggle");
    auto& configuration = storage.configuration();
    const auto enabledKey = QStringLiteral("system/auto_start_at_boot");
    require(configuration.setValue(enabledKey, true), "startup import baseline");
    auto imported = configuration.snapshot();
    imported.insert(enabledKey, false);
    require(!backend.importConfigurationSnapshot(
                imported, snow_shot::storage::ConfigurationStore::currentSchemaVersion()) &&
                configuration.value(enabledKey).toBool() && backend.switchValue(binding),
            "failed native import keeps the pre-import startup preference for recovery");
    fail = false;
    save = false;
    require(!backend.applySwitchValue(binding, false) && !backend.switchValue(binding) &&
                error.contains(QStringLiteral("preference")),
            "persistence failure retains observed native status");
    save = true;
    require(backend.resetSection(settings::SettingsSectionReset::SystemGeneral) &&
                backend.switchValue(binding) &&
                backend.selectValue(settings::SettingsSelectBinding::UpdateMode).toString() ==
                    u"check",
            "reset restores native startup and macOS automatic check defaults");
    const int beforeRefresh = changes;
    native.status = LoginItemStatus::Unregistered;
    backend.refreshPlatformSettings();
    require(!backend.switchValue(binding) && changes == beforeRefresh,
            "external removal refresh never re-registers");
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    SettingsPageWidget page(registry, QStringLiteral("system-settings"), session);
    native = {LoginItemStatus::Unavailable, QStringLiteral("install app")};
    page.show();
    require(!backend.switchEnabled(binding) && backend.switchHint(binding) == u"install app",
            "showing settings refreshes eligibility and explanation");
    page.hide();
    native = {LoginItemStatus::Enabled, {}};
    emit app.applicationStateChanged(Qt::ApplicationActive);
    require(backend.switchEnabled(binding) && backend.switchValue(binding),
            "activation refresh observes external enable");
    storage.shutdown();
    std::cout << "Login item settings tests passed\n";
}
