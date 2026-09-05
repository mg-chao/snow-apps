#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUuid>

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

QImage patternedImage(const QSize& size, int seed) {
    QImage image(size, QImage::Format_ARGB32);
    for (int y = 0; y < size.height(); ++y) {
        for (int x = 0; x < size.width(); ++x) {
            image.setPixel(x, y,
                           qRgb((x * 7 + seed) % 256, (y * 13 + seed * 3) % 256,
                                ((x + y) * 5 + seed * 11) % 256));
        }
    }
    return image;
}

bool samePixels(const QImage& first, const QImage& second) {
    return first.size() == second.size() && first.convertToFormat(QImage::Format_ARGB32) ==
                                                second.convertToFormat(QImage::Format_ARGB32);
}

storage::PinnedWindowRecord recordWithId(const QString& id, const QImage& image) {
    storage::PinnedWindowRecord value;
    value.id = id;
    value.image = image;
    value.nativeGeometry = QRect(0, 0, 2, 2);
    value.canvasSourceRect = QRectF(0, 0, 2, 2);
    value.contentCanvasRect = QRectF(0, 0, 2, 2);
    value.surfaceCanvasRect = QRectF(0, 0, 2, 2);
    value.initialPhysicalSize = image.size();
    value.screenDpi = 1.0;
    value.firstCreationTextDpi = 1.0;
    value.scalePercent = 100.0;
    value.opacityPercent = 100;
    return value;
}

QString payloadFilePath(const QString& root, const QString& id) {
    return QDir(root).filePath(QStringLiteral("pinned_windows_v3/pins/%1/source.png").arg(id));
}

QByteArray pngBytes(const QImage& image, int compression) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG", compression),
            "failed to encode PNG test data");
    return bytes;
}

QByteArray readBytes(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "failed to read committed payload");
    return file.readAll();
}

void preparedSourceIsWrittenOnceAndStateUpdatesPreserveIt() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QImage resident = patternedImage(QSize(29, 13), 3);
    const QImage persisted = patternedImage(QSize(29, 13), 19);
    storage::PinnedWindowRecord record = recordWithId(id, resident);
    record.canvasSession = QByteArrayLiteral("canvas-1");
    const auto sharedBytes = std::make_shared<const QByteArray>(pngBytes(persisted, 8));
    const auto prepared = storage::PreparedPngImage::fromBytes(persisted.size(), sharedBytes);
    require(prepared.has_value(), "prepared pinned PNG was rejected");

    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    require(repository.create(record, *prepared).success,
            "failed to create a pinned source from prepared PNG bytes");
    require(repository.flush().success, "failed to flush the prepared pinned source");
    const QString sourcePath = payloadFilePath(directory.path(), id);
    require(readBytes(sourcePath) == *sharedBytes,
            "pinned storage replaced the prepared source bytes");
    const auto loaded = repository.loadRecord(id);
    require(loaded.has_value() && samePixels(loaded->image, persisted),
            "pinned storage did not load the prepared source image");

    record.nativeGeometry.moveTo(31, 47);
    record.canvasSession = QByteArrayLiteral("canvas-2");
    require(repository.updateState(record).success,
            "failed to update pinned metadata and session state");
    require(repository.flush().success, "failed to flush the pinned state update");
    require(readBytes(sourcePath) == *sharedBytes,
            "a pinned state update rewrote the immutable source image");
    const auto updated = repository.loadRecord(id);
    require(updated.has_value() && updated->nativeGeometry.topLeft() == QPoint(31, 47) &&
                updated->canvasSession == QByteArrayLiteral("canvas-2") &&
                samePixels(updated->image, persisted),
            "pinned state update did not preserve source and update session metadata");

    record.canvasSession.clear();
    require(repository.updateState(record).success, "failed to clear pinned session state");
    require(repository.flush().success, "failed to flush the cleared pinned state");
    storage::PinnedWindowRepository restored(directory.path(), true, 30000);
    const auto cleared = restored.loadRecord(id);
    require(cleared.has_value() && cleared->canvasSession.isEmpty() &&
                samePixels(cleared->image, persisted),
            "cleared pinned state left a stale payload descriptor");
}

