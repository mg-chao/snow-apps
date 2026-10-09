#include "snow_shot/presentation/screenshotimageconversioncontroller.h"
#include "snow_shot/presentation/screenshotimageconversionpersistence.h"
#include "snow_shot/presentation/screenshotimageconversionview.h"
#include "snow_shot/presentation/screenshottoolbarlayoutmodel.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshotocrtexteditingsession.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/translation/translationservice.h"
#include "theme/theme_manager.h"
#include "widgets/modal.h"
#include "widgets/select.h"
#include "widgets/button.h"
#include "widgets/context_menu.h"
#include "physical_key_test_support.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QEventLoop>
#include <QPushButton>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextBrowser>
#include <QTextEdit>
#include <QTextCursor>
#include <QTimer>
#include <QScopeGuard>
#include <QDir>
#include <QFontDatabase>
#include <QMimeData>
#include <QScrollBar>
#include <QScreen>
#include <QKeyEvent>
#include <QKeySequence>
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

void until(const std::function<bool()>& predicate,
           const char* timeoutMessage = "asynchronous conversion test timed out") {
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
    require(predicate(), timeoutMessage);
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
#if defined(Q_OS_WIN)
    // Only Windows configures a themed app font for this suite; the preview document
    // must carry the theme's unhinted outline policy either way.
    require(browser->document()->defaultFont().hintingPreference() == QFont::PreferNoHinting,
            "conversion previews must render unhinted outlines");
#endif
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

void recognitionFileSnapshotTracksPartialConversionAndQrValues() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    ConversionServer server;
    server.hold = true;
    SnowShotApiClient api(server.url());
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, actions);
    session.setTarget({QStringLiteral("file-export"), sampleImage(), QRectF(0, 0, 240, 120)});
    ScreenshotRecognitionResults results;
    results.key = QStringLiteral("file-export");
    results.qr =
        ScreenshotQrRecognitionResult{{QStringLiteral("first"), QStringLiteral("second")}, {}, {}};
    session.seedRecognitionResults(results);
    for (const auto mode : {Mode::Markdown, Mode::Html}) {
        session.activate(mode);
        const auto empty = session.fileExportSnapshot();
        require(empty && empty->source.isEmpty(),
                "conversion file snapshot identifies its format before source arrives");
        until([&]() {
            const auto snapshot = session.fileExportSnapshot();
            return snapshot && snapshot->source == QStringLiteral("# Partial");
        });
        const auto snapshot = session.fileExportSnapshot();
        require(snapshot && snapshot->kind == (mode == Mode::Markdown
                                                   ? ScreenshotRecognitionFileKind::Markdown
                                                   : ScreenshotRecognitionFileKind::Html),
                "conversion file snapshot preserves active format and partial source");
    }
    session.activate(Mode::Qr);
    const auto qr = session.fileExportSnapshot();
    require(qr && qr->kind == ScreenshotRecognitionFileKind::Qr &&
                qr->source == QStringLiteral("first\nsecond"),
            "QR file snapshot joins all recognized values in order");
    session.deactivate();
    require(!session.fileExportSnapshot(), "inactive recognition must not route text save");
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
                                                        QStringLiteral("latex-recognition"),
                                                        QStringLiteral("convert-to-html")} &&
                // Upload, quick-save, and print inherit the manual save action's visibility.
                migrated.hidden ==
                    QStringList{QStringLiteral("save-as-file"), QStringLiteral("upload-to-cloud"),
                                QStringLiteral("quick-save"), QStringLiteral("print")},
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
    auto expectedCustomGrouped = customGrouped;
    expectedCustomGrouped.positions[1].insert(3, QStringLiteral("latex-recognition"));
    verify(customGrouped, expectedCustomGrouped);
    verify({}, defaults);
    auto customized = previousDefault;
    customized.positions.move(1, customized.positions.size() - 1);
    auto expectedCustomized = customized;
    expectedCustomized.positions.last().append(QStringLiteral("latex-recognition"));
    verify(customized, expectedCustomized);
    auto qrHidden = original;
    qrHidden.positions[1].removeAll(QStringLiteral("barcode-recognition"));
    qrHidden.hidden.push_back(QStringLiteral("barcode-recognition"));
    auto tableGroup = migrated;
    tableGroup.positions[1].removeAll(QStringLiteral("barcode-recognition"));
    // Recognition keeps its input position before the inherited hidden save companions.
    tableGroup.hidden.insert(1, QStringLiteral("barcode-recognition"));
    verify(qrHidden, tableGroup);
    auto recognitionHidden = defaults;
    recognitionHidden.positions.removeFirst();
    recognitionHidden.hidden = {QStringLiteral("barcode-recognition"),
                                QStringLiteral("table-recognition")};
    auto conversionsOnly = recognitionHidden;
    conversionsOnly.positions.push_back({QStringLiteral("convert-to-html"),
                                         QStringLiteral("convert-to-markdown"),
                                         QStringLiteral("latex-recognition")});
    verify(recognitionHidden, conversionsOnly);
}
} // namespace

