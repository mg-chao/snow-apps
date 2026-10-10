#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshottabledocument.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/storage/settingsadapters.h"
#include "widgets/button.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QItemSelectionModel>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QRunnable>
#include <QScrollBar>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

namespace {
using Mode = ScreenshotRecognitionSessionController::Mode;
using Settings = snow_shot::storage::ScreenshotRecognitionModelSettings;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void until(const std::function<bool()>& predicate, const char* message) {
    if (!predicate()) {
        QEventLoop loop;
        QTimer poll;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (predicate())
                loop.quit();
        });
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        poll.start(5);
        deadline.start(5000);
        loop.exec();
    }
    require(predicate(), message);
}

struct ModelSettingsGuard {
    QString table = Settings().tableModel();
    QString latex = Settings().latexModel();
    ~ModelSettingsGuard() {
        require(Settings().setTableModel(table) && Settings().setLatexModel(latex),
                "restore recognition model preferences");
    }
};

struct RecognitionLanguageGuard {
    QString preference = snow_shot::presentation::LanguageManager::instance().languagePreference();
    ~RecognitionLanguageGuard() {
        require(snow_shot::presentation::LanguageManager::instance().setLanguage(preference),
                "restore recognition test language");
    }
};

ScreenshotRecognitionTarget target(const QString& key) {
    QImage image(80, 60, QImage::Format_RGBA8888);
    image.fill(Qt::white);
    return {key, image, QRectF(0, 0, 80, 60)};
}

ScreenshotRecognitionSessionActions headlessActions() {
    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = []() -> ScreenshotRecognitionWindow* { return nullptr; };
    return actions;
}

class RecognitionServer final : public QTcpServer {
  public:
    RecognitionServer() {
        require(listen(QHostAddress::LocalHost), "recognition model fixture listens");
        connect(this, &QTcpServer::newConnection, this, [this] {
            auto* socket = nextPendingConnection();
            ++connections;
            auto bytes = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket, bytes] {
                *bytes += socket->readAll();
                const qsizetype end = bytes->indexOf("\r\n\r\n");
                if (end < 0 || socket->property("answered").toBool())
                    return;
                qsizetype length = 0;
                for (const auto& line : bytes->left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:"))
                        length = line.mid(line.indexOf(':') + 1).trimmed().toLongLong();
                if (bytes->size() < end + 4 + length)
                    return;
                socket->setProperty("answered", true);
                if (bytes->startsWith("GET ")) {
                    ++modelRequests;
                    if (holdCatalog) {
                        heldCatalog = socket;
                    } else {
                        respondCatalog(socket);
                    }
                    return;
                }
                if (bytes->startsWith("POST /api/v1/table/extract ")) {
                    ++dedicatedRequests;
                    respond(socket,
                            R"({"data":{"html":"<table><tr><td>Dedicated</td></tr></table>"}})",
                            "application/json");
                    return;
                }
                if (bytes->startsWith("POST /api/v1/latex/extract ")) {
                    ++dedicatedRequests;
                    respond(socket, R"({"data":{"latex":"x^2"}})", "application/json");
                    return;
                }
                const auto request = QJsonDocument::fromJson(bytes->mid(end + 4)).object();
                visionRequests.append(request);
                if (request.value(QStringLiteral("model")).toString() == holdVisionModel) {
                    heldVision = socket;
                    return;
                }
                if (failNextVision) {
                    failNextVision = false;
                    respond(socket, R"({"code":"fixture_error","detail":"Try again"})",
                            "application/json", "500 Failed");
                    return;
                }
                respond(socket, frame(visionSource) + "data: [DONE]\n\n", "text/event-stream");
            });
        });
    }

    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

    static QByteArray frame(const QString& text) {
        return "data: " +
               QJsonDocument(QJsonObject{{QStringLiteral("choices"),
                                          QJsonArray{QJsonObject{
                                              {QStringLiteral("delta"),
                                               QJsonObject{{QStringLiteral("content"), text}}}}}}})
                   .toJson(QJsonDocument::Compact) +
               "\n\n";
    }

    static void respond(QTcpSocket* socket, const QByteArray& body, const QByteArray& type,
                        const QByteArray& status = "200 OK") {
        require(socket != nullptr, "respond to a live recognition fixture connection");
        socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    void respondCatalog(QTcpSocket* socket) {
        QJsonArray models;
        for (const auto& id : catalogOrder)
            models.append(QJsonObject{{QStringLiteral("model"), id},
                                      {QStringLiteral("name"), id},
                                      {QStringLiteral("supports_vision"), includeBuiltinVision}});
        respond(socket,
                QJsonDocument(QJsonObject{{QStringLiteral("data"), models}})
                    .toJson(QJsonDocument::Compact),
                "application/json");
    }

    int connections = 0;
    int modelRequests = 0;
    int dedicatedRequests = 0;
    QVector<QJsonObject> visionRequests;
    bool holdCatalog = false;
    bool failNextVision = false;
    bool includeBuiltinVision = true;
    QStringList catalogOrder{QStringLiteral("vision-a"), QStringLiteral("vision-b")};
    QString holdVisionModel;
    QString visionSource = QStringLiteral("<table><tr><td>B result</td></tr></table>");
    QPointer<QTcpSocket> heldCatalog;
    QPointer<QTcpSocket> heldVision;
};

void loadCatalog(SnowShotApiClient& api) {
    bool completed = false;
    const QString locale =
        snow_shot::presentation::LanguageManager::instance().currentLocale().name();
    require(api.ensureChatModels(locale, &api,
                                 [&](SnowShotChatModelsResult result) {
                                     require(result.succeeded(),
                                             "recognition fixture returns a valid model catalog");
                                     completed = true;
                                 }) != 0,
            "recognition model catalog request starts");
    until([&] { return completed; }, "recognition model catalog request completes");
}

void refreshCatalog(SnowShotApiClient& api) {
    bool completed = false;
    const QString locale =
        snow_shot::presentation::LanguageManager::instance().currentLocale().name();
    require(api.fetchChatModels(locale, &api,
                                [&](SnowShotChatModelsResult result) {
                                    require(result.succeeded(), "refresh the recognition catalog");
                                    completed = true;
                                }) != 0,
            "recognition catalog refresh starts");
    until([&] { return completed; }, "recognition catalog refresh completes");
}

ScreenshotTableRecognitionEntry tableEntry(const QString& model, const QString& html,
                                           SnowShotApiClient* api = nullptr) {
    ScreenshotTableRecognitionEntry entry;
    entry.model = model;
    entry.modelFingerprint = api ? api->modelFingerprint(model) : QString{};
    entry.result.html = html;
    return entry;
}

ScreenshotLatexRecognitionEntry latexEntry(const QString& model, const QString& source,
                                           SnowShotApiClient* api = nullptr) {
    ScreenshotLatexRecognitionEntry entry;
    entry.model = model;
    entry.modelFingerprint = api ? api->modelFingerprint(model) : QString{};
    entry.result.latex = source;
    return entry;
}

QJsonObject cellAt(const QJsonObject& result, int row, int column) {
    for (const auto& value : result.value(QStringLiteral("cells")).toArray()) {
        const auto cell = value.toObject();
        if (cell.value(QStringLiteral("row")).toInt() == row &&
            cell.value(QStringLiteral("column")).toInt() == column)
            return cell;
    }
    return {};
}

void defaultsAndInactiveToolsDoNotFetchModels() {
    ModelSettingsGuard guard;
    require(Settings().tableModel() == screenshotDedicatedRecognitionModelId() &&
                Settings().latexModel() == screenshotDedicatedRecognitionModelId(),
            "fresh Table and LaTeX preferences both default to dedicated recognition models");
    RecognitionServer server;
    SnowShotApiClient api(server.url());
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, headlessActions());
    require(session.recognitionModelSelection(Mode::Table) ==
                    screenshotDedicatedRecognitionModelId() &&
                session.recognitionModelSelection(Mode::Latex) ==
                    screenshotDedicatedRecognitionModelId(),
            "sessions read both independent model defaults");
    session.setTarget(target(QStringLiteral("inactive")));
    session.setProviders(nullptr, nullptr, nullptr);
    session.setProviders(nullptr, nullptr, &api);
    require(Settings().setTableModel(QStringLiteral("vision-b")),
            "save an inactive Table preference");
    require(session.recognitionModelSelection(Mode::Table) == QStringLiteral("vision-b"),
            "inactive sessions observe a saved model preference");
    session.setRecognitionModel(QStringLiteral("vision-a"));
    require(session.recognitionModelSelection(Mode::Table) == QStringLiteral("vision-b"),
            "model changes require an active recognition tool");
    ScreenshotRecognitionResults qr;
    qr.key = QStringLiteral("inactive");
    qr.qr = ScreenshotQrRecognitionResult{{QStringLiteral("payload")}, {}, {}};
    session.seedRecognitionResults(qr);
    session.activate(Mode::Qr);
    session.synchronizeUiState();
    QCoreApplication::processEvents();
    require(server.connections == 0 && server.modelRequests == 0 &&
                api.findChild<QNetworkAccessManager*>() == nullptr &&
                session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("payload"),
            "construction, providers, targets, preferences, and QR activation must fetch no model "
            "catalog");
}

