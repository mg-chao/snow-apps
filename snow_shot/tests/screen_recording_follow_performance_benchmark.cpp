#include "screen_recording_follow_performance_benchmark.h"
#include "snow_shot/presentation/screenrecordingareawindow.h"
#include "presentation/recording/screenrecordingperfinstrumentation.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintEvent>
#include <QScreen>
#include <cstring>
#include <iostream>

namespace {
namespace perf = snow_shot::presentation::recording_perf;

class Measurements final : public QObject, public perf::Sink {
  public:
    qint64 paints = 0;
    qint64 resizes = 0;
    qint64 paintedLogicalPixels = 0;
    qint64 layouts = 0;

    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Resize)
            ++resizes;
        if (event->type() == QEvent::Paint) {
            ++paints;
            for (const QRect& rect : static_cast<QPaintEvent*>(event)->region())
                paintedLogicalPixels += qint64(rect.width()) * rect.height();
        }
        return false;
    }
    void recordScope(const char*, qint64) override {}
    void recordMilestone(const char*, qint64) override {}
    void recordCounter(const char* name, qint64 value) override {
        if (std::strcmp(name, "area.follow_layouts") == 0)
            layouts += value;
    }
    void finish(bool) override {}
};

void drainEvents() {
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
    QCoreApplication::processEvents();
}
} // namespace

int runRecordingFollowPerformanceBenchmark() {
    constexpr int frames = 180;
    for (const QSize extent : {QSize(1920, 1080), QSize(3840, 2160)}) {
        for (const bool stationary : {false, true}) {
            // The first sample warms backing stores and native window resources.
            for (int sample = -1; sample < 3; ++sample) {
                ScreenRecordingAreaWindow area;
                const QPoint origin =
                    QGuiApplication::primaryScreen()->geometry().topLeft() + QPoint(40, 40);
                const QRectF initial(origin, extent);
                area.setRecordingRegion(initial.toRect());
                area.setFollowCaptureActive(true);
                area.setRecordingState(ScreenshotToolPalette::RecordingState::Recording);
                area.show();
                drainEvents();
                Measurements measurements;
                area.installEventFilter(&measurements);
                perf::installSink(&measurements);
                perf::beginSample("follow", extent.width(), extent.height());
                QElapsedTimer timer;
                timer.start();
                for (int frame = 1; frame <= frames; ++frame) {
                    const qreal offset = stationary ? 0.0 : frame % 80 + 0.25;
                    area.setCaptureViewport(initial.translated(offset, offset / 2));
                    drainEvents();
                }
                const qint64 elapsed = timer.nsecsElapsed();
                perf::finish(true);
                perf::installSink(nullptr);
                area.removeEventFilter(&measurements);
                if (sample >= 0) {
                    const QJsonObject result{
                        {QStringLiteral("platform"), QGuiApplication::platformName()},
                        {QStringLiteral("dpr"), area.devicePixelRatioF()},
                        {QStringLiteral("width"), extent.width()},
                        {QStringLiteral("height"), extent.height()},
                        {QStringLiteral("stationary"), stationary},
                        {QStringLiteral("sample"), sample},
                        {QStringLiteral("frames"), frames},
                        {QStringLiteral("elapsed_ns"), elapsed},
                        {QStringLiteral("resizes"), measurements.resizes},
                        {QStringLiteral("paints"), measurements.paints},
                        {QStringLiteral("painted_logical_pixels"),
                         measurements.paintedLogicalPixels},
                        {QStringLiteral("layouts"), measurements.layouts}};
                    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData()
                              << '\n';
                }
            }
        }
    }
    return 0;
}
