#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>

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

struct TrackedImageOwner final {
    QImage image;
    std::shared_ptr<int> lifetime = std::make_shared<int>(0);
};

QImage trackedImage(std::weak_ptr<int>* lifetime, int seed) {
    auto* owner = new TrackedImageOwner{patternedImage(QSize(33, 17), seed)};
    *lifetime = owner->lifetime;
    return QImage(
        owner->image.bits(), owner->image.width(), owner->image.height(),
        owner->image.bytesPerLine(), owner->image.format(),
        [](void* data) { delete static_cast<TrackedImageOwner*>(data); }, owner);
}

bool samePixels(const QImage& first, const QImage& second) {
    if (first.size() != second.size())
        return false;
    // QImage equality also checks color metadata. These round trips assert the
    // pixel contents independently of the decoder's explicit sRGB tag.
    const auto firstArgb = first.convertToFormat(QImage::Format_ARGB32);
    const auto secondArgb = second.convertToFormat(QImage::Format_ARGB32);
    const auto rowBytes = static_cast<std::size_t>(first.width()) * sizeof(QRgb);
    for (int row = 0; row < first.height(); ++row) {
        if (std::memcmp(firstArgb.constScanLine(row), secondArgb.constScanLine(row), rowBytes) != 0)
            return false;
    }
    return true;
}

storage::PinnedWindowRecord recordWithId(const QString& id, const QImage& image) {
    storage::PinnedWindowRecord value;
    value.id = id;
    value.image = image;
    value.nativeGeometry = QRect(0, 0, 2, 2);
    value.canvasSourceRect = QRectF(0, 0, 2, 2);
    value.contentCanvasRect = QRectF(0, 0, 2, 2);
    value.surfaceCanvasRect = QRectF(0, 0, 2, 2);
    value.initialWindowSize = image.size();
    value.screenDpi = 1.0;
    value.firstCreationTextDpi = 1.0;
    value.scalePercent = 100.0;
    value.opacityPercent = 100;
    return value;
}

QString payloadFilePath(const QString& root, const QString& id) {
    return QDir(root).filePath(QStringLiteral("pinned_windows_v2/pins/%1/source.png").arg(id));
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

void stateUpdatesBeforeFirstFlushPreserveRestorableSources() {
    for (int source = 0; source < 3; ++source) {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary storage directory is unavailable");
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString group = QUuid::createUuid().toString(QUuid::WithoutBraces);
        auto record = recordWithId(id, patternedImage(QSize(29, 13), 3));
        if (source == 0) {
            record.image.setPixelColor(0, 0, Qt::transparent);
        }
        record.checkerboardEnabled = source == 0;
        const QImage originalImage = record.image;
        const QByteArray encoded = pngBytes(originalImage, 8);
        if (source == 1) {
            record.sourceKind = storage::PinnedWindowSourceKind::ClipboardText;
            record.image = {};
            record.originalText = QStringLiteral("Pinned text");
            record.originalHtml = QStringLiteral("<b>Pinned text</b>");
        } else if (source == 2) {
            record.sourceKind = storage::PinnedWindowSourceKind::ClipboardImageFile;
            record.originalFileName = QStringLiteral("original.png");
            record.originalFilePath = QDir(directory.path()).filePath(record.originalFileName);
            require(originalImage.save(record.originalFilePath), "save original file source");
        }
        {
            storage::PinnedWindowRepository repository(directory.path(), true, 30000);
            require(repository
                        .setGroups({{QStringLiteral("default"), QStringLiteral("Default"), true},
                                    {group, QStringLiteral("Other"), false}},
                                   QStringLiteral("default"))
                        .success,
                    "create inactive group");
            if (source == 0) {
                const auto prepared = storage::PreparedPngImage::fromBytes(
                    originalImage.size(), std::make_shared<const QByteArray>(encoded));
                require(prepared && repository.create(record, *prepared).success,
                        "create resident prepared source");
            } else {
                require(repository.create(record).success, "create resident clipboard source");
            }
            record.canvasSession = QByteArrayLiteral("annotations");
            record.recognitionResults = QByteArrayLiteral("recognition");
            require(repository.updateState(record).success, "update state before first flush");
            require(repository.setRecordGroup(id, group).success &&
                        repository.setActiveGroup(group).success,
                    "move pin into inactive group and activate it");
            const auto beforeFlush = repository.loadRecord(id);
            require(beforeFlush && beforeFlush->groupId == group &&
                        beforeFlush->checkerboardEnabled == record.checkerboardEnabled &&
                        beforeFlush->canvasSession == record.canvasSession &&
                        beforeFlush->recognitionResults == record.recognitionResults &&
                        (source == 1 ? beforeFlush->originalHtml == record.originalHtml
                                     : samePixels(beforeFlush->image, originalImage)),
                    "group activation must load source and state before first flush");
            // Destruction flushes the same pending record as application shutdown.
        }
        storage::PinnedWindowRepository reopened(directory.path(), false);
        const auto restored = reopened.loadRecord(id);
        require(reopened.summaries().size() == 1 && reopened.activeGroupId() == group && restored &&
                    restored->groupId == group &&
                    restored->checkerboardEnabled == record.checkerboardEnabled &&
                    restored->canvasSession == record.canvasSession &&
                    restored->recognitionResults == record.recognitionResults &&
                    (source == 1 ? restored->originalText == record.originalText &&
                                       restored->originalHtml == record.originalHtml
                                 : samePixels(restored->image, originalImage)),
                "restart must retain group count, source, annotations, and recognition");
    }
}

void allocationAdmissionPrecedesSourceDecode() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto image = patternedImage(QSize(81, 63), 23);
    auto record = recordWithId(id, image);
    record.canvasSession = QByteArrayLiteral("retained editor session");
    record.originalText = QStringLiteral("retained source text");
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const auto prepared = storage::PreparedPngImage::fromBytes(
        image.size(), std::make_shared<const QByteArray>(pngBytes(image, 8)));
    require(prepared && repository.create(record, *prepared).success && repository.flush().success,
            "publish pinned source admission fixture");
    int rejectedCalls = 0;
    require(!repository.loadRecord(
                id,
                [&](qint64 bytes) {
                    ++rejectedCalls;
                    require(bytes > 0, "pin admission must include retained text/session payloads");
                    return false;
                }) &&
                rejectedCalls == 1,
            "rejected pin payload must stop before reading or decoding the image");
    qint64 peak = 0;
    const auto accepted = repository.loadRecord(id, [&](qint64 bytes) {
        peak = std::max(peak, bytes);
        return true;
    });
    require(accepted && samePixels(accepted->image, image) && peak >= 2 * image.sizeInBytes() &&
                accepted->canvasSession == record.canvasSession,
            "pin admission must account for decoded buffers and preserve source payloads");
    int decodedReservations = 0;
    require(!repository.loadRecord(id,
                                   [&](qint64 bytes) {
                                       if (bytes >= 2 * image.sizeInBytes()) {
                                           ++decodedReservations;
                                           return false;
                                       }
                                       return true;
                                   }) &&
                decodedReservations == 1,
            "encoded pin may be admitted while decoded raster allocation is rejected");
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
    require(
        !QFileInfo::exists(
            QDir(directory.path())
                .filePath(QStringLiteral("pinned_windows_v2/pins/%1/canvas_session.bin").arg(id))),
        "a committed state removal must prune its obsolete payload file");
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

void manifestFailuresReleaseWrittenPayloadsAndRetryMetadata() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString manifestPath =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    const QImage expected = patternedImage(QSize(33, 17), 7);
    {
        storage::PinnedWindowRepository repository(directory.path(), true, 30000);
        require(QDir().mkpath(manifestPath), "block the manifest commit");
        std::weak_ptr<int> lifetime;
        auto record = recordWithId(id, trackedImage(&lifetime, 7));
        record.originalHtml = QStringLiteral("<p>original</p>");
        record.originalText = QStringLiteral("original");
        record.resultStyle = QByteArrayLiteral("style");
        record.canvasSession = QByteArrayLiteral("annotations");
        record.recognitionResults = QByteArrayLiteral("recognition");
        require(repository.upsert(std::move(record)).success &&
                    repository.markClosedDeferred(id).success,
                "save and close a pin before its first commit");
        require(!repository.flush().success && lifetime.expired(),
                "a manifest failure must release completely written image buffers");
        const auto loaded = repository.loadRecord(id);
        require(loaded && loaded->ignored && samePixels(loaded->image, expected) &&
                    loaded->originalHtml == QStringLiteral("<p>original</p>") &&
                    loaded->originalText == QStringLiteral("original") &&
                    loaded->resultStyle == QByteArrayLiteral("style") &&
                    loaded->canvasSession == QByteArrayLiteral("annotations") &&
                    loaded->recognitionResults == QByteArrayLiteral("recognition"),
                "uncommitted metadata must still reload its written payloads");
        QFile source(payloadFilePath(directory.path(), id));
        const QDateTime originalTimestamp =
            QDateTime::fromString(QStringLiteral("2000-01-01T00:00:00Z"), Qt::ISODate);
        require(source.open(QIODevice::ReadWrite) &&
                    source.setFileTime(originalTimestamp, QFileDevice::FileModificationTime),
                "stamp the independently written source");
        source.close();
        require(QDir(manifestPath).removeRecursively() && repository.flush().success,
                "retry the manifest after storage recovers");
        require(QFileInfo(source.fileName()).lastModified() == originalTimestamp,
                "a manifest retry must not rewrite already durable payloads");
    }
    storage::PinnedWindowRepository reopened(directory.path(), false);
    const auto restored = reopened.loadRecord(id);
    require(restored && restored->ignored && samePixels(restored->image, expected) &&
                restored->canvasSession == QByteArrayLiteral("annotations"),
            "recovered manifest metadata and payloads must survive restart");
}

