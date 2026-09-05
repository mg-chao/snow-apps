#include "snow_shot/storage/storageusagetracker.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

namespace storage = snow_shot::storage;

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void writeBytes(const QString& path, qint64 size) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "failed to create test directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "failed to open test file");
    const QByteArray payload(size, 'x');
    require(file.write(payload) == payload.size(), "failed to write test file");
    file.close();
}

void setLastModified(const QString& path, const QDateTime& when) {
    namespace fs = std::filesystem;
    const auto moment = std::chrono::clock_cast<fs::file_time_type::clock>(
        std::chrono::system_clock::time_point{std::chrono::milliseconds(when.toMSecsSinceEpoch())});
    std::error_code error;
    fs::last_write_time(fs::path(path.toStdWString()), moment, error);
    require(!error, "failed to set test file timestamp");
}

struct UsageRecorder {
    std::mutex mutex;
    std::vector<storage::AppStorageUsage> usages;
    std::vector<std::pair<storage::StorageCacheKind, storage::StorageResult>> clears;

    storage::StorageUsageTrackerOptions::Callbacks callbacks() {
        return {
            [this](const storage::AppStorageUsage& usage) {
                std::lock_guard<std::mutex> lock(mutex);
                usages.push_back(usage);
            },
            [this](storage::StorageCacheKind kind, const storage::StorageResult& result) {
                std::lock_guard<std::mutex> lock(mutex);
                clears.push_back({kind, result});
            },
        };
    }
};

struct TrackerDirs {
    QTemporaryDir temporary;
    QString appData;
    QString thumbnails;
    QString recordingTemp;

    TrackerDirs()
        : appData(QDir(temporary.path()).filePath(QStringLiteral("app-data"))),
          thumbnails(QDir(temporary.path()).filePath(QStringLiteral("thumbnails"))),
          recordingTemp(QDir(temporary.path()).filePath(QStringLiteral("recordings"))) {
        require(temporary.isValid(), "failed to create tracker test directory");
    }
};

storage::StorageUsageTrackerOptions
trackerOptions(const TrackerDirs& dirs, storage::StorageUsageTrackerOptions::Callbacks callbacks,
               const QDateTime& cutoff = QDateTime::currentDateTime()) {
    storage::StorageUsageTrackerOptions options;
    options.appDataDirectory = dirs.appData;
    options.thumbnailCacheDirectory = dirs.thumbnails;
    options.recordingTempDirectory = dirs.recordingTemp;
    options.activeFileCutoff = cutoff;
    options.callbacks = std::move(callbacks);
    return options;
}

void scanCategorizesAppOwnedLocations() {
    TrackerDirs dirs;
    writeBytes(
        QDir(dirs.appData).filePath(QStringLiteral("capture_history_records/rec1/manifest.json")),
        100);
    writeBytes(
        QDir(dirs.appData).filePath(QStringLiteral("capture_history_quarantine/q1/payload.bin")),
        50);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("pinned_windows_v3/index.json")), 25);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("assets/ocr/model.onnx")), 200);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 10);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("stray.log")), 5);
    writeBytes(QDir(dirs.thumbnails).filePath(QStringLiteral("thumb.png")), 30);
    writeBytes(QDir(dirs.recordingTemp).filePath(QStringLiteral("bundle.bin")), 40);

    UsageRecorder recorder;
    {
        storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
        tracker.requestRefresh();
        tracker.drain();
        const storage::AppStorageUsage usage = tracker.usage();
        require(!usage.scanning, "a drained tracker must not report scanning");
        require(usage.historyBytes == 150, "history bytes must cover records and quarantine");
        require(usage.pinnedWindowBytes == 25, "pinned window bytes must match payload");
        require(usage.ocrAssetBytes == 200, "ocr asset bytes must match payload");
        require(usage.thumbnailCacheBytes == 30, "thumbnail cache bytes must match payload");
        require(usage.recordingTempBytes == 40, "recording temp bytes must match payload");
        require(usage.otherBytes == 15, "other bytes must cover config and stray files");
        require(usage.totalBytes() == 460, "total bytes must be the sum of all categories");
    }

    std::lock_guard<std::mutex> lock(recorder.mutex);
    require(recorder.usages.size() >= 2,
            "a scan must publish a scanning snapshot and a final snapshot");
    require(recorder.usages.front().scanning, "the first published snapshot must be scanning");
    require(!recorder.usages.back().scanning && recorder.usages.back().totalBytes() == 460,
            "the last published snapshot must carry the final usage");
}

