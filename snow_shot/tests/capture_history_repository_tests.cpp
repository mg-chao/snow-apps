#include "snow_shot/storage/capturehistoryrepository.h"
#include "snowimageqtcodec.h"

#include <QCoreApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>

namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "failed to open repository fixture");
    require(file.write(bytes) == bytes.size(), "failed to write repository fixture");
}

QJsonObject readObject(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "failed to open stored JSON");
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
            "stored JSON is malformed");
    return document.object();
}

QByteArray pngBytes(const QImage& image, int compression) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG", compression),
            "failed to encode PNG test data");
    return bytes;
}

storage::CaptureHistoryDraft draftAt(const QDateTime& createdUtc,
                                     const QSize& imageSize = {32, 24}) {
    storage::CaptureHistoryDraft draft;
    draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    draft.createdUtc = createdUtc;
    draft.canvasBounds = QRect(QPoint(0, 0), imageSize);
    draft.selection.rectangle = draft.canvasBounds;
    draft.selection.cornerRadius = 4;
    draft.selection.shadowWidth = 2;
    draft.selection.shadowColor = QColor(0, 0, 0, 96);
    draft.canvasHistory = QByteArrayLiteral("{\"schemaVersion\":1,\"document\":{},\"history\":{}}");
    QImage image(imageSize, QImage::Format_RGBA8888);
    image.fill(qRgba(25, 75, 125, 255));
    draft.displays.push_back({QStringLiteral("display-id"), QStringLiteral("Display"), image});
    return draft;
}

QString onlyRecordDirectory(const QString& root) {
    const QDir history(QDir(root).filePath(QStringLiteral("capture_history/records")));
    const QFileInfoList entries =
        history.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    require(entries.size() == 1, "expected exactly one published record directory");
    return entries.constFirst().absoluteFilePath();
}

QString indexPath(const QString& root) {
    return QDir(root).filePath(QStringLiteral("capture_history/index.json"));
}

QJsonObject firstRecord(const QString& root) {
    return readObject(indexPath(root))
        .value(QStringLiteral("records"))
        .toArray()
        .first()
        .toObject();
}

void pointGeometryRoundTripsAndLegacyIndexRemainsReadable() {
    QTemporaryDir temporary;
    const auto now = QDateTime::currentDateTimeUtc();
    auto draft = draftAt(now);
    draft.displays.front().sourceCanvasRect = QRect(-10, 20, 10, 6);
    draft.displays.front().backingScale = 2.0;
    draft.displays.front().nativeDisplayId = 42;
    draft.displays.front().canvasUsesPoints = true;
    storage::CaptureHistoryRecord published;
    {
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        const auto result = repository->publish(draft).get();
        require(result.storage.success, "point geometry publication failed");
        published = result.record;
    }
    {
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        require(repository->records().size() == 1 && repository->records().front() == published &&
                    repository->records().front().displays.front().nativeDisplayId == 42 &&
                    repository->records().front().displays.front().backingScale == 2.0 &&
                    repository->records().front().displays.front().sourceCanvasRect ==
                        QRect(-10, 20, 10, 6) &&
                    repository->records().front().displays.front().canvasUsesPoints,
                "point geometry was not persisted");
    }
    QJsonObject index = readObject(indexPath(temporary.path()));
    index.insert(QStringLiteral("format_version"), 1);
    auto records = index.value(QStringLiteral("records")).toArray();
    auto record = records[0].toObject();
    auto images = record.value(QStringLiteral("displays")).toArray();
    auto image = images[0].toObject();
    image.remove(QStringLiteral("source_canvas_rect"));
    image.remove(QStringLiteral("canvas_space"));
    images[0] = image;
    record.insert(QStringLiteral("displays"), images);
    records[0] = record;
    index.insert(QStringLiteral("records"), records);
    QFile file(indexPath(temporary.path()));
    require(file.open(QIODevice::WriteOnly), "legacy index write failed");
    file.write(QJsonDocument(index).toJson());
    file.close();
    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    require(repository->records().size() == 1 &&
                !repository->records().front().displays.front().canvasUsesPoints,
            "legacy pixel-space history was rejected");
}

void sourceCanvasOriginsRoundTripAndRejectInvalidCoordinates() {
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto origin :
         {std::optional<QPoint>{}, std::optional<QPoint>{QPoint(-1920, -200)},
          std::optional<QPoint>{QPoint(0, 0)}, std::optional<QPoint>{QPoint(1920, 400)}}) {
        QTemporaryDir temporary;
        storage::CaptureHistoryRecord published;
        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            auto draft = draftAt(now);
            draft.displays.front().sourceCanvasOrigin = origin;
            const auto result = repository->publish(draft).get();
            require(result.storage.success, "positioned display publication failed");
            published = result.record;
        }
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        require(repository->records().size() == 1 && repository->records().front() == published &&
                    published.displays.front().sourceCanvasOrigin == origin &&
                    repository->load(published).has_value(),
                "display source origin did not survive a repository restart");
    }

    QTemporaryDir temporary;
    {
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        auto draft = draftAt(now);
        require(repository->publish(draft).get().storage.success,
                "source origin validation fixture failed");
        draft.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        draft.displays.front().sourceCanvasOrigin = QPoint(std::numeric_limits<int>::max(), 0);
        require(!repository->publish(draft).get().storage.success,
                "publication accepted an overflowing source rectangle");
    }
    const QJsonObject index = readObject(indexPath(temporary.path()));
    const QJsonValue invalidOrigins[] = {
        QJsonValue(QJsonValue::Null), QJsonObject{},
        QJsonObject{{QStringLiteral("x"), 1.5}, {QStringLiteral("y"), 0}},
        QJsonObject{{QStringLiteral("x"), std::numeric_limits<int>::max()},
                    {QStringLiteral("y"), 0}},
        QJsonObject{{QStringLiteral("x"), 0},
                    {QStringLiteral("y"), std::numeric_limits<int>::max()}}};
    for (const auto& origin : invalidOrigins) {
        QJsonObject record = index.value(QStringLiteral("records")).toArray().first().toObject();
        QJsonArray displays = record.value(QStringLiteral("displays")).toArray();
        QJsonObject display = displays.first().toObject();
        display.insert(QStringLiteral("source_canvas_origin"), origin);
        displays[0] = display;
        record.insert(QStringLiteral("displays"), displays);
        QJsonObject invalidIndex = index;
        invalidIndex.insert(QStringLiteral("records"), QJsonArray{record});
        writeBytes(indexPath(temporary.path()), QJsonDocument(invalidIndex).toJson());
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        require(repository->records().isEmpty(), "invalid source origin was accepted on load");
    }
}

