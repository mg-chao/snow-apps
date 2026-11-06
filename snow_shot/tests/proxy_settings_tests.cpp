#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsformfield.h"
#include "snow_shot/presentation/settings/settingssearchindex.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/configurationarchive.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/translation/translationservice.h"

#include <QApplication>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
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

void proxySettings(const QTemporaryDir& temporary) {
    const QString key = QStringLiteral("network/http_proxy");
    const QString modeKey = QStringLiteral("network/proxy");
    const QString fieldId = QStringLiteral("network.http-proxy");
    const auto binding = settings::SettingsTextBinding::HttpProxy;
    auto& configuration = storage::ApplicationStorage::instance().configuration();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    settings::BuiltInSettingsBackend backend(shortcuts);
    const auto& registry = settings::builtInSettingsRegistry();
    settings::SettingsRuntimeSession session(registry, backend);
    require(registry.isValid() && registry.fieldForText(binding) != nullptr,
            "proxy address belongs to the settings registry");

    SettingsPageWidget page(registry, QStringLiteral("connections-services"), session);
    page.resize(1000, 800);
    page.show();
    page.reveal({page.pageId(), QStringLiteral("network"), fieldId});
    QApplication::processEvents();
    settings::SettingsFormField* addressField = nullptr;
    settings::SettingsFormField* modeField = nullptr;
    for (auto* object : page.findChildren<QObject*>()) {
        if (auto* field = dynamic_cast<settings::SettingsFormField*>(object)) {
            if (field->descriptor().id == fieldId)
                addressField = field;
            if (field->descriptor().id == QStringLiteral("network.proxy"))
                modeField = field;
        }
    }
    require(addressField && modeField, "proxy dropdown and address editor are rendered");
    auto* editor = qobject_cast<QLineEdit*>(addressField->editor());
    require(editor && !editor->isEnabled() && editor->isVisible(),
            "proxy address is visible but disabled outside manual mode");
    require(addressField->viewWidget()->mapTo(&page, QPoint()).y() >
                modeField->viewWidget()->mapTo(&page, QPoint()).y(),
            "proxy address appears below the dropdown");
    require(
        session.applySelectValue(settings::SettingsSelectBinding::Proxy, QStringLiteral("manual")),
        "manual proxy mode is accepted");
    QApplication::processEvents();
    require(editor->isEnabled(), "manual mode enables the proxy editor");
    editor->setText(QStringLiteral("  user:p%40ss@127.0.0.1:7890/  "));
    require(QMetaObject::invokeMethod(editor, "editingFinished"), "commit proxy address");
    const QString accepted = QStringLiteral("http://user:p%40ss@127.0.0.1:7890");
    require(backend.textValue(binding) == accepted && !session.hasPendingWrites(),
            "proxy address is normalized and saved");
    editor->setText(QStringLiteral("uncommitted draft"));
    page.retranslateUi();
    require(editor->text() == QStringLiteral("uncommitted draft"),
            "retranslation preserves the uncommitted address");
    const QStringList invalid{
        QStringLiteral("socks5://localhost:1080"), QStringLiteral("https://localhost:7890"),
        QStringLiteral("http://bad host:80"),      QStringLiteral("http://localhost:0"),
        QStringLiteral("http://localhost:99999"),  QStringLiteral("http://localhost/path"),
        QStringLiteral("http://localhost?query"),  QStringLiteral("http://localhost#fragment")};
    for (const auto& value : invalid) {
        require(!session.applyTextValue(binding, value) && backend.textValue(binding) == accepted,
                "invalid proxy addresses preserve the saved value");
        require(!session.state(fieldId).error.isEmpty() && editor->text() == value,
                "invalid proxy draft remains visible with validation feedback");
    }
    require(!storage::ConfigurationSchema::normalize(key, 42).valid,
            "non-string proxy values are rejected");
    require(
        storage::ConfigurationSchema::normalize(key, QStringLiteral("[::1]:8080")).value ==
                QStringLiteral("http://[::1]:8080") &&
            storage::ConfigurationSchema::normalize(key, QStringLiteral("proxy.example")).value ==
                QStringLiteral("http://proxy.example:80"),
        "proxy normalization supports IPv6 and the default HTTP port");
    require(session.discard(fieldId), "invalid draft can be discarded");

    SnowShotApiClient client(QStringLiteral("http://localhost:8080"));
    auto* network = new QNetworkAccessManager(&client);
    static_cast<void>(
        snow_shot::translation::TranslationService::forClient(client, configuration, QLocale::c()));
    require(network->proxy().type() == QNetworkProxy::HttpProxy &&
                network->proxy().hostName() == QStringLiteral("127.0.0.1") &&
                network->proxy().port() == 7890 &&
                network->proxy().password() == QStringLiteral("p@ss"),
            "saved manual proxy is applied when the API client starts");
    require(
        session.applySelectValue(settings::SettingsSelectBinding::Proxy, QStringLiteral("system")),
        "switch to system mode");
    QApplication::processEvents();
    require(!editor->isEnabled() && backend.textValue(binding) == accepted &&
                network->proxyFactory() != nullptr,
            "system mode retains the address and updates the API client");
    require(
        session.applySelectValue(settings::SettingsSelectBinding::Proxy, QStringLiteral("manual")),
        "restore manual mode");
    require(session.applyTextValue(binding, QStringLiteral("proxy.example:8080")) &&
                network->proxy().hostName() == QStringLiteral("proxy.example") &&
                network->proxy().port() == 8080,
            "editing the address updates the active client immediately");
    require(configuration.flushNow().success, "proxy preferences persist");
    storage::ConfigurationStore reloaded(temporary.filePath(QStringLiteral("data/config.json")),
                                         true, true, 60000);
    require(reloaded.value(key).toString() == QStringLiteral("http://proxy.example:8080") &&
                reloaded.value(modeKey).toString() == QStringLiteral("manual"),
            "manual proxy settings survive restart");
    const QString archive = temporary.filePath(QStringLiteral("proxy.zip"));
    require(storage::ConfigurationArchive::write(
                archive, {{key, reloaded.value(key)}, {modeKey, reloaded.value(modeKey)}},
                storage::ConfigurationStore::currentSchemaVersion())
                .isEmpty(),
            "proxy settings export");
    const auto imported = storage::ConfigurationArchive::read(archive);
    require(imported.isValid(), "proxy settings archive is valid");
    require(session.reset(settings::SettingsSectionReset::Network) &&
                backend.textValue(binding).isEmpty() &&
                configuration.value(modeKey).toString() == QStringLiteral("none") &&
                network->proxy().type() == QNetworkProxy::NoProxy,
            "network reset clears manual proxy settings and restores direct requests");
    require(configuration.setValues(imported.values), "proxy settings import");
    QApplication::processEvents();
    require(editor->text() == QStringLiteral("http://proxy.example:8080") && editor->isEnabled(),
            "import restores the manual proxy editor");
    editor->clear();
    require(QMetaObject::invokeMethod(editor, "editingFinished"), "clear proxy address");
    require(backend.textValue(binding).isEmpty() &&
                network->proxy().type() == QNetworkProxy::NoProxy,
            "clearing the manual address removes the proxy");
    settings::SettingsSearchIndex search(registry);
    bool found = false;
    for (const auto& entry : search.search(QStringLiteral("HTTP/HTTPS proxy")))
        found = found || entry.location.itemId == fieldId;
    require(found, "manual proxy setting is searchable");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QTemporaryDir temporary;
    require(temporary.isValid(), "isolated proxy settings directory");
    auto& storage = storage::ApplicationStorage::instance();
    static_cast<void>(storage.initialize({temporary.filePath(QStringLiteral("bin")),
                                          temporary.filePath(QStringLiteral("data")), 60000}));
    proxySettings(temporary);
    storage.shutdown();
    return 0;
}