void partialPayloadFailuresDemoteOnlyCompleteRevisions() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const QString firstId = QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QString secondId = QStringLiteral("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    const QString blockedSession = QDir(directory.path())
                                       .filePath(QStringLiteral("pinned_windows_v2/pins/%1/"
                                                                "canvas_session.bin")
                                                     .arg(secondId));
    require(QDir().mkpath(blockedSession), "block the second record's session write");
    std::weak_ptr<int> firstLifetime;
    std::weak_ptr<int> secondLifetime;
    auto first = recordWithId(firstId, trackedImage(&firstLifetime, 11));
    first.canvasSession = QByteArrayLiteral("first annotations");
    auto second = recordWithId(secondId, trackedImage(&secondLifetime, 13));
    second.canvasSession = QByteArrayLiteral("second annotations");
    require(repository.upsert(std::move(first)).success &&
                repository.upsert(std::move(second)).success && !repository.flush().success,
            "fail after writing one complete record and part of the next");
    require(firstLifetime.expired() && !secondLifetime.expired(),
            "only the completely written payload revision may release its image");
    {
        const auto loaded = repository.loadRecord(secondId);
        require(loaded && loaded->canvasSession == QByteArrayLiteral("second annotations") &&
                    samePixels(loaded->image, patternedImage(QSize(33, 17), 13)),
                "a partially written record must preserve its complete resident source");
    }
    std::weak_ptr<int> replacementLifetime;
    auto replacement = recordWithId(firstId, trackedImage(&replacementLifetime, 17));
    replacement.canvasSession = QByteArrayLiteral("replacement annotations");
    require(repository.upsertExisting(std::move(replacement)).success &&
                !replacementLifetime.expired(),
            "an older written revision must not demote a new source replacement");
    require(QDir(blockedSession).removeRecursively() && repository.flush().success &&
                replacementLifetime.expired() && secondLifetime.expired(),
            "successful retry must release the remaining original image buffers");
    const auto loaded = repository.loadRecord(firstId);
    require(loaded && loaded->canvasSession == QByteArrayLiteral("replacement annotations") &&
                samePixels(loaded->image, patternedImage(QSize(33, 17), 17)),
            "retry must reload the replacement rather than an older source revision");

    const QString abandonedId = QStringLiteral("ffffffff-ffff-4fff-8fff-ffffffffffff");
    const QString abandonedDirectory =
        QDir(directory.path())
            .filePath(QStringLiteral("pinned_windows_v2/pins/%1").arg(abandonedId));
    require(QDir().mkpath(QDir(abandonedDirectory).filePath(QStringLiteral("canvas_session.bin"))),
            "block a payload that will be deleted before its first manifest commit");
    auto abandoned = recordWithId(abandonedId, patternedImage(QSize(33, 17), 31));
    abandoned.canvasSession = QByteArrayLiteral("abandoned annotations");
    require(repository.upsert(std::move(abandoned)).success && !repository.flush().success &&
                repository.remove(abandonedId).success && repository.flush().success &&
                !QFileInfo::exists(abandonedDirectory),
            "deleting a partially written record must reclaim its uncommitted payload directory");
}

void failedManifestSourceReplacementsKeepCurrentPayloadDescriptors() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const QString manifestPath =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    require(QDir().mkpath(manifestPath), "block replacement manifest commits");
    const QString importedPath = QDir(directory.path()).filePath(QStringLiteral("imported.png"));
    const QImage expected = patternedImage(QSize(33, 17), 19);
    require(expected.save(importedPath), "save replacement file source");
    for (int source = 0; source < 3; ++source) {
        auto record = recordWithId(id, expected);
        record.canvasSession = QByteArrayLiteral("old annotations");
        record.recognitionResults = QByteArrayLiteral("old recognition");
        if (source == 1) {
            record.sourceKind = storage::PinnedWindowSourceKind::ClipboardImageFile;
            record.originalFilePath = importedPath;
            record.originalFileName = QStringLiteral("imported.png");
        } else if (source == 2) {
            record.sourceKind = storage::PinnedWindowSourceKind::ClipboardText;
            record.image = {};
            record.originalText = QStringLiteral("replacement text");
            record.originalHtml = QStringLiteral("<b>replacement text</b>");
        }
        require(repository.upsert(std::move(record)).success && !repository.flush().success,
                "write a source replacement while its manifest remains blocked");
        const auto loaded = repository.loadRecord(id);
        require(loaded && loaded->canvasSession == QByteArrayLiteral("old annotations") &&
                    (source == 2
                         ? loaded->sourceKind == storage::PinnedWindowSourceKind::ClipboardText &&
                               loaded->originalText == QStringLiteral("replacement text") &&
                               loaded->originalHtml == QStringLiteral("<b>replacement text</b>")
                         : samePixels(loaded->image, expected)),
                "each written replacement must reload its own source kind and descriptor");
        if (source == 1) {
            require(
                QFile::remove(importedPath) && !repository.flush().success &&
                    repository.loadRecord(id).has_value(),
                "manifest retry must use the private file after its external source disappears");
        }
    }
    auto state = *repository.loadRecord(id);
    state.canvasSession = QByteArrayLiteral("new annotations");
    state.recognitionResults = QByteArrayLiteral("new recognition");
    require(repository.updateState(state).success, "update demoted state before retry");
    const auto beforeFlush = repository.loadRecord(id);
    require(beforeFlush && beforeFlush->canvasSession == state.canvasSession &&
                beforeFlush->recognitionResults == state.recognitionResults,
            "lazy source reload must not overwrite newer resident state with older disk bytes");
    require(!repository.flush().success && QDir(manifestPath).removeRecursively() &&
                repository.flush().success,
            "commit the final replacement after manifest recovery");
    storage::PinnedWindowRepository reopened(directory.path(), false);
    const auto restored = reopened.loadRecord(id);
    require(restored && restored->sourceKind == storage::PinnedWindowSourceKind::ClipboardText &&
                restored->canvasSession == state.canvasSession &&
                restored->recognitionResults == state.recognitionResults &&
                !QFileInfo::exists(payloadFilePath(directory.path(), id)),
            "final metadata must restore current text/state and prune obsolete image payloads");
}

void continuousChangesDoNotPostponePayloadWrites() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    storage::PinnedWindowRepository repository(directory.path(), true, 200);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(33, 17), 23));
    require(repository.upsert(record).success, "queue a source with a debounce deadline");
    const QString manifestPath =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    QElapsedTimer timeout;
    timeout.start();
    while (!QFileInfo::exists(manifestPath) && timeout.elapsed() < 3000) {
        record.opacityPercent = record.opacityPercent == 100 ? 99 : 100;
        require(repository.updateState(record).success, "keep changing queued pin metadata");
        QThread::msleep(1);
    }
    require(QFileInfo::exists(manifestPath),
            "continuous mutations must not postpone the first payload commit indefinitely");
    const auto loaded = repository.loadRecord(id);
    require(loaded && loaded->image.cacheKey() != record.image.cacheKey(),
            "the continuously updated record must release its original resident source");
}

void residentPayloadPressureBypassesLongDebounce() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::weak_ptr<int> lifetime;
    auto record = recordWithId(id, trackedImage(&lifetime, 29));
    constexpr qsizetype bytes = 24 * 1024 * 1024;
    record.resultStyle = QByteArray(bytes, 's');
    record.canvasSession = QByteArray(bytes, 'c');
    record.recognitionResults = QByteArray(bytes, 'r');
    require(repository.upsert(std::move(record)).success, "queue a large resident payload batch");
    QElapsedTimer timeout;
    timeout.start();
    while (!lifetime.expired() && timeout.elapsed() < 3000)
        QThread::msleep(1);
    require(lifetime.expired(),
            "resident payload pressure must trigger a write before the long debounce expires");
    const auto loaded = repository.loadRecord(id);
    require(loaded && loaded->resultStyle.size() == bytes && loaded->resultStyle.front() == 's' &&
                loaded->canvasSession.size() == bytes && loaded->canvasSession.front() == 'c' &&
                loaded->recognitionResults.size() == bytes &&
                loaded->recognitionResults.front() == 'r',
            "pressure-triggered demotion must preserve every state payload");
}

