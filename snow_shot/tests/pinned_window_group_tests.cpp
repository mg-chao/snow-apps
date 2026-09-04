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
    return QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v3/index.json"));
}

QJsonObject readManifest(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "failed to read pinned-window manifest");
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    require(document.isObject(), "pinned-window manifest is not an object");
    return document.object();
}

void defaultGroupAndFreshSchema() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");

    storage::PinnedWindowRepository repository(directory.path());
    require(repository.groups().size() == 1 && repository.groups().front().id == "default" &&
                repository.groups().front().builtIn,
            "the default group should always be materialized");
    require(repository.activeGroupId() == "default", "default group should be active initially");
    require(repository.upsert(record(QUuid::createUuid().toString(QUuid::WithoutBraces))).success,
            "failed to seed a pinned-window record");
    require(repository.flush().success, "failed to flush the v3 pinned-window index");

    const QJsonObject manifest = readManifest(manifestPath(directory));
    require(manifest.value(QStringLiteral("format_version")).toInt() == 3,
            "the fresh pinned-window index should use the v3 schema");
    require(manifest.value(QStringLiteral("groups")).toArray().size() == 1 &&
                manifest.value(QStringLiteral("records")).toArray().size() == 1,
            "the fresh pinned-window index should contain the default group and seeded record");

    QTemporaryDir legacyDirectory;
    require(legacyDirectory.isValid(), "legacy fixture directory is unavailable");
    const QString legacyPath = QDir(legacyDirectory.path()).filePath(
        QStringLiteral("pinned_windows/manifest.json"));
    QDir().mkpath(QFileInfo(legacyPath).absolutePath());
    QFile legacy(legacyPath);
    require(legacy.open(QIODevice::WriteOnly), "failed to create legacy pinned-window fixture");
    require(legacy.write(QByteArrayLiteral("{\"format_version\":2,\"records\":[]}")) > 0,
            "failed to write legacy pinned-window fixture");
    legacy.close();
    storage::PinnedWindowRepository fresh(legacyDirectory.path());
    require(fresh.records().isEmpty() && QFileInfo::exists(legacyPath),
            "legacy pinned-window data should remain untouched and unimported");
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

void displayOrderKeepsDefaultGroupFirst() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary storage directory is unavailable");

    storage::PinnedWindowRepository repository(directory.path());
    presentation::PinnedWindowGroupManager manager(&repository);
    const auto zeroId = manager.createGroup(QStringLiteral("000"));
    const auto alphaId = manager.createGroup(QStringLiteral("AAA"));
    const auto zuluId = manager.createGroup(QStringLiteral("Zzz"));
    require(zeroId.has_value() && alphaId.has_value() && zuluId.has_value(),
            "failed to seed groups that sort around the default group name");

    const QVector<storage::PinnedWindowGroup> ordered = manager.groupsSortedForDisplay();
    require(ordered.size() == 4, "the display order should contain every group");
    require(ordered.front().id == "default",
            "the default group must stay first in the display order");
    require(ordered.at(1).id == *zeroId && ordered.at(2).id == *alphaId &&
                ordered.at(3).id == *zuluId,
            "custom groups should keep the locale-aware name order after the default group");
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
    require(repository.flush().success,
            "the latest group revision should flush before constructing a new repository");

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
    defaultGroupAndFreshSchema();
    managerValidationPersistenceAndCounts();
    activeGroupFallbackAndEmptyDeletion();
    displayOrderKeepsDefaultGroupFirst();
    groupCountLimitIsEnforced();
    return 0;
}
