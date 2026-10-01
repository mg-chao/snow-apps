#include "scrolling_image_replay.h"
#include "presentation/capture/scrollinghoverpreview.h"
#include "snow_stitch_images.h"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace snow_shot::scrolling_benchmark;
using Session = std::unique_ptr<SnowStitchSession, decltype(&snow_stitch_session_destroy)>;
using OwnedImage =
    std::unique_ptr<SnowStitchOwnedImage, decltype(&snow_stitch_owned_image_destroy)>;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

qint64 now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               ScrollClock::now().time_since_epoch())
        .count();
}

void fillPixels(uchar* pixels, int width, int height, qsizetype stride) {
    for (int y = 0; y < height; ++y) {
        uchar* row = pixels + y * stride;
        for (int x = 0; x < width; ++x) {
            row[x * 4] = static_cast<uchar>((x * 13 + y * 17) & 0xff);
            row[x * 4 + 1] = static_cast<uchar>((x * 31 + y * 5) & 0xff);
            row[x * 4 + 2] = static_cast<uchar>((x * 7 + y * 23) & 0xff);
            row[x * 4 + 3] = 255;
        }
    }
}

Session seedSession(bool horizontal, QSize size) {
    using Pool = std::unique_ptr<SnowStitchFramePool, decltype(&snow_stitch_frame_pool_destroy)>;
    Pool pool(snow_stitch_frame_pool_create(static_cast<std::uint32_t>(size.width()),
                                            static_cast<std::uint32_t>(size.height()), 1),
              &snow_stitch_frame_pool_destroy);
    SnowStitchConfig config{};
    require(snow_stitch_config_default(&config) != 0, "could not initialize stitch configuration");
    config.axis = horizontal ? SNOW_STITCH_AXIS_HORIZONTAL : SNOW_STITCH_AXIS_VERTICAL;
    Session session(snow_stitch_session_create(&config), &snow_stitch_session_destroy);
    require(pool && session, "could not allocate hover benchmark stitch state");
    SnowStitchFrameBuffer* frame = snow_stitch_frame_pool_acquire(pool.get());
    SnowStitchMutableImageInfo info{};
    require(frame && snow_stitch_frame_buffer_info(frame, &info) != 0,
            "could not allocate hover benchmark source frame");
    fillPixels(info.rgba_bytes, size.width(), size.height(), info.stride_bytes);
    SnowStitchFrameOutcome outcome{};
    require(snow_stitch_session_push_owned(session.get(), &frame, &outcome) != 0 &&
                frame == nullptr &&
                outcome.output_width == static_cast<std::uint32_t>(size.width()) &&
                outcome.output_height == static_cast<std::uint32_t>(size.height()),
            "could not seed hover benchmark stitch output");
    return session;
}

