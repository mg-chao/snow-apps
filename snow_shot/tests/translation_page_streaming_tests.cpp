#include "translation_test_support.h"

#include "snow_shot/presentation/components/translationpagewidget.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/translationpagecontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "widgets/input_text_edit.h"
#include "widgets/scroll_area.h"

#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>

using namespace translation_tests;
using snow_shot::presentation::TranslationPageController;

namespace {
void continuousStreamMakesProgress() {
    Server server;
    SnowShotApiClient client(server.url());
    TranslationPageWidget page(nullptr, &client, 0);
    auto* controller = page.findChild<TranslationPageController*>();
    auto* result =
        page.findChild<adqt::widgets::AdTextEdit*>(QStringLiteral("translationResultText"));
    page.setSourceText(QStringLiteral("continuous translation"));
    waitUntil([&] { return server.streams.size() == 1; }, "start continuous stream");
    const QString original = QStringLiteral("A\u00a0B\r\n") + QString::fromUcs4(U"\U0001f30d");
    const QString normalized = QStringLiteral("A B\n") + QString::fromUcs4(U"\U0001f30d");
    server.delta(0, original);
    waitUntil([&] { return result->toPlainText() == normalized; },
              "render normalized Unicode text");
    result->selectAll();
    const QString selected = result->textCursor().selectedText();
    QTimer producer;
    producer.setInterval(1);
    QObject::connect(&producer, &QTimer::timeout, &page,
                     [&] { server.delta(0, QStringLiteral("x")); });
    producer.start();
    waitUntil([&] { return result->toPlainText().size() > normalized.size(); },
              "a continuous stream renders without waiting for a quiet period");
    producer.stop();
    require(controller->translating() && result->textCursor().selectedText() == selected,
            "streaming past normalized characters preserves the existing document selection");
    server.finish(0);
    waitUntil([&] { return !controller->translating(); }, "finish continuous stream");
    require(result->toPlainText() == normalized + controller->resultText().mid(original.size()),
            "rendered offsets retain every Unicode and streamed character exactly once");
}

void streamingUpdatesAreBatched() {
    Server server;
    SnowShotApiClient client(server.url());
    TranslationPageWidget page(nullptr, &client, 0);
    page.resize(900, 650);
    page.show();
    auto* controller = page.findChild<TranslationPageController*>();
    auto* result =
        page.findChild<adqt::widgets::AdTextEdit*>(QStringLiteral("translationResultText"));
    page.setSourceText(QString(5000, u'x'));
    waitUntil([&] { return server.streams.size() == 1; }, "start long translation");
    server.delta(0, QStringLiteral("Translated paragraph.\n").repeated(200));
    waitUntil(
        [&] {
            return result->toPlainText() == controller->resultText() &&
                   !controller->resultText().isEmpty();
        },
        "display long initial output");
    int changes = 0;
    int stateChanges = 0;
    QObject::connect(result->document(), &QTextDocument::contentsChanged, &page,
                     [&] { ++changes; });
    QObject::connect(controller, &TranslationPageController::stateChanged, &page,
                     [&] { ++stateChanges; });
    const QString previous = controller->resultText();
    QByteArray burst;
    for (int index = 0; index < 128; ++index) {
        burst += "data: {\"choices\":[{\"delta\":{\"content\":\"x\"}}]}\n\n";
    }
    server.send(0, burst);
    waitUntil([&] { return controller->resultText().size() == previous.size() + 128; },
              "receive every token in the burst");
    waitUntil([&] { return result->toPlainText() == controller->resultText(); },
              "render the complete burst without losing tokens");
    std::cout << "Document changes for 128 tokens: " << changes << '\n';
    require(changes < 128, "network token bursts must coalesce document/layout updates");
    require(stateChanges == 0, "tokens do not resynchronize unrelated page controls");
    require(!result->document()->isUndoAvailable(),
            "read-only streamed output keeps no undo history");

    QTextCursor selection(result->document());
    selection.setPosition(20);
    selection.setPosition(5, QTextCursor::KeepAnchor);
    result->setTextCursor(selection);
    auto* scroll = page.findChild<adqt::widgets::AdScrollArea*>()->verticalScrollBar();
    scroll->setValue(scroll->maximum() / 2);
    const int scrollPosition = scroll->value();
    changes = 0;
    server.delta(0, QStringLiteral("\nOne more paragraph."));
    waitUntil(
        [&] {
            return result->toPlainText() == controller->resultText() &&
                   controller->resultText().endsWith(QStringLiteral("One more paragraph."));
        },
        "append a new paragraph");
    require(changes == 1, "an append and its footer formatting form one document update");
    require(result->textCursor().anchor() == 20 && result->textCursor().position() == 5,
            "batched appends preserve selection endpoints and direction");
    require(scroll->value() == scrollPosition, "streaming preserves the page scroll position");
    require(result->document()->lastBlock().blockFormat().bottomMargin() == 36 &&
                result->document()->lastBlock().previous().blockFormat().bottomMargin() == 0,
            "only the last paragraph reserves space for result actions");
    server.finish(0);
    waitUntil([&] { return !controller->translating(); }, "complete translation");
    require(result->toPlainText() == controller->resultText(),
            "completion synchronously flushes the final output");
}

void pendingOutputLifecycle() {
    Server server;
    SnowShotApiClient client(server.url());
    TranslationPageWidget page(nullptr, &client, 0);
    auto* controller = page.findChild<TranslationPageController*>();
    auto* result =
        page.findChild<adqt::widgets::AdTextEdit*>(QStringLiteral("translationResultText"));
    page.setSourceText(QStringLiteral("first request"));
    waitUntil([&] { return server.streams.size() == 1; }, "start lifecycle stream");

    // Observe the stream synchronously, before the next scheduled paint can run.
    bool checkedPending = false;
    auto connection =
        QObject::connect(controller, &TranslationPageController::resultChanged, &page, [&] {
            require(result->toPlainText().isEmpty(), "tokens are buffered before rendering");
            require(page.findChild<QAction*>(QStringLiteral("translationCopy"))->isEnabled(),
                    "the first buffered token immediately enables copy");
            page.findChild<QAction*>(QStringLiteral("translationCopy"))->trigger();
            require(QApplication::clipboard()->text() == controller->resultText(),
                    "copy includes received tokens before the next visual update");
            checkedPending = true;
        });
    server.delta(0, QStringLiteral("final buffered text"));
    server.finish(0);
    waitUntil([&] { return !controller->translating(); }, "finish buffered stream");
    require(checkedPending && result->toPlainText() == QStringLiteral("final buffered text"),
            "completion flushes buffered output immediately");
    QObject::disconnect(connection);

    page.setSourceText(QStringLiteral("second request"));
    require(result->toPlainText().isEmpty(),
            "source replacement immediately clears the old result");
    waitUntil([&] { return server.streams.size() == 2; }, "start replacement stream");
    server.delta(1, QStringLiteral("partial before error"));
    server.fail(1);
    waitUntil([&] { return !controller->translating(); }, "fail buffered stream");
    require(result->toPlainText() == QStringLiteral("partial before error") &&
                !controller->errorText().isEmpty(),
            "errors flush and retain the partial output");

    page.setSourceText(QStringLiteral("cancel pending request"));
    waitUntil([&] { return server.streams.size() == 3; }, "start cancellable stream");
    connection = QObject::connect(
        controller, &TranslationPageController::resultChanged, &page,
        [&] { page.setSourceText(QString()); }, Qt::QueuedConnection);
    server.delta(2, QStringLiteral("obsolete buffered text"));
    waitUntil([&] { return server.disconnected(2); }, "cancel a stream with pending output");
    QObject::disconnect(connection);
    QEventLoop settle;
    QTimer::singleShot(100, &settle, &QEventLoop::quit);
    settle.exec();
    require(result->toPlainText().isEmpty() && controller->resultText().isEmpty(),
            "a scheduled render cannot restore cancelled output");

    page.setSourceText(QStringLiteral("deactivate pending request"));
    waitUntil([&] { return server.streams.size() == 4; }, "start stream before deactivation");
    connection = QObject::connect(
        controller, &TranslationPageController::resultChanged, &page, [&] { page.deactivate(); },
        Qt::QueuedConnection);
    server.delta(3, QStringLiteral("late buffered text"));
    waitUntil([&] { return server.disconnected(3); }, "deactivation cancels the stream");
    QObject::disconnect(connection);
    QTimer::singleShot(100, &settle, &QEventLoop::quit);
    settle.exec();
    require(result->toPlainText().isEmpty(), "deactivation cancels scheduled rendering");
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QTemporaryDir directory;
    require(directory.isValid(), "create isolated streaming test storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage.initialize({directory.path(), directory.path(), 60000}).success,
            "initialize streaming test storage");
    snow_shot::presentation::styles::ThemeManager::instance().initialize(app);
    continuousStreamMakesProgress();
    streamingUpdatesAreBatched();
    pendingOutputLifecycle();
    storage.shutdown();
    return 0;
}
