#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/settings/settingsformfield.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QLineEdit>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>

namespace settings = snow_shot::presentation::settings;
namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void serverSettings(const QTemporaryDir& temporary) {
    const QString key = QStringLiteral("api_configuration/server_url");
    const QString fieldId = QStringLiteral("api.server-url");
    const auto binding = settings::SettingsTextBinding::ServerUrl;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    const auto updateMode = configuration.value(QStringLiteral("updates/mode"));
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto& registry = settings::builtInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    require(registry.isValid() && registry.fieldForText(binding) != nullptr,
            "server field belongs to valid registry");
    require(configuration.value(key).toString().isEmpty() && backend.textValue(binding).isEmpty(),
            "empty stored value uses the configured default");

    SettingsPageWidget page(registry, QStringLiteral("connections-services"), session);
    page.resize(1000, 800);
    page.show();
    page.reveal({page.pageId(), QStringLiteral("snow-shot-server"), fieldId});
    QApplication::processEvents();
    QLineEdit* serverControl = nullptr;
    for (auto* control : page.findChildren<QLineEdit*>()) {
        if (control->placeholderText() == SnowShotApiClient::configuredBaseUrl())
            serverControl = control;
    }
    require(serverControl != nullptr, "server editor displays the resolved default address");
    settings::SettingsFormField* serverField = nullptr;
    for (auto* candidate : page.findChildren<QObject*>()) {
        auto* field = dynamic_cast<settings::SettingsFormField*>(candidate);
        if (field && field->descriptor().id == fieldId) {
            serverField = field;
            break;
        }
    }
    require(serverField != nullptr, "server editor uses the shared settings field binding");
    serverControl->setText(QStringLiteral("local unsaved draft"));
    page.retranslateUi();
    require(serverControl->text() == QStringLiteral("local unsaved draft") &&
                session.state(fieldId).draftValue.toString().isEmpty(),
            "retranslation preserves local uncommitted text without a settings write");
    settings::SettingsSearchIndex search(registry);
    bool found = false;
    for (const auto& entry : search.search(QStringLiteral("Server address")))
        found = found || entry.location.itemId == fieldId;
    require(found, "server setting is searchable");

    require(session.applyTextValue(binding, QStringLiteral("  http://localhost:8089/prefix///  ")),
            "valid server is accepted");
    const QString accepted = QStringLiteral("http://localhost:8089/prefix");
    require(backend.textValue(binding) == accepted && !session.hasPendingWrites(),
            "normalization settles the runtime write");
    const QStringList invalid{QStringLiteral("example.test"),
                              QStringLiteral("ftp://example.test"),
                              QStringLiteral("https:///path"),
                              QStringLiteral("https://user@example.test"),
                              QStringLiteral("https://@example.test"),
                              QStringLiteral("https://example.test?x=1"),
                              QStringLiteral("https://example.test#part"),
                              QStringLiteral("https://example.test?"),
                              QStringLiteral("https://example.test#"),
                              QStringLiteral("https://bad host"),
                              QStringLiteral("http://localhost:99999"),
                              QStringLiteral("https://example.test/%zz")};
    for (const auto& value : invalid) {
        require(!session.applyTextValue(binding, value) && backend.textValue(binding) == accepted,
                "invalid input preserves the accepted address");
        require(session.state(fieldId).error.contains(QStringLiteral("HTTP or HTTPS")),
                "validation error explains the required address format");
        require(serverControl->text() == value &&
                    serverField->controller()->item()->errorMessages().contains(
                        session.state(fieldId).error),
                "rejected drafts remain visible with shared inline feedback");
    }
    require(!storage::ConfigurationSchema::normalize(key, 123).valid,
            "configuration rejects non-string addresses");
    require(session.discard(fieldId), "invalid draft can be discarded");
    require(configuration.flushNow().success, "server preference is persisted");
    storage::ConfigurationStore reloaded(temporary.filePath(QStringLiteral("data/config.json")),
                                         true, true, 60000);
    require(reloaded.value(key).toString() == accepted, "server survives a restart");

    const QString archive = temporary.filePath(QStringLiteral("server.zip"));
    require(storage::ConfigurationArchive::write(
                archive, {{key, accepted}}, storage::ConfigurationStore::currentSchemaVersion())
                .isEmpty(),
            "server preference exports");
    const auto imported = storage::ConfigurationArchive::read(archive);
    require(imported.isValid() && imported.values.value(key).toString() == accepted,
            "server preference survives archive round trip");
    require(session.reset(settings::SettingsSectionReset::Server) &&
                backend.textValue(binding).isEmpty(),
            "section reset restores default server");
    require(configuration.setValues(imported.values) && backend.textValue(binding) == accepted,
            "import restores saved server");
    // Import notifications are deliberately coalesced until the next event-loop turn.
    QApplication::processEvents();
    require(serverControl->text() == accepted, "imported address is reflected in the editor");
    serverControl->setText(QStringLiteral("  "));
    require(QMetaObject::invokeMethod(serverControl, "editingFinished"), "commit editor contents");
    require(backend.textValue(binding).isEmpty() && !session.hasPendingWrites(),
            "clearing restores default server");
    require(configuration.value(QStringLiteral("updates/mode")) == updateMode,
            "server changes leave update configuration unchanged");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated settings directory");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    static_cast<void>(
        applicationStorage.initialize({temporary.filePath(QStringLiteral("bin")),
                                       temporary.filePath(QStringLiteral("data")), 60000}));
    serverSettings(temporary);
    applicationStorage.shutdown();
    return 0;
}