namespace {
void initializeRecognitionTestFonts() {
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
}
} // namespace

void runImageConversionTests() {
    initializeRecognitionTestFonts();
    settingsInitialAvailability();
    conversionCacheUsesRecognitionKeys();
    conversionLifecycleAndSettings();
    customModelWorkflows();
    conversionSourceNormalization();
    renderingCopyAndPersistence();
    conversionSessionRoutesSourceAndClearsOldViews();
    recognitionFileSnapshotTracksPartialConversionAndQrValues();
    conversionUsesRecognitionMessages();
    previewThemesAndLongDocuments();
    conversionToolbarMigration();
}

void latexSessionRequestsAreIsolated() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "LaTeX session fixture listens");
    int requests = 0;
    bool hold = true;
    bool fail = false;
    QPointer<QTcpSocket> held;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto* socket = server.nextPendingConnection();
        auto bytes = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, &server, [&, socket, bytes]() {
            *bytes += socket->readAll();
            const auto end = bytes->indexOf("\r\n\r\n");
            if (end < 0 || socket->property("answered").toBool())
                return;
            if (bytes->startsWith("GET /api/v2/chat/models ")) {
                socket->setProperty("answered", true);
                ConversionServer::respond(
                    socket,
                    R"({"data":[{"model":"vision","name":"Vision","supports_vision":true}]})",
                    "application/json");
                return;
            }
            qsizetype length = 0;
            for (const auto& line : bytes->left(end).split('\n'))
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
            if (bytes->size() < end + 4 + length)
                return;
            socket->setProperty("answered", true);
            ++requests;
            if (hold) {
                held = socket;
                return;
            }
            const QByteArray body = fail ? R"({"code":"no_formula","detail":"No formula"})"
                                         : R"({"data":{"latex":"x^2"}})";
            socket->write(QByteArray("HTTP/1.1 ") + (fail ? "422 No Formula" : "200 OK") +
                          "\r\nContent-Type: application/json\r\nContent-Length: " +
                          QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    auto api = std::make_unique<SnowShotApiClient>(
        QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, api.get(), actions);
    auto* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "LaTeX recognition lifecycle fixture has a screen");
    const QRect geometry(screen->availableGeometry().topLeft() + QPoint(320, 120), QSize(360, 220));
    require(window.present({screen, nullptr, geometry, QRectF(0, 0, 240, 120)}),
            "LaTeX recognition lifecycle fixture presents");
    const auto preview = [&]() {
        return window.findChild<ScreenshotOriginalImagePreviewWindow*>();
    };
    const auto requireNoPreview = [&](const char* message) {
        QCoreApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        require(preview() == nullptr, message);
    };
    const auto requirePreviewVisible = [&]() {
        until([&] { return preview() && preview()->isVisible(); },
              "successful LaTeX recognition displays the formula preview");
    };
    const auto target = [&](const QString& key) {
        session.setTarget({key, sampleImage(), QRectF(0, 0, 240, 120)});
    };
    target(QStringLiteral("success"));
    session.activate(Mode::Latex);
    until([&] { return held != nullptr; });
    require(session.busy(Mode::Latex), "LaTeX recognition remains pending until the response");
    requireNoPreview("switching to LaTeX cannot create a preview before successful recognition");
    ConversionServer::respond(held, R"({"data":{"latex":"x^2"}})", "application/json");
    held.clear();
    hold = false;
    until([&] { return !session.busy(Mode::Latex); });
    require(requests == 1 && session.cachedRecognitionResults().latex.has_value(),
            "successful LaTeX request caches result");
    requirePreviewVisible();
    session.deactivate();
    requireNoPreview("deactivation releases the successful formula preview");
    session.activate(Mode::Latex);
    require(requests == 1 && !session.busy(), "cached reactivation does not upload");
    requirePreviewVisible();
    session.activate(Mode::Qr);
    requireNoPreview("switching recognition tools releases the formula preview");
    session.activate(Mode::Latex);
    requirePreviewVisible();
    fail = true;
    target(QStringLiteral("retry"));
    session.activate(Mode::Latex);
    until([&] { return !session.busy(); });
    require(!session.cachedRecognitionResults().latex &&
                !session.workflowState().value(QStringLiteral("error")).toString().isEmpty(),
            "failed recognition reports error without caching");
    requireNoPreview("failed LaTeX recognition cannot create or retain a preview");
    fail = false;
    session.activate(Mode::Latex);
    until([&] { return !session.busy(); });
    require(requests == 3 && session.cachedRecognitionResults().latex.has_value(),
            "reactivation retries a failed request");
    requirePreviewVisible();
    hold = true;
    target(QStringLiteral("old"));
    session.activate(Mode::Latex);
    until([&] { return held != nullptr; });
    requireNoPreview(
        "a new target cannot display the preceding formula while recognition is pending");
    session.deactivate();
    require(!session.active(), "mode exit retains no active result view");
    target(QStringLiteral("new"));
    held->write("HTTP/1.1 200 OK\r\nContent-Length: 24\r\n\r\n{\"data\":{\"latex\":\"old\"}}");
    held->disconnectFromHost();
    QCoreApplication::processEvents();
    require(!session.cachedRecognitionResults().latex && !session.busy(),
            "target changes cancel stale results");
    requireNoPreview("a stale recognition response cannot create a formula preview");
    held.clear();
    session.activate(Mode::Latex);
    until([&] { return held != nullptr; });
    api.reset();
    require(!session.busy() && !session.cachedRecognitionResults().latex,
            "provider destruction releases pending recognition");
    requireNoPreview("provider destruction cannot create a formula preview");
    session.activate(Mode::Latex);
    requireNoPreview("an unavailable recognition provider cannot create a formula preview");
}