void missingOptionalPayloadDoesNotHideRestorableImage() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QImage image = patternedImage(QSize(33, 17), 8);
    auto record = recordWithId(id, image);
    record.canvasSession = QByteArrayLiteral("drawing");
    record.recognitionResults = QByteArrayLiteral("recognition");
    const QString missingPath =
        QDir(directory.path())
            .filePath(QStringLiteral("pinned_windows_v2/pins/%1/recognition_results.bin").arg(id));
    {
        storage::PinnedWindowRepository repository(directory.path(), true, 30000);
        require(repository.upsert(record).success && repository.flush().success,
                "commit the pin with optional recognition results");
        require(QFile::remove(missingPath), "simulate an interrupted optional-payload cleanup");
        const auto loaded = repository.loadRecord(id);
        require(loaded && samePixels(loaded->image, image) &&
                    loaded->canvasSession == record.canvasSession &&
                    loaded->recognitionResults.isEmpty(),
                "a missing optional payload must not block full image loading or restore");
    }
    storage::PinnedWindowRepository reopened(directory.path(), true, 30000);
    const auto loaded = reopened.loadRecord(id);
    require(loaded && samePixels(loaded->image, image) &&
                loaded->canvasSession == record.canvasSession &&
                loaded->recognitionResults.isEmpty(),
            "a missing optional payload must not hide the pin after restart");
    require(reopened.markClosed(id).success && reopened.flush().success,
            "persist the recovered record");
    const QJsonObject manifest =
        QJsonDocument::fromJson(
            readBytes(
                QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"))))
            .object();
    const QJsonObject payloads = manifest.value(QStringLiteral("records"))
                                     .toArray()
                                     .first()
                                     .toObject()
                                     .value(QStringLiteral("payloads"))
                                     .toObject();
    require(!payloads.contains(QStringLiteral("recognition_results")),
            "the recovered index must drop the missing optional payload reference");
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

    const QString sessionPath =
        QDir(directory.path())
            .filePath(QStringLiteral("pinned_windows_v2/pins/%1/canvas_session.bin").arg(id));
    const QFileInfo session(sessionPath);
    const QDateTime sessionCommittedAt = session.lastModified();
    record.opacityPercent = 87;
    require(repository.updateState(record).success && repository.flush().success &&
                session.lastModified() == sessionCommittedAt,
            "a state-only update must not rewrite an unchanged drawing session");

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

void specifiedGroupRemovalIsAtomicAndPersistent() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary group-removal storage is unavailable");
    const QString defaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString alphaRecordId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString betaRecordId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString alphaGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString betaGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString missingGroupId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    require(repository
                .setGroups({{QStringLiteral("default"), QStringLiteral("Default"), true},
                            {alphaGroupId, QStringLiteral("Alpha"), false},
                            {betaGroupId, QStringLiteral("Beta"), false}},
                           betaGroupId)
                .success,
            "failed to seed removable groups");
    auto defaultRecord = recordWithId(defaultId, patternedImage(QSize(8, 8), 1));
    auto alphaRecord = recordWithId(alphaRecordId, patternedImage(QSize(8, 8), 2));
    auto betaRecord = recordWithId(betaRecordId, patternedImage(QSize(8, 8), 3));
    alphaRecord.groupId = alphaGroupId;
    betaRecord.groupId = betaGroupId;
    require(repository.upsert(defaultRecord).success && repository.upsert(alphaRecord).success &&
                repository.upsert(betaRecord).success && repository.flush().success,
            "failed to persist group-removal records");

    require(repository.removeGroupAndRecords(QStringLiteral("default")).success,
            "clearing Default should succeed");
    require(repository.groups().size() == 3 && repository.activeGroupId() == betaGroupId &&
                !repository.loadRecord(defaultId).has_value() &&
                repository.loadRecord(alphaRecordId).has_value() &&
                repository.loadRecord(betaRecordId).has_value(),
            "clearing Default must preserve all groups, active selection, and unrelated records");

    require(repository.removeGroupAndRecords(betaGroupId).success,
            "deleting the active custom group should succeed");
    require(
        repository.groups().size() == 2 && repository.activeGroupId() == "default" &&
            !repository.loadRecord(betaRecordId).has_value() &&
            repository.loadRecord(alphaRecordId).has_value(),
        "deleting an active custom group must remove only its records and fall back to Default");
    require(!repository.removeGroupAndRecords(missingGroupId).success &&
                repository.groups().size() == 2 && repository.loadRecord(alphaRecordId).has_value(),
            "an unknown group must fail without partial mutation");
    require(repository.flush().success, "failed to flush specified group removal");
    require(!QFileInfo::exists(payloadFilePath(directory.path(), defaultId)) &&
                !QFileInfo::exists(payloadFilePath(directory.path(), betaRecordId)) &&
                QFileInfo::exists(payloadFilePath(directory.path(), alphaRecordId)),
            "group removal must prune only the deleted records' payloads");

    storage::PinnedWindowRepository restored(directory.path(), true, 30000);
    require(restored.groups().size() == 2 && restored.activeGroupId() == "default" &&
                restored.loadRecord(alphaRecordId).has_value() &&
                !restored.loadRecord(defaultId).has_value() &&
                !restored.loadRecord(betaRecordId).has_value(),
            "specified group removal must survive a repository restart");
    storage::PinnedWindowRepository readOnly(directory.path(), false, 30000);
    require(!readOnly.removeGroupAndRecords(alphaGroupId).success &&
                readOnly.groups().size() == 2 && readOnly.loadRecord(alphaRecordId).has_value(),
            "read-only group removal must fail without changing repository state");
}
void recognitionVisibilityRoundTripsAndDefaultsToHidden() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(8, 8), 3));
    const QString manifest =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    {
        storage::PinnedWindowRepository repository(directory.path());
        record.recognitionVisible = true;
        record.translationVisible = true;
        require(repository.upsert(record).success && repository.flush().success,
                "visible recognition state should be saved");
        require(repository.loadRecord(id)->recognitionVisible &&
                    repository.loadRecord(id)->translationVisible,
                "visible recognition state should survive payload demotion");
        record.recognitionVisible = false;
        record.translationVisible = false;
        require(repository.updateState(record).success && repository.flush().success,
                "hidden recognition state should be saved");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded.has_value() && !loaded->recognitionVisible && !loaded->translationVisible,
                "hidden recognition state should survive reopening");
        record.recognitionVisible = true;
        record.translationVisible = true;
        require(repository.updateState(record).success && repository.flush().success,
                "recognition can be made visible again");
    }
    auto root = QJsonDocument::fromJson(readBytes(manifest)).object();
    auto records = root.value(QStringLiteral("records")).toArray();
    require(records.size() == 1 &&
                records[0].toObject().value(QStringLiteral("recognition_visible")).toBool() &&
                records[0].toObject().value(QStringLiteral("translation_visible")).toBool(),
            "the manifest must explicitly store visible recognition");
    auto legacyRecord = records[0].toObject();
    legacyRecord.remove(QStringLiteral("recognition_visible"));
    legacyRecord.remove(QStringLiteral("translation_visible"));
    records[0] = legacyRecord;
    root.insert(QStringLiteral("records"), records);
    QFile file(manifest);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open legacy manifest fixture");
    const QByteArray legacyBytes = QJsonDocument(root).toJson();
    require(file.write(legacyBytes) == legacyBytes.size(), "write legacy manifest fixture");
    file.close();
    storage::PinnedWindowRepository legacy(directory.path());
    const auto loaded = legacy.loadRecord(id);
    require(loaded.has_value() && !loaded->recognitionVisible && !loaded->translationVisible,
            "records without recognition visibility must default to hidden");
}
void clickThroughStateRoundTripsAndRecoversLegacyOrConflictingMetadata() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary click-through storage is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(200, 100), 7));
    record.clickThroughMode = true;
    record.clickThroughOpacityPercent = 37;
    record.lockedMode = true;
    record.preThumbnailNativeGeometry = record.nativeGeometry;
    record.hideToTopHandleNativeGeometry = QRect(record.nativeGeometry.topLeft(), QSize(30, 6));
    record.hideToTopAccentIndex = 0;
    const QString manifest =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(record).success && repository.flush().success,
                "click-through state must be committed to disk");
        const auto demoted = repository.loadRecord(id);
        require(demoted.has_value() && demoted->clickThroughMode && demoted->lockedMode &&
                    demoted->clickThroughOpacityPercent == 37,
                "click-through state must survive payload demotion");

        record.clickThroughMode = false;
        record.lockedMode = false;
        require(repository.updateState(record).success && repository.flush().success,
                "exiting click-through must update persisted metadata");
        require(!repository.loadRecord(id)->clickThroughMode &&
                    !repository.loadRecord(id)->lockedMode,
                "the repository must expose the persisted click-through exit");

        record.clickThroughMode = true;
        record.lockedMode = true;
        require(repository.updateState(record).success && repository.flush().success,
                "re-entering click-through must update persisted metadata");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded.has_value() && loaded->clickThroughMode && loaded->lockedMode &&
                    loaded->clickThroughOpacityPercent == 37,
                "click-through state must survive repository recreation");
    }

    const auto original = QJsonDocument::fromJson(readBytes(manifest)).object();
    for (int scenario = 0; scenario < 3; ++scenario) {
        auto root = original;
        auto records = root.value(QStringLiteral("records")).toArray();
        auto item = records.at(0).toObject();
        if (scenario == 0) {
            item.remove(QStringLiteral("click_through_mode"));
            item.remove(QStringLiteral("locked_mode"));
        } else if (scenario == 1) {
            item.insert(QStringLiteral("thumbnail_mode"), true);
        } else {
            item.insert(QStringLiteral("hide_to_top_mode"), true);
        }
        records.replace(0, item);
        root.insert(QStringLiteral("records"), records);
        QFile file(manifest);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "open click-through compatibility fixture");
        const QByteArray bytes = QJsonDocument(root).toJson();
        require(file.write(bytes) == bytes.size(), "write click-through compatibility fixture");
        file.close();

        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded.has_value() && !loaded->clickThroughMode && !loaded->lockedMode,
                "legacy and conflicting records must restore as interactive windows");
        if (scenario == 1) {
            require(loaded->thumbnailMode,
                    "thumbnail mode must win a conflicting click-through record");
        } else if (scenario == 2) {
            require(loaded->hideToTopMode,
                    "Hide to Top must win a conflicting click-through record");
        }
    }
    const QList<QJsonValue> opacityValues{QJsonValue(QJsonValue::Undefined),
                                          QJsonValue(),
                                          QJsonValue(-1),
                                          QJsonValue(101),
                                          QJsonValue(37.5),
                                          QJsonValue(QStringLiteral("42")),
                                          QJsonValue(true),
                                          QJsonValue(0),
                                          QJsonValue(100),
                                          QJsonValue(61)};
    for (const auto& value : opacityValues) {
        auto root = original;
        auto records = root.value(QStringLiteral("records")).toArray();
        auto item = records.at(0).toObject();
        item.insert(QStringLiteral("click_through_opacity_percent"), value);
        records.replace(0, item);
        root.insert(QStringLiteral("records"), records);
        QFile file(manifest);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "open opacity metadata fixture");
        const auto bytes = QJsonDocument(root).toJson();
        require(file.write(bytes) == bytes.size(), "write opacity metadata fixture");
        file.close();
        storage::PinnedWindowRepository repository(directory.path());
        auto loaded = repository.loadRecord(id);
        const int expected =
            value.isDouble() && value.toInt(-1) >= 0 && value.toInt(-1) <= 100 ? value.toInt() : 50;
        require(loaded && loaded->clickThroughOpacityPercent == expected &&
                    loaded->opacityPercent == 100 && loaded->clickThroughMode,
                "missing or malformed opacity must recover independently without losing the pin");
        require(repository.updateState(*loaded).success && repository.flush().success,
                "recovered opacity must be writable");
        storage::PinnedWindowRepository reopened(directory.path());
        require(reopened.loadRecord(id)->clickThroughOpacityPercent == expected,
                "opacity endpoints and recovered defaults must survive another round trip");
    }
}
void alwaysOnTopStateRoundTripsAndDefaultsToEnabledForLegacyRecords() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary always-on-top storage is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(200, 100), 5));
    record.alwaysOnTop = false;
    const QString manifest =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(record).success && repository.flush().success,
                "the always-on-top opt-out must be committed to disk");
        const auto demoted = repository.loadRecord(id);
        require(demoted.has_value() && !demoted->alwaysOnTop,
                "the always-on-top opt-out must survive payload demotion");
        record.alwaysOnTop = true;
        require(repository.updateState(record).success && repository.flush().success,
                "re-enabling always-on-top must update persisted metadata");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded.has_value() && loaded->alwaysOnTop,
                "always-on-top state must survive repository recreation");
    }

    // Records saved before the preference existed only ever floated above
    // everything, so a missing key must restore as enabled.
    auto root = QJsonDocument::fromJson(readBytes(manifest)).object();
    auto records = root.value(QStringLiteral("records")).toArray();
    auto item = records.at(0).toObject();
    item.remove(QStringLiteral("always_on_top"));
    records.replace(0, item);
    root.insert(QStringLiteral("records"), records);
    QFile file(manifest);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "open always-on-top legacy fixture");
    const QByteArray bytes = QJsonDocument(root).toJson();
    require(file.write(bytes) == bytes.size(), "write always-on-top legacy fixture");
    file.close();
    storage::PinnedWindowRepository repository(directory.path());
    const auto loaded = repository.loadRecord(id);
    require(loaded.has_value() && loaded->alwaysOnTop,
            "legacy records must restore with always-on-top enabled");
}
void pinSourceIdentitySurvivesRestart() {
    QTemporaryDir directory;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(20, 10), 5));
    const QString sourcePath = directory.filePath(QStringLiteral("original.png"));
    require(record.image.save(sourcePath), "save original file fixture");
    record.sourceKind = storage::PinnedWindowSourceKind::ClipboardImageFile;
    record.originalFilePath = sourcePath;
    record.sourceIdentity = {QStringLiteral("file:") + sourcePath};
    const auto identity = record.sourceIdentity;
    const QString manifest =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(record).success && repository.flush().success,
                "commit source identity");
        const auto stored = repository.loadRecord(id);
        require(stored && stored->sourceIdentity == identity &&
                    stored->originalFilePath != sourcePath,
                "original identity survives rewriting file paths to a private copy");
        record.sourceIdentity = {};
        record.nativeGeometry.translate(20, 30);
        require(repository.updateState(record).success && repository.flush().success,
                "ordinary state updates preserve immutable source identity");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.sourceIdentity(id) == identity,
                "source identity is available before image loading");
        const auto restored = repository.loadRecord(id);
        require(restored && restored->sourceIdentity == identity,
                "source identity survives restart");
    }
    auto root = QJsonDocument::fromJson(readBytes(manifest)).object();
    auto records = root.value(QStringLiteral("records")).toArray();
    auto item = records.first().toObject();
    item.remove(QStringLiteral("pin_source_identity"));
    records.replace(0, item);
    root.insert(QStringLiteral("records"), records);
    QFile file(manifest);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open legacy identity fixture");
    file.write(QJsonDocument(root).toJson());
    file.close();
    storage::PinnedWindowRepository repository(directory.path());
    const auto legacy = repository.loadRecord(id);
    require(legacy && !legacy->sourceIdentity.isValid(),
            "legacy records remain readable without invented identity");
}

