#include "snow_shot/presentation/screenshotimageconversioncontroller.h"
#include "snow_shot/presentation/screenshotimageconversionpersistence.h"
#include "snow_shot/presentation/screenshotimageconversionview.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/translation/translationservice.h"
#include "theme/theme_manager.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/button.h"

#include <QApplication>
#include <QClipboard>
#include <QEventLoop>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTimer>
#include <QDir>
#include <QFontDatabase>
#include <QMimeData>
#include <QScrollBar>
#include <QKeyEvent>
#include <QLabel>

#include <cstdlib>
#include <functional>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void until(const std::function<bool()>& predicate) {
    if (predicate()) {
        return;
    }
    QEventLoop loop;
    QTimer poll;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (predicate()) {
            loop.quit();
        }
    });
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    poll.start(5);
    timeout.start(5000);
    loop.exec();
    require(predicate(), "asynchronous conversion test timed out");
}

class ConversionServer final : public QTcpServer {
  public:
    ConversionServer() {
        require(listen(QHostAddress::LocalHost), "vision fixture listens");
        connect(this, &QTcpServer::newConnection, this, [this]() {
            auto* socket = nextPendingConnection();
            auto buffer = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer]() {
                *buffer += socket->readAll();
                const auto end = buffer->indexOf("\r\n\r\n");
                if (end < 0 || socket->property("answered").toBool()) {
                    return;
                }
                qsizetype size = 0;
                for (const auto& line : buffer->left(end).split('\n')) {
                    if (line.toLower().startsWith("content-length:")) {
                        size = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                    }
                }
                if (buffer->size() < end + 4 + size) {
                    return;
                }
                socket->setProperty("answered", true);
                if (buffer->startsWith("GET ")) {
                    ++modelRequests;
                    respond(socket,
                            QJsonDocument(QJsonObject{{QStringLiteral("code"), 0},
                                                      {QStringLiteral("data"), models}})
                                .toJson(QJsonDocument::Compact),
                            "application/json");
                } else {
                    requests.push_back(QJsonDocument::fromJson(buffer->mid(end + 4)).object());
                    if (hold) {
                        held = socket;
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: "
                                      "text/event-stream\r\nConnection: close\r\n\r\n");
                        socket->write(frame(QStringLiteral("# Partial")));
                        socket->flush();
                    } else {
                        respond(socket, frame(source) + "data: [DONE]\n\n", "text/event-stream");
                    }
                }
            });
        });
        for (const QString& id :
             {QStringLiteral("text"), QStringLiteral("vision-a"), QStringLiteral("vision-b")}) {
            models.push_back(
                QJsonObject{{QStringLiteral("model"), id},
                            {QStringLiteral("name"), id},
                            {QStringLiteral("supports_vision"), id != QStringLiteral("text")}});
        }
    }
    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }
    static QByteArray frame(const QString& source) {
        const QJsonObject delta{{QStringLiteral("content"), source}};
        const QJsonArray choices{QJsonObject{{QStringLiteral("delta"), delta}}};
        return "data: " +
               QJsonDocument(QJsonObject{{QStringLiteral("choices"), choices}})
                   .toJson(QJsonDocument::Compact) +
               "\n\n";
    }
    static void respond(QTcpSocket* socket, const QByteArray& body, const QByteArray& type) {
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }
    int modelRequests = 0;
    QVector<QJsonObject> requests;
    QJsonArray models;
    QString source = QStringLiteral("# Hello\n\n| Name | Value |\n| --- | --- |\n| Item | 42 |\n");
    bool hold = false;
    QPointer<QTcpSocket> held;
};

