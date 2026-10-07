#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <numeric>
#include <vector>

namespace {
void require(bool ok, const char* message) {
    if (!ok) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1,
                           static_cast<std::size_t>(std::ceil(values.size() * fraction) - 1))];
}
double mean(const std::vector<double>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / values.size();
}
void addDistances(SnowCanvasRuntime& runtime, int count, bool offscreen, bool bare = false) {
    for (int base = 0; base < count; base += 200) {
        QJsonArray operations;
        for (int i = base; i < std::min(count, base + 200); ++i) {
            const double x = offscreen ? 10000.0 + i : -500.0 + (i % 10) * 100.0;
            const double y = offscreen ? 10000.0 : -300.0 + ((i / 10) % 20) * 30.0;
            operations.append(
                QJsonObject{{"type", bare ? "arrow" : "distance"},
                            {"points", QJsonArray{QJsonArray{x, y}, QJsonArray{x + 80.0, y}}}});
        }
        const auto result =
            QJsonDocument::fromJson(
                runtime.applyAnnotationTransaction(
                    QJsonDocument(QJsonObject{{"version", 1}, {"operations", operations}})
                        .toJson()))
                .object();
        require(!result.contains("error") && result.contains("created_element_ids"),
                "add distances");
    }
}
void interaction(const char* name, int width, int count, bool offscreen, bool sameLabel,
                 bool endpoint, bool arrowTool, int iterations, int repeat) {
    SnowCanvasRuntime runtime;
    addDistances(runtime, count, offscreen, arrowTool);
    SnowCanvasWidget canvas(runtime);
    canvas.resize(1280, 720);
    canvas.show();
    QApplication::processEvents();
    auto style = canvas.canvasDistanceStyle();
    style.strokeWidth = width;
    require(canvas.setCanvasDistanceStyle(style), "distance style");
    require(canvas.setCanvasTool(arrowTool ? SnowCanvasTool::Arrow : SnowCanvasTool::Distance),
            "tool");
    const QPointF start(300, 360), end(600, 360);
    if (endpoint) {
        mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseMove, end, Qt::NoButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        require(canvas.setCanvasTool(SnowCanvasTool::Select), "select tool");
        mouse(canvas, QEvent::MouseButtonPress, end, Qt::LeftButton, Qt::LeftButton);
        mouse(canvas, QEvent::MouseButtonRelease, end, Qt::LeftButton, Qt::NoButton);
        mouse(canvas, QEvent::MouseButtonPress, end, Qt::LeftButton, Qt::LeftButton);
    } else {
        mouse(canvas, QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton);
    }
    std::vector<double> input, paint;
    QPointF point;
    for (int i = 0; i < iterations + 30; ++i) {
        point = sameLabel ? start + QPointF(300 * std::cos(i * .001), 300 * std::sin(i * .001))
                          : start + QPointF(310 + i, i % 37);
        QElapsedTimer timer;
        timer.start();
        mouse(canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
        const double inputMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        QApplication::processEvents();
        const double totalMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        if (i >= 30) {
            input.push_back(inputMs);
            paint.push_back(totalMs - inputMs);
        }
    }
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    const auto document = QJsonDocument::fromJson(runtime.serializeDocumentSession())
                              .object()
                              .value(QStringLiteral("document"))
                              .toObject();
    int arrows = 0;
    for (const auto& slot : document.value(QStringLiteral("slots")).toArray()) {
        const auto data = slot.toObject().value(QStringLiteral("data")).toObject();
        if (data.contains(QStringLiteral("Arrow")))
            ++arrows;
    }
    require(arrows == count + 1, "interaction must commit the distance or arrow");
    std::cout << name << ',' << width << ',' << count << ',' << repeat << ','
              << percentile(input, .5) << ',' << percentile(input, .95) << ','
              << percentile(paint, .5) << ',' << percentile(paint, .95) << ',' << mean(input) << ','
              << mean(paint) << '\n';
}
void staticPaint(int count, int iterations, int repeat) {
    SnowCanvasRuntime runtime;
    addDistances(runtime, count, false);
    SnowCanvasWidget canvas(runtime);
    canvas.resize(1280, 720);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setCanvasTool(SnowCanvasTool::Distance), "measure committed labels");
    require(canvas.setCanvasTool(SnowCanvasTool::Select), "leave no creation preview");
    std::vector<double> paint;
    for (int i = 0; i < iterations + 30; ++i) {
        QElapsedTimer timer;
        timer.start();
        canvas.repaint();
        if (i >= 30)
            paint.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
    }
    std::cout << "static-distance,2," << count << ',' << repeat << ",0,0," << percentile(paint, .5)
              << ',' << percentile(paint, .95) << ",0," << mean(paint) << '\n';
}
void calibration(int count, int iterations, int repeat) {
    SnowCanvasRuntime runtime;
    addDistances(runtime, count, true);
    SnowCanvasWidget canvas(runtime);
    canvas.resize(1280, 720);
    require(canvas.setCanvasTool(SnowCanvasTool::Distance), "measure calibration labels");
    std::vector<double> input;
    for (int i = 0; i < iterations + 30; ++i) {
        QElapsedTimer timer;
        timer.start();
        require(canvas.setDistanceCreationPixelScale(QSizeF(2 + i % 2, 2 + i % 2)),
                "update image calibration");
        if (i >= 30)
            input.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
    }
    std::cout << "distance-calibration,2," << count << ',' << repeat << ',' << percentile(input, .5)
              << ',' << percentile(input, .95) << ",0,0," << mean(input) << ",0\n";
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
#endif
    QApplication app(argc, argv);
    const int iterations = argc > 1 ? std::max(20, std::atoi(argv[1])) : 300;
    const int repeats = argc > 2 ? std::max(1, std::atoi(argv[2])) : 3;
    std::cout << "scenario,stroke_width,existing_annotations,repeat,input_p50_ms,input_p95_ms,"
                 "paint_p50_ms,paint_p95_ms,input_mean_ms,paint_mean_ms\n";
    for (int repeat = 0; repeat < repeats; ++repeat) {
        interaction("arrow-create", 2, 0, false, false, false, true, iterations, repeat);
        for (int width : {2, 8, 24, 72})
            interaction("distance-create", width, 0, false, false, false, false, iterations,
                        repeat);
        interaction("distance-same-label", 2, 0, false, true, false, false, iterations, repeat);
        interaction("distance-endpoint", 2, 0, false, false, true, false, iterations, repeat);
        for (int count : {10, 100, 300}) {
            interaction("arrow-offscreen", 2, count, true, false, false, true, iterations, repeat);
            interaction("distance-offscreen", 2, count, true, false, false, false, iterations,
                        repeat);
        }
        for (int count : {1, 10, 100, 300})
            staticPaint(count, iterations, repeat);
        for (int count : {0, 100, 300})
            calibration(count, iterations, repeat);
    }
}
