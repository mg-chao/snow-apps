#include "app/application_setup.h"
#include "decoding/image_loader.h"
#include "ui/viewer_window.h"
#include "widgets/platform_compatibility.h"

#include <snow/image/service.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QSaveFile>

#include <memory>

namespace {

bool parsePerformanceFormat(const QString& name, snow::image::Format* format, bool* lossless) {
    *lossless = false;
    if (name.isEmpty()) {
        *format = snow::image::Format::unknown;
        return true;
    }
    if (name == QStringLiteral("png"))
        *format = snow::image::Format::png;
    else if (name == QStringLiteral("jpeg"))
        *format = snow::image::Format::jpeg;
    else if (name == QStringLiteral("webp-lossy"))
        *format = snow::image::Format::webp;
    else if (name == QStringLiteral("webp-lossless")) {
        *format = snow::image::Format::webp;
        *lossless = true;
    } else if (name == QStringLiteral("avif"))
        *format = snow::image::Format::avif;
    else if (name == QStringLiteral("heif"))
        *format = snow::image::Format::heif;
    else if (name == QStringLiteral("jxl"))
        *format = snow::image::Format::jxl;
    else
        return false;
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    QElapsedTimer applicationTimer;
    applicationTimer.start();
    QApplication app(argc, argv);
    adqt::widgets::initializePlatformCompatibility(app);
    snow::image_viewer::configureViewerApplicationIdentity(app);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Color-managed QRhi image viewer"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption editPerformanceOption(
        QStringLiteral("edit-performance-test"),
        QStringLiteral("Load the image, enter edit mode, record the first preview, and exit."));
    const QCommandLineOption editPerformanceOutputOption(
        QStringLiteral("edit-performance-output"),
        QStringLiteral("Write the edit performance JSON report to this file."),
        QStringLiteral("path"));
    const QCommandLineOption editPerformanceIterationsOption(
        QStringLiteral("edit-performance-iterations"),
        QStringLiteral("Number of measured edit requests to run."), QStringLiteral("count"),
        QStringLiteral("10"));
    const QCommandLineOption editPerformanceWarmupsOption(
        QStringLiteral("edit-performance-warmups"),
        QStringLiteral("Number of unmeasured warmup edit requests to run."),
        QStringLiteral("count"), QStringLiteral("5"));
    const QCommandLineOption editPerformanceFormatOption(
        QStringLiteral("edit-performance-format"),
        QStringLiteral(
            "Benchmark one encoder: png, jpeg, webp-lossy, webp-lossless, avif, heif, or jxl."),
        QStringLiteral("format"));
    const QCommandLineOption editPerformanceScaleOption(
        QStringLiteral("edit-performance-scale"),
        QStringLiteral("Benchmark scale factor, such as 1, 0.5, or 0.125."),
        QStringLiteral("factor"), QStringLiteral("1"));
    const QCommandLineOption editPerformanceCpuOption(
        QStringLiteral("edit-performance-cpu"),
        QStringLiteral("Force the CPU reference raster pipeline."));
    const QCommandLineOption editPerformanceRapidOption(
        QStringLiteral("edit-performance-rapid-superseding"),
        QStringLiteral("Benchmark rapid superseding requests and worker restarts."));
    const QCommandLineOption editPerformanceMetadataOption(
        QStringLiteral("edit-performance-preserve-metadata"),
        QStringLiteral("Preserve encoder-supported metadata during the benchmark."));
    parser.addOption(editPerformanceOption);
    parser.addOption(editPerformanceOutputOption);
    parser.addOption(editPerformanceIterationsOption);
    parser.addOption(editPerformanceWarmupsOption);
    parser.addOption(editPerformanceFormatOption);
    parser.addOption(editPerformanceScaleOption);
    parser.addOption(editPerformanceCpuOption);
    parser.addOption(editPerformanceRapidOption);
    parser.addOption(editPerformanceMetadataOption);
    parser.addPositionalArgument(QStringLiteral("image"), QStringLiteral("Image to open."),
                                 QStringLiteral("[image]"));
    parser.process(app);

