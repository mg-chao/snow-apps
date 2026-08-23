#include "snow_shot/presentation/screenshotclipboardservice.h"
#include "snow_shot/presentation/screenshotdefaultstyles.h"
#include "snow_shot/presentation/screenshotdisplaysession.h"
#include "snow_shot/presentation/screenshotexportservice.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotresultcompositor.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "../src/presentation/services/screenshotclipboardperfinstrumentation.h"

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QMap>
#include <QSet>
#include <QSysInfo>
#include <QThread>
#include <QTimer>

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <vector>

namespace clipboard_perf = snow_shot::presentation::clipboard_perf;

namespace {
constexpr int kReportSchemaVersion = 1;
constexpr int kDefaultWarmups = 3;
constexpr int kDefaultSamples = 20;
constexpr int kExportTimeoutMilliseconds = 60'000;

enum class ScenarioKind {
    ClipboardOnly,
    SelectionExport,
};

struct Scenario {
    QString id;
    QString description;
    ScenarioKind kind = ScenarioKind::ClipboardOnly;
    QSize size;
    QImage::Format format = QImage::Format_ARGB32;
    ScreenshotResultStyle style;
    bool twoSources = false;
};

struct TraceAggregate {
    qint64 count = 0;
    qint64 totalNanoseconds = 0;
    qint64 maximumNanoseconds = 0;
};

struct ValidationResult {
    bool success = false;
    QString error;
    quint64 expectedChecksum = 0;
    quint64 actualChecksum = 0;
    qsizetype payloadBytes = 0;
};

struct Sample {
    QMap<QString, TraceAggregate> durations;
    QMap<QString, qint64> counters;
    bool success = false;
    bool timedOut = false;
    QString error;
    quint64 expectedChecksum = 0;
    quint64 actualChecksum = 0;
    qint64 privateBytesBefore = -1;
    qint64 privateBytesAfter = -1;
};

struct ExportRunState {
    Sample sample;
    bool cancelled = false;
};

class Collector final : public clipboard_perf::Sink {
  public:
    void setSample(Sample* sample) {
        const std::lock_guard lock(m_mutex);
        m_sample = sample;
    }

    void recordDuration(const char* name, qint64 elapsedNanoseconds) override {
        const std::lock_guard lock(m_mutex);
        if (m_sample == nullptr) {
            return;
        }
        TraceAggregate& aggregate = m_sample->durations[QString::fromLatin1(name)];
        ++aggregate.count;
        aggregate.totalNanoseconds += elapsedNanoseconds;
        aggregate.maximumNanoseconds =
            std::max(aggregate.maximumNanoseconds, elapsedNanoseconds);
    }

    void recordCounter(const char* name, qint64 value) override {
        const std::lock_guard lock(m_mutex);
        if (m_sample != nullptr) {
            m_sample->counters[QString::fromLatin1(name)] += value;
        }
    }

    void addDuration(Sample& sample, const QString& name, qint64 elapsedNanoseconds) {
        const std::lock_guard lock(m_mutex);
        TraceAggregate& aggregate = sample.durations[name];
        ++aggregate.count;
        aggregate.totalNanoseconds += elapsedNanoseconds;
        aggregate.maximumNanoseconds =
            std::max(aggregate.maximumNanoseconds, elapsedNanoseconds);
    }

