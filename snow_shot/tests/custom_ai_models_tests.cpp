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
#include "widgets/tag.h"
#include "widgets/combo_box.h"
#include "widgets/spin.h"
#include <QLineEdit>
#include <QListView>
#include <QTcpServer>
#include <QTcpSocket>
#include <QElapsedTimer>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
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
class EditorPaintObserver final : public QObject {
  public:
    QList<QRect> frames;

  protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        auto* widget = qobject_cast<QWidget*>(object);
        if (event->type() == QEvent::Paint && widget != nullptr &&
            widget->objectName() == QStringLiteral("modelName")) {
            const QRect frame(widget->mapTo(widget->window(), QPoint()), widget->size());
            if (frames.isEmpty() || frames.last() != frame) {
                frames.append(frame);
            }
        }
        return false;
    }
};
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
        require(add->buttonStyle() == AdButton::ButtonStyle::Dashed &&
                    add->shape() == AdButton::Shape::Rounded && add->width() == widget->width(),
                "add action matches the full-width rounded key configuration button");
        require(widget->layout()->itemAt(widget->layout()->count() - 1)->widget() == add,
                "add action follows the model list");
        for (auto* label : widget->findChildren<QLabel*>(QString(), Qt::FindDirectChildrenOnly)) {
            require(label->text() != QStringLiteral("OpenAI-compatible Chat Completions"),
                    "custom model heading has no subtitle");
        }
        EditorPaintObserver observer;
        application.installEventFilter(&observer);
        add->click();
        flush();
        application.removeEventFilter(&observer);
        require(observer.frames.size() == 1, "editor geometry is stable from its first paint");
        auto* modal = widget->findChild<AdModal*>(QStringLiteral("customAiModelEditor"));
        require(modal != nullptr, "add opens form");
        modal->acceptButton()->click();
        flush();
        require(session.customAiModels().isEmpty(), "empty submission does not create a record");
        auto* name = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("modelName"));
        auto* url = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("apiUrl"));
        auto* apiModel = modal->contentWidget()->findChild<AdComboBox*>(QStringLiteral("apiModel"));
        auto* key = modal->contentWidget()->findChild<AdPasswordEdit*>(QStringLiteral("apiKey"));
        require(name && url && apiModel && key && !key->textVisible(),
                "form has masked password input");
        const auto position = [modal](QWidget* control) {
            return control->mapTo(modal->contentWidget(), QPoint());
        };
        require(name->width() >= 250 && qAbs(name->width() - url->width()) <= 1 &&
                    name->width() == key->width(),
                "model inputs have equal usable column widths");
        require(position(name).x() == position(key).x() &&
                    position(url).x() == position(apiModel).x() &&
                    position(url).x() > position(name).x() &&
                    position(key).y() > position(name).y(),
                "editor arranges fields in two columns in reading order");
        const auto fields = modal->contentWidget()->findChildren<AdFormItem*>();
        require(fields.size() == 5, "editor has five labeled fields");
        for (auto* field : fields) {
            auto* tooltip = field->findChild<QLabel*>(QStringLiteral("ad-form-item-label-tooltip"));
            require(!field->tooltipText().isEmpty() && field->extraText().isEmpty() &&
                        tooltip != nullptr && !tooltip->isHidden() &&
                        tooltip->toolTip() == field->tooltipText(),
                    "every field exposes its description through a label tooltip");
        }
        name->setText(QStringLiteral("Personal model"));
        url->setText(QStringLiteral("http://localhost:1234/v1/"));
        apiModel->lineEdit()->setText(QStringLiteral("local-id"));
        emit apiModel->lineEdit()->textEdited(QStringLiteral("local-id"));
        key->setText(QStringLiteral("portable-secret"));
        require(apiModel->editable() &&
                    apiModel->currentValue().toString() == QStringLiteral("local-id"),
                "typed custom model ID commits without selecting an option");
        QTcpServer server;
        require(server.listen(QHostAddress::LocalHost), "model list server listens");
        QByteArray received;
        QList<QTcpSocket*> requests;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
            auto* socket = server.nextPendingConnection();
            QObject::connect(socket, &QTcpSocket::readyRead, &server, [&, socket]() {
                received += socket->readAll();
                if (received.contains("\r\n\r\n") && !requests.contains(socket)) {
                    requests.append(socket);
                }
            });
        });
        const auto waitFor = [](auto predicate) {
            QElapsedTimer timer;
            timer.start();
            while (!predicate() && timer.elapsed() < 3000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            }
            require(predicate(), "asynchronous model list operation completes");
        };
        const auto respond = [](QTcpSocket* socket, const QByteArray& payload) {
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(payload.size()) + "\r\nConnection: close\r\n\r\n" +
                          payload);
            socket->disconnectFromHost();
        };
        apiModel->hidePopup();
        url->setText(QStringLiteral("http://127.0.0.1:%1/v1/").arg(server.serverPort()));
        apiModel->showPopup();
        require(apiModel->loading(), "remote fetch uses the select loading effect");
        auto* fetchContent = apiModel->notFoundContentWidget();
        auto* fetchSpin = fetchContent->findChild<AdSpin*>(QStringLiteral("modelFetchSpin"));
        require(fetchSpin && fetchSpin->spinning() && fetchSpin->isVisible() &&
                    fetchSpin->sizeClass() == AdSpin::SizeClass::Small &&
                    !apiModel->view()->isVisible(),
                "remote fetch replaces empty results with a visible small Spin like Ant Design");
        require(apiModel->popupFooterWidget() == nullptr,
                "loading model popup does not reserve an empty status footer");
        waitFor([&]() { return requests.size() == 1; });
        require(fetchSpin->x() == fetchContent->layout()->contentsMargins().left() &&
                    fetchSpin->width() == fetchSpin->sizeHint().width() &&
                    fetchSpin->geometry().right() < fetchContent->width() / 2,
                "loading Spin stays at the left content inset at its natural size");
        require(received.startsWith("GET /v1/models HTTP/1.1") &&
                    received.contains("Authorization: Bearer portable-secret"),
                "models use the currently entered base URL and API key");
        respond(requests.last(),
                R"({"data":[{"id":"remote-model"},{"id":"remote-model"},{"id":""},{}]})");
        waitFor([&]() { return !apiModel->loading(); });
        require(apiModel->options().size() == 1 &&
                    apiModel->options().first().value.toString() ==
                        QStringLiteral("remote-model") &&
                    apiModel->currentValue().toString() == QStringLiteral("local-id"),
                "remote IDs are deduplicated without replacing custom input");
        require(apiModel->notFoundContentWidget() == nullptr && !fetchSpin->spinning() &&
                    apiModel->view()->isVisible(),
                "completed fetch removes Spin and restores the results list");
        require(apiModel->popupFooterWidget() == nullptr,
                "successful model popup does not reserve an empty status footer");
        require(apiModel->popupLayerMode() == AdComboBox::PopupLayerMode::QtTool &&
                    apiModel->view()->window()->isWindow() &&
                    apiModel->view()->window()->windowType() == Qt::Tool,
                "model list opens in a native Qt tool window");
        apiModel->setCurrentIndex(0);
        apiModel->hidePopup();
        require(apiModel->currentValue().toString() == QStringLiteral("remote-model"),
                "a fetched model can be selected");
        received.clear();
        apiModel->showPopup();
        require(!apiModel->loading() && apiModel->options().size() == 1 &&
                    apiModel->currentValue().toString() == QStringLiteral("remote-model"),
                "reopening the dropdown reuses successful results and preserves selection");
        flush();
        require(requests.size() == 1, "cached results do not issue another request");
        apiModel->hidePopup();
        key->setText(QStringLiteral("changed-secret"));
        require(apiModel->options().isEmpty(), "changing API key invalidates cached models");
        apiModel->showPopup();
        waitFor([&]() { return requests.size() == 2; });
        respond(requests.last(), "invalid json");
        waitFor([&]() { return !apiModel->loading(); });
        require(!qobject_cast<QLabel*>(apiModel->popupFooterWidget())->text().isEmpty(),
                "failed model lookup displays retry and custom input guidance");
        apiModel->hidePopup();
        received.clear();
        apiModel->showPopup();
        waitFor([&]() { return requests.size() == 3; });
        require(apiModel->popupFooterWidget() == nullptr,
                "retry removes the previous error footer from popup geometry");
        respond(requests.last(), R"({"data":[]})");
        waitFor([&]() { return !apiModel->loading(); });
        apiModel->hidePopup();
        apiModel->showPopup();
        require(!apiModel->loading() && apiModel->options().isEmpty(),
                "successful empty model lists are cached too");
        flush();
        require(requests.size() == 3, "empty cached results do not issue another request");
        apiModel->hidePopup();
        url->setText(QStringLiteral("http://127.0.0.1:%1/v2/").arg(server.serverPort()));
        apiModel->showPopup();
        waitFor([&]() { return requests.size() == 4; });
        url->setText(QStringLiteral("http://localhost:1234/v1/"));
        require(!apiModel->loading() && apiModel->options().isEmpty(),
                "changing connection cancels stale results");
        apiModel->hidePopup();
        key->setText(QStringLiteral("portable-secret"));
        apiModel->lineEdit()->setText(QStringLiteral("local-id"));
        emit apiModel->lineEdit()->textEdited(QStringLiteral("local-id"));
        require(apiModel->currentValue().toString() == QStringLiteral("local-id"),
                "custom input remains available after a failed lookup");
        key->setTextVisible(true);
        require(key->textVisible(), "key can be revealed");
        modal->contentWidget()
            ->findChild<AdSwitch*>(QStringLiteral("visionSupport"))
            ->setChecked(true);
        modal->acceptButton()->click();
        flush();
        require(session.customAiModels().size() == 1, "create persists one model");
        const auto original = session.customAiModels().first();
        auto* row = widget->findChild<QWidget*>(QStringLiteral("customAiModelRow:") + original.id);
        require(row != nullptr, "saved model has a list row");
        auto* visionTag = row->findChild<AdTag*>();
        auto* modelName = row->findChild<QLabel*>();
        require(visionTag != nullptr && visionTag->text() == QStringLiteral("Vision") &&
                    visionTag->x() ==
                        modelName->geometry().right() + 1 + row->layout()->spacing() &&
                    modelName->width() <= modelName->sizeHint().width(),
                "vision badge sits immediately after the model name at its natural width");
        modelName->setText(QString(200, u'W'));
        flush();
        require(modelName->width() < modelName->sizeHint().width() &&
                    visionTag->geometry().right() < row->width() &&
                    row->findChild<AdButton*>(QStringLiteral("copy:") + original.id)
                            ->geometry()
                            .right() < row->width(),
                "long model names shrink to keep the vision badge and actions inside the row");
        modelName->setText(original.name);
        flush();
        require(add->y() > row->y() && add->width() == row->width(),
                "add action spans the list beneath model cards");
        for (const auto& action :
             {QStringLiteral("edit"), QStringLiteral("delete"), QStringLiteral("copy")}) {
            auto* button = row->findChild<AdButton*>(action + u':' + original.id);
            require(button != nullptr && button->text().isEmpty() &&
                        button->sizeClass() == AdButton::SizeClass::Small &&
                        !button->toolTip().isEmpty() && !button->accessibleName().isEmpty(),
                    "model actions are small labeled accessible icon buttons");
        }
        require(original.apiKey == QStringLiteral("portable-secret") && original.supportsVision &&
                    original.model == QStringLiteral("local-id"),
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
        observer.frames.clear();
        application.installEventFilter(&observer);
        widget->findChild<AdButton*>(QStringLiteral("edit:") + original.id)->click();
        flush();
        application.removeEventFilter(&observer);
        require(observer.frames.size() == 1, "populated editor is stable from its first paint");
        modal = widget->findChild<AdModal*>(QStringLiteral("customAiModelEditor"));
        url = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("apiUrl"));
        apiModel = modal->contentWidget()->findChild<AdComboBox*>(QStringLiteral("apiModel"));
        key = modal->contentWidget()->findChild<AdPasswordEdit*>(QStringLiteral("apiKey"));
        url->setText(QStringLiteral("http://127.0.0.1:%1/v1/").arg(server.serverPort()));
        key->setText(QStringLiteral("changed-secret"));
        apiModel->showPopup();
        require(apiModel->loading(), "a new editor does not reuse the previous modal's cache");
        waitFor([&]() { return requests.size() == 5; });
        respond(requests.last(), R"({"data":[]})");
        waitFor([&]() { return !apiModel->loading(); });
        apiModel->hidePopup();
        url->setText(original.baseUrl);
        key->setText(original.apiKey);
        name = modal->contentWidget()->findChild<AdLineEdit*>(QStringLiteral("modelName"));
        name->setText(copied[1].name);
        modal->acceptButton()->click();
        flush();
        require(session.customAiModels().first() == original, "duplicate edit rejected");
        name->setText(QStringLiteral("Renamed model"));
        modal->contentWidget()
            ->findChild<AdSwitch*>(QStringLiteral("visionSupport"))
            ->setChecked(false);
        modal->acceptButton()->click();
        flush();
        require(session.customAiModels().first().id == original.id &&
                    session.customAiModels().first().name == QStringLiteral("Renamed model"),
                "rename preserves identity");
        require(widget->findChild<QWidget*>(QStringLiteral("customAiModelRow:") + original.id)
                        ->findChild<AdTag*>() == nullptr,
                "text-only models do not display a vision tag");
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