QJsonArray extractionMeasurements() {
    QJsonArray report;
    for (const bool horizontal : {false, true}) {
        const QSize sourceSize = horizontal ? QSize(12000, 1080) : QSize(1920, 12000);
        const QSize outputSize(1920, 1080);
        const int viewportExtent = horizontal ? outputSize.width() : outputSize.height();
        Session session = seedSession(horizontal, sourceSize);
        const int last = 12000 - viewportExtent;
        for (const int start : {0, last / 2 + 13, last}) {
            std::vector<qint64> samples;
            qsizetype outputBytes = 0;
            for (int round = -3; round < 31; ++round) {
                const qint64 began = now();
                OwnedImage image(snow_stitch_session_materialize_axis(
                                     session.get(), static_cast<std::uint32_t>(start),
                                     static_cast<std::uint32_t>(start + viewportExtent)),
                                 &snow_stitch_owned_image_destroy);
                const qint64 elapsed = now() - began;
                require(image != nullptr, "viewport extraction failed");
                SnowStitchImageInfo info{};
                require(snow_stitch_owned_image_info(image.get(), &info) != 0 &&
                            info.width == static_cast<std::uint32_t>(outputSize.width()) &&
                            info.height == static_cast<std::uint32_t>(outputSize.height()) &&
                            info.rgba_len == static_cast<size_t>(outputSize.width()) *
                                                 static_cast<size_t>(outputSize.height()) * 4,
                        "viewport extraction allocation exceeded requested pixels");
                outputBytes = static_cast<qsizetype>(info.rgba_len);
                if (round == 0) {
                    for (int y = 0; y < outputSize.height(); ++y) {
                        const uchar* row = info.rgba_bytes + y * info.stride_bytes;
                        for (int x = 0; x < outputSize.width(); ++x) {
                            const int sourceX = x + (horizontal ? start : 0);
                            const int sourceY = y + (horizontal ? 0 : start);
                            require(row[x * 4] == ((sourceX * 13 + sourceY * 17) & 0xff) &&
                                        row[x * 4 + 1] == ((sourceX * 31 + sourceY * 5) & 0xff) &&
                                        row[x * 4 + 2] == ((sourceX * 7 + sourceY * 23) & 0xff) &&
                                        row[x * 4 + 3] == 255,
                                    "viewport benchmark pixels changed");
                        }
                    }
                }
                if (round >= 0)
                    samples.push_back(elapsed);
            }
            report.append(QJsonObject{
                {QStringLiteral("axis"),
                 horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical")},
                {QStringLiteral("start"), start},
                {QStringLiteral("source_width"), sourceSize.width()},
                {QStringLiteral("source_height"), sourceSize.height()},
                {QStringLiteral("source_pixel_bytes"),
                 static_cast<qint64>(sourceSize.width()) * sourceSize.height() * 4},
                {QStringLiteral("viewport_pixel_bytes"), static_cast<qint64>(outputBytes)},
                {QStringLiteral("extraction"), distribution(std::move(samples))}});
        }
    }
    return report;
}

class TwoFrameSource final : public ScrollingFrameSource {
  public:
    TwoFrameSource(QImage image, bool horizontal)
        : m_image(std::move(image)), m_horizontal(horizontal) {}
    ScrollingSourceEvent receive(int) override {
        if (m_delivered == 2) {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopped; });
            return {};
        }
        ScrollingSourceEvent result;
        result.kind = ScrollingSourceEvent::Kind::Frame;
        const int offset = m_delivered++ * 128;
        result.frame.image =
            m_image.copy(m_horizontal ? offset : 0, m_horizontal ? 0 : offset, 640, 640);
        return result;
    }
    void setTargetFps(int) override {}
    ScrollingSourceStats stats() const override {
        return {};
    }
    void stop() override {
        std::lock_guard lock(m_mutex);
        m_stopped = true;
        m_wake.notify_all();
    }

  private:
    QImage m_image;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stopped = false;
    bool m_horizontal = false;
    int m_delivered = 0;
};