void scrollingMarkerRoundTripsAndRejectsNonBooleanValues() {
    const auto now = QDateTime::currentDateTimeUtc();
    for (const std::optional<bool> scrolling :
         {std::optional<bool>{true}, std::optional<bool>{false}}) {
        QTemporaryDir temporary;
        storage::CaptureHistoryRecord published;
        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            auto draft = draftAt(now);
            draft.scrolling = scrolling;
            const auto result = repository->publish(draft).get();
            require(result.storage.success, "scrolling marker publication failed");
            published = result.record;
        }
        const QJsonObject record = firstRecord(temporary.path());
        require(record.value(QStringLiteral("scrolling")).isBool() &&
                    record.value(QStringLiteral("scrolling")).toBool() == *scrolling,
                "the scrolling marker was not persisted as a boolean");
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        require(repository->records().size() == 1 && repository->records().front() == published &&
                    repository->records().front().scrolling == scrolling,
                "the scrolling marker did not survive a repository restart");
    }

    // Records persisted before the marker existed load without one.
    {
        QTemporaryDir temporary;
        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            auto draft = draftAt(now);
            draft.scrolling = false;
            require(repository->publish(draft).get().storage.success,
                    "legacy marker fixture publication failed");
        }
        QJsonObject index = readObject(indexPath(temporary.path()));
        QJsonArray records = index.value(QStringLiteral("records")).toArray();
        QJsonObject record = records[0].toObject();
        record.remove(QStringLiteral("scrolling"));
        records[0] = record;
        index.insert(QStringLiteral("records"), records);
        writeBytes(indexPath(temporary.path()), QJsonDocument(index).toJson());
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        require(repository->records().size() == 1 &&
                    !repository->records().front().scrolling.has_value(),
                "a missing scrolling marker was not treated as unrecorded");
    }

    // A marker that is not a boolean corrupts the record and therefore the index.
    {
        QTemporaryDir temporary;
        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            require(repository->publish(draftAt(now)).get().storage.success,
                    "invalid marker fixture publication failed");
        }
        const QJsonObject index = readObject(indexPath(temporary.path()));
        for (const QJsonValue& invalid :
             {QJsonValue(QJsonValue::Null), QJsonValue(1), QJsonValue(QStringLiteral("yes"))}) {
            QJsonObject record =
                index.value(QStringLiteral("records")).toArray().first().toObject();
            record.insert(QStringLiteral("scrolling"), invalid);
            QJsonObject invalidIndex = index;
            invalidIndex.insert(QStringLiteral("records"), QJsonArray{record});
            writeBytes(indexPath(temporary.path()), QJsonDocument(invalidIndex).toJson());
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            require(repository->records().isEmpty(), "a non-boolean scrolling marker was accepted");
        }
    }
}

void desktopGeometryRoundTripsAndValidatesCoordinates() {
    for (const bool points : {false, true}) {
        QTemporaryDir temporary;
        storage::CaptureHistoryRecord published;
        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            auto draft = draftAt(QDateTime::currentDateTimeUtc());
            draft.desktopGeometry =
                storage::CaptureHistoryDesktopGeometry{QPoint(-1920, -1080), points};
            const auto result = repository->publish(draft).get();
            require(result.storage.success, "desktop geometry publication failed");
            published = result.record;
            require(published.desktopGeometry == draft.desktopGeometry,
                    "publication lost the captured desktop geometry");
        }
        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            require(repository->records().size() == 1 && repository->records().front() == published,
                    "desktop geometry did not survive a repository restart");
        }
        const QJsonObject index = readObject(indexPath(temporary.path()));
        auto record = index.value(QStringLiteral("records")).toArray().first().toObject();
        const auto validGeometry = record.value(QStringLiteral("desktop_geometry")).toObject();
        const auto writeRecord = [&] {
            auto changed = index;
            changed.insert(QStringLiteral("records"), QJsonArray{record});
            writeBytes(indexPath(temporary.path()), QJsonDocument(changed).toJson());
        };
        record.remove(QStringLiteral("desktop_geometry"));
        writeRecord();
        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            require(repository->records().size() == 1 &&
                        !repository->records().front().desktopGeometry,
                    "legacy records must load without inventing a desktop origin");
        }
        for (const QJsonValue invalid :
             {QJsonValue(QJsonValue::Null), QJsonValue(0), QJsonValue(QStringLiteral("invalid"))}) {
            record.insert(QStringLiteral("desktop_geometry"), invalid);
            writeRecord();
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            require(repository->records().isEmpty(), "invalid desktop geometry was accepted");
        }
        for (const QString key :
             {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("space")}) {
            for (const QJsonValue invalid :
                 {QJsonValue(QJsonValue::Null), QJsonValue(0.5), QJsonValue(2147483648.0),
                  QJsonValue(QStringLiteral("unknown"))}) {
                auto geometry = validGeometry;
                geometry.insert(key, invalid);
                record.insert(QStringLiteral("desktop_geometry"), geometry);
                writeRecord();
                auto repository = storage::makeCaptureHistoryRepository(temporary.path());
                require(repository->records().isEmpty(),
                        "invalid desktop coordinate or space was accepted");
            }
        }
    }
}

