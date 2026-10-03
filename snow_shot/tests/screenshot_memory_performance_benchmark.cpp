#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/presentation/screenshotrecognitionimage.h"
#include "snow_shot/presentation/screenshotselectionshadowrenderer.h"
#include "snowimageqtcodec.h"
#include "../../test-support/memorysnapshot.h"

#include <QApplication>
#include <QBuffer>
#include <QColorSpace>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QImageReader>
#include <QImageWriter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSysInfo>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

#ifndef SNOW_SHOT_BENCHMARK_MANAGED_READER
#define SNOW_SHOT_BENCHMARK_MANAGED_READER 1
#endif

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QJsonValue footprint() {
    const auto value = snow::test_support::memorySnapshot().footprintBytes;
    return value ? QJsonValue(static_cast<qint64>(value)) : QJsonValue(QJsonValue::Null);
}

QByteArray rasterHash(const QImage& image) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const qsizetype rowBytes = (static_cast<qsizetype>(image.width()) * image.depth() + 7) / 8;
    for (int y = 0; y < image.height(); ++y)
        hash.addData(
            QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(y)), rowBytes));
    return hash.result().toHex();
}

QImage patternedImage(QSize size, QImage::Format format) {
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int alpha = 160 + ((x * 5 + y * 3) % 96);
            row[x] = qPremultiply(qRgba((x * 17 + y * 3) & 255, (x * 5 + y * 19) & 255,
                                        (x * 11 + y * 7) & 255, alpha));
        }
    }
    image = image.convertToFormat(format);
    image.setColorSpace(QColorSpace::SRgb);
    image.setDotsPerMeterX(4321);
    image.setDotsPerMeterY(5678);
    image.setText(QStringLiteral("Author"), QStringLiteral("Snow Shot memory benchmark"));
    return image;
}

QByteArray pngBytes(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly), "could not open PNG output");
    QImageWriter writer(&buffer, "png");
    writer.setCompression(1);
    require(writer.write(image), "could not encode deterministic PNG fixture");
    return bytes;
}

QImage readerImage(const QByteArray& encoded, QSize scaled, bool managed) {
    QBuffer buffer;
    buffer.setData(encoded);
    require(buffer.open(QIODevice::ReadOnly), "could not open PNG input");
    QImageReader reader(&buffer, "png");
    if (scaled.isValid())
        reader.setScaledSize(scaled);
#if SNOW_SHOT_BENCHMARK_MANAGED_READER
    if (managed)
        return snow_shot::image_codec::readManagedImage(reader);
#else
    Q_UNUSED(managed);
#endif
    return reader.read();
}

ScreenshotRecognitionImageSnapshot recognitionSnapshot(const QImage& image, int lineCount) {
    ScreenshotRecognitionImageSnapshot snapshot;
    snapshot.image = image;
    snapshot.canvasRect = QRectF(QPointF(), QSizeF(image.size()));
    snapshot.font = QFont(QStringLiteral("Arial"), 12);
    snapshot.textColor = Qt::black;
    for (int i = 0; i < lineCount; ++i) {
        const qreal x = (i % 5) * image.width() / 5.0;
        const qreal y = (i / 5) * image.height() / 100.0;
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Recognized translation %1").arg(i);
        line.quad = {
            {x, y}, {x + image.width() / 5.0, y}, {x + image.width() / 5.0, y + 20}, {x, y + 20}};
        snapshot.lines.push_back(line);
    }
    return snapshot;
}

QJsonObject distribution(std::vector<double> values) {
    require(!values.empty(), "empty timing series");
    std::sort(values.begin(), values.end());
    const auto percentile = [&](double fraction) {
        const auto index =
            static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size())));
        return values[std::clamp(index, std::size_t{1}, values.size()) - 1];
    };
    return {{QStringLiteral("p50_ms"), percentile(0.50)},
            {QStringLiteral("p95_ms"), percentile(0.95)},
            {QStringLiteral("p99_ms"), percentile(0.99)}};
}

struct Fixture {
    QImage source;
    QByteArray encoded;
    std::unique_ptr<QMimeData> mime;
    std::optional<ScreenshotRecognitionImageSnapshot> recognition;
    std::optional<ScreenshotClipboardContent> clipboardContent;
    ScreenshotResultStyle style;
    QSize scaled;
    std::function<QImage()> operation;
    bool clearCacheBefore = false;
};

const QStringList& scenarios() {
    static const QStringList values{
        QStringLiteral("recognition-native-20"),  QStringLiteral("recognition-rgba-20"),
        QStringLiteral("recognition-native-500"), QStringLiteral("compositor-rounded"),
        QStringLiteral("compositor-region-cold"), QStringLiteral("compositor-region-warm"),
        QStringLiteral("clipboard-image-import"), QStringLiteral("clipboard-rich-text"),
        QStringLiteral("qt-reader-rgba"),         QStringLiteral("qt-reader-scaled"),
        QStringLiteral("qt-reader-rgba64")};
    return values;
}

