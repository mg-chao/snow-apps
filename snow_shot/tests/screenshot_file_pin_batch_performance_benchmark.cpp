// Drives the real ScreenshotFilePinBatch pipeline (file snapshot, decode,
// pinned-window presentation) with real pinned windows so batch latency can
// be compared across pipeline changes and prewarm strategies. The present
// callback mirrors ScreenshotController::filePinPresenter and the prewarm
// variants mirror the controller's submit-then-prewarm ordering.

#include "snow_shot/presentation/screenshotfilepinbatch.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/presentation/screenshotselectionexportuiservices.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QApplication>
#include <QColorSpace>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QPointer>
#include <QScreen>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

struct Scenario final {
    const char* name;
    int fileCount;
    QSize imageSize;
    const char* format;
    int quality;
    bool prewarm;
};

struct Sample final {
    qint64 firstPinNanoseconds = 0;
    qint64 totalNanoseconds = 0;
    int presents = 0;
};

[[noreturn]] void fail(const QString& message) {
    throw std::runtime_error(message.toStdString());
}

void require(bool condition, const QString& message) {
    if (!condition)
        fail(message);
}

// Product code lazily opens the developer's real AppData configuration
// whenever it finds storage uninitialized, and pinned windows persist their
// records and shortcut settings through that store.
void initializeIsolatedStorage(const QString& root) {
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    storage.shutdown();
    const snow_shot::storage::StorageInitializationOptions options{
        QDir(root).filePath(QStringLiteral("bin")),
        QDir(root).filePath(QStringLiteral("settings")),
        0,
    };
    require(storage.initialize(options).success,
            QStringLiteral("failed to initialize isolated benchmark storage"));
}

QImage makeImage(QSize size) {
    QImage image(size, QImage::Format_RGBA8888);
    require(!image.isNull(), QStringLiteral("could not allocate benchmark source image"));
    for (int y = 0; y < size.height(); ++y) {
        uchar* row = image.scanLine(y);
        for (int x = 0; x < size.width(); ++x) {
            row[x * 4] = uchar((x + y * 3) & 0xff);
            row[x * 4 + 1] = uchar((x * 5 + y) & 0xff);
            row[x * 4 + 2] = uchar((x * 3 + y * 7) & 0xff);
            row[x * 4 + 3] = 255;
        }
    }
    image.setColorSpace(QColorSpace::SRgb);
    return image;
}

QStringList writeFixtureFiles(const QString& name, const Scenario& scenario,
                              const QString& directory) {
    const QImage image = makeImage(scenario.imageSize);
    QStringList paths;
    for (int index = 0; index < scenario.fileCount; ++index) {
        const QString path = QDir(directory).filePath(
            QStringLiteral("%1-%2.%3")
                .arg(name, QString::number(index), QLatin1String(scenario.format)));
        require(image.save(path, scenario.format, scenario.quality),
                QStringLiteral("could not write fixture %1").arg(path));
        paths.append(path);
    }
    return paths;
}

void closePresentedWindows() {
    QVector<QPointer<ScreenshotPinnedWindow>> windows;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* window = qobject_cast<ScreenshotPinnedWindow*>(widget)) {
            windows.append(window);
        }
    }
    for (const QPointer<ScreenshotPinnedWindow>& window : windows) {
        if (window != nullptr) {
            window->close();
        }
    }
    QElapsedTimer drain;
    drain.start();
    while (drain.elapsed() < 5000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
        const bool remaining = std::any_of(
            windows.cbegin(), windows.cend(),
            [](const QPointer<ScreenshotPinnedWindow>& window) { return window != nullptr; });
        if (!remaining) {
            return;
        }
    }
    fail(QStringLiteral("presented windows were not closed between samples"));
}

