#include "scrolling_image_replay.h"
#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"
#include "snowimageqtcodec.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
using namespace snow_shot::scrolling_benchmark;
namespace perf = snow_shot::capture_detail::scrolling_perf;

QString text(const char* source) {
    return QCoreApplication::translate("ScrollingImageBenchmark", source);
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(text(message).toStdString());
}
int integerOption(const QCommandLineParser& parser, const char* name) {
    bool valid = false;
    const int value = parser.value(QString::fromLatin1(name)).toInt(&valid);
    require(valid, "An integer option is invalid.");
    return value;
}
void writeFile(const QString& path, const QByteArray& bytes) {
    QSaveFile file(path);
    require(!bytes.isEmpty() && file.open(QIODevice::WriteOnly) &&
                file.write(bytes) == bytes.size() && file.commit(),
            "Could not write an artifact.");
}
QJsonObject dimensions(QSize size) {
    return {{QStringLiteral("width"), size.width()}, {QStringLiteral("height"), size.height()}};
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({{{QStringLiteral("image")},
                        text("Input PNG image."),
                        QStringLiteral("path"),
                        QStringLiteral(SNOW_SCROLLING_DEFAULT_IMAGE)},
                       {{QStringLiteral("viewport-height")},
                        text("Viewport height in pixels."),
                        QStringLiteral("pixels"),
                        QStringLiteral("1600")},
                       {{QStringLiteral("step-px")},
                        text("Vertical movement per scroll step."),
                        QStringLiteral("pixels"),
                        QStringLiteral("25")},
                       {{QStringLiteral("scroll-fps")},
                        text("Source scroll steps per second."),
                        QStringLiteral("rate"),
                        QStringLiteral("30")},
                       {{QStringLiteral("max-steps")},
                        text("Maximum movements; -1 traverses the image."),
                        QStringLiteral("count"),
                        QStringLiteral("-1")},
                       {{QStringLiteral("output-dir")},
                        text("Benchmark artifact directory."),
                        QStringLiteral("path"),
                        QStringLiteral(SNOW_SCROLLING_DEFAULT_OUTPUT)}});
    parser.process(application);
    const QString output = QFileInfo(parser.value(QStringLiteral("output-dir"))).absoluteFilePath();
    QJsonObject report{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("timing_unit"), QStringLiteral("nanoseconds")},
        {QStringLiteral("nested_timings"), QStringLiteral("inclusive")},
        {QStringLiteral("build_config"), QStringLiteral(SNOW_SCROLLING_BUILD_CONFIG)},
        {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
        {QStringLiteral("cpu_architecture"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("platform"), QGuiApplication::platformName()},
        {QStringLiteral("created_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
        {QStringLiteral("detailed_timing"), true},
#else
        {QStringLiteral("detailed_timing"), false},
#endif
        {QStringLiteral("success"), false}};
    QJsonObject initialization;
    QString error;
    std::unique_ptr<ReplayState> replay;
    QImage painted;
    QImage preview;
    QSize outputSize;
    qint64 finishedAt = 0;
    const auto runStarted = perf::now();
    try {
        require(QStringLiteral(SNOW_SCROLLING_BUILD_CONFIG) == QStringLiteral("Release"),
                "Run this benchmark with the windows-msvc-performance Release preset.");
        require(QDir().mkpath(output), "Could not create the output directory.");
        const QString inputPath =
            QFileInfo(parser.value(QStringLiteral("image"))).absoluteFilePath();
        const int height = integerOption(parser, "viewport-height");
        const int step = integerOption(parser, "step-px");
        const int maximumSteps = integerOption(parser, "max-steps");
        bool rateValid = false;
        const double fps = parser.value(QStringLiteral("scroll-fps")).toDouble(&rateValid);
        require(rateValid, "The scroll rate is invalid.");
        report[QStringLiteral("configuration")] =
            QJsonObject{{QStringLiteral("image"), inputPath},
                        {QStringLiteral("viewport_height"), height},
                        {QStringLiteral("step_px"), step},
                        {QStringLiteral("scroll_fps"), fps},
                        {QStringLiteral("max_steps"), maximumSteps},
                        {QStringLiteral("capture_min_fps"), 1},
                        {QStringLiteral("capture_max_fps"), 30}};
        QFile file(inputPath);
        auto started = perf::now();
        require(file.open(QIODevice::ReadOnly), "Could not open the input image.");
        QByteArray encoded = file.readAll();
        require(file.error() == QFileDevice::NoError && !encoded.isEmpty(),
                "Could not read the input image.");
        file.close();
        initialization[QStringLiteral("file_read_ns")] = perf::now() - started;
        started = perf::now();
        require(readablePng(encoded), "The input is not a readable PNG image.");
        initialization[QStringLiteral("png_validation_ns")] = perf::now() - started;
        started = perf::now();
        QImage image =
            snow_shot::image_codec::decode(encoded, snow::image::Format::png, "replay.png");
        initialization[QStringLiteral("decode_ns")] = perf::now() - started;
        encoded.clear();
        encoded.squeeze();
        require(!image.isNull(), "Could not decode the input PNG image.");
        started = perf::now();
        image = image.convertToFormat(QImage::Format_RGBA8888);
        initialization[QStringLiteral("rgba_normalization_ns")] = perf::now() - started;
        report[QStringLiteral("source_size")] = dimensions(image.size());
        replay = std::make_unique<ReplayState>(std::move(image), height, step, fps, maximumSteps);
        report[QStringLiteral("movements")] = replay->schedule.movements();
        report[QStringLiteral("scheduled_motion_ns")] = replay->schedule.durationNs();

        QWidget parent;
        parent.setAttribute(Qt::WA_DontShowOnScreen);
        ScreenshotScrollingThumbnailWidget thumbnail(parent);
        thumbnail.setMaximumPreviewExtent(640);
        QEventLoop loop;
        bool initialPainted = false;
        bool finalObserved = false;
        bool inputStopped = false;
        auto setupStart = perf::now();
        std::unique_ptr<ScreenshotScrollingPipeline> pipeline;
        pipeline = std::make_unique<ScreenshotScrollingPipeline>(
            [&](ScrollingPipelineFrame frame) {
                if (frame.fatalError) {
                    error = text("The scrolling stitching worker failed.");
                    loop.quit();
                    return;
                }
                if (frame.changed) {
#if defined(SNOW_SHOT_SCROLLING_PERF_DETAIL)
                    perf::TraceContext context(frame.trace);
#endif
                    {
                        perf::Scope scope(frame.trace, perf::Stage::ThumbnailUpdate);
                        thumbnail.setStitchedImage(
                            frame.previewImage, frame.sourceSize, frame.change, frame.addedRows,
                            frame.previewReplaced, frame.replacedPreviewRows);
                    }
                    {
                        perf::Scope scope(frame.trace, perf::Stage::ThumbnailPaint);
                        if (painted.size() != thumbnail.size()) {
                            painted = QImage(thumbnail.size(), QImage::Format_ARGB32_Premultiplied);
                        }
                        if (painted.isNull()) {
                            error = text("Could not allocate the thumbnail paint surface.");
                            loop.quit();
                            return;
                        }
                        painted.fill(Qt::transparent);
                        thumbnail.render(&painted);
                    }
                    frame.trace->paintedAt = perf::now();
                    frame.trace->record(perf::Stage::CaptureToPaint,
                                        frame.trace->paintedAt - frame.trace->capturedAt);
                    frame.trace->record(perf::Stage::MotionToPaint,
                                        frame.trace->paintedAt - frame.trace->motionAt);
                    frame.trace->disposition = "painted";
                    outputSize = frame.sourceSize;
                    if (!initialPainted) {
                        initialPainted = true;
                        initialization[QStringLiteral("first_thumbnail_ns")] =
                            perf::now() - setupStart;
                        replay->startMotion(perf::now());
                    }
                }
                if (initialPainted && frame.trace->offset == replay->schedule.finalOffset() &&
                    !finalObserved) {
                    finalObserved = true;
                    pipeline->finishInput([&]() { inputStopped = true; });
                }
            },
            [&](quint64, QString message) {
                error = std::move(message);
                loop.quit();
            });
        pipeline->begin(1, QSize(replay->image.width(), height),
                        ScreenshotScrollingRecognitionMode::Vertical, replay->factory());
        initialization[QStringLiteral("pipeline_setup_ns")] = perf::now() - setupStart;
        QTimer watchdog;
        QObject::connect(&watchdog, &QTimer::timeout, &loop, [&]() {
            const auto now = perf::now();
            const auto deadline = initialPainted ? replay->endTime() : setupStart;
            const auto completion =
                replayCompletion(now, deadline, finalObserved, inputStopped, pipeline->idle());
            if (completion == ReplayCompletion::TimedOut) {
                error = text("Timed out while waiting for scrolling output.");
                loop.quit();
            } else if (completion == ReplayCompletion::Complete) {
                loop.quit();
            }
        });
        watchdog.start(5);
        loop.exec();
        watchdog.stop();
        finishedAt = perf::now();
        report[QStringLiteral("drain_ns")] =
            initialPainted ? std::max(qint64{0}, finishedAt - replay->endTime()) : 0;
        started = perf::now();
        pipeline.reset();
        report[QStringLiteral("teardown_ns")] = perf::now() - started;
        preview = thumbnail.previewImageForTesting();
        for (const auto* widget : QApplication::topLevelWidgets()) {
            require(!widget->isVisible(), "A top-level widget became visible.");
        }
        const QSize expected(replay->image.width(), height + replay->schedule.finalOffset());
        const QSize expectedPreview(
            128, qCeil(static_cast<double>(expected.height()) * 128 / expected.width()));
        report[QStringLiteral("expected_output_size")] = dimensions(expected);
        report[QStringLiteral("output_size")] = dimensions(outputSize);
        report[QStringLiteral("preview_size")] = dimensions(preview.size());
        report[QStringLiteral("thumbnail_size")] = dimensions(painted.size());
        report[QStringLiteral("preview_checksum")] = QString::number(imageChecksum(preview));
        report[QStringLiteral("thumbnail_checksum")] = QString::number(imageChecksum(painted));
        report[QStringLiteral("coverage_complete")] = outputSize == expected;
        report[QStringLiteral("preview_logical_bytes")] = thumbnail.previewLogicalBytesForTesting();
        report[QStringLiteral("preview_allocated_bytes")] =
            thumbnail.previewAllocatedBytesForTesting();
        require(error.isEmpty(), "The scrolling replay did not complete.");
        require(initialPainted && finalObserved && !painted.isNull() && !preview.isNull(),
                "The scrolling replay did not produce a final thumbnail.");
        require(outputSize == expected && preview.size() == expectedPreview,
                "The stitched output does not cover the complete replay image.");
        report[QStringLiteral("success")] = true;
    } catch (const std::exception& failure) {
        if (error.isEmpty())
            error = QString::fromUtf8(failure.what());
    }
    if (finishedAt == 0)
        finishedAt = perf::now();
    if (replay)
        report[QStringLiteral("replay")] = replayReport(*replay, finishedAt);
    report[QStringLiteral("initialization")] = initialization;
    report[QStringLiteral("error")] = error;
    report[QStringLiteral("run_before_artifacts_ns")] = perf::now() - runStarted;
    try {
        const auto started = perf::now();
        if (!painted.isNull())
            writeFile(QDir(output).filePath(QStringLiteral("thumbnail.png")),
                      snow_shot::image_codec::encodePng(painted));
        if (!preview.isNull())
            writeFile(QDir(output).filePath(QStringLiteral("preview.png")),
                      snow_shot::image_codec::encodePng(preview));
        report[QStringLiteral("image_artifacts_ns")] = perf::now() - started;
    } catch (const std::exception& failure) {
        error = QString::fromUtf8(failure.what());
        report[QStringLiteral("success")] = false;
        report[QStringLiteral("error")] = error;
    }
    try {
        writeFile(QDir(output).filePath(QStringLiteral("results.json")),
                  QJsonDocument(report).toJson(QJsonDocument::Indented));
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
    std::cout << "Scrolling replay: "
              << (report[QStringLiteral("success")].toBool() ? "complete" : "failed")
              << "\nArtifacts: " << output.toStdString() << '\n';
    const auto stages =
        report[QStringLiteral("replay")].toObject()[QStringLiteral("stages")].toObject();
    for (auto it = stages.begin(); it != stages.end(); ++it) {
        const auto summary = it.value().toObject();
        if (summary[QStringLiteral("count")].toInteger() == 0)
            continue;
        std::cout << it.key().toStdString()
                  << ": p50=" << summary[QStringLiteral("p50_ns")].toDouble() / 1'000'000.0
                  << " ms, p95=" << summary[QStringLiteral("p95_ns")].toDouble() / 1'000'000.0
                  << " ms, max=" << summary[QStringLiteral("max_ns")].toDouble() / 1'000'000.0
                  << " ms\n";
    }
    if (!error.isEmpty())
        std::cerr << error.toStdString() << '\n';
    return report[QStringLiteral("success")].toBool() ? 0 : 1;
}
