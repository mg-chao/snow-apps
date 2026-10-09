#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"
#include "snow_shot/storage/settingsadapters.h"
#include "../../snow_draw_engine_qt/src/core/snow_canvas_runtime_access.h"

#include <QApplication>
#include <QAction>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QScreen>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <qscopeguard.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#endif

namespace {
namespace storage = snow_shot::storage;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QImage sourceImage(QSize size) {
    QImage image(size, QImage::Format_RGB32);
    require(!image.isNull(), "allocate lifecycle fixture");
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x)
            row[x] = qRgb((x * 7 + y) & 255, (y * 13 + x) & 255, (x * 3 + y * 5) & 255);
    }
    return image;
}

struct Sample {
    double openMs = 0;
    double closeReturnMs = 0;
    double hiddenMs = 0;
    double destroyedMs = 0;
    double longestEventTurnMs = 0;
    qint64 residentBeforeBytes = 0;
    qint64 residentLiveBytes = 0;
    qint64 residentAfterBytes = 0;
    qint64 privateBeforeBytes = 0;
    qint64 privateLiveBytes = 0;
    qint64 privateAfterBytes = 0;
};

struct ProcessMemory {
    qint64 residentBytes = 0;
    qint64 privateBytes = 0;
};

ProcessMemory processMemory() {
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                static_cast<DWORD>(sizeof(counters))))
        return {static_cast<qint64>(counters.WorkingSetSize),
                static_cast<qint64>(counters.PrivateUsage)};
#elif defined(Q_OS_MACOS)
    mach_task_basic_info_data_t counters{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&counters),
                  &count) == KERN_SUCCESS)
        return {static_cast<qint64>(counters.resident_size), 0};
#endif
    return {};
}

void memoryBeforeOpen(Sample& sample) {
    const auto memory = processMemory();
    sample.residentBeforeBytes = memory.residentBytes;
    sample.privateBeforeBytes = memory.privateBytes;
}

void memoryWhileOpen(Sample& sample) {
    const auto memory = processMemory();
    sample.residentLiveBytes = memory.residentBytes;
    sample.privateLiveBytes = memory.privateBytes;
}

void memoryAfterClose(Sample& sample) {
    const auto memory = processMemory();
    sample.residentAfterBytes = memory.residentBytes;
    sample.privateAfterBytes = memory.privateBytes;
}

class HideObserver final : public QObject {
  public:
    HideObserver(QElapsedTimer& timer, Sample& sample) : m_timer(timer), m_sample(sample) {}

  protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Hide && m_sample.hiddenMs == 0)
            m_sample.hiddenMs = double(m_timer.nsecsElapsed()) / 1'000'000.;
        return false;
    }

  private:
    QElapsedTimer& m_timer;
    Sample& m_sample;
};

storage::PinnedWindowRecord record(const QString& id, const QImage& image, QScreen& screen,
                                   bool showShadow) {
    const auto fit = ScreenshotGeometryMapper::fitImageToAvailableGeometry(
        image.size(), screen.availableGeometry(), screen.geometry(),
        ScreenshotGeometryMapper::physicalRectForScreen(screen), 16);
    require(fit.valid, "fit lifecycle fixture");
    storage::PinnedWindowRecord result;
    result.id = id;
    result.image = image;
    result.nativeGeometry = fit.nativeGeometry;
    result.initialWindowSize = fit.initialWindowSize;
    result.canvasSourceRect = QRectF(QPointF(), image.size());
    result.contentCanvasRect = result.canvasSourceRect;
    result.surfaceCanvasRect = result.canvasSourceRect;
    result.showBorder = true;
    result.showShadow = showShadow;
    return result;
}