void latexPendingResultReplacesBorrowedDocumentSafely() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    QTcpServer server;
    require(server.listen(QHostAddress::LocalHost), "LaTeX replacement fixture listens");
    QPointer<QTcpSocket> held;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
        auto* socket = server.nextPendingConnection();
        auto bytes = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, &server, [&, socket, bytes]() {
            *bytes += socket->readAll();
            const auto end = bytes->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            if (bytes->startsWith("GET /api/v2/chat/models ")) {
                socket->setProperty("answered", true);
                ConversionServer::respond(
                    socket,
                    R"({"data":[{"model":"vision","name":"Vision","supports_vision":true}]})",
                    "application/json");
                return;
            }
            qsizetype length = 0;
            for (const auto& line : bytes->left(end).split('\n'))
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
            if (bytes->size() >= end + 4 + length)
                held = socket;
        });
    });
    SnowShotApiClient api(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, actions);
    const QString key = QStringLiteral("latex-pending-seed");
    session.setTarget({key, sampleImage(), QRectF(0, 0, 240, 120)});
    session.activate(Mode::Latex);
    until([&]() { return held != nullptr; });
    ScreenshotRecognitionResults seed;
    seed.key = key;
    seed.latex = SnowShotLatexResult{QStringLiteral("a\r\n+b"), {}, {}};
    session.seedRecognitionResults(seed);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor && session.busy(),
            "active seeding attaches a source while recognition is pending");
    auto* originalDocument = editor->document();
    QPointer<QTextDocument> originalGuard(originalDocument);
    bool oldDocumentRetired = false;
    QObject::connect(originalDocument, &QObject::destroyed, &window, [&]() {
        const auto* currentEditor =
            window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
        require(currentEditor && currentEditor->document() != originalDocument,
                "a replaced source document must outlive the editor's detach");
        oldDocumentRetired = true;
    });
    QTextCursor cursor(originalDocument);
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("+c"));
    require(session.latexDraft() == QStringLiteral("a\r\n+b+c"),
            "the seeded document remains editable while the request is pending");
    const QByteArray body = R"({"data":{"latex":"x^2"}})";
    held->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    held->disconnectFromHost();
    until([&]() { return !session.busy(); });
    editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor && editor->toPlainText() == QStringLiteral("x^2") &&
                session.latexDraft() == QStringLiteral("x^2") &&
                session.fileExportSnapshot()->source == QStringLiteral("x^2") &&
                oldDocumentRetired && originalGuard.isNull(),
            "late recognition safely rebinds the editor before retiring the seeded source");
}