void publicationAndRecovery() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create publication directory");
    const QDateTime now =
        QDateTime::fromString(QStringLiteral("2026-08-05T12:30:00.000Z"), Qt::ISODateWithMs);
    storage::CaptureHistoryRepositoryOptions options;
    options.clock = [now]() { return now; };
    storage::CaptureHistoryRecord published;
    {
        auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
        storage::CaptureHistoryDraft draft = draftAt(now);
        QImage resultImage(QSize(20, 12), QImage::Format_RGBA8888);
        resultImage.fill(qRgba(210, 80, 40, 220));
        draft.resultImage = resultImage;
        draft.source = storage::CaptureHistorySource::PinnedToScreen;
        const storage::CaptureHistoryPublishResult result = repository->publish(draft).get();
        require(result.storage.success, "self-contained publication failed");
        published = result.record;
        const QString directory = onlyRecordDirectory(temporary.path());
        const QJsonObject manifest = firstRecord(temporary.path());
        require(readObject(indexPath(temporary.path()))
                            .value(QStringLiteral("format_version"))
                            .toInt() == 2 &&
                    manifest.value(QStringLiteral("id")).toString() == published.id &&
                    manifest.value(QStringLiteral("source")).toString() ==
                        QStringLiteral("pinned_to_screen") &&
                    published.source == storage::CaptureHistorySource::PinnedToScreen &&
                    QFileInfo(QDir(directory).filePath(QStringLiteral("canvas_history.json")))
                        .isFile() &&
                    QFileInfo(QDir(directory).filePath(QStringLiteral("display_0.png"))).isFile() &&
                    QFileInfo(QDir(directory).filePath(QStringLiteral("capture_result.png")))
                        .isFile() &&
                    published.result.has_value() && published.result->imageSize == QSize(20, 12),
                "published record is not self-describing");
        qint64 physicalBytes =
            QFileInfo(QDir(directory).filePath(QStringLiteral("canvas_history.json"))).size() +
            QFileInfo(QDir(directory).filePath(QStringLiteral("display_0.png"))).size();
        const qint64 resultBytes =
            QFileInfo(QDir(directory).filePath(QStringLiteral("capture_result.png"))).size();
        physicalBytes += resultBytes;
        require(published.totalBytes == physicalBytes &&
                    manifest.value(QStringLiteral("total_record_size")).toInteger() ==
                        physicalBytes,
                "manifest total byte accounting is inconsistent");
        const storage::CaptureHistoryUsage usage = repository->usage();
        require(usage.entryCount == 1 && usage.recordBytes == physicalBytes &&
                    usage.indexBytes == QFileInfo(indexPath(temporary.path())).size() &&
                    usage.totalBytes == physicalBytes + usage.indexBytes,
                "capture-history usage includes data outside self-contained records");
        const auto payload = repository->load(published);
        require(payload.has_value() && payload->displayImages.size() == 1 &&
                    payload->displayImages.constFirst().size() == QSize(32, 24),
                "published record could not be loaded");
#if defined(Q_OS_WIN) || defined(_WIN32)
        require(payload->displayImages.constFirst().format() == QImage::Format_ARGB32,
                "Windows history display image should use BGRA-backed ARGB32 pixels");
#endif
        const auto loadedResult = repository->loadResultImage(published);
        require(loadedResult.has_value() && loadedResult->size() == QSize(20, 12) &&
                    loadedResult->pixelColor(3, 4) == resultImage.pixelColor(3, 4),
                "published result image could not be loaded");
#if defined(Q_OS_WIN) || defined(_WIN32)
        require(loadedResult->format() == QImage::Format_ARGB32,
                "Windows history result image should use BGRA-backed ARGB32 pixels");
#endif
    }

    {
        auto recovered = storage::makeCaptureHistoryRepository(temporary.path(), options);
        require(recovered->records().size() == 1 &&
                    recovered->records().constFirst().id == published.id &&
                    recovered->records().constFirst().source ==
                        storage::CaptureHistorySource::PinnedToScreen,
                "record was not recovered from its self-contained directory");
    }
}

void preparedResultBytesAreCommittedWithoutReplacement() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create prepared-result directory");
    const QDateTime now = QDateTime::currentDateTimeUtc();
    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    storage::CaptureHistoryDraft draft = draftAt(now);
    QImage result(QSize(23, 11), QImage::Format_RGBA8888);
    result.fill(QColor(17, 91, 203, 197));
    const auto sharedBytes = std::make_shared<const QByteArray>(pngBytes(result, 7));
    const auto prepared = storage::PreparedPngImage::fromBytes(result.size(), sharedBytes);
    require(prepared.has_value() && prepared->sharedBytes().get() == sharedBytes.get(),
            "prepared PNG validation did not preserve immutable buffer identity");
    require(!storage::PreparedPngImage::fromBytes(QSize(22, 11), sharedBytes).has_value(),
            "prepared PNG validation accepted mismatched dimensions");
    draft.preparedResultImage = prepared;

    const storage::CaptureHistoryPublishResult published = repository->publish(draft).get();
    require(published.storage.success && published.record.result.has_value() &&
                published.record.result->imageSize == result.size(),
            "prepared history result was not published");
    QFile stored(
        QDir(onlyRecordDirectory(temporary.path())).filePath(QStringLiteral("capture_result.png")));
    require(stored.open(QIODevice::ReadOnly) && stored.readAll() == *sharedBytes,
            "history replaced the prepared PNG bytes during publication");
    const auto loaded = repository->loadResultImage(published.record);
    require(loaded.has_value() && loaded->pixelColor(4, 6) == result.pixelColor(4, 6),
            "prepared history result did not round-trip");
    const auto loadedPng = repository->loadResultPng(published.record);
    require(loadedPng.has_value() && loadedPng->bytes() == *sharedBytes,
            "history clipboard read must preserve the stored PNG bytes");
}