Sample runSample(const storage::PinnedWindowRecord& source, QScreen& screen,
                 storage::PinnedWindowRepository& repository, bool concurrentRead) {
    Sample sample;
    ScreenshotPinnedWindow::Config config;
    config.screen = &screen;
    config.nativeGeometry = source.nativeGeometry;
    config.initialWindowSize = source.initialWindowSize;
    config.canvasSourceRect = source.canvasSourceRect;
    config.imageSource = ScreenshotImageSource::fromImage(source.image, source.canvasSourceRect);
    config.initialCanvasSession = source.canvasSession;
    config.automaticTextRecognition = false;
    config.persistenceId = source.id;
    config.persistenceWriter = [&repository](const auto& snapshot) {
        require(repository.updateState(snapshot).success, "persist lifecycle state");
    };
    config.persistenceCloser = [&repository](const auto& snapshot) {
        require(repository.updateState(snapshot).success, "persist lifecycle close state");
        require(repository.markClosedDeferred(snapshot.id).success, "mark lifecycle source closed");
        storage::ApplicationStorage::instance().requestPinnedWindowRetentionCleanup();
    };
    require(repository.markRestored(source.id).success, "mark lifecycle source restored");
    memoryBeforeOpen(sample);
    QElapsedTimer open;
    open.start();
    QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow);
    require(window->present(config), "present lifecycle window");
    sample.openMs = double(open.nsecsElapsed()) / 1'000'000.;
    QApplication::processEvents();
    const QJsonObject visibility = window->automationState();
    require(visibility.value(QStringLiteral("show_border")).toBool() &&
                visibility.value(QStringLiteral("show_shadow")).toBool() == source.showShadow,
            "lifecycle pin must use the configured border and shadow visibility");
    memoryWhileOpen(sample);

    QSemaphore readStarted;
    std::atomic<bool> announced = false;
    std::future<bool> loading;
    if (concurrentRead) {
        loading = std::async(std::launch::async, [&] {
            return repository
                .loadRecord(QStringLiteral("73b485f8-e136-4387-b357-7e79a81a9c76"),
                            [&](qint64) {
                                if (!announced.exchange(true))
                                    readStarted.release();
                                return true;
                            })
                .has_value();
        });
        require(readStarted.tryAcquire(1, 10000), "background read started");
    }

    QElapsedTimer close;
    close.start();
    HideObserver observer(close, sample);
    window->installEventFilter(&observer);
    QObject::connect(window, &QObject::destroyed, &observer,
                     [&] { sample.destroyedMs = double(close.nsecsElapsed()) / 1'000'000.; });
    require(window->automationAction(QStringLiteral("close")), "lifecycle close input accepted");
    sample.closeReturnMs = double(close.nsecsElapsed()) / 1'000'000.;
    while (window && close.elapsed() < 30000) {
        QElapsedTimer turn;
        turn.start();
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        sample.longestEventTurnMs =
            std::max(sample.longestEventTurnMs, double(turn.nsecsElapsed()) / 1'000'000.);
    }
    require(!window, "lifecycle window deletion completed");
    require(sample.hiddenMs > 0 && sample.destroyedMs > 0, "observe hide and destruction");
    if (loading.valid())
        require(loading.get(), "background source loaded successfully");
    memoryAfterClose(sample);
    return sample;
}

// Exercise the application's asynchronous disk restore and window pool, including
// two closes in one event turn so cleanup cannot hide the next input's latency.
Sample runRestoreSample(ScreenshotSelectionExportUiServices& services,
                        snow_shot::presentation::PinnedWindowGroupManager& groups, int windowCount,
                        bool showShadow) {
    Sample sample;
    std::vector<QPointer<ScreenshotPinnedWindow>> windows;
    memoryBeforeOpen(sample);
    QElapsedTimer open;
    open.start();
    for (int index = 0; index < windowCount; ++index)
        services.restoreLastClosedWindow();
    while (int(windows.size()) < windowCount && open.elapsed() < 30000) {
        QElapsedTimer turn;
        turn.start();
        QApplication::processEvents();
        sample.longestEventTurnMs =
            std::max(sample.longestEventTurnMs, double(turn.nsecsElapsed()) / 1'000'000.);
        for (auto* window : groups.liveWindows()) {
            if (std::find(windows.cbegin(), windows.cend(), window) == windows.cend() &&
                window->automationState().value(QStringLiteral("ready")).toBool()) {
                windows.emplace_back(window);
            }
        }
        if (int(windows.size()) < windowCount)
            QThread::msleep(1);
    }
    require(int(windows.size()) == windowCount, "last closed windows restored and ready");
    sample.openMs = double(open.nsecsElapsed()) / 1'000'000.;
    for (const auto& window : windows) {
        const QJsonObject visibility = window->automationState();
        require(visibility.value(QStringLiteral("show_border")).toBool() &&
                    visibility.value(QStringLiteral("show_shadow")).toBool() == showShadow,
                "restored lifecycle pin must retain the configured decoration visibility");
    }
    memoryWhileOpen(sample);
    QElapsedTimer close;
    close.start();
    std::vector<std::unique_ptr<HideObserver>> observers;
    // Deliver both close inputs in one event turn, matching the reported sequence.
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        observers.push_back(std::make_unique<HideObserver>(close, sample));
        (*it)->installEventFilter(observers.back().get());
        QObject::connect(*it, &QObject::destroyed, observers.back().get(),
                         [&] { sample.destroyedMs = double(close.nsecsElapsed()) / 1'000'000.; });
        require((*it)->automationAction(QStringLiteral("close")),
                "restored window accepts close input");
        require(!(*it)->isVisible(), "one close input hides restored window");
    }
    sample.closeReturnMs = double(close.nsecsElapsed()) / 1'000'000.;
    while (
        std::any_of(windows.cbegin(), windows.cend(), [](const auto& window) { return window; }) &&
        close.elapsed() < 30000) {
        QElapsedTimer turn;
        turn.start();
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        sample.longestEventTurnMs =
            std::max(sample.longestEventTurnMs, double(turn.nsecsElapsed()) / 1'000'000.);
    }
    require(
        std::all_of(windows.cbegin(), windows.cend(), [](const auto& window) { return !window; }),
        "restored windows deleted");
    memoryAfterClose(sample);
    return sample;
}