QJsonObject cursorBurstMeasurement(bool horizontal) {
    const auto mode = horizontal ? ScreenshotScrollingRecognitionMode::Horizontal
                                 : ScreenshotScrollingRecognitionMode::Vertical;
    QImage source(horizontal ? QSize(768, 640) : QSize(640, 768), QImage::Format_RGBA8888);
    source.fill(Qt::white);
    {
        QPainter painter(&source);
        quint32 random = 1234567;
        for (int y = 0; y < source.height(); y += 7) {
            for (int x = 0; x < source.width(); x += 7) {
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                painter.fillRect(x, y, 5, 5, QColor::fromRgb(random | 0xff000000U));
            }
        }
    }
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    ScreenshotScrollingPipeline* target = nullptr;
    ScrollingHoverPreview* hover = nullptr;
    bool ready = false;
    bool complete = false;
    int inFlight = 0;
    int requests = 0;
    int maximumInFlight = 0;
    QRect desired;
    qint64 latestChangedAt = 0;
    qint64 latestLatency = 0;
    qint64 maximumOutputBytes = 0;
    QString error;
    ScreenshotScrollingPipeline pipeline(
        [&](ScrollingPipelineFrame frame) {
            require(!frame.fatalError, "cursor burst fixture stitching failed");
            if (!frame.changed || frame.sourceSize != source.size())
                return;
            hover->setEnabled(true);
            desired = {0, 0, 640, 640};
            latestChangedAt = now();
            hover->setHoverRect(desired);
        },
        [&](quint64, QString value) {
            error = std::move(value);
            loop.quit();
        });
    target = &pipeline;
    ScrollingHoverPreview preview({
        [&](std::function<void()> acknowledged) {
            target->pause(71, [&, acknowledged = std::move(acknowledged)] {
                ready = true;
                acknowledged();
                // The first viewport is already in flight. Production code must coalesce every
                // subsequent position into a single newest request with the full capture size.
                for (int index = 1; index < 10000; ++index) {
                    const int extent = horizontal ? source.width() : source.height();
                    const int start = (index * 37) % (extent - 640 + 1);
                    desired = {horizontal ? start : 0, horizontal ? 0 : start, 640, 640};
                    latestChangedAt = now();
                    hover->setHoverRect(desired);
                }
            });
        },
        [&](const QRect& rect, std::function<void(QImage)> completed) {
            require(rect.size() == QSize(640, 640),
                    "hover request must retain the full capture viewport dimensions");
            ++requests;
            maximumInFlight = std::max(maximumInFlight, ++inFlight);
            const int start = horizontal ? rect.left() : rect.top();
            return target->requestViewportPreview(
                start, start + 640, &loop, [&, completed = std::move(completed)](QImage image) {
                    --inFlight;
                    require(!image.isNull(), "cursor burst extraction failed");
                    maximumOutputBytes =
                        std::max(maximumOutputBytes, static_cast<qint64>(image.sizeInBytes()));
                    completed(std::move(image));
                });
        },
        [&](const QImage& image, bool) {
            latestLatency = now() - latestChangedAt;
            require(image.size() == desired.size(), "latest viewport dimensions changed");
            for (int y = 0; y < image.height(); ++y) {
                require(std::memcmp(image.constScanLine(y),
                                    source.constScanLine(y + desired.top()) + desired.left() * 4,
                                    static_cast<size_t>(image.bytesPerLine())) == 0,
                        "latest cursor output is incorrect");
            }
            complete = true;
            loop.quit();
        },
        [] {},
        [&] {
            target->resume(71, QSize(640, 640), [source, horizontal] {
                return std::make_unique<TwoFrameSource>(source, horizontal);
            });
        },
    });
    hover = &preview;
    pipeline.begin(71, QSize(640, 640), mode, [source, horizontal] {
        return std::make_unique<TwoFrameSource>(source, horizontal);
    });
    timeout.start(15000);
    loop.exec();
    require(ready && complete && error.isEmpty() && requests == 2 && maximumInFlight == 1,
            "cursor requests must coalesce to one in-flight and one newest range");
    require(maximumOutputBytes == 640 * 640 * 4,
            "cursor burst allocated more than viewport-sized output");
    return {{QStringLiteral("axis"),
             horizontal ? QStringLiteral("horizontal") : QStringLiteral("vertical")},
            {QStringLiteral("cursor_events"), 10000},
            {QStringLiteral("worker_requests"), requests},
            {QStringLiteral("maximum_in_flight"), maximumInFlight},
            {QStringLiteral("maximum_output_pixel_bytes"), maximumOutputBytes},
            {QStringLiteral("latest_result_latency_ns"), latestLatency}};
}
} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    try {
        require(QStringLiteral(SNOW_SCROLLING_BUILD_CONFIG) == QStringLiteral("Release"),
                "hover preview benchmark requires the Release performance preset");
        const QJsonObject report{
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("build_config"), QStringLiteral(SNOW_SCROLLING_BUILD_CONFIG)},
            {QStringLiteral("timing_unit"), QStringLiteral("nanoseconds")},
            {QStringLiteral("extraction"), extractionMeasurements()},
            {QStringLiteral("cursor_burst"),
             QJsonArray{cursorBurstMeasurement(false), cursorBurstMeasurement(true)}},
            {QStringLiteral("success"), true}};
        const QByteArray encoded = QJsonDocument(report).toJson(QJsonDocument::Indented);
        std::cout << encoded.constData();
        const QString output = qEnvironmentVariable("SNOW_SCROLLING_HOVER_PERF_OUTPUT");
        if (!output.isEmpty()) {
            require(QDir().mkpath(QFileInfo(output).absolutePath()),
                    "could not create hover benchmark output directory");
            QSaveFile file(output);
            require(file.open(QIODevice::WriteOnly) && file.write(encoded) == encoded.size() &&
                        file.commit(),
                    "could not save hover benchmark report");
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
