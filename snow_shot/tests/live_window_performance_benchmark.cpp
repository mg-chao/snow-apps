#include "snow_shot/presentation/livewindowsource.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QWidget>
#include <cstdlib>
#include <iostream>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <psapi.h>
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
template <typename Predicate> bool waitFor(Predicate predicate, int timeout = 10000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) {
        QApplication::processEvents();
        QThread::msleep(1);
    }
    QApplication::processEvents();
    return predicate();
}
class SourceWindow final : public QWidget {
  public:
    int phase = 0;
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor::fromHsv(phase % 360, 180, 220));
    }
};
#ifdef Q_OS_WIN
PROCESS_MEMORY_COUNTERS_EX memoryUsage() {
    PROCESS_MEMORY_COUNTERS_EX usage{};
    usage.cb = sizeof(usage);
    require(K32GetProcessMemoryInfo(GetCurrentProcess(),
                                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&usage),
                                    sizeof(usage)) != FALSE,
            "Process memory counters must be available for the benchmark.");
    return usage;
}
#endif
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
#ifndef Q_OS_WIN
    std::cout << "This native DWM/WGC benchmark requires Windows.\n";
    return EXIT_SUCCESS;
#else
    using namespace snow_shot::presentation;
    SourceWindow window;
    window.setWindowFlag(Qt::WindowStaysOnTopHint);
    window.resize(960, 640);
    window.move(100, 100);
    window.show();
    window.raise();
    QApplication::processEvents();
    RECT bounds{};
    GetWindowRect(reinterpret_cast<HWND>(window.winId()), &bounds);
    const QPoint point((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
    std::optional<LiveWindowTarget> target;
    require(waitFor([&] {
                target = liveWindowTargetAtPoint(point, {});
                return target && target->nativeId == static_cast<quintptr>(window.winId());
            }),
            "The benchmark must identify its native source window.");
    const QRect selection(point - QPoint(200, 150), QSize(400, 300));
    const auto initialize = [&](const QRect& crop) {
        auto source = createLiveWindowSource(*target, crop);
        bool done = false;
        bool ok = false;
        source->initialize([&](bool success, const QString&) {
            ok = success;
            done = true;
        });
        require(waitFor([&] { return done; }) && ok, "A benchmark live source must initialize.");
        return source;
    };
    auto source = initialize(selection);
    QWidget shell;
    shell.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    shell.setAttribute(Qt::WA_TranslucentBackground);
    shell.setGeometry(1100, 100, 400, 300);
    shell.show();
    QApplication::processEvents();
    LiveWindowPresentation presentation;
    presentation.owner = &shell;
    require(source->setPresentation(presentation) && source->usesNativePresentation(),
            "Normal live display must use DWM.");
    int deliveries = 0;
    QObject::connect(source.get(), &LiveWindowSource::frameChanged, [&] { ++deliveries; });
    QElapsedTimer idle;
    idle.start();
    waitFor([&] { return idle.elapsed() >= 500; });
    require(deliveries == 0, "DWM display must not continuously deliver CPU image frames.");
    const auto baselineMemory = memoryUsage();

    QTimer updates;
    updates.setInterval(16);
    QObject::connect(&updates, &QTimer::timeout, [&] {
        window.phase += 7;
        window.update();
    });
    updates.start();
    presentation.imageTransform.rotate(90);
    require(source->setPresentation(presentation), "Transformed display must start WGC.");
    QElapsedTimer cadence;
    cadence.start();
    require(waitFor([&] { return deliveries >= 60; }),
            "The source must deliver sixty live updates.");
    const qint64 frameMilliseconds = cadence.elapsed();
    const int cadenceFrames = deliveries;
    require(deliveries <= frameMilliseconds * 30.0 / 1000.0 + 3,
            "Native frame delivery must respect the thirty-fps limit.");

    auto second = initialize(QRect(selection.topLeft() + QPoint(20, 20), QSize(200, 150)));
    LiveWindowPresentation secondPresentation = presentation;
    secondPresentation.owner = &window;
    require(second->setPresentation(secondPresentation),
            "A second crop shares the capture source.");
    int secondDeliveries = 0;
    QObject::connect(second.get(), &LiveWindowSource::frameChanged, [&] { ++secondDeliveries; });
    require(waitFor([&] { return secondDeliveries >= 15; }),
            "A concurrent crop must keep receiving current frames.");
    const auto activeMemory = memoryUsage();

    qint64 exportMilliseconds = 0;
    for (int index = 0; index < 5; ++index) {
        QElapsedTimer timer;
        timer.start();
        bool done = false;
        bool valid = false;
        source->requestFrame([&](const QImage& frame, const QString& error) {
            valid = error.isEmpty() && frame.size() == selection.size();
            done = true;
        });
        require(waitFor([&] { return done; }) && valid, "Fresh exports remain valid under load.");
        exportMilliseconds += timer.elapsed();
    }
    second->shutdown();
    second.reset();
    updates.stop();
    presentation.imageTransform.reset();
    require(source->setPresentation(presentation) && source->usesNativePresentation(),
            "Resetting transforms returns to compositor display.");
    const DWORD baselineHandles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    QElapsedTimer creation;
    creation.start();
    for (int index = 0; index < 6; ++index) {
        auto temporary = initialize(selection);
        temporary->shutdown();
    }
    const qint64 creationMilliseconds = creation.elapsed();
    const DWORD finalHandles = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    require(finalHandles <= baselineHandles + 4,
            "Repeated capture startup must not leak GDI handles.");
    source->shutdown();
    source.reset();
    QApplication::processEvents();
    const auto finalMemory = memoryUsage();
    std::cout << "{\"frames\":" << cadenceFrames << ",\"sixty_frame_ms\":" << frameMilliseconds
              << ",\"source_pixel_width\":" << target->desktopBounds.width()
              << ",\"source_pixel_height\":" << target->desktopBounds.height()
              << ",\"actual_fps\":" << cadenceFrames * 1000.0 / frameMilliseconds
              << ",\"mean_snapshot_ms\":" << exportMilliseconds / 5.0
              << ",\"mean_creation_ms\":" << creationMilliseconds / 6.0
              << ",\"gdi_handle_delta\":" << static_cast<qint64>(finalHandles) - baselineHandles
              << ",\"baseline_working_set_bytes\":" << baselineMemory.WorkingSetSize
              << ",\"active_working_set_bytes\":" << activeMemory.WorkingSetSize
              << ",\"final_working_set_bytes\":" << finalMemory.WorkingSetSize
              << ",\"baseline_private_bytes\":" << baselineMemory.PrivateUsage
              << ",\"active_private_bytes\":" << activeMemory.PrivateUsage
              << ",\"final_private_bytes\":" << finalMemory.PrivateUsage << "}\n";
    return EXIT_SUCCESS;
#endif
}
