#include "snow_stitch_images.h"
#include "snowimageqtcodec.h"

#include <snow/image/format.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QObject>
#include <QSize>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <vector>

namespace {
constexpr std::uint32_t kWidth = 3840;
constexpr std::uint32_t kHeight = 2160;
constexpr int kWarmups = 2;
constexpr int kRounds = 9;

using SharedSnapshot = std::shared_ptr<SnowStitchSnapshot>;

struct MaterializedResult {
    QSize size;
    std::uint64_t checksum = 0;
    qsizetype pngBytes = 0;
    bool valid = false;
};

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(1);
}

void releaseOwnedImage(void* context) {
    snow_stitch_owned_image_destroy(static_cast<SnowStitchOwnedImage*>(context));
}

MaterializedResult materialize(const SharedSnapshot& snapshot) {
    SnowStitchOwnedImage* image = snow_stitch_snapshot_materialize(snapshot.get());
    if (image == nullptr) {
        return {};
    }
    SnowStitchImageInfo info{};
    if (snow_stitch_owned_image_info(image, &info) == 0 || info.rgba_bytes == nullptr ||
        info.width == 0 || info.height == 0 || info.stride_bytes != info.width * 4) {
        snow_stitch_owned_image_destroy(image);
        return {};
    }

    QImage output(info.rgba_bytes, static_cast<int>(info.width), static_cast<int>(info.height),
                  static_cast<int>(info.stride_bytes), QImage::Format_RGBA8888, &releaseOwnedImage,
                  image);
    if (output.isNull()) {
        snow_stitch_owned_image_destroy(image);
        return {};
    }

    std::uint64_t checksum = 1469598103934665603ULL;
    const auto* bytes = output.constBits();
    for (qsizetype index = 0; index < output.sizeInBytes(); ++index) {
        checksum ^= bytes[index];
        checksum *= 1099511628211ULL;
    }
    const QByteArray png = snow_shot::image_codec::encodePng(output);
    const QImage decoded =
        snow_shot::image_codec::decode(png, snow::image::Format::png, "benchmark.png");
    return {output.size(), checksum, png.size(),
            !png.isEmpty() && !decoded.isNull() && decoded.size() == output.size()};
}

SharedSnapshot makeSnapshot() {
    SnowStitchFramePool* pool = snow_stitch_frame_pool_create(kWidth, kHeight, 1);
    SnowStitchConfig config{};
    if (snow_stitch_config_default(&config) == 0) {
        fail("could not initialize stitch benchmark config");
    }
    SnowStitchSession* session = snow_stitch_session_create(&config);
    if (pool == nullptr || session == nullptr) {
        fail("could not create stitch benchmark state");
    }

    SnowStitchFrameBuffer* frame = snow_stitch_frame_pool_acquire(pool);
    SnowStitchMutableImageInfo info{};
    if (frame == nullptr || snow_stitch_frame_buffer_info(frame, &info) == 0) {
        fail("could not acquire stitch benchmark frame");
    }
    for (std::uint32_t y = 0; y < info.height; ++y) {
        for (std::uint32_t x = 0; x < info.width; ++x) {
            const size_t offset =
                static_cast<size_t>(y) * info.stride_bytes + static_cast<size_t>(x) * 4;
            info.rgba_bytes[offset] = static_cast<std::uint8_t>((x + y) & 0xff);
            info.rgba_bytes[offset + 1] = static_cast<std::uint8_t>((x * 3 + y * 5) & 0xff);
            info.rgba_bytes[offset + 2] = static_cast<std::uint8_t>((x * 7 + y * 11) & 0xff);
            info.rgba_bytes[offset + 3] = 255;
        }
    }

    SnowStitchFrameOutcome outcome{};
    if (snow_stitch_session_push_owned(session, &frame, &outcome) == 0 || frame != nullptr ||
        outcome.output_height != kHeight) {
        fail("could not seed stitch benchmark snapshot");
    }
    SnowStitchSnapshot* rawSnapshot =
        snow_stitch_session_snapshot(session, 0, outcome.output_height);
    snow_stitch_session_destroy(session);
    snow_stitch_frame_pool_destroy(pool);
    if (rawSnapshot == nullptr) {
        fail("could not create stitch benchmark snapshot");
    }
    return SharedSnapshot(rawSnapshot, &snow_stitch_snapshot_destroy);
}

double percentile95(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t index =
        static_cast<size_t>(std::ceil(static_cast<double>(values.size()) * 0.95)) - 1;
    return values[index];
}