  private:
    std::mutex m_mutex;
    Sample* m_sample = nullptr;
};

QVector<Scenario> scenarios() {
    return {
        {QStringLiteral("clipboard-1080p-argb32"),
         QStringLiteral("Direct 1920x1080 ARGB32 publication"),
         ScenarioKind::ClipboardOnly, QSize(1920, 1080), QImage::Format_ARGB32, {}, false},
        {QStringLiteral("clipboard-4k-argb32"),
         QStringLiteral("Direct 3840x2160 ARGB32 publication"),
         ScenarioKind::ClipboardOnly, QSize(3840, 2160), QImage::Format_ARGB32, {}, false},
        {QStringLiteral("clipboard-4k-premultiplied"),
         QStringLiteral("Direct 3840x2160 premultiplied publication"),
         ScenarioKind::ClipboardOnly, QSize(3840, 2160),
         QImage::Format_ARGB32_Premultiplied, {}, false},
        {QStringLiteral("clipboard-4k-rgba8888"),
         QStringLiteral("Direct 3840x2160 RGBA8888 publication"),
         ScenarioKind::ClipboardOnly, QSize(3840, 2160), QImage::Format_RGBA8888, {}, false},
        {QStringLiteral("export-1080p-plain"),
         QStringLiteral("1920x1080 selection export and publication"),
         ScenarioKind::SelectionExport, QSize(1920, 1080), QImage::Format_ARGB32, {}, false},
        {QStringLiteral("export-4k-plain"),
         QStringLiteral("3840x2160 selection export and publication"),
         ScenarioKind::SelectionExport, QSize(3840, 2160), QImage::Format_ARGB32, {}, false},
        {QStringLiteral("export-4k-effects"),
         QStringLiteral("3840x2160 selection export with rounded corners and shadow"),
         ScenarioKind::SelectionExport, QSize(3840, 2160), QImage::Format_ARGB32,
         ScreenshotResultStyle{16, 24, QColor(0x33, 0x33, 0x33, 180)}, false},
        {QStringLiteral("export-dual-display-5120x1440"),
         QStringLiteral("5120x1440 selection spanning two captured displays"),
         ScenarioKind::SelectionExport, QSize(5120, 1440), QImage::Format_ARGB32, {}, true},
    };
}

QString scenarioKindName(ScenarioKind kind) {
    return kind == ScenarioKind::ClipboardOnly ? QStringLiteral("clipboard_only")
                                                : QStringLiteral("selection_export");
}

QString imageFormatName(QImage::Format format) {
    switch (format) {
    case QImage::Format_ARGB32:
        return QStringLiteral("argb32");
    case QImage::Format_ARGB32_Premultiplied:
        return QStringLiteral("argb32_premultiplied");
    case QImage::Format_RGBA8888:
        return QStringLiteral("rgba8888");
    default:
        return QStringLiteral("format_%1").arg(static_cast<int>(format));
    }
}

QImage patternedImage(const QSize& size, QImage::Format format, int seed) {
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = 160 + ((x * 3 + y * 5 + seed) % 96);
            row[x] = qRgba((x + y + seed * 17) & 0xff,
                           (x * 3 + y * 7 + seed * 29) & 0xff,
                           (x * 11 + y * 5 + seed * 43) & 0xff, alpha);
        }
    }
    return format == QImage::Format_ARGB32 ? image : image.convertToFormat(format);
}

quint64 checksum(const uchar* bytes, qsizetype size) {
    quint64 value = 1469598103934665603ULL;
    for (qsizetype index = 0; index < size; ++index) {
        value ^= bytes[index];
        value *= 1099511628211ULL;
    }
    return value;
}

qint64 privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                              sizeof(counters))) {
        return -1;
    }
    return static_cast<qint64>(counters.PrivateUsage);
}

bool openClipboardForValidation() {
    constexpr int attempts = 20;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (OpenClipboard(nullptr) != FALSE) {
            return true;
        }
        QThread::msleep(2);
    }
    return false;
}