void latexRepeatedSeedingPreservesBaselineAndHistory() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    ScreenshotRecognitionResults initial;
    initial.key = QStringLiteral("latex-repeated-seed");
    const QString original = QStringLiteral("\\alpha\r\n+\u00a0a");
    initial.latex = SnowShotLatexResult{original, {}, {}};
    session.setTarget({initial.key, sampleImage(), QRectF(0, 0, 240, 120)});
    session.seedRecognitionResults(initial);
    session.activate(Mode::Latex);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor != nullptr, "repeated LaTeX seeding fixture exposes an editor");
    QPointer<QTextDocument> sourceDocument(editor->document());
    const QString edited = original + QStringLiteral("+x");
    session.setTextDraft(edited);
    ScreenshotRecognitionResults repeated;
    repeated.key = initial.key;
    repeated.latex = SnowShotLatexResult{QStringLiteral("\\beta\u2028+b"), {}, {}};
    repeated.latexDraft = QStringLiteral("\\gamma\u2029+c");
    session.seedRecognitionResults(repeated);
    const auto cached = session.cachedRecognitionResults();
    require(sourceDocument && editor->document() == sourceDocument && cached.latex &&
                cached.latex->latex == original && cached.latexDraft == edited &&
                session.latexDraft() == edited,
            "repeated seeding preserves the paired recognized baseline and edited document");
    session.undoTextEdit();
    require(session.latexDraft() == original && !session.cachedRecognitionResults().latexDraft,
            "repeated seeding preserves formula undo history and its original exact source");
    session.redoTextEdit();
    require(session.latexDraft() == edited, "repeated seeding preserves formula redo history");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("reset_text")}}) &&
                session.latexDraft() == cached.latex->latex,
            "reset restores the same recognition baseline exported by the cached snapshot");
}