void defaultLatexUsesDedicatedRecognitionWhileCatalogIsLoading() {
    ModelSettingsGuard guard;
    RecognitionServer server;
    server.holdCatalog = true;
    SnowShotApiClient api(server.url());
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, headlessActions());
    session.setTarget(target(QStringLiteral("default-dedicated-latex")));
    session.activate(Mode::Latex);
    until([&] { return !session.busy() && !session.workflowResult().isEmpty(); },
          "default LaTeX recognition completes without waiting for the vision catalog");
    const auto snapshot = session.recognitionResultsSnapshot();
    require(server.dedicatedRequests == 1 && server.visionRequests.isEmpty() &&
                session.latexDraft() == QStringLiteral("x^2") &&
                snapshot.latexModelSelection == screenshotDedicatedRecognitionModelId() &&
                snapshot.latexEntries.size() == 1 &&
                snapshot.latexEntries[0].model == screenshotDedicatedRecognitionModelId(),
            "default LaTeX uses and caches the dedicated formula recognition result");
}

void visionTablesRequireActualMarkupAndRetainMergedEmptyCells() {
    for (const auto& malformed :
         {"<table><tr><td>Incomplete</tr></table>",
          "<table><tr><td rowspan=\"99999999\">Oversized</td></tr></table>",
          "<table><tr><td><img src=\"file:///private.png\"></td></tr></table>",
          "<table><tr><td colspan=\"256\">A</td><td colspan=\"256\">B</td></tr></table>",
          "<table><tr><td colspan=\"2\" colspan=\"2\">Duplicate span</td></tr></table>",
          "<table>Commentary<tr><td>A</td></tr></table>",
          "<table><tr><td><table><tr><td>Nested</td></tr></table></td></tr></table>"}) {
        require(ScreenshotTableDocument::fromHtml(QString::fromUtf8(malformed), true).empty(),
                "vision table parsing rejects incomplete, resource-bearing, nested, or oversized "
                "markup");
    }
    QString expanded = QStringLiteral("<table>");
    for (int row = 0; row < 256; ++row)
        expanded += QStringLiteral("<tr><td colspan=\"256\">Expanded</td></tr>");
    expanded += QStringLiteral("</table>");
    require(ScreenshotTableDocument::fromHtml(expanded, true).empty(),
            "vision tables reject excessive expanded grid slots before rich-text allocation");
    const auto rowHeaders = ScreenshotTableDocument::fromHtml(
        QStringLiteral("<table><tr><th>Heading</th><td>Value</td></tr>"
                       "<tr><th>Row label</th><td>0012</td></tr></table>"),
        true);
    require(!rowHeaders.empty() && rowHeaders.cellAt(0, 0)->header &&
                !rowHeaders.cellAt(0, 1)->header && rowHeaders.cellAt(1, 0)->header &&
                rowHeaders.cellText(1, 1) == QStringLiteral("0012"),
            "vision tables preserve individual column and row headers and numeric text");
    require(
        ScreenshotTableDocument::fromHtml(QStringLiteral("The image contains no table."), true)
                .empty() &&
            ScreenshotTableDocument::fromHtml(QStringLiteral("<p>A\tB<br>C\tD</p>"), true).empty(),
        "vision table recognition must reject prose and HTML paragraphs instead of creating a "
        "table");
    const auto document = ScreenshotTableDocument::fromHtml(
        QStringLiteral("<table><tr><th colspan=\"2\">Header</th></tr>"
                       "<tr><td></td><td>&amp;</td></tr></table>"),
        true);
    require(!document.empty() && document.rowCount() == 2 && document.columnCount() == 2 &&
                document.cellAt(0, 0)->columnSpan == 2 && document.cellAt(0, 0)->header &&
                document.cellText(1, 0).isEmpty() && document.cellText(1, 1) == QStringLiteral("&"),
            "strict vision table parsing preserves actual tables with spans, headers, empty cells, "
            "and entities");
}

void visionTableHeadersFollowGridCoordinates() {
    const auto ragged = ScreenshotTableDocument::fromHtml(
        QStringLiteral("<table><tr><td>A</td></tr><tr><th>B</th><td>C</td></tr></table>"), true);
    require(!ragged.empty() && ragged.rowCount() == 2 && ragged.columnCount() == 2 &&
                !ragged.cellAt(0, 0)->header && !ragged.cellAt(0, 1)->header &&
                ragged.cellAt(1, 0)->header && !ragged.cellAt(1, 1)->header &&
                ragged.cellText(1, 0) == QStringLiteral("B"),
            "synthesized empty cells must not consume the next source cell's header flag");
    const auto spanned = ScreenshotTableDocument::fromHtml(
        QStringLiteral("<table><tr><td rowspan=\"2\">A</td><td>B</td></tr>"
                       "<tr><th>C</th><td>D</td></tr></table>"),
        true);
    require(!spanned.empty() && spanned.rowCount() == 2 && spanned.columnCount() == 3 &&
                spanned.cellAt(0, 0)->rowSpan == 2 && !spanned.cellAt(0, 2)->header &&
                spanned.cellAt(1, 1)->header && !spanned.cellAt(1, 2)->header &&
                spanned.cellText(1, 1) == QStringLiteral("C"),
            "header coordinates account for row spans and missing cells together");
    require(ScreenshotTableDocument::fromHtml(ragged.toHtml(), true) == ragged &&
                ScreenshotTableDocument::fromHtml(spanned.toHtml(), true) == spanned,
            "ragged table headers and spans survive draft serialization and restoration");
}