QImage sampleImage() {
    QImage image(240, 120, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    return image;
}

void settingsInitialAvailability() {
    ConversionServer server;
    SnowShotApiClient api(server.url());
    QWidget owner;
    owner.resize(640, 480);
    owner.show();
    for (auto* provider : {&api, static_cast<SnowShotApiClient*>(nullptr)}) {
        ScreenshotImageConversionController controller;
        controller.setProvider(provider);
        controller.openSettings(&owner);
        auto* modal = controller.findChild<adqt::widgets::AdModal*>();
        require(modal && modal->acceptButton() && !modal->acceptButton()->isEnabled(),
                "loading and unavailable providers disable OK on first display");
        auto* select = modal->contentWidget()->findChild<adqt::widgets::AdSelect*>();
        require(select && !select->isEnabled(),
                "model selection is disabled until models are available");
        modal->reject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

void conversionCacheUsesRecognitionKeys() {
    using Controller = ScreenshotImageConversionController;
    using Format = SnowShotImageConversionFormat;
    snow_shot::storage::ScreenshotImageConversionSettings().setVisionModel(
        QStringLiteral("vision-a"));
    ConversionServer server;
    SnowShotApiClient api(server.url());
    Controller controller;
    controller.setProvider(&api);
    const QString originalKey = QStringLiteral("1:0,0,240,120");
    QImage changedImage = sampleImage();
    changedImage.setPixelColor(0, 0, Qt::black);
    for (const Format format : {Format::Markdown, Format::Html}) {
        const auto before = server.requests.size();
        controller.activate(originalKey, sampleImage(), format);
        until([&]() { return controller.state() == Controller::State::Completed; });
        require(server.requests.size() == before + 1, "each format has its own completed result");
        const QString source = controller.source();
        controller.deactivate();
        controller.activate(originalKey, changedImage, format);
        require(controller.state() == Controller::State::Completed &&
                    controller.source() == source && server.requests.size() == before + 1,
                "matching recognition identity reuses output independently of pixels");
        for (const auto& key : {QStringLiteral("1:1,0,240,120"), QStringLiteral("2:0,0,240,120")}) {
            const auto requests = server.requests.size();
            controller.activate(key, sampleImage(), format);
            until([&]() { return controller.state() == Controller::State::Completed; });
            require(server.requests.size() == requests + 1,
                    "selection and capture session changes use separate cache entries");
        }
        const auto requests = server.requests.size();
        controller.activate(originalKey, changedImage, format);
        require(controller.state() == Controller::State::Completed &&
                    server.requests.size() == requests,
                "returning to a cached selection reuses its result");
        Controller restored;
        restored.seed(originalKey, controller.entries(originalKey));
        restored.activate(originalKey, changedImage, format);
        require(restored.state() == Controller::State::Completed && restored.source() == source,
                "both formats restore by recognition key without a provider");
    }
}

void conversionLifecycleAndSettings() {
    using Controller = ScreenshotImageConversionController;
    using Format = SnowShotImageConversionFormat;
    const snow_shot::storage::ScreenshotImageConversionSettings settings;
    settings.setVisionModel({});
    ConversionServer server;
    SnowShotApiClient api(server.url());
    Controller controller;
    controller.setProvider(&api);
    controller.activate(QStringLiteral("image-one"), sampleImage(), Format::Markdown);
    until([&]() { return controller.state() == Controller::State::Completed; });
    require(settings.visionModel() == QStringLiteral("vision-a") && server.requests.size() == 1 &&
                controller.source() == server.source,
            "first activation picks the first vision model and streams source without OCR");
    const auto saved = controller.entries(QStringLiteral("image-one"));
    controller.deactivate();
    controller.activate(QStringLiteral("image-one"), sampleImage(), Format::Markdown);
    require(controller.state() == Controller::State::Completed && server.requests.size() == 1,
            "matching completed conversion is reused without a request");
    QWidget owner;
    owner.resize(640, 480);
    owner.show();
    controller.openSettings(&owner);
    auto* modal = controller.findChild<adqt::widgets::AdModal*>();
    require(modal != nullptr, "conversion settings opens a modal");
    const QRect initialGeometry = modal->contentWidget()->window()->geometry();
    const QRect initialBodyGeometry = modal->contentWidget()->geometry();
    require(modal->contentWidget()->height() == modal->contentWidget()->sizeHint().height(),
            "settings opens with content-sized height without unused alert space");
    QCoreApplication::processEvents();
    require(modal->contentWidget()->window()->geometry() == initialGeometry,
            "settings geometry is stable after first display");
    require(modal->contentWidget()->geometry() == initialBodyGeometry,
            "settings content geometry is stable after first display");
    require(modal->acceptButton()->isEnabled(), "cached vision models enable OK on first display");
    auto* select = modal->contentWidget()->findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotVisionModel"));
    require(select && select->options().size() == 2 &&
                select->options().first().value == QStringLiteral("vision-a") &&
                select->popupLayerMode() == adqt::widgets::AdSelect::PopupLayerMode::QtTool,
            "settings offers only vision models in server order using a tool popup");
    const QString snapshots = qEnvironmentVariable("SNOW_SHOT_CONVERSION_SNAPSHOTS");
    if (!snapshots.isEmpty()) {
        require(QDir().mkpath(snapshots), "create settings snapshot directory");
        QCoreApplication::processEvents();
        require(modal->contentWidget()->window()->grab().save(
                    QDir(snapshots).filePath(QStringLiteral("vision-settings.png"))),
                "save settings snapshot");
    }
    select->setCurrentValue(QStringLiteral("vision-b"));
    modal->closeRequested(adqt::widgets::AdModal::CloseReason::CancelAction);
    require(settings.visionModel() == QStringLiteral("vision-a") && server.requests.size() == 1,
            "cancel does not save settings or rerun conversion");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    controller.openSettings(&owner);
    modal = controller.findChild<adqt::widgets::AdModal*>();
    select = modal->contentWidget()->findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotVisionModel"));
    select->setCurrentValue(QStringLiteral("vision-b"));
    modal->closeRequested(adqt::widgets::AdModal::CloseReason::OkAction);
    until([&]() { return controller.state() == Controller::State::Completed; });
    require(settings.visionModel() == QStringLiteral("vision-b") && server.requests.size() == 2 &&
                server.requests.last().value(QStringLiteral("model")) == QStringLiteral("vision-b"),
            "accept persists the shared model and reruns only the active window");
    server.source = QStringLiteral("```html\n<h1>Hello</h1>\n```");
    controller.activate(QStringLiteral("image-one"), sampleImage(), Format::Html);
    until([&]() { return controller.state() == Controller::State::Completed; });
    require(controller.source() == QStringLiteral("<h1>Hello</h1>") &&
                controller.entries(QStringLiteral("image-one")).size() == 2,
            "formats have independent results and only the explicit outer fence is removed");
    server.hold = true;
    controller.activate(QStringLiteral("image-two"), sampleImage(), Format::Markdown);
    until([&]() { return controller.source() == QStringLiteral("# Partial"); });
    require(controller.entries(QStringLiteral("image-two")).isEmpty(),
            "partial output is not a completed cache entry");
    controller.deactivate();
    require(controller.state() == Controller::State::Idle && !controller.busy(),
            "deactivation cancels unfinished work");
    server.hold = false;
    controller.activate(QStringLiteral("image-three"), sampleImage(), Format::Html);
    until([&]() { return controller.state() == Controller::State::Completed; });
    require(controller.entries(QStringLiteral("image-two")).isEmpty(),
            "cancelled target has no completed cache");
    Controller restored;
    restored.seed(QStringLiteral("image-one"), saved);
    settings.setVisionModel(QStringLiteral("vision-a"));
    restored.activate(QStringLiteral("image-one"), sampleImage(), Format::Markdown);
    require(restored.state() == Controller::State::Completed,
            "persisted result restores without an API provider");
    QImage changedImage = sampleImage();
    changedImage.setPixelColor(0, 0, Qt::black);
    restored.activate(QStringLiteral("image-one"), changedImage, Format::Markdown);
    require(restored.state() == Controller::State::Completed,
            "the same recognition key reuses persisted output even when pixels differ");
    server.models = QJsonArray{server.models.first()};
    controller.retry();
    until([&]() { return controller.state() == Controller::State::Failed; });
    require(controller.error().contains(QStringLiteral("No vision models")),
            "empty vision catalog has a distinct error");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    controller.openSettings(&owner);
    modal = controller.findChild<adqt::widgets::AdModal*>();
    require(modal && modal->acceptButton() && !modal->acceptButton()->isEnabled(),
            "settings cannot accept an empty vision catalog");
    controller.deactivate();
}

void customModelWorkflows() {
    using Controller = ScreenshotImageConversionController;
    using Format = SnowShotImageConversionFormat;
    ConversionServer server;
    SnowShotApiClient api(QStringLiteral("http://127.0.0.1:1"));
    snow_shot::CustomAiModelConfiguration model{QUuid::createUuid().toString(QUuid::WithoutBraces),
                                                QStringLiteral("Custom Vision"),
                                                server.url() + QStringLiteral("/v1"),
                                                {},
                                                QStringLiteral("local-vision"),
                                                true};
    auto second = model;
    second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    second.name = QStringLiteral("Other Model");
    const snow_shot::storage::ApiConfigurationSettings apiSettings;
    const auto previousModels = apiSettings.customModels();
    require(apiSettings.setCustomModels({model, second}), "configure shared custom models");
    snow_shot::translation::TranslationService::forClient(
        api, snow_shot::storage::ApplicationStorage::instance().configuration(), QLocale::English);
    const snow_shot::storage::ScreenshotImageConversionSettings settings;
    settings.setVisionModel(model.selectionId());
    Controller controller;
    controller.setProvider(&api);
    controller.activate(QStringLiteral("custom-image"), sampleImage(), Format::Markdown);
    until([&]() { return controller.state() == Controller::State::Completed; });
    require(server.modelRequests == 0 && server.requests.size() == 1,
            "selected custom model runs without builtin catalog");
    const auto cached = controller.entries(QStringLiteral("custom-image"));
    require(!cached.first().modelFingerprint.isEmpty(),
            "custom conversion records connection identity");
    model.name = QStringLiteral("Renamed Vision");
    require(apiSettings.setCustomModels({model, second}), "rename shared custom model");
    controller.activate(QStringLiteral("custom-image"), sampleImage(), Format::Markdown);
    require(server.requests.size() == 1 && controller.state() == Controller::State::Completed,
            "rename preserves completed conversion");
    model.model = QStringLiteral("changed-provider-id");
    require(apiSettings.setCustomModels({model, second}), "edit shared custom model connection");
    require(controller.entries(QStringLiteral("custom-image")).isEmpty() && !controller.busy(),
            "connection edit invalidates results without automatic request");
    controller.seed(QStringLiteral("custom-image"), cached);
    require(controller.entries(QStringLiteral("custom-image")).isEmpty(),
            "stale restored custom result cannot be reseeded");
    controller.retry();
    until([&]() { return controller.state() == Controller::State::Completed; });
    require(server.requests.size() == 2 &&
                server.requests.last().value(QStringLiteral("model")) == model.model,
            "retry uses edited provider model");
    QWidget owner;
    owner.resize(800, 700);
    owner.show();
    controller.openSettings(&owner);
    auto* modal = controller.findChild<adqt::widgets::AdModal*>();
    auto* select = modal->contentWidget()->findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotVisionModel"));
    require(select && select->isEnabled() && select->options().size() == 2,
            "custom conversion choices available while builtin catalog loads");
    model.supportsVision = false;
    require(apiSettings.setCustomModels({model, second}),
            "update shared custom model capabilities");
    require(select->options().size() == 1 && settings.visionModel() == second.selectionId(),
            "vision removal updates open selector and shared fallback");
    modal->reject();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    ScreenshotRecognitionSessionActions actions;
    actions.translationSettingsOwner = [&]() { return &owner; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, actions);
    session.openTranslationSettings();
    auto* translation = session.findChild<adqt::widgets::AdModal*>();
    require(translation != nullptr, "translation settings opens with local models");
    const auto selects = translation->contentWidget()->findChildren<adqt::widgets::AdSelect*>();
    bool found = false;
    for (auto* service : selects) {
        bool hasModel = false, hasVision = false;
        for (const auto& option : service->options()) {
            hasModel = hasModel || option.value == model.selectionId();
            hasVision = hasVision || option.value == second.selectionId();
        }
        found = found || (hasModel && hasVision);
    }
    require(found, "translation includes both text and vision custom models");
    translation->reject();
    server.hold = true;
    Controller other;
    other.setProvider(&api);
    controller.activate(QStringLiteral("active-one"), sampleImage(), Format::Html);
    other.activate(QStringLiteral("active-two"), sampleImage(), Format::Html);
    until([&]() { return server.requests.size() == 4; });
    require(apiSettings.setCustomModels({model}), "delete shared custom model");
    require(!controller.busy() && !other.busy() && settings.visionModel().isEmpty(),
            "deletion cancels all active consumers and clears unavailable vision selection");
    settings.setVisionModel({});
    require(apiSettings.setCustomModels(previousModels), "restore shared custom models");
}

void conversionSourceNormalization() {
    using Format = SnowShotImageConversionFormat;
    const auto check = [](const QString& input, Format format, const QString& expected) {
        const QString actual = normalizedImageConversionSource(input, format);
        require(actual == expected, "conversion normalization preserves document content");
        require(normalizedImageConversionSource(actual, format) == actual,
                "normalized document is stable on repeated normalization");
    };
    check(QStringLiteral("``` HTML\r\n<p>Hello</p>\r\n```  "), Format::Html,
          QStringLiteral("<p>Hello</p>"));
    check(QStringLiteral("~~~md\n# Hello\n~~~~"), Format::Markdown, QStringLiteral("# Hello"));
    check(QStringLiteral("````markdown\n```cpp\nint x;\n```\n````"), Format::Markdown,
          QStringLiteral("```cpp\nint x;\n```"));
    check(QStringLiteral("```html\n```"), Format::Html, {});
    check(QStringLiteral("~~~md\r\n  \r\n~~~"), Format::Markdown, QStringLiteral("  "));
    check(QString(QChar(0xfeff)) + QStringLiteral("# Hello"), Format::Markdown,
          QStringLiteral("# Hello"));
    for (const QString& source :
         {QStringLiteral("```cpp\ncode\n```"), QStringLiteral("```\n# Heading\n```"),
          QStringLiteral("```markdown\n# One\n```\nText\n```markdown\n# Two\n```"),
          QStringLiteral("```md\n# Missing closing fence"),
          QStringLiteral("````md\n# Short closing fence\n```"),
          QStringLiteral("```md\n# Heading\n```\nCommentary"),
          QStringLiteral("  # Heading\n\n    indented code\n")}) {
        check(source, Format::Markdown, source);
    }
    const QString wrongFormat = QStringLiteral("```html\n<p>Hello</p>\n```");
    check(wrongFormat, Format::Markdown, wrongFormat);

    ConversionServer server;
    server.source = QStringLiteral("~~~markdown\r\n  \r\n~~~");
    SnowShotApiClient api(server.url());
    ScreenshotImageConversionController controller;
    controller.setProvider(&api);
    controller.activate(QStringLiteral("empty-wrapper"), sampleImage(), Format::Markdown);
    until(
        [&]() { return controller.state() == ScreenshotImageConversionController::State::Failed; });
    require(controller.entries(QStringLiteral("empty-wrapper")).isEmpty(),
            "empty normalized output is never cached as a completed conversion");
    server.source = QStringLiteral("~~~md\n# Recovered\n~~~");
    controller.retry();
    until([&]() {
        return controller.state() == ScreenshotImageConversionController::State::Completed;
    });
    require(controller.source() == QStringLiteral("# Recovered") &&
                controller.entries(QStringLiteral("empty-wrapper")).size() == 1,
            "retry replaces unusable output with a normalized completed result");
}

void renderingCopyAndPersistence() {
    using Format = SnowShotImageConversionFormat;
    const QString markdown = QStringLiteral("# Heading\n\nA **bold** paragraph.\n\n| A | B |\n| "
                                            "--- | --- |\n| 1 | 2 |\n\n```cpp\nint n = 42;\n```\n");
    ScreenshotImageConversionView view;
    view.resize(480, 240);
    view.show();
    view.setContent(Format::Markdown, markdown, false, {});
    auto* browser = view.findChild<QTextBrowser*>();
    require(browser && browser->toPlainText().contains(QStringLiteral("Heading")) &&
                !browser->toPlainText().contains(QStringLiteral("**bold**")) &&
                browser->toPlainText().contains(QStringLiteral("int n = 42;")),
            "Markdown preview renders structure and preserves fenced code");
    require(view.copyToClipboard() && QApplication::clipboard()->text() == markdown,
            "copy without selection preserves exact Markdown source");
    QTextCursor cursor(browser->document());
    cursor.setPosition(0);
    cursor.setPosition(7, QTextCursor::KeepAnchor);
    browser->setTextCursor(cursor);
    require(view.copyToClipboard() &&
                QApplication::clipboard()->text() == QStringLiteral("Heading"),
            "selected preview text copies as ordinary text");
    view.setContent(Format::Markdown, markdown + QStringLiteral("\nMore"), true, {});
    require(browser->textCursor().selectedText() == QStringLiteral("Heading"),
            "streaming preserves text selection");
    const QString html = QStringLiteral(
        "<h1>Title</h1><table><tr><th>A</th></tr><tr><td>42</td></tr></table>"
        "<img src='file:///C:/Windows/win.ini'><a href='javascript:alert(1)'>Link</a>");
    view.setContent(Format::Html, html, false, {});
    require(browser->toPlainText().contains(QStringLiteral("42")) &&
                !browser->document()
                     ->resource(QTextDocument::ImageResource,
                                QUrl(QStringLiteral("file:///C:/Windows/win.ini")))
                     .isValid(),
            "HTML renders tables without loading local resources");
    bool opened = false;
    QObject::connect(&view, &ScreenshotImageConversionView::linkActivated, &view,
                     [&](const QUrl&) { opened = true; });
    browser->anchorClicked(QUrl(QStringLiteral("javascript:alert(1)")));
    require(!opened, "non-web link schemes are inert");
    browser->anchorClicked(QUrl(QStringLiteral("https://example.com")));
    require(opened, "explicit web link clicks are forwarded");
    require(
        normalizedImageConversionSource(QStringLiteral("```cpp\ncode\n```"), Format::Markdown) ==
            QStringLiteral("```cpp\ncode\n```"),
        "ordinary code fences survive normalization");
    ScreenshotRecognitionResults results;
    results.key = QStringLiteral("sample");
    results.conversions = {{Format::Markdown, QStringLiteral("vision-a"), markdown},
                           {Format::Html, QStringLiteral("vision-a"), html}};
    results.visibleConversion = Format::Html;
    const QByteArray payload = snow_shot::presentation::encodeImageConversions(results);
    ScreenshotRecognitionResults restored;
    snow_shot::presentation::decodeImageConversions(payload, restored);
    require(restored.conversions.size() == 2 && restored.conversions.first().source == markdown &&
                restored.visibleConversion == Format::Html,
            "conversion formats and visible state round-trip exactly");
    auto legacyRoot = QJsonDocument::fromJson(payload).object();
    auto legacyEntries = legacyRoot.value(QStringLiteral("entries")).toArray();
    for (qsizetype index = 0; index < legacyEntries.size(); ++index) {
        auto entry = legacyEntries.at(index).toObject();
        require(!entry.contains(QStringLiteral("image")), "new payloads omit image hashes");
        entry.insert(QStringLiteral("image"), QString(64, u'a'));
        legacyEntries[index] = entry;
    }
    legacyRoot.insert(QStringLiteral("entries"), legacyEntries);
    snow_shot::presentation::decodeImageConversions(QJsonDocument(legacyRoot).toJson(), restored);
    require(restored.conversions.size() == 2 && restored.conversions.first().source == markdown &&
                restored.visibleConversion == Format::Html,
            "legacy image hashes are ignored when restoring conversions");
    ScreenshotRecognitionResults invalid;
    auto invalidEntry = legacyEntries.first().toObject();
    invalidEntry.insert(QStringLiteral("prompt_version"), 99);
    legacyRoot.insert(QStringLiteral("entries"), QJsonArray{invalidEntry});
    snow_shot::presentation::decodeImageConversions(QJsonDocument(legacyRoot).toJson(), invalid);
    require(invalid.conversions.isEmpty() && !invalid.visibleConversion,
            "unsupported prompt versions remain invalid without image hashes");
    restored.qr = ScreenshotQrRecognitionResult{};
    snow_shot::presentation::decodeImageConversions(QByteArrayLiteral("{invalid"), restored);
    require(restored.qr.has_value() && restored.conversions.size() == 2,
            "malformed optional data preserves recognition results");
}

void conversionSessionRoutesSourceAndClearsOldViews() {
    ConversionServer server;
    SnowShotApiClient api(server.url());
    int captureCopies = 0;
    ScreenshotRecognitionWindowActions windowActions;
    windowActions.handleCopy = [&]() { ++captureCopies; };
    ScreenshotRecognitionWindow window(windowActions);
    window.show();
    window.activateWindow();
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, actions);
    session.setProviders(nullptr, nullptr, nullptr);
    session.setTarget({QStringLiteral("selection"), sampleImage(), QRectF(0, 0, 240, 120)});
    session.activate(ScreenshotRecognitionSessionController::Mode::Markdown);
    until([&]() { return session.recognitionResultsSnapshot().visibleConversion.has_value(); });
    auto* browser =
        window.findChild<QTextBrowser*>(QStringLiteral("screenshotImageConversionPreview"));
    require(browser != nullptr, "screenshot session displays the streamed rendered preview");
    browser->selectAll();
    require(session.recognitionClipboardMimeData()->text() == server.source,
            "toolbar Copy returns source even with a preview selection");
    require(window.copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() != server.source,
            "preview Copy instead returns selected rendered text");
    browser->setFocus();
    QCoreApplication::processEvents();
    QApplication::clipboard()->clear();
    QKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(browser, &copy);
    require(copy.isAccepted() && captureCopies == 0 &&
                QApplication::clipboard()->text() == browser->toPlainText(),
            "preview keyboard Copy copies selected text without ending screenshot capture");
    window.showQrContents({QStringLiteral("QR content")});
    require(!window.findChild<ScreenshotImageConversionView*>() &&
                window.copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() == QStringLiteral("QR content"),
            "switching recognition presentations cannot keep a stale conversion clipboard route");
    session.deactivate();
    require(!session.recognitionResultsSnapshot().visibleConversion &&
                session.cachedRecognitionResults().conversions.size() == 1,
            "session deactivation keeps completed source but not visible state");
}

void conversionUsesRecognitionMessages() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    ConversionServer server;
    server.hold = true;
    SnowShotApiClient api(server.url());
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    window.show();
    QStringList prompts;
    bool loading = false;
    int dismissals = 0;
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    actions.showRecognition = [&](const QString& message) {
        prompts.push_back(message);
        loading = true;
    };
    actions.hideLoading = [&]() {
        loading = false;
        ++dismissals;
    };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, actions);
    session.setTarget({QStringLiteral("messages"), sampleImage(), QRectF(0, 0, 240, 120)});
    for (const Mode mode : {Mode::Markdown, Mode::Html}) {
        const auto promptCount = prompts.size();
        session.activate(mode);
        require(loading && prompts.size() == promptCount + 1 &&
                    prompts.last() == (mode == Mode::Markdown
                                           ? QStringLiteral("Converting to Markdown")
                                           : QStringLiteral("Converting to HTML")),
                "both formats start the OCR message callback while loading models");
        until([&]() {
            const auto* browser =
                window.findChild<QTextBrowser*>(QStringLiteral("screenshotImageConversionPreview"));
            return browser && browser->toPlainText().contains(QStringLiteral("Partial"));
        });
        auto* status = window.findChild<QLabel*>(QStringLiteral("screenshotImageConversionStatus"));
        auto* retry = window.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotImageConversionRetry"));
        require(loading && prompts.size() == promptCount + 1 && status && !status->isVisible() &&
                    retry && !retry->isVisible(),
                "streaming keeps one message without a duplicate inline spinner or Retry");
        server.held->write(ConversionServer::frame(QStringLiteral(" more")));
        server.held->flush();
        until([&]() {
            return session.recognitionClipboardMimeData()->text().endsWith(QStringLiteral(" more"));
        });
        const int beforeCompletion = dismissals;
        server.held->write("data: [DONE]\n\n");
        server.held->disconnectFromHost();
        until([&]() { return !session.busy(); });
        require(!loading && dismissals == beforeCompletion + 1 && prompts.size() == promptCount + 1,
                "completion dismisses the shared message exactly once despite streamed deltas");
        session.activate(mode);
        require(!loading && prompts.size() == promptCount + 1 && !session.busy(),
                "cached previews do not flash a loading message");
    }

    session.setTarget({QStringLiteral("interrupted"), sampleImage(), QRectF(0, 0, 240, 120)});
    session.activate(Mode::Markdown);
    until([&]() { return server.requests.size() == 3 && server.held; });
    server.held->disconnectFromHost();
    until([&]() { return !session.busy(); });
    auto* retry = window.findChild<adqt::widgets::AdButton*>(
        QStringLiteral("screenshotImageConversionRetry"));
    auto* status = window.findChild<QLabel*>(QStringLiteral("screenshotImageConversionStatus"));
    require(!loading && retry && retry->isVisible() && status && status->isVisible() &&
                status->text().contains(QStringLiteral("Incomplete result:")),
            "interrupted conversion dismisses loading and retains the inline error and Retry");
    const auto beforeRetry = prompts.size();
    retry->click();
    require(loading && prompts.size() == beforeRetry + 1 && !retry->isVisible(),
            "Retry starts a fresh message and hides the previous error row");
    session.activate(Mode::Html);
    require(loading && prompts.size() == beforeRetry + 2 &&
                prompts.last() == QStringLiteral("Converting to HTML"),
            "switching formats replaces the cancelled request's message");
    session.activate(Mode::Qr);
    require(!loading, "switching to another recognition tool dismisses conversion loading");
    session.activate(Mode::Html);
    require(loading, "conversion can start again after switching tools");
    session.deactivate();
    require(!loading && !session.busy(), "closing recognition cancels and dismisses loading");
    session.activate(Mode::Html);
    session.invalidate();
    require(!loading && !session.busy(), "changing the capture clears conversion loading");

    server.hold = false;
    server.models = QJsonArray{server.models.first()};
    SnowShotApiClient emptyApi(server.url());
    ScreenshotRecognitionSessionController empty(nullptr, nullptr, &emptyApi, actions);
    empty.setTarget({QStringLiteral("no-models"), sampleImage(), QRectF(0, 0, 240, 120)});
    empty.activate(Mode::Markdown);
    require(loading, "model discovery is covered by the same message");
    until([&]() { return !empty.busy(); });
    require(!loading, "model discovery failure dismisses loading");
}

