#include "screenshot_selection_presentation_fixture.h"
#include "snow_shot/presentation/screenshotpinnedwindow.h"
#include "snow_shot/runtime/memoryoptimizationcontroller.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSysInfo>
#include <qscopeguard.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
QImage renderCanvas(SnowCanvasWidget& canvas, ScreenshotCanvasRenderer& renderer) {
    const qreal dpr = canvas.devicePixelRatioF();
    const QSize physicalSize(static_cast<int>(std::ceil(canvas.width() * dpr)),
                             static_cast<int>(std::ceil(canvas.height() * dpr)));
    QImage image(physicalSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    const SnowCanvasRenderContext context{canvas.rect(), canvas.rect(),
                                          canvas.canvasToViewTransform(), dpr};
    renderer.renderBeforeCanvas(painter, context);
    renderer.renderAfterCanvas(painter, context);
    painter.end();
    return image;
}
} // namespace

// Read-only fixture access keeps the benchmark on the production renderer and retained source.
class ScreenshotPinnedWindowTestAccess final {
  public:
    static const QImage& image(const ScreenshotPinnedWindow& window) {
        return window.m_originalImage;
    }
    static QImage render(ScreenshotPinnedWindow& window) {
        return renderCanvas(*window.m_canvas, *window.m_screenshotRenderer);
    }
};

namespace {
namespace runtime = snow_shot::runtime;
constexpr std::size_t kBallastBytes = 64U * 1024U * 1024U;
constexpr std::uint64_t kChecksumSeed = 1469598103934665603ULL;
constexpr std::uint64_t kChecksumPrime = 1099511628211ULL;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void drainEvents() {
    for (int turn = 0; turn < 4; ++turn) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
        QApplication::processEvents();
    }
}

QByteArray rasterHash(const QImage& image) {
    require(!image.isNull(), "the fixture must render an image");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qsizetype rowBytes = static_cast<qsizetype>(image.width()) * image.depth() / 8;
    for (int row = 0; row < image.height(); ++row)
        hash.addData(
            QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(row)), rowBytes));
    return hash.result().toHex();
}

std::uint64_t checksum(const std::vector<std::uint64_t>& allocation) {
    const volatile std::uint64_t* data = allocation.data();
    std::uint64_t value = kChecksumSeed;
    for (std::size_t index = 0; index < allocation.size(); ++index)
        value = (value ^ data[index]) * kChecksumPrime;
    return value;
}

QJsonObject memoryJson(const runtime::ProcessMemorySample& sample) {
    return {{QStringLiteral("private_working_set_bytes"),
             static_cast<qint64>(sample.privateWorkingSetBytes)},
            {QStringLiteral("page_fault_count"), static_cast<qint64>(sample.pageFaultCount)}};
}

runtime::ProcessMemorySample sample(const runtime::MemoryOptimizationController::Options& options) {
    const auto result = options.sample();
    require(result.has_value(), "the native benchmark memory sample must succeed");
    return *result;
}

QJsonObject distribution(std::vector<double> values) {
    require(!values.empty(), "the benchmark distribution must contain samples");
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        const auto index =
            static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size())));
        return values[std::clamp(index, std::size_t{1}, values.size()) - 1];
    };
    return {{QStringLiteral("p50"), percentile(0.50)},
            {QStringLiteral("p95"), percentile(0.95)},
            {QStringLiteral("min"), values.front()},
            {QStringLiteral("max"), values.back()}};
}

struct Scenario {
    std::function<void()> perform;
    std::function<QImage()> render;
    std::function<bool()> validate;
    std::function<void()> cleanup;
};