QJsonObject distribution(std::vector<double> values, QStringView unit = u"ms") {
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        const auto index = size_t(std::ceil(fraction * double(values.size())) - 1);
        return values[std::min(index, values.size() - 1)];
    };
    return {{QStringLiteral("p50_%1").arg(unit), percentile(.50)},
            {QStringLiteral("p95_%1").arg(unit), percentile(.95)},
            {QStringLiteral("p99_%1").arg(unit), percentile(.99)},
            {QStringLiteral("max_%1").arg(unit), values.back()}};
}

QByteArray serializeTwoPass(SnowCanvasRuntime& runtime) {
    const auto handle = snow_canvas_runtime::Access::handle(runtime);
    std::size_t size = 0;
    require(snow_runtime_serialize_document_session(handle, nullptr, 0, &size) == SNOW_OK,
            "query reference session size");
    QByteArray bytes(qsizetype(size), Qt::Uninitialized);
    require(snow_runtime_serialize_document_session(
                handle, reinterpret_cast<std::uint8_t*>(bytes.data()), size, &size) == SNOW_OK,
            "serialize reference session");
    return bytes;
}

QJsonObject measureSerialization(const QByteArray& session, int count) {
    SnowCanvasRuntime runtime;
    require(session.isEmpty() || runtime.restoreDocumentSession(session),
            "restore serialization fixture");
    const auto expected = serializeTwoPass(runtime);
    std::vector<double> reference;
    std::vector<double> current;
    for (int cycle = -3; cycle < count; ++cycle) {
        // Alternate order so either implementation pays the same cold-cache costs.
        for (int index = 0; index < 2; ++index) {
            const bool twoPass = (cycle + index) % 2 == 0;
            QElapsedTimer timer;
            timer.start();
            const auto bytes =
                twoPass ? serializeTwoPass(runtime) : runtime.serializeDocumentSession();
            const double elapsed = double(timer.nsecsElapsed()) / 1'000'000.;
            require(bytes == expected, "serialization preserves every document/history byte");
            if (cycle >= 0)
                (twoPass ? reference : current).push_back(elapsed);
        }
    }
    return {{QStringLiteral("session_bytes"), expected.size()},
            {QStringLiteral("two_pass_reference"), distribution(std::move(reference))},
            {QStringLiteral("qt_snapshot"), distribution(std::move(current))}};
}

QJsonObject measurePreparedShell(QScreen& screen, int count) {
    std::vector<double> construction;
    std::vector<double> nativePrewarm;
    for (int cycle = -3; cycle < count; ++cycle) {
        QElapsedTimer timer;
        timer.start();
        auto window = std::make_unique<ScreenshotPinnedWindow>();
        const double constructed = double(timer.nsecsElapsed()) / 1'000'000.;
        timer.restart();
        require(window->prewarm(&screen), "prepare native shell");
        const double prepared = double(timer.nsecsElapsed()) / 1'000'000.;
        if (cycle >= 0) {
            construction.push_back(constructed);
            nativePrewarm.push_back(prepared);
        }
        window.reset();
        QApplication::processEvents();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    return {{QStringLiteral("construction"), distribution(std::move(construction))},
            {QStringLiteral("native_prewarm"), distribution(std::move(nativePrewarm))}};
}
} // namespace