void displayCompressionIsIndependentOfResultCompression() {
    QImage image(QSize(96, 64), QImage::Format_RGBA8888);
    for (int row = 0; row < image.height(); ++row) {
        for (int column = 0; column < image.width(); ++column) {
            image.setPixel(column, row,
                           qRgba((column / 4 + row * 5) % 256, (column + row / 3) % 256,
                                 (column * 3 + row) % 256, 255));
        }
    }
    QVector<QByteArray> displayEncodings;
    for (const int compression : {0, 6, 9}) {
        QTemporaryDir temporary;
        require(temporary.isValid(), "failed to create compression fixture directory");
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        auto draft = draftAt(QDateTime::currentDateTimeUtc(), image.size());
        draft.displays.front().image = image;
        draft.resultImage = image;
        draft.pngCompressionLevel = 0;
        draft.displayPngCompressionLevel = compression;
        const auto published = repository->publish(draft).get();
        require(published.storage.success, "compressed history publication failed");
        const QDir directory(onlyRecordDirectory(temporary.path()));
        QFile displayFile(directory.filePath(QStringLiteral("display_0.png")));
        QFile resultFile(directory.filePath(QStringLiteral("capture_result.png")));
        require(displayFile.open(QIODevice::ReadOnly) && resultFile.open(QIODevice::ReadOnly),
                "compressed history files are missing");
        const QByteArray displayBytes = displayFile.readAll();
        require(displayBytes == snow_shot::image_codec::encodePng(image, compression) &&
                    resultFile.readAll() == snow_shot::image_codec::encodePng(image, 0),
                "display compression must not change the result PNG encoding");
        const auto payload = repository->load(published.record);
        require(payload.has_value() && payload->displayImages.size() == 1 &&
                    payload->displayImages.front().pixelColor(17, 12) == image.pixelColor(17, 12),
                "display compression changed decoded pixels");
        displayEncodings.push_back(displayBytes);
    }
    require(displayEncodings[0] != displayEncodings[1] &&
                displayEncodings[1] != displayEncodings[2],
            "low, medium, and high must produce distinct display encodings");
}

void quickCaptureSourcesRoundTrip() {
    const auto verifySource = [](storage::CaptureHistorySource source,
                                 const QString& manifestSource) {
        QTemporaryDir temporary;
        require(temporary.isValid(), "failed to create quick-capture source directory");
        const QDateTime createdUtc =
            QDateTime::fromString(QStringLiteral("2026-08-12T04:30:00.000Z"), Qt::ISODateWithMs);
        storage::CaptureHistoryRepositoryOptions options;
        options.clock = [createdUtc]() { return createdUtc; };
        QString recordId;

        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
            storage::CaptureHistoryDraft draft = draftAt(createdUtc);
            draft.source = source;
            const storage::CaptureHistoryPublishResult result = repository->publish(draft).get();
            require(result.storage.success, "quick-capture source publication failed");
            recordId = result.record.id;

            const QJsonObject manifest = firstRecord(temporary.path());
            require(manifest.value(QStringLiteral("source")).toString() == manifestSource &&
                        result.record.source == source,
                    "quick-capture source was not encoded in the manifest");
        }

        auto recovered = storage::makeCaptureHistoryRepository(temporary.path(), options);
        require(recovered->records().size() == 1 &&
                    recovered->records().constFirst().id == recordId &&
                    recovered->records().constFirst().source == source,
                "quick-capture source was not recovered from its manifest");
    };

    verifySource(storage::CaptureHistorySource::SavedToFile, QStringLiteral("saved_to_file"));
    verifySource(storage::CaptureHistorySource::CurrentMonitor, QStringLiteral("current_monitor"));
    verifySource(storage::CaptureHistorySource::FocusedWindow, QStringLiteral("focused_window"));
}

void trustedStartupAndExplicitClear() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create history leftover directory");
    QString recordDirectory;
    {
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        require(repository->publish(draftAt(QDateTime::currentDateTimeUtc())).get().storage.success,
                "failed to publish leftover fixture");
        recordDirectory = onlyRecordDirectory(temporary.path());
    }
    writeBytes(QDir(recordDirectory).filePath(QStringLiteral("canvas_history.json")),
               QByteArrayLiteral("not-json"));
    const QString temporaryRecord =
        QDir(temporary.path()).filePath(QStringLiteral("capture_history/records/.tmp-abandoned"));
    require(QDir().mkpath(temporaryRecord), "failed to create temporary fixture");
    writeBytes(QDir(temporaryRecord).filePath(QStringLiteral("partial")), QByteArrayLiteral("x"));

    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    require(repository->records().size() == 1 && QFileInfo::exists(temporaryRecord),
            "startup inspected payloads or unmanaged leftovers");
    const auto clearResult = repository->requestClear().get();
    require(clearResult.success && repository->usage().entryCount == 0 &&
                repository->usage().totalBytes == repository->usage().indexBytes &&
                !QFileInfo::exists(temporaryRecord),
            "clear did not remove unmanaged leftovers inside the history tree");
}