void writeArtifact(const QJsonObject& artifact) {
    const QString path = qEnvironmentVariable("SNOW_SCROLLING_PERF_OUTPUT");
    if (path.isEmpty()) {
        return;
    }
    const QFileInfo info(path);
    QDir().mkpath(info.absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fail("could not create async result benchmark artifact");
    }
    file.write(QJsonDocument(artifact).toJson(QJsonDocument::Indented));
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const SharedSnapshot snapshot = makeSnapshot();

    for (int warmup = 0; warmup < kWarmups; ++warmup) {
        if (!materialize(snapshot).valid) {
            fail("synchronous materialization warmup failed");
        }
    }

    std::vector<double> synchronousMilliseconds;
    MaterializedResult synchronousResult;
    for (int round = 0; round < kRounds; ++round) {
        QElapsedTimer timer;
        timer.start();
        synchronousResult = materialize(snapshot);
        synchronousMilliseconds.push_back(static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0);
        if (!synchronousResult.valid) {
            fail("synchronous materialization failed");
        }
    }

    QThread workerThread;
    auto* worker = new QObject;
    worker->moveToThread(&workerThread);
    QObject::connect(&workerThread, &QThread::finished, worker, &QObject::deleteLater);
    workerThread.start();

    QElapsedTimer eventClock;
    eventClock.start();
    qint64 previousPulse = eventClock.nsecsElapsed();
    double maximumEventLoopGapMilliseconds = 0.0;
    QTimer pulse;
    pulse.setTimerType(Qt::PreciseTimer);
    pulse.setInterval(1);
    QObject::connect(&pulse, &QTimer::timeout, [&]() {
        const qint64 now = eventClock.nsecsElapsed();
        maximumEventLoopGapMilliseconds =
            std::max(maximumEventLoopGapMilliseconds,
                     static_cast<double>(now - previousPulse) / 1'000'000.0);
        previousPulse = now;
    });
    pulse.start();

    std::vector<double> schedulingMilliseconds;
    std::vector<double> asynchronousCompletionMilliseconds;
    MaterializedResult asynchronousResult;
    for (int round = -kWarmups; round < kRounds; ++round) {
        QEventLoop loop;
        QElapsedTimer completionTimer;
        completionTimer.start();
        QElapsedTimer schedulingTimer;
        schedulingTimer.start();
        const bool invoked = QMetaObject::invokeMethod(
            worker,
            [&application, &loop, &asynchronousResult, snapshot]() {
                MaterializedResult result = materialize(snapshot);
                static_cast<void>(QMetaObject::invokeMethod(
                    &application,
                    [&loop, &asynchronousResult, result]() {
                        asynchronousResult = result;
                        loop.quit();
                    },
                    Qt::QueuedConnection));
            },
            Qt::QueuedConnection);
        const double schedulingElapsed =
            static_cast<double>(schedulingTimer.nsecsElapsed()) / 1'000'000.0;
        if (!invoked) {
            fail("could not schedule asynchronous materialization");
        }
        loop.exec();
        if (!asynchronousResult.valid) {
            fail("asynchronous materialization failed");
        }
        if (round >= 0) {
            schedulingMilliseconds.push_back(schedulingElapsed);
            asynchronousCompletionMilliseconds.push_back(
                static_cast<double>(completionTimer.nsecsElapsed()) / 1'000'000.0);
        }
    }
    pulse.stop();
    workerThread.quit();
    workerThread.wait();

    const double synchronousP95 = percentile95(synchronousMilliseconds);
    const double schedulingP95 = percentile95(schedulingMilliseconds);
    const double asynchronousCompletionP95 = percentile95(asynchronousCompletionMilliseconds);
    const double callerBlockingReductionPercent = (1.0 - schedulingP95 / synchronousP95) * 100.0;
    const double completionChangePercent =
        (asynchronousCompletionP95 / synchronousP95 - 1.0) * 100.0;
    const bool checksumMatches = synchronousResult.checksum == asynchronousResult.checksum &&
                                 synchronousResult.size == asynchronousResult.size;

    const QJsonObject artifact{
        {QStringLiteral("width"), synchronousResult.size.width()},
        {QStringLiteral("height"), synchronousResult.size.height()},
        {QStringLiteral("rounds"), kRounds},
        {QStringLiteral("synchronous_caller_p95_ms"), synchronousP95},
        {QStringLiteral("scheduling_p95_ms"), schedulingP95},
        {QStringLiteral("caller_blocking_reduction_percent"), callerBlockingReductionPercent},
        {QStringLiteral("asynchronous_completion_p95_ms"), asynchronousCompletionP95},
        {QStringLiteral("completion_change_percent"), completionChangePercent},
        {QStringLiteral("maximum_event_loop_gap_ms"), maximumEventLoopGapMilliseconds},
        {QStringLiteral("checksum_matches"), checksumMatches},
        {QStringLiteral("checksum"), QString::number(synchronousResult.checksum)},
        {QStringLiteral("png_bytes"), synchronousResult.pngBytes},
    };
    writeArtifact(artifact);
    std::cout << QJsonDocument(artifact).toJson(QJsonDocument::Compact).constData() << '\n';

    if (!checksumMatches || schedulingP95 >= 5.0 || callerBlockingReductionPercent < 90.0 ||
        maximumEventLoopGapMilliseconds >= 50.0 || completionChangePercent > 5.0) {
        return 1;
    }
    return 0;
}
