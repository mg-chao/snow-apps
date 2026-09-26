#include "snow_shot/app/mcp/screenshotmcpserver.h"
#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include "snow_shot/app/mcp/screenshotmcpselection.h"
#include <QApplication>
#include <QClipboard>
#include <QMimeData>
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <cstdlib>
#include <iostream>
#include <utility>
using namespace snow_shot::app::mcp;
namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}
template <class F> void await(F predicate) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    require(predicate(), "asynchronous operation timed out");
}
QByteArray frame(const QJsonObject& object) {
    const auto json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray result(8, Qt::Uninitialized);
    qToBigEndian(static_cast<quint32>(json.size() + 4), result.data());
    qToBigEndian(static_cast<quint32>(json.size()), result.data() + 4);
    return result + json;
}
QJsonObject receive(QLocalSocket& socket) {
    await([&] { return socket.bytesAvailable() >= 8; });
    const auto header = socket.peek(8);
    const auto size = qFromBigEndian<quint32>(header.constData());
    await([&] { return socket.bytesAvailable() >= size + 4; });
    const auto bytes = socket.read(size + 4);
    return QJsonDocument::fromJson(bytes.mid(8, qFromBigEndian<quint32>(bytes.constData() + 4)))
        .object();
}
QJsonObject request(const QString& id, const QString& method, const QJsonObject& params = {}) {
    return {{QStringLiteral("protocol"), QStringLiteral("snow-shot-mcp/1")},
            {QStringLiteral("request_id"), id},
            {QStringLiteral("method"), method},
            {QStringLiteral("params"), params}};
}
void transport() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary runtime directory");
    ScreenshotMcpServer server(nullptr, directory.path());
    ScreenshotMcpServer::Completion pending;
    int requests = 0;
    server.setRequestHandler([&](const ScreenshotMcpRequest& r, auto completion) {
        require(QThread::currentThread() == QCoreApplication::instance()->thread(),
                "GUI thread dispatch");
        ++requests;
        if (r.method == QStringLiteral("pending")) {
            pending = std::move(completion);
            return;
        }
        if (pending) {
            ScreenshotMcpResponse canceled;
            canceled.errorCode = QStringLiteral("canceled");
            pending(canceled);
            pending = {};
        }
        ScreenshotMcpResponse response;
        response.ok = true;
        completion(response);
    });
    QString error;
    require(server.start(&error), qPrintable(error));
    QFile descriptor(server.descriptorPath());
    require(descriptor.open(QIODevice::ReadOnly), "published descriptor");
    auto data = QJsonDocument::fromJson(descriptor.readAll()).object();
    descriptor.close();
    require(data.value(QStringLiteral("token")).toString().size() == 64, "256-bit token");
    ScreenshotMcpServer competing(nullptr, directory.path());
    require(!competing.start(), "exclusive endpoint owner");
    QLocalSocket invalid;
    invalid.connectToServer(server.socketName());
    require(invalid.waitForConnected(2000), "invalid peer connects");
    invalid.write(frame(request(QStringLiteral("bad"), QStringLiteral("handshake"),
                                {{QStringLiteral("token"), QStringLiteral("bad")}})));
    require(!receive(invalid).value(QStringLiteral("ok")).toBool(), "invalid token rejected");
    QLocalSocket client;
    client.connectToServer(server.socketName());
    require(client.waitForConnected(2000), "peer connects");
    auto hello =
        frame(request(QStringLiteral("hello"), QStringLiteral("handshake"),
                      {{QStringLiteral("token"), data.value(QStringLiteral("token"))},
                       {QStringLiteral("client_protocol"), QStringLiteral("snow-shot-mcp/1")}}));
    for (char byte : hello) {
        client.write(&byte, 1);
        client.flush();
        QCoreApplication::processEvents();
    }
    require(receive(client).value(QStringLiteral("ok")).toBool(), "fragmented handshake accepted");
    client.write(frame(request(QStringLiteral("one"), QStringLiteral("pending"))));
    await([&] { return bool(pending); });
    client.write(frame(request(QStringLiteral("cancel"), QStringLiteral("screenshot_cancel"))));
    require(receive(client).value(QStringLiteral("request_id")).toString() == QStringLiteral("one"),
            "pending request ID preserved");
    require(receive(client).value(QStringLiteral("request_id")).toString() ==
                QStringLiteral("cancel"),
            "bypass cancellation response preserved");
    require(requests == 2, "authenticated dispatch count");
    server.stop();
    require(!QFile::exists(server.descriptorPath()), "descriptor removed on disable");
    await([&] { return client.state() == QLocalSocket::UnconnectedState; });
    require(server.start(), "runtime re-enable");
    server.stop();
}
void descriptorOverride() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary descriptor directory");
    const QByteArray previous = qgetenv("SNOW_SHOT_MCP_DESCRIPTOR");
    const bool wasSet = qEnvironmentVariableIsSet("SNOW_SHOT_MCP_DESCRIPTOR");
    const QString descriptor = directory.filePath(QStringLiteral("custom/descriptor.json"));
    qputenv("SNOW_SHOT_MCP_DESCRIPTOR", descriptor.toUtf8());
    ScreenshotMcpServer server;
    require(server.descriptorPath() == descriptor, "descriptor override path");
    QString error;
    require(server.start(&error), qPrintable(error));
    require(QFile::exists(descriptor), "override descriptor published");
    server.stop();
    require(!QFile::exists(descriptor), "override descriptor removed");
    qputenv("SNOW_SHOT_MCP_DESCRIPTOR", QByteArrayLiteral("relative/descriptor.json"));
    ScreenshotMcpServer relativeOverride;
    require(!relativeOverride.start(), "relative override rejected");
    if (wasSet)
        qputenv("SNOW_SHOT_MCP_DESCRIPTOR", previous);
    else
        qunsetenv("SNOW_SHOT_MCP_DESCRIPTOR");
}
void selection() {
    ScreenshotSelectionModel model;
    const QRectF canvas(0, 0, 800, 600);
    QString field;
    require(applySelection(model, canvas,
                           {{QStringLiteral("bounds"), QJsonArray{10, 20, 100, 100}}}, &field),
            "rectangle selection");
    const auto original = model.selectionRegion().toJson();
    require(!applySelection(model, canvas, {{QStringLiteral("bounds"), QJsonArray{-1, 0, 20, 20}}},
                            &field),
            "outside canvas rejected");
    require(model.selectionRegion().toJson() == original, "invalid selection is atomic");
    for (const auto* type : {"polygon", "polyline", "freehand"}) {
        require(applySelection(
                    model, canvas,
                    {{QStringLiteral("type"), QLatin1String(type)},
                     {QStringLiteral("points"),
                      QJsonArray{QJsonArray{0, 0}, QJsonArray{100, 0}, QJsonArray{100, 100}}}},
                    &field),
                "path selection");
    }
}
void session() {
    QJsonObject editor{{QStringLiteral("capture_phase"), QStringLiteral("idle")}};
    int canceled = 0, artifacts = 0, mutations = 0;
    bool deferImage = false;
    std::function<void(QImage)> deliverImage;
    ScreenshotMcpSession::Ports ports;
    ports.state = [&] { return editor; };
    ports.begin = [&](const QJsonObject&, QString*) {
        editor.insert(QStringLiteral("capture_phase"), QStringLiteral("capturing"));
        return true;
    };
    ports.cancel = [&] {
        ++canceled;
        editor.insert(QStringLiteral("capture_phase"), QStringLiteral("idle"));
    };
    ports.selection = [&](const QJsonObject&, QString*) {
        editor.insert(QStringLiteral("selection_version"), ++mutations);
        return true;
    };
    ports.tool = [](const QString&, QString*) { return true; };
    ports.annotations = [](const QByteArray&, QJsonObject*, QString*) { return true; };
    ports.history = [&](bool) { editor.insert(QStringLiteral("document_revision"), ++mutations); };
    ports.artifact = [&](qreal) {
        ++artifacts;
        if (deferImage)
            return std::make_shared<ScreenshotExportArtifact>(
                ScreenshotExportSource::fromImageLoader(
                    [&](QObject*, std::function<void(QImage)> done) {
                        deliverImage = std::move(done);
                        return true;
                    }));
        QImage image(32, 20, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::red);
        return std::make_shared<ScreenshotExportArtifact>(ScreenshotExportSource::fromImage(image));
    };
    ports.copy = [](auto, auto done) {
        done(true);
        return true;
    };
    ports.pin = [](auto, auto done) {
        done(true);
        return true;
    };
    ports.direct = [](const QJsonObject&, auto done) {
        QImage image(24, 12, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::blue);
        done(image, {}, {});
        return true;
    };
    ScreenshotMcpSession session(std::move(ports));
    quint64 sequence = 0;
    const auto call = [&](QString method, QJsonObject params = {}, quint64 owner = 1,
                          std::optional<quint64> revision = std::nullopt) {
        ScreenshotMcpRequest r;
        r.connectionId = owner;
        r.requestId = QString::number(++sequence);
        r.idempotencyKey = r.requestId;
        r.method = method;
        r.params = params;
        r.sessionId = session.state().value(QStringLiteral("session_id")).toString();
        r.expectedRevision =
            revision ? revision
                     : std::optional<quint64>(static_cast<quint64>(
                           session.state().value(QStringLiteral("revision")).toInteger()));
        std::optional<ScreenshotMcpResponse> response;
        session.request(r, [&](auto value) { response = std::move(value); });
        if (method == QStringLiteral("screenshot_begin") && !response) {
            editor.insert(QStringLiteral("capture_phase"), QStringLiteral("editing"));
            session.capturePresented();
        }
        await([&] { return response.has_value(); });
        return *response;
    };
    require(call(QStringLiteral("screenshot_begin")).ok, "begin completes on capture presented");
    require(call(QStringLiteral("screenshot_begin"), {}, 2).errorCode == QStringLiteral("busy"),
            "one session globally");
    const auto oldRevision =
        static_cast<quint64>(session.state().value(QStringLiteral("revision")).toInteger());
    editor.insert(QStringLiteral("user_edit"), true);
    session.observe();
    require(call(QStringLiteral("screenshot_set_selection"), {}, 1, oldRevision).errorCode ==
                QStringLiteral("stale_revision"),
            "user edit conflicts");
    require(mutations == 0, "stale mutation not applied");
    require(call(QStringLiteral("screenshot_set_selection")).ok, "current revision applies");
    auto rendered = call(QStringLiteral("screenshot_render"));
    require(rendered.ok && !rendered.attachment.isEmpty(), "render PNG attachment");
    require(call(QStringLiteral("screenshot_render")).ok && artifacts == 1,
            "render cache reused across output revisions");
    require(call(QStringLiteral("screenshot_undo")).ok, "history mutation");
    require(call(QStringLiteral("screenshot_render")).ok && artifacts == 2,
            "history invalidates render cache");
    QTemporaryDir output;
    const QString path = output.filePath(QStringLiteral("test.png"));
    require(!ScreenshotMcpSession::validateOutputPath(QStringLiteral("relative.png"), nullptr),
            "relative save rejected");
    require(!ScreenshotMcpSession::validateOutputPath(QStringLiteral("https://example.com/a.png"),
                                                      nullptr),
            "URL save rejected");
    require(call(QStringLiteral("screenshot_save"), {{QStringLiteral("path"), path}}).ok &&
                QFile::exists(path),
            "explicit atomic save");
    require(call(QStringLiteral("screenshot_copy")).ok, "canonical clipboard publication");
    require(call(QStringLiteral("screenshot_pin")).ok, "pin completion");
    ScreenshotMcpRequest idempotent;
    idempotent.connectionId = 1;
    idempotent.sessionId = session.state().value(QStringLiteral("session_id")).toString();
    idempotent.method = QStringLiteral("screenshot_set_selection");
    idempotent.idempotencyKey = QStringLiteral("same-edit");
    idempotent.expectedRevision =
        static_cast<quint64>(session.state().value(QStringLiteral("revision")).toInteger());
    ScreenshotMcpResponse first, second;
    session.request(idempotent, [&](auto response) { first = response; });
    const int applied = mutations;
    session.request(idempotent, [&](auto response) { second = response; });
    require(first.ok && second.ok && first.revision == second.revision && mutations == applied,
            "duplicate mutation returns original result without replay");
    idempotent.params.insert(QStringLiteral("different"), true);
    session.request(idempotent, [&](auto response) { second = response; });
    require(second.errorCode == QStringLiteral("idempotency_conflict"),
            "idempotency key cannot change payload");
    deferImage = true;
    call(QStringLiteral("screenshot_undo"));
    ScreenshotMcpRequest pending;
    pending.connectionId = 1;
    pending.requestId = QStringLiteral("deferred-render");
    pending.method = QStringLiteral("screenshot_render");
    pending.sessionId = session.state().value(QStringLiteral("session_id")).toString();
    pending.expectedRevision =
        static_cast<quint64>(session.state().value(QStringLiteral("revision")).toInteger());
    std::optional<ScreenshotMcpResponse> deferredResponse;
    session.request(pending, [&](auto response) { deferredResponse = response; });
    await([&] { return bool(deliverImage); });
    require(call(QStringLiteral("screenshot_state"))
                    .result.value(QStringLiteral("pending_operation"))
                    .toString() == pending.method,
            "state reports pending output");
    require(call(QStringLiteral("screenshot_cancel"),
                 {{QStringLiteral("request_id"), pending.requestId}})
                .ok,
            "cancel a pending output");
    require(deferredResponse && deferredResponse->errorCode == QStringLiteral("canceled") &&
                session.state().value(QStringLiteral("active")).toBool(),
            "request cancellation keeps the editor session");
    deliverImage(QImage(2, 2, QImage::Format_ARGB32));
    deliverImage = {};
    deferImage = false;
    require(call(QStringLiteral("screenshot_render")).ok,
            "render recovers after output cancellation");
    session.disconnected(1);
    require(canceled == 0 && !session.state().value(QStringLiteral("active")).toBool(),
            "visible disconnect preserves work");
    editor.insert(QStringLiteral("capture_phase"), QStringLiteral("idle"));
    require(call(QStringLiteral("screenshot_begin"),
                 {{QStringLiteral("presentation"), QStringLiteral("silent")}})
                .ok,
            "silent begin");
    session.disconnected(1);
    require(canceled == 1, "silent disconnect cancels capture");
    auto direct = call(QStringLiteral("screenshot_direct_capture"));
    require(direct.ok && !direct.attachment.isEmpty() &&
                !session.state().value(QStringLiteral("active")).toBool(),
            "direct output finishes session");
    session.shutdown();
}
void workflowOperations() {
    QJsonObject editor{{QStringLiteral("capture_phase"), QStringLiteral("idle")}};
    ScreenshotMcpSession::Ports ports;
    ports.state = [&] { return editor; };
    ports.begin = [&](const QJsonObject&, QString*) {
        editor.insert(QStringLiteral("capture_phase"), QStringLiteral("editing"));
        return true;
    };
    ports.cancel = [&] { editor.insert(QStringLiteral("capture_phase"), QStringLiteral("idle")); };
    ports.selection = [&](const QJsonObject&, QString*) { return true; };
    int commands = 0, canceled = 0, detached = 0;
    bool completeOnCancel = false;
    ScreenshotMcpSession::Ports::CommandCompletion deliver;
    ports.command = [&](const QString&, const QJsonObject&, auto completion) {
        ++commands;
        deliver = std::move(completion);
    };
    ports.cancelCommand = [&] {
        ++canceled;
        if (completeOnCancel && deliver) {
            auto callback = std::exchange(deliver, {});
            callback({}, QStringLiteral("canceled"));
        }
    };
    ports.detached = [&] { ++detached; };
    ScreenshotMcpSession session(std::move(ports));
    int counter = 0;
    const auto makeRequest = [&](const QString& method) {
        ScreenshotMcpRequest r;
        r.connectionId = 1;
        r.method = method;
        r.requestId = QString::number(++counter);
        r.idempotencyKey = r.requestId;
        r.sessionId = session.state().value(QStringLiteral("session_id")).toString();
        r.expectedRevision =
            static_cast<quint64>(session.state().value(QStringLiteral("revision")).toInteger());
        return r;
    };
    std::optional<ScreenshotMcpResponse> response;
    const auto call = [&](const ScreenshotMcpRequest& r) {
        response.reset();
        session.request(r, [&](auto value) { response = value; });
    };
    call(makeRequest(QStringLiteral("screenshot_begin")));
    session.capturePresented();
    require(response && response->ok, "workflow begin completes");
    auto start = makeRequest(QStringLiteral("screenshot_recognize"));
    start.params.insert(QStringLiteral("kind"), QStringLiteral("text"));
    call(start);
    require(response && response->ok && commands == 1, "recognition returns promptly");
    const auto operation = response->result.value(QStringLiteral("operation_id")).toString();
    require(!operation.isEmpty() &&
                response->result.value(QStringLiteral("status")) == QStringLiteral("running"),
            "running operation has ID");
    call(start);
    require(response && response->ok && commands == 1,
            "idempotent start never repeats provider work");
    auto expiredCancel = makeRequest(QStringLiteral("screenshot_cancel"));
    expiredCancel.params.insert(QStringLiteral("request_id"), start.requestId);
    call(expiredCancel);
    require(response && response->errorCode == QStringLiteral("request_not_found") && canceled == 0,
            "canceling a completed request cannot stop a running background operation");
    auto query = makeRequest(QStringLiteral("screenshot_operation"));
    query.params.insert(QStringLiteral("operation_id"), operation);
    query.expectedRevision.reset();
    query.idempotencyKey.clear();
    call(query);
    require(response && response->ok, "result query needs no revision or mutation key");
    auto intruder = query;
    intruder.connectionId = 2;
    call(intruder);
    require(response && response->errorCode == QStringLiteral("session_not_found"),
            "results remain private to owner");
    call(makeRequest(QStringLiteral("screenshot_set_tool_style")));
    require(response && response->errorCode == QStringLiteral("busy"),
            "conflicting edit rejected while recognition runs");
    deliver({{QStringLiteral("text"), QStringLiteral("recognized")}}, {});
    call(query);
    require(response &&
                response->result.value(QStringLiteral("status")) == QStringLiteral("completed") &&
                response->result.value(QStringLiteral("result"))
                        .toObject()
                        .value(QStringLiteral("text")) == QStringLiteral("recognized"),
            "typed operation result retained");
    auto translationRequest = makeRequest(QStringLiteral("screenshot_translate"));
    call(translationRequest);
    const auto second = response->result.value(QStringLiteral("operation_id")).toString();
    auto late = deliver;
    auto cancel = makeRequest(QStringLiteral("screenshot_cancel"));
    cancel.params.insert(QStringLiteral("operation_id"), second);
    call(cancel);
    require(response && response->ok && canceled > 0, "operation cancellation reaches provider");
    late({{QStringLiteral("text"), QStringLiteral("late")}}, {});
    query.params.insert(QStringLiteral("operation_id"), second);
    call(query);
    require(response &&
                response->result.value(QStringLiteral("status")) == QStringLiteral("canceled"),
            "late completion cannot resurrect canceled operation");
    auto step = makeRequest(QStringLiteral("screenshot_scroll_once"));
    step.params.insert(QStringLiteral("direction"), QStringLiteral("down"));
    call(step);
    require(!response, "scroll waits for controller completion");
    const int before = commands;
    deliver({{QStringLiteral("changed"), true}}, {});
    require(response && response->ok, "scroll completes after capture barrier");
    call(step);
    require(response && response->ok && commands == before,
            "scroll replay cannot dispatch another notch");
    step = makeRequest(QStringLiteral("screenshot_scroll_once"));
    step.params.insert(QStringLiteral("direction"), QStringLiteral("down"));
    std::optional<ScreenshotMcpResponse> stepResponse;
    int stepCompletions = 0;
    session.request(step, [&](auto value) {
        ++stepCompletions;
        stepResponse = std::move(value);
    });
    auto lateStep = deliver;
    const int cancellationsBefore = canceled;
    call(expiredCancel);
    require(response && response->errorCode == QStringLiteral("request_not_found") &&
                canceled == cancellationsBefore && !stepResponse,
            "a mismatched cancellation leaves the pending step running");
    cancel = makeRequest(QStringLiteral("screenshot_cancel"));
    cancel.params.insert(QStringLiteral("request_id"), step.requestId);
    completeOnCancel = true;
    call(cancel);
    completeOnCancel = false;
    require(response && response->ok && stepResponse &&
                stepResponse->errorCode == QStringLiteral("canceled") && stepCompletions == 1 &&
                session.state().value(QStringLiteral("active")).toBool(),
            "synchronous controller cancellation completes once and preserves the session");
    lateStep({{QStringLiteral("changed"), true}}, {});
    require(stepCompletions == 1, "late step completion cannot overwrite cancellation");
    session.disconnected(1);
    require(detached == 1 &&
                editor.value(QStringLiteral("capture_phase")) == QStringLiteral("editing"),
            "disconnect stops automation while preserving visible work");
}
void annotationRuntime() {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    canvas.resize(300, 200);
    canvas.show();
    QCoreApplication::processEvents();
    int changes = 0;
    runtime.setDocumentChangedHandler([&] { ++changes; });
    const auto result = runtime.applyAnnotationTransaction(
        R"({"version":1,"operations":[{"type":"rectangle","bounds":[10,10,50,60]}]})");
    require(!result.isEmpty() && changes == 1 && canvas.canvasHistoryState().canUndo,
            "typed transaction refreshes registered visible viewport");
    require(runtime.undo() && canvas.canvasHistoryState().canRedo && changes == 2,
            "undo refreshes viewport history");
    require(runtime.redo() && canvas.canvasHistoryState().canUndo && changes == 3,
            "redo refreshes viewport history");
    const auto revision = runtime.documentRevision();
    require(runtime.applyAnnotationTransaction(
                       R"({"version":1,"operations":[{"type":"rectangle","bounds":[0,0,-5,8]}]})")
                    .isEmpty() &&
                runtime.documentRevision() == revision,
            "invalid typed input is atomic through Qt runtime");
}
} // namespace
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    workflowOperations();
    annotationRuntime();
    transport();
    descriptorOverride();
    selection();
    session();
    return 0;
}