void showBorderStateRoundTripsAndDefaultsToEnabledForLegacyRecords() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary show border storage is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(200, 100), 5));
    record.showBorder = false;
    record.borderAppearance =
        storage::PinnedBorderAppearance{QSize(200, 100), QRectF(8, 8, 184, 84), 16.0, true, {}};
    const QString manifest =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(record).success && repository.flush().success,
                "the show border opt-out must be committed to disk");
        const auto demoted = repository.loadRecord(id);
        require(demoted.has_value() && !demoted->showBorder &&
                    demoted->borderAppearance == record.borderAppearance,
                "the show border opt-out must survive payload demotion");
        record.showBorder = true;
        require(repository.updateState(record).success && repository.flush().success,
                "re-enabling the border must update persisted metadata");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded.has_value() && loaded->showBorder &&
                    loaded->borderAppearance == record.borderAppearance,
                "show border state must survive repository recreation");
    }

    // Records saved before the preference existed always rendered their rim,
    // so a missing key must restore with the border visible.
    auto root = QJsonDocument::fromJson(readBytes(manifest)).object();
    auto records = root.value(QStringLiteral("records")).toArray();
    auto item = records.at(0).toObject();
    item.remove(QStringLiteral("show_border"));
    item.remove(QStringLiteral("border_appearance"));
    records.replace(0, item);
    root.insert(QStringLiteral("records"), records);
    QFile file(manifest);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "open show border legacy fixture");
    const QByteArray bytes = QJsonDocument(root).toJson();
    require(file.write(bytes) == bytes.size(), "write show border legacy fixture");
    file.close();
    storage::PinnedWindowRepository repository(directory.path());
    const auto loaded = repository.loadRecord(id);
    require(loaded.has_value() && loaded->showBorder && !loaded->borderAppearance,
            "legacy records must restore with the border visible");
}
void malformedCustomBorderRejectsRecord() {
    QTemporaryDir directory;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(200, 100), 5));
    record.borderAppearance =
        storage::PinnedBorderAppearance{QSize(200, 100), QRectF(0, 0, 200, 100), 0, false, {}};
    QPainterPath shape;
    shape.addEllipse(QRectF(0, 0, 200, 100));
    record.borderAppearance->region =
        ScreenshotRegionGeometry::fromPath(shape, ScreenshotRegionType::Curve);
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(record).success && repository.flush().success,
                "save custom pin fixture");
    }
    const auto manifest =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    auto root = QJsonDocument::fromJson(readBytes(manifest)).object();
    auto records = root.value(QStringLiteral("records")).toArray();
    auto item = records.first().toObject();
    auto border = item.value(QStringLiteral("border_appearance")).toObject();
    border.insert(QStringLiteral("geometry"), QJsonObject{{QStringLiteral("version"), 999}});
    item.insert(QStringLiteral("border_appearance"), border);
    records[0] = item;
    root.insert(QStringLiteral("records"), records);
    QFile file(manifest);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open malformed pin fixture");
    file.write(QJsonDocument(root).toJson());
    file.close();
    storage::PinnedWindowRepository repository(directory.path());
    require(!repository.loadRecord(id),
            "malformed custom outline must not restore as a bounding rectangle");
}
void thumbnailStateSurvivesRestartAndExit() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary thumbnail storage is unavailable");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(400, 200), 3));
    record.nativeGeometry = QRect(120, 80, 125, 125);
    record.thumbnailMode = true;
    record.preThumbnailNativeGeometry = QRect(50, 40, 800, 400);
    record.screenDpi = 1.5;
    record.scalePercent = 200.0;
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(record).success && repository.flush().success,
                "thumbnail state must be committed to disk");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded.has_value() && loaded->thumbnailMode &&
                    loaded->nativeGeometry == record.nativeGeometry &&
                    loaded->preThumbnailNativeGeometry == record.preThumbnailNativeGeometry &&
                    loaded->scalePercent == record.scalePercent,
                "repository restart must retain the mode and both physical rectangles");
        record.thumbnailMode = false;
        record.nativeGeometry = record.preThumbnailNativeGeometry;
        require(repository.updateState(record).success && repository.flush().success,
                "leaving thumbnail mode must update persisted metadata");
    }
    storage::PinnedWindowRepository repository(directory.path());
    const auto loaded = repository.loadRecord(id);
    require(loaded.has_value() && !loaded->thumbnailMode &&
                loaded->nativeGeometry == record.nativeGeometry,
            "thumbnail exit must survive another repository restart");
}
void hideToTopRoundTripsAndRecoversLegacyMetadata() {
    QTemporaryDir directory;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(200, 100), 2));
    record.nativeGeometry = QRect(-900, 46, 200, 100);
    record.hideToTopMode = true;
    record.hideToTopHandleNativeGeometry = QRect(-900, 40, 30, 6);
    {
        storage::PinnedWindowRepository repository(directory.path());
        record.hideToTopAccentIndex = repository.allocateHideToTopAccent();
        require(record.hideToTopAccentIndex == 0, "first accent starts at blue");
        require(repository.upsert(record).success && repository.flush().success,
                "hide-to-top metadata must commit");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded && loaded->hideToTopMode && loaded->hideToTopAccentIndex == 0 &&
                    loaded->hideToTopHandleNativeGeometry == record.hideToTopHandleNativeGeometry &&
                    loaded->nativeGeometry == record.nativeGeometry,
                "hide-to-top geometry and accent must survive restart");
        for (int i = 1; i < 14; ++i) {
            require(repository.allocateHideToTopAccent() == i % 13,
                    "accent allocation must continue across restart and wrap at thirteen");
        }
        record.hideToTopMode = false;
        require(repository.updateState(record).success && repository.flush().success,
                "exit must persist while retaining the assigned color");
    }
    const QString manifest =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    const auto original = QJsonDocument::fromJson(readBytes(manifest)).object();
    for (int scenario = 0; scenario < 3; ++scenario) {
        auto root = original;
        auto records = root.value(QStringLiteral("records")).toArray();
        auto item = records.at(0).toObject();
        if (scenario == 0) {
            item.remove(QStringLiteral("hide_to_top_mode"));
            item.remove(QStringLiteral("hide_to_top_handle_geometry"));
            item.remove(QStringLiteral("hide_to_top_accent_index"));
            root.remove(QStringLiteral("next_hide_to_top_accent"));
        } else {
            item.insert(QStringLiteral("hide_to_top_mode"), true);
            item.insert(scenario == 1 ? QStringLiteral("hide_to_top_accent_index")
                                      : QStringLiteral("hide_to_top_handle_geometry"),
                        QStringLiteral("invalid"));
        }
        records.replace(0, item);
        root.insert(QStringLiteral("records"), records);
        QFile file(manifest);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open test manifest");
        file.write(QJsonDocument(root).toJson());
        file.close();
        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded && !loaded->hideToTopMode && loaded->nativeGeometry == record.nativeGeometry,
                "legacy and malformed hide metadata must retain a recoverable normal window");
    }
}