void previewThemesAndLongDocuments() {
    using namespace adqt::theme;
    const auto previousTheme = ThemeManager::instance().config();
    const QString snapshots = qEnvironmentVariable("SNOW_SHOT_CONVERSION_SNAPSHOTS");
    if (!snapshots.isEmpty()) {
        require(QDir().mkpath(snapshots), "create preview snapshot directory");
    }
    const QString markdown =
        QStringLiteral("# Quarterly report\n\nA short document with **emphasis**, [a "
                       "reference](https://example.com), "
                       "and a faithful reading order.\n\n## Results\n\n| Item | Quantity | Status "
                       "|\n| --- | ---: | --- |\n"
                       "| Screenshots | 42 | Complete |\n| Documents | 18 | Reviewed |\n\n"
                       "- Preserve headings and lists\n- Keep original text and numbers\n\n"
                       "```cpp\nconst int converted = 42;\nreturn converted;\n```\n\n> A note from "
                       "the original document.\n");
    for (const auto scheme : {ThemeScheme::Light, ThemeScheme::Dark}) {
        ThemeManager::instance().setColorScheme(scheme);
        const QString suffix =
            scheme == ThemeScheme::Light ? QStringLiteral("light") : QStringLiteral("dark");
        for (const auto format :
             {SnowShotImageConversionFormat::Markdown, SnowShotImageConversionFormat::Html}) {
            ScreenshotImageConversionView view;
            view.resize(580, 490);
            view.show();
            const QString source =
                format == SnowShotImageConversionFormat::Markdown
                    ? markdown
                    : QStringLiteral("<h1>Quarterly report</h1><p>A short document with "
                                     "<b>emphasis</b> and a faithful reading order.</p>"
                                     "<h2>Results</h2><table><tr><th>Item</th><th>Quantity</"
                                     "th><th>Status</th></tr>"
                                     "<tr><td>Screenshots</td><td>42</td><td>Complete</td></"
                                     "tr><tr><td>Documents</td><td>18</td>"
                                     "<td>Reviewed</td></tr></table><ul><li>Preserve headings and "
                                     "lists</li><li>Keep original text</li></ul>"
                                     "<pre><code>const int converted = 42;\nreturn "
                                     "converted;</code></pre><blockquote>A note from the original "
                                     "document.</blockquote>");
            view.setContent(format, source, false, {});
            QCoreApplication::processEvents();
            auto* browser = view.findChild<QTextBrowser*>();
            require(browser && browser->palette().color(QPalette::Base) ==
                                   ThemeManager::instance().resolveTheme(browser).colorBgContainer,
                    "preview follows the active theme");
            if (!snapshots.isEmpty()) {
                const QString prefix = format == SnowShotImageConversionFormat::Markdown
                                           ? QStringLiteral("markdown-")
                                           : QStringLiteral("html-");
                require(view.grab().save(
                            QDir(snapshots).filePath(prefix + suffix + QStringLiteral(".png"))),
                        "save preview snapshot");
            }
            view.resize(260, 180);
            view.setContent(format, source.repeated(20), false,
                            QStringLiteral("Connection interrupted"));
            QCoreApplication::processEvents();
            require(browser->verticalScrollBar()->maximum() > 0,
                    "small previews scroll long documents");
            browser->verticalScrollBar()->setValue(25);
            view.setContent(format, source.repeated(21), true, {});
            require(browser->verticalScrollBar()->value() == 25,
                    "streaming does not force a scrolled reader to the bottom");
        }
    }
    ThemeManager::instance().setConfig(previousTheme);
}

