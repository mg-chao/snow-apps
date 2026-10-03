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
};

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

storage::PinnedWindowRecord record(const QString& id, const QImage& image, QScreen& screen) {
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
    QElapsedTimer open;
    open.start();
    QPointer<ScreenshotPinnedWindow> window(new ScreenshotPinnedWindow);
    require(window->present(config), "present lifecycle window");
    sample.openMs = double(open.nsecsElapsed()) / 1'000'000.;
    QApplication::processEvents();

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
    auto* closeAction = window->findChild<QAction*>(QStringLiteral("screenshotPinnedCloseAction"));
    require(closeAction, "lifecycle close action exists");
    closeAction->trigger();
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
    return sample;
}

// Exercise the application's asynchronous disk restore and window pool, including
// two closes in one event turn so cleanup cannot hide the next input's latency.
Sample runRestoreSample(ScreenshotSelectionExportUiServices& services,
                        snow_shot::presentation::PinnedWindowGroupManager& groups,
                        int windowCount) {
    Sample sample;
    std::vector<QPointer<ScreenshotPinnedWindow>> windows;
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
    QElapsedTimer close;
    close.start();
    std::vector<std::unique_ptr<HideObserver>> observers;
    // Deliver both close inputs in one event turn, matching the reported sequence.
    for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
        observers.push_back(std::make_unique<HideObserver>(close, sample));
        (*it)->installEventFilter(observers.back().get());
        QObject::connect(*it, &QObject::destroyed, observers.back().get(),
                         [&] { sample.destroyedMs = double(close.nsecsElapsed()) / 1'000'000.; });
        auto* action = (*it)->findChild<QAction*>(QStringLiteral("screenshotPinnedCloseAction"));
        require(action, "restored window exposes close action");
        action->trigger();
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
    return sample;
}

QJsonObject distribution(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        const auto index = size_t(std::ceil(fraction * double(values.size())) - 1);
        return values[std::min(index, values.size() - 1)];
    };
    return {{QStringLiteral("p50_ms"), percentile(.50)},
            {QStringLiteral("p95_ms"), percentile(.95)},
            {QStringLiteral("p99_ms"), percentile(.99)},
            {QStringLiteral("max_ms"), values.back()}};
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
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Measured cycles per scenario"),
                      QStringLiteral("count"), QStringLiteral("40")});
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON result path"), QStringLiteral("path")});
    parser.process(application);
    bool valid = false;
    const int count = parser.value(QStringLiteral("samples")).toInt(&valid);
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
                settings.setAutomaticTextRecognition(false),
            "configure reported pin settings in isolated storage");
    QJsonArray scenarios;
    QJsonObject serialization;
    for (int scenarioIndex = 0; scenarioIndex < 6; ++scenarioIndex) {
        const bool concurrentRead = scenarioIndex == 2;
        const bool restore = scenarioIndex >= 3;
        const bool edited = scenarioIndex == 1 || scenarioIndex == 5;
        const auto image =
            sourceImage(concurrentRead || scenarioIndex == 3 ? QSize(2560, 1440) : QSize(800, 600));
        auto source =
            record(QStringLiteral("638ab928-9c03-43ac-b63a-35dce600f014"), image, *screen);
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
                                   sourceImage(QSize(4000, 3000)), *screen))
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
            const auto sample = restore
                                    ? runRestoreSample(services, groups, scenarioIndex == 4 ? 2 : 1)
                                    : runSample(source, *screen, repository, concurrentRead);
            if (cycle >= 0)
                samples.push_back(sample);
        }
        const auto metric = [&](auto member) {
            std::vector<double> values;
            for (const auto& sample : samples)
                values.push_back(sample.*member);
            return distribution(std::move(values));
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
            {QStringLiteral("open"), metric(&Sample::openMs)},
            {QStringLiteral("close_return"), metric(&Sample::closeReturnMs)},
            {QStringLiteral("hidden"), metric(&Sample::hiddenMs)},
            {QStringLiteral("destroyed"), metric(&Sample::destroyedMs)},
            {QStringLiteral("longest_event_turn"), metric(&Sample::longestEventTurnMs)}};
        scenarios.append(scenario);
        std::cout << QJsonDocument(scenario).toJson(QJsonDocument::Compact).constData() << '\n';
    }
    const QJsonDocument report(
        QJsonObject{{QStringLiteral("configuration"), QStringLiteral("Release")},
                    {QStringLiteral("platform"), QApplication::platformName()},
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