void configureFixture(Fixture& fixture, const QString& scenario, QSize size) {
    const auto format = scenario == QStringLiteral("qt-reader-rgba64") ? QImage::Format_RGBA64
                        : scenario == QStringLiteral("recognition-rgba-20") ||
                                scenario.startsWith(QStringLiteral("qt-reader")) ||
                                scenario.startsWith(QStringLiteral("clipboard"))
                            ? QImage::Format_RGBA8888
                            : QImage::Format_ARGB32_Premultiplied;
    fixture.source = patternedImage(size, format);
    if (scenario.startsWith(QStringLiteral("recognition"))) {
        fixture.recognition = recognitionSnapshot(
            fixture.source, scenario.endsWith(QStringLiteral("500")) ? 500 : 20);
        fixture.operation = [&fixture] {
            return renderScreenshotRecognitionImage(*fixture.recognition);
        };
    } else if (scenario.startsWith(QStringLiteral("compositor"))) {
        fixture.style.cornerRadius = 18;
        fixture.style.shadowWidth = 16;
        if (scenario.contains(QStringLiteral("region"))) {
            fixture.style.region =
                QRegion(QRect(QPoint(), size))
                    .subtracted(QRegion(size.width() / 3, 0, size.width() / 4, size.height() / 2));
            fixture.clearCacheBefore = scenario.endsWith(QStringLiteral("cold"));
        }
        fixture.operation = [&fixture] {
            return ScreenshotResultCompositor::compose(fixture.source, fixture.style);
        };
    } else if (scenario == QStringLiteral("clipboard-image-import")) {
        fixture.mime = std::make_unique<QMimeData>();
        fixture.mime->setImageData(fixture.source);
        fixture.operation = [&fixture] {
            auto snapshot = ScreenshotClipboardContentReader::snapshotMimeData(fixture.mime.get(),
                                                                               1.0, Qt::white);
            require(snapshot.has_value(), "clipboard snapshot failed");
            fixture.clipboardContent =
                ScreenshotClipboardContentReader::decode(std::move(*snapshot));
            require(fixture.clipboardContent.has_value() && fixture.clipboardContent->isValid(),
                    "clipboard import failed");
            return fixture.clipboardContent->image;
        };
    } else if (scenario == QStringLiteral("clipboard-rich-text")) {
        fixture.encoded = pngBytes(fixture.source);
        fixture.mime = std::make_unique<QMimeData>();
        fixture.mime->setHtml(
            QStringLiteral("<html><body><p>Deterministic clipboard content</p>"
                           "<img src=\"data:image/png;base64,%1\" width=\"%2\" height=\"%3\">"
                           "<p>End of content</p></body></html>")
                .arg(QString::fromLatin1(fixture.encoded.toBase64()))
                .arg(std::min(960, size.width()))
                .arg(std::max(1, size.height() / 2)));
        fixture.operation = [&fixture] {
            auto snapshot = ScreenshotClipboardContentReader::snapshotMimeData(fixture.mime.get(),
                                                                               2.0, Qt::white);
            require(snapshot.has_value(), "rich text snapshot failed");
            fixture.clipboardContent =
                ScreenshotClipboardContentReader::decode(std::move(*snapshot));
            require(fixture.clipboardContent.has_value() &&
                        fixture.clipboardContent->isFormattedText(),
                    "rich text render failed");
            return fixture.clipboardContent->image;
        };
    } else {
        fixture.encoded = pngBytes(fixture.source);
        if (scenario == QStringLiteral("qt-reader-scaled"))
            fixture.scaled = QSize(std::max(1, size.width() - 3), std::max(1, size.height() - 3));
        fixture.operation = [&fixture] {
            return readerImage(fixture.encoded, fixture.scaled, true);
        };
    }
}