void constructionDefersScanningUntilRefresh() {
    TrackerDirs dirs;
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 10);
    writeBytes(QDir(dirs.thumbnails).filePath(QStringLiteral("thumb.png")), 30);

    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    tracker.drain();
    const storage::AppStorageUsage idle = tracker.usage();
    require(!idle.scanning && idle.totalBytes() == 0,
            "a fresh tracker must not scan or report usage before a refresh");
    {
        std::lock_guard<std::mutex> lock(recorder.mutex);
        require(recorder.usages.empty(), "a fresh tracker must not publish usage before a refresh");
    }

    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().totalBytes() == 40,
            "the first refresh must scan every app-owned location");
}

void workerRestartsAfterGoingIdle() {
    TrackerDirs dirs;
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 10);
    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    for (int round = 0; round < 3; ++round) {
        tracker.requestRefresh();
        tracker.drain();
    }
    require(tracker.usage().otherBytes == 10,
            "refreshes submitted after an idle gap must respawn the worker and scan");

    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("extra.bin")), 20);
    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().otherBytes == 30, "a respawned worker must observe new files");
}

void missingLocationsReportZeroUsage() {
    TrackerDirs dirs;
    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    tracker.requestRefresh();
    tracker.drain();
    const storage::AppStorageUsage usage = tracker.usage();
    require(!usage.scanning && usage.totalBytes() == 0 && usage.historyBytes == 0 &&
                usage.pinnedWindowBytes == 0 && usage.ocrAssetBytes == 0 &&
                usage.thumbnailCacheBytes == 0 && usage.recordingTempBytes == 0 &&
                usage.otherBytes == 0,
            "missing locations must report zero usage");
}

void refreshRequestsAreCoalesced() {
    TrackerDirs dirs;
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 10);
    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().otherBytes == 10, "the initial refresh must observe the config file");

    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("extra.bin")), 20);
    for (int index = 0; index < 5; ++index) {
        tracker.requestRefresh();
    }
    tracker.drain();
    require(tracker.usage().otherBytes == 30, "coalesced refreshes must observe new files");

    std::lock_guard<std::mutex> lock(recorder.mutex);
    const int scans = static_cast<int>(recorder.usages.size());
    require(scans >= 4 && scans <= 6,
            "burst refreshes must coalesce into a single rescan of the changed state");
}

void historyBytesProviderReplacesDirectoryWalk() {
    TrackerDirs dirs;
    writeBytes(
        QDir(dirs.appData).filePath(QStringLiteral("capture_history_records/rec1/manifest.json")),
        100);
    writeBytes(
        QDir(dirs.appData).filePath(QStringLiteral("capture_history_quarantine/q1/payload.bin")),
        50);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 10);
    writeBytes(
        QDir(dirs.appData).filePath(QStringLiteral("capture_history/records/id/display.png")), 80);

    std::atomic<int> providerCalls{0};
    storage::StorageUsageTrackerOptions options = trackerOptions(dirs, {});
    std::atomic_int scans{0};
    options.directoryScanObserved = [&](const QString&) { ++scans; };
    options.historyBytesProvider = [&providerCalls]() {
        ++providerCalls;
        return static_cast<qint64>(777);
    };
    UsageRecorder recorder;
    options.callbacks = recorder.callbacks();
    storage::StorageUsageTracker tracker(options);
    tracker.requestRefresh();
    tracker.drain();

    const storage::AppStorageUsage usage = tracker.usage();
    require(usage.historyBytes == 777,
            "a history provider must replace the recursive history directory walk");
    require(usage.otherBytes == 10, "other bytes must still come from the disk walk");
    require(usage.totalBytes() == 787, "total bytes must combine the provider with the walk");
    require(providerCalls.load() >= 1, "the history provider must be consulted on each scan");
    require(scans == 0, "history provider still performed a recursive directory scan");
}