void precisePlacementAndPreviousVersionIsolation() {
    QTemporaryDir directory;
    const QString oldDirectory = QDir(directory.path()).filePath(QStringLiteral("pinned_windows"));
    require(QDir().mkpath(oldDirectory), "create previous-version fixture");
    const QString oldIndex = QDir(oldDirectory).filePath(QStringLiteral("index.json"));
    QFile oldFile(oldIndex);
    require(oldFile.open(QIODevice::WriteOnly), "write previous-version fixture");
    const QByteArray oldBytes = QByteArrayLiteral("{\"format_version\":1,\"records\":[]}");
    oldFile.write(oldBytes);
    oldFile.close();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = recordWithId(id, patternedImage(QSize(321, 181), 9));
    record.initialWindowSize = QSize(301, 201);
    record.screenDpi = 2.;
    record.placement = {QStringLiteral("Retina"), QStringLiteral("display-serial"),
                        QPointF(-10.5, 38.5), QSize(321, 181)};
    record.preThumbnailPlacement = {QStringLiteral("External"), QStringLiteral("external-serial"),
                                    QPointF(40.25, 60.75), QSize(800, 450)};
    record.hideToTopPlacement = {QStringLiteral("Retina"), QStringLiteral("display-serial"),
                                 QPointF(10.5, 38), QSize(60, 12)};
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(record).success && repository.flush().success,
                "save precise placement");
    }
    storage::PinnedWindowRepository restored(directory.path());
    const auto loaded = restored.loadRecord(id);
    require(loaded && loaded->placement == record.placement &&
                loaded->preThumbnailPlacement == record.preThumbnailPlacement &&
                loaded->hideToTopPlacement == record.hideToTopPlacement &&
                loaded->initialWindowSize == record.initialWindowSize &&
                loaded->image.size() == record.image.size() &&
                loaded->placement.units == storage::kPinnedGeometryUnits,
            "all placement states must retain display identity and fractional point positions");
    require(readBytes(oldIndex) == oldBytes,
            "version two must not modify or reinterpret previous-version storage");
}

void managementLifecycleAndRetention() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const auto now = QDateTime::currentDateTimeUtc();
    const auto make = [&]() {
        return recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                            patternedImage({8, 8}, 1));
    };
    auto first = make();
    first.creationSource = storage::PinnedWindowCreationSource::Clipboard;
    auto second = make();
    auto protectedRecord = make();
    require(repository.upsert(first).success && repository.upsert(second).success &&
                repository.upsert(protectedRecord).success,
            "create management records");
    const auto original = repository.loadRecord(first.id);
    require(original && original->createdUtc.isValid() && !original->ignored,
            "new pins must have creation metadata and be retained");
    require(repository.markClosed(first.id, now).success &&
                repository.markClosed(second.id, now).success,
            "close pins");
    require(repository.loadRecord(second.id)->activitySequence >
                repository.loadRecord(first.id)->activitySequence,
            "equal-time closes must retain deterministic order");
    require(repository.updateState(first).success, "stale snapshot update");
    auto closed = repository.loadRecord(first.id);
    require(closed->ignored &&
                closed->creationSource == storage::PinnedWindowCreationSource::Clipboard &&
                closed->createdUtc == original->createdUtc,
            "state updates must preserve lifecycle metadata");
    auto policy = repository.policy();
    policy.maxEntries = 1;
    require(repository.setPolicy(policy).success, "set closed-record count limit");
    require(!repository.loadRecord(first.id) && repository.loadRecord(second.id) &&
                repository.loadRecord(protectedRecord.id),
            "prune oldest closed pin without affecting retained pins");
    require(!repository.upsertExisting(first).success,
            "late state save must not recreate pruned record");
    require(repository.markRestored(second.id).success &&
                !repository.loadRecord(second.id)->ignored,
            "restore must unignore record");
    require(repository.clearClosed().success && repository.summaries().size() == 2,
            "clear closed must protect restored pins");
    auto pending = make();
    repository.reserveCreation(pending.id);
    require(repository.markClosed(pending.id, now).success && repository.upsert(pending).success &&
                repository.loadRecord(pending.id)->ignored,
            "close before first image publication must survive");
    require(repository.remove(pending.id).success && !repository.upsertExisting(pending).success,
            "destroy blocks late state publication");
    require(repository.markClosed(second.id, now).success, "close before disabling history");
    policy.enabled = false;
    require(repository.setPolicy(policy).success, "disable closed history");
    require(repository.loadRecord(second.id).has_value(),
            "disabling keeps existing closed records");
    require(repository.markClosed(protectedRecord.id, now).success &&
                !repository.loadRecord(protectedRecord.id),
            "future closes delete when disabled");
    require(repository.flush().success, "flush management metadata");
    storage::PinnedWindowRepository reloaded(directory.path(), true, 30000);
    const auto restored = reloaded.loadRecord(second.id);
    require(restored && restored->ignored && restored->lastClosedUtc == now &&
                restored->activitySequence > 0,
            "closed lifecycle survives restart");
    policy.enabled = true;
    policy.retentionDays = 1;
    require(reloaded.setPolicy(policy).success && reloaded.enforcePolicy(now.addDays(2)).success &&
                reloaded.summaries().isEmpty(),
            "retention age uses close date");
}