void tableModelSwitchesPreserveSpansEditsAndUndo() {
    ModelSettingsGuard guard;
    RecognitionServer server;
    SnowShotApiClient api(server.url());
    loadCatalog(api);
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, headlessActions());
    ScreenshotRecognitionSessionController observer(nullptr, nullptr, &api, headlessActions());
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("table-model-history");
    seed.tableModelSelection = QStringLiteral("vision-a");
    seed.tableEntries = {
        tableEntry(QStringLiteral("vision-a"),
                   QStringLiteral("<table><tr><th rowspan=\"2\">A</th><th>B</th></tr>"
                                  "<tr><td>C</td></tr></table>"),
                   &api),
        tableEntry(QStringLiteral("vision-b"),
                   QStringLiteral("<table><tr><td>D</td><td>E</td></tr>"
                                  "<tr><td>F</td><td>G</td></tr></table>"),
                   &api),
    };
    session.setTarget(target(seed.key));
    session.seedRecognitionResults(seed);
    session.activate(Mode::Table);
    require(cellAt(session.workflowResult(), 0, 0).value(QStringLiteral("row_span")).toInt() == 2 &&
                cellAt(session.workflowResult(), 0, 0).value(QStringLiteral("header")).toBool(),
            "model A exposes recognized row spans and header cells");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_cell")},
                                  {QStringLiteral("row"), 0},
                                  {QStringLiteral("column"), 0},
                                  {QStringLiteral("text"), QStringLiteral("A edited")}}),
            "edit model A table");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("select_cells")},
                                  {QStringLiteral("range"), QJsonArray{0, 1, 1, 1}}}) &&
                session.editWorkflow({{QStringLiteral("action"), QStringLiteral("merge_cells")}}),
            "merge model A cells using a preserved table selection");
    const auto editedA = session.workflowResult();
    session.setRecognitionModel(QStringLiteral("vision-b"));
    require(session.workflowResult().value(QStringLiteral("cells")).toArray().size() == 4 &&
                Settings().tableModel() == QStringLiteral("vision-b") &&
                observer.recognitionModelSelection(Mode::Table) == QStringLiteral("vision-b") &&
                observer.recognitionModelSelection(Mode::Latex) ==
                    screenshotDedicatedRecognitionModelId(),
            "selecting model B publishes its table and persists Table preferences independently");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_cell")},
                                  {QStringLiteral("row"), 0},
                                  {QStringLiteral("column"), 0},
                                  {QStringLiteral("text"), QStringLiteral("B edited")}}),
            "edit model B table separately");
    session.setRecognitionModel(QStringLiteral("vision-a"));
    require(session.workflowResult() == editedA,
            "A to B to A restores the exact edited table and merged spans");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("undo")}}) &&
                session.workflowResult().value(QStringLiteral("cells")).toArray().size() == 3 &&
                cellAt(session.workflowResult(), 0, 0).value(QStringLiteral("text")) ==
                    QStringLiteral("A edited"),
            "model A retains its own undo stack across model switches");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("redo")}}) &&
                session.workflowResult() == editedA,
            "model A retains redo state and merged-cell metadata");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("split_cells")}}),
            "model A retains its selected cells across model switches");
    session.setRecognitionModel(QStringLiteral("vision-b"));
    require(cellAt(session.workflowResult(), 0, 0).value(QStringLiteral("text")) ==
                QStringLiteral("B edited"),
            "model B retains its independent draft");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("undo")}}) &&
                cellAt(session.workflowResult(), 0, 0).value(QStringLiteral("text")) ==
                    QStringLiteral("D"),
            "model B undo never applies model A history");
    const auto snapshot = session.recognitionResultsSnapshot();
    require(snapshot.tableEntries.size() == 2 &&
                snapshot.tableModelSelection == QStringLiteral("vision-b") &&
                std::any_of(snapshot.tableEntries.cbegin(), snapshot.tableEntries.cend(),
                            [](const auto& entry) {
                                return entry.model == QStringLiteral("vision-a") &&
                                       entry.draftHtml.has_value();
                            }) &&
                server.modelRequests == 1 && server.visionRequests.isEmpty() &&
                server.dedicatedRequests == 0,
            "model-aware snapshots retain separate drafts and cached switches do not upload or "
            "refetch");
}

void tableModelSwitchesReuseEditorAndRestoreIndependentViewState() {
    ModelSettingsGuard guard;
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    auto actions = headlessActions();
    actions.ensureContent = [&] { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("table-editor-model-switch");
    seed.tableModelSelection = QStringLiteral("vision-a");
    const auto scrollableTable = [](const QString& prefix) {
        QString html = QStringLiteral("<table>");
        for (int row = 0; row < 16; ++row) {
            html += QStringLiteral("<tr>");
            for (int column = 0; column < 6; ++column)
                html += QStringLiteral("<td>%1</td>")
                            .arg(row == 0 && column == 0
                                     ? prefix
                                     : QStringLiteral("%1%2_%3").arg(prefix).arg(row).arg(column));
            html += QStringLiteral("</tr>");
        }
        return html + QStringLiteral("</table>");
    };
    seed.tableEntries = {
        tableEntry(QStringLiteral("vision-a"), scrollableTable(QStringLiteral("A"))),
        tableEntry(QStringLiteral("vision-b"), scrollableTable(QStringLiteral("B"))),
    };
    session.setTarget(target(seed.key));
    session.seedRecognitionResults(seed);
    session.activate(Mode::Table);
    window.resize(440, 180);
    window.show();
    QCoreApplication::processEvents();
    QPointer<ScreenshotTableEditor> editor = window.findChild<ScreenshotTableEditor*>();
    require(editor && editor->model() &&
                session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_cell")},
                                      {QStringLiteral("row"), 1},
                                      {QStringLiteral("column"), 2},
                                      {QStringLiteral("text"), QStringLiteral("A edited")}}),
            "a real recognition window binds the selected editable Table session");
    const auto select = [&](const ScreenshotTableRange& range, int row, int column) {
        editor->selectionModel()->select(
            QItemSelection(editor->model()->index(range.top, range.left),
                           editor->model()->index(range.bottom, range.right)),
            QItemSelectionModel::ClearAndSelect);
        editor->selectionModel()->setCurrentIndex(editor->model()->index(row, column),
                                                  QItemSelectionModel::NoUpdate);
        require(editor->selectedRange() == range && editor->currentIndex().row() == row &&
                    editor->currentIndex().column() == column,
                "the Table editor fixture sets a distinct selected range and current cell");
    };
    const ScreenshotTableRange selectionA{13, 4, 15, 5};
    const ScreenshotTableRange selectionB{0, 0, 0, 1};
    select(selectionA, 14, 5);
    require(editor->horizontalScrollBar()->maximum() > 20 &&
                editor->verticalScrollBar()->maximum() > 64,
            "the Table editor fixture has scrollable content on both axes");
    editor->horizontalScrollBar()->setValue(20);
    editor->verticalScrollBar()->setValue(64);
    const int horizontalA = editor->horizontalScrollBar()->value();
    const int verticalA = editor->verticalScrollBar()->value();
    session.setRecognitionModel(QStringLiteral("vision-b"));
    require(editor && window.findChild<ScreenshotTableEditor*>() == editor &&
                editor->model()->index(0, 0).data() == QStringLiteral("B"),
            "cached model switches rebind the same Table editor widget to the selected model");
    select(selectionB, 0, 0);
    session.setRecognitionModel(QStringLiteral("vision-a"));
    require(editor && window.findChild<ScreenshotTableEditor*>() == editor,
            "returning to model A preserves the same editor widget");
    require(editor->currentIndex().row() == 14 && editor->currentIndex().column() == 5,
            "returning to model A preserves its current cell");
    require(editor->model()->index(1, 2).data() == QStringLiteral("A edited"),
            "returning to model A preserves its edited cell value");
    require(editor->selectedRange() == selectionA,
            "returning to model A preserves its selected cell range");
    const auto requireScrollA = [&] {
        require(editor->horizontalScrollBar()->value() == horizontalA &&
                    editor->verticalScrollBar()->value() == verticalA,
                "returning to model A restores its saved viewport without scrolling to the current "
                "cell");
    };
    requireScrollA();
    QCoreApplication::processEvents();
    requireScrollA();
    session.setRecognitionModel(QStringLiteral("vision-b"));
    require(editor && editor->selectedRange() == selectionB && editor->currentIndex().row() == 0 &&
                editor->currentIndex().column() == 0 &&
                editor->model()->index(0, 0).data() == QStringLiteral("B"),
            "model B restores its own view state without inheriting model A edits");
    session.setTarget(target(QStringLiteral("table-editor-new-target")));
    require(editor.isNull(), "changing the screenshot releases the retained model editor widget");
}

class BlockedRecognitionWorkers final {
  public:
    BlockedRecognitionWorkers() {
        require(m_pool->waitForDone(5000), "previous recognition workers have settled");
        m_pool->setMaxThreadCount(1);
        m_pool->start(QRunnable::create([this] {
            m_entered.release();
            m_release.acquire();
        }));
        require(m_entered.tryAcquire(1, 5000), "the recognition worker pool is blocked");
    }

    ~BlockedRecognitionWorkers() {
        releaseAndWait();
        m_pool->setMaxThreadCount(m_previousThreadCount);
    }

    void releaseAndWait() {
        if (!m_released) {
            m_released = true;
            m_release.release();
        }
        require(m_pool->waitForDone(5000), "pending recognition workers settle after release");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    }

  private:
    QThreadPool* m_pool = QThreadPool::globalInstance();
    int m_previousThreadCount = m_pool->maxThreadCount();
    QSemaphore m_entered;
    QSemaphore m_release;
    bool m_released = false;
};

