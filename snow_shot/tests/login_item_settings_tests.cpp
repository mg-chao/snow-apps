#include "login_item_test_support.h"
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
#include <QSemaphore>
#include <QTimer>
#include <atomic>
#include "snow_shot/storage/configurationarchive.h"
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
    struct Gate {
        QSemaphore entered;
        QSemaphore release;
    };
    std::atomic<Gate*> queryGate{nullptr};
    LoginItemSnapshot native{LoginItemStatus::Unregistered, {}};
    bool fail = false;
    bool save = true;
    int opened = 0;
    int changes = 0;
    LoginItemService service({[&] {
                                  if (auto gate = queryGate.exchange(nullptr)) {
                                      gate->entered.release();
                                      gate->release.acquire();
                                  }
                                  return native;
                              },
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
    login_item_test::idle(service);
    const auto settled = [&](bool admitted) {
        const auto completion = service.completionFuture();
        login_item_test::idle(service);
        return admitted && (!completion.valid() || completion.get().success);
    };
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
    require(settled(backend.applySwitchValue(binding, true)) && backend.switchValue(binding) &&
                pendingSignals > 0,
            "toggle registers through service and emits busy state");
    require(!backend.switchHint(binding).isEmpty(), "approval hint is visible");
    require(backend.triggerAction(settings::SettingsActionBinding::OpenLoginItemSettings) &&
                opened == 1,
            "settings action reaches injected native operation");
    require(language.setLanguage(QStringLiteral("en_US")), "English language");
    login_item_test::idle(service);
    const QString englishHint = backend.switchHint(binding);
    for (const QString& locale : {QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
        require(language.setLanguage(locale), "switch translated language");
        login_item_test::idle(service);
        require(backend.switchHint(binding) != englishHint &&
                    !backend.switchHint(binding).isEmpty(),
                "approval hint retranslates at runtime");
    }
    require(language.setLanguage(QStringLiteral("en_US")), "restore English");
    login_item_test::idle(service);
    fail = true;
    require(!settled(backend.applySwitchValue(binding, false)) && backend.switchValue(binding) &&
                error == u"native failure",
            "native failure surfaces without falsely changing toggle");
    auto& configuration = storage.configuration();
    const auto enabledKey = QStringLiteral("system/auto_start_at_boot");
    require(configuration.setValue(enabledKey, true), "startup import baseline");
    auto imported = configuration.snapshot();
    imported.insert(enabledKey, false);
    require(!settled(backend.importConfigurationSnapshot(
                imported, snow_shot::storage::ConfigurationStore::currentSchemaVersion())) &&
                configuration.value(enabledKey).toBool() && backend.switchValue(binding),
            "failed native import keeps the pre-import startup preference for recovery");
    fail = false;
    save = false;
    require(!settled(backend.applySwitchValue(binding, false)) && !backend.switchValue(binding) &&
                error.contains(QStringLiteral("preference")),
            "persistence failure retains observed native status");
    save = true;
    require(settled(backend.resetSection(settings::SettingsSectionReset::SystemGeneral)) &&
                backend.switchValue(binding) &&
                backend.selectValue(settings::SettingsSelectBinding::UpdateMode).toString() ==
                    u"check",
            "reset restores native startup and macOS automatic check defaults");
    const int beforeRefresh = changes;
    native.status = LoginItemStatus::Unregistered;
    backend.refreshPlatformSettings();
    login_item_test::idle(service);
    require(!backend.switchValue(binding) && changes == beforeRefresh,
            "external removal refresh never re-registers");
    const auto registry = settings::buildBuiltInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    const QString startupId = QStringLiteral("system.auto-start-at-boot");
    const QString updatesId = QStringLiteral("updates.mode");
    require(configuration.setValue(QStringLiteral("updates/mode"), QStringLiteral("manual")),
            "prepare nondefault update preference");
    session.refreshAll();
    fail = true;
    error.clear();
    require(!settled(session.reset(settings::SettingsSectionReset::SystemGeneral)) &&
                !backend.switchValue(binding) && error == u"native failure",
            "failed startup reset remains an aggregate failure and reports the native error");
    require(session.state(startupId).phase == settings::SettingsWritePhase::Failed &&
                session.state(startupId).error == u"native failure" &&
                session.state(updatesId).phase == settings::SettingsWritePhase::Clean &&
                session.state(updatesId).acceptedValue.toString() == u"check" &&
                session.state(updatesId).error.isEmpty(),
            "failed registration must not prevent or reject the independent update reset");
    app.processEvents();
    require(session.state(startupId).error == u"native failure" &&
                session.state(updatesId).error.isEmpty(),
            "synchronization preserves only the real startup failure");
    fail = false;
    require(settled(session.reset(settings::SettingsSectionReset::SystemGeneral)) &&
                session.state(startupId).phase == settings::SettingsWritePhase::Clean &&
                session.state(startupId).error.isEmpty(),
            "reset can recover after registration becomes available");
    save = false;
    require(!settled(session.reset(settings::SettingsSectionReset::SystemGeneral)) &&
                session.state(startupId).phase == settings::SettingsWritePhase::Failed &&
                session.state(startupId).error.contains(QStringLiteral("preference")) &&
                session.state(updatesId).phase == settings::SettingsWritePhase::Clean &&
                session.state(updatesId).error.isEmpty(),
            "startup preference persistence failures remain scoped to startup");
    save = true;
    require(settled(session.reset(settings::SettingsSectionReset::SystemGeneral)),
            "recover startup preference persistence");
    configuration.suspendWrites(true);
    require(!settled(session.reset(settings::SettingsSectionReset::SystemGeneral)) &&
                session.state(startupId).phase == settings::SettingsWritePhase::Clean &&
                session.state(startupId).error.isEmpty() &&
                session.state(updatesId).phase == settings::SettingsWritePhase::Rejected &&
                !session.state(updatesId).error.isEmpty(),
            "an update write failure must not reject a successful startup reset");
    configuration.suspendWrites(false);
    require(settled(session.reset(settings::SettingsSectionReset::SystemGeneral)),
            "recover update preference persistence");
    SettingsPageWidget page(registry, QStringLiteral("general"), session);
    native = {LoginItemStatus::Unavailable, QStringLiteral("install app")};
    page.show();
    login_item_test::idle(service);
    require(!backend.switchEnabled(binding) && backend.switchHint(binding) == u"install app",
            "showing settings refreshes eligibility and explanation");
    page.hide();
    native = {LoginItemStatus::Enabled, {}};
    emit app.applicationStateChanged(Qt::ApplicationActive);
    login_item_test::idle(service);
    require(backend.switchEnabled(binding) && backend.switchValue(binding),
            "activation refresh observes external enable");
    app.processEvents();
    int refreshes = 0;
    QObject::connect(&session, &settings::SettingsRuntimeSession::refreshed, &app,
                     [&] { ++refreshes; });
    SettingsPageWidget screenshots(registry, QStringLiteral("screenshots"), session);
    for (int cycle = 0; cycle < 3; ++cycle) {
        page.show();
        login_item_test::idle(service);
        app.processEvents();
        page.hide();
        screenshots.show();
        login_item_test::idle(service);
        app.processEvents();
        screenshots.hide();
    }
    require(refreshes == 0,
            "switching pages with unchanged native state must not refresh all settings");
    native = {LoginItemStatus::Unavailable, QStringLiteral("install app")};
    page.show();
    login_item_test::idle(service);
    app.processEvents();
    require(refreshes == 1 && !session.state(QStringLiteral("system.auto-start-at-boot")).enabled,
            "page switch must still refresh settings when native eligibility changes");
    native.error = QStringLiteral("updated explanation");
    backend.refreshPlatformSettings();
    login_item_test::idle(service);
    app.processEvents();
    require(refreshes == 2 && backend.switchHint(binding) == native.error,
            "changed native explanation must refresh settings even with the same status");
    backend.refreshPlatformSettings();
    login_item_test::idle(service);
    app.processEvents();
    require(refreshes == 2,
            "repeated platform refresh must not repeat the complete settings refresh");
    // Exercise the real application-activation connection while native status is blocked.
    native = {LoginItemStatus::Unregistered, {}};
    login_item_test::refresh(service);
    auto gate = std::make_shared<Gate>();
    queryGate = gate.get();
    emit app.applicationStateChanged(Qt::ApplicationActive);
    login_item_test::waitFor([&] { return gate->entered.tryAcquire(); });
    bool guiEvent = false;
    QTimer::singleShot(0, &app, [&] { guiEvent = true; });
    login_item_test::waitFor([&] { return guiEvent; });
    require(!service.pending(),
            "background activation refresh does not mark a settings write pending");
    gate->release.release();
    login_item_test::idle(service);

    // Failed async resets and retries retain their own draft/error, including repeated errors.
    settings::SettingsRuntimeSession asynchronousSession(registry, backend);
    fail = true;
    require(asynchronousSession.reset(settings::SettingsSectionReset::SystemGeneral) &&
                asynchronousSession.state(startupId).phase ==
                    settings::SettingsWritePhase::Pending &&
                asynchronousSession.state(startupId).draftValue.toBool(),
            "reset exposes the requested target while native registration is pending");
    login_item_test::idle(service);
    require(asynchronousSession.state(startupId).phase == settings::SettingsWritePhase::Failed &&
                asynchronousSession.state(startupId).draftValue.toBool() &&
                !asynchronousSession.state(startupId).acceptedValue.toBool() &&
                asynchronousSession.state(updatesId).phase == settings::SettingsWritePhase::Clean,
            "failed reset preserves a retryable target and independent successes");
    fail = false;
    save = false;
    require(asynchronousSession.retry(startupId), "first persistence failure is admitted");
    login_item_test::idle(service);
    require(asynchronousSession.state(startupId).phase == settings::SettingsWritePhase::Failed,
            "native success with failed persistence remains a failed write");
    require(asynchronousSession.retry(startupId), "retry of persistence failure is admitted");
    login_item_test::idle(service);
    require(asynchronousSession.state(startupId).phase == settings::SettingsWritePhase::Failed &&
                asynchronousSession.state(startupId).error.contains(QStringLiteral("preference")),
            "repeated identical persistence errors must not become false success");
    save = true;
    require(asynchronousSession.retry(startupId), "persistence retry recovers");
    login_item_test::idle(service);

    // Import completion must cover both login-item work and history-policy work.
    require(login_item_test::change(service, false).success, "prepare import native baseline");
    require(configuration.setValue(enabledKey, false), "prepare import stored baseline");
    imported = configuration.snapshot();
    imported.insert(enabledKey, true);
    const QString historyKey = QStringLiteral("capture_history/max_entries");
    imported.insert(historyKey, configuration.value(historyKey).toInt() + 1);
    gate = std::make_shared<Gate>();
    queryGate = gate.get();
    std::shared_future<snow_shot::storage::StorageResult> importFuture;
    require(backend.importConfigurationSnapshot(
                imported, snow_shot::storage::ConfigurationStore::currentSchemaVersion(),
                &importFuture),
            "asynchronous import admitted");
    login_item_test::waitFor([&] { return gate->entered.tryAcquire(); });
    require(importFuture.valid() &&
                importFuture.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
            "history completion cannot hide a blocked login-item import");
    gate->release.release();
    login_item_test::idle(service);
    login_item_test::waitFor([&] {
        return importFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    });
    require(importFuture.get().success && service.snapshot().requested(),
            "import completes after all native and history work");

    // The interactive import action must also wait and surface late native failures.
    require(configuration.setValue(enabledKey, true), "prepare archive import baseline");
    imported = configuration.snapshot();
    imported.insert(enabledKey, false);
    const QString archive = temporary.filePath(QStringLiteral("settings.zip"));
    require(snow_shot::storage::ConfigurationArchive::write(
                archive, imported, snow_shot::storage::ConfigurationStore::currentSchemaVersion())
                .isEmpty(),
            "write import archive");
    int finishedImports = 0;
    bool importSucceeded = true;
    QObject::connect(&backend, &settings::SettingsBackend::actionFinished, &app,
                     [&](settings::SettingsActionBinding action, bool success, const QString&) {
                         if (action == settings::SettingsActionBinding::ImportConfiguration) {
                             ++finishedImports;
                             importSucceeded = success;
                         }
                     });
    fail = true;
    gate = std::make_shared<Gate>();
    queryGate = gate.get();
    require(backend.triggerAction(settings::SettingsActionBinding::ImportConfiguration, archive),
            "interactive import admitted");
    login_item_test::waitFor([&] { return gate->entered.tryAcquire(); });
    require(finishedImports == 0 &&
                backend.actionState(settings::SettingsActionBinding::ImportConfiguration).busy,
            "interactive import stays pending while native work is outstanding");
    gate->release.release();
    login_item_test::waitFor([&] { return finishedImports == 1; });
    require(!importSucceeded && configuration.value(enabledKey).toBool() &&
                service.snapshot().requested(),
            "late native failure fails the action and preserves actual startup state");
    login_item_test::idle(service);
    storage.shutdown();
    std::cout << "Login item settings tests passed\n";
}
