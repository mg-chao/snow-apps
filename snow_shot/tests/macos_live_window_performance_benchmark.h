// SPDX-License-Identifier: GPL-3.0-or-later
// Included by the native macOS fixture only in benchmark-enabled presets.
class BenchmarkPin final : public QWidget {
  public:
    QImage image;
    QTransform transform;
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        snow_shot::presentation::live_window_detail::paintLiveWindowFrame(painter, rect(), image,
                                                                          transform, true);
    }
};
double processCpuMs() {
    rusage usage{};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "Process CPU usage is available.");
    return double(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
           double(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}
uint64_t processFootprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    require(task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info),
                      &count) == KERN_SUCCESS,
            "Process memory footprint is available.");
    return info.phys_footprint;
}
int runPerformance() {
    using namespace snow_shot::presentation;
    if (QGuiApplication::platformName() != QStringLiteral("cocoa") ||
        !CGPreflightScreenCaptureAccess())
        return 77;
    SourceWindow window;
    window.resize(1920, 1080);
    window.move(40, 40);
    window.show();
    require(waitFor([&] { return !desktopBounds(nativeId(window)).isEmpty(); }),
            "The animated benchmark source is visible.");
    const QRect bounds = desktopBounds(nativeId(window));
    const LiveWindowTarget target{nativeId(window), NSProcessInfo.processInfo.processIdentifier,
                                  bounds, bounds.center()};
    int changes = 0;
    QTimer animation;
    animation.setTimerType(Qt::PreciseTimer);
    animation.setInterval(33);
    QObject::connect(&animation, &QTimer::timeout, &window, [&] {
        ++changes;
        window.color = QColor(40 + changes % 120, 120, 200);
        window.update();
    });
    animation.start();
    for (int count : {1, 4}) {
        for (bool native : {false, true}) {
            std::vector<std::unique_ptr<BenchmarkPin>> pins;
            std::vector<std::shared_ptr<LiveWindowSource>> sources;
            int cpuFrames = 0;
            QSize capturedPixels;
            for (int index = 0; index < count; ++index) {
                auto pin = std::make_unique<BenchmarkPin>();
                pin->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
                pin->setAttribute(Qt::WA_TranslucentBackground);
                pin->setGeometry(100 + index * 170, 150 + index * 100, 480, 270);
                pin->show();
                auto source = createLiveWindowSource(target, bounds);
                bool initialized = false;
                source->initialize([&](bool ok, const QString&) {
                    require(ok, "Benchmark live source initializes.");
                    initialized = true;
                });
                require(waitFor([&] { return initialized; }),
                        "Benchmark initialization completes.");
                capturedPixels = source->initialFrame().size();
                source->releaseInitialFrame();
                QObject::connect(source.get(), &LiveWindowSource::frameChanged, pin.get(),
                                 [&, view = pin.get()](const QImage& image) {
                                     ++cpuFrames;
                                     view->image = image;
                                     view->update();
                                 });
                LiveWindowPresentation presentation;
                // No native owner exercises the supported raster fallback using
                // the same capture broker, source dimensions, and image cadence.
                presentation.owner = native ? pin.get() : nullptr;
                require(source->setPresentation(presentation), "Benchmark display starts.");
                require(source->usesNativePresentation() == native,
                        "Benchmark uses its chosen path.");
                pins.push_back(std::move(pin));
                sources.push_back(std::move(source));
            }
            QElapsedTimer clock;
            clock.start();
            waitFor([&] { return clock.elapsed() >= 700; });
            cpuFrames = 0;
            changes = 0;
            const double cpuBefore = processCpuMs();
            clock.restart();
            waitFor([&] { return clock.elapsed() >= 3000; });
            const double wallMs = double(clock.nsecsElapsed()) / 1000000.0;
            const double cpuMs = processCpuMs() - cpuBefore;
            require(native ? cpuFrames == 0 : cpuFrames >= 30 * count,
                    "Native display avoids CPU frames; raster baseline continues updating.");
            std::cout << "live_macos mode=" << (native ? "native" : "raster") << " pins=" << count
                      << " source=" << capturedPixels.width() << 'x' << capturedPixels.height()
                      << " cpu_ms=" << cpuMs << " wall_ms=" << wallMs
                      << " cpu_core_percent=" << cpuMs * 100.0 / wallMs
                      << " footprint_bytes=" << processFootprint() << " cpu_frames=" << cpuFrames
                      << " source_updates=" << changes << '\n';
            for (auto& source : sources)
                source->shutdown();
            sources.clear();
            pins.clear();
            clock.restart();
            waitFor([&] { return clock.elapsed() >= 300; });
        }
    }
    return EXIT_SUCCESS;
}