void latexSourcePreservesWhitespace() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    const QString source = QStringLiteral("\\text{a\u00a0b}\r\n+x\u2028+y\u2029+z\r+1\n");
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    session.setTarget({QStringLiteral("latex-whitespace"), sampleImage(), QRectF(0, 0, 240, 120)});
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("latex-whitespace");
    seed.latex = SnowShotLatexResult{source, {}, {}};
    session.seedRecognitionResults(seed);
    session.activate(Mode::Latex);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor && editor->document()->toPlainText() != source,
            "fixture includes whitespace normalized by QTextDocument");
    const auto sourceIs = [&](const QString& expected) {
        const auto file = session.fileExportSnapshot();
        return session.latexDraft() == expected &&
               session.recognitionClipboardMimeData()->text() == expected && file &&
               file->source == expected &&
               session.workflowResult().value(QStringLiteral("text")).toString() == expected;
    };
    require(sourceIs(source) && !session.cachedRecognitionResults().latexDraft,
            "recognized whitespace remains verbatim through every source export");
    QTextCursor copyCursor(editor->document());
    editor->setTextCursor(copyCursor);
    QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
    require(window.copyVisibleContentToClipboard() && QApplication::clipboard()->text() == source,
            "LaTeX copy without a selection preserves every original source character");
    copyCursor.select(QTextCursor::Document);
    editor->setTextCursor(copyCursor);
    QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
    require(window.copyVisibleContentToClipboard() && QApplication::clipboard()->text() == source,
            "LaTeX Select All copy preserves CRLF, NBSP and Unicode separators");
    const QString raw = editor->document()->toRawText();
    copyCursor.setPosition(static_cast<int>(raw.indexOf(QLatin1Char('a'))));
    copyCursor.setPosition(static_cast<int>(raw.indexOf(QLatin1Char('z')) + 1),
                           QTextCursor::KeepAnchor);
    editor->setTextCursor(copyCursor);
    const qsizetype sourceStart = source.indexOf(QLatin1Char('a'));
    const QString selectedSource =
        source.mid(sourceStart, source.indexOf(QLatin1Char('z')) + 1 - sourceStart);
    QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
    require(window.copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() == selectedSource,
            "partial LaTeX copy maps document positions to exact source spans");
    const int paragraphPosition = static_cast<int>(raw.indexOf(QChar::ParagraphSeparator));
    copyCursor.setPosition(paragraphPosition);
    copyCursor.setPosition(paragraphPosition + 1, QTextCursor::KeepAnchor);
    editor->setTextCursor(copyCursor);
    QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
    require(window.copyVisibleContentToClipboard() &&
                QApplication::clipboard()->text() == QStringLiteral("\r\n"),
            "copying one document paragraph preserves the source CRLF pair");

    QString edited = source;
    QTextCursor cursor(editor->document());
    cursor.setPosition(static_cast<int>(editor->document()->toRawText().indexOf(QChar(0x00a0))));
    cursor.insertText(QStringLiteral("X"));
    edited.insert(edited.indexOf(QChar(0x00a0)), QStringLiteral("X"));
    require(sourceIs(edited), "native insertion preserves adjacent NBSP and all line separators");
    const auto separator =
        static_cast<int>(editor->document()->toRawText().indexOf(QChar::ParagraphSeparator));
    cursor.setPosition(separator);
    cursor.setPosition(separator + 1, QTextCursor::KeepAnchor);
    cursor.removeSelectedText();
    edited.remove(edited.indexOf(QStringLiteral("\r\n")), 2);
    require(sourceIs(edited), "native paragraph deletion consumes exactly its retained CRLF pair");
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("\n"));
    edited += QLatin1Char('\n');
    require(sourceIs(edited), "new paragraphs use LF while untouched source spans stay exact");
    session.undoTextEdit();
    require(sourceIs(edited.chopped(1)), "undo preserves original line separator spelling");
    session.redoTextEdit();
    require(sourceIs(edited), "redo preserves original line separator spelling");

    const QString replacement = QStringLiteral("\\alpha\r\n+\u00a0x\u2028+y\u2029+z\r");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_text")},
                                  {QStringLiteral("text"), replacement}}) &&
                sourceIs(replacement) &&
                session.cachedRecognitionResults().latexDraft == replacement,
            "workflow replacement and persisted draft retain exact source whitespace");
    session.undoTextEdit();
    require(sourceIs(edited), "workflow undo restores the native source without normalization");
    session.redoTextEdit();
    require(sourceIs(replacement),
            "workflow redo restores the supplied source without normalization");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("reset_text")}}) &&
                sourceIs(source) && !session.cachedRecognitionResults().latexDraft,
            "reset restores the exact recognized baseline and clears the persisted draft");

    ScreenshotOcrTextEditingSession detached(source, true);
    QTextCursor detachedCursor(detached.document());
    detachedCursor.setPosition(
        static_cast<int>(detached.document()->toRawText().indexOf(QChar(0x00a0)) + 1));
    detachedCursor.insertText(QStringLiteral("Y"));
    QString detachedExpected = source;
    detachedExpected.insert(detachedExpected.indexOf(QChar(0x00a0)) + 1, QStringLiteral("Y"));
    require(detached.text() == detachedExpected,
            "headless document edits also retain untouched source spans");
    ScreenshotOcrTextEditingSession repeated(QStringLiteral("a\r\n\r\n\nb"), true);
    static_cast<void>(repeated.document()->documentLayout());
    QTextCursor repeatedCursor(repeated.document());
    repeatedCursor.setPosition(2);
    repeatedCursor.deleteChar();
    require(repeated.text() == QStringLiteral("a\r\n\nb"),
            "native deletion identifies the correct span among repeated normalized paragraphs");
    ScreenshotOcrTextEditingSession textRecognition(source);
    require(textRecognition.text() == textRecognition.document()->toPlainText(),
            "OCR editing keeps its existing plain-text normalization");
    QTextEdit survivingEditor;
    bool documentDestroyed = false;
    auto transient = std::make_unique<ScreenshotOcrTextEditingSession>(source, true);
    survivingEditor.setDocument(transient->document());
    QObject::connect(transient->document(), &QObject::destroyed, &survivingEditor,
                     [&]() { documentDestroyed = true; });
    QTextCursor transientCursor(transient->document());
    transientCursor.insertText(QStringLiteral("X"));
    survivingEditor.setDocument(nullptr);
    transient.reset();
    require(documentDestroyed && survivingEditor.toPlainText().isEmpty(),
            "detached session teardown preserves the editor and external destruction observers");
}