void policyBoundariesAndDisabledPreservation() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create policy directory");
    const QDateTime now =
        QDateTime::fromString(QStringLiteral("2026-08-05T12:00:00.000Z"), Qt::ISODateWithMs);
    storage::CaptureHistoryRepositoryOptions options;
    options.clock = [now]() { return now; };
    options.policy.retentionDays = 365;
    auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    require(repository->publish(draftAt(now.addDays(-7))).get().storage.success &&
                repository->publish(draftAt(now.addDays(-7).addMSecs(-1))).get().storage.success,
            "failed to publish age-boundary fixtures");
    storage::CaptureHistoryPolicy policy = options.policy;
    policy.retentionDays = 7;
    require(repository->updatePolicy(policy).get().success && repository->records().size() == 1 &&
                repository->records().constFirst().createdUtc == now.addDays(-7),
            "retention did not apply the exact age cutoff");

    require(repository->publish(draftAt(now.addSecs(-2))).get().storage.success &&
                repository->publish(draftAt(now.addSecs(-1))).get().storage.success,
            "failed to publish count-policy fixtures");
    policy.enabled = false;
    policy.maxEntries = 1;
    require(repository->updatePolicy(policy).get().success && repository->records().size() == 3,
            "disabling history pruned existing records");
    require(!repository->publish(draftAt(now)).get().storage.success &&
                repository->records().size() == 3,
            "disabled history accepted a persistent draft");
    policy.enabled = true;
    require(repository->updatePolicy(policy).get().success && repository->records().size() == 3,
            "re-enabling history enforced capacity before an addition");
    const auto added = repository->publish(draftAt(now)).get();
    require(added.storage.success && repository->records().size() == 1 &&
                repository->records().first().id == added.record.id,
            "addition did not enforce capacity oldest-first");
}

void publicationQueueCapacity() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create worker directory");
    storage::CaptureHistoryRepositoryOptions options;
    options.maxQueuedPublications = 2;
    std::promise<void> started;
    std::promise<void> release;
    auto released = release.get_future().share();
    options.operationObserved = [&](storage::CaptureHistoryOperation operation) {
        if (operation == storage::CaptureHistoryOperation::WorkerStarted) {
            started.set_value();
            released.wait();
        }
    };
    auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    auto first = repository->publish(draftAt(now));
    started.get_future().wait();
    auto second = repository->publish(draftAt(now.addMSecs(1)));
    auto third = repository->publish(draftAt(now.addMSecs(2)));
    auto overloaded = repository->publish(draftAt(now.addMSecs(3), QSize(32, 32)));
    require(overloaded.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
                !overloaded.get().storage.success,
            "publication overload was not rejected immediately");
    release.set_value();
    require(first.get().storage.success && second.get().storage.success &&
                third.get().storage.success,
            "worker did not allow one active and two queued publications");
    repository->drain();
    require(repository->records().size() == 3,
            "serialized worker did not retain all accepted publications");
}
void displayAssetsAreMetadataOnly() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create display-asset directory");
    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    const auto publication =
        repository->publish(draftAt(QDateTime::currentDateTimeUtc(), QSize(48, 32))).get();
    require(publication.storage.success, "failed to publish display-asset fixture");

    const storage::CaptureHistoryRecord record = publication.record;
    const auto assets = repository->displayAssets(record);
    require(assets.has_value() && assets->recordId == record.id && assets->displays.size() == 1 &&
                assets->displays.constFirst().recordId == record.id &&
                assets->displays.constFirst().stableId == record.displays.constFirst().stableId &&
                assets->displays.constFirst().imageSize == QSize(48, 32) &&
                assets->displays.constFirst().localFileUrl.isLocalFile(),
            "repository did not expose the validated display descriptor");

    storage::CaptureHistoryRecord stale = record;
    stale.displays[0].name.append(QStringLiteral(" stale"));
    require(!repository->displayAssets(stale).has_value(),
            "asset lookup accepted stale record metadata");
    stale = record;
    stale.displays.clear();
    require(!repository->displayAssets(stale).has_value(),
            "asset lookup accepted a display-index mismatch");
    stale = record;
    stale.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    require(!repository->displayAssets(stale).has_value(),
            "asset lookup accepted a stale record id");

    const QString imagePath = assets->displays.constFirst().localFileUrl.toLocalFile();
    QFile imageFile(imagePath);
    require(imageFile.open(QIODevice::ReadOnly), "failed to read display-asset fixture");
    const QByteArray encodedImage = imageFile.readAll();
    const qsizetype encodedSize = encodedImage.size();
    imageFile.close();

    const QString linkTarget = temporary.filePath(QStringLiteral("linked-display.png"));
    writeBytes(linkTarget, encodedImage);
    require(QFile::remove(imagePath), "failed to prepare display symlink fixture");
    std::error_code linkError;
    std::filesystem::create_symlink(std::filesystem::path(linkTarget.toStdWString()),
                                    std::filesystem::path(imagePath.toStdWString()), linkError);
    if (!linkError) {
        require(QFileInfo(imagePath).isSymLink(), "display link fixture is not a symlink");
        require(repository->displayAssets(record).has_value(),
                "asset lookup inspected a symlinked display file");
        require(QFile::remove(imagePath), "failed to remove display symlink fixture");
    }
    writeBytes(imagePath, encodedImage);

    writeBytes(imagePath, QByteArray(encodedSize, '\0'));
    require(repository->displayAssets(record).has_value(),
            "asset lookup inspected a deliberately corrupted image payload");
    require(!repository->load(record).has_value(),
            "payload load unexpectedly accepted the deliberately corrupted image");
    repository->drain();
    require(repository->records().isEmpty(),
            "invalid payload was not removed after lazy validation failure");

    require(!QFileInfo::exists(imagePath), "invalid display payload was left in place");
    require(!repository->displayAssets(record).has_value(),
            "asset lookup accepted a missing display file");
}

void traversalManifestIsRejected() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create traversal directory");
    {
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        require(repository->publish(draftAt(QDateTime::currentDateTimeUtc())).get().storage.success,
                "failed to publish traversal fixture");
    }

    const QString manifestPath = indexPath(temporary.path());
    QJsonObject catalog = readObject(manifestPath);
    QJsonObject manifest = firstRecord(temporary.path());
    QJsonArray displays = manifest.value(QStringLiteral("displays")).toArray();
    QJsonObject display = displays.first().toObject();
    display.insert(QStringLiteral("image_file"), QStringLiteral("../outside.png"));
    displays.replace(0, display);
    manifest.insert(QStringLiteral("displays"), displays);
    catalog.insert(QStringLiteral("records"), QJsonArray{manifest});
    writeBytes(manifestPath, QJsonDocument(catalog).toJson(QJsonDocument::Compact));
    auto recovered = storage::makeCaptureHistoryRepository(temporary.path());
    require(recovered->records().isEmpty(),
            "repository accepted a traversal display filename from the manifest");
}

