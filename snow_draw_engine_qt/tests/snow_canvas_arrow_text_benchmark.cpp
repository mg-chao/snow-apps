#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QLineF>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

QJsonObject arrow(const SnowCanvasRuntime& runtime) {
    const auto document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                              .object()
                              .value(QStringLiteral("document"))
                              .toObject();
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto data = slot.toObject().value(QStringLiteral("data")).toObject();
        if (data.contains(QStringLiteral("Arrow"))) {
            return data.value(QStringLiteral("Arrow")).toObject();
        }
    }
    require(false, "missing benchmark arrow");
    return {};
}

struct Scenario {
    const char* name;
    int length;
    bool bound = true;
    bool curve = false;
    bool movedLabel = false;
    bool wrapped = false;
    bool vertical = false;
};

void prepare(SnowCanvasRuntime& runtime, SnowCanvasWidget& canvas, const Scenario& scenario) {
    canvas.resize(2800, 1000);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Arrow), "activate arrow tool");
    mouse(canvas, QEvent::MouseButtonPress, {80, 250}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, {500, 250}, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {500, 250}, Qt::LeftButton, Qt::NoButton);
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "activate selection tool");
    if (scenario.bound) {
        mouse(canvas, QEvent::MouseButtonDblClick, {290, 250}, Qt::LeftButton, Qt::LeftButton);
        require(canvas.hasActiveTextEditing(), "open benchmark label");
        const QString text =
            scenario.wrapped
                ? QStringLiteral("A label that must wrap as its arrow changes width. ").repeated(20)
                : QStringLiteral("Request");
        QKeyEvent input(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, text);
        QApplication::sendEvent(&canvas, &input);
        QKeyEvent commit(QEvent::KeyPress, Qt::Key_Return, Qt::ControlModifier);
        QApplication::sendEvent(&canvas, &commit);
        require(!canvas.hasActiveTextEditing(), "commit benchmark label");
    }

    auto session = QJsonDocument::fromJson(runtime.serializeDocumentSession()).object();
    auto document = session.value(QStringLiteral("document")).toObject();
    auto records = document.value(QStringLiteral("slots")).toArray();
    for (qsizetype i = 0; i < records.size(); ++i) {
        auto slot = records[i].toObject();
        auto data = slot.value(QStringLiteral("data")).toObject();
        if (!data.contains(QStringLiteral("Arrow"))) {
            continue;
        }
        auto value = data.value(QStringLiteral("Arrow")).toObject();
        const double length = scenario.length;
        value[QStringLiteral("width")] = scenario.vertical ? 0.0 : length;
        value[QStringLiteral("height")] = scenario.vertical ? length : scenario.curve ? 300 : 0;
        if (scenario.curve) {
            value[QStringLiteral("arrow_type")] = QStringLiteral("curve");
            value[QStringLiteral("points")] =
                QJsonArray{QJsonArray{0, 0}, QJsonArray{length * 0.3, -120},
                           QJsonArray{length * 0.65, 180}, QJsonArray{length, 0}};
        } else {
            value[QStringLiteral("points")] =
                QJsonArray{QJsonArray{0, 0}, QJsonArray{scenario.vertical ? 0.0 : length,
                                                        scenario.vertical ? length : 0.0}};
        }
        if (scenario.movedLabel) {
            value[QStringLiteral("text_path_fraction")] = 0.37;
        }
        data[QStringLiteral("Arrow")] = value;
        slot[QStringLiteral("data")] = data;
        records[i] = slot;
    }
    document[QStringLiteral("slots")] = records;
    session[QStringLiteral("document")] = document;
    SnowCanvasRuntime empty;
    session[QStringLiteral("history")] = QJsonDocument::fromJson(empty.serializeDocumentSession())
                                             .object()
                                             .value(QStringLiteral("history"));
    require(runtime.restoreDocumentSession(QJsonDocument(session).toJson()), "restore fixture");
    // Clicking the start selects the arrow without hitting its label.
    mouse(canvas, QEvent::MouseButtonPress, {80, 250}, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, {80, 250}, Qt::LeftButton, Qt::NoButton);
    QApplication::processEvents();
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::ceil(values.size() * fraction) - 1);
    return values[std::min(index, values.size() - 1)];
}

