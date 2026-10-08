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
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void mouse(SnowCanvasWidget& canvas, QEvent::Type type, QPointF point,
           Qt::MouseButton button = Qt::NoButton, Qt::MouseButtons buttons = Qt::NoButton) {
    QMouseEvent event(type, point, point, point, button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&canvas, &event);
}

void click(SnowCanvasWidget& canvas, QPointF point) {
    mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton);
}

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1,
                           static_cast<std::size_t>(std::ceil(values.size() * fraction) - 1))];
}

void populate(SnowCanvasRuntime& runtime, int count, bool offscreen) {
    for (int base = 0; base < count; base += 200) {
        QJsonArray operations;
        for (int i = base; i < std::min(count, base + 200); ++i) {
            const double x = offscreen ? 10000.0 + i : -500.0 + (i % 10) * 100.0;
            const double y = offscreen ? 10000.0 : -300.0 + ((i / 10) % 20) * 30.0;
            operations.append(
                QJsonObject{{"type", "angle"},
                            {"points", QJsonArray{QJsonArray{x + 80, y}, QJsonArray{x, y},
                                                  QJsonArray{x, y - 40}}}});
        }
        const auto result =
            QJsonDocument::fromJson(
                runtime.applyAnnotationTransaction(
                    QJsonDocument(QJsonObject{{"version", 1}, {"operations", operations}})
                        .toJson()))
                .object();
        require(!result.contains("error") && result.contains("created_element_ids"),
                "populate angles");
        require(result.value(QStringLiteral("created_element_ids")).toArray().size() ==
                    operations.size(),
                "each population operation must create one angle owner");
    }
}

enum class Scenario { Preview, SameLabel, Wheel, Endpoint, StaticPaint };

QJsonObject selectedAngle(const SnowCanvasRuntime& runtime) {
    const auto payload = runtime.serializeSelectedDrawTemplate();
    require(!payload.isEmpty(), "benchmark selection must serialize");
    const auto drawTemplate = QJsonDocument::fromJson(payload).object();
    QJsonObject angle;
    int count = 0;
    for (const auto& element : drawTemplate.value(QStringLiteral("elements")).toArray()) {
        const auto arrow = element.toObject()
                               .value(QStringLiteral("data"))
                               .toObject()
                               .value(QStringLiteral("Arrow"))
                               .toObject();
        if (arrow.value(QStringLiteral("angle")).isObject()) {
            angle = arrow;
            ++count;
        }
    }
    require(count == 1, "benchmark selection must contain one committed angle");
    return angle;
}