void startupReadsOnlyIndexAndWorkersStartOnDemand() {
    QTemporaryDir temporary;
    const auto now = QDateTime::currentDateTimeUtc();
    storage::CaptureHistoryRecord record;
    {
        auto writer = storage::makeCaptureHistoryRepository(temporary.path());
        record = writer->publish(draftAt(now)).get().record;
    }
    const QString directory = onlyRecordDirectory(temporary.path());
    require(QDir(directory).removeRecursively(), "failed to remove fixture payloads");
    std::atomic_int indexReads{0}, payloadReads{0}, workers{0}, indexWrites{0};
    storage::CaptureHistoryRepositoryOptions options;
    options.operationObserved = [&](storage::CaptureHistoryOperation operation) {
        switch (operation) {
        case storage::CaptureHistoryOperation::IndexRead:
            ++indexReads;
            break;
        case storage::CaptureHistoryOperation::PayloadRead:
            ++payloadReads;
            break;
        case storage::CaptureHistoryOperation::WorkerStarted:
            ++workers;
            break;
        case storage::CaptureHistoryOperation::IndexWrite:
            ++indexWrites;
            break;
        }
    };
    auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    require(repository->records().size() == 1 &&
                repository->usage().recordBytes == record.totalBytes &&
                repository->displayAssets(record).has_value(),
            "metadata depended on payload files");
    repository->drain();
    require(indexReads == 1 && payloadReads == 0 && workers == 0 && indexWrites == 0,
            "metadata-only startup started work or read payloads");
    require(!repository->load(record), "missing payload read succeeded");
    repository->drain();
    require(workers == 1 && repository->records().isEmpty() &&
                repository->usage().pendingDeletionBytes == 0,
            "read failure was not committed");
    payloadReads = 0;
    require(repository->publish(draftAt(now)).get().storage.success, "publication failed");
    require(payloadReads == 0, "publication reread its payloads");
}

void resultReadDoesNotInspectOtherPayloads() {
    QTemporaryDir temporary;
    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    auto draft = draftAt(QDateTime::currentDateTimeUtc());
    draft.resultImage = draft.displays.first().image;
    const auto published = repository->publish(draft).get();
    require(published.storage.success, "failed to publish isolated read fixture");
    const QDir directory(onlyRecordDirectory(temporary.path()));
    writeBytes(directory.filePath(QStringLiteral("canvas_history.json")), QByteArrayLiteral("bad"));
    require(QFile::remove(directory.filePath(QStringLiteral("display_0.png"))),
            "failed to remove unused display");
    require(repository->loadResultImage(published.record).has_value() &&
                repository->records().size() == 1,
            "result read depended on unrelated payloads");
    require(!repository->load(published.record), "invalid canvas read succeeded");
    repository->drain();
    require(repository->records().isEmpty(), "invalid canvas was not removed");
}

void indexFailurePreservesFilesUntilClear() {
    QTemporaryDir temporary;
    {
        auto writer = storage::makeCaptureHistoryRepository(temporary.path());
        require(writer->publish(draftAt(QDateTime::currentDateTimeUtc())).get().storage.success,
                "failed to create index failure fixture");
    }
    const auto directory = onlyRecordDirectory(temporary.path());
    writeBytes(indexPath(temporary.path()), QByteArrayLiteral("broken index"));
    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    require(!repository->lastError().isEmpty() && repository->records().isEmpty() &&
                QFileInfo::exists(directory),
            "broken index destroyed payloads");
    require(!repository->publish(draftAt(QDateTime::currentDateTimeUtc())).get().storage.success,
            "broken index allowed publication");
    QFile broken(indexPath(temporary.path()));
    require(broken.open(QIODevice::ReadOnly) &&
                broken.readAll() == QByteArrayLiteral("broken index"),
            "broken index was overwritten");
    broken.close();
    require(repository->requestClear().get().success &&
                repository->publish(draftAt(QDateTime::currentDateTimeUtc())).get().storage.success,
            "explicit clear did not reset broken index");
}

void failedCommitPreservesPublishedHistory() {
    QTemporaryDir temporary;
    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    const auto first = repository->publish(draftAt(QDateTime::currentDateTimeUtc())).get();
    require(first.storage.success, "failed to publish commit fixture");
    auto policy = repository->policy();
    policy.maxEntries = 1;
    require(repository->updatePolicy(policy).get().success, "failed to change capacity");
    const QString index = indexPath(temporary.path());
    const QString saved = index + QStringLiteral(".saved");
    require(QFile::rename(index, saved) && QDir().mkdir(index), "failed to block index commit");
    const auto failed = repository->publish(draftAt(QDateTime::currentDateTimeUtc())).get();
    require(!failed.storage.success && repository->records() == QVector{first.record} &&
                repository->load(first.record).has_value(),
            "failed commit evicted acknowledged history");
    require(QDir().rmdir(index) && QFile::rename(saved, index), "failed to restore index");
    repository.reset();
    auto reopened = storage::makeCaptureHistoryRepository(temporary.path());
    require(reopened->records() == QVector{first.record},
            "failed publication appeared after restart");
}