int runPinnedLifecyclePerformanceBenchmark(QApplication& application) {
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption(
        {QStringLiteral("lifecycle"), QStringLiteral("Measure pinned open/close latency")});
    parser.addOption({QStringLiteral("show-shadow"),
                      QStringLiteral("Measure pinned lifecycle with border and shadow visible")});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Measured cycles per scenario"),
                      QStringLiteral("count"), QStringLiteral("40")});
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON result path"), QStringLiteral("path")});
    parser.process(application);
    bool valid = false;
    const int count = parser.value(QStringLiteral("samples")).toInt(&valid);
    const bool showShadow = parser.isSet(QStringLiteral("show-shadow"));
    require(valid && count >= 5 && count <= 200, "samples must be between 5 and 200");
    QApplication::setQuitOnLastWindowClosed(false);
    auto* screen = QApplication::primaryScreen();
    require(screen, "lifecycle benchmark requires a screen");
    QTemporaryDir directory;
    require(directory.isValid(), "temporary lifecycle storage");
    auto& applicationStorage = storage::ApplicationStorage::instance();
    applicationStorage.shutdown();
    require(applicationStorage
                .initialize({QDir(directory.path()).filePath(QStringLiteral("bin")),
                             QDir(directory.path()).filePath(QStringLiteral("settings")), 20})
                .success,
            "initialize isolated lifecycle storage");
    const auto shutdown = qScopeGuard([&] { applicationStorage.shutdown(); });
    auto& repository = applicationStorage.pinnedWindows();
    const storage::PinToScreenSettings settings;
    require(settings.setDoubleClickAction(QStringLiteral("none")) &&
                settings.setAutomaticTextRecognition(false) &&
                settings.setShowBorderByDefault(true) &&
                settings.setShowShadowByDefault(showShadow),
            "configure reported pin settings in isolated storage");
    QJsonArray scenarios;
    QJsonObject serialization;
    for (int scenarioIndex = 0; scenarioIndex < 6; ++scenarioIndex) {
        const bool concurrentRead = scenarioIndex == 2;
        const bool restore = scenarioIndex >= 3;
        const bool edited = scenarioIndex == 1 || scenarioIndex == 5;
        const auto image =
            sourceImage(concurrentRead || scenarioIndex == 3 ? QSize(2560, 1440) : QSize(800, 600));
        auto source = record(QStringLiteral("638ab928-9c03-43ac-b63a-35dce600f014"), image, *screen,
                             showShadow);
        if (edited) {
            SnowCanvasRuntime runtime;
            for (int batch = 0; batch < 8; ++batch) {
                QJsonArray operations;
                for (int index = 0; index < 64; ++index) {
                    operations.append(
                        QJsonObject{{QStringLiteral("type"), QStringLiteral("rectangle")},
                                    {QStringLiteral("bounds"),
                                     QJsonArray{(index * 13) % 750, (batch * 67) % 550, 30, 30}}});
                }
                require(
                    !runtime
                         .applyAnnotationTransaction(
                             QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                       {QStringLiteral("operations"), operations}})
                                 .toJson(QJsonDocument::Compact))
                         .isEmpty(),
                    "create edited lifecycle fixture");
            }
            require(runtime.canUndo(), "edited lifecycle fixture has history");
            source.canvasSession = runtime.serializeDocumentSession();
        }
        if (scenarioIndex == 0 || scenarioIndex == 1)
            serialization[edited ? QStringLiteral("edited") : QStringLiteral("empty")] =
                measureSerialization(source.canvasSession, count);
        require(repository.upsert(source).success, "seed lifecycle source");
        require(repository
                    .upsert(record(QStringLiteral("73b485f8-e136-4387-b357-7e79a81a9c76"),
                                   sourceImage(QSize(4000, 3000)), *screen, showShadow))
                    .success,
                "seed background read source");
        const QString secondId = QStringLiteral("87308318-8f7a-49f4-95fd-f8d6a78c7621");
        if (restore) {
            require(
                repository.remove(QStringLiteral("73b485f8-e136-4387-b357-7e79a81a9c76")).success,
                "remove unrelated restore candidate");
            require(repository.markClosedDeferred(source.id).success, "close restore fixture");
            if (scenarioIndex == 4) {
                auto second = source;
                second.id = secondId;
                require(repository.upsert(second).success &&
                            repository.markClosedDeferred(secondId).success,
                        "seed second closed window");
            } else {
                require(repository.remove(secondId).success, "remove previous second window");
            }
        }
        require(repository.flush().success, "commit lifecycle fixture");
        snow_shot::presentation::PinnedWindowGroupManager groups(&repository);
        ScreenshotSelectionExportUiServices services(nullptr, nullptr, nullptr, {}, {}, &groups);
        std::vector<Sample> samples;
        for (int cycle = -3; cycle < count; ++cycle) {
            const auto sample =
                restore ? runRestoreSample(services, groups, scenarioIndex == 4 ? 2 : 1, showShadow)
                        : runSample(source, *screen, repository, concurrentRead);
            if (cycle >= 0)
                samples.push_back(sample);
        }
        const auto metric = [&](auto member, QStringView unit = u"ms") {
            std::vector<double> values;
            for (const auto& sample : samples)
                values.push_back(static_cast<double>(sample.*member));
            return distribution(std::move(values), unit);
        };
        const auto retained = [&](auto after, auto before) {
            std::vector<double> values;
            for (const auto& sample : samples)
                values.push_back(static_cast<double>(sample.*after - sample.*before));
            return distribution(std::move(values), u"bytes");
        };
        QJsonObject scenario{
            {QStringLiteral("name"),
             scenarioIndex == 5   ? QStringLiteral("edited-restore-close")
             : scenarioIndex == 4 ? QStringLiteral("restore-restore-close-close")
             : restore            ? QStringLiteral("restore-close")
             : concurrentRead     ? QStringLiteral("large-close-during-payload-read")
             : scenarioIndex == 1 ? QStringLiteral("edited-open-close")
                                  : QStringLiteral("rapid-open-close")},
            {QStringLiteral("samples"), count},
            {QStringLiteral("windows_per_cycle"), scenarioIndex == 4 ? 2 : 1},
            {QStringLiteral("image_width"), image.width()},
            {QStringLiteral("image_height"), image.height()},
            {QStringLiteral("canvas_session_bytes"), source.canvasSession.size()},
            {QStringLiteral("show_border"), true},
            {QStringLiteral("show_shadow"), showShadow},
            {QStringLiteral("open"), metric(&Sample::openMs)},
            {QStringLiteral("close_return"), metric(&Sample::closeReturnMs)},
            {QStringLiteral("hidden"), metric(&Sample::hiddenMs)},
            {QStringLiteral("destroyed"), metric(&Sample::destroyedMs)},
            {QStringLiteral("longest_event_turn"), metric(&Sample::longestEventTurnMs)},
            {QStringLiteral("process_memory"),
             QJsonObject{
                 {QStringLiteral("resident_before"),
                  metric(&Sample::residentBeforeBytes, u"bytes")},
                 {QStringLiteral("resident_live"), metric(&Sample::residentLiveBytes, u"bytes")},
                 {QStringLiteral("resident_after"), metric(&Sample::residentAfterBytes, u"bytes")},
                 {QStringLiteral("resident_retained_delta"),
                  retained(&Sample::residentAfterBytes, &Sample::residentBeforeBytes)},
                 {QStringLiteral("private_before"), metric(&Sample::privateBeforeBytes, u"bytes")},
                 {QStringLiteral("private_live"), metric(&Sample::privateLiveBytes, u"bytes")},
                 {QStringLiteral("private_after"), metric(&Sample::privateAfterBytes, u"bytes")},
                 {QStringLiteral("private_retained_delta"),
                  retained(&Sample::privateAfterBytes, &Sample::privateBeforeBytes)}}}};
        scenarios.append(scenario);
        std::cout << QJsonDocument(scenario).toJson(QJsonDocument::Compact).constData() << '\n';
    }
    const QJsonDocument report(QJsonObject{
        {QStringLiteral("configuration"), QStringLiteral("Release")},
        {QStringLiteral("platform"), QApplication::platformName()},
        {QStringLiteral("show_border"), true},
        {QStringLiteral("show_shadow"), showShadow},
        {QStringLiteral("resident_memory_available"), processMemory().residentBytes > 0},
        {QStringLiteral("private_memory_available"), processMemory().privateBytes > 0},
        {QStringLiteral("scenarios"), scenarios},
        {QStringLiteral("serialization"), serialization},
        {QStringLiteral("prepared_shell"), measurePreparedShell(*screen, count)}});
    if (parser.isSet(QStringLiteral("output"))) {
        QFile output(parser.value(QStringLiteral("output")));
        require(output.open(QIODevice::WriteOnly), "open lifecycle report");
        require(output.write(report.toJson()) > 0, "write lifecycle report");
    }
    return 0;
}