// Invariant: payload data is available before the writer commits it and is
// served from disk afterwards. A resident in-memory copy shares the upserted
// QImage's cache key; a disk round-trip produces a fresh one.
void committedPayloadsAreServedFromDisk() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QImage image = patternedImage(QSize(33, 17), 5);
    storage::PinnedWindowRecord record = recordWithId(id, image);
    record.originalHtml = QStringLiteral("<p>original</p>");
    record.originalText = QStringLiteral("original");
    record.resultStyle = QByteArrayLiteral("style");
    record.canvasSession = QByteArrayLiteral("canvas-session");
    record.recognitionResults = QByteArrayLiteral("recognition");
    {
        // The long debounce keeps the writer from committing before flush().
        storage::PinnedWindowRepository repository(directory.path(), true, 30000);
        require(repository.upsert(record).success, "failed to upsert the pinned record");
        const auto resident = repository.loadRecord(id);
        require(resident.has_value() && resident->image.cacheKey() == image.cacheKey(),
                "an uncommitted payload should be served from the resident record");

        require(repository.flush().success, "failed to flush the pinned record");
        const auto lazy = repository.loadRecord(id);
        require(lazy.has_value(), "the committed record disappeared from the repository");
        require(lazy->image.cacheKey() != image.cacheKey(),
                "the committed payload is still served from a resident in-memory copy");
        require(samePixels(lazy->image, image), "the committed image changed on round-trip");
        require(lazy->originalHtml == record.originalHtml &&
                    lazy->originalText == record.originalText &&
                    lazy->resultStyle == record.resultStyle &&
                    lazy->canvasSession == record.canvasSession &&
                    lazy->recognitionResults == record.recognitionResults,
                "the committed payload fields changed on round-trip");
        const QVector<storage::PinnedWindowRecord> all = repository.records();
        require(all.size() == 1 && all.front().image.cacheKey() != image.cacheKey(),
                "records() still serves the committed payload from memory");
    }
    // The lazy form produced by a committing session must reload in a fresh
    // repository instance exactly like the manifest-loaded form.
    storage::PinnedWindowRepository restored(directory.path(), true, 30000);
    const auto reloaded = restored.loadRecord(id);
    require(reloaded.has_value() && samePixels(reloaded->image, image) &&
                reloaded->canvasSession == record.canvasSession &&
                reloaded->originalHtml == record.originalHtml,
            "the committed lazy record did not survive a repository restart");
}

// Invariant: demotion must not corrupt payload identity. A metadata-only
// update after a commit reuses the committed payload instead of re-encoding
// and re-writing it.
void metadataOnlyUpdatesDoNotRewriteCommittedPayloads() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QImage image = patternedImage(QSize(24, 12), 9);
    storage::PinnedWindowRecord record = recordWithId(id, image);
    record.canvasSession = QByteArrayLiteral("canvas-session");

    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    require(repository.upsert(record).success, "failed to upsert the pinned record");
    require(repository.flush().success, "failed to flush the pinned record");
    const QString payloadPath = payloadFilePath(directory.path(), id);
    const QFileInfo payload(payloadPath);
    require(payload.isFile(), "the committed payload file is missing");
    const QDateTime committedAt = payload.lastModified();

    record.nativeGeometry = QRect(16, 12, 2, 2);
    require(repository.upsert(record).success, "failed to upsert the metadata update");
    require(repository.flush().success, "failed to flush the metadata update");
    require(payload.lastModified() == committedAt,
            "a metadata-only update re-wrote the committed payload");

    const auto updated = repository.loadRecord(id);
    require(updated.has_value() && updated->nativeGeometry == QRect(16, 12, 2, 2),
            "the metadata update did not persist");
    require(samePixels(updated->image, image) && updated->canvasSession == record.canvasSession,
            "the payload drifted after a metadata-only update");
}

// Invariant: a genuinely changed payload re-commits and is demoted again.
void changedPayloadsRecommitAndStayLazy() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QImage first = patternedImage(QSize(20, 10), 1);
    const QImage second = patternedImage(QSize(20, 10), 2);
    storage::PinnedWindowRecord record = recordWithId(id, first);

    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    require(repository.upsert(record).success, "failed to upsert the pinned record");
    require(repository.flush().success, "failed to flush the pinned record");

    record.image = second;
    require(repository.upsert(record).success, "failed to upsert the changed payload");
    require(repository.flush().success, "failed to flush the changed payload");
    const auto loaded = repository.loadRecord(id);
    require(loaded.has_value() && samePixels(loaded->image, second),
            "the changed payload did not commit");
    require(loaded->image.cacheKey() != second.cacheKey(),
            "the re-committed payload is still served from a resident in-memory copy");
}

// Invariant: a removed record releases its slot, and the payloads written for
// it are pruned from disk, also after the record has been demoted.
void removedRecordsPruneTheirPayloads() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QImage image = patternedImage(QSize(8, 8), 3);
    storage::PinnedWindowRecord record = recordWithId(id, image);

    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    require(repository.upsert(record).success, "failed to upsert the pinned record");
    require(repository.flush().success, "failed to flush the pinned record");
    require(repository.remove(id).success, "failed to remove the pinned record");
    require(repository.flush().success, "failed to flush the removal");
    require(!repository.loadRecord(id).has_value(), "the removed record is still served");
    require(!QFileInfo::exists(payloadFilePath(directory.path(), id)),
            "the removed record's payload survived on disk");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    committedPayloadsAreServedFromDisk();
    preparedSourceIsWrittenOnceAndStateUpdatesPreserveIt();
    metadataOnlyUpdatesDoNotRewriteCommittedPayloads();
    changedPayloadsRecommitAndStayLazy();
    removedRecordsPruneTheirPayloads();
    return 0;
}