void largeSeededTablesMaterializeOnlyWhenSelectedAndIgnoreStaleWorkers() {
    ModelSettingsGuard guard;
    const QString baselineText(64 * 1024, QLatin1Char('A'));
    const QString draftText(64 * 1024, QLatin1Char('D'));
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("large-seeded-table-models");
    seed.tableModelSelection = QStringLiteral("vision-a");
    auto large =
        tableEntry(QStringLiteral("vision-a"),
                   QStringLiteral("<table><tr><td>%1</td></tr></table>").arg(baselineText));
    large.draftHtml = QStringLiteral("<table><tr><td>%1</td></tr></table>").arg(draftText);
    seed.tableEntries = {
        large,
        tableEntry(QStringLiteral("vision-b"),
                   QStringLiteral("<table><tr><td>Small B</td></tr></table>")),
    };
    {
        BlockedRecognitionWorkers workers;
        ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr,
                                                       headlessActions());
        session.setTarget(target(seed.key));
        session.seedRecognitionResults(seed);
        const auto inactive = session.cachedRecognitionResults();
        const auto inactiveLarge = std::find_if(
            inactive.tableEntries.cbegin(), inactive.tableEntries.cend(),
            [](const auto& entry) { return entry.model == QStringLiteral("vision-a"); });
        require(!session.active() && !session.busy() && session.workflowResult().isEmpty() &&
                    inactive.tableEntries.size() == 2 &&
                    inactiveLarge != inactive.tableEntries.cend() &&
                    inactiveLarge->result.html == large.result.html &&
                    inactiveLarge->draftHtml == large.draftHtml,
                "inactive large table seeds retain baseline and draft metadata without parsing or "
                "requiring an API provider");
        session.activate(Mode::Table);
        require(session.busy(Mode::Table) && session.workflowResult().isEmpty(),
                "selected large table seeds become busy while their worker is pending");
        session.activate(Mode::Table);
        require(session.busy(Mode::Table) && session.workflowResult().isEmpty() &&
                    session.recognitionModelSelection(Mode::Table) == QStringLiteral("vision-a"),
                "repeated activation preserves the pending large parser and its selected model");
        session.setRecognitionModel(QStringLiteral("vision-b"));
        const auto smallResult = session.workflowResult();
        require(!session.busy() && cellAt(smallResult, 0, 0).value(QStringLiteral("text")) ==
                                       QStringLiteral("Small B"),
                "a small cached model restores immediately while another model's large parser is "
                "blocked");
        workers.releaseAndWait();
        require(!session.busy() && session.workflowResult() == smallResult &&
                    session.recognitionModelSelection(Mode::Table) == QStringLiteral("vision-b"),
                "completion from a previously selected large table cannot replace the current "
                "model's result");
        session.setRecognitionModel(QStringLiteral("vision-a"));
        until([&] { return !session.busy(); }, "reselecting the large seeded table completes");
        require(cellAt(session.workflowResult(), 0, 0).value(QStringLiteral("text")) == draftText &&
                    session.recognitionResultsSnapshot().tableEntries.size() == 2,
                "reselecting the large model materializes its own draft and keeps both model "
                "baselines");
        const auto restoredLarge = session.workflowResult();
        session.activate(Mode::Table);
        require(!session.busy() && session.workflowResult() == restoredLarge,
                "repeated activation reuses the materialized large draft without another worker");
        require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("reset_table")}}) &&
                    cellAt(session.workflowResult(), 0, 0).value(QStringLiteral("text")) ==
                        baselineText,
                "a lazily materialized large draft still resets to its recognized baseline");
    }
    {
        BlockedRecognitionWorkers workers;
        int publications = 0;
        auto actions = headlessActions();
        actions.setBusyState = [&](bool, bool, bool) { ++publications; };
        auto session = std::make_unique<ScreenshotRecognitionSessionController>(nullptr, nullptr,
                                                                                nullptr, actions);
        session->setTarget(target(seed.key));
        session->seedRecognitionResults(seed);
        session->activate(Mode::Table);
        require(session->busy(Mode::Table), "large seed parsing is pending before shutdown");
        session.reset();
        const int beforeCompletion = publications;
        workers.releaseAndWait();
        require(publications == beforeCompletion,
                "a pending large table parser cannot publish after its controller is destroyed");
    }
}

void invalidLazyTableSeedsRemoveOnlyTheirOwnEntryAndOfferRetry() {
    ModelSettingsGuard guard;
    const QString longText(64 * 1024, QLatin1Char('x'));
    for (const bool invalidDraft : {false, true}) {
        ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
        auto actions = headlessActions();
        actions.ensureContent = [&] { return &window; };
        ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
        ScreenshotRecognitionResults seed;
        seed.key = QStringLiteral("invalid-lazy-table-seed");
        seed.tableModelSelection = QStringLiteral("vision-a");
        auto invalid = tableEntry(
            QStringLiteral("vision-a"),
            invalidDraft ? QStringLiteral("<table><tr><td>%1</td></tr></table>").arg(longText)
                         : QStringLiteral("<p>%1</p>").arg(longText));
        if (invalidDraft)
            invalid.draftHtml = QStringLiteral("<p>Invalid table draft</p>");
        seed.tableEntries = {
            invalid,
            tableEntry(QStringLiteral("vision-b"),
                       QStringLiteral("<table><tr><td>Valid B</td></tr></table>")),
        };
        session.setTarget(target(seed.key));
        session.seedRecognitionResults(seed);
        require(session.cachedRecognitionResults().tableEntries.size() == 2 && !session.busy(),
                "inactive seeds retain unparsed per-model metadata until that model is selected");
        session.activate(Mode::Table);
        require(session.busy(Mode::Table), "large invalid seeds are validated on a worker");
        until([&] { return !session.busy(); }, "invalid lazy table validation completes");
        auto* retry = window.findChild<adqt::widgets::AdButton*>(
            QStringLiteral("screenshotRecognitionRetry"));
        const auto remaining = session.cachedRecognitionResults();
        require(session.workflowResult().isEmpty() && remaining.tableEntries.size() == 1 &&
                    remaining.tableEntries.at(0).model == QStringLiteral("vision-b") && retry &&
                    retry->isEnabled() &&
                    !session.workflowState().value(QStringLiteral("error")).toString().isEmpty(),
                "invalid lazy baselines and drafts remove only the selected entry and expose Retry "
                "without a provider or model fallback");
        session.setRecognitionModel(QStringLiteral("vision-b"));
        require(!session.busy() &&
                    cellAt(session.workflowResult(), 0, 0).value(QStringLiteral("text")) ==
                        QStringLiteral("Valid B"),
                "invalid model data never prevents using another cached table model");
    }
}

void latexModelSwitchesPreserveEmptyDraftsAndHeadlessExports() {
    ModelSettingsGuard guard;
    RecognitionServer server;
    SnowShotApiClient api(server.url());
    loadCatalog(api);
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, headlessActions());
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("latex-model-history");
    seed.latexModelSelection = QStringLiteral("vision-a");
    seed.latexEntries = {
        latexEntry(QStringLiteral("vision-a"), QStringLiteral("\\alpha\r\n+\\beta"), &api),
        latexEntry(QStringLiteral("vision-b"), QStringLiteral("\\frac{x}{y}"), &api),
    };
    session.setTarget(target(seed.key));
    session.seedRecognitionResults(seed);
    session.activate(Mode::Latex);
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_text")},
                                  {QStringLiteral("text"), QString()}}),
            "an empty LaTeX source is a valid edited draft");
    session.setRecognitionModel(QStringLiteral("vision-b"));
    require(session.latexDraft() == QStringLiteral("\\frac{x}{y}"),
            "model B exposes its own formula");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_text")},
                                  {QStringLiteral("text"), QStringLiteral("\\gamma + 1")}}),
            "edit model B formula");
    session.setRecognitionModel(QStringLiteral("vision-a"));
    require(session.latexDraft().isEmpty() &&
                session.workflowResult().value(QStringLiteral("kind")) == QStringLiteral("latex") &&
                session.workflowResult().value(QStringLiteral("text")).toString().isEmpty(),
            "returning to model A preserves its empty draft in headless workflow output");
    const auto exportA = session.fileExportSnapshot();
    auto clipboardA = session.recognitionClipboardMimeData();
    require(exportA && exportA->kind == ScreenshotRecognitionFileKind::Latex &&
                exportA->source.isEmpty() && clipboardA && clipboardA->text().isEmpty(),
            "LaTeX export and copy use the selected model's empty draft");
    const auto snapshot = session.recognitionResultsSnapshot();
    require(snapshot.visibleLatex && snapshot.latexModelSelection == QStringLiteral("vision-a") &&
                snapshot.latexEntries.size() == 2 &&
                std::any_of(snapshot.latexEntries.cbegin(), snapshot.latexEntries.cend(),
                            [](const auto& entry) {
                                return entry.model == QStringLiteral("vision-a") && entry.draft &&
                                       entry.draft->isEmpty();
                            }),
            "LaTeX snapshots distinguish an empty draft from an absent draft");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("undo")}}) &&
                session.latexDraft() == seed.latexEntries[0].result.latex,
            "model A retains formula undo across model changes");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("redo")}}) &&
                session.latexDraft().isEmpty(),
            "model A redo restores the empty source");
    session.setRecognitionModel(QStringLiteral("vision-b"));
    require(session.fileExportSnapshot()->source == QStringLiteral("\\gamma + 1") &&
                session.workflowResult().value(QStringLiteral("text")) ==
                    QStringLiteral("\\gamma + 1") &&
                Settings().tableModel() == screenshotDedicatedRecognitionModelId() &&
                Settings().latexModel() == QStringLiteral("vision-b"),
            "selected formula exports and LaTeX preferences remain independent of Table");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("reset_text")}}) &&
                session.latexDraft() == seed.latexEntries[1].result.latex &&
                server.modelRequests == 1 && server.visionRequests.isEmpty(),
            "each model resets to its own recognition baseline without new uploads");
}

