#include "snow_shot/presentation/mainwindowskincontroller.h"
#include "snow_shot/presentation/mainwindowskinwidget.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snowimageqtcodec.h"
#include "snow_draw_engine_qt/snow_canvas_region_filter.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstdlib>
#include <iostream>

#ifdef Q_OS_UNIX
#include <sys/resource.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#endif

namespace {
double milliseconds(const QElapsedTimer& timer) {
    return double(timer.nsecsElapsed()) / 1000000.0;
}

QJsonObject distribution(QVector<double> values) {
    std::sort(values.begin(), values.end());
    return {{QStringLiteral("median_ms"), values.at(values.size() / 2)},
            {QStringLiteral("p95_ms"), values.at((values.size() - 1) * 95 / 100)}};
}

bool waitForIdle(QApplication& application,
                 const snow_shot::presentation::MainWindowSkinController& controller) {
    QElapsedTimer timeout;
    timeout.start();
    do {
        application.processEvents();
        QThread::msleep(1);
    } while (controller.diagnostics().busy && timeout.elapsed() < 30000);
    return !controller.diagnostics().busy;
}

double peakResidentBytes() {
#ifdef Q_OS_UNIX
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#ifdef Q_OS_MACOS
    return double(usage.ru_maxrss);
#else
    return double(usage.ru_maxrss) * 1024.0;
#endif
#elif defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS usage{};
    return K32GetProcessMemoryInfo(GetCurrentProcess(), &usage, sizeof(usage))
               ? double(usage.PeakWorkingSetSize)
               : 0.0;
#else
    return 0;
#endif
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    const auto arguments = application.arguments();
    const auto outputIndex = arguments.indexOf(QStringLiteral("--output"));
    const auto samplesIndex = arguments.indexOf(QStringLiteral("--samples"));
    const int samples = samplesIndex < 0 ? 5 : arguments.value(samplesIndex + 1).toInt();
    if (outputIndex < 0 || arguments.value(outputIndex + 1).isEmpty() || samples < 3) {
        std::cerr << "Usage: skin benchmark --output result.json [--samples 5]\n";
        return EXIT_FAILURE;
    }
    QTemporaryDir temporary;
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    if (!temporary.isValid() ||
        !storage.initialize({temporary.path(), temporary.path(), 60000}).success) {
        return EXIT_FAILURE;
    }
    QImage source(3840, 2160, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x) {
            row[x] = qRgb(x % 256, y % 256, (x + y) % 256);
        }
    }
    const QString path = temporary.filePath(QStringLiteral("skin.png"));
    QFile fixture(path);
    if (!fixture.open(QIODevice::WriteOnly)) {
        return EXIT_FAILURE;
    }
    {
        const auto encoded = snow_shot::image_codec::encodePng(source);
        if (fixture.write(encoded) != encoded.size()) {
            return EXIT_FAILURE;
        }
    }
    fixture.close();
    QVector<double> decodeTimes;
    for (int sample = -1; sample < samples; ++sample) {
        QElapsedTimer timer;
        timer.start();
        const auto decoded = snow_shot::image_codec::decodeSkinFile(path);
        if (decoded.image.isNull()) {
            return EXIT_FAILURE;
        }
        if (sample >= 0) {
            decodeTimes.append(milliseconds(timer));
        }
    }
    QJsonArray preparation;
    namespace presentation = snow_shot::presentation;
    SnowCanvasRegionFilterScratch preparationScratch;
    for (const QSize viewport :
         {QSize(900, 640), QSize(1920, 1080), QSize(960, 56), QSize(480, 320), QSize(320, 440)}) {
        for (const qreal dpr : {1.0, 1.5, 2.0}) {
            for (const int blur : {0, 16, 100}) {
                for (const auto mode : {presentation::MainWindowSkinDisplayMode::Overlay,
                                        presentation::MainWindowSkinDisplayMode::Contain}) {
                    QVector<double> times;
                    for (int sample = -1; sample < samples; ++sample) {
                        QElapsedTimer timer;
                        timer.start();
                        const auto frame = presentation::prepareMainWindowSkin(
                            source, viewport, dpr, mode, blur, &preparationScratch);
                        if (frame.image.isNull()) {
                            return EXIT_FAILURE;
                        }
                        if (sample >= 0) {
                            times.append(milliseconds(timer));
                        }
                    }
                    auto record = distribution(times);
                    record.insert(QStringLiteral("logical_width"), viewport.width());
                    record.insert(QStringLiteral("logical_height"), viewport.height());
                    record.insert(QStringLiteral("dpr"), dpr);
                    record.insert(QStringLiteral("blur"), blur);
                    record.insert(QStringLiteral("mode"),
                                  mode == presentation::MainWindowSkinDisplayMode::Overlay
                                      ? QStringLiteral("overlay")
                                      : QStringLiteral("contain"));
                    preparation.append(record);
                }
            }
        }
    }
    presentation::MainWindowSkinController controller;
    QJsonObject report;
    {
        presentation::MainWindowSkinWidget widget(nullptr, &controller);
        widget.resize(1920, 1080);
        widget.show();
        storage.configuration().setValue(QStringLiteral("interface/skin_path"), path);
        storage.configuration().setValue(QStringLiteral("interface/skin_blur_level"), 100);
        QElapsedTimer responsiveness;
        responsiveness.start();
        double previousTick = 0;
        double longestGap = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        QObject::connect(&heartbeat, &QTimer::timeout, &application, [&] {
            const double now = milliseconds(responsiveness);
            longestGap = std::max(longestGap, now - previousTick);
            previousTick = now;
        });
        heartbeat.start();
        controller.reload();
        do {
            application.processEvents();
            QThread::msleep(1);
        } while (controller.diagnostics().busy && responsiveness.elapsed() < 30000);
        heartbeat.stop();
        if (!controller.skinActive()) {
            return EXIT_FAILURE;
        }
        QJsonArray paints;
        quint64 scheduledPaintJobs = 0;
        for (const QSize viewport : {QSize(900, 640), QSize(3840, 2160)}) {
            widget.resize(viewport);
            QElapsedTimer preparationTimeout;
            preparationTimeout.start();
            do {
                application.processEvents();
                QThread::msleep(1);
            } while (controller.diagnostics().busy && preparationTimeout.elapsed() < 30000);
            if (controller.diagnostics().busy) {
                return EXIT_FAILURE;
            }
            const auto beforePaint = controller.diagnostics();
            const qreal dpr = widget.devicePixelRatioF();
            QImage target(QSize(qRound(viewport.width() * dpr), qRound(viewport.height() * dpr)),
                          QImage::Format_ARGB32_Premultiplied);
            target.setDevicePixelRatio(dpr);
            QVector<double> paintTimes;
            for (int sample = -1; sample < samples * 10; ++sample) {
                QElapsedTimer timer;
                timer.start();
                widget.render(&target);
                if (sample >= 0) {
                    paintTimes.append(milliseconds(timer));
                }
            }
            scheduledPaintJobs +=
                controller.diagnostics().preparationJobs - beforePaint.preparationJobs;
            auto record = distribution(paintTimes);
            record.insert(QStringLiteral("physical_width"), target.width());
            record.insert(QStringLiteral("physical_height"), target.height());
            paints.append(record);
        }
        const auto afterPaint = controller.diagnostics();
        report = QJsonObject{
            {QStringLiteral("decode"), distribution(decodeTimes)},
            {QStringLiteral("preparation"), preparation},
            {QStringLiteral("cached_paint"), paints},
            {QStringLiteral("load_ui_max_tick_gap_ms"), longestGap},
            {QStringLiteral("retained_cache_bytes"), double(afterPaint.retainedBytes)},
            {QStringLiteral("retained_worker_scratch_bytes"),
             double(afterPaint.scratchRetainedBytes)},
            {QStringLiteral("retained_preparation_scratch_bytes"),
             double(preparationScratch.retainedBytes())},
            {QStringLiteral("peak_resident_bytes"), peakResidentBytes()},
            {QStringLiteral("cached_paint_scheduled_jobs"), double(scheduledPaintJobs)}};
    }
    if (!storage.configuration().setValues(
            {{QStringLiteral("interface/toolbar_skin_path"), path},
             {QStringLiteral("interface/tray_menu_skin_path"), path}}))
        return EXIT_FAILURE;
    QObject mainView;
    QObject firstToolbar;
    QObject secondToolbar;
    QObject popover;
    QObject matchingPopover;
    QObject trayView;
    const auto beforeConcurrent = controller.diagnostics();
    QElapsedTimer concurrentTime;
    concurrentTime.start();
    double lastTick = 0;
    double concurrentTickGap = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    QObject::connect(&heartbeat, &QTimer::timeout, &application, [&] {
        const double now = milliseconds(concurrentTime);
        concurrentTickGap = std::max(concurrentTickGap, now - lastTick);
        lastTick = now;
    });
    heartbeat.start();
    controller.attach(&mainView, presentation::SkinSurface::MainWindow, QSize(900, 640), 2.0);
    controller.attach(&firstToolbar, presentation::SkinSurface::Toolbar, QSize(960, 56), 2.0);
    controller.attach(&secondToolbar, presentation::SkinSurface::Toolbar, QSize(640, 56), 1.5);
    controller.attach(&popover, presentation::SkinSurface::Toolbar, QSize(480, 320), 2.0);
    controller.attach(&matchingPopover, presentation::SkinSurface::Toolbar, QSize(480, 320), 2.0);
    controller.attach(&trayView, presentation::SkinSurface::TrayMenu, QSize(320, 440), 1.5);
    if (!waitForIdle(application, controller) || !controller.skinActive(&mainView) ||
        !controller.skinActive(&firstToolbar) || !controller.skinActive(&secondToolbar) ||
        !controller.skinActive(&popover) || !controller.skinActive(&trayView) ||
        controller.pixmap(&popover).cacheKey() != controller.pixmap(&matchingPopover).cacheKey())
        return EXIT_FAILURE;
    heartbeat.stop();
    const auto loadedConcurrent = controller.diagnostics();
    const double concurrentLoadMs = milliseconds(concurrentTime);
    QVector<double> popupOpenTimes;
    for (int sample = -1; sample < samples * 10; ++sample) {
        controller.detach(&popover);
        QElapsedTimer timer;
        timer.start();
        controller.attach(&popover, presentation::SkinSurface::Toolbar, QSize(480, 320), 2.0);
        if (!controller.skinActive(&popover))
            return EXIT_FAILURE;
        if (sample >= 0)
            popupOpenTimes.append(milliseconds(timer));
    }
    const auto reopened = controller.diagnostics();
    if (reopened.preparationJobs != loadedConcurrent.preparationJobs ||
        reopened.pixmapConversions != loadedConcurrent.pixmapConversions)
        return EXIT_FAILURE;
    QElapsedTimer resizeTime;
    resizeTime.start();
    for (int step = 1; step <= 30; ++step) {
        controller.setViewport(&firstToolbar, QSize(960 + step, 56), 2.0);
        controller.setViewport(&secondToolbar, QSize(640 + step, 56), 1.5);
    }
    if (!waitForIdle(application, controller))
        return EXIT_FAILURE;
    const auto resized = controller.diagnostics();
    report.insert(
        QStringLiteral("concurrent_surfaces"),
        QJsonObject{
            {QStringLiteral("visible_views"), 6},
            {QStringLiteral("load_ms"), concurrentLoadMs},
            {QStringLiteral("load_ui_max_tick_gap_ms"), concurrentTickGap},
            {QStringLiteral("additional_decode_jobs"),
             double(loadedConcurrent.decodeJobs - beforeConcurrent.decodeJobs)},
            {QStringLiteral("additional_preparation_jobs"),
             double(loadedConcurrent.preparationJobs - beforeConcurrent.preparationJobs)},
            {QStringLiteral("additional_pixmap_conversions"),
             double(loadedConcurrent.pixmapConversions - beforeConcurrent.pixmapConversions)},
            {QStringLiteral("cached_popup_open"), distribution(popupOpenTimes)},
            {QStringLiteral("cached_popup_preparation_jobs"),
             double(reopened.preparationJobs - loadedConcurrent.preparationJobs)},
            {QStringLiteral("resize_burst_ms"), milliseconds(resizeTime)},
            {QStringLiteral("resize_burst_preparation_jobs"),
             double(resized.preparationJobs - reopened.preparationJobs)},
            {QStringLiteral("retained_bytes"), double(resized.retainedBytes)},
            {QStringLiteral("idle_frame_bytes"), double(resized.idleFrameBytes)}});
    controller.detach(&mainView);
    controller.detach(&firstToolbar);
    controller.detach(&secondToolbar);
    controller.detach(&popover);
    controller.detach(&matchingPopover);
    controller.detach(&trayView);
    if (!waitForIdle(application, controller))
        return EXIT_FAILURE;
    report.insert(QStringLiteral("suspended_idle_frame_bytes"),
                  double(controller.diagnostics().idleFrameBytes));
    report.insert(QStringLiteral("suspended_executor_count"),
                  controller.diagnostics().executorCount);
    QFile output(arguments.value(outputIndex + 1));
    if (!output.open(QIODevice::WriteOnly) || output.write(QJsonDocument(report).toJson()) < 0)
        return EXIT_FAILURE;
    std::cout << QJsonDocument(report).toJson().toStdString();
    storage.shutdown();
    return 0;
}
