// Identical synthetic model/renderer ports are linked against baseline or current
// MCP Qt transport/session sources. Native capture is deliberately outside scope.
#include "snow_shot/app/mcp/screenshotmcpserver.h"
#include "snow_shot/app/mcp/screenshotmcpsession.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTimer>
#include <iostream>
#include <optional>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

using namespace snow_shot::app::mcp;

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    const auto arguments = application.arguments();
    const int serve = arguments.indexOf(QStringLiteral("--serve"));
    if (serve < 0 || serve + 1 >= arguments.size())
        return 2;
    const QString directory = arguments.at(serve + 1);
    if (!QDir(directory).exists())
        return 2;
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    const int sizeIndex = arguments.indexOf(QStringLiteral("--size"));
    const auto sizeParts = sizeIndex >= 0 && sizeIndex + 1 < arguments.size()
                               ? arguments.at(sizeIndex + 1).split(QLatin1Char('x'))
                               : QStringList{QStringLiteral("800"), QStringLiteral("600")};
    if (sizeParts.size() != 2)
        return 2;
    const int width = sizeParts[0].toInt(), height = sizeParts[1].toInt();
    if (width < 320 || height < 200 || width > 3840 || height > 2160)
        return 2;
    QImage background(width, height, QImage::Format_ARGB32_Premultiplied);
    quint32 noise = 20260926;
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<QRgb*>(background.scanLine(y));
        for (int x = 0; x < width; ++x) {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            row[x] = qRgb(noise & 255, (noise >> 8) & 255, (noise >> 16) & 255);
        }
    }
    QRectF selection(0, 0, background.width(), background.height());
    QString phase = QStringLiteral("idle");
    QString tool = QStringLiteral("select");
    QString recognizedText;
    bool scrolling = false;
    int scrollSteps = 0;
    quint64 commandGeneration = 0;
    ScreenshotMcpSession* activeSession = nullptr;
    ScreenshotMcpSession::Ports ports;
    ports.state = [&] {
        return QJsonObject{
            {QStringLiteral("capture_phase"), phase},
            {QStringLiteral("canvas_bounds"), QJsonArray{0, 0, width, height}},
            {QStringLiteral("selection"),
             QJsonObject{
                 {QStringLiteral("bounds"),
                  QJsonArray{selection.x(), selection.y(), selection.width(), selection.height()}},
                 {QStringLiteral("type"), QStringLiteral("rectangle")}}},
            {QStringLiteral("active_tool"), tool},
            {QStringLiteral("document_revision"), static_cast<qint64>(runtime.documentRevision())},
            {QStringLiteral("can_undo"), runtime.canUndo()},
            {QStringLiteral("can_redo"), runtime.canRedo()}};
    };
    ports.begin = [&](const QJsonObject&, QString*) {
        static_cast<void>(runtime.clearDocumentPreservingViewports());
        selection = QRectF(0, 0, width, height);
        recognizedText.clear();
        scrolling = false;
        scrollSteps = 0;
        phase = QStringLiteral("capturing");
        QTimer::singleShot(0, &application, [&] {
            phase = QStringLiteral("editing");
            activeSession->capturePresented();
        });
        return true;
    };
    ports.cancel = [&] { phase = QStringLiteral("idle"); };
    ports.selection = [&](const QJsonObject& input, QString*) {
        const auto bounds = input.value(QStringLiteral("bounds")).toArray();
        if (bounds.size() != 4)
            return false;
        const QRectF candidate(bounds[0].toDouble(), bounds[1].toDouble(), bounds[2].toDouble(),
                               bounds[3].toDouble());
        if (!candidate.isValid() || !QRectF(0, 0, width, height).contains(candidate))
            return false;
        selection = candidate;
        return true;
    };
    ports.tool = [&](const QString& name, QString*) {
        tool = name;
        return canvas.setCanvasTool(name == QStringLiteral("rectangle") ? SnowCanvasTool::Shape
                                                                        : SnowCanvasTool::Select);
    };
    ports.annotations = [&](const QByteArray& payload, QJsonObject* result, QString*) {
        *result = QJsonDocument::fromJson(runtime.applyAnnotationTransaction(payload)).object();
        return !result->isEmpty();
    };
    ports.history = [&](bool redo) {
        if (redo)
            static_cast<void>(runtime.redo());
        else
            static_cast<void>(runtime.undo());
    };
    // Deterministic provider ports verify session orchestration, not native OCR,
    // translation or scrolling. Engine edits/templates still use the real runtime.
    ports.command = [&](const QString& method, const QJsonObject& input, auto done) {
        SnowCanvasRuntimeEditor editor(runtime);
        const auto action = input.value(QStringLiteral("action")).toString();
        QJsonObject result;
        if (method == QStringLiteral("screenshot_recognize") ||
            method == QStringLiteral("screenshot_translate") ||
            method == QStringLiteral("screenshot_auto_filter")) {
            const auto generation = ++commandGeneration;
            QTimer::singleShot(20, &application, [&, generation, method, done = std::move(done)] {
                if (generation != commandGeneration)
                    return;
                recognizedText = method == QStringLiteral("screenshot_translate")
                                     ? QStringLiteral("Fixture translated text")
                                     : QStringLiteral("Fixture recognized text");
                done({{QStringLiteral("text"), recognizedText},
                      {QStringLiteral("fixture_provider"), true}},
                     {});
            });
            return;
        }
        if (method == QStringLiteral("screenshot_scrolling")) {
            if (action == QStringLiteral("start"))
                scrolling = true;
            else if (action == QStringLiteral("stop"))
                scrolling = false;
            else if (!scrolling) {
                done({}, QStringLiteral("action_unavailable"));
                return;
            }
            result = {{QStringLiteral("scrolling"), scrolling}};
        } else if (method == QStringLiteral("screenshot_scroll_once")) {
            if (!scrolling) {
                done({}, QStringLiteral("action_unavailable"));
                return;
            }
            result = {{QStringLiteral("scroll_steps"), ++scrollSteps}};
        } else if (method == QStringLiteral("screenshot_edit_recognition") ||
                   method == QStringLiteral("screenshot_export_recognition")) {
            if (recognizedText.isEmpty()) {
                done({}, QStringLiteral("recognition_required"));
                return;
            }
            if (action == QStringLiteral("set_text"))
                recognizedText = input.value(QStringLiteral("text")).toString();
            result = {{QStringLiteral("text"), recognizedText}};
        } else if (method == QStringLiteral("screenshot_draw_template")) {
            if (action == QStringLiteral("export"))
                result = {{QStringLiteral("payload"),
                           QString::fromUtf8(runtime.serializeSelectedDrawTemplate())}};
            else if (!editor.insertDrawTemplate(
                         input.value(QStringLiteral("payload")).toString().toUtf8(),
                         selection.center())) {
                done({}, QStringLiteral("action_unavailable"));
                return;
            }
        } else if (method == QStringLiteral("screenshot_edit_elements")) {
            bool ok = false;
            if (action == QStringLiteral("select")) {
                const auto transaction =
                    QJsonObject{{QStringLiteral("version"), 1},
                                {QStringLiteral("operations"),
                                 QJsonArray{QJsonObject{
                                     {QStringLiteral("type"), QStringLiteral("select")},
                                     {QStringLiteral("id"), input.value(QStringLiteral("id"))}}}}};
                ok = !runtime
                          .applyAnnotationTransaction(
                              QJsonDocument(transaction).toJson(QJsonDocument::Compact))
                          .isEmpty();
            } else if (action == QStringLiteral("opacity"))
                ok = editor.setSelectedOpacity(input.value(QStringLiteral("opacity")).toDouble());
            if (!ok) {
                done({}, QStringLiteral("action_unavailable"));
                return;
            }
        } else if (method == QStringLiteral("screenshot_recapture")) {
            static_cast<void>(runtime.clearDocumentPreservingViewports());
            phase = QStringLiteral("editing");
        } else if (method == QStringLiteral("screenshot_set_selection_style") ||
                   method == QStringLiteral("screenshot_set_tool_style")) {
            result = {{QStringLiteral("fixture_style_acknowledged"), true}};
        } else {
            done({}, QStringLiteral("action_unavailable"));
            return;
        }
        done(result, {});
    };
    ports.cancelCommand = [&] { ++commandGeneration; };
    ports.artifact = [&](qreal scale) {
        ScreenshotPinnedViewportExportSource source{
            runtime.serializeDocumentSession(),
            background.copy(selection.toAlignedRect()),
            selection,
            QSize(qRound(selection.width() * scale), qRound(selection.height() * scale)),
            {},
            runtime.smartEraseSnapshot(),
            1,
            {}};
        return std::make_shared<ScreenshotExportArtifact>(
            ScreenshotExportSource::fromPinnedViewport(std::move(source)));
    };
    ports.copy = [](auto, auto done) {
        done(true);
        return true;
    };
    ports.pin = [](auto, auto done) {
        done(true);
        return true;
    };
    ports.direct = [&](const QJsonObject&, auto done) {
        done(background, {}, {});
        return true;
    };
    ScreenshotMcpSession session(std::move(ports));
    activeSession = &session;
    ScreenshotMcpServer server(nullptr, directory);
    server.setRequestHandler([&](const ScreenshotMcpRequest& request, auto done) {
        session.request(request, std::move(done));
    });
    server.setClientDisconnectedHandler([&](quint64 owner) { session.disconnected(owner); });
    QString error;
    if (!server.start(&error)) {
        std::cerr << error.toStdString() << '\n';
        return 3;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    const auto threadCpuMilliseconds = []() -> std::optional<double> {
#ifdef Q_OS_WIN
        FILETIME created{}, exited{}, kernel{}, user{};
        if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
            return std::nullopt;
        const auto ticks = [](FILETIME value) {
            return (static_cast<quint64>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        };
        return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
#else
        return std::nullopt;
#endif
    };
    const auto initialCpu = threadCpuMilliseconds();
    qint64 last = 0;
    qint64 maximum = 0;
    qint64 count = 0;
    QTimer heartbeat;
    heartbeat.setInterval(10);
    QObject::connect(&heartbeat, &QTimer::timeout, &application, [&] {
        const auto now = elapsed.elapsed();
        maximum = std::max(maximum, std::max<qint64>(0, now - last - 10));
        last = now;
        ++count;
        if (count % 100 == 0) {
            QSaveFile file(QDir(directory).filePath(QStringLiteral("metrics.json")));
            if (file.open(QIODevice::WriteOnly)) {
                const auto currentCpu = threadCpuMilliseconds();
                const auto cpu = initialCpu && currentCpu
                                     ? std::optional<double>(*currentCpu - *initialCpu)
                                     : std::nullopt;
                file.write(
                    QJsonDocument(
                        QJsonObject{
                            {QStringLiteral("ticks"), count},
                            {QStringLiteral("max_lateness_ms"), maximum},
                            {QStringLiteral("wall_ms"), now},
                            {QStringLiteral("cpu_ms"), cpu ? QJsonValue(*cpu) : QJsonValue{}},
                            {QStringLiteral("cpu_utilization"),
                             cpu && now > 0 ? QJsonValue(*cpu / static_cast<double>(now))
                                            : QJsonValue{}}})
                        .toJson(QJsonDocument::Compact));
                file.commit();
            }
        }
    });
    heartbeat.start();
    QTimer::singleShot(180000, &application, &QCoreApplication::quit);
    const int result = application.exec();
    session.shutdown();
    server.stop();
    return result;
}