void seededBuiltinResultsWorkWithoutProvidersAndLegacyResultsStayDedicated() {
    ModelSettingsGuard guard;
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, headlessActions());
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("headless-builtins");
    seed.tableModelSelection = QStringLiteral("vision-a");
    seed.latexModelSelection = QStringLiteral("vision-b");
    seed.tableEntries = {
        tableEntry(QStringLiteral("vision-a"),
                   QStringLiteral("<table><tr><td>A</td></tr></table>")),
        tableEntry(QStringLiteral("vision-b"),
                   QStringLiteral("<table><tr><td>B</td></tr></table>")),
    };
    seed.latexEntries = {latexEntry(QStringLiteral("vision-b"), QStringLiteral("x^2"))};
    session.setTarget(target(seed.key));
    session.seedRecognitionResults(seed);
    session.activate(Mode::Table);
    require(session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("A"),
            "headless cached builtin tables activate without a recognition provider");
    session.setRecognitionModel(QStringLiteral("vision-b"));
    require(session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("B"),
            "headless cached model switches expose the selected table");
    session.activate(Mode::Latex);
    require(session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("x^2"),
            "headless cached builtin formulas activate without a recognition provider");

    ScreenshotRecognitionSessionController legacy(nullptr, nullptr, nullptr, headlessActions());
    ScreenshotRecognitionResults old;
    old.key = QStringLiteral("legacy-dedicated");
    old.table =
        SnowShotTableResult{QStringLiteral("<table><tr><td>Legacy</td></tr></table>"), {}, {}};
    old.latex = SnowShotLatexResult{QStringLiteral("\\alpha"), {}, {}};
    old.latexDraft = QString();
    legacy.setTarget(target(old.key));
    legacy.seedRecognitionResults(old);
    require(legacy.recognitionModelSelection(Mode::Table) ==
                    screenshotDedicatedRecognitionModelId() &&
                legacy.recognitionModelSelection(Mode::Latex) ==
                    screenshotDedicatedRecognitionModelId(),
            "legacy recognition results promote to dedicated-model cache entries");
    legacy.activate(Mode::Latex);
    const auto promoted = legacy.recognitionResultsSnapshot();
    require(legacy.latexDraft().isEmpty() && promoted.tableEntries.size() == 1 &&
                promoted.latexEntries.size() == 1 &&
                promoted.latexEntries[0].model == screenshotDedicatedRecognitionModelId() &&
                promoted.latexEntries[0].draft && promoted.latexEntries[0].draft->isEmpty(),
            "legacy promotion preserves an empty formula draft and both dedicated baselines");
    require(legacy.editWorkflow({{QStringLiteral("action"), QStringLiteral("reset_text")}}) &&
                legacy.latexDraft() == QStringLiteral("\\alpha"),
            "legacy formula reset restores its original recognition baseline");
}

void changedCustomConfigurationInvalidatesOnlyItsOwnResults() {
    ModelSettingsGuard guard;
    RecognitionServer server;
    SnowShotApiClient api(server.url());
    loadCatalog(api);
    snow_shot::CustomAiModelConfiguration custom{
        QStringLiteral("11111111-1111-4111-8111-111111111111"),
        QStringLiteral("Custom vision"),
        server.url() + QStringLiteral("/v1"),
        QStringLiteral("key-a"),
        QStringLiteral("remote-model"),
        true};
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, headlessActions());
    api.setCustomModels({custom});
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("custom-invalidation");
    seed.tableModelSelection = custom.selectionId();
    seed.tableEntries = {
        tableEntry(custom.selectionId(), QStringLiteral("<table><tr><td>Custom</td></tr></table>"),
                   &api),
        tableEntry(QStringLiteral("vision-a"),
                   QStringLiteral("<table><tr><td>Built-in</td></tr></table>"), &api),
    };
    seed.latexEntries = {latexEntry(custom.selectionId(), QStringLiteral("x^2"), &api)};
    session.setTarget(target(seed.key));
    session.seedRecognitionResults(seed);
    session.activate(Mode::Table);
    require(session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("Custom"),
            "configured custom model activates its fingerprint-matched table");
    custom.name = QStringLiteral("Renamed custom vision");
    api.setCustomModels({custom});
    require(session.cachedRecognitionResults().tableEntries.size() == 2 &&
                session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("Custom"),
            "renaming a custom model preserves cached recognition and edits");
    custom.apiKey = QStringLiteral("key-b");
    api.setCustomModels({custom});
    const auto invalidated = session.cachedRecognitionResults();
    require(invalidated.tableEntries.size() == 1 &&
                invalidated.tableEntries[0].model == QStringLiteral("vision-a") &&
                invalidated.latexEntries.isEmpty() && session.workflowResult().isEmpty() &&
                !session.workflowState().value(QStringLiteral("error")).toString().isEmpty(),
            "connection changes invalidate that custom model's table and formula caches and clear "
            "its active output");
    session.seedRecognitionResults(seed);
    require(session.cachedRecognitionResults().tableEntries.size() == 1,
            "stale persisted custom fingerprints cannot restore invalidated results");
    session.setRecognitionModel(QStringLiteral("vision-a"));
    require(session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("Built-in") &&
                server.visionRequests.isEmpty(),
            "invalidating a custom model leaves other cached model results immediately usable");
}

void sourceSwitchesInvalidateBuiltinsAndPreserveIndependentCustomEndpoints() {
    ModelSettingsGuard guard;
    RecognitionServer original;
    RecognitionServer replacement;
    SnowShotApiClient api(original.url());
    loadCatalog(api);
    const snow_shot::CustomAiModelConfiguration custom{
        QStringLiteral("22222222-2222-4222-8222-222222222222"),
        QStringLiteral("Independent vision"),
        original.url() + QStringLiteral("/v1"),
        {},
        QStringLiteral("custom-model"),
        true};
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, headlessActions());
    api.setCustomModels({custom});
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("source-namespace");
    seed.tableModelSelection = custom.selectionId();
    seed.tableEntries = {
        tableEntry(custom.selectionId(),
                   QStringLiteral("<table><tr><td>Custom source</td></tr></table>"), &api),
        tableEntry(QStringLiteral("vision-a"),
                   QStringLiteral("<table><tr><td>Old builtin</td></tr></table>"), &api),
        tableEntry(screenshotDedicatedRecognitionModelId(),
                   QStringLiteral("<table><tr><td>Old dedicated</td></tr></table>")),
    };
    session.setTarget(target(seed.key));
    session.seedRecognitionResults(seed);
    session.activate(Mode::Table);
    require(api.setBaseUrl(replacement.url()), "change Snow Shot recognition server");
    const auto remaining = session.cachedRecognitionResults();
    require(remaining.tableEntries.size() == 1 &&
                remaining.tableEntries[0].model == custom.selectionId() &&
                session.workflowResult().value(QStringLiteral("text")) ==
                    QStringLiteral("Custom source") &&
                replacement.connections == 0,
            "switching the Snow Shot server invalidates builtin and dedicated results while "
            "preserving an independently configured custom endpoint");
    session.setRecognitionModel(QStringLiteral("vision-a"));
    require(session.workflowResult().isEmpty() &&
                !session.workflowState().value(QStringLiteral("error")).toString().isEmpty(),
            "selecting a builtin after a source switch cannot display the former server's cached "
            "output");
}

