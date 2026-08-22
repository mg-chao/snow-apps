#include "snow_shot/storage/capturehistoryrepository.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>

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
    const QDir history(QDir(root).filePath(QStringLiteral("capture_history_records")));
    const QFileInfoList entries =
        history.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    require(entries.size() == 1, "expected exactly one published record directory");
    return entries.constFirst().absoluteFilePath();
}

void publicationAndRecovery() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create publication directory");
    const QDateTime now =
        QDateTime::fromString(QStringLiteral("2026-08-05T12:30:00.000Z"), Qt::ISODateWithMs);
    storage::CaptureHistoryRecord published;
    {
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        storage::CaptureHistoryDraft draft = draftAt(now);
        QImage resultImage(QSize(20, 12), QImage::Format_RGBA8888);
        resultImage.fill(qRgba(210, 80, 40, 220));
        draft.resultImage = resultImage;
        draft.source = storage::CaptureHistorySource::PinnedToScreen;
        const storage::CaptureHistoryPublishResult result = repository->publish(draft).get();
        require(result.storage.success, "self-contained publication failed");
        published = result.record;
        const QString directory = onlyRecordDirectory(temporary.path());
        const QJsonObject manifest =
            readObject(QDir(directory).filePath(QStringLiteral("manifest.json")));
        require(manifest.value(QStringLiteral("format_version")).toInt() == 2 &&
                    manifest.value(QStringLiteral("id")).toString() == published.id &&
                    manifest.value(QStringLiteral("source")).toString() ==
                        QStringLiteral("pinned_to_screen") &&
                    published.source == storage::CaptureHistorySource::PinnedToScreen &&
                    QFileInfo(QDir(directory).filePath(QStringLiteral("canvas_history.json")))
                        .isFile() &&
                    QFileInfo(QDir(directory).filePath(QStringLiteral("display_0.png"))).isFile() &&
                    QFileInfo(QDir(directory).filePath(QStringLiteral("capture_result.png"))).isFile() &&
                    published.result.has_value() && published.result->imageSize == QSize(20, 12),
                "published record is not self-describing");
        qint64 physicalBytes =
            QFileInfo(QDir(directory).filePath(QStringLiteral("manifest.json"))).size() +
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
                    usage.quarantineBytes == 0 && usage.temporaryBytes == 0 &&
                    usage.totalBytes == physicalBytes,
                "capture-history usage includes data outside self-contained records");
        const auto payload = repository->load(published);
        require(payload.has_value() && payload->displayImages.size() == 1 &&
                    payload->displayImages.constFirst().size() == QSize(32, 24),
                "published record could not be loaded");
        const auto loadedResult = repository->loadResultImage(published);
        require(loadedResult.has_value() && loadedResult->size() == QSize(20, 12) &&
                    loadedResult->pixelColor(3, 4) == resultImage.pixelColor(3, 4),
                "published result image could not be loaded");
        require(
            !QFileInfo::exists(
                QDir(temporary.path()).filePath(QStringLiteral("capture_history_catalog.json"))),
            "capture-history publication unexpectedly created a catalog");
    }

    {
        auto recovered = storage::makeCaptureHistoryRepository(temporary.path());
        require(recovered->records().size() == 1 &&
                    recovered->records().constFirst().id == published.id &&
                    recovered->records().constFirst().source ==
                        storage::CaptureHistorySource::PinnedToScreen,
                "record was not recovered from its self-contained directory");
    }
}

void quickCaptureSourcesRoundTrip() {
    const auto verifySource = [](storage::CaptureHistorySource source,
                                 const QString& manifestSource) {
        QTemporaryDir temporary;
        require(temporary.isValid(), "failed to create quick-capture source directory");
        const QDateTime createdUtc =
            QDateTime::fromString(QStringLiteral("2026-08-12T04:30:00.000Z"), Qt::ISODateWithMs);
        QString recordId;

        {
            auto repository = storage::makeCaptureHistoryRepository(temporary.path());
            storage::CaptureHistoryDraft draft = draftAt(createdUtc);
            draft.source = source;
            const storage::CaptureHistoryPublishResult result = repository->publish(draft).get();
            require(result.storage.success, "quick-capture source publication failed");
            recordId = result.record.id;

            const QString directory = onlyRecordDirectory(temporary.path());
            const QJsonObject manifest =
                readObject(QDir(directory).filePath(QStringLiteral("manifest.json")));
            require(manifest.value(QStringLiteral("source")).toString() == manifestSource &&
                        result.record.source == source,
                    "quick-capture source was not encoded in the manifest");
        }

        auto recovered = storage::makeCaptureHistoryRepository(temporary.path());
        require(recovered->records().size() == 1 &&
                    recovered->records().constFirst().id == recordId &&
                    recovered->records().constFirst().source == source,
                "quick-capture source was not recovered from its manifest");
    };

    verifySource(storage::CaptureHistorySource::CurrentMonitor, QStringLiteral("current_monitor"));
    verifySource(storage::CaptureHistorySource::FocusedWindow, QStringLiteral("focused_window"));
}