QJsonObject run(const QString& scenario, QSize size, int warmup, int samples, bool verifyOnly) {
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    const auto beforeFixture = footprint();
    Fixture fixture;
    configureFixture(fixture, scenario, size);
    const auto sourceHash = rasterHash(fixture.source);
    const auto fixtureFootprint = footprint();
    QImage verified = fixture.operation();
    require(!verified.isNull(), "benchmark operation returned a null image");
    const auto expectedHash = rasterHash(verified);
    const auto outputSize = verified.size();
    const auto outputFormat = verified.format();
    const auto outputBytes = verified.sizeInBytes();
    if (scenario == QStringLiteral("clipboard-image-import"))
        require(verified == fixture.source && verified.format() == fixture.source.format() &&
                    verified.colorSpace() == fixture.source.colorSpace(),
                "clipboard import changed its source pixels or metadata");
    if (scenario.startsWith(QStringLiteral("compositor"))) {
        const auto layout = ScreenshotResultCompositor::layoutForContent(size, fixture.style);
        require(verified.size() == layout.outputRect.size() &&
                    verified.pixelColor(layout.contentRect.topLeft()).alpha() < 255,
                "rounded compositor geometry or transparency changed");
        if (fixture.style.region)
            require(
                verified.pixelColor(layout.contentRect.topLeft() +
                                    QPoint(size.width() / 3 + size.width() / 8, size.height() / 4))
                        .alpha() == 0,
                "compound compositor exposed pixels in the excluded area");
    }
    if (verifyOnly && scenario.startsWith(QStringLiteral("qt-reader"))) {
        const QImage reference = readerImage(fixture.encoded, fixture.scaled, false);
        require(!reference.isNull() && verified == reference &&
                    verified.format() == reference.format() &&
                    verified.colorSpace() == reference.colorSpace() &&
                    verified.dotsPerMeterX() == reference.dotsPerMeterX() &&
                    verified.dotsPerMeterY() == reference.dotsPerMeterY() &&
                    verified.text(QStringLiteral("Author")) ==
                        reference.text(QStringLiteral("Author")),
                "managed reader differs from the unmodified Qt reference");
    }
    verified = {};
    fixture.clipboardContent = {};
    std::vector<double> createTimes, releaseTimes;
    QJsonArray raw;
    qint64 maximumRetained = 0, maximumReleased = 0;
    std::size_t scratchPeak = 0, cacheRetainedPeak = 0;
    const auto beforeSamples = footprint();
    for (int index = -warmup; !verifyOnly && index < samples; ++index) {
        if (fixture.clearCacheBefore)
            ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
        ScreenshotSelectionShadowRenderer::resetDiagnosticsForCurrentThread();
        const auto before = footprint();
        QElapsedTimer timer;
        timer.start();
        QImage output = fixture.operation();
        const double createMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        const auto retained = footprint();
        require(output.size() == outputSize && output.format() == outputFormat &&
                    rasterHash(output) == expectedHash,
                "benchmark output changed between iterations");
        const auto diagnostics = ScreenshotSelectionShadowRenderer::diagnosticsForCurrentThread();
        timer.restart();
        output = {};
        fixture.clipboardContent = {};
        const double releaseMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        const auto released = footprint();
        if (index < 0)
            continue;
        createTimes.push_back(createMs);
        releaseTimes.push_back(releaseMs);
        maximumRetained = std::max(maximumRetained, retained.toInteger());
        maximumReleased = std::max(maximumReleased, released.toInteger());
        scratchPeak = std::max(scratchPeak, diagnostics.regionScratchPeakBytes);
        cacheRetainedPeak = std::max(cacheRetainedPeak, diagnostics.regionCacheRetainedBytes);
        raw.append(QJsonObject{{QStringLiteral("index"), index},
                               {QStringLiteral("create_ms"), createMs},
                               {QStringLiteral("release_ms"), releaseMs},
                               {QStringLiteral("footprint_before_bytes"), before},
                               {QStringLiteral("footprint_with_output_bytes"), retained},
                               {QStringLiteral("footprint_after_release_bytes"), released},
                               {QStringLiteral("region_mask_builds"),
                                static_cast<qint64>(diagnostics.regionMaskBuilds)},
                               {QStringLiteral("region_shadow_builds"),
                                static_cast<qint64>(diagnostics.regionShadowBuilds)}});
    }
    require(rasterHash(fixture.source) == sourceHash, "benchmark mutated its source image");
    const auto afterSamples = footprint();
    ScreenshotSelectionShadowRenderer::resetCacheForCurrentThread();
    fixture.operation = {};
    fixture.recognition = {};
    fixture.mime = {};
    fixture.source = {};
    fixture.encoded = {};
    fixture.style = {};
    const auto afterFixtureRelease = footprint();
    const auto finalMemory = snow::test_support::memorySnapshot();
    return {
        {QStringLiteral("scenario"), scenario},
        {QStringLiteral("width"), size.width()},
        {QStringLiteral("height"), size.height()},
        {QStringLiteral("source_sha256"), QString::fromLatin1(sourceHash)},
        {QStringLiteral("output_sha256"), QString::fromLatin1(expectedHash)},
        {QStringLiteral("output_width"), outputSize.width()},
        {QStringLiteral("output_height"), outputSize.height()},
        {QStringLiteral("output_format"), static_cast<int>(outputFormat)},
        {QStringLiteral("output_bytes"), static_cast<qint64>(outputBytes)},
        {QStringLiteral("create"), verifyOnly ? QJsonObject{} : distribution(createTimes)},
        {QStringLiteral("release"), verifyOnly ? QJsonObject{} : distribution(releaseTimes)},
        {QStringLiteral("footprint_before_fixture_bytes"), beforeFixture},
        {QStringLiteral("footprint_with_fixture_bytes"), fixtureFootprint},
        {QStringLiteral("footprint_before_samples_bytes"), beforeSamples},
        {QStringLiteral("footprint_after_samples_bytes"), afterSamples},
        {QStringLiteral("footprint_after_fixture_release_bytes"), afterFixtureRelease},
        {QStringLiteral("final_resident_bytes"), static_cast<qint64>(finalMemory.residentBytes)},
        {QStringLiteral("process_peak_resident_bytes"),
         static_cast<qint64>(finalMemory.peakResidentBytes)},
        {QStringLiteral("maximum_sampled_with_output_footprint_bytes"),
         maximumRetained ? QJsonValue(maximumRetained) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("maximum_sampled_after_release_footprint_bytes"),
         maximumReleased ? QJsonValue(maximumReleased) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("region_scratch_peak_bytes"), static_cast<qint64>(scratchPeak)},
        {QStringLiteral("region_cache_retained_peak_bytes"),
         static_cast<qint64>(cacheRetainedPeak)},
        {QStringLiteral("raw"), raw}};
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QGuiApplication::setFont(QFont(QStringLiteral("Arial"), 12));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("scenario"), QStringLiteral("Run a single scenario"),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("width"), QStringLiteral("Fixture width"),
                      QStringLiteral("pixels"), QStringLiteral("1920")});
    parser.addOption({QStringLiteral("height"), QStringLiteral("Fixture height"),
                      QStringLiteral("pixels"), QStringLiteral("1080")});
    parser.addOption({QStringLiteral("warmup"), QStringLiteral("Warmup iterations"),
                      QStringLiteral("count"), QStringLiteral("3")});
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Measured iterations"),
                      QStringLiteral("count"), QStringLiteral("30")});
    parser.addOption({QStringLiteral("list"), QStringLiteral("List supported scenarios")});
    parser.addOption({QStringLiteral("verify-only"),
                      QStringLiteral("Validate outputs and Qt equivalence without timing")});
    parser.process(app);
    if (parser.isSet(QStringLiteral("list"))) {
        for (const auto& scenario : scenarios())
            std::cout << scenario.toStdString() << '\n';
        return 0;
    }
    try {
        const QSize size(parser.value(QStringLiteral("width")).toInt(),
                         parser.value(QStringLiteral("height")).toInt());
        const int warmup = parser.value(QStringLiteral("warmup")).toInt();
        const int samples = parser.value(QStringLiteral("samples")).toInt();
        const bool verifyOnly = parser.isSet(QStringLiteral("verify-only"));
        require(size.width() > 0 && size.height() > 0 && size.width() <= 7680 &&
                    size.height() <= 4320 && warmup >= 0 && samples >= 1 && samples <= 10000,
                "invalid fixture dimensions or sample count");
        QStringList selected = scenarios();
        if (parser.isSet(QStringLiteral("scenario"))) {
            const auto id = parser.value(QStringLiteral("scenario"));
            require(selected.contains(id), "unknown scenario");
            selected = {id};
        }
        QJsonArray reports;
        for (const auto& id : selected)
            reports.append(run(id, size, warmup, samples, verifyOnly));
        const QJsonObject report{
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("benchmark"), QStringLiteral("screenshot_memory_paths")},
            {QStringLiteral("platform"), QSysInfo::prettyProductName()},
            {QStringLiteral("qt_version"), QString::fromLatin1(qVersion())},
            {QStringLiteral("managed_reader"), SNOW_SHOT_BENCHMARK_MANAGED_READER != 0},
            {QStringLiteral("verify_only"), verifyOnly},
            {QStringLiteral("warmup"), warmup},
            {QStringLiteral("samples"), samples},
            {QStringLiteral("memory_metric"),
             QStringLiteral(
                 "macOS Mach phys_footprint or Windows private committed bytes; footprint is null "
                 "on Linux. Resident and process peak resident bytes are reported on all supported "
                 "platforms and include fixture setup. "
                 "Checkpoint maxima exclude unobserved transient allocations. "
                 "Use one scenario per fresh process and an external peak RSS monitor.")},
            {QStringLiteral("timing_scope"),
             QStringLiteral("production operation and output destruction measured separately; "
                            "fixture setup, validation, hashing and memory reads excluded")},
            {QStringLiteral("scenarios"), reports}};
        std::cout << QJsonDocument(report).toJson(QJsonDocument::Compact).constData() << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