ValidationResult validateClipboard(const QImage& source) {
    ValidationResult result;
    const QImage expected = source.convertToFormat(QImage::Format_ARGB32);
    const qsizetype expectedBytes = expected.sizeInBytes();
    result.expectedChecksum = checksum(expected.constBits(), expectedBytes);

    if (!openClipboardForValidation()) {
        result.error = QStringLiteral("OpenClipboard failed during validation: %1")
                           .arg(static_cast<qulonglong>(GetLastError()));
        return result;
    }

    HANDLE dibV5Handle = GetClipboardData(CF_DIBV5);
    const bool dibV5 = dibV5Handle != nullptr;
    HANDLE handle = dibV5Handle;
    if (handle == nullptr) {
        handle = GetClipboardData(CF_DIB);
        if (handle == nullptr) {
            result.error =
                QStringLiteral("Neither CF_DIBV5 nor CF_DIB was present after publication");
            CloseClipboard();
            return result;
        }
    }
    const SIZE_T allocationBytes = GlobalSize(handle);
    const void* memory = GlobalLock(handle);
    if (memory == nullptr) {
        result.error = QStringLiteral("GlobalLock failed during clipboard validation");
        CloseClipboard();
        return result;
    }

    const auto* header = static_cast<const BITMAPINFOHEADER*>(memory);
    const quint64 headerBytes = dibV5 ? sizeof(BITMAPV5HEADER) : sizeof(BITMAPINFOHEADER);
    const quint64 requiredBytes = headerBytes + static_cast<quint64>(expectedBytes);
    bool validHeader = allocationBytes >= requiredBytes &&
                       header->biWidth == expected.width() &&
                       header->biHeight == (dibV5 ? -expected.height() : expected.height()) &&
                       header->biPlanes == 1 &&
                       header->biBitCount == 32 &&
                       header->biSizeImage == static_cast<DWORD>(expectedBytes);
    if (dibV5) {
        const auto* v5Header = static_cast<const BITMAPV5HEADER*>(memory);
        validHeader = validHeader && v5Header->bV5Size == sizeof(BITMAPV5HEADER) &&
                      v5Header->bV5Compression == BI_BITFIELDS &&
                      v5Header->bV5RedMask == 0x00ff0000 &&
                      v5Header->bV5GreenMask == 0x0000ff00 &&
                      v5Header->bV5BlueMask == 0x000000ff &&
                      v5Header->bV5AlphaMask == 0xff000000;
    } else {
        validHeader = validHeader && header->biSize == sizeof(BITMAPINFOHEADER) &&
                      header->biCompression == BI_RGB;
    }
    if (validHeader) {
        const auto* payload = static_cast<const uchar*>(memory) + headerBytes;
        if (!dibV5) {
            QImage bottomUp(expected.size(), QImage::Format_ARGB32);
            for (int row = 0; row < expected.height(); ++row) {
                std::memcpy(bottomUp.scanLine(row),
                            expected.constScanLine(expected.height() - 1 - row),
                            static_cast<std::size_t>(expected.bytesPerLine()));
            }
            result.expectedChecksum = checksum(bottomUp.constBits(), bottomUp.sizeInBytes());
        }
        result.actualChecksum = checksum(payload, expectedBytes);
        result.payloadBytes = expectedBytes;
    }
    GlobalUnlock(handle);
    CloseClipboard();

    if (!validHeader) {
        result.error = QStringLiteral("clipboard DIB header or payload length was invalid");
        return result;
    }
    if (result.actualChecksum != result.expectedChecksum) {
        result.error =
            QStringLiteral("clipboard DIB pixel checksum did not match the source image");
        return result;
    }
    result.success = true;
    return result;
}

class ExportFixture final {
  public:
    explicit ExportFixture(const Scenario& scenario)
        : m_runtime(SnowCanvasRuntimeConfig{
              snow_shot::presentation::screenshotCanvasStyleDefaults()}) {
        const int sourceCount = scenario.twoSources ? 2 : 1;
        int left = 0;
        for (int index = 0; index < sourceCount; ++index) {
            const int width = index + 1 == sourceCount ? scenario.size.width() - left
                                                       : scenario.size.width() / sourceCount;
            const QRect rect(left, 0, width, scenario.size.height());
            CapturedDisplayModel display;
            display.stableId = QStringLiteral("benchmark-display-%1").arg(index);
            display.name = QStringLiteral("Benchmark display %1").arg(index + 1);
            display.physicalRect = rect;
            display.canvasRect = rect;
            display.imageSourceCanvasRect = rect;
            display.logicalRect = rect;
            display.image = patternedImage(rect.size(), QImage::Format_ARGB32, index + 1);
            display.active = true;
            m_displays.appendDisplay(std::move(display));
            left += width;
        }
        m_selection = QRect(QPoint(), scenario.size);
        m_service = std::make_unique<ScreenshotExportService>(ScreenshotExportServiceContext{
            m_displays,
            m_runtime,
            m_geometry,
        });
    }

    [[nodiscard]] bool isValid() const {
        return m_runtime.isValid() && m_service != nullptr;
    }

    ScreenshotExportService& service() {
        return *m_service;
    }

    [[nodiscard]] const QRect& selection() const {
        return m_selection;
    }

    [[nodiscard]] QImage expectedImage(const ScreenshotResultStyle& style) {
        QList<CanvasExportSource> sources;
        m_displays.forEachActiveDisplay(
            [&sources](qsizetype, const CapturedDisplayModel& display) {
                sources.push_back({display.image,
                                   ScreenshotGeometryMapper::displayImageSourceCanvasRect(display)});
            });
        return ScreenshotResultCompositor::compose(
            m_runtime.renderToImage(QRectF(m_selection), m_selection.size(), sources), style);
    }