void conversionToolbarMigration() {
    namespace storage = snow_shot::storage;
    namespace layout = snow_shot::presentation::toolbar_layout;
    const storage::ScreenshotToolbarLayout original{
        {{QStringLiteral("text-recognition")},
         {QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")}},
        {QStringLiteral("save-as-file")}};
    const auto migrated =
        layout::normalizedLayout(original, storage::ScreenshotToolbarLayoutKind::ActionTools);
    require(migrated.positions.at(0) == original.positions.at(0) &&
                migrated.positions.at(1) == QStringList{QStringLiteral("barcode-recognition"),
                                                        QStringLiteral("table-recognition"),
                                                        QStringLiteral("convert-to-markdown"),
                                                        QStringLiteral("convert-to-html")} &&
                migrated.hidden == original.hidden,
            "older layouts gain conversions inside recognition without rearranging other tools");
    const storage::ScreenshotToolbarSettings settings;
    settings.setLayout(storage::ScreenshotToolbarLayoutKind::ActionTools, original);
    const auto persisted = settings.layout(storage::ScreenshotToolbarLayoutKind::ActionTools);
    require(persisted.positions == migrated.positions && persisted.hidden == migrated.hidden,
            "storage and presentation apply identical layout migration");
    auto hidden = migrated;
    hidden.positions[1].removeAll(QStringLiteral("convert-to-markdown"));
    hidden.hidden.push_back(QStringLiteral("convert-to-markdown"));
    const auto retained =
        layout::normalizedLayout(hidden, storage::ScreenshotToolbarLayoutKind::ActionTools);
    require(retained.hidden.contains(QStringLiteral("convert-to-markdown")),
            "explicitly hidden conversion actions stay hidden");

    const auto verify = [&](const storage::ScreenshotToolbarLayout& input,
                            const storage::ScreenshotToolbarLayout& expected) {
        const auto result =
            layout::normalizedLayout(input, storage::ScreenshotToolbarLayoutKind::ActionTools);
        require(result == expected, "recognition group migration matches the expected layout");
        require(layout::normalizedLayout(
                    result, storage::ScreenshotToolbarLayoutKind::ActionTools) == result,
                "recognition group migration is idempotent");
        settings.setLayout(storage::ScreenshotToolbarLayoutKind::ActionTools, input);
        require(settings.layout(storage::ScreenshotToolbarLayoutKind::ActionTools) == result,
                "storage and presentation agree on every migration branch");
    };
    verify(hidden, retained);
    storage::ScreenshotToolbarLayout defaults{layout::actionDefaultPositions(), {}};
    auto previousDefault = defaults;
    previousDefault.positions[0] = {QStringLiteral("barcode-recognition"),
                                    QStringLiteral("table-recognition")};
    previousDefault.positions.insert(1, QStringList{QStringLiteral("convert-to-markdown")});
    previousDefault.positions.insert(2, QStringList{QStringLiteral("convert-to-html")});
    verify(previousDefault, defaults);
    auto previousGroupedDefault = defaults;
    previousGroupedDefault.positions[0] = {
        QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
        QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")};
    verify(previousGroupedDefault, defaults);
    auto customGrouped = previousGroupedDefault;
    customGrouped.positions.move(0, 1);
    verify(customGrouped, customGrouped);
    verify({}, defaults);
    auto customized = previousDefault;
    customized.positions.move(1, customized.positions.size() - 1);
    verify(customized, customized);
    auto qrHidden = original;
    qrHidden.positions[1].removeAll(QStringLiteral("barcode-recognition"));
    qrHidden.hidden.push_back(QStringLiteral("barcode-recognition"));
    auto tableGroup = migrated;
    tableGroup.positions[1].removeAll(QStringLiteral("barcode-recognition"));
    tableGroup.hidden.push_back(QStringLiteral("barcode-recognition"));
    verify(qrHidden, tableGroup);
    auto recognitionHidden = defaults;
    recognitionHidden.positions.removeFirst();
    recognitionHidden.hidden = {QStringLiteral("barcode-recognition"),
                                QStringLiteral("table-recognition")};
    auto conversionsOnly = recognitionHidden;
    conversionsOnly.positions.push_back(
        {QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown")});
    verify(recognitionHidden, conversionsOnly);
}
} // namespace

void runImageConversionTests() {
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "offscreen conversion tests require a font");
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/consola.ttf")) >= 0,
            "offscreen conversion tests require a monospace font");
    auto theme = adqt::theme::ThemeManager::instance().config();
    theme.appFont = QFont(QStringLiteral("Segoe UI"));
    adqt::theme::ThemeManager::instance().setConfig(theme);
    adqt::theme::ThemeManager::instance().applyTo(*qApp);
#endif
    settingsInitialAvailability();
    conversionCacheUsesRecognitionKeys();
    conversionLifecycleAndSettings();
    customModelWorkflows();
    conversionSourceNormalization();
    renderingCopyAndPersistence();
    conversionSessionRoutesSourceAndClearsOldViews();
    conversionUsesRecognitionMessages();
    previewThemesAndLongDocuments();
    conversionToolbarMigration();
}