void pendingDeletionResumesWithoutScanningOrphans() {
    QTemporaryDir temporary;
    storage::CaptureHistoryRecord record;
    {
        auto writer = storage::makeCaptureHistoryRepository(temporary.path());
        record = writer->publish(draftAt(QDateTime::currentDateTimeUtc())).get().record;
    }
    const auto directory = onlyRecordDirectory(temporary.path());
    auto index = readObject(indexPath(temporary.path()));
    index.insert(QStringLiteral("records"), QJsonArray{});
    index.insert(QStringLiteral("pending_deletions"),
                 QJsonArray{QJsonObject{{QStringLiteral("id"), record.id},
                                        {QStringLiteral("bytes"), record.totalBytes}}});
    writeBytes(indexPath(temporary.path()), QJsonDocument(index).toJson());
    const auto orphan =
        QDir(temporary.path()).filePath(QStringLiteral("capture_history/records/.tmp-orphan"));
    require(QDir().mkpath(orphan), "failed to create orphan");
    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    repository->drain();
    require(!QFileInfo::exists(directory) && QFileInfo::exists(orphan) &&
                repository->usage().pendingDeletionBytes == 0 &&
                readObject(indexPath(temporary.path()))
                    .value(QStringLiteral("pending_deletions"))
                    .toArray()
                    .isEmpty(),
            "pending cleanup failed or scanned unrelated payloads");
}

void startupExpiresAgeButDoesNotEnforceCapacity() {
    QTemporaryDir temporary;
    const auto now = QDateTime::currentDateTimeUtc();
    storage::CaptureHistoryRepositoryOptions options;
    options.clock = [now]() { return now; };
    options.policy.retentionDays = 365;
    {
        auto writer = storage::makeCaptureHistoryRepository(temporary.path(), options);
        for (const auto& date : {now.addDays(-8), now.addDays(-7), now}) {
            require(writer->publish(draftAt(date)).get().storage.success,
                    "failed to publish retention fixture");
        }
    }
    options.policy.retentionDays = 7;
    options.policy.maxEntries = 1;
    auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    repository->drain();
    require(repository->records().size() == 2 &&
                repository->records().last().createdUtc == now.addDays(-7),
            "startup enforced capacity or got the retention cutoff wrong");
    repository.reset();
    options.writeAvailable = false;
    options.policy.retentionDays = 1;
    auto readOnly = storage::makeCaptureHistoryRepository(temporary.path(), options);
    require(readOnly->records().size() == 2 && !readOnly->requestClear().get().success,
            "read-only repository modified history");
}

void batchRemovalCommitsAndNotifiesOnce() {
    QTemporaryDir temporary;
    std::atomic_int indexWrites{0};
    std::atomic_int recordChanges{0};
    storage::CaptureHistoryRepositoryOptions options;
    options.operationObserved = [&](storage::CaptureHistoryOperation operation) {
        if (operation == storage::CaptureHistoryOperation::IndexWrite) {
            ++indexWrites;
        }
    };
    options.callbacks.recordsChanged = [&]() { ++recordChanges; };
    auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    QVector<storage::CaptureHistoryRecord> published;
    for (int index = 0; index < 4; ++index) {
        const auto result = repository->publish(draftAt(now.addSecs(-index))).get();
        require(result.storage.success, "failed to publish batch-removal fixture");
        published.push_back(result.record);
    }
    const auto payloadPath = [&](const QString& id) {
        return QDir(temporary.path())
            .filePath(QStringLiteral("capture_history/records/%1").arg(id));
    };
    indexWrites = 0;
    recordChanges = 0;

    const QString unknownId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto result =
        repository->removeMany({published[0].id, published[2].id, published[0].id, unknownId})
            .get();
    require(result.success, "batch removal failed");
    const QVector<storage::CaptureHistoryRecord> remaining = repository->records();
    require(remaining.size() == 2 && remaining[0].id == published[1].id &&
                remaining[1].id == published[3].id,
            "batch removal did not retain exactly the unselected records");
    require(repository->usage().entryCount == 2 && recordChanges.load() == 1,
            "batch removal did not update usage and notify exactly once");
    require(indexWrites.load() == 2,
            "batch removal must use one state commit and one payload-cleanup commit");
    require(!QFileInfo::exists(payloadPath(published[0].id)) &&
                !QFileInfo::exists(payloadPath(published[2].id)) &&
                QFileInfo::exists(payloadPath(published[1].id)) &&
                QFileInfo::exists(payloadPath(published[3].id)),
            "batch removal did not clean only the selected payload directories");

    indexWrites = 0;
    recordChanges = 0;
    require(repository->removeMany({}).get().success &&
                repository->removeMany({unknownId, unknownId}).get().success,
            "empty and unknown-only removal batches must be successful no-ops");
    require(indexWrites.load() == 0 && recordChanges.load() == 0,
            "no-op removal batches must not write or notify");

    repository.reset();
    storage::CaptureHistoryRepositoryOptions readOnlyOptions;
    readOnlyOptions.writeAvailable = false;
    auto readOnly =
        storage::makeCaptureHistoryRepository(temporary.path(), std::move(readOnlyOptions));
    require(readOnly->records().size() == 2 &&
                !readOnly->removeMany({published[1].id}).get().success &&
                readOnly->records().size() == 2,
            "read-only batch removal modified persisted history");
    readOnly.reset();

    repository = storage::makeCaptureHistoryRepository(temporary.path());
    require(repository->remove(published[1].id).get().success &&
                repository->records().size() == 1 &&
                repository->records().first().id == published[3].id,
            "single removal no longer follows the shared batch path");
}