void managementDiskQuotaAndRestorationProtection() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const auto make = [&]() {
        return recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                            patternedImage({2, 2}, 1));
    };
    auto active = make();
    auto closed = make();
    require(repository.upsert(active).success && repository.upsert(closed).success,
            "seed quota records");
    auto policy = repository.policy();
    policy.keepPermanently = true;
    policy.maxDiskMiB = 128;
    require(repository.setPolicy(policy).success && repository.markClosed(closed.id).success &&
                repository.flush().success,
            "persist quota fixture");
    for (const auto& id : {active.id, closed.id}) {
        QFile payload(payloadFilePath(directory.path(), id));
        require(payload.open(QIODevice::ReadWrite) && payload.resize(129LL * 1024 * 1024),
                "extend payload for deterministic byte accounting");
    }
    require(repository.enforcePolicy().success && repository.summaries().size() == 2,
            "permanent retention bypasses disk quota");
    require(repository.beginRestore(closed.id).success &&
                !repository.beginRestore(closed.id).success,
            "restoration reservation excludes duplicate attempts");
    policy.keepPermanently = false;
    require(repository.setPolicy(policy).success && repository.summaries().size() == 2,
            "quota must protect an in-flight restoration");
    repository.cancelRestore(closed.id);
    require(repository.enforcePolicy().success && repository.summaries().size() == 1 &&
                repository.summaries().front().id == active.id,
            "quota removes closed bytes and never active pins");
}

void membershipAndPreviewRevisionsTrackOnlyTheirSources() {
    QTemporaryDir directory;
    require(directory.isValid(), "revision fixture needs a directory");
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    quint64 previewRevision = 0;
    {
        storage::PinnedWindowRepository repository(directory.path(), true, 30000);
        auto record = recordWithId(id, patternedImage({12, 8}, 1));
        require(repository.upsert(record).success, "create revision fixture");
        const quint64 membershipRevision = repository.membershipRevision();
        previewRevision = *repository.previewSourceRevision(id);
        record.opacityPercent = 70;
        require(repository.updateState(record).success &&
                    repository.membershipRevision() == membershipRevision &&
                    repository.previewSourceRevision(id) == previewRevision,
                "state-only saves must not rebuild membership or thumbnails");
        require(repository.markClosed(id).success &&
                    repository.membershipRevision() > membershipRevision,
                "closing a pin changes membership");
        const quint64 closedRevision = repository.membershipRevision();
        require(repository.markRestored(id).success &&
                    repository.membershipRevision() > closedRevision,
                "restoring a pin changes membership");
        record.image = patternedImage({12, 8}, 2);
        require(repository.upsert(record).success, "change preview source");
        previewRevision = *repository.previewSourceRevision(id);
        require(repository.flush().success, "commit stable preview revision");
    }
    {
        storage::PinnedWindowRepository reloaded(directory.path(), true, 30000);
        require(reloaded.previewSourceRevision(id) == previewRevision,
                "disk thumbnail keys must remain stable across restart");
        require(reloaded.remove(id).success && reloaded.flush().success,
                "remove source before reusing its id");
    }
    storage::PinnedWindowRepository recreated(directory.path(), true, 30000);
    require(recreated.upsert(recordWithId(id, patternedImage({12, 8}, 3))).success &&
                *recreated.previewSourceRevision(id) > previewRevision,
            "recreated ids must not reuse a stale disk thumbnail key");
}

void deferredPolicyEnforcementRunsOnlyWhenRequested() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    auto first =
        recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces), patternedImage({2, 2}, 1));
    auto second =
        recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces), patternedImage({2, 2}, 2));
    require(repository.upsert(first).success && repository.upsert(second).success &&
                repository.markClosedDeferred(first.id).success &&
                repository.markClosedDeferred(second.id).success,
            "seed closed pins without a synchronous sweep");
    auto policy = repository.policy();
    policy.maxEntries = 1;
    require(repository.setPolicy(policy, false).success && repository.summaries().size() == 2,
            "deferred policy update leaves cleanup to the maintenance worker");
    require(repository.enforcePolicy().success && repository.summaries().size() == 1,
            "maintenance applies the deferred policy");
}

void managementPolicySizeCacheTracksPayloadCommits() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    auto record =
        recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces), patternedImage({2, 2}, 1));
    auto policy = repository.policy();
    policy.maxDiskMiB = 128;
    require(repository.setPolicy(policy).success && repository.upsert(record).success &&
                repository.markClosed(record.id).success && repository.flush().success &&
                repository.enforcePolicy().success,
            "warm closed-record disk usage");

    auto changed = repository.loadRecord(record.id);
    require(changed.has_value(), "load closed record before payload update");
    changed->canvasSession = QByteArrayLiteral("changed canvas state");
    require(repository.updateState(*changed).success && repository.flush().success,
            "commit changed payload after disk usage was cached");
    QFile payload(payloadFilePath(directory.path(), record.id));
    require(payload.open(QIODevice::ReadWrite) && payload.resize(129LL * 1024 * 1024),
            "extend committed payload for deterministic quota accounting");
    require(repository.enforcePolicy().success && !repository.loadRecord(record.id),
            "payload commit invalidates cached disk usage before quota enforcement");
}

void managementCreationOrderSurvivesOutOfOrderEncoding() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const auto now = QDateTime::currentDateTimeUtc();
    auto first =
        recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces), patternedImage({2, 2}, 1));
    auto second = first;
    second.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    repository.reserveCreation(first.id, now);
    repository.reserveCreation(second.id, now);
    require(repository.upsert(second).success && repository.upsert(first).success,
            "image encoding may finish in reverse creation order");
    require(repository.loadRecord(first.id)->activitySequence <
                    repository.loadRecord(second.id)->activitySequence &&
                repository.loadRecord(first.id)->createdUtc == now,
            "creation order and date belong to window creation, not encoding completion");
}

void managementExpiresBeforeApplyingQuotas() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const auto now = QDateTime::currentDateTimeUtc();
    auto policy = repository.policy();
    policy.keepPermanently = true;
    policy.maxEntries = 1;
    require(repository.setPolicy(policy).success, "suspend cleanup for clock-change fixture");
    const auto retained =
        recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces), patternedImage({2, 2}, 1));
    auto expired = retained;
    expired.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    require(repository.upsert(retained).success && repository.upsert(expired).success &&
                repository.markClosed(retained.id, now).success &&
                repository.markClosed(expired.id, now.addDays(-8)).success,
            "close records across a backward wall-clock adjustment");
    policy.keepPermanently = false;
    require(repository.setPolicy(policy).success && repository.summaries().size() == 1 &&
                repository.loadRecord(retained.id).has_value(),
            "expired records must be removed before count quota consumes an unexpired record");
}

void managementLegacyMetadataDefaults() {
    QTemporaryDir directory;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        storage::PinnedWindowRepository repository(directory.path());
        require(repository.upsert(recordWithId(id, patternedImage({2, 2}, 1))).success &&
                    repository.flush().success,
                "write legacy fixture");
    }
    const QString path =
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
    auto root = QJsonDocument::fromJson(readBytes(path)).object();
    auto entries = root.value(QStringLiteral("records")).toArray();
    auto item = entries[0].toObject();
    for (const auto* key :
         {"creation_source", "created_utc", "last_closed_utc", "ignored", "activity_sequence"})
        item.remove(QString::fromLatin1(key));
    entries[0] = item;
    root.insert(QStringLiteral("records"), entries);
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "edit legacy fixture");
    file.write(QJsonDocument(root).toJson());
    file.close();
    storage::PinnedWindowRepository repository(directory.path());
    const auto loaded = repository.loadRecord(id);
    require(loaded && !loaded->ignored &&
                loaded->creationSource == storage::PinnedWindowCreationSource::Other &&
                loaded->createdUtc == loaded->updatedUtc,
            "legacy records remain retained with recoverable metadata");
}

void managementHasNoTotalRecordCap() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    for (int i = 0; i < 140; ++i) {
        auto record = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                   patternedImage({2, 2}, i));
        require(repository.upsert(record).success,
                "active pins must not be capped by closed-history limits");
    }
    require(repository.flush().success, "flush more than 128 pins");
    storage::PinnedWindowRepository reloaded(directory.path());
    require(reloaded.summaries().size() == 140, "loading must not truncate at 128 pins");
}

void previewsReadOnlySourcePayloadAndKeepStableRevision() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    auto record = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                               patternedImage({64, 32}, 7));
    record.canvasSession = QByteArrayLiteral("first drawing");
    record.recognitionResults = QByteArrayLiteral("first recognition");
    require(repository.upsert(record).success && repository.flush().success,
            "save preview source with unrelated payloads");

    const auto revision = repository.previewSourceRevision(record.id);
    const auto preview = repository.loadPreviewSource(record.id);
    require(revision && preview && samePixels(preview->image, record.image) &&
                preview->originalHtml.isEmpty() && preview->originalText.isEmpty(),
            "preview reads the saved source image");

    const QString extraPath =
        QDir(directory.path())
            .filePath(
                QStringLiteral("pinned_windows_v2/pins/%1/canvas_session.bin").arg(record.id));
    require(QFile::remove(extraPath), "remove unrelated drawing payload");
    const auto recovered = repository.loadRecord(record.id);
    require(repository.loadPreviewSource(record.id).has_value() && recovered &&
                samePixels(recovered->image, record.image) && recovered->canvasSession.isEmpty(),
            "a missing drawing payload must not hide the restorable source image");

    record.canvasSession = QByteArrayLiteral("second drawing");
    require(repository.updateState(record).success &&
                repository.previewSourceRevision(record.id) == revision,
            "drawing updates do not invalidate the source preview");
    require(repository.upsert(record).success &&
                repository.previewSourceRevision(record.id) == revision,
            "a subsequent state save keeps the immutable source revision");
    record.image = patternedImage({64, 32}, 19);
    require(repository.upsert(record).success &&
                repository.previewSourceRevision(record.id) != revision,
            "replacing image content invalidates the source preview");
}