  private:
    ScreenshotDisplaySession m_displays;
    SnowCanvasRuntime m_runtime;
    ScreenshotGeometryMapper m_geometry;
    QRect m_selection;
    std::unique_ptr<ScreenshotExportService> m_service;
};

qint64 elapsedNanoseconds(const std::chrono::steady_clock::time_point& started) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

Sample runClipboardSample(const QImage& image, Collector& collector) {
    Sample sample;
    collector.setSample(&sample);
    sample.privateBytesBefore = privateBytes();
    const auto started = std::chrono::steady_clock::now();
    const bool published =
        ScreenshotClipboardService::publishImage(QApplication::clipboard(), image);
    collector.addDuration(sample, QStringLiteral("benchmark.end_to_end"),
                          elapsedNanoseconds(started));
    const ValidationResult validation = validateClipboard(image);
    sample.privateBytesAfter = privateBytes();
    sample.success = published &&
                     sample.counters.value(QStringLiteral("clipboard.success")) == 1 &&
                     validation.success;
    sample.error = validation.error;
    sample.expectedChecksum = validation.expectedChecksum;
    sample.actualChecksum = validation.actualChecksum;
    collector.setSample(nullptr);
    return sample;
}

Sample runExportSample(const Scenario& scenario, ExportFixture& fixture, Collector& collector) {
    auto state = std::make_shared<ExportRunState>();
    collector.setSample(&state->sample);
    state->sample.privateBytesBefore = privateBytes();

    QObject receiver;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(kExportTimeoutMilliseconds);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        state->cancelled = true;
        state->sample.timedOut = true;
        state->sample.error = QStringLiteral("selection export timed out");
        loop.quit();
    });

    const QImage expected = fixture.expectedImage(scenario.style);
    const auto started = std::chrono::steady_clock::now();
    const bool scheduled = fixture.service().requestSelectionClipboard(
        fixture.selection(), scenario.style, &receiver,
         [&, state, expected](ScreenshotSelectionClipboardResult result) {
            if (state->cancelled) {
                return;
            }
            if (!result.isValid()) {
                state->sample.error = QStringLiteral("selection export produced an invalid payload");
                collector.addDuration(state->sample, QStringLiteral("benchmark.end_to_end"),
                                      elapsedNanoseconds(started));
                loop.quit();
                return;
            }
            const bool published = ScreenshotClipboardService::publish(
                QApplication::clipboard(), std::move(result.payload));
            collector.addDuration(state->sample, QStringLiteral("benchmark.end_to_end"),
                                  elapsedNanoseconds(started));
            const ValidationResult validation = validateClipboard(expected);
            state->sample.success =
                published &&
                state->sample.counters.value(QStringLiteral("export.success")) == 1 &&
                state->sample.counters.value(QStringLiteral("clipboard.success")) == 1 &&
                validation.success;
            state->sample.error = validation.error;
            state->sample.expectedChecksum = validation.expectedChecksum;
            state->sample.actualChecksum = validation.actualChecksum;
            loop.quit();
        });
    if (!scheduled) {
        state->sample.error = QStringLiteral("selection export could not be scheduled");
    } else {
        timeout.start();
        loop.exec();
        timeout.stop();
    }
    state->sample.privateBytesAfter = privateBytes();
    collector.setSample(nullptr);
    return std::move(state->sample);
}

double percentile(std::vector<double> values, double fraction) {
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = std::min(
        values.size() - 1,
        static_cast<size_t>(std::ceil(fraction * static_cast<double>(values.size()))) - 1);
    return values[index];
}

QJsonObject distribution(const std::vector<double>& values) {
    if (values.empty()) {
        return QJsonObject{{QStringLiteral("count"), 0}};
    }
    const auto [minimum, maximum] = std::minmax_element(values.begin(), values.end());
    const double mean =
        std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
    double variance = 0.0;
    for (const double value : values) {
        const double delta = value - mean;
        variance += delta * delta;
    }
    variance /= static_cast<double>(values.size());
    return {
        {QStringLiteral("count"), static_cast<qint64>(values.size())},
        {QStringLiteral("min"), *minimum},
        {QStringLiteral("mean"), mean},
        {QStringLiteral("stddev"), std::sqrt(variance)},
        {QStringLiteral("p50"), percentile(values, 0.50)},
        {QStringLiteral("p95"), percentile(values, 0.95)},
        {QStringLiteral("p99"), percentile(values, 0.99)},
        {QStringLiteral("max"), *maximum},
    };
}