void latexKeyboardHistoryRoutesToSession() {
    using Mode = ScreenshotRecognitionSessionController::Mode;
    QPointer<ScreenshotRecognitionSessionController> activeSession;
    int undoCallbacks = 0;
    int redoCallbacks = 0;
    int copyCallbacks = 0;
    ScreenshotRecognitionWindowActions windowActions;
    windowActions.handleUndoTextEdit = [&]() {
        require(activeSession != nullptr, "LaTeX keyboard undo has a live session");
        ++undoCallbacks;
        activeSession->undoTextEdit();
    };
    windowActions.handleRedoTextEdit = [&]() {
        require(activeSession != nullptr, "LaTeX keyboard redo has a live session");
        ++redoCallbacks;
        activeSession->redoTextEdit();
    };
    windowActions.handleCopy = [&]() { ++copyCallbacks; };
    ScreenshotRecognitionWindow window(windowActions);
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    activeSession = &session;
    const QString original = QStringLiteral("\\alpha");
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("latex-keyboard-history");
    seed.latex = SnowShotLatexResult{original, {}, {}};
    session.setTarget({seed.key, sampleImage(), QRectF(0, 0, 240, 120)});
    session.seedRecognitionResults(seed);
    session.activate(Mode::Latex);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor != nullptr, "LaTeX keyboard fixture attaches the source editor");
    auto* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "LaTeX keyboard fixture has a screen");
    const QRect geometry(screen->availableGeometry().topLeft() + QPoint(320, 120), QSize(360, 220));
    require(window.present({screen, nullptr, geometry, QRectF(0, 0, 240, 120)}),
            "LaTeX keyboard fixture presents the selection and source editor");
    // The offscreen platform activates passive tool windows on their first show.
    // Settle the companion before focusing the input whose shortcuts are under test.
    until(
        [&]() {
            const auto* preview = window.findChild<ScreenshotOriginalImagePreviewWindow*>();
            return preview && preview->isVisible() && preview->accessibleDescription().isEmpty();
        },
        "LaTeX keyboard fixture's initial formula preview did not settle");
    window.activateWindow();
    editor->setFocus();
    until([&]() { return editor->hasFocus(); },
          "LaTeX keyboard fixture's presented source editor did not receive focus");
    QTextCursor cursor(editor->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(QStringLiteral("+x"));
    const QString edited = original + QStringLiteral("+x");
    require(session.latexDraft() == edited, "keyboard fixture records a native source edit");
    const auto sendStandardKey = [&](QKeySequence::StandardKey standard) {
        const auto bindings = QKeySequence::keyBindings(standard);
        require(!bindings.isEmpty() && bindings.front().count() == 1,
                "LaTeX source editing has a platform standard shortcut");
        const auto combination = bindings.front()[0];
        PhysicalKeyEvent press(QEvent::KeyPress, combination.key(),
                               combination.keyboardModifiers());
        QApplication::sendEvent(editor, &press);
        PhysicalKeyEvent release(QEvent::KeyRelease, combination.key(),
                                 combination.keyboardModifiers());
        QApplication::sendEvent(editor, &release);
    };
    sendStandardKey(QKeySequence::Undo);
    require(undoCallbacks == 1 && redoCallbacks == 0 && session.latexDraft() == original &&
                editor->toPlainText() == original,
            "focused LaTeX Undo invokes the window callback and session history");
    sendStandardKey(QKeySequence::Redo);
    require(undoCallbacks == 1 && redoCallbacks == 1 && session.latexDraft() == edited &&
                editor->toPlainText() == edited,
            "focused LaTeX Redo invokes the window callback and session history");
    const QString exactSource = QStringLiteral("\\text{a\u00a0b}\r\n+x\u2028+y\u2029+z\r+w\n");
    session.setTextDraft(exactSource);
    const QString raw = editor->document()->toRawText();
    QTextCursor selection(editor->document());
    selection.setPosition(static_cast<int>(raw.indexOf(QLatin1Char('a'))));
    selection.setPosition(static_cast<int>(raw.indexOf(QLatin1Char('z')) + 1),
                          QTextCursor::KeepAnchor);
    editor->setTextCursor(selection);
    const qsizetype sourceStart = exactSource.indexOf(QLatin1Char('a'));
    const QString selectedSource =
        exactSource.mid(sourceStart, exactSource.indexOf(QLatin1Char('z')) + 1 - sourceStart);
    QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
    sendStandardKey(QKeySequence::Copy);
    require(QApplication::clipboard()->text() == selectedSource && copyCallbacks == 0 &&
                session.active() && session.latexDraft() == exactSource,
            "LaTeX keyboard Copy preserves a selected source span without finishing capture");
    bool contextCopyTriggered = false;
    QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
    QTimer::singleShot(0, &window, [&]() {
        auto* menu = window.findChild<adqt::widgets::AdContextMenu*>(
            QStringLiteral("screenshotTextEditorContextMenu"));
        require(menu && !menu->actions().isEmpty() && menu->actions().first()->isEnabled(),
                "selected LaTeX source exposes the local context Copy action");
        menu->actions().first()->trigger();
        contextCopyTriggered = true;
        menu->close();
    });
    const QPoint menuPosition = editor->viewport()->rect().center();
    QContextMenuEvent context(QContextMenuEvent::Mouse, menuPosition,
                              editor->viewport()->mapToGlobal(menuPosition));
    QApplication::sendEvent(editor->viewport(), &context);
    require(contextCopyTriggered && context.isAccepted() &&
                QApplication::clipboard()->text() == selectedSource && copyCallbacks == 0,
            "LaTeX context Copy preserves the selected source without finishing capture");
    window.activateWindow();
    editor->setFocus();
    until([&]() { return editor->hasFocus(); },
          "LaTeX source editor did not regain focus after its context menu");
    sendStandardKey(QKeySequence::SelectAll);
    QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
    sendStandardKey(QKeySequence::Copy);
    require(QApplication::clipboard()->text() == exactSource && copyCallbacks == 0,
            "LaTeX keyboard Select All and Copy preserve the complete exact source");
    selection = editor->textCursor();
    selection.clearSelection();
    editor->setTextCursor(selection);
    QApplication::clipboard()->setText(QStringLiteral("clipboard sentinel"));
    sendStandardKey(QKeySequence::Copy);
    require(QApplication::clipboard()->text() == exactSource && copyCallbacks == 0 &&
                session.active(),
            "LaTeX keyboard Copy without a selection stays local and preserves the whole source");
    session.deactivate();
    window.hide();
}

