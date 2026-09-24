#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cstdlib>
#include <iostream>

namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

storage::PinnedWindowRecord makeRecord(const QString& id, QSize sourceSize) {
    storage::PinnedWindowRecord record;
    record.id = id;
    record.sourceKind = storage::PinnedWindowSourceKind::ClipboardText;
    record.originalText = QStringLiteral("Benchmark pinned text source");
    record.nativeGeometry = QRect(QPoint(0, 0), sourceSize);
    record.canvasSourceRect = QRectF(record.nativeGeometry);
    record.contentCanvasRect = record.canvasSourceRect;
    record.surfaceCanvasRect = record.canvasSourceRect;
    record.initialWindowSize = sourceSize;
    return record;
}

double milliseconds(QElapsedTimer& timer) {
    return static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
}

void benchmarkPreviewReads() {
    constexpr int kRecords = 24;
    constexpr int kRounds = 4;
    QTemporaryDir directory;
    require(directory.isValid(), "preview benchmark needs a temporary directory");
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const QByteArray drawing(2 * 1024 * 1024, 'd');
    const QByteArray recognition(2 * 1024 * 1024, 'r');
    QVector<QString> ids;
    for (int index = 0; index < kRecords; ++index) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto record = makeRecord(id, QSize(512, 512));
        record.canvasSession = drawing;
        record.recognitionResults = recognition;
        require(repository.upsert(record).success, "create preview benchmark record");
        ids.push_back(id);
    }
    require(repository.flush().success, "commit preview benchmark records");

    for (const auto& id : ids) {
        require(repository.loadRecord(id).has_value() &&
                    repository.loadPreviewSource(id).has_value(),
                "warm preview benchmark inputs");
    }

    qint64 checksum = 0;
    QElapsedTimer timer;
    timer.start();
    for (int round = 0; round < kRounds; ++round) {
        for (const auto& id : ids) {
            const auto record = repository.loadRecord(id);
            require(record.has_value(), "full record load failed");
            checksum += record->originalText.size() + record->canvasSession.size();
        }
    }
    const double fullMs = milliseconds(timer);

    timer.restart();
    for (int round = 0; round < kRounds; ++round) {
        for (const auto& id : ids) {
            const auto source = repository.loadPreviewSource(id);
            require(source.has_value(), "source-only preview load failed");
            checksum += source->originalText.size();
        }
    }
    const double sourceMs = milliseconds(timer);
    require(checksum > 0, "preview benchmark results were not consumed");
    std::cout << "preview_records=" << kRecords << " rounds=" << kRounds
              << " source_type=text unrelated_payload_mib_per_record=4 full_load_ms=" << fullMs
              << " source_only_ms=" << sourceMs << " speedup=" << fullMs / std::max(0.001, sourceMs)
              << "x\n";
}

void benchmarkBulkRemoval() {
    constexpr int kRecords = 250;
    QTemporaryDir individualDirectory;
    QTemporaryDir batchDirectory;
    require(individualDirectory.isValid() && batchDirectory.isValid(),
            "removal benchmark needs temporary directories");
    storage::PinnedWindowRepository individual(individualDirectory.path(), true, 30000);
    storage::PinnedWindowRepository batch(batchDirectory.path(), true, 30000);
    QVector<QString> ids;
    for (int index = 0; index < kRecords; ++index) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto record = makeRecord(id, QSize(2, 2));
        require(individual.upsert(record).success && batch.upsert(record).success,
                "create removal benchmark records");
        ids.push_back(id);
    }
    require(individual.flush().success && batch.flush().success,
            "commit removal benchmark records");
    int individualNotifications = 0;
    int batchNotifications = 0;
    individual.setChangedCallback([&individualNotifications]() { ++individualNotifications; });
    batch.setChangedCallback([&batchNotifications]() { ++batchNotifications; });

    QElapsedTimer timer;
    timer.start();
    for (const auto& id : ids) {
        require(individual.remove(id).success, "individual removal failed");
    }
    const double individualMs = milliseconds(timer);
    timer.restart();
    require(batch.removeMany(ids).success, "batch removal failed");
    const double batchMs = milliseconds(timer);
    require(individualNotifications == kRecords && batchNotifications == 1 &&
                individual.summaries().isEmpty() && batch.summaries().isEmpty(),
            "removal paths produced different results or notification counts");
    std::cout << "remove_records=" << kRecords << " individual_ms=" << individualMs
              << " batch_ms=" << batchMs << " speedup=" << individualMs / std::max(0.001, batchMs)
              << "x individual_notifications=" << individualNotifications
              << " batch_notifications=" << batchNotifications << '\n';
    individual.setChangedCallback({});
    batch.setChangedCallback({});
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    benchmarkPreviewReads();
    benchmarkBulkRemoval();
    return 0;
}