void run(const Scenario& scenario, int iterations, int repeat) {
    SnowCanvasRuntime runtime;
    SnowCanvasWidget canvas(runtime);
    prepare(runtime, canvas, scenario);
    const auto before = arrow(runtime);
    // Vertical cases drag the start: their far endpoint can be outside the viewport.
    const QPointF handle(scenario.vertical ? 80 : 80 + scenario.length, 250);
    mouse(canvas, QEvent::MouseButtonPress, handle, Qt::LeftButton, Qt::LeftButton);
    std::vector<double> inputTimes, paintTimes, frameTimes;
    constexpr int warmup = 30;
    QPointF point;
    for (int i = 0; i < iterations + warmup; ++i) {
        point = handle + QPointF(scenario.vertical ? 0 : 10 + i % 41, 10 + i % 37);
        QElapsedTimer timer;
        timer.start();
        mouse(canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
        const double inputMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        QApplication::processEvents();
        const double frameMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        if (i >= warmup) {
            inputTimes.push_back(inputMs);
            paintTimes.push_back(frameMs - inputMs);
            frameTimes.push_back(frameMs);
        }
    }
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    const auto after = arrow(runtime);
    require(before.value(QStringLiteral("points")) != after.value(QStringLiteral("points")),
            "benchmark must reshape the arrow, not move the selection or hover");
    const auto points = after.value(QStringLiteral("points")).toArray();
    const auto fixed = points[scenario.vertical ? points.size() - 1 : 0].toArray();
    const QPointF fixedPosition(after.value(QStringLiteral("x")).toDouble() + fixed[0].toDouble(),
                                after.value(QStringLiteral("y")).toDouble() + fixed[1].toDouble());
    const auto oldPoints = before.value(QStringLiteral("points")).toArray();
    const auto oldFixed = oldPoints[scenario.vertical ? oldPoints.size() - 1 : 0].toArray();
    const QPointF oldFixedPosition(
        before.value(QStringLiteral("x")).toDouble() + oldFixed[0].toDouble(),
        before.value(QStringLiteral("y")).toDouble() + oldFixed[1].toDouble());
    require(QLineF(fixedPosition, oldFixedPosition).length() < 0.01,
            "opposite endpoint stays fixed");
    std::cout << scenario.name << ',' << scenario.length << ',' << repeat << ','
              << percentile(inputTimes, 0.5) << ',' << percentile(inputTimes, 0.95) << ','
              << percentile(paintTimes, 0.5) << ',' << percentile(paintTimes, 0.95) << ','
              << percentile(frameTimes, 0.5) << ',' << percentile(frameTimes, 0.95) << '\n';
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
#endif
    QApplication app(argc, argv);
    const int iterations = argc > 1 ? std::max(20, std::atoi(argv[1])) : 300;
    const int repeats = argc > 2 ? std::max(1, std::atoi(argv[2])) : 5;
    std::cout << "scenario,length,repeat,input_median_ms,input_p95_ms,paint_median_ms,paint_p95_ms,"
                 "frame_median_ms,frame_p95_ms\n";
    for (int repeat = 0; repeat < repeats; ++repeat) {
        for (int length : {200, 800, 2400}) {
            run({"unbound", length, false}, iterations, repeat);
            run({"straight", length}, iterations, repeat);
            run({"curve", length, true, true}, iterations, repeat);
            run({"moved-curve-label", length, true, true, true}, iterations, repeat);
            run({"wrapped", length, true, false, false, true}, iterations, repeat);
        }
        run({"vertical", 2400, true, false, false, false, true}, iterations, repeat);
    }
}