Sample runSample(const Scenario& scenario, const QStringList& paths, QScreen& screen) {
    // A fresh services object per sample gives every variant an empty window
    // pool, so "noprewarm" truly pays cold shell construction on the first
    // present and "prewarm" truly benefits from the prebuilt spare.
    ScreenshotSelectionExportUiServices services;
    ScreenshotFilePinBatch batch;
    Sample sample;
    QElapsedTimer timer;
    const auto present = [&sample, &timer, &screen, &services](ScreenshotClipboardContent content) {
        if (sample.presents++ == 0) {
            sample.firstPinNanoseconds = timer.nsecsElapsed();
        }
        const auto fit = ScreenshotGeometryMapper::fitImageToAvailableGeometry(
            content.image.size(), screen.availableGeometry(), screen.geometry(),
            ScreenshotGeometryMapper::physicalRectForScreen(screen), 16);
        require(fit.valid, QStringLiteral("pinned geometry fit failed"));
        static_cast<void>(services.presentPinnedImage(content.image, &screen, fit.nativeGeometry,
                                                      fit.fullResolutionSize, {}, {}, 1.0,
                                                      std::move(content.originalContent)));
        return true;
    };
    timer.start();
    batch.start(paths, present);
    if (scenario.prewarm) {
        services.prewarmPinnedWindow(&screen);
    }
    QElapsedTimer timeout;
    timeout.start();
    while (batch.active() && timeout.elapsed() < 120000) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(1);
    }
    require(!batch.active(), QStringLiteral("the file-pin batch timed out"));
    sample.totalNanoseconds = timer.nsecsElapsed();
    require(sample.presents == scenario.fileCount,
            QStringLiteral("expected %1 pinned files, presented %2")
                .arg(scenario.fileCount)
                .arg(sample.presents));
    closePresentedWindows();
    return sample;
}

qint64 median(std::vector<qint64> values) {
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double milliseconds(qint64 nanoseconds) {
    return double(nanoseconds) / 1'000'000.0;
}

void report(const Scenario& scenario, const std::vector<Sample>& samples) {
    const auto values = [&](const auto& member) {
        std::vector<qint64> result;
        result.reserve(samples.size());
        for (const Sample& sample : samples)
            result.push_back(member(sample));
        return result;
    };
    std::cout << std::left << std::setw(11) << scenario.name << ' '
              << (scenario.prewarm ? "prewarm  " : "noprewarm") << std::fixed
              << std::setprecision(2) << " files=" << scenario.fileCount << " first_pin_ms="
              << milliseconds(
                     median(values([](const Sample& value) { return value.firstPinNanoseconds; })))
              << " total_ms=" << milliseconds(median(values([](const Sample& value) {
                     return value.totalNanoseconds;
                 })))
              << " presents="
              << median(values([](const Sample& value) { return qint64(value.presents); })) << '\n';
}
} // namespace

int main(int argc, char* argv[]) {
#if defined(_DEBUG)
    std::cerr << "This benchmark must be built in Release mode\n";
    return 2;
#endif
    QApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    QCommandLineOption samplesOption(QStringLiteral("samples"),
                                     QStringLiteral("Samples per scenario (median is reported)."),
                                     QStringLiteral("count"), QStringLiteral("5"));
    parser.addOption(samplesOption);
    parser.process(application);
    bool validSamples = false;
    const int sampleCount = parser.value(samplesOption).toInt(&validSamples);
    if (!validSamples || sampleCount < 1 || sampleCount > 20) {
        std::cerr << "--samples must be between 1 and 20\n";
        return 2;
    }

    const std::vector<Scenario> scenarios{
        {"jpeg-large", 8, QSize(4000, 3000), "jpg", 85, true},
        {"jpeg-large", 8, QSize(4000, 3000), "jpg", 85, false},
        {"png-small", 24, QSize(320, 240), "png", -1, true},
        {"png-small", 24, QSize(320, 240), "png", -1, false},
    };

    try {
        QScreen* screen = QGuiApplication::primaryScreen();
        require(screen != nullptr, QStringLiteral("no screen is available for pinning"));
        QTemporaryDir storageDirectory;
        require(storageDirectory.isValid(),
                QStringLiteral("temporary storage directory unavailable"));
        initializeIsolatedStorage(storageDirectory.path());
        QTemporaryDir fixtureDirectory;
        require(fixtureDirectory.isValid(),
                QStringLiteral("temporary fixture directory unavailable"));
        std::cout << "File-pin batch medians, samples=" << sampleCount << '\n';
        QString fixtureName;
        QStringList fixturePaths;
        for (const Scenario& scenario : scenarios) {
            if (scenario.name != fixtureName) {
                fixturePaths = writeFixtureFiles(scenario.name, scenario, fixtureDirectory.path());
                fixtureName = scenario.name;
            }
            std::vector<Sample> samples;
            samples.reserve(size_t(sampleCount));
            for (int sample = 0; sample < sampleCount; ++sample)
                samples.push_back(runSample(scenario, fixturePaths, *screen));
            report(scenario, samples);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