    const QStringList positionalArguments = parser.positionalArguments();
    const QString requestedPath =
        positionalArguments.isEmpty() ? QString() : positionalArguments.constFirst();
    const QFileInfo requestedInfo(requestedPath);
    const QString initialImagePath =
        requestedInfo.isFile() ? requestedInfo.absoluteFilePath() : QString();
    const bool runEditPerformanceTest = parser.isSet(editPerformanceOption);
    bool iterationsValid = false;
    const int editPerformanceIterations =
        parser.value(editPerformanceIterationsOption).toInt(&iterationsValid);
    bool warmupsValid = false;
    const int editPerformanceWarmups =
        parser.value(editPerformanceWarmupsOption).toInt(&warmupsValid);
    bool scaleValid = false;
    const double editPerformanceScale =
        parser.value(editPerformanceScaleOption).toDouble(&scaleValid);
    snow::image::Format editPerformanceFormat = snow::image::Format::unknown;
    bool editPerformanceLossless = false;
    const bool formatValid =
        parsePerformanceFormat(parser.value(editPerformanceFormatOption).trimmed().toLower(),
                               &editPerformanceFormat, &editPerformanceLossless);
    if (runEditPerformanceTest && initialImagePath.isEmpty()) {
        qCritical("--edit-performance-test requires an existing image path.");
        return 2;
    }
    if (runEditPerformanceTest &&
        (!iterationsValid || editPerformanceIterations < 1 || editPerformanceIterations > 1000)) {
        qCritical("--edit-performance-iterations must be between 1 and 1000.");
        return 2;
    }
    if (runEditPerformanceTest &&
        (!warmupsValid || editPerformanceWarmups < 1 || editPerformanceWarmups > 100)) {
        qCritical("--edit-performance-warmups must be between 1 and 100.");
        return 2;
    }
    if (runEditPerformanceTest &&
        (!scaleValid || editPerformanceScale <= 0.0 || editPerformanceScale > 16.0)) {
        qCritical("--edit-performance-scale must be greater than 0 and at most 16.");
        return 2;
    }
    if (runEditPerformanceTest && !formatValid) {
        qCritical("--edit-performance-format is not recognized.");
        return 2;
    }
    if (runEditPerformanceTest && editPerformanceFormat != snow::image::Format::unknown &&
        !snow::image::Service().encoder_info(editPerformanceFormat)) {
        qCritical("The requested edit-performance encoder is unavailable in this build.");
        return 3;
    }

    std::unique_ptr<snow::image_viewer::ImageLoader> startupLoader;
    if (!initialImagePath.isEmpty()) {
        startupLoader = std::make_unique<snow::image_viewer::ImageLoader>();
        startupLoader->load(initialImagePath, snow::image_viewer::kSystemThumbnailMaximumExtent);
    }

    snow::image_viewer::configureViewerApplicationAppearance(app);
    std::unique_ptr<snow::image_viewer::ViewerWindow> window =
        startupLoader
            ? std::make_unique<snow::image_viewer::ViewerWindow>(*startupLoader, initialImagePath)
            : std::make_unique<snow::image_viewer::ViewerWindow>();
    if (!requestedPath.isEmpty() && initialImagePath.isEmpty()) {
        window->openImage(requestedPath);
    }
    window->show();
    if (runEditPerformanceTest) {
        QString outputPath = parser.value(editPerformanceOutputOption);
        if (outputPath.isEmpty()) {
            outputPath = QDir::current().filePath(QStringLiteral("edit-mode-performance.json"));
        }
        QObject::connect(window.get(),
                         &snow::image_viewer::ViewerWindow::editModePerformanceTestFinished, &app,
                         [outputPath](bool succeeded, const QByteArray& report) {
                             QSaveFile output(outputPath);
                             if (!output.open(QIODevice::WriteOnly) ||
                                 output.write(report) != report.size() || !output.commit()) {
                                 qCritical("Could not write edit performance report: %s",
                                           qPrintable(output.errorString()));
                                 QApplication::exit(2);
                                 return;
                             }
                             qInfo("Edit performance report: %s",
                                   qPrintable(QFileInfo(outputPath).absoluteFilePath()));
                             QApplication::exit(succeeded ? 0 : 1);
                         });
        snow::image_viewer::EditPerformanceOptions options;
        options.iterations = editPerformanceIterations;
        options.warmupIterations = editPerformanceWarmups;
        options.scale = editPerformanceScale;
        options.format = editPerformanceFormat;
        options.lossless = editPerformanceLossless;
        options.preserveMetadata = parser.isSet(editPerformanceMetadataOption);
        options.forceCpu = parser.isSet(editPerformanceCpuOption);
        options.rapidSuperseding = parser.isSet(editPerformanceRapidOption);
        window->startEditModePerformanceTest(applicationTimer.nsecsElapsed(), options);
    }
    return QApplication::exec();
}
