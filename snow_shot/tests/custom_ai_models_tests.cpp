#include "snow_shot/customaimodelconfiguration.h"
#include "snow_shot/storage/configurationstore.h"
#include "snow_shot/storage/configurationschema.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/presentation/components/customaimodelssettingswidget.h"
#include "snow_shot/presentation/components/settingspagewidget.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "widgets/button.h"
#include "widgets/form.h"
#include "widgets/input_line_edit.h"
#include "widgets/input_password_edit.h"
#include "widgets/modal.h"
#include "widgets/switch.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLabel>
#include <QTemporaryDir>
#include <QTimer>
#include <QEventLoop>
#include <cstdlib>
#include <iostream>

using namespace snow_shot;
using namespace adqt::widgets;
namespace settings = snow_shot::presentation::settings;
namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::_Exit(1);
    }
}
void flush() {
    QEventLoop settle;
    QTimer::singleShot(250, &settle, &QEventLoop::quit);
    settle.exec();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}
CustomAiModelConfiguration example() {
    return {QUuid::createUuid().toString(QUuid::WithoutBraces),
            QStringLiteral("My Model"),
            QStringLiteral("http://localhost:1234/v1"),
            {},
            QStringLiteral("model-id"),
            true};
}
void storageContracts() {
    const QString key = QStringLiteral("api_configuration/custom_models");
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("config.json"));
    auto model = example();
    {
        storage::ConfigurationStore store(path, true, true, 8000);
        require(store.value(key).toArray().isEmpty(), "custom models default to empty");
        model.baseUrl += QStringLiteral("/// ");
        model.name += QStringLiteral(" ");
        require(store.setValue(key, customAiModelsToJson({model})), "valid model saves");
        model = normalizeCustomAiModel(model);
        require(customAiModelsFromJson(store.value(key)) == CustomAiModels{model},
                "values normalized");
        auto duplicate = model;
        duplicate.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        duplicate.name = model.name.toUpper();
        require(!store.setValue(key, customAiModelsToJson({model, duplicate})),
                "duplicate names reject whole write");
        require(customAiModelsFromJson(store.value(key)).size() == 1,
                "rejection preserves previous records");
        require(store.flushNow().success, "configuration flush succeeds");
    }
    {
        storage::ConfigurationStore store(path, true, true, 8000);
        require(customAiModelsFromJson(store.value(key)) == CustomAiModels{model},
                "models survive reopening");
    }
    {
        storage::ConfigurationStore store(path, true, false, 8000);
        require(!store.setValue(key, QJsonArray()), "read-only storage rejects deletion");
    }
    for (const auto& url : {QStringLiteral("ftp://example.com"),
                            QStringLiteral("https://user:password@example.com/v1"),
                            QStringLiteral("https://example.com/v1?key=x"),
                            QStringLiteral("https://example.com/v1#x"), QStringLiteral("/v1"),
                            QStringLiteral("https://example.com/v1/chat/completions")}) {
        auto invalid = model;
        invalid.baseUrl = url;
        require(
            !storage::ConfigurationSchema::normalize(key, customAiModelsToJson({invalid})).valid,
            "invalid URL rejected");
    }
    auto document = storage::ConfigurationSchema::completeDefaultDocument();
    const QJsonArray raw{
        customAiModelsToJson({model}).first(),
        QJsonObject{{QStringLiteral("api_key"), QStringLiteral("never-log-this")}}};
    document.insert(QStringLiteral("api_configuration"),
                    QJsonObject{{QStringLiteral("custom_models"), raw}});
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open malformed fixture");
    const auto bytes = QJsonDocument(document).toJson();
    file.write(bytes);
    file.close();
    {
        storage::ConfigurationStore store(path, true, true, 8000);
        require(customAiModelsFromJson(store.value(key)) == CustomAiModels{model},
                "load salvages valid records");
        require(!store.lastError().isEmpty() &&
                    !store.lastError().contains(QStringLiteral("never-log-this")),
                "load error excludes contents");
        static_cast<void>(store.flushNow());
    }
    require(file.open(QIODevice::ReadOnly), "read preserved document");
    require(QJsonDocument::fromJson(file.readAll())
                    .object()
                    .value(QStringLiteral("api_configuration"))
                    .toObject()
                    .value(QStringLiteral("custom_models")) == raw,
            "opening malformed collection must preserve every original record");
}