void bulkRemovalIsAtomicAndNotifiesOnce() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    QVector<QString> ids;
    for (int index = 0; index < 20; ++index) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ids.push_back(id);
        require(repository.upsert(recordWithId(id, patternedImage({2, 2}, index))).success,
                "create bulk-removal fixture");
    }
    require(repository.flush().success, "commit bulk-removal fixture");
    std::atomic_int notifications{0};
    repository.setChangedCallback([&notifications]() { ++notifications; });
    require(!repository.removeMany({ids.front(), QStringLiteral("../invalid")}).success &&
                repository.summaries().size() == ids.size() && notifications == 0,
            "invalid bulk removal leaves every record untouched");
    require(repository.removeMany(ids).success && repository.summaries().isEmpty() &&
                notifications == 1,
            "bulk removal updates all records with one change notification");
    repository.setChangedCallback({});
}

void canceledCreationReleasesLifecycleState() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    const QDateTime firstTime = QDateTime::currentDateTimeUtc().addSecs(-60);
    const QDateTime secondTime = firstTime.addSecs(30);
    auto record = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                               patternedImage({8, 8}, 17));
    const auto prepared =
        storage::PreparedPngImage::fromBytes(record.image.size(), pngBytes(record.image, 6));
    require(prepared.has_value(), "prepare reserved pin source");

    repository.reserveCreation(record.id, firstTime);
    require(repository.markClosed(record.id, firstTime).success,
            "close can precede first publication");
    repository.cancelCreation(record.id);
    require(!repository.createReserved(record, *prepared).success,
            "canceled asynchronous publication must not create a record");

    repository.reserveCreation(record.id, secondTime);
    require(repository.createReserved(record, *prepared).success,
            "a new explicit reservation can create a record");
    const auto created = repository.loadRecord(record.id);
    require(created && !created->ignored && created->createdUtc == secondTime,
            "cancellation must release both creation and close state");
    require(repository.remove(record.id).success && !repository.upsertExisting(record).success,
            "a late update must not recreate a removed record");

    repository.reserveCreation(record.id, secondTime);
    require(repository.createReserved(record, *prepared).success,
            "removed IDs must not remain in a permanent tombstone set");
    require(repository.remove(record.id).success, "remove explicitly recreated record");

    const QString removedBeforeCreate = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.id = removedBeforeCreate;
    repository.reserveCreation(record.id);
    require(repository.remove(record.id).success &&
                !repository.createReserved(record, *prepared).success,
            "deletion must revoke an outstanding first save");

    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    repository.reserveCreation(record.id);
    require(repository.markClosed(record.id).success && repository.clearClosed().success &&
                !repository.createReserved(record, *prepared).success,
            "clearing closed pins must revoke an outstanding first save");
}

class OperationBarrier final {
  public:
    explicit OperationBarrier(storage::PinnedWindowOperation operation)
        : m_operation(operation), m_entered(m_entry.get_future()),
          m_released(m_release.get_future().share()) {}

    void arm() {
        m_armed.store(true);
    }

    void observe(storage::PinnedWindowOperation operation) {
        if (operation == m_operation && m_armed.exchange(false)) {
            m_entry.set_value();
            m_released.wait();
        }
    }

    void waitUntilEntered() {
        require(m_entered.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                "repository operation did not reach its barrier");
    }

    void release() {
        m_release.set_value();
    }

  private:
    storage::PinnedWindowOperation m_operation;
    std::atomic_bool m_armed = false;
    std::promise<void> m_entry;
    std::future<void> m_entered;
    std::promise<void> m_release;
    std::shared_future<void> m_released;
};

void payloadReadsKeepTheirRevisionWithoutBlockingMetadata() {
    for (const bool preview : {false, true}) {
        QTemporaryDir directory;
        OperationBarrier barrier(storage::PinnedWindowOperation::PayloadRead);
        storage::PinnedWindowRepository repository(
            directory.path(), true, 30000,
            [&barrier](storage::PinnedWindowOperation operation) { barrier.observe(operation); });
        auto record = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                   patternedImage({19, 13}, 29));
        record.canvasSession = QByteArrayLiteral("original canvas revision");
        const auto original = record;
        require(repository.upsert(record).success && repository.flush().success,
                "commit the original payload read revision");
        barrier.arm();
        auto reading = std::async(std::launch::async, [&repository, &record, preview]() {
            if (preview) {
                const auto source = repository.loadPreviewSource(record.id);
                return source ? source->image : QImage();
            }
            const auto loaded = repository.loadRecord(record.id);
            require(loaded &&
                        loaded->canvasSession == QByteArrayLiteral("original canvas revision"),
                    "admitted full reads must keep their original canvas revision");
            return loaded->image;
        });
        barrier.waitUntilEntered();
        record.image = patternedImage({19, 13}, 31);
        record.canvasSession = QByteArrayLiteral("replacement canvas revision");
        auto mutation = std::async(std::launch::async, [&repository, record]() {
            return repository.upsertExisting(record).success &&
                   repository.markClosedDeferred(record.id).success &&
                   repository.beginRestore(record.id).success &&
                   repository.markRestored(record.id).success;
        });
        const bool responsive =
            mutation.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        auto writing =
            std::async(std::launch::async, [&repository]() { return repository.flush(); });
        barrier.release();
        const auto image = reading.get();
        require(responsive && mutation.get(),
                "payload reads must not block source updates, close, or restore metadata");
        const auto writeResult = writing.get();
        require(writeResult.success, "commit the replacement after the admitted read finishes");
        require(samePixels(image, original.image),
                "an admitted read must finish before its files are overwritten");
        const auto replaced = repository.loadRecord(record.id);
        require(replaced && samePixels(replaced->image, record.image) &&
                    replaced->canvasSession == record.canvasSession && !replaced->ignored,
                "later reads must see the complete replacement revision and restored lifecycle");
    }
}

void recordDeletionDoesNotInvalidateAdmittedPayloadRead() {
    QTemporaryDir directory;
    OperationBarrier barrier(storage::PinnedWindowOperation::PayloadRead);
    storage::PinnedWindowRepository repository(
        directory.path(), true, 30000,
        [&barrier](storage::PinnedWindowOperation operation) { barrier.observe(operation); });
    const auto record = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                     patternedImage({17, 11}, 33));
    require(repository.upsert(record).success && repository.flush().success,
            "commit a payload before its concurrent deletion");
    barrier.arm();
    auto reading = std::async(
        std::launch::async, [&repository, &record]() { return repository.loadRecord(record.id); });
    barrier.waitUntilEntered();
    auto removing = std::async(std::launch::async, [&repository, &record]() {
        return repository.remove(record.id).success && !repository.upsertExisting(record).success &&
               repository.summaries().isEmpty();
    });
    const bool responsive = removing.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    auto writing = std::async(std::launch::async, [&repository]() { return repository.flush(); });
    barrier.release();
    const auto loaded = reading.get();
    require(responsive && removing.get(),
            "deletion must complete while an admitted read is paused");
    require(
        writing.get().success && loaded && samePixels(loaded->image, record.image) &&
            !repository.loadRecord(record.id) &&
            !QFileInfo::exists(payloadFilePath(directory.path(), record.id)),
        "deletion must reclaim files after admitted readers finish without resurrecting records");
}