void unavailableSelectionsOfferRetryWithoutDedicatedOrCustomFallback() {
    ModelSettingsGuard guard;
    require(Settings().setTableModel(QStringLiteral("missing-model")) &&
                Settings().setLatexModel(screenshotDefaultVisionRecognitionModelId()),
            "set explicit unavailable and default-vision recognition preferences");
    RecognitionServer server;
    SnowShotApiClient api(server.url());
    loadCatalog(api);
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    auto actions = headlessActions();
    actions.ensureContent = [&] { return &window; };
    ScreenshotRecognitionSessionController missing(nullptr, nullptr, &api, actions);
    missing.setTarget(target(QStringLiteral("unavailable-model")));
    missing.activate(Mode::Table);
    until([&] { return !missing.busy(); }, "unavailable model discovery settles");
    auto* retry =
        window.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRecognitionRetry"));
    require(missing.workflowResult().isEmpty() &&
                !missing.workflowState().value(QStringLiteral("error")).toString().isEmpty() &&
                retry && retry->isEnabled() && server.dedicatedRequests == 0 &&
                server.visionRequests.isEmpty(),
            "an unavailable explicit model presents Retry without silently invoking dedicated "
            "recognition");
    retry->click();
    until([&] { return !missing.busy(); }, "retry of an unavailable selection settles");
    require(server.modelRequests == 2 && server.dedicatedRequests == 0 &&
                server.visionRequests.isEmpty(),
            "retry refreshes an unavailable selection without falling back to a different model");

    ScreenshotRecognitionSessionController noProvider(nullptr, nullptr, nullptr, headlessActions());
    noProvider.setTarget(target(QStringLiteral("default-no-provider")));
    noProvider.activate(Mode::Latex);
    require(
        !noProvider.busy() && noProvider.workflowResult().isEmpty() &&
            !noProvider.workflowState().value(QStringLiteral("error")).toString().isEmpty() &&
            noProvider.recognitionModelSelection(Mode::Latex) ==
                screenshotDefaultVisionRecognitionModelId(),
        "default Snow Shot vision without a provider remains a visible unavailable-model error");

    RecognitionServer customOnly;
    customOnly.includeBuiltinVision = false;
    SnowShotApiClient customApi(customOnly.url());
    loadCatalog(customApi);
    ScreenshotRecognitionSessionController builtinDefault(nullptr, nullptr, &customApi,
                                                          headlessActions());
    customApi.setCustomModels({{QStringLiteral("33333333-3333-4333-8333-333333333333"),
                                QStringLiteral("Only custom vision"),
                                customOnly.url() + QStringLiteral("/v1"),
                                {},
                                QStringLiteral("custom-model"),
                                true}});
    builtinDefault.setTarget(target(QStringLiteral("default-custom-only")));
    builtinDefault.activate(Mode::Latex);
    until([&] { return !builtinDefault.busy(); }, "builtin-only default discovery settles");
    require(
        builtinDefault.workflowResult().isEmpty() &&
            !builtinDefault.workflowState().value(QStringLiteral("error")).toString().isEmpty() &&
            customOnly.visionRequests.isEmpty() && customOnly.dedicatedRequests == 0,
        "Snow Shot's default vision alias must never resolve to a custom model or dedicated "
        "fallback");
}

void retryRefreshesUnavailableCatalogsForBothRecognitionModes() {
    ModelSettingsGuard guard;
    for (const Mode mode : {Mode::Table, Mode::Latex}) {
        for (const QString& selection :
             {QStringLiteral("new-vision"), screenshotDefaultVisionRecognitionModelId()}) {
            RecognitionServer server;
            server.catalogOrder = {QStringLiteral("vision-a")};
            server.includeBuiltinVision = selection != screenshotDefaultVisionRecognitionModelId();
            server.visionSource = mode == Mode::Table
                                      ? QStringLiteral("<table><tr><th>Recovered</th></tr></table>")
                                      : QStringLiteral("x^2");
            SnowShotApiClient api(server.url());
            loadCatalog(api);
            require(mode == Mode::Table ? Settings().setTableModel(selection)
                                        : Settings().setLatexModel(selection),
                    "select a currently unavailable builtin recognition model");
            ScreenshotRecognitionSessionController session(nullptr, nullptr, &api,
                                                           headlessActions());
            session.setTarget(target(QStringLiteral("refresh-unavailable-model")));
            session.activate(mode);
            until([&] { return !session.busy(); }, "initial unavailable model settles");
            require(
                session.workflowResult().isEmpty() &&
                    !session.workflowState().value(QStringLiteral("error")).toString().isEmpty(),
                "an unavailable model reports an error before the server catalog changes");
            server.holdCatalog = true;
            server.includeBuiltinVision = true;
            server.catalogOrder.append(QStringLiteral("new-vision"));
            session.retryRecognition();
            until([&] { return server.modelRequests == 2 && server.heldCatalog; },
                  "retry starts a fresh catalog request despite the cached catalog");
            require(session.busy(mode) && session.workflowResult().isEmpty() &&
                        session.recognitionModelSelection(mode) == selection &&
                        server.visionRequests.isEmpty() && server.dedicatedRequests == 0,
                    "retry awaits discovery and preserves the selected model without fallback");
            server.respondCatalog(server.heldCatalog);
            until([&] { return !session.busy() && !session.workflowResult().isEmpty(); },
                  "the refreshed catalog makes the selected model usable");
            const QString expected = selection == screenshotDefaultVisionRecognitionModelId()
                                         ? QStringLiteral("vision-a")
                                         : selection;
            require(server.modelRequests == 2 && server.visionRequests.size() == 1 &&
                        server.visionRequests.first().value(QStringLiteral("model")) == expected &&
                        session.workflowState().value(QStringLiteral("error")).toString().isEmpty(),
                    "both table and formula retries recover using the refreshed selected model");
        }
    }
}

void inFlightModelSwitchesCancelOldRequestsAndRetryFailures() {
    ModelSettingsGuard guard;
    require(Settings().setTableModel(QStringLiteral("vision-a")),
            "select builtin model A for network lifecycle test");
    RecognitionServer server;
    SnowShotApiClient api(server.url());
    loadCatalog(api);
    server.holdVisionModel = QStringLiteral("vision-a");
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, headlessActions());
    session.setTarget(target(QStringLiteral("network-model-switch")));
    session.activate(Mode::Table);
    until([&] { return server.heldVision != nullptr; },
          "model A starts a held recognition request");
    require(session.busy(Mode::Table) && session.workflowResult().isEmpty(),
            "pending vision recognition exposes no stale output");
    session.setRecognitionModel(QStringLiteral("vision-b"));
    until(
        [&] {
            return !session.busy() && session.workflowResult().value(QStringLiteral("kind")) ==
                                          QStringLiteral("table");
        },
        "model B recognition completes after switching away from model A");
    until(
        [&] {
            return !server.heldVision ||
                   server.heldVision->state() == QAbstractSocket::UnconnectedState;
        },
        "switching models aborts the old model's transport");
    require(
        server.visionRequests.size() == 2 &&
            server.visionRequests[0].value(QStringLiteral("model")) == QStringLiteral("vision-a") &&
            server.visionRequests[1].value(QStringLiteral("model")) == QStringLiteral("vision-b") &&
            session.cachedRecognitionResults().tableEntries.size() == 1 &&
            session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("B result"),
        "only the newly selected model may publish or cache a completed result");
    server.failNextVision = true;
    session.setTarget(target(QStringLiteral("network-retry")));
    session.activate(Mode::Table);
    until([&] { return server.visionRequests.size() == 3 && !session.busy(); },
          "selected vision model failure settles");
    require(session.workflowResult().isEmpty() &&
                !session.workflowState().value(QStringLiteral("error")).toString().isEmpty(),
            "a recognition failure clears output and reports retryable error");
    session.retryRecognition();
    until(
        [&] {
            return server.visionRequests.size() == 4 && !session.busy() &&
                   !session.workflowResult().isEmpty();
        },
        "recognition retry succeeds using the selected model");
    require(server.modelRequests == 1 &&
                session.cachedRecognitionResults().tableEntries.size() == 1 &&
                session.workflowState().value(QStringLiteral("error")).toString().isEmpty(),
            "retry reuses the catalog, clears its error, and caches only the successful result");
}