QJsonObject durationJson(const Sample& sample) {
    QJsonObject output;
    for (auto iterator = sample.durations.cbegin(); iterator != sample.durations.cend();
         ++iterator) {
        output.insert(
            iterator.key(),
            QJsonObject{
                {QStringLiteral("count"), iterator->count},
                {QStringLiteral("total_ms"),
                 static_cast<double>(iterator->totalNanoseconds) / 1'000'000.0},
                {QStringLiteral("max_ms"),
                 static_cast<double>(iterator->maximumNanoseconds) / 1'000'000.0},
            });
    }
    return output;
}

QJsonObject countersJson(const QMap<QString, qint64>& counters) {
    QJsonObject output;
    for (auto iterator = counters.cbegin(); iterator != counters.cend(); ++iterator) {
        output.insert(iterator.key(), iterator.value());
    }
    return output;
}

QJsonObject sampleJson(const Scenario& scenario, int sampleIndex, const Sample& sample) {
    return {
        {QStringLiteral("scenario"), scenario.id},
        {QStringLiteral("sample_index"), sampleIndex},
        {QStringLiteral("success"), sample.success},
        {QStringLiteral("timed_out"), sample.timedOut},
        {QStringLiteral("error"), sample.error},
        {QStringLiteral("expected_checksum"), QString::number(sample.expectedChecksum)},
        {QStringLiteral("actual_checksum"), QString::number(sample.actualChecksum)},
        {QStringLiteral("private_bytes_before"), sample.privateBytesBefore},
        {QStringLiteral("private_bytes_after"), sample.privateBytesAfter},
        {QStringLiteral("private_bytes_delta"),
         sample.privateBytesBefore >= 0 && sample.privateBytesAfter >= 0
             ? sample.privateBytesAfter - sample.privateBytesBefore
             : -1},
        {QStringLiteral("durations"), durationJson(sample)},
        {QStringLiteral("counters"), countersJson(sample.counters)},
    };
}