QJsonObject runComparison(const QString& name, int pairs, int warmups, Scenario scenario,
                          const runtime::MemoryOptimizationController::Options& options) {
    std::vector<std::uint64_t> ballast(kBallastBytes / sizeof(std::uint64_t));
    for (std::size_t index = 0; index < ballast.size(); ++index)
        ballast[index] = static_cast<std::uint64_t>(index) * 0x9e3779b97f4a7c15ULL + 17U;
    const auto expectedChecksum = checksum(ballast);
    QByteArray expectedRaster;
    for (int warmup = 0; warmup < warmups; ++warmup) {
        scenario.perform();
        require(scenario.validate(), "the warm fixture must preserve functionality");
        const QByteArray hash = rasterHash(scenario.render());
        if (expectedRaster.isEmpty())
            expectedRaster = hash;
        require(hash == expectedRaster, "warm fixture rendering must be deterministic");
        scenario.cleanup();
        drainEvents();
    }
    QJsonArray raw;
    for (int pair = 0; pair < pairs; ++pair) {
        for (int position = 0; position < 2; ++position) {
            const bool trim = (pair % 2 == 0) == (position == 0);
            // Each measurement begins with the same operation and touched retained memory.
            // This prevents a preceding trimmed run from making the control run accidentally cold.
            {
                scenario.perform();
                require(scenario.validate() && rasterHash(scenario.render()) == expectedRaster,
                        "rewarming must preserve the fixture rendering");
            }
            scenario.cleanup();
            drainEvents();
            require(checksum(ballast) == expectedChecksum, "rewarm the retained 64 MiB allocation");
            const auto before = sample(options);
            const auto trimStarted = std::chrono::steady_clock::now();
            runtime::MemoryTrimResult result{true, 0};
            if (trim)
                result = options.trim();
            const double trimMs = trim ? std::chrono::duration<double, std::milli>(
                                             std::chrono::steady_clock::now() - trimStarted)
                                             .count()
                                       : 0.0;
            const auto afterTrim = sample(options);
            require(result.succeeded, "the controlled native working-set trim must succeed");
            QElapsedTimer operation;
            operation.start();
            scenario.perform();
            const double operationMs = static_cast<double>(operation.nsecsElapsed()) / 1'000'000.0;
            const auto afterOperation = sample(options);
            // Samples and latency precede the checksum and verification raster allocation/refaults.
            const bool integrity = checksum(ballast) == expectedChecksum;
            const bool functional = scenario.validate();
            const bool renderCorrect = rasterHash(scenario.render()) == expectedRaster;
            require(integrity && functional && renderCorrect,
                    "trimming must preserve allocation bytes and screenshot/pin rendering");
            raw.append(QJsonObject{
                {QStringLiteral("pair"), pair},
                {QStringLiteral("position"), position},
                {QStringLiteral("trimmed"), trim},
                {QStringLiteral("before"), memoryJson(before)},
                {QStringLiteral("after_trim_before_operation"), memoryJson(afterTrim)},
                {QStringLiteral("after_operation_before_checksum"), memoryJson(afterOperation)},
                {QStringLiteral("trim_ms"), trimMs},
                {QStringLiteral("next_operation_ms"), operationMs},
                {QStringLiteral("trim_and_operation_ms"), trimMs + operationMs},
                {QStringLiteral("trim_page_faults"),
                 static_cast<qint64>(afterTrim.pageFaultCount - before.pageFaultCount)},
                {QStringLiteral("next_operation_page_faults"),
                 static_cast<qint64>(afterOperation.pageFaultCount - afterTrim.pageFaultCount)},
                {QStringLiteral("private_working_set_reduction_bytes"),
                 static_cast<qint64>(before.privateWorkingSetBytes) -
                     static_cast<qint64>(afterTrim.privateWorkingSetBytes)},
                {QStringLiteral("checksum_ok"), integrity},
                {QStringLiteral("functional_ok"), functional},
                {QStringLiteral("render_ok"), renderCorrect},
            });
            scenario.cleanup();
            drainEvents();
        }
    }
    QJsonArray summaries;
    for (bool trimmed : {false, true}) {
        QJsonObject metrics;
        for (const QString& key :
             {QStringLiteral("trim_ms"), QStringLiteral("next_operation_ms"),
              QStringLiteral("trim_and_operation_ms"), QStringLiteral("next_operation_page_faults"),
              QStringLiteral("private_working_set_reduction_bytes")}) {
            std::vector<double> values;
            for (const auto& entry : raw) {
                const auto row = entry.toObject();
                if (row.value(QStringLiteral("trimmed")).toBool() == trimmed)
                    values.push_back(row.value(key).toDouble());
            }
            metrics.insert(key, distribution(std::move(values)));
        }
        summaries.append(QJsonObject{{QStringLiteral("trimmed"), trimmed},
                                     {QStringLiteral("samples"), pairs},
                                     {QStringLiteral("metrics"), metrics}});
    }
    return {{QStringLiteral("scenario"), name},
            {QStringLiteral("ballast_bytes"), static_cast<qint64>(kBallastBytes)},
            {QStringLiteral("expected_raster_sha256"), QString::fromLatin1(expectedRaster)},
            {QStringLiteral("summaries"), summaries},
            {QStringLiteral("samples"), raw}};
}

