#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_canvas_display_item.h"
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
#include <vector>
namespace {
void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}
void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF p) {
    QMouseEvent event(type, p, p, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                      type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}
double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(
        std::ceil(static_cast<double>(values.size()) * fraction) - 1)];
}
void populate(SnowCanvasRuntime& runtime, int count, bool visible) {
    for (int base = 0; base < count; base += 200) {
        QJsonArray operations;
        for (int i = base; i < std::min(count, base + 200); ++i) {
            const double x = visible ? -610.0 + (i % 50) * 16.0 : 10000.0 + i;
            const double y = visible ? -320.0 + ((i / 50) % 50) * 12.0 : 10000.0;
            operations.append(
                QJsonObject{{"type", "bracket"},
                            {"points", QJsonArray{QJsonArray{x, y}, QJsonArray{x + 10, y + 5},
                                                  QJsonArray{x, y + 10}}}});
        }
        const auto result =
            QJsonDocument::fromJson(
                runtime.applyAnnotationTransaction(
                    QJsonDocument(QJsonObject{{"version", 1}, {"operations", operations}})
                        .toJson()))
                .object();
        require(result.value("created_element_ids").toArray().size() == operations.size(),
                "populate brackets");
    }
}
void run(int count, bool visible, const char* scenario, int iterations) {
    SnowCanvasRuntime runtime;
    populate(runtime, count, visible);
    require(runtime.setAutomaticallySelectNewlyCreatedElements(true), "automatic selection");
    require(runtime.setQuickSelectionDisabledTools({SnowCanvasTool::Bracket}),
            "disable creation quick select");
    SnowCanvasWidget canvas(runtime);
    canvas.resize(1280, 720);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0, 0, 1) && canvas.setCanvasTool(SnowCanvasTool::Bracket),
            "activate bracket");
    const QPointF top(1000, 180), bottom(1120, 540), tip(1120, 360), guide(1000, 360);
    const bool creation = QString::fromLatin1(scenario) == QStringLiteral("creation");
    mouse(canvas, QEvent::MouseButtonPress, top);
    mouse(canvas, QEvent::MouseMove, bottom);
    QPointF start = bottom;
    if (!creation) {
        mouse(canvas, QEvent::MouseButtonRelease, bottom);
        require(runtime.selectedElementIds().size() == 1, "selected benchmark bracket");
        const QString name = QString::fromLatin1(scenario);
        start = name == "endpoint" ? top : (name == "apex" ? tip : guide);
        mouse(canvas, QEvent::MouseButtonPress, start);
    }
    std::vector<double> inputs, frames;
    SnowCanvasSceneItem::PathGeometryDiagnostics counters{};
    for (int i = 0; i < iterations + 20; ++i) {
        QElapsedTimer timer;
        timer.start();
        mouse(canvas, QEvent::MouseMove, start + QPointF((i % 31) * 0.7, (i % 23) * 0.5));
        const double input = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        canvas.repaint();
        const double frame = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        const auto d = SnowCanvasSceneItem::takePathGeometryDiagnostics();
        if (i >= 20) {
            inputs.push_back(input);
            frames.push_back(frame);
            counters.reads += d.reads;
            counters.builds += d.builds;
            counters.reuses += d.reuses;
        }
    }
    QElapsedTimer commitTimer;
    commitTimer.start();
    mouse(canvas, QEvent::MouseButtonRelease,
          start + QPointF(((iterations + 19) % 31) * 0.7, ((iterations + 19) % 23) * 0.5));
    const double commitMs = static_cast<double>(commitTimer.nsecsElapsed()) / 1e6;
    require(counters.builds <= static_cast<std::uint64_t>(iterations) * 4,
            "editing must not rebuild population paths");
    std::cout << scenario << ',' << count << ',' << (visible ? "visible" : "offscreen") << ','
              << percentile(inputs, .5) << ',' << percentile(inputs, .95) << ','
              << percentile(frames, .5) << ',' << percentile(frames, .95) << ',' << counters.reads
              << ',' << counters.builds << ',' << counters.reuses << ',' << commitMs << std::endl;
}
} // namespace
int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    const int iterations = argc > 1 ? std::max(20, std::atoi(argv[1])) : 120;
    std::cout << "scenario,existing_annotations,population,input_p50_ms,input_p95_ms,frame_p50_ms,"
                 "frame_p95_ms,path_reads,path_builds,path_reuses,commit_ms\n";
    for (int count : {0, 100, 1000, 10000})
        for (bool visible : {false, true}) {
            if (count == 0 && visible)
                continue;
            for (const char* scenario : {"creation", "endpoint", "apex", "guide"})
                run(count, visible, scenario, iterations);
        }
    return 0;
}