void quarantineTemporaryCleanupAndClear() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create quarantine directory");
    QString recordDirectory;
    {
        auto repository = storage::makeCaptureHistoryRepository(temporary.path());
        require(repository->publish(draftAt(QDateTime::currentDateTimeUtc())).get().storage.success,
                "failed to publish quarantine fixture");
        recordDirectory = onlyRecordDirectory(temporary.path());
    }
    writeBytes(QDir(recordDirectory).filePath(QStringLiteral("canvas_history.json")),
               QByteArrayLiteral("not-json"));
    const QString temporaryRecord =
        QDir(temporary.path()).filePath(QStringLiteral("capture_history_records/.tmp-abandoned"));
    require(QDir().mkpath(temporaryRecord), "failed to create temporary fixture");
    writeBytes(QDir(temporaryRecord).filePath(QStringLiteral("partial")), QByteArrayLiteral("x"));
    const QString expiredQuarantine =
        QDir(temporary.path())
            .filePath(QStringLiteral(
                "capture_history_quarantine/expired.quarantine-20200101-000000-000"));
    require(QDir().mkpath(expiredQuarantine), "failed to create expired quarantine fixture");
    writeBytes(QDir(expiredQuarantine).filePath(QStringLiteral("partial")), QByteArrayLiteral("x"));

    auto repository = storage::makeCaptureHistoryRepository(temporary.path());
    require(repository->records().isEmpty() && !QFileInfo::exists(temporaryRecord) &&
                !QFileInfo::exists(expiredQuarantine) && repository->usage().quarantineBytes > 0,
            "startup did not clean temporary data and quarantine the invalid record");
    const auto clearResult = repository->requestClear().get();
    require(clearResult.success && repository->usage().entryCount == 0 &&
                repository->usage().totalBytes == 0 &&
                !QFileInfo::exists(
                    QDir(temporary.path()).filePath(QStringLiteral("capture_history_records"))) &&
                !QFileInfo::exists(
                    QDir(temporary.path()).filePath(QStringLiteral("capture_history_quarantine"))),
            "clear did not remove every managed history area");
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
    require(repository->updatePolicy(policy).get().success && repository->records().size() == 1 &&
                repository->records().constFirst().createdUtc == now.addSecs(-1),
            "re-enabling history did not prune oldest-first");
}

void publicationQueueCapacity() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "failed to create worker directory");
    storage::CaptureHistoryRepositoryOptions options;
    options.maxQueuedPublications = 2;
    auto repository = storage::makeCaptureHistoryRepository(temporary.path(), options);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    auto first = repository->publish(draftAt(now, QSize(2048, 2048)));
    auto second = repository->publish(draftAt(now.addMSecs(1), QSize(2048, 2048)));
    auto third = repository->publish(draftAt(now.addMSecs(2), QSize(2048, 2048)));
    auto overloaded = repository->publish(draftAt(now.addMSecs(3), QSize(32, 32)));
    require(overloaded.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready &&
                !overloaded.get().storage.success,
            "publication overload was not rejected immediately");
    require(first.get().storage.success && second.get().storage.success &&
                third.get().storage.success,
            "worker did not allow one active and two queued publications");
    repository->drain();
    require(repository->records().size() == 3,
            "serialized worker did not retain all accepted publications");
}
void displayAssetsAreValidatedWithoutPayloadDecode() {
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
        require(!repository->displayAssets(record).has_value(),
                "asset lookup accepted a symlinked display file");
        require(QFile::remove(imagePath), "failed to remove display symlink fixture");
    }
    writeBytes(imagePath, encodedImage);

    writeBytes(imagePath, QByteArray(encodedSize, '\0'));
    require(!repository->displayAssets(record).has_value(),
            "asset lookup accepted a deliberately corrupted image payload");
    repository->drain();
    require(!repository->load(record).has_value(),
            "payload load unexpectedly accepted the deliberately corrupted image");
    require(repository->records().isEmpty(),
            "invalid payload was not removed after lazy validation failure");

    require(!QFileInfo::exists(imagePath), "quarantine left the invalid display payload in place");
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

    const QString recordDirectory = onlyRecordDirectory(temporary.path());
    const QString manifestPath = QDir(recordDirectory).filePath(QStringLiteral("manifest.json"));
    QJsonObject manifest = readObject(manifestPath);
    QJsonArray displays = manifest.value(QStringLiteral("displays")).toArray();
    QJsonObject display = displays.first().toObject();
    display.insert(QStringLiteral("image_file"), QStringLiteral("../outside.png"));
    displays.replace(0, display);
    manifest.insert(QStringLiteral("displays"), displays);
    writeBytes(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact));
    auto recovered = storage::makeCaptureHistoryRepository(temporary.path());
    require(recovered->records().isEmpty(),
            "repository accepted a traversal display filename from the manifest");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    publicationAndRecovery();
    quickCaptureSourcesRoundTrip();
    quarantineTemporaryCleanupAndClear();
    policyBoundariesAndDisabledPreservation();
    publicationQueueCapacity();
    displayAssetsAreValidatedWithoutPayloadDecode();
    traversalManifestIsRejected();
    return 0;
}
