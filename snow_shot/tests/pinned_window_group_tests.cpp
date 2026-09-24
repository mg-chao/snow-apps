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
    value.initialWindowSize = QSize(2, 2);
    value.screenDpi = 1.0;
    value.firstCreationTextDpi = 1.0;
    value.scalePercent = 100.0;
    value.opacityPercent = 100;
    value.imageTransform = QTransform();
    return value;
}

QString manifestPath(const QTemporaryDir& directory) {
    return QDir(directory.path()).filePath(QStringLiteral("pinned_windows_v2/index.json"));
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
    require(repository.flush().success, "failed to flush the pinned-window index");

    const QJsonObject manifest = readManifest(manifestPath(directory));
    require(manifest.value(QStringLiteral("format_version")).toInt() == 2,
            "the fresh pinned-window index should use the current schema");
    require(manifest.value(QStringLiteral("groups")).toArray().size() == 1 &&
                manifest.value(QStringLiteral("records")).toArray().size() == 1,
            "the fresh pinned-window index should contain the default group and seeded record");

    QTemporaryDir unrelatedDirectory;
    require(unrelatedDirectory.isValid(), "unrelated fixture directory is unavailable");
    const QString unrelatedPath =
        QDir(unrelatedDirectory.path()).filePath(QStringLiteral("unrelated_pins/manifest.json"));
    QDir().mkpath(QFileInfo(unrelatedPath).absolutePath());
    QFile unrelated(unrelatedPath);
    require(unrelated.open(QIODevice::WriteOnly),
            "failed to create unrelated pinned-window fixture");
    require(unrelated.write(QByteArrayLiteral("{\"format_version\":1,\"records\":[]}")) > 0,
            "failed to write unrelated pinned-window fixture");
    unrelated.close();
    storage::PinnedWindowRepository fresh(unrelatedDirectory.path());
    require(fresh.summaries().isEmpty() && QFileInfo::exists(unrelatedPath),
            "unrelated pinned-window files should remain untouched and unimported");
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
    first.hideToTopMode = true;
    first.hideToTopHandleNativeGeometry = QRect(20, 0, 30, 6);
    first.hideToTopAccentIndex = 4;
    require(repository.upsert(first).success, "failed to seed an alpha record");
    require(repository.upsert(record(secondId)).success, "failed to seed a default record");
    require(manager.windowCount(*alphaId) == 1 && manager.windowCount("default") == 1,
            "group counts should include persisted records");
    require(manager.windowCounts(*alphaId).nonIgnored == 1 &&
                manager.windowCounts(*alphaId).total == 1,
            "persisted pins should contribute to both group counts");
    manager.registerPendingPin(QStringLiteral("pending-alpha"), *alphaId);
    require(manager.windowCounts(*alphaId).nonIgnored == 2 &&
                manager.windowCounts(*alphaId).total == 2,
            "pending pins should contribute to both group counts");
    manager.completePendingPin(QStringLiteral("pending-alpha"));

    require(manager.setActiveGroup(*alphaId), "activating a user group should succeed");
    require(repository.activeGroupId() == *alphaId, "the active group should be persisted");
    presentation::PinnedWindowGroupManager restored(&repository);
    require(restored.activeGroupId() == *alphaId, "the active group should survive manager reload");
    const auto restoredRecord = repository.loadRecord(firstId);
    require(restoredRecord && restoredRecord->hideToTopMode &&
                restoredRecord->hideToTopHandleNativeGeometry ==
                    first.hideToTopHandleNativeGeometry &&
                restoredRecord->hideToTopAccentIndex == 4,
            "group switches must preserve hidden geometry and stable accent metadata");
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

void specifiedGroupDeletionPreservesUnrelatedState() {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary specified-group storage is unavailable");

    storage::PinnedWindowRepository repository(directory.path());
    presentation::PinnedWindowGroupManager manager(&repository);
    const auto alphaId = manager.createGroup(QStringLiteral("Alpha"));
    const auto betaId = manager.createGroup(QStringLiteral("Beta"));
    require(alphaId.has_value() && betaId.has_value(),
            "failed to create specified-deletion groups");
    const QString defaultRecordId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString alphaRecordId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString betaRecordId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    storage::PinnedWindowRecord alphaRecord = record(alphaRecordId);
    storage::PinnedWindowRecord betaRecord = record(betaRecordId);
    alphaRecord.groupId = *alphaId;
    betaRecord.groupId = *betaId;
    require(repository.upsert(record(defaultRecordId)).success &&
                repository.upsert(alphaRecord).success && repository.upsert(betaRecord).success,
            "failed to seed specified-deletion records");
    manager.registerPendingPin(QStringLiteral("pending-alpha"), *alphaId);
    require(manager.setActiveGroup(*alphaId), "Alpha should be active before deletion");

    int groupsChanged = 0;
    int activeChanged = 0;
    int restoreRequests = 0;
    QStringList deletionRequests;
    QObject::connect(&manager, &presentation::PinnedWindowGroupManager::groupsChanged,
                     [&groupsChanged]() { ++groupsChanged; });
    QObject::connect(&manager, &presentation::PinnedWindowGroupManager::activeGroupChanged,
                     [&activeChanged](const QString&) { ++activeChanged; });
    QObject::connect(&manager,
                     &presentation::PinnedWindowGroupManager::restoreActiveGroupWindowsRequested,
                     [&restoreRequests]() { ++restoreRequests; });
    QObject::connect(&manager, &presentation::PinnedWindowGroupManager::groupDeletionRequested,
                     [&deletionRequests](const QString& id) { deletionRequests.push_back(id); });

    require(manager.deleteSpecifiedGroup(QStringLiteral("default")),
            "clearing Default should succeed");
    QCoreApplication::processEvents();
    require(manager.contains(QStringLiteral("default")) && manager.activeGroupId() == *alphaId &&
                !repository.loadRecord(defaultRecordId).has_value() &&
                repository.loadRecord(alphaRecordId).has_value() && activeChanged == 0 &&
                restoreRequests == 0 && deletionRequests == QStringList{QStringLiteral("default")},
            "clearing Default must preserve the group, active group, and unrelated records");

    require(manager.deleteSpecifiedGroup(*alphaId),
            "deleting the active custom group should succeed");
    QCoreApplication::processEvents();
    require(!manager.contains(*alphaId) && manager.contains(*betaId) &&
                manager.activeGroupId() == "default" && manager.windowCount(*alphaId) == 0 &&
                !repository.loadRecord(alphaRecordId).has_value() &&
                repository.loadRecord(betaRecordId).has_value() && activeChanged == 1 &&
                restoreRequests == 1 && deletionRequests.size() == 2 &&
                deletionRequests.back() == *alphaId && groupsChanged >= 2,
            "active custom deletion must remove pending and persisted state then restore Default");
    require(!manager.deleteSpecifiedGroup(QStringLiteral("missing-group")) &&
                deletionRequests.size() == 2,
            "unknown specified deletion must fail without emitting a window deletion request");
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
void ignoredRecordsCountTowardTotalAndDeleteWithEmptyGroups() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    presentation::PinnedWindowGroupManager manager(&repository);
    const auto group = manager.createGroup(QStringLiteral("Closed only"));
    require(group.has_value(), "create closed-only group");
    auto item = record(QUuid::createUuid().toString(QUuid::WithoutBraces));
    item.groupId = *group;
    require(repository.upsert(item).success && manager.windowCount(*group) == 1,
            "retained pins count toward group membership");
    require(repository.markClosed(item.id).success && manager.windowCount(*group) == 0 &&
                manager.windowCounts(*group).total == 1,
            "ignored pins should remain in the total after closing");
    manager.registerPendingPin(item.id, *group);
    require(manager.windowCounts(*group).nonIgnored == 1 && manager.windowCounts(*group).total == 1,
            "an ignored pin being restored should count once in the total");
    manager.completePendingPin(item.id);
    require(manager.windowCounts(*group).nonIgnored == 0 && manager.windowCounts(*group).total == 1,
            "completing a pending pin should refresh the non-ignored count");
    require(manager.deleteEmptyGroups() && !manager.contains(*group) &&
                !repository.loadRecord(item.id).has_value(),
            "empty-group cleanup should delete an ignored-only group and its saved pins");
}

void persistedMetadataDoesNotRefreshGroupMenus() {
    QTemporaryDir directory;
    storage::PinnedWindowRepository repository(directory.path(), true, 30000);
    presentation::PinnedWindowGroupManager manager(&repository);
    auto item = record(QUuid::createUuid().toString(QUuid::WithoutBraces));
    require(repository.upsert(item).success && manager.windowCount(QStringLiteral("default")) == 1,
            "prime persisted group counts");
    int updates = 0;
    QObject::connect(&manager, &presentation::PinnedWindowGroupManager::groupsChanged, &manager,
                     [&updates]() { ++updates; });
    item.opacityPercent = 75;
    require(repository.updateState(item).success, "update pin metadata without changing counts");
    manager.onPinnedRecordsChanged();
    QCoreApplication::processEvents();
    require(updates == 0, "metadata-only changes do not rebuild group menus");
    require(repository.markClosed(item.id).success, "close counted pin");
    manager.onPinnedRecordsChanged();
    QCoreApplication::processEvents();
    require(updates == 1, "closing a pin refreshes group counts");
    require(repository.markRestored(item.id).success, "restore counted pin");
    manager.onPinnedRecordsChanged();
    QCoreApplication::processEvents();
    require(updates == 2, "restoring a pin refreshes group counts");
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    ignoredRecordsCountTowardTotalAndDeleteWithEmptyGroups();
    persistedMetadataDoesNotRefreshGroupMenus();
    defaultGroupAndFreshSchema();
    managerValidationPersistenceAndCounts();
    activeGroupFallbackAndEmptyDeletion();
    specifiedGroupDeletionPreservesUnrelatedState();
    displayOrderKeepsDefaultGroupFirst();
    groupCountLimitIsEnforced();
    return 0;
}
