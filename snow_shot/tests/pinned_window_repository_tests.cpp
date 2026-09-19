#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
                    restored->groupId == group && restored->canvasSession == record.canvasSession &&
                    restored->recognitionResults == record.recognitionResults &&
                    (source == 1 ? restored->originalText == record.originalText &&
                                       restored->originalHtml == record.originalHtml
                                 : samePixels(restored->image, originalImage)),
                "restart must retain group count, source, annotations, and recognition");
    }
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
        require(demoted.has_value() && demoted->clickThroughMode &&
                    demoted->clickThroughOpacityPercent == 37,
                "click-through state must survive payload demotion");

        record.clickThroughMode = false;
        require(repository.updateState(record).success && repository.flush().success,
                "exiting click-through must update persisted metadata");
        require(!repository.loadRecord(id)->clickThroughMode,
                "the repository must expose the persisted click-through exit");

        record.clickThroughMode = true;
        require(repository.updateState(record).success && repository.flush().success,
                "re-entering click-through must update persisted metadata");
    }
    {
        storage::PinnedWindowRepository repository(directory.path());
        const auto loaded = repository.loadRecord(id);
        require(loaded.has_value() && loaded->clickThroughMode &&
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
        require(loaded.has_value() && !loaded->clickThroughMode,
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
        QDir(directory.path()).filePath(QStringLiteral("pinned_windows/index.json"));
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
                loaded->hideToTopPlacement == record.hideToTopPlacement,
            "all placement states must retain display identity and fractional point positions");
    require(readBytes(oldIndex) == oldBytes,
            "version two must not modify or reinterpret previous-version storage");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    precisePlacementAndPreviousVersionIsolation();
    stateUpdatesBeforeFirstFlushPreserveRestorableSources();
    committedPayloadsAreServedFromDisk();
    preparedSourceIsWrittenOnceAndStateUpdatesPreserveIt();
    metadataOnlyUpdatesDoNotRewriteCommittedPayloads();
    changedPayloadsRecommitAndStayLazy();
    removedRecordsPruneTheirPayloads();
    specifiedGroupRemovalIsAtomicAndPersistent();
    recognitionVisibilityRoundTripsAndDefaultsToHidden();
    clickThroughStateRoundTripsAndRecoversLegacyOrConflictingMetadata();
    alwaysOnTopStateRoundTripsAndDefaultsToEnabledForLegacyRecords();
    thumbnailStateSurvivesRestartAndExit();
    hideToTopRoundTripsAndRecoversLegacyMetadata();
    return 0;
}