void defaultVisionModelRemainsBoundAcrossCatalogRefreshes() {
    ModelSettingsGuard guard;
    require(Settings().setTableModel(screenshotDefaultVisionRecognitionModelId()) &&
                Settings().setLatexModel(screenshotDefaultVisionRecognitionModelId()),
            "select semantic builtin defaults for both recognition tools");
    for (const Mode mode : {Mode::Table, Mode::Latex}) {
        RecognitionServer server;
        SnowShotApiClient api(server.url());
        loadCatalog(api);
        server.holdVisionModel = QStringLiteral("vision-a");
        ScreenshotRecognitionModelState publishedModelState;
        ScreenshotRecognitionModelState inactiveModelState;
        auto actions = headlessActions();
        actions.setRecognitionModelState =
            [mode, &publishedModelState,
             &inactiveModelState](int publishedMode, const ScreenshotRecognitionModelState& state) {
                if (publishedMode == static_cast<int>(mode))
                    publishedModelState = state;
                else
                    inactiveModelState = state;
            };
        ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, std::move(actions));
        session.setTarget(target(QStringLiteral("default-catalog-refresh-%1").arg(int(mode))));
        session.activate(mode);
        until([&] { return server.heldVision != nullptr; },
              "semantic default starts extraction using the initial builtin");
        require(publishedModelState.selection == screenshotDefaultVisionRecognitionModelId() &&
                    publishedModelState.effectiveModel == QStringLiteral("vision-a") &&
                    inactiveModelState.effectiveModel.isEmpty(),
                "only the active default selector publishes its bound extraction model");
        server.catalogOrder = {QStringLiteral("vision-b"), QStringLiteral("vision-a")};
        refreshCatalog(api);
        require(api.builtInVisionModel() == QStringLiteral("vision-b") && session.busy(mode) &&
                    session.workflowResult().isEmpty() && server.visionRequests.size() == 1 &&
                    publishedModelState.effectiveModel == QStringLiteral("vision-a") &&
                    inactiveModelState.effectiveModel.isEmpty(),
                "catalog reordering keeps the active default extraction bound to its model");
        const QString recognizedA =
            mode == Mode::Table ? QStringLiteral("<table><tr><td>A result</td></tr></table>")
                                : QStringLiteral("a^2");
        RecognitionServer::respond(server.heldVision,
                                   RecognitionServer::frame(recognizedA) + "data: [DONE]\n\n",
                                   "text/event-stream");
        until([&] { return !session.busy() && !session.workflowResult().isEmpty(); },
              "the original default extraction and asynchronous table parse publish successfully");
        require(session.workflowResult().value(QStringLiteral("text")) ==
                        (mode == Mode::Table ? QStringLiteral("A result") : recognizedA) &&
                    publishedModelState.effectiveModel == QStringLiteral("vision-a"),
                "a refreshed catalog must not discard the original default model result");
        const QJsonObject edit =
            mode == Mode::Table
                ? QJsonObject{{QStringLiteral("action"), QStringLiteral("set_cell")},
                              {QStringLiteral("row"), 0},
                              {QStringLiteral("column"), 0},
                              {QStringLiteral("text"), QStringLiteral("A edited")}}
                : QJsonObject{{QStringLiteral("action"), QStringLiteral("set_text")},
                              {QStringLiteral("text"), QStringLiteral("a^3")}};
        require(session.editWorkflow(edit), "edit the active semantic default result");
        const auto edited = session.workflowResult();
        server.catalogOrder = {QStringLiteral("vision-a"), QStringLiteral("vision-b")};
        refreshCatalog(api);
        server.catalogOrder = {QStringLiteral("vision-b"), QStringLiteral("vision-a")};
        refreshCatalog(api);
        require(session.workflowResult() == edited && !session.busy() &&
                    server.visionRequests.size() == 1 && server.dedicatedRequests == 0 &&
                    publishedModelState.effectiveModel == QStringLiteral("vision-a"),
                "later catalog refreshes preserve the bound model's edited successful draft");
        if (mode == Mode::Latex) {
            const auto exported = session.fileExportSnapshot();
            const auto clipboard = session.recognitionClipboardMimeData();
            require(exported && exported->source == QStringLiteral("a^3") && clipboard &&
                        clipboard->text() == QStringLiteral("a^3"),
                    "catalog reordering preserves edited LaTeX exports and clipboard data");
        }
        session.deactivate();
        server.holdVisionModel.clear();
        server.visionSource = mode == Mode::Table
                                  ? QStringLiteral("<table><tr><td>B result</td></tr></table>")
                                  : QStringLiteral("b^2");
        session.activate(mode);
        until([&] { return !session.busy() && server.visionRequests.size() == 2; },
              "a fresh activation resolves the newest first builtin model");
        require(
            server.visionRequests.last().value(QStringLiteral("model")) ==
                    QStringLiteral("vision-b") &&
                session.workflowResult().value(QStringLiteral("text")) ==
                    (mode == Mode::Table ? QStringLiteral("B result") : QStringLiteral("b^2")) &&
                publishedModelState.effectiveModel == QStringLiteral("vision-b") &&
                inactiveModelState.effectiveModel.isEmpty(),
            "deactivation releases the default binding while retaining independent results");
        session.setTarget(target(QStringLiteral("default-new-target-%1").arg(int(mode))));
        session.activate(mode);
        until([&] { return !session.busy() && server.visionRequests.size() == 3; },
              "a new target resolves the refreshed default model");
        require(publishedModelState.selection == screenshotDefaultVisionRecognitionModelId() &&
                    publishedModelState.effectiveModel == QStringLiteral("vision-b") &&
                    inactiveModelState.effectiveModel.isEmpty() &&
                    server.visionRequests.last().value(QStringLiteral("model")) ==
                        QStringLiteral("vision-b"),
                "a new target publishes the current default model without changing its preference");
    }
}

void catalogFetchIsSharedAndCancelledSubscribersDoNotAffectOtherTools() {
    ModelSettingsGuard guard;
    require(Settings().setLatexModel(screenshotDefaultVisionRecognitionModelId()),
            "select the builtin vision default for the shared discovery regression");
    RecognitionServer server;
    server.holdCatalog = true;
    server.visionSource = QStringLiteral("x^2");
    SnowShotApiClient api(server.url());
    ScreenshotRecognitionSessionController table(nullptr, nullptr, &api, headlessActions());
    ScreenshotRecognitionSessionController latex(nullptr, nullptr, &api, headlessActions());
    table.setTarget(target(QStringLiteral("shared-table")));
    latex.setTarget(target(QStringLiteral("shared-latex")));
    table.activate(Mode::Table);
    latex.activate(Mode::Latex);
    until([&] { return server.heldCatalog != nullptr; },
          "activated tools request the model catalog lazily");
    require(
        server.modelRequests == 1 && latex.busy(Mode::Latex) && server.visionRequests.isEmpty(),
        "concurrent activated tools share discovery while default LaTeX waits for builtin vision");
    table.deactivate();
    require(
        server.heldCatalog && server.heldCatalog->state() == QAbstractSocket::ConnectedState,
        "cancelling one tool's discovery subscription preserves the other tool's catalog request");
    server.respondCatalog(server.heldCatalog);
    until(
        [&] {
            return !latex.busy() &&
                   latex.workflowResult().value(QStringLiteral("kind")) == QStringLiteral("latex");
        },
        "the surviving tool completes after shared model discovery");
    require(server.modelRequests == 1 && server.visionRequests.size() == 1 &&
                server.visionRequests[0].value(QStringLiteral("model")) ==
                    QStringLiteral("vision-a") &&
                latex.latexDraft() == QStringLiteral("x^2") && !table.active(),
            "default LaTeX resolves Snow Shot builtin vision without reviving a deactivated tool");
}