QJsonObject summarizeScenario(const Scenario& scenario, const QVector<Sample>& samples) {
    QSet<QString> durationNames;
    QSet<QString> counterNames;
    int successes = 0;
    qint64 maximumPrivateBytes = -1;
    for (const Sample& sample : samples) {
        successes += sample.success ? 1 : 0;
        maximumPrivateBytes = std::max(maximumPrivateBytes, sample.privateBytesBefore);
        maximumPrivateBytes = std::max(maximumPrivateBytes, sample.privateBytesAfter);
        for (auto iterator = sample.durations.cbegin(); iterator != sample.durations.cend();
             ++iterator) {
            durationNames.insert(iterator.key());
        }
        for (auto iterator = sample.counters.cbegin(); iterator != sample.counters.cend();
             ++iterator) {
            counterNames.insert(iterator.key());
        }
    }

    QJsonObject durationDistributions;
    for (const QString& name : durationNames) {
        std::vector<double> milliseconds;
        for (const Sample& sample : samples) {
            const auto found = sample.durations.constFind(name);
            if (found != sample.durations.cend()) {
                milliseconds.push_back(
                    static_cast<double>(found->totalNanoseconds) / 1'000'000.0);
            }
        }
        durationDistributions.insert(name, distribution(milliseconds));
    }

    QJsonObject counterDistributions;
    QJsonObject counterTotals;
    for (const QString& name : counterNames) {
        std::vector<double> values;
        qint64 total = 0;
        for (const Sample& sample : samples) {
            const qint64 value = sample.counters.value(name);
            values.push_back(static_cast<double>(value));
            total += value;
        }
        counterDistributions.insert(name, distribution(values));
        counterTotals.insert(name, total);
    }

    std::vector<double> copyThroughput;
    std::vector<double> preparationThroughput;
    std::vector<double> publicationThroughput;
    for (const Sample& sample : samples) {
        const double bytes =
            static_cast<double>(sample.counters.value(QStringLiteral("clipboard.pixel_bytes")));
        auto copy = sample.durations.constFind(QStringLiteral("clipboard.copy_pixels"));
        if (copy == sample.durations.cend()) {
            copy = sample.durations.constFind(QStringLiteral("clipboard.fused_rgba8888"));
        }
        const auto preparation =
            sample.durations.constFind(QStringLiteral("clipboard.prepare_total"));
        const auto total = sample.durations.constFind(QStringLiteral("clipboard.publish_total"));
        if (bytes > 0.0 && copy != sample.durations.cend() && copy->totalNanoseconds > 0) {
            copyThroughput.push_back(bytes * 1'000'000'000.0 /
                                     static_cast<double>(copy->totalNanoseconds) /
                                     (1024.0 * 1024.0));
        }
        if (bytes > 0.0 && preparation != sample.durations.cend() &&
            preparation->totalNanoseconds > 0) {
            preparationThroughput.push_back(bytes * 1'000'000'000.0 /
                                            static_cast<double>(preparation->totalNanoseconds) /
                                            (1024.0 * 1024.0));
        }
        if (bytes > 0.0 && total != sample.durations.cend() && total->totalNanoseconds > 0) {
            publicationThroughput.push_back(bytes * 1'000'000'000.0 /
                                            static_cast<double>(total->totalNanoseconds) /
                                            (1024.0 * 1024.0));
        }
    }

    return {
        {QStringLiteral("id"), scenario.id},
        {QStringLiteral("description"), scenario.description},
        {QStringLiteral("kind"), scenarioKindName(scenario.kind)},
        {QStringLiteral("input_width"), scenario.size.width()},
        {QStringLiteral("input_height"), scenario.size.height()},
        {QStringLiteral("input_format"), imageFormatName(scenario.format)},
        {QStringLiteral("source_count"), scenario.twoSources ? 2 : 1},
        {QStringLiteral("corner_radius"), scenario.style.cornerRadius},
        {QStringLiteral("shadow_width"), scenario.style.shadowWidth},
        {QStringLiteral("samples"), samples.size()},
        {QStringLiteral("successful_samples"), successes},
        {QStringLiteral("failed_samples"), samples.size() - successes},
        {QStringLiteral("maximum_observed_private_bytes"), maximumPrivateBytes},
        {QStringLiteral("duration_ms"), durationDistributions},
        {QStringLiteral("counter_values"), counterDistributions},
        {QStringLiteral("counter_totals"), counterTotals},
        {QStringLiteral("copy_throughput_mib_per_second"), distribution(copyThroughput)},
        {QStringLiteral("preparation_throughput_mib_per_second"),
         distribution(preparationThroughput)},
        {QStringLiteral("publication_throughput_mib_per_second"),
         distribution(publicationThroughput)},
    };
}

QString compilerDescription() {
#if defined(_MSC_VER)
    return QStringLiteral("MSVC %1").arg(_MSC_VER);
#elif defined(__clang__)
    return QStringLiteral("Clang %1.%2.%3")
        .arg(__clang_major__)
        .arg(__clang_minor__)
        .arg(__clang_patchlevel__);
#else
    return QStringLiteral("unknown");
#endif
}