void run(const char* name, Scenario scenario, int count, bool offscreen, int width, int iterations,
         int repeat) {
    SnowCanvasRuntime runtime;
    populate(runtime, count, offscreen);
    require(runtime.setQuickSelectionDisabledTools({SnowCanvasTool::Angle}),
            "benchmark angle clicks must create drafts over the visible population");
    SnowCanvasWidget canvas(runtime);
    canvas.resize(1280, 720);
    canvas.show();
    QApplication::processEvents();
    require(canvas.setViewportCamera(0, 0, 1), "benchmark uses a fixed canvas camera");
    require(canvas.setCanvasTool(SnowCanvasTool::Angle), "activate angle");
    auto style = canvas.canvasAngleStyle();
    style.strokeWidth = width;
    require(canvas.setCanvasAngleStyle(style), "configure angle width");
    const QPointF a(840, 360), vertex(640, 360), b(640, 220);
    if (scenario != Scenario::StaticPaint) {
        click(canvas, a);
        click(canvas, vertex);
        mouse(canvas, QEvent::MouseMove, b);
        if (scenario == Scenario::Wheel || scenario == Scenario::Endpoint) {
            click(canvas, b);
            require(canvas.setCanvasTool(SnowCanvasTool::Select), "select angle");
            click(canvas, {790, 360});
            if (scenario == Scenario::Endpoint)
                mouse(canvas, QEvent::MouseButtonPress, b, Qt::LeftButton, Qt::LeftButton);
        }
    } else {
        require(canvas.setCanvasTool(SnowCanvasTool::Select), "measure committed angles");
    }
    QApplication::processEvents();
    QJsonArray originalPoints;
    if (scenario == Scenario::Wheel || scenario == Scenario::Endpoint) {
        require(runtime.selectedElementIds().size() == 1, "benchmark selects one angle");
        const auto original = selectedAngle(runtime);
        originalPoints = original.value(QStringLiteral("points")).toArray();
        require(original.value(QStringLiteral("x")).toDouble() == 200.0 &&
                    original.value(QStringLiteral("y")).toDouble() == 0.0 &&
                    originalPoints == QJsonArray{QJsonArray{0.0, 0.0}, QJsonArray{-200.0, 0.0},
                                                 QJsonArray{-200.0, -140.0}},
                "benchmark must select the newly created angle");
    }
    std::vector<double> inputTimes, paintTimes;
    for (int i = 0; i < iterations + 30; ++i) {
        QElapsedTimer timer;
        timer.start();
        if (scenario == Scenario::Wheel) {
            require(canvas.adjustAngleValue((i % 2 == 0 ? 1 : -1) * 0.017453292519943295),
                    "adjust selected angle");
        } else if (scenario != Scenario::StaticPaint) {
            const double theta = scenario == Scenario::SameLabel
                                     ? 1.5707963267948966 + (i % 2) * .001
                                     : 1.1 + (i % 80) * .01;
            const double length = scenario == Scenario::SameLabel ? 120.0 + (i % 30) : 140.0;
            const QPointF point =
                vertex + QPointF(length * std::cos(theta), -length * std::sin(theta));
            mouse(canvas, QEvent::MouseMove, point, Qt::NoButton,
                  scenario == Scenario::Endpoint ? Qt::LeftButton : Qt::NoButton);
        }
        const double inputMs = static_cast<double>(timer.nsecsElapsed()) / 1e6;
        if (scenario == Scenario::StaticPaint)
            canvas.repaint();
        else
            QApplication::processEvents();
        const double paintMs = static_cast<double>(timer.nsecsElapsed()) / 1e6 - inputMs;
        if (i >= 30) {
            inputTimes.push_back(inputMs);
            paintTimes.push_back(paintMs);
        }
        if (i == 0 && scenario == Scenario::Wheel) {
            const auto points = selectedAngle(runtime).value(QStringLiteral("points")).toArray();
            require(points.at(0) == originalPoints.at(0) && points.at(1) == originalPoints.at(1) &&
                        points.at(2) != originalPoints.at(2),
                    "wheel benchmark must rotate the newly created second arm");
        }
    }
    if (scenario == Scenario::Endpoint) {
        const double theta = 1.1 + ((iterations + 29) % 80) * .01;
        const auto point = vertex + QPointF(140.0 * std::cos(theta), -140.0 * std::sin(theta));
        mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton);
        const auto points = selectedAngle(runtime).value(QStringLiteral("points")).toArray();
        require(points.at(0) == originalPoints.at(0) && points.at(1) == originalPoints.at(1) &&
                    points.at(2) != originalPoints.at(2),
                "endpoint benchmark must edit the newly created second arm");
    }
    std::cout << name << ',' << width << ',' << count << ',' << repeat << ','
              << percentile(inputTimes, .5) << ',' << percentile(inputTimes, .95) << ','
              << percentile(paintTimes, .5) << ',' << percentile(paintTimes, .95) << std::endl;
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
#ifdef Q_OS_WIN
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
#endif
    QApplication application(argc, argv);
    const int iterations = argc > 1 ? std::max(20, std::atoi(argv[1])) : 300;
    const int repeats = argc > 2 ? std::max(1, std::atoi(argv[2])) : 3;
    const int maximumCount = argc > 3 ? std::max(0, std::atoi(argv[3])) : 10000;
    std::cout << "scenario,stroke_width,existing_annotations,repeat,input_p50_ms,input_p95_ms,"
                 "paint_p50_ms,paint_p95_ms\n";
    for (int repeat = 0; repeat < repeats; ++repeat) {
        for (int width : {2, 8, 24, 72})
            run("angle-preview", Scenario::Preview, 0, false, width, iterations, repeat);
        run("angle-same-label", Scenario::SameLabel, 0, false, 2, iterations, repeat);
        run("angle-wheel", Scenario::Wheel, 0, false, 2, iterations, repeat);
        run("angle-endpoint", Scenario::Endpoint, 0, false, 2, iterations, repeat);
        for (int count : {100, 1000, 10000}) {
            if (count > maximumCount)
                continue;
            run("angle-offscreen", Scenario::Preview, count, true, 2, iterations, repeat);
            run("angle-offscreen-same-label", Scenario::SameLabel, count, true, 2, iterations,
                repeat);
            run("angle-offscreen-wheel", Scenario::Wheel, count, true, 2, iterations, repeat);
            run("angle-offscreen-endpoint", Scenario::Endpoint, count, true, 2, iterations, repeat);
            run("angle-visible-preview", Scenario::Preview, count, false, 2, iterations, repeat);
            run("angle-visible-same-label", Scenario::SameLabel, count, false, 2, iterations,
                repeat);
            run("angle-visible-wheel", Scenario::Wheel, count, false, 2, iterations, repeat);
            run("angle-visible-endpoint", Scenario::Endpoint, count, false, 2, iterations, repeat);
            run("angle-visible-paint", Scenario::StaticPaint, count, false, 2, iterations, repeat);
        }
    }
    return EXIT_SUCCESS;
}