void providerDestructionDuringDiscoveryPublishesRetryableFailure() {
    ModelSettingsGuard guard;
    require(Settings().setLatexModel(screenshotDefaultVisionRecognitionModelId()),
            "select the builtin vision default for the provider destruction regression");
    RecognitionServer server;
    server.holdCatalog = true;
    auto api = std::make_unique<SnowShotApiClient>(server.url());
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    auto actions = headlessActions();
    actions.ensureContent = [&] { return &window; };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, api.get(), actions);
    session.setTarget(target(QStringLiteral("discovery-provider-destruction")));
    session.activate(Mode::Latex);
    until([&] { return server.heldCatalog != nullptr; }, "LaTeX awaits held model discovery");
    require(session.busy(Mode::Latex) && session.workflowResult().isEmpty(),
            "default vision recognition waits for discovery before starting extraction");
    api.reset();
    until([&] { return !session.busy(); }, "provider destruction settles discovery busy state");
    auto* retry =
        window.findChild<adqt::widgets::AdButton*>(QStringLiteral("screenshotRecognitionRetry"));
    require(!session.workflowState().value(QStringLiteral("error")).toString().isEmpty() && retry &&
                retry->isEnabled() && session.workflowResult().isEmpty() &&
                server.visionRequests.isEmpty() && server.dedicatedRequests == 0,
            "destroying a provider during discovery must show an actionable error and Retry "
            "instead of silently leaving an empty result");
}

void choiceOnlySnapshotsRestoreLocalSelectionWithoutChangingPreferences() {
    ModelSettingsGuard guard;
    ScreenshotRecognitionSessionController original(nullptr, nullptr, nullptr, headlessActions());
    original.setTarget(target(QStringLiteral("choice-only-snapshot")));
    original.activate(Mode::Table);
    original.setRecognitionModel(QStringLiteral("vision-b"));
    original.deactivate();
    const auto snapshot = original.recognitionResultsSnapshot();
    require(snapshot.key == QStringLiteral("choice-only-snapshot") && !snapshot.isEmpty() &&
                snapshot.tableEntries.isEmpty() && snapshot.latexEntries.isEmpty() &&
                snapshot.tableModelSelection == QStringLiteral("vision-b"),
            "recognition snapshots retain the chosen model even when extraction has no completed "
            "result");
    require(Settings().setTableModel(QStringLiteral("vision-a")),
            "change the global Table preference before restoring a pin");
    ScreenshotRecognitionSessionController restored(nullptr, nullptr, nullptr, headlessActions());
    restored.setTarget(target(snapshot.key));
    restored.seedRecognitionResults(snapshot);
    require(restored.recognitionModelSelection(Mode::Table) == QStringLiteral("vision-b") &&
                restored.recognitionModelSelection(Mode::Latex) == snapshot.latexModelSelection &&
                Settings().tableModel() == QStringLiteral("vision-a") &&
                Settings().latexModel() == guard.latex,
            "restoring a choice-only snapshot selects the local saved model without rewriting "
            "global preferences");
    require(restored.recognitionResultsSnapshot().key == snapshot.key &&
                restored.cachedRecognitionResults().tableModelSelection ==
                    QStringLiteral("vision-b"),
            "restored model choices survive another snapshot before any tool activation");
}

void currentLocaleDiscoveryGatesBuiltinsWhileCachedCustomResultsRemainUsable() {
    ModelSettingsGuard modelGuard;
    RecognitionLanguageGuard languageGuard;
    auto& language = snow_shot::presentation::LanguageManager::instance();
    language.initialize();
    const auto catalogs = language.availableLanguages();
    require(std::any_of(catalogs.cbegin(), catalogs.cend(),
                        [](const auto& catalog) {
                            return catalog.localeName == QStringLiteral("en_US");
                        }) &&
                std::any_of(catalogs.cbegin(), catalogs.cend(),
                            [](const auto& catalog) {
                                return catalog.localeName == QStringLiteral("zh_CN");
                            }),
            "recognition locale fixture embeds real English and Simplified Chinese catalogs");
    require(language.setLanguage(QStringLiteral("en_US")),
            "select English for the initial recognition catalog");
    RecognitionServer server;
    SnowShotApiClient api(server.url());
    loadCatalog(api);
    const snow_shot::CustomAiModelConfiguration custom{
        QStringLiteral("44444444-4444-4444-8444-444444444444"),
        QStringLiteral("Locale-independent vision"),
        server.url() + QStringLiteral("/v1"),
        {},
        QStringLiteral("custom-model"),
        true};
    ScreenshotRecognitionSessionController session(nullptr, nullptr, &api, headlessActions());
    api.setCustomModels({custom});
    ScreenshotRecognitionResults seed;
    seed.key = QStringLiteral("locale-recognition-discovery");
    seed.latexModelSelection = screenshotDefaultVisionRecognitionModelId();
    seed.latexEntries = {
        latexEntry(QStringLiteral("vision-a"), QStringLiteral("a^2"), &api),
        latexEntry(custom.selectionId(), QStringLiteral("c^2"), &api),
    };
    session.setTarget(target(seed.key));
    session.seedRecognitionResults(seed);
    session.activate(Mode::Latex);
    require(session.latexDraft() == QStringLiteral("a^2"),
            "initial-locale default resolves the cached builtin formula");
    session.deactivate();
    server.holdCatalog = true;
    require(language.setLanguage(QStringLiteral("zh_CN")),
            "change recognition language before reactivation");
    require(server.modelRequests == 1, "changing language while inactive must not fetch models");
    session.activate(Mode::Latex);
    until([&] { return server.heldCatalog != nullptr; },
          "reactivation requests a current-locale catalog");
    require(session.busy(Mode::Latex) && session.workflowResult().isEmpty(),
            "the default builtin alias must await a current-locale catalog rather than expose a "
            "stale catalog's result");
    session.setRecognitionModel(custom.selectionId());
    require(!session.busy() &&
                session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("c^2"),
            "cached custom results remain immediately usable while builtin locale discovery is "
            "pending");
    session.setRecognitionModel(QStringLiteral("vision-a"));
    require(!session.busy() &&
                session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("a^2"),
            "an explicit builtin reuses a fingerprint-valid cached result without blocking edits "
            "behind locale discovery");
    session.setRecognitionModel(QStringLiteral("vision-b"));
    require(session.busy(Mode::Latex) && session.workflowResult().isEmpty() &&
                server.visionRequests.isEmpty(),
            "an uncached explicit builtin waits for current-locale availability before starting "
            "extraction");
    session.setRecognitionModel(screenshotDefaultVisionRecognitionModelId());
    server.respondCatalog(server.heldCatalog);
    until(
        [&] {
            return !session.busy() &&
                   session.workflowResult().value(QStringLiteral("text")) == QStringLiteral("a^2");
        },
        "current-locale model discovery restores the matching cached builtin result");
    require(
        server.modelRequests == 2 && server.visionRequests.isEmpty() &&
            api.cachedChatModelsLocale() == QStringLiteral("zh_CN"),
        "locale refresh performs one lazy catalog request and reuses completed extraction results");
}
} // namespace

void runRecognitionModelTests() {
    defaultsAndInactiveToolsDoNotFetchModels();
    defaultLatexUsesDedicatedRecognitionWhileCatalogIsLoading();
    visionTablesRequireActualMarkupAndRetainMergedEmptyCells();
    visionTableHeadersFollowGridCoordinates();
    tableModelSwitchesPreserveSpansEditsAndUndo();
    tableModelSwitchesReuseEditorAndRestoreIndependentViewState();
    largeSeededTablesMaterializeOnlyWhenSelectedAndIgnoreStaleWorkers();
    invalidLazyTableSeedsRemoveOnlyTheirOwnEntryAndOfferRetry();
    latexModelSwitchesPreserveEmptyDraftsAndHeadlessExports();
    seededBuiltinResultsWorkWithoutProvidersAndLegacyResultsStayDedicated();
    changedCustomConfigurationInvalidatesOnlyItsOwnResults();
    sourceSwitchesInvalidateBuiltinsAndPreserveIndependentCustomEndpoints();
    unavailableSelectionsOfferRetryWithoutDedicatedOrCustomFallback();
    retryRefreshesUnavailableCatalogsForBothRecognitionModes();
    inFlightModelSwitchesCancelOldRequestsAndRetryFailures();
    defaultVisionModelRemainsBoundAcrossCatalogRefreshes();
    catalogFetchIsSharedAndCancelledSubscribersDoNotAffectOtherTools();
    providerDestructionDuringDiscoveryPublishesRetryableFailure();
    choiceOnlySnapshotsRestoreLocalSelectionWithoutChangingPreferences();
    currentLocaleDiscoveryGatesBuiltinsWhileCachedCustomResultsRemainUsable();
}