void retentionWorkDoesNotBlockLifecycleUpdates() {
    enum class Entry { Sweep, Close, Policy };
    for (const auto entry : {Entry::Sweep, Entry::Close, Entry::Policy}) {
        for (const bool waitForWriter : {false, true}) {
            QTemporaryDir directory;
            OperationBarrier barrier(waitForWriter ? storage::PinnedWindowOperation::PayloadWrite
                                                   : storage::PinnedWindowOperation::RetentionScan);
            std::promise<void> retentionWait;
            auto waiting = retentionWait.get_future();
            std::atomic_bool waitObserved = false;
            storage::PinnedWindowRepository repository(
                directory.path(), true, 30000,
                [&barrier, &retentionWait,
                 &waitObserved](storage::PinnedWindowOperation operation) {
                    barrier.observe(operation);
                    if (operation == storage::PinnedWindowOperation::RetentionWait &&
                        !waitObserved.exchange(true))
                        retentionWait.set_value();
                });
            auto record = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                       patternedImage({13, 9}, 35));
            require(repository.upsert(record).success && repository.flush().success &&
                        repository.markClosedDeferred(record.id).success,
                    "seed a closed record for concurrent maintenance");
            auto policy = repository.policy();
            policy.maxEntries = 1;
            require(repository.setPolicy(policy, false).success,
                    "defer the single-entry retention policy");
            barrier.arm();
            std::future<storage::StorageResult> writing;
            if (waitForWriter) {
                writing =
                    std::async(std::launch::async, [&repository]() { return repository.flush(); });
                barrier.waitUntilEntered();
            }
            auto maintenance =
                std::async(std::launch::async, [&repository, record, policy, entry]() {
                    switch (entry) {
                    case Entry::Close:
                        return repository.markClosed(record.id);
                    case Entry::Policy:
                        return repository.setPolicy(policy);
                    case Entry::Sweep:
                        return repository.enforcePolicy();
                    }
                    return storage::StorageResult::failure(
                        QStringLiteral("Unknown retention test entry"));
                });
            if (waitForWriter) {
                require(waiting.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                        "retention must reach its active-writer wait");
            } else {
                barrier.waitUntilEntered();
            }
            auto pending = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                        patternedImage({13, 9}, 37));
            const auto prepared = storage::PreparedPngImage::fromBytes(pending.image.size(),
                                                                       pngBytes(pending.image, 6));
            require(prepared.has_value(), "prepare the late pinned source");
            auto mutation =
                std::async(std::launch::async, [&repository, record, pending, prepared]() mutable {
                    if (!repository.beginRestore(record.id).success ||
                        !repository.markRestored(record.id).success)
                        return false;
                    record.canvasSession =
                        QByteArrayLiteral("updated while maintenance was paused");
                    if (!repository.updateState(record).success)
                        return false;
                    repository.reserveCreation(pending.id);
                    if (!repository.markClosedDeferred(pending.id).success ||
                        !repository.createReserved(pending, *prepared).success)
                        return false;
                    pending.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
                    repository.reserveCreation(pending.id);
                    return repository.markClosedDeferred(pending.id).success &&
                           repository.remove(pending.id).success &&
                           !repository.createReserved(pending, *prepared).success;
                });
            const bool responsive =
                mutation.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
            barrier.release();
            require(responsive && mutation.get(),
                    "retention scans and writer waits must not block lifecycle mutations");
            if (writing.valid())
                require(writing.get().success,
                        "commit concurrent metadata after the writer resumes");
            require(maintenance.get().success && repository.flush().success,
                    "complete concurrent maintenance and payload commits");
            const auto restored = repository.loadRecord(record.id);
            const auto lateClosed = repository.loadRecord(pending.id);
            require(
                restored && !restored->ignored && lateClosed && lateClosed->ignored &&
                    repository.summaries().size() == 2,
                "stale sweeps must protect restored records and preserve closes before first save");
        }
    }
}

void disabledRetentionDoesNotWaitForActiveWriter() {
    for (const bool permanent : {false, true}) {
        QTemporaryDir directory;
        OperationBarrier barrier(storage::PinnedWindowOperation::PayloadWrite);
        storage::PinnedWindowRepository repository(
            directory.path(), true, 30000,
            [&barrier](storage::PinnedWindowOperation operation) { barrier.observe(operation); });
        const auto record = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                         patternedImage({11, 7}, 39));
        require(repository.upsert(record).success, "create a record before a paused write");
        auto policy = repository.policy();
        policy.enabled = permanent;
        policy.keepPermanently = permanent;
        require(repository.setPolicy(policy, false).success, "defer bypassed retention settings");
        barrier.arm();
        auto writing =
            std::async(std::launch::async, [&repository]() { return repository.flush(); });
        barrier.waitUntilEntered();
        auto maintenance =
            std::async(std::launch::async, [&repository]() { return repository.enforcePolicy(); });
        const bool responsive =
            maintenance.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        barrier.release();
        require(responsive && maintenance.get().success && writing.get().success,
                "disabled and permanent retention must bypass active-writer waits");
    }
}

void storageExchangeDrainsReadersAndRejectsStaleAdmissions() {
    for (const bool preview : {false, true}) {
        for (const bool leased : {false, true}) {
            QTemporaryDir sourceDirectory;
            QTemporaryDir destinationDirectory;
            OperationBarrier barrier(leased ? storage::PinnedWindowOperation::PayloadRead
                                            : storage::PinnedWindowOperation::PayloadReadAdmission);
            std::promise<void> drainReached;
            auto draining = drainReached.get_future();
            std::atomic_bool drainObserved = false;
            std::atomic_bool oldPayloadRead = false;
            storage::PinnedWindowRepository repository(
                sourceDirectory.path(), true, 30000,
                [&barrier, &drainReached, &drainObserved,
                 &oldPayloadRead](storage::PinnedWindowOperation operation) {
                    if (operation == storage::PinnedWindowOperation::PayloadRead)
                        oldPayloadRead.store(true);
                    barrier.observe(operation);
                    if (operation == storage::PinnedWindowOperation::StorageReadDrain &&
                        !drainObserved.exchange(true))
                        drainReached.set_value();
                });
            auto prepared = std::make_unique<storage::PinnedWindowRepository>(
                destinationDirectory.path(), true, 30000);
            auto record = recordWithId(QUuid::createUuid().toString(QUuid::WithoutBraces),
                                       patternedImage({9, 5}, 41));
            const auto originalImage = record.image;
            require(repository.upsert(record).success && repository.flush().success,
                    "commit the original storage generation");
            record.image = patternedImage({9, 5}, 43);
            require(prepared->upsert(record).success && prepared->flush().success,
                    "commit the prepared storage generation");
            barrier.arm();
            auto reading = std::async(
                std::launch::async, [&repository, &record, preview]() -> std::optional<QImage> {
                    if (preview) {
                        const auto source = repository.loadPreviewSource(record.id);
                        return source ? std::optional<QImage>(source->image) : std::nullopt;
                    }
                    const auto loaded = repository.loadRecord(record.id);
                    return loaded ? std::optional<QImage>(loaded->image) : std::nullopt;
                });
            barrier.waitUntilEntered();
            auto exchanging =
                std::async(std::launch::async, [&repository, &prepared, &sourceDirectory]() {
                    repository.suspendWrites(true);
                    repository.exchangeStorage(*prepared);
                    prepared.reset();
                    // The real migration removes old files after activation.
                    const bool removed = QDir(sourceDirectory.path()).removeRecursively();
                    repository.suspendWrites(false);
                    return removed;
                });
            require(draining.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
                    "migration must fence new readers before draining its payload leases");
            if (leased) {
                require(!repository.loadRecord(record.id) &&
                            !repository.loadPreviewSource(record.id) &&
                            repository.summaries().size() == 1 &&
                            exchanging.wait_for(std::chrono::milliseconds(0)) !=
                                std::future_status::ready,
                        "migration must wait for admitted readers while metadata queries proceed");
                barrier.release();
                const auto original = reading.get();
                require(original && samePixels(*original, originalImage) && exchanging.get(),
                        "admitted reads must finish before migration unlinks their source files");
            } else {
                require(exchanging.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
                            exchanging.get(),
                        "a captured generation without a file lease must not block migration");
                barrier.release();
                require(
                    !reading.get() && !oldPayloadRead.load(),
                    "stale full and preview captures must revalidate before reading old payloads");
            }
            const auto current = repository.loadRecord(record.id);
            require(current && samePixels(current->image, record.image),
                    "new reads must use the activated storage generation after old-file cleanup");
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    payloadReadsKeepTheirRevisionWithoutBlockingMetadata();
    recordDeletionDoesNotInvalidateAdmittedPayloadRead();
    retentionWorkDoesNotBlockLifecycleUpdates();
    disabledRetentionDoesNotWaitForActiveWriter();
    storageExchangeDrainsReadersAndRejectsStaleAdmissions();
    managementLifecycleAndRetention();
    managementHasNoTotalRecordCap();
    previewsReadOnlySourcePayloadAndKeepStableRevision();
    bulkRemovalIsAtomicAndNotifiesOnce();
    canceledCreationReleasesLifecycleState();
    managementDiskQuotaAndRestorationProtection();
    membershipAndPreviewRevisionsTrackOnlyTheirSources();
    deferredPolicyEnforcementRunsOnlyWhenRequested();
    managementPolicySizeCacheTracksPayloadCommits();
    managementLegacyMetadataDefaults();
    managementExpiresBeforeApplyingQuotas();
    managementCreationOrderSurvivesOutOfOrderEncoding();
    precisePlacementAndPreviousVersionIsolation();
    stateUpdatesBeforeFirstFlushPreserveRestorableSources();
    committedPayloadsAreServedFromDisk();
    manifestFailuresReleaseWrittenPayloadsAndRetryMetadata();
    partialPayloadFailuresDemoteOnlyCompleteRevisions();
    failedManifestSourceReplacementsKeepCurrentPayloadDescriptors();
    continuousChangesDoNotPostponePayloadWrites();
    residentPayloadPressureBypassesLongDebounce();
    missingOptionalPayloadDoesNotHideRestorableImage();
    preparedSourceIsWrittenOnceAndStateUpdatesPreserveIt();
    allocationAdmissionPrecedesSourceDecode();
    metadataOnlyUpdatesDoNotRewriteCommittedPayloads();
    changedPayloadsRecommitAndStayLazy();
    removedRecordsPruneTheirPayloads();
    specifiedGroupRemovalIsAtomicAndPersistent();
    recognitionVisibilityRoundTripsAndDefaultsToHidden();
    clickThroughStateRoundTripsAndRecoversLegacyOrConflictingMetadata();
    alwaysOnTopStateRoundTripsAndDefaultsToEnabledForLegacyRecords();
    pinSourceIdentitySurvivesRestart();
    showBorderStateRoundTripsAndDefaultsToEnabledForLegacyRecords();
    malformedCustomBorderRejectsRecord();
    thumbnailStateSurvivesRestartAndExit();
    hideToTopRoundTripsAndRecoversLegacyMetadata();
    return 0;
}
