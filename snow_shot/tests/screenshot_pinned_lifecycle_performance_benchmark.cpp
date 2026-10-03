#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/pinnedwindowrepository.h"

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
#include <QTimer>

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
    auto& repository = applicationStorage.pinnedWindows();
    QJsonArray scenarios;
    for (int scenarioIndex = 0; scenarioIndex < 3; ++scenarioIndex) {
        const bool concurrentRead = scenarioIndex == 2;
        const auto image = sourceImage(concurrentRead ? QSize(2560, 1440) : QSize(800, 600));
        auto source =
            record(QStringLiteral("638ab928-9c03-43ac-b63a-35dce600f014"), image, *screen);
        if (scenarioIndex == 1) {
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
        require(repository.upsert(source).success, "seed lifecycle source");
        require(repository
                    .upsert(record(QStringLiteral("73b485f8-e136-4387-b357-7e79a81a9c76"),
                                   sourceImage(QSize(4000, 3000)), *screen))
                    .success,
                "seed background read source");
        require(repository.flush().success, "commit lifecycle fixture");
        std::vector<Sample> samples;
        for (int cycle = -3; cycle < count; ++cycle) {
            const auto sample = runSample(source, *screen, repository, concurrentRead);
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
            {QStringLiteral("name"), concurrentRead
                                         ? QStringLiteral("large-close-during-payload-read")
                                     : scenarioIndex == 1 ? QStringLiteral("edited-open-close")
                                                          : QStringLiteral("rapid-open-close")},
            {QStringLiteral("samples"), count},
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
                    {QStringLiteral("scenarios"), scenarios}});
    if (parser.isSet(QStringLiteral("output"))) {
        QFile output(parser.value(QStringLiteral("output")));
        require(output.open(QIODevice::WriteOnly), "open lifecycle report");
        require(output.write(report.toJson()) > 0, "write lifecycle report");
    }
    applicationStorage.shutdown();
    return 0;
}