void permanentHistoryBypassesLimitsAndAllowsManualDeletion() {
    QTemporaryDir temporary;
    const auto now = QDateTime::fromString(QStringLiteral("2026-09-08T12:00:00Z"), Qt::ISODate);
    storage::CaptureHistoryRepositoryOptions options;
    options.clock = [now]() { return now; };
    options.policy.retentionDays = 1;
    options.policy.maxEntries = 1;
    options.policy.maxDiskMiB = storage::CaptureHistoryPolicy::MinimumDiskMiB;
    require(!options.policy.keepPermanently, "permanent history must default to off");
    auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    auto oversized = draftAt(now);
    const auto& image = oversized.displays.first().image;
    auto encoded = pngBytes(image, 7);
    // PNG readers allow trailing bytes; exceed the quota without a huge decoded image.
    encoded.append(QByteArray(options.policy.maxDiskMiB * 1024 * 1024, '\0'));
    oversized.preparedResultImage = storage::PreparedPngImage::fromBytes(
        image.size(), std::make_shared<const QByteArray>(std::move(encoded)));
    require(oversized.preparedResultImage.has_value(), "oversized PNG fixture must be valid");
    require(!repository->publish(oversized).get().storage.success,
            "bounded history must reject a record larger than its disk quota");
    options.policy.keepPermanently = true;
    require(repository->updatePolicy(options.policy).get().success,
            "failed to enable permanent history");
    require(repository->publish(oversized).get().storage.success &&
                repository->publish(draftAt(now.addDays(-400))).get().storage.success &&
                repository->records().size() == 2 &&
                repository->usage().recordBytes > options.policy.maxDiskMiB * 1024LL * 1024,
            "permanent history must bypass age, count, disk, and publication limits");
    repository.reset();
    repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    repository->drain();
    require(repository->records().size() == 2,
            "startup must preserve permanent history beyond all limits");
    auto bounded = options.policy;
    bounded.keepPermanently = false;
    require(repository->updatePolicy(bounded).get().success && repository->records().size() == 1,
            "turning off permanent history must resume age cleanup");
    require(repository->publish(draftAt(now.addSecs(1))).get().storage.success &&
                repository->records().size() == 1 &&
                repository->records().first().createdUtc == now.addSecs(1),
            "turning off permanent history must resume capacity cleanup on publication");
    require(repository->updatePolicy(options.policy).get().success &&
                repository->requestClear().get().success && repository->records().isEmpty(),
            "permanent history must still allow explicit clearing");
}

void clearCancelsQueuedPublicationsAndShutdownDrains() {
    QTemporaryDir temporary;
    std::promise<void> started, release;
    auto released = release.get_future().share();
    storage::CaptureHistoryRepositoryOptions options;
    options.operationObserved = [&](storage::CaptureHistoryOperation operation) {
        if (operation == storage::CaptureHistoryOperation::WorkerStarted) {
            started.set_value();
            released.wait();
        }
    };
    auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    auto publication = repository->publish(draftAt(QDateTime::currentDateTimeUtc()));
    started.get_future().wait();
    auto cleared = repository->requestClear();
    require(!publication.get().storage.success, "clear did not cancel queued publication");
    release.set_value();
    require(cleared.get().success, "clear failed");
    auto accepted = repository->publish(draftAt(QDateTime::currentDateTimeUtc()));
    repository.reset();
    require(accepted.get().storage.success, "shutdown abandoned an accepted publication");
}
} // namespace

void compoundSelectionSurvivesRepositoryRestart() {
    QTemporaryDir directory;
    require(directory.isValid(), "compound history temporary directory");
    auto draft = draftAt(QDateTime::currentDateTimeUtc());
    draft.selection.region = QRegion(draft.canvasBounds).subtracted(QRect(8, 6, 10, 10));
    {
        auto repository = storage::makeCaptureHistoryRepository(directory.path());
        const auto published = repository->publish(draft).get();
        require(published.storage.success && published.record.selection == draft.selection,
                "publishing history must preserve region geometry");
    }
    auto repository = storage::makeCaptureHistoryRepository(directory.path());
    require(repository->records().size() == 1 &&
                repository->records().first().selection == draft.selection,
            "reopened screenshot history must retain holes");
}

void revisionCheckedMutations() {
    QTemporaryDir directory;
    auto repository = storage::makeCaptureHistoryRepository(directory.path());
    const auto initial = repository->recordsSnapshot();
    auto first = draftAt(QDateTime::currentDateTimeUtc());
    require(repository->publish(first).get().storage.success, "revision fixture publication");
    const auto published = repository->recordsSnapshot();
    require(published.revision > initial.revision && published.records.size() == 1,
            "history snapshot atomically reports records and revision");
    require(!repository->removeIfRevision({first.id}, initial.revision).get().success &&
                repository->records().size() == 1,
            "stale deletion must not remove a newer history record");
    auto second = draftAt(QDateTime::currentDateTimeUtc().addSecs(1));
    require(repository->publish(second).get().storage.success,
            "second revision fixture publication");
    require(!repository->removeIfRevision({}, published.revision, true).get().success &&
                repository->records().size() == 2,
            "stale clear must not remove new captures");
    require(repository->removeIfRevision({first.id}, repository->recordsSnapshot().revision)
                    .get()
                    .success &&
                repository->records().size() == 1 && repository->records().first().id == second.id,
            "revision-checked deletion affects only the selected record");
}

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    revisionCheckedMutations();
    compoundSelectionSurvivesRepositoryRestart();
    pointGeometryRoundTripsAndLegacyIndexRemainsReadable();
    sourceCanvasOriginsRoundTripAndRejectInvalidCoordinates();
    scrollingMarkerRoundTripsAndRejectsNonBooleanValues();
    desktopGeometryRoundTripsAndValidatesCoordinates();
    publicationAndRecovery();
    preparedResultBytesAreCommittedWithoutReplacement();
    displayCompressionIsIndependentOfResultCompression();
    quickCaptureSourcesRoundTrip();
    trustedStartupAndExplicitClear();
    policyBoundariesAndDisabledPreservation();
    publicationQueueCapacity();
    displayAssetsAreMetadataOnly();
    traversalManifestIsRejected();
    startupReadsOnlyIndexAndWorkersStartOnDemand();
    resultReadDoesNotInspectOtherPayloads();
    indexFailurePreservesFilesUntilClear();
    failedCommitPreservesPublishedHistory();
    pendingDeletionResumesWithoutScanningOrphans();
    startupExpiresAgeButDoesNotEnforceCapacity();
    batchRemovalCommitsAndNotifiesOnce();
    permanentHistoryBypassesLimitsAndAllowsManualDeletion();
    clearCancelsQueuedPublicationsAndShutdownDrains();
    return 0;
}