void runLatexRecognitionTests() {
    const snow_shot::storage::ScreenshotRecognitionModelSettings modelSettings;
    const QString previousModel = modelSettings.latexModel();
    const auto restoreModel = qScopeGuard([&] { modelSettings.setLatexModel(previousModel); });
    // These lifecycle regressions explicitly exercise the retained dedicated API contract.
    modelSettings.setLatexModel(screenshotDedicatedRecognitionModelId());
    initializeRecognitionTestFonts();
    latexSessionRequestsAreIsolated();
    latexPendingResultReplacesBorrowedDocumentSafely();
    latexRepeatedSeedingPreservesBaselineAndHistory();
    latexSourcePreservesWhitespace();
    latexKeyboardHistoryRoutesToSession();
    using Mode = ScreenshotRecognitionSessionController::Mode;
    namespace layout = snow_shot::presentation::toolbar_layout;
    using Kind = snow_shot::storage::ScreenshotToolbarLayoutKind;
    const QString source = QStringLiteral("\\frac{a_b}{c^2} <x> & \\alpha\n+1");
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [&]() { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    session.setTarget({QStringLiteral("latex-image"), sampleImage(), QRectF(0, 0, 240, 120)});
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("latex-image");
    seed.latex = SnowShotLatexResult{source, {}, {}};
    session.seedRecognitionResults(seed);
    session.activate(Mode::Latex);
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor && !editor->isReadOnly() && editor->toPlainText() == source,
            "LaTeX opens an editable source document without interpreting source");
    QPointer<QTextDocument> sourceDocument(editor->document());
    require(session.recognitionClipboardMimeData()->text() == source,
            "LaTeX clipboard source is verbatim");
    const auto file = session.fileExportSnapshot();
    require(file && file->kind == ScreenshotRecognitionFileKind::Latex && file->source == source,
            "LaTeX saves as text");
    session.setShowOriginalImage(true);
    require(session.showOriginalImage() && session.recognitionClipboardMimeData()->text() == source,
            "original image toggle retains source");
    session.activate(Mode::Latex);
    editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor && sourceDocument && editor->document() == sourceDocument && !session.busy() &&
                session.workflowResult().value(QStringLiteral("text")).toString() == source,
            "reactivation reuses the cached source document in the current editor");
    const auto snapshot = session.recognitionResultsSnapshot();
    require(snapshot.latex && snapshot.visibleLatex, "snapshot preserves visible LaTeX result");
    ScreenshotRecognitionSessionController pinned(nullptr, nullptr, nullptr, {});
    pinned.setTarget({seed.key, sampleImage(), QRectF(0, 0, 240, 120)});
    pinned.seedRecognitionResults(snapshot);
    pinned.activate(Mode::Latex);
    require(pinned.recognitionClipboardMimeData()->text() == source,
            "pin transfer needs no API call");
    const QString edited = QStringLiteral("\\frac{x+1}{y} \\text{edited}\n");
    int changes = 0;
    QObject::connect(&session, &ScreenshotRecognitionSessionController::recognitionResultsChanged,
                     &window, [&]() { ++changes; });
    QTextCursor cursor(editor->document());
    cursor.select(QTextCursor::Document);
    cursor.insertText(edited);
    require(changes > 0 && session.latexDraft() == edited &&
                session.workflowResult().value(QStringLiteral("text")) == edited &&
                session.recognitionClipboardMimeData()->text() == edited &&
                session.fileExportSnapshot()->source == edited,
            "direct document edits reach persistence, workflow, clipboard and save source");
    require(session.cachedRecognitionResults().latex->latex == source &&
                session.cachedRecognitionResults().latexDraft == edited,
            "edited LaTeX preserves the successful recognition baseline");
    session.undoTextEdit();
    require(session.latexDraft() == source, "LaTeX undo restores the original source");
    session.redoTextEdit();
    require(session.latexDraft() == edited, "LaTeX redo restores the edited source");
    session.deactivate();
    session.activate(Mode::Latex);
    editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor && sourceDocument && editor->document() == sourceDocument &&
                editor->toPlainText() == edited && !session.busy(),
            "reactivation preserves the document, edit and history without recognition");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_text")},
                                  {QStringLiteral("text"), QString()}}),
            "LaTeX workflow accepts an empty draft");
    const auto emptySnapshot = session.recognitionResultsSnapshot();
    require(emptySnapshot.latex && emptySnapshot.latex->succeeded() && emptySnapshot.latexDraft &&
                emptySnapshot.latexDraft->isEmpty() &&
                session.workflowState().value(QStringLiteral("editing")).toBool(),
            "an empty edit remains a recognized editable result");
    ScreenshotRecognitionSessionController emptyTransfer(nullptr, nullptr, nullptr, {});
    emptyTransfer.setTarget({seed.key, sampleImage(), QRectF(0, 0, 240, 120)});
    emptyTransfer.seedRecognitionResults(emptySnapshot);
    emptyTransfer.activate(Mode::Latex);
    require(emptyTransfer.recognitionClipboardMimeData()->text().isEmpty() &&
                emptyTransfer.fileExportSnapshot()->source.isEmpty() &&
                emptyTransfer.editWorkflow(
                    {{QStringLiteral("action"), QStringLiteral("reset_text")}}) &&
                emptyTransfer.latexDraft() == source,
            "headless transfer preserves empty source and reset baseline");
    require(!session.editWorkflow({{QStringLiteral("action"), QStringLiteral("format")},
                                   {QStringLiteral("value"), QStringLiteral("smart")}}),
            "LaTeX source does not accept text recognition transforms");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("reset_text")}}) &&
                session.latexDraft() == source,
            "reset restores recognized LaTeX exactly");
    session.setTarget({QStringLiteral("other"), sampleImage(), QRectF(0, 0, 240, 120)});
    require(!session.cachedRecognitionResults().latex, "another target cannot reuse this formula");
    for (const auto kind : {Kind::ActionTools, Kind::PinnedActionTools}) {
        snow_shot::storage::ScreenshotToolbarLayout old;
        old.positions = {{QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
                          QStringLiteral("barcode-recognition")}};
        const auto migrated = layout::normalizedLayout(old, kind);
        require(migrated.positions.front().at(2) == QStringLiteral("latex-recognition"),
                "migration inserts LaTeX immediately after Markdown");
        snow_shot::storage::ScreenshotToolbarLayout previousDefault;
        previousDefault.positions = {
            {QStringLiteral("convert-to-html"), QStringLiteral("latex-recognition"),
             QStringLiteral("convert-to-markdown"), QStringLiteral("barcode-recognition"),
             QStringLiteral("table-recognition")}};
        const auto updated = layout::normalizedLayout(previousDefault, kind);
        require(updated.positions.front().at(1) == QStringLiteral("convert-to-markdown") &&
                    updated.positions.front().at(2) == QStringLiteral("latex-recognition"),
                "previous default moves LaTeX to Markdown's left in the reversed popup");
        old.hidden = {QStringLiteral("latex-recognition")};
        const auto hidden = layout::normalizedLayout(old, kind);
        require(hidden.hidden.contains(QStringLiteral("latex-recognition")) &&
                    !hidden.positions.front().contains(QStringLiteral("latex-recognition")),
                "explicitly hidden LaTeX stays hidden");
    }
}