void widgetContracts(QApplication& application) {
    QTemporaryDir directory;
    auto& storage = storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 0}).success,
            "initialize isolated storage");
    presentation::styles::ThemeManager::instance().initialize(application);
    presentation::LanguageManager::instance().initialize();
    require(presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "select English for UI assertions");
    {
        presentation::GlobalShortcutManager shortcuts;
        settings::BuiltInSettingsBackend backend(shortcuts);
        const auto registry = settings::buildBuiltInSettingsRegistry();
        settings::SettingsRuntimeSession session(registry, backend);
        SettingsPageWidget page(registry, QStringLiteral("api-configuration"), session);
        page.resize(880, 760);
        page.show();
        flush();
        auto* widget = page.findChild<CustomAiModelsSettingsWidget*>();
        require(widget != nullptr, "page constructs custom model renderer");
        auto* add = widget->findChild<AdButton*>(QStringLiteral("customAiModelAdd"));
        require(add != nullptr && widget->findChild<QLabel*>(QStringLiteral("customAiModelsEmpty")),
                "empty state with add action");
        add->click();
        flush();
        auto* modal = widget->findChild<AdModal*>(QStringLiteral("customAiModelEditor"));
        require(modal != nullptr, "add opens form");
        modal->acceptButton()->click();
        flush();
        require(session.customAiModels().isEmpty(), "empty submission does not create a record");
        auto* name = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("modelName"));
        auto* url = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("apiUrl"));
        auto* apiModel = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("apiModel"));
        auto* key = modal->contentWidget()->findChild<AdPasswordEdit*>(QStringLiteral("apiKey"));
        require(name && url && apiModel && key && !key->textVisible(),
                "form has masked password input");
        require(name->width() >= 400 && name->width() == apiModel->width() &&
                    name->width() == key->width(),
                "model inputs fill the form with consistent widths");
        name->setText(QStringLiteral("Personal model"));
        url->setText(QStringLiteral("http://localhost:1234/v1/"));
        apiModel->setText(QStringLiteral("local-id"));
        key->setText(QStringLiteral("portable-secret"));
        key->setTextVisible(true);
        require(key->textVisible(), "key can be revealed");
        modal->contentWidget()
            ->findChild<AdSwitch*>(QStringLiteral("visionSupport"))
            ->setChecked(true);
        modal->acceptButton()->click();
        flush();
        require(session.customAiModels().size() == 1, "create persists one model");
        const auto original = session.customAiModels().first();
        require(original.apiKey == QStringLiteral("portable-secret") && original.supportsVision,
                "key and vision persist");
        widget->findChild<AdButton*>(QStringLiteral("copy:") + original.id)->click();
        flush();
        widget->findChild<AdButton*>(QStringLiteral("copy:") + original.id)->click();
        flush();
        const auto copied = session.customAiModels();
        require(copied.size() == 3 && copied[1].id != original.id &&
                    copied[1].apiKey == original.apiKey &&
                    copied[1].name == QStringLiteral("Personal model (Copy)") &&
                    copied[2].name == QStringLiteral("Personal model (Copy 2)"),
                "copy duplicates immediately with independent identity and name");
        widget->findChild<AdButton*>(QStringLiteral("edit:") + original.id)->click();
        flush();
        modal = widget->findChild<AdModal*>(QStringLiteral("customAiModelEditor"));
        name = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("modelName"));
        name->setText(copied[1].name);
        modal->acceptButton()->click();
        flush();
        require(session.customAiModels().first() == original, "duplicate edit rejected");
        name->setText(QStringLiteral("Renamed model"));
        modal->acceptButton()->click();
        flush();
        require(session.customAiModels().first().id == original.id &&
                    session.customAiModels().first().name == QStringLiteral("Renamed model"),
                "rename preserves identity");
        widget->findChild<AdButton*>(QStringLiteral("delete:") + copied[2].id)->click();
        flush();
        auto* deletion = widget->findChild<AdModal*>(QStringLiteral("customAiModelDeleteModal"));
        require(deletion != nullptr, "delete requests confirmation");
        deletion->reject();
        flush();
        require(session.customAiModels().size() == 3, "cancel delete preserves list");
        widget->findChild<AdButton*>(QStringLiteral("delete:") + copied[2].id)->click();
        flush();
        widget->findChild<AdModal*>(QStringLiteral("customAiModelDeleteModal"))
            ->acceptButton()
            ->click();
        flush();
        require(session.customAiModels().size() == 2, "confirmed deletion persists");
        add->click();
        flush();
        widget->findChild<AdModal*>(QStringLiteral("customAiModelEditor"))->reject();
        flush();
        require(session.customAiModels().size() == 2, "cancel create preserves list");

        const auto args = application.arguments();
        const int previewIndex = args.indexOf(QStringLiteral("--preview-dir"));
        if (previewIndex >= 0 && previewIndex + 1 < args.size()) {
            QDir output(args[previewIndex + 1]);
            output.mkpath(QStringLiteral("."));
            for (const auto& locale :
                 {QStringLiteral("en_US"), QStringLiteral("zh_CN"), QStringLiteral("zh_TW")}) {
                static_cast<void>(presentation::LanguageManager::instance().setLanguage(locale));
                for (const auto mode : {presentation::styles::ThemeMode::Light,
                                        presentation::styles::ThemeMode::Dark}) {
                    presentation::styles::ThemeManager::instance().setThemeMode(mode);
                    flush();
                    const QString suffix = locale + (mode == presentation::styles::ThemeMode::Light
                                                         ? QStringLiteral("-light")
                                                         : QStringLiteral("-dark"));
                    page.grab().save(output.filePath(suffix + QStringLiteral("-page.png")));
                    add->click();
                    flush();
                    QEventLoop loop;
                    QTimer::singleShot(250, &loop, &QEventLoop::quit);
                    loop.exec();
                    auto* previewModal =
                        widget->findChild<AdModal*>(QStringLiteral("customAiModelEditor"));
                    require(previewModal->contentWidget()->window()->grab().save(
                                output.filePath(suffix + QStringLiteral("-modal.png"))),
                            "save modal preview");
                    widget->findChild<AdModal*>(QStringLiteral("customAiModelEditor"))->reject();
                    flush();
                }
            }
        }
    }
    storage.shutdown();
}
} // namespace
int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("custom_ai_models_tests"));
    storageContracts();
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "load offscreen UI font");
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) >= 0,
            "load offscreen Chinese font");
#endif
    widgetContracts(application);
    return 0;
}