QJsonObject environmentMetadata() {
    return {
        {QStringLiteral("timestamp_utc"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("os_product"), QSysInfo::prettyProductName()},
        {QStringLiteral("os_kernel_type"), QSysInfo::kernelType()},
        {QStringLiteral("os_kernel_version"), QSysInfo::kernelVersion()},
        {QStringLiteral("cpu_architecture"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("cpu_identifier"),
         QString::fromLocal8Bit(qgetenv("PROCESSOR_IDENTIFIER"))},
        {QStringLiteral("logical_cpu_count"), QThread::idealThreadCount()},
        {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
        {QStringLiteral("qt_library_path"), QLibraryInfo::path(QLibraryInfo::LibrariesPath)},
        {QStringLiteral("compiler"), compilerDescription()},
#if defined(NDEBUG)
        {QStringLiteral("build_configuration"), QStringLiteral("release")},
#else
        {QStringLiteral("build_configuration"), QStringLiteral("debug")},
#endif
        {QStringLiteral("process_priority_class"),
         static_cast<qint64>(GetPriorityClass(GetCurrentProcess()))},
    };
}

QJsonObject instrumentationCalibration(Collector& collector) {
    constexpr int iterations = 10'000;
    Sample sample;
    collector.setSample(&sample);

    auto started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        SNOW_SHOT_CLIPBOARD_PERF_SCOPE("calibration.scope");
    }
    const qint64 scopeNanoseconds = elapsedNanoseconds(started);

    started = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        SNOW_SHOT_CLIPBOARD_PERF_COUNTER("calibration.counter", 1);
    }
    const qint64 counterNanoseconds = elapsedNanoseconds(started);
    collector.setSample(nullptr);

    return {
        {QStringLiteral("iterations"), iterations},
        {QStringLiteral("scope_call_ns"),
         static_cast<double>(scopeNanoseconds) / static_cast<double>(iterations)},
        {QStringLiteral("counter_call_ns"),
         static_cast<double>(counterNanoseconds) / static_cast<double>(iterations)},
    };
}

bool writeJson(QFile& file, const QJsonObject& object, QJsonDocument::JsonFormat format) {
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(QJsonDocument(object).toJson(format)) >= 0;
}

bool runSelfTest(Collector& collector) {
    const QJsonObject stats = distribution({1.0, 2.0, 3.0, 4.0});
    if (stats.value(QStringLiteral("p50")).toDouble() != 2.0 ||
        stats.value(QStringLiteral("p95")).toDouble() != 4.0) {
        std::cerr << "distribution self-test failed\n";
        return false;
    }

    const QImage image = patternedImage(QSize(64, 48), QImage::Format_ARGB32, 7);
    const Sample sample = runClipboardSample(image, collector);
    const bool instrumented =
        sample.durations.contains(QStringLiteral("clipboard.prepare_total")) &&
        sample.durations.contains(QStringLiteral("clipboard.publish_total")) &&
        sample.durations.contains(QStringLiteral("clipboard.copy_pixels")) &&
        sample.counters.value(QStringLiteral("clipboard.success")) == 1;
    if (!sample.success || !instrumented) {
        std::cerr << "clipboard publication self-test failed: "
                  << sample.error.toLocal8Bit().constData() << '\n';
        return false;
    }
    std::cout << "clipboard benchmark self-test passed\n";
    return true;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Snow Shot Clipboard Benchmark"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Native Windows screenshot clipboard performance benchmark. This "
                       "executable replaces the current clipboard contents."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("output"), QStringLiteral("Artifact output directory"),
                      QStringLiteral("directory"),
                      QStringLiteral("clipboard-performance-results")});
    parser.addOption({QStringLiteral("warmups"), QStringLiteral("Warmups per scenario"),
                      QStringLiteral("count"), QString::number(kDefaultWarmups)});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Measured samples per scenario"),
                      QStringLiteral("count"), QString::number(kDefaultSamples)});
    parser.addOption({QStringLiteral("scenario"),
                      QStringLiteral("Run one scenario; may be specified repeatedly"),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("list"), QStringLiteral("List scenarios and exit")});
    parser.addOption({QStringLiteral("self-test"),
                      QStringLiteral(
                          "Validate instrumentation, statistics, and native DIB output")});
    parser.process(application);

    const QVector<Scenario> availableScenarios = scenarios();
    if (parser.isSet(QStringLiteral("list"))) {
        for (const Scenario& scenario : availableScenarios) {
            std::cout << scenario.id.toLocal8Bit().constData() << "\t"
                      << scenario.description.toLocal8Bit().constData() << '\n';
        }
        return EXIT_SUCCESS;
    }

    Collector collector;
    clipboard_perf::setSink(&collector);
    if (parser.isSet(QStringLiteral("self-test"))) {
        const bool passed = runSelfTest(collector);
        clipboard_perf::setSink(nullptr);
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    bool warmupsValid = false;
    bool samplesValid = false;
    const int warmups = parser.value(QStringLiteral("warmups")).toInt(&warmupsValid);
    const int sampleCount = parser.value(QStringLiteral("samples")).toInt(&samplesValid);
    if (!warmupsValid || !samplesValid || warmups < 0 || sampleCount < 1) {
        std::cerr << "warmups must be non-negative and samples must be positive\n";
        clipboard_perf::setSink(nullptr);
        return EXIT_FAILURE;
    }

    QVector<Scenario> selectedScenarios;
    const QStringList selectedIds = parser.values(QStringLiteral("scenario"));
    if (selectedIds.isEmpty()) {
        selectedScenarios = availableScenarios;
    } else {
        QSet<QString> added;
        for (const QString& id : selectedIds) {
            const auto found = std::find_if(
                availableScenarios.cbegin(), availableScenarios.cend(),
                [&id](const Scenario& scenario) { return scenario.id == id; });
            if (found == availableScenarios.cend()) {
                std::cerr << "unknown scenario: " << id.toLocal8Bit().constData() << '\n';
                clipboard_perf::setSink(nullptr);
                return EXIT_FAILURE;
            }
            if (!added.contains(id)) {
                selectedScenarios.push_back(*found);
                added.insert(id);
            }
        }
    }

    const QString outputDirectory =
        QDir::cleanPath(parser.value(QStringLiteral("output")));
    if (!QDir().mkpath(outputDirectory)) {
        std::cerr << "could not create output directory\n";
        clipboard_perf::setSink(nullptr);
        return EXIT_FAILURE;
    }
    QFile rawFile(QDir(outputDirectory).filePath(QStringLiteral("raw.jsonl")));
    if (!rawFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        std::cerr << "could not create raw JSONL artifact\n";
        clipboard_perf::setSink(nullptr);
        return EXIT_FAILURE;
    }

    bool allSuccessful = true;
    QJsonArray scenarioReports;
    for (const Scenario& scenario : selectedScenarios) {
        std::cout << "running " << scenario.id.toLocal8Bit().constData() << '\n';
        QVector<Sample> measured;
        measured.reserve(sampleCount);
        const QImage directImage =
            scenario.kind == ScenarioKind::ClipboardOnly
                ? patternedImage(scenario.size, scenario.format, 11)
                : QImage{};
        std::unique_ptr<ExportFixture> exportFixture;
        if (scenario.kind == ScenarioKind::SelectionExport) {
            exportFixture = std::make_unique<ExportFixture>(scenario);
            if (!exportFixture->isValid()) {
                std::cerr << "could not initialize canvas export fixture\n";
                allSuccessful = false;
                break;
            }
        }

        for (int index = -warmups; index < sampleCount; ++index) {
            Sample sample = scenario.kind == ScenarioKind::ClipboardOnly
                                ? runClipboardSample(directImage, collector)
                                : runExportSample(scenario, *exportFixture, collector);
            if (!sample.success) {
                allSuccessful = false;
                std::cerr << scenario.id.toLocal8Bit().constData() << " sample " << index
                          << " failed: " << sample.error.toLocal8Bit().constData() << '\n';
            }
            if (index < 0) {
                if (sample.timedOut) {
                    break;
                }
                continue;
            }
            rawFile.write(QJsonDocument(sampleJson(scenario, index, sample))
                              .toJson(QJsonDocument::Compact));
            rawFile.write("\n");
            measured.push_back(std::move(sample));
            if (measured.back().timedOut) {
                break;
            }
        }
        scenarioReports.push_back(summarizeScenario(scenario, measured));
    }
    rawFile.close();

    QJsonObject report{
        {QStringLiteral("schema_version"), kReportSchemaVersion},
        {QStringLiteral("benchmark"), QStringLiteral("screenshot_copy_to_clipboard")},
        {QStringLiteral("clipboard_side_effect"),
         QStringLiteral("replaces_existing_contents_and_leaves_final_fixture")},
        {QStringLiteral("configuration"),
         QJsonObject{
             {QStringLiteral("warmups"), warmups},
             {QStringLiteral("samples"), sampleCount},
             {QStringLiteral("scenario_count"), selectedScenarios.size()},
         }},
        {QStringLiteral("environment"), environmentMetadata()},
        {QStringLiteral("instrumentation_calibration"),
         instrumentationCalibration(collector)},
        {QStringLiteral("scenarios"), scenarioReports},
        {QStringLiteral("all_samples_valid"), allSuccessful},
    };
    const QString reportPath =
        QDir(outputDirectory).filePath(QStringLiteral("report.json"));
    QFile reportFile(reportPath);
    if (!writeJson(reportFile, report, QJsonDocument::Indented)) {
        std::cerr << "could not write JSON report\n";
        clipboard_perf::setSink(nullptr);
        return EXIT_FAILURE;
    }

    clipboard_perf::setSink(nullptr);
    std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
    std::cout << "report: " << QFileInfo(reportPath).absoluteFilePath().toLocal8Bit().constData()
              << '\n';
    return allSuccessful ? EXIT_SUCCESS : 2;
}