void staleRefreshReusesRecentScan() {
    TrackerDirs dirs;
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 10);
    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    tracker.requestRefresh();
    tracker.drain();
    std::size_t scansAfterFirst = 0;
    {
        std::lock_guard<std::mutex> lock(recorder.mutex);
        scansAfterFirst = recorder.usages.size();
        require(scansAfterFirst >= 2, "the first refresh must publish scanning and final usage");
    }

    // A completed scan is authoritative inside the staleness window, even when
    // files changed on disk in the meantime.
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("extra.bin")), 20);
    tracker.requestRefreshIfStale(std::chrono::hours(1));
    tracker.drain();
    {
        std::lock_guard<std::mutex> lock(recorder.mutex);
        require(recorder.usages.size() == scansAfterFirst,
                "a refresh inside the staleness window must reuse the cached snapshot");
    }
    require(tracker.usage().otherBytes == 10,
            "a skipped refresh must leave the cached usage untouched");

    // A zero window and an explicit request must both rescan.
    tracker.requestRefreshIfStale(std::chrono::milliseconds(0));
    tracker.drain();
    require(tracker.usage().otherBytes == 30, "a zero staleness window must force a rescan");
    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().otherBytes == 30, "requestRefresh must bypass the staleness window");
}

void thumbnailClearRemovesOnlyThumbnailFiles() {
    TrackerDirs dirs;
    const QString kept = QDir(dirs.thumbnails).filePath(QStringLiteral("index.txt"));
    const QString lowercase = QDir(dirs.thumbnails).filePath(QStringLiteral("a.png"));
    const QString uppercase = QDir(dirs.thumbnails).filePath(QStringLiteral("b.PNG"));
    writeBytes(lowercase, 10);
    writeBytes(uppercase, 20);
    writeBytes(kept, 30);

    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().thumbnailCacheBytes == 60, "thumbnail bytes must match payloads");

    const auto result = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    tracker.drain();
    require(result.valid() && result.get().success, "thumbnail clear must succeed");
    require(!QFile::exists(lowercase) && !QFile::exists(uppercase),
            "thumbnail clear must remove png files case-insensitively");
    require(QFile::exists(kept), "thumbnail clear must keep non-thumbnail files");
    require(tracker.usage().thumbnailCacheBytes == 30,
            "usage must reflect the files removed by the clear");

    std::lock_guard<std::mutex> lock(recorder.mutex);
    require(!recorder.clears.empty() &&
                recorder.clears.back().first == storage::StorageCacheKind::ThumbnailCache &&
                recorder.clears.back().second.success,
            "thumbnail clear must report completion through the callback");
}

void recordingTempClearHonorsActiveCutoff() {
    TrackerDirs dirs;
    const QDateTime cutoff = QDateTime::currentDateTime();
    const QString stale = QDir(dirs.recordingTemp).filePath(QStringLiteral("stale.pcm"));
    const QString staleDirectory =
        QDir(dirs.recordingTemp).filePath(QStringLiteral("stale-bundle"));
    const QString active = QDir(dirs.recordingTemp).filePath(QStringLiteral("active.pcm"));
    writeBytes(stale, 10);
    writeBytes(QDir(staleDirectory).filePath(QStringLiteral("bundle.bin")), 20);
    writeBytes(active, 40);
    setLastModified(stale, cutoff.addSecs(-3600));
    setLastModified(staleDirectory, cutoff.addSecs(-3600));
    setLastModified(active, cutoff.addSecs(3600));

    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks(), cutoff));
    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().recordingTempBytes == 70,
            "recording temp bytes must include stale and active files");

    const auto result = tracker.requestClear(storage::StorageCacheKind::RecordingTemp);
    tracker.drain();
    require(result.valid() && result.get().success, "recording temp clear must succeed");
    require(!QFile::exists(stale) && !QDir(staleDirectory).exists(),
            "recording temp clear must remove stale entries");
    require(QFile::exists(active), "recording temp clear must keep active-session files");
    require(tracker.usage().recordingTempBytes == 40,
            "usage must reflect only the active session after the clear");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("storage-usage-tracker-tests"));
    scanCategorizesAppOwnedLocations();
    constructionDefersScanningUntilRefresh();
    workerRestartsAfterGoingIdle();
    missingLocationsReportZeroUsage();
    refreshRequestsAreCoalesced();
    historyBytesProviderReplacesDirectoryWalk();
    staleRefreshReusesRecentScan();
    thumbnailClearRemovesOnlyThumbnailFiles();
    recordingTempClearHonorsActiveCutoff();
    return 0;
}