QImage sourceImage() {
    QImage image(QSize(2560, 1440), QImage::Format_RGB32);
    require(!image.isNull(), "allocate the retained pin source image");
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            row[x] = qRgb((x * 17 + y * 3) & 255, (x * 5 + y * 19) & 255, (x * 11 + y * 7) & 255);
    }
    return image;
}

QJsonObject benchmark(QApplication& application) {
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("pairs"), QStringLiteral("Pairs per scenario (2 to 100)"),
                      QStringLiteral("count"), QStringLiteral("20")});
    parser.addOption({QStringLiteral("warmups"),
                      QStringLiteral("Warmup operations per scenario (1 to 10)"),
                      QStringLiteral("count"), QStringLiteral("2")});
    parser.addOption(
        {QStringLiteral("output"), QStringLiteral("JSON result file"), QStringLiteral("path")});
    parser.process(application);
    bool validPairs = false;
    bool validWarmups = false;
    const int pairs = parser.value(QStringLiteral("pairs")).toInt(&validPairs);
    const int warmups = parser.value(QStringLiteral("warmups")).toInt(&validWarmups);
    require(validPairs && validWarmups && pairs >= 2 && pairs <= 100 && warmups >= 1 &&
                warmups <= 10,
            "invalid benchmark pair or warmup count");
    auto options = runtime::windowsMemoryOptimizationOptions();
    require(options.sample && options.trim,
            "the benchmark requires the production Windows memory backend");
    selection_presentation_test::IsolatedStorage storage;
    require(snow_shot::storage::PinToScreenSettings().setAutomaticTextRecognition(false),
            "disable recognition in the isolated benchmark fixture");
    QJsonArray scenarios;
    {
        selection_presentation_test::Fixture fixture(QSize(1200, 800), false, true);
        const QRectF selection = fixture.baseSelection().translated(4, 3);
        qint64 previousNotifications = 0;
        Scenario screenshot;
        screenshot.perform = [&] {
            previousNotifications = fixture.stateNotifications;
            // Reuse the widget pool and captured source, but advance the capture epoch so
            // every next-use measurement performs a real semantic presentation.
            ++fixture.captureState.sessionId;
            fixture.displays.startup->sessionId = fixture.captureState.sessionId;
            fixture.coordinator.applyDisplayModels(fixture.displays);
            fixture.coordinator.setCanvasInteractionEnabled(fixture.displays, true);
            fixture.requestSelection(selection);
            fixture.flushFrame();
            fixture.coordinator.showOverlayWindows(fixture.displays,
                                                   ScreenshotOverlayShowMode::CapturedImage);
            drainEvents();
        };
        screenshot.render = [&] {
            return renderCanvas(*fixture.overlay.canvas(),
                                *fixture.overlay.screenshotRendererForTesting());
        };
        screenshot.validate = [&] {
            return fixture.overlay.isVisible() && fixture.displayedSelection() == selection &&
                   fixture.stateNotifications > previousNotifications;
        };
        screenshot.cleanup = [&] {
            fixture.coordinator.hideOverlayWindowsImmediately(fixture.displays);
            fixture.coordinator.hideOverlayWindows(fixture.displays);
            fixture.coordinator.hideToolbar();
            if (auto* toolbar = fixture.coordinator.selectionToolbar())
                toolbar->hide();
        };
        screenshot.cleanup();
        drainEvents();
        scenarios.append(runComparison(QStringLiteral("screenshot_selection_presentation"), pairs,
                                       warmups, std::move(screenshot), options));
    }
    drainEvents();
    {
        auto* screen = QApplication::primaryScreen();
        require(screen != nullptr, "the pin fixture requires a Qt screen");
        const QImage image = sourceImage();
        const QByteArray sourceHash = rasterHash(image);
        const auto fitted = ScreenshotGeometryMapper::fitImageToAvailableGeometry(
            image.size(), screen->availableGeometry(), screen->geometry(),
            ScreenshotGeometryMapper::physicalRectForScreen(*screen), 16);
        require(fitted.valid, "fit the pin image to the fixture screen");
        ScreenshotPinnedWindow::Config config;
        config.screen = screen;
        config.nativeGeometry = fitted.nativeGeometry;
        config.initialWindowSize = fitted.initialWindowSize;
        config.canvasSourceRect = QRectF(QPointF(), image.size());
        config.imageSource = ScreenshotImageSource::fromImage(image, config.canvasSourceRect);
        config.automaticTextRecognition = false;
        config.initialBorderVisible = true;
        config.initialShadowVisible = false;
        std::unique_ptr<ScreenshotPinnedWindow> window;
        Scenario pin;
        pin.perform = [&] {
            window = std::make_unique<ScreenshotPinnedWindow>();
            require(window->present(config), "present the real pin fixture");
            QElapsedTimer timeout;
            timeout.start();
            while (!window->automationState().value(QStringLiteral("ready")).toBool() &&
                   timeout.elapsed() < 10000)
                drainEvents();
            require(window->automationState().value(QStringLiteral("ready")).toBool(),
                    "publish the pin's first content frame");
            drainEvents();
        };
        pin.render = [&] { return ScreenshotPinnedWindowTestAccess::render(*window); };
        pin.validate = [&] {
            return window && window->isVisible() &&
                   rasterHash(ScreenshotPinnedWindowTestAccess::image(*window)) == sourceHash;
        };
        pin.cleanup = [&] { window.reset(); };
        scenarios.append(runComparison(QStringLiteral("pin_image_presentation"), pairs, warmups,
                                       std::move(pin), options));
    }
    const QJsonObject report{
        {QStringLiteral("schema_version"), 2},
        {QStringLiteral("created_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {QStringLiteral("build_type"), QStringLiteral("Release")},
        {QStringLiteral("qt_platform"), QApplication::platformName()},
        {QStringLiteral("os"), QSysInfo::prettyProductName()},
        {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("pairs_per_scenario"), pairs},
        {QStringLiteral("warmups_per_scenario"), warmups},
        {QStringLiteral("sample_order"), QStringLiteral("AB then BA, each trial rewarmed")},
        {QStringLiteral("measurement"),
         QStringLiteral("Controlled next-use screenshot selection and pin presentation; not native "
                        "OS capture or admission scheduling")},
        {QStringLiteral("scenarios"), scenarios},
    };
    if (parser.isSet(QStringLiteral("output"))) {
        QFile output(parser.value(QStringLiteral("output")));
        require(output.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "open the benchmark JSON result file");
        const QByteArray bytes = QJsonDocument(report).toJson(QJsonDocument::Indented);
        require(output.write(bytes) == bytes.size(), "write the complete benchmark report");
    }
    return report;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    try {
#ifndef NDEBUG
        throw std::runtime_error(
            "memory optimization performance measurements require a Release build");
#endif
        const auto report = benchmark(application);
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
