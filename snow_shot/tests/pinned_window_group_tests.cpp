#include "snow_shot/presentation/pinnedwindowgroupmanager.h"
#include "snow_shot/storage/pinnedwindowrepository.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdlib>
#include <iostream>
#include <utility>

namespace storage = snow_shot::storage;
namespace presentation = snow_shot::presentation;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

storage::PinnedWindowRecord record(const QString& id) {
    storage::PinnedWindowRecord value;
    value.id = id;
    value.image = QImage(2, 2, QImage::Format_ARGB32_Premultiplied);
    value.image.fill(Qt::white);
    value.nativeGeometry = QRect(0, 0, 2, 2);
    value.canvasSourceRect = QRectF(0, 0, 2, 2);
    value.contentCanvasRect = QRectF(0, 0, 2, 2);
    value.surfaceCanvasRect = QRectF(0, 0, 2, 2);
    value.initialPhysicalSize = QSize(2, 2);
    value.screenDpi = 1.0;
    value.firstCreationTextDpi = 1.0;
    value.scalePercent = 100.0;
    value.opacityPercent = 100;
    value.imageTransform = QTransform();
    return value;
}

QString manifestPath(const QTemporaryDir& directory) {
    return QDir(directory.path()).filePath(QStringLiteral("pinned_windows/manifest.json"));
}

QJsonObject readManifest(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "failed to read pinned-window manifest");
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), "pinned-window manifest is not an object");
    return document.object();
}

void writeManifest(const QString& path, QJsonObject object) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
            "failed to open pinned-window manifest for writing");
    const QByteArray bytes = QJsonDocument(std::move(object)).toJson(QJsonDocument::Indented);
    require(file.write(bytes) == bytes.size(), "failed to write pinned-window manifest");
}

void defaultGroupAndLegacyMigration() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");

    storage::PinnedWindowRepository repository(directory.path());
    require(repository.groups().size() == 1 && repository.groups().front().id == "default" &&
                repository.groups().front().builtIn,
            "the default group should always be materialized");
    require(repository.activeGroupId() == "default", "default group should be active initially");
    require(repository.upsert(record(QUuid::createUuid().toString(QUuid::WithoutBraces))).success,
            "failed to seed a pinned-window record");

    QJsonObject legacy = readManifest(manifestPath(directory));
    legacy[QStringLiteral("format_version")] = 1;
    legacy.remove(QStringLiteral("groups"));
    legacy.remove(QStringLiteral("active_group_id"));
    QJsonArray records = legacy.value(QStringLiteral("records")).toArray();
    // QJsonValue::toObject() returns a detached object, so write each object
    // back into the array after removing the new field.
    for (int index = 0; index < records.size(); ++index) {
        QJsonObject object = records.at(index).toObject();
        object.remove(QStringLiteral("group_id"));
        records[index] = object;
    }
    legacy[QStringLiteral("records")] = records;
    writeManifest(manifestPath(directory), legacy);

    storage::PinnedWindowRepository migrated(directory.path());
    require(migrated.groups().size() == 1 && migrated.activeGroupId() == "default" &&
                migrated.records().size() == 1 && migrated.records().front().groupId == "default",
            "legacy pinned records should migrate into the default group");
    require(migrated.flush().success, "migrated manifest should be writable");
    require(readManifest(manifestPath(directory)).value(QStringLiteral("format_version")).toInt() == 2,
            "flushing a migrated manifest should write the current format");
}

void managerValidationPersistenceAndCounts() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");

    storage::PinnedWindowRepository repository(directory.path());
    presentation::PinnedWindowGroupManager manager(&repository);
    const auto alphaId = manager.createGroup(QStringLiteral("  Alpha  "));
    require(alphaId.has_value() && manager.displayName(*alphaId) == "Alpha",
            "group names should be trimmed and persisted");
    require(!manager.createGroup(QStringLiteral("alpha")).has_value(),
            "group names should be case-insensitively unique");
    require(!manager.createGroup(QStringLiteral("   ")).has_value(),
            "blank group names should be rejected");
    require(!manager.createGroup(QString(17, QLatin1Char('x'))).has_value(),
            "group names longer than 16 characters should be rejected");

    const QString firstId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString secondId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    storage::PinnedWindowRecord first = record(firstId);
    first.groupId = *alphaId;
    require(repository.upsert(first).success, "failed to seed an alpha record");
    require(repository.upsert(record(secondId)).success, "failed to seed a default record");
    require(manager.windowCount(*alphaId) == 1 && manager.windowCount("default") == 1,
            "group counts should include persisted records");

    require(manager.setActiveGroup(*alphaId), "activating a user group should succeed");
    require(repository.activeGroupId() == *alphaId, "the active group should be persisted");
    presentation::PinnedWindowGroupManager restored(&repository);
    require(restored.activeGroupId() == *alphaId, "the active group should survive manager reload");
    require(!restored.deleteEmptyGroups(),
            "a group containing persisted records should not be deleted");
    require(restored.activeGroupId() == *alphaId,
            "a group containing persisted records should not be deleted");
}

void activeGroupFallbackAndEmptyDeletion() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");

    storage::PinnedWindowRepository repository(directory.path());
    presentation::PinnedWindowGroupManager manager(&repository);
    const auto emptyId = manager.createGroup(QStringLiteral("Empty"));
    require(emptyId.has_value() && manager.setActiveGroup(*emptyId),
            "the empty group should be selectable");
    require(manager.deleteEmptyGroups(), "the empty active group should be deleted");
    require(manager.activeGroupId() == "default" && !manager.contains(*emptyId),
            "deleting the active empty group should fall back to Default");
    require(manager.groups().size() == 1 && manager.groups().front().builtIn,
            "Default must never be deleted");
}

void groupCountLimitIsEnforced() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");

    storage::PinnedWindowRepository repository(directory.path());
    presentation::PinnedWindowGroupManager manager(&repository);
    for (int index = 0; index < 127; ++index) {
        require(manager.createGroup(QStringLiteral("Group%1").arg(index)).has_value(),
                "group creation should succeed below the persisted limit");
    }
    require(manager.groups().size() == 128,
            "the built-in group plus 127 custom groups should reach the persisted limit");
    const QString lastGroupId = manager.groups().back().id;
    require(manager.setActiveGroup(lastGroupId),
            "the last group within the persisted limit should be selectable");
    require(!manager.createGroup(QStringLiteral("Overflow")).has_value(),
            "creating a group beyond the persisted limit should be rejected");
    require(repository.groups().size() == 128,
            "the repository should never persist more groups than it can load");

    auto tooManyGroups = manager.groups();
    tooManyGroups.push_back({QUuid::createUuid().toString(QUuid::WithoutBraces),
                             QStringLiteral("Direct overflow"), false});
    require(!repository.setGroups(tooManyGroups, lastGroupId).success,
            "the repository should reject oversized group sets from every caller");
    require(repository.groups().size() == 128,
            "rejecting an oversized group set should preserve the repository state");

    storage::PinnedWindowRepository restored(directory.path());
    require(restored.groups().size() == 128,
            "reloading groups at the persisted limit should preserve every group");
    require(restored.groups().back().name == QStringLiteral("Group126") &&
                restored.activeGroupId() == lastGroupId,
            "the last active group within the persisted limit should survive reload");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    defaultGroupAndLegacyMigration();
    managerValidationPersistenceAndCounts();
    activeGroupFallbackAndEmptyDeletion();
    groupCountLimitIsEnforced();
    return 0;
}
