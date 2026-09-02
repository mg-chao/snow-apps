#include "snow_shot/presentation/screenshotscrollingthumbnailwidget.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QWidget>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
constexpr int kPreviewWidth = 128;
constexpr int kTileHeight = 256;
constexpr int kUpdates = 10'000;
constexpr int kSourceRowsPerPreviewRow = 5;

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(1);
}

std::uint64_t checksum(const QImage& image) {
    std::uint64_t value = 1469598103934665603ULL;
    for (qsizetype index = 0; index < image.sizeInBytes(); ++index) {
        value ^= image.constBits()[index];
        value *= 1099511628211ULL;
    }
    return value;
}

QImage referenceContiguousPreview(const std::vector<QImage>& strips) {
    QImage preview;
    for (const QImage& strip : strips) {
        if (preview.isNull()) {
            preview = strip;
            continue;
        }
        QImage next(kPreviewWidth, preview.height() + strip.height(), QImage::Format_RGBA8888);
        if (next.isNull()) {
            fail("reference preview allocation failed");
        }
        QPainter painter(&next);
        painter.drawImage(QPoint(0, 0), preview);
        painter.drawImage(QPoint(0, preview.height()), strip);
        painter.end();
        preview = std::move(next);
    }
    return preview;
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
        fail("could not create tiled preview benchmark artifact");
    }
    file.write(QJsonDocument(artifact).toJson(QJsonDocument::Indented));
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    std::vector<QImage> strips;
    std::vector<QImage> edgePatches;
    strips.reserve(kUpdates);
    edgePatches.reserve(kUpdates);
    for (int update = 0; update < kUpdates; ++update) {
        QImage strip(kPreviewWidth, 1, QImage::Format_RGBA8888);
        strip.fill(QColor(update & 0xff, (update * 3) & 0xff, (update * 7) & 0xff, 255));
        strips.push_back(std::move(strip));
    }
    edgePatches.push_back(strips.front());
    for (int update = 1; update < kUpdates; ++update) {
        QImage patch(kPreviewWidth, 2, QImage::Format_RGBA8888);
        std::memcpy(patch.scanLine(0), strips[static_cast<size_t>(update - 1)].constScanLine(0),
                    static_cast<size_t>(patch.bytesPerLine()));
        std::memcpy(patch.scanLine(1), strips[static_cast<size_t>(update)].constScanLine(0),
                    static_cast<size_t>(patch.bytesPerLine()));
        edgePatches.push_back(std::move(patch));
    }

    QElapsedTimer referenceTimer;
    referenceTimer.start();
    const QImage reference = referenceContiguousPreview(strips);
    const double referenceMilliseconds = static_cast<double>(referenceTimer.nsecsElapsed()) / 1'000'000.0;

    QWidget parent;
    ScreenshotScrollingThumbnailWidget thumbnail(parent);
    QElapsedTimer tiledTimer;
    tiledTimer.start();
    for (int update = 0; update < kUpdates; ++update) {
        thumbnail.setStitchedImage(edgePatches[static_cast<size_t>(update)],
                                   QSize(kPreviewWidth * kSourceRowsPerPreviewRow,
                                         (update + 1) * kSourceRowsPerPreviewRow),
                                   update == 0 ? ScreenshotScrollingStitchChange::Initial
                                               : ScreenshotScrollingStitchChange::AppendedDown,
                                   kSourceRowsPerPreviewRow, false, update == 0 ? 0 : 1);
    }
    const double tiledMilliseconds = static_cast<double>(tiledTimer.nsecsElapsed()) / 1'000'000.0;
    const QImage tiled = thumbnail.previewImageForTesting();

    const std::uint64_t referenceChecksum = checksum(reference);
    const std::uint64_t tiledChecksum = checksum(tiled);
    const bool checksumMatches = referenceChecksum == tiledChecksum && reference.size() == tiled.size();
    const qsizetype logicalBytes = thumbnail.previewLogicalBytesForTesting();
    const qsizetype allocatedBytes = thumbnail.previewAllocatedBytesForTesting();
    constexpr qsizetype tileBytes = kPreviewWidth * kTileHeight * 4;
    const bool storageBounded = allocatedBytes <= logicalBytes + tileBytes;
    const double improvementPercent = (1.0 - tiledMilliseconds / referenceMilliseconds) * 100.0;

    const QJsonObject artifact{
        {QStringLiteral("updates"), kUpdates},
        {QStringLiteral("reference_contiguous_ms"), referenceMilliseconds},
        {QStringLiteral("tiled_ms"), tiledMilliseconds},
        {QStringLiteral("improvement_percent"), improvementPercent},
        {QStringLiteral("logical_bytes"), static_cast<double>(logicalBytes)},
        {QStringLiteral("allocated_bytes"), static_cast<double>(allocatedBytes)},
        {QStringLiteral("tile_bytes"), static_cast<double>(tileBytes)},
        {QStringLiteral("storage_bounded"), storageBounded},
        {QStringLiteral("checksum_matches"), checksumMatches},
        {QStringLiteral("checksum"), QString::number(referenceChecksum)},
    };
    writeArtifact(artifact);
    std::cout << QJsonDocument(artifact).toJson(QJsonDocument::Compact).constData() << '\n';
    return checksumMatches && storageBounded && improvementPercent >= 80.0 ? 0 : 1;
}
