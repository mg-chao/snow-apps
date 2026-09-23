#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintEvent>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <vector>

namespace {
class PaintProbe final : public QObject {
  public:
    void reset() {
        m_region = {};
    }
    QRegion region() const {
        return m_region;
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        Q_UNUSED(watched);
        if (event != nullptr && event->type() == QEvent::Paint) {
            m_region += static_cast<QPaintEvent*>(event)->region();
        }
        return false;
    }

  private:
    QRegion m_region;
};

double percentile(std::vector<double> values, double fraction) {
    std::sort(values.begin(), values.end());
    const std::size_t index = std::min(
        values.size() - 1, static_cast<std::size_t>(std::ceil(fraction * values.size())) - 1);
    return values[index];
}

double paintedRatio(const QRegion& region, const QSize& size) {
    qint64 pixels = 0;
    for (const QRect& rect : region) {
        pixels += static_cast<qint64>(rect.width()) * rect.height();
    }
    return static_cast<double>(pixels) / size.width() / size.height();
}

QJsonObject measure(const QString& name, SnowCanvasWidget& canvas, PaintProbe& probe, int warmup,
                    int iterations, const std::function<void(int)>& update) {
    for (int index = 0; index < warmup; ++index) {
        update(index);
        QApplication::processEvents();
    }

    std::vector<double> milliseconds;
    milliseconds.reserve(static_cast<std::size_t>(iterations));
    double paintRatioSum = 0.0;
    for (int index = 0; index < iterations; ++index) {
        probe.reset();
        QElapsedTimer timer;
        timer.start();
        update(index + warmup);
        QApplication::processEvents();
        milliseconds.push_back(timer.nsecsElapsed() / 1'000'000.0);
        paintRatioSum += paintedRatio(probe.region(), canvas.size());
    }

    QJsonObject result;
    result.insert(QStringLiteral("name"), name);
    result.insert(QStringLiteral("sampleCount"), iterations);
    result.insert(QStringLiteral("p50Ms"), percentile(milliseconds, 0.50));
    result.insert(QStringLiteral("p95Ms"), percentile(milliseconds, 0.95));
    result.insert(QStringLiteral("meanPaintRegionRatio"), paintRatioSum / iterations);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("summary"), QStringLiteral("JSON summary output path"),
                      QStringLiteral("path"), QStringLiteral("decoration-area-benchmark.json")});
    parser.addOption({QStringLiteral("iterations"), QStringLiteral("Measured samples"),
                      QStringLiteral("count"), QStringLiteral("240")});
    parser.addOption({QStringLiteral("warmup"), QStringLiteral("Warmup samples"),
                      QStringLiteral("count"), QStringLiteral("40")});
    parser.process(application);

    bool iterationsValid = false;
    bool warmupValid = false;
    const int iterations = parser.value(QStringLiteral("iterations")).toInt(&iterationsValid);
    const int warmup = parser.value(QStringLiteral("warmup")).toInt(&warmupValid);
    if (!iterationsValid || !warmupValid || iterations <= 0 || warmup < 0) {
        return 2;
    }

    const QSize surfaceSize(3840, 2160);
    const QRectF selection(-960.0, -540.0, 1920.0, 1080.0);
    QWidget window;
    window.setAttribute(Qt::WA_NativeWindow, true);
    window.resize(surfaceSize);
    auto* layout = new QVBoxLayout(&window);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* canvas = new SnowCanvasWidget(&window);
    layout->addWidget(canvas);
    static_cast<void>(canvas->setViewportCamera(0.0, 0.0, 1.0));
    PaintProbe probe;
    canvas->installEventFilter(&probe);
    window.show();
    QApplication::processEvents();

    QJsonArray scenarios;
    scenarios.append(measure(
        QStringLiteral("inactive-decoration-area-update"), *canvas, probe, warmup, iterations,
        [&](int index) {
            for (int updateIndex = 0; updateIndex < 1024; ++updateIndex) {
                const QRectF area =
                    selection.translated(static_cast<qreal>((index + updateIndex) & 1), 0.0);
                canvas->setDecorationRenderAreas(
                    {std::optional<QRectF>(area), std::optional<QRectF>(area)});
            }
        }));

    SnowCanvasWatermarkConfig watermark;
    watermark.text = QStringLiteral("SNOW SHOT");
    watermark.color = Qt::white;
    watermark.fontSize = 28.0;
    watermark.opacity = 1.0;
    static_cast<void>(canvas->setCanvasWatermarkConfig(watermark));
    const SnowCanvasDecorationRenderAreas fixedArea{std::optional<QRectF>(selection), std::nullopt};
    canvas->setDecorationRenderAreas(fixedArea);
    QApplication::processEvents();
    scenarios.append(measure(QStringLiteral("unchanged-active-watermark-area"), *canvas, probe,
                             warmup, iterations,
                             [&](int) { canvas->setDecorationRenderAreas(fixedArea); }));
    scenarios.append(measure(QStringLiteral("moving-active-watermark-area"), *canvas, probe, warmup,
                             iterations, [&](int index) {
                                 canvas->setDecorationRenderAreas(
                                     {std::optional<QRectF>(selection.translated(index & 63, 0.0)),
                                      std::nullopt});
                             }));

    QJsonObject summary;
    summary.insert(QStringLiteral("surfaceWidth"), surfaceSize.width());
    summary.insert(QStringLiteral("surfaceHeight"), surfaceSize.height());
    summary.insert(QStringLiteral("scenarios"), scenarios);
    const QByteArray json = QJsonDocument(summary).toJson(QJsonDocument::Indented);
    QFile file(parser.value(QStringLiteral("summary")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(json) != json.size()) {
        return 3;
    }
    QTextStream(stdout) << json;
    return 0;
}
