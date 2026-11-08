#include "snow_shot/storage/storageusagetracker.h"
#include "snow_shot/app/distribution.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QStandardPaths>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <system_error>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winioctl.h>
#include <cstring>
#endif

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
    const auto systemMoment =
        std::chrono::system_clock::time_point{std::chrono::milliseconds(when.toMSecsSinceEpoch())};
#if __cpp_lib_chrono >= 201907L
    const auto moment = std::chrono::clock_cast<fs::file_time_type::clock>(systemMoment);
#else
    // libc++ provides the file-clock conversion but not C++20 clock_cast yet.
    const auto moment = fs::file_time_type::clock::from_sys(systemMoment);
#endif
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
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("capture_history/index.json")), 40);
    writeBytes(
        QDir(dirs.appData).filePath(QStringLiteral("capture_history/records/rec1/display.png")),
        60);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("pinned_windows_v2/index.json")), 25);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("assets/ocr/model.onnx")), 200);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 10);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("stray.log")), 5);
    writeBytes(QDir(dirs.thumbnails).filePath(QStringLiteral("thumb.png")), 30);
    writeBytes(QDir(dirs.recordingTemp).filePath(QStringLiteral("bundle.bin")), 40);
    const QString logs = QDir(dirs.appData).filePath(QStringLiteral("logs"));
    const QString fallback = QDir(dirs.temporary.path()).filePath(QStringLiteral("fallback-logs"));
    writeBytes(QDir(logs).filePath(QStringLiteral("today.log")), 17);
    writeBytes(QDir(fallback).filePath(QStringLiteral("crashes/report.dmp")), 23);

    UsageRecorder recorder;
    {
        auto options = trackerOptions(dirs, recorder.callbacks());
        options.diagnosticsDirectories = {logs, logs, fallback, fallback,
                                          QDir(fallback).filePath(QStringLiteral("crashes"))};
        storage::StorageUsageTracker tracker(options);
        tracker.requestRefresh();
        tracker.drain();
        const storage::AppStorageUsage usage = tracker.usage();
        require(!usage.scanning, "a drained tracker must not report scanning");
        require(usage.historyBytes == 100, "history bytes must cover the history tree");
        require(usage.pinnedWindowBytes == 25, "pinned window bytes must match payload");
        require(usage.ocrAssetBytes == 200, "ocr asset bytes must match payload");
        require(usage.thumbnailCacheBytes == 30, "thumbnail cache bytes must match payload");
        require(usage.recordingTempBytes == 40, "recording temp bytes must match payload");
        require(usage.otherBytes == 15, "other bytes must cover config and stray files");
        require(usage.diagnosticsBytes == 40, "fallback diagnostics are counted exactly once");
        require(usage.totalBytes() == 450, "total bytes must be the sum of all categories");
    }

    std::lock_guard<std::mutex> lock(recorder.mutex);
    require(recorder.usages.size() >= 2,
            "a scan must publish a scanning snapshot and a final snapshot");
    require(recorder.usages.front().scanning, "the first published snapshot must be scanning");
    require(!recorder.usages.back().scanning && recorder.usages.back().totalBytes() == 450,
            "the last published snapshot must carry the final usage");
}

void legacyLogsOutsideConfiguredRootsAreNotScanned() {
    TrackerDirs dirs;
    const QString legacy = QDir(dirs.appData).filePath(QStringLiteral("logs"));
    const QString current = dirs.temporary.filePath(QStringLiteral("roaming/logs"));
    writeBytes(QDir(legacy).filePath(QStringLiteral("old.log")), 99);
    writeBytes(QDir(current).filePath(QStringLiteral("current.log")), 17);
    QStringList observed;
    auto options = trackerOptions(dirs, {});
    options.diagnosticsDirectories = {current, QDir(current).filePath(QStringLiteral("crashes"))};
    options.directoryScanObserved = [&](const QString& path) { observed.append(path); };
    storage::StorageUsageTracker tracker(options);
    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().diagnosticsBytes == 17 && !observed.contains(legacy) &&
                QFileInfo(QDir(legacy).filePath(QStringLiteral("old.log"))).size() == 99,
            "usage only scans the configured diagnostic roots and leaves legacy logs untouched");
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

void emptyLocationsDoNotInspectOrClearTheWorkingDirectory() {
    QTemporaryDir temporary;
    require(temporary.isValid(), "empty-location fixture must be available");
    const QString previousDirectory = QDir::currentPath();
    const auto restoreDirectory = qScopeGuard([&] { QDir::setCurrent(previousDirectory); });
    require(QDir::setCurrent(temporary.path()), "empty-location fixture must become current");
    const QString thumbnail = temporary.filePath(QStringLiteral("keep.png"));
    const QString recording = temporary.filePath(QStringLiteral("keep/session.pcm"));
    writeBytes(thumbnail, 11);
    writeBytes(recording, 13);
    storage::StorageUsageTrackerOptions options;
    options.diagnosticsDirectories = {QString()};
    storage::StorageUsageTracker tracker(options);
    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().totalBytes() == 0,
            "unavailable locations must not scan the current directory");
    const auto thumbnails = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    const auto recordings = tracker.requestClear(storage::StorageCacheKind::RecordingTemp);
    tracker.drain();
    require(thumbnails.get().success && recordings.get().success && QFile::exists(thumbnail) &&
                QFile::exists(recording),
            "unavailable locations must not delete current-directory files");
}

void nestedCachesAreCountedOnce() {
    TrackerDirs dirs;
    dirs.thumbnails = QDir(dirs.appData).filePath(QStringLiteral("cache/thumbnails"));
    dirs.recordingTemp = QDir(dirs.appData).filePath(QStringLiteral("cache/recordings"));
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 7);
    writeBytes(QDir(dirs.thumbnails).filePath(QStringLiteral("thumb.png")), 11);
    writeBytes(QDir(dirs.recordingTemp).filePath(QStringLiteral("session.pcm")), 13);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("cache/clipboard/export.png")), 17);
    storage::StorageUsageTracker tracker(trackerOptions(dirs, {}));
    tracker.requestRefresh();
    tracker.drain();
    const auto usage = tracker.usage();
    require(usage.thumbnailCacheBytes == 11 && usage.recordingTempBytes == 13 &&
                usage.otherBytes == 24 && usage.totalBytes() == 48,
            "nested caches must retain their category without double counting app data");
}

void defaultCachesFollowTheDistributionPolicy() {
    namespace distribution = snow_shot::app::distribution;
    if (!distribution::isStoreManaged()) {
        const QString root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        const QString recordingRoot =
            root.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::TempLocation) : root;
        require(storage::StorageUsageTracker::defaultThumbnailCacheDirectory() ==
                        (root.isEmpty() ? QString()
                                        : QDir(root).filePath(
                                              QStringLiteral("snow-shot/history-thumbnails"))) &&
                    storage::StorageUsageTracker::defaultRecordingTempDirectory() ==
                        (recordingRoot.isEmpty()
                             ? QString()
                             : QDir(recordingRoot).filePath(QStringLiteral("recordings"))),
                "standalone cache defaults must retain their native paths");
        return;
    }
    QTemporaryDir temporary;
    require(temporary.isValid(), "Store cache fixture must be available");
    const bool hadLocal = qEnvironmentVariableIsSet("LOCALAPPDATA");
    const QByteArray previousLocal = qgetenv("LOCALAPPDATA");
    const auto restoreLocal = qScopeGuard([&] {
        if (hadLocal)
            qputenv("LOCALAPPDATA", previousLocal);
        else
            qunsetenv("LOCALAPPDATA");
    });
    require(qputenv("LOCALAPPDATA", temporary.path().toUtf8()),
            "Store cache LOCALAPPDATA fixture must be set");
    const QString localState = distribution::writableDataDirectory();
    const QString cache = QDir(localState).filePath(QStringLiteral("cache"));
    const QString thumbnails = storage::StorageUsageTracker::defaultThumbnailCacheDirectory();
    const QString recordings = storage::StorageUsageTracker::defaultRecordingTempDirectory();
    require(!localState.isEmpty() &&
                thumbnails ==
                    QDir(cache).filePath(QStringLiteral("snow-shot/history-thumbnails")) &&
                recordings == QDir(cache).filePath(QStringLiteral("recordings")),
            "Store caches must remain inside the stable package LocalState directory");
    const QString nativeThumbnail = temporary.filePath(QStringLiteral("native/cache/thumb.png"));
    const QString nativeRecording = temporary.filePath(QStringLiteral("native/recordings/a.pcm"));
    writeBytes(nativeThumbnail, 19);
    writeBytes(nativeRecording, 23);
    writeBytes(QDir(thumbnails).filePath(QStringLiteral("thumb.png")), 11);
    writeBytes(QDir(recordings).filePath(QStringLiteral("a.pcm")), 13);
    storage::StorageUsageTrackerOptions options;
    options.appDataDirectory = localState;
    options.thumbnailCacheDirectory = thumbnails;
    options.recordingTempDirectory = recordings;
    storage::StorageUsageTracker tracker(options);
    const auto clearThumbnails = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    const auto clearRecordings = tracker.requestClear(storage::StorageCacheKind::RecordingTemp);
    tracker.drain();
    require(clearThumbnails.get().success && clearRecordings.get().success &&
                QFile::exists(nativeThumbnail) && QFile::exists(nativeRecording),
            "clearing Store caches must preserve standalone files");
    require(qunsetenv("LOCALAPPDATA"), "Store cache LOCALAPPDATA fixture must be unset");
    require(storage::StorageUsageTracker::defaultThumbnailCacheDirectory().isEmpty() &&
                storage::StorageUsageTracker::defaultRecordingTempDirectory().isEmpty(),
            "unavailable Store cache roots must not fall back to shared directories");
    require(qputenv("LOCALAPPDATA", "relative-store-data"),
            "relative Store cache LOCALAPPDATA fixture must be set");
    require(storage::StorageUsageTracker::defaultThumbnailCacheDirectory().isEmpty() &&
                storage::StorageUsageTracker::defaultRecordingTempDirectory().isEmpty(),
            "relative Store cache roots must not create working-directory caches");
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
        QDir(dirs.appData).filePath(QStringLiteral("capture_history/records/id/display.png")), 80);
    writeBytes(QDir(dirs.appData).filePath(QStringLiteral("config.json")), 10);

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
    const QString sidecar = lowercase + QStringLiteral(".size");
    const QString orphan = QDir(dirs.thumbnails).filePath(QStringLiteral("orphan.PNG.SIZE"));
    const QString nested = QDir(dirs.thumbnails).filePath(QStringLiteral("nested/keep.png"));
    writeBytes(lowercase, 10);
    writeBytes(uppercase, 20);
    writeBytes(sidecar, 5);
    writeBytes(orphan, 7);
    writeBytes(nested, 11);
    writeBytes(kept, 30);

    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    tracker.requestRefresh();
    tracker.drain();
    require(tracker.usage().thumbnailCacheBytes == 83, "thumbnail bytes must match payloads");

    const auto result = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    tracker.drain();
    require(result.valid() && result.get().success, "thumbnail clear must succeed");
    require(!QFile::exists(lowercase) && !QFile::exists(uppercase) && !QFile::exists(sidecar) &&
                !QFile::exists(orphan),
            "thumbnail clear removes PNGs and orphan size sidecars case-insensitively");
    require(QFile::exists(kept) && QFile::exists(nested),
            "thumbnail clear must keep unrelated files and subdirectories");
    require(tracker.usage().thumbnailCacheBytes == 41,
            "usage must reflect the files removed by the clear");

    std::lock_guard<std::mutex> lock(recorder.mutex);
    require(!recorder.clears.empty() &&
                recorder.clears.back().first == storage::StorageCacheKind::ThumbnailCache &&
                recorder.clears.back().second.success,
            "thumbnail clear must report completion through the callback");
}

void thumbnailClearRemovesEmptyDirectoriesWithoutCreatingMissingOnes() {
    TrackerDirs dirs;
    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    const auto missing = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    tracker.drain();
    require(missing.get().success && !QDir(dirs.thumbnails).exists(),
            "clearing a missing cache must not create a directory");
    writeBytes(QDir(dirs.thumbnails).filePath(QStringLiteral("orphan.png.size")), 5);
    const auto empty = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    tracker.drain();
    require(empty.get().success && !QDir(dirs.thumbnails).exists() &&
                tracker.usage().thumbnailCacheBytes == 0,
            "clearing the final sidecar removes the empty cache directory");
}

void thumbnailClearSkipsLinkedDirectories() {
    TrackerDirs dirs;
    const QString external = dirs.temporary.filePath(QStringLiteral("external"));
    const QString kept = QDir(external).filePath(QStringLiteral("keep.png"));
    writeBytes(kept, 19);
#ifdef Q_OS_WIN
    // Junctions need no symlink privilege and exercise the Windows reparse path.
    require(QDir().mkpath(dirs.thumbnails), "create junction fixture");
    const auto native = QDir::toNativeSeparators(dirs.thumbnails).toStdWString();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "open thumbnail junction fixture");
    struct JunctionData {
        DWORD tag;
        WORD length;
        WORD reserved;
        WORD substituteOffset;
        WORD substituteLength;
        WORD printOffset;
        WORD printLength;
        wchar_t paths[2048];
    } data{};
    const auto print = QDir::toNativeSeparators(external).toStdWString();
    const auto target = std::wstring(1, L'\\') + L"??" + L'\\' + print;
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substituteLength = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.printOffset = static_cast<WORD>((target.size() + 1) * sizeof(wchar_t));
    data.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    data.length = static_cast<WORD>(8 + data.printOffset + data.printLength + sizeof(wchar_t));
    std::memcpy(data.paths, target.c_str(), (target.size() + 1) * sizeof(wchar_t));
    std::memcpy(reinterpret_cast<char*>(data.paths) + data.printOffset, print.c_str(),
                (print.size() + 1) * sizeof(wchar_t));
    DWORD returned = 0;
    const bool created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.length + 8,
                                         nullptr, 0, &returned, nullptr) != 0;
    CloseHandle(handle);
    require(created, "create thumbnail junction without elevation");
#else
    std::error_code error;
    std::filesystem::create_directory_symlink(external.toStdString(), dirs.thumbnails.toStdString(),
                                              error);
    require(!error, "create thumbnail directory symlink");
#endif
    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    const auto linkedRoot = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    tracker.drain();
    require(linkedRoot.get().success && QFile::exists(kept),
            "cleanup must never traverse a linked cache root");
    const QString nestedLink = dirs.temporary.filePath(QStringLiteral("cache/link.png"));
    require(QDir().mkpath(QFileInfo(nestedLink).absolutePath()) &&
                QDir().rename(dirs.thumbnails, nestedLink),
            "move cache link into a real cache directory");
    dirs.thumbnails = QFileInfo(nestedLink).absolutePath();
    storage::StorageUsageTracker nestedTracker(trackerOptions(dirs, recorder.callbacks()));
    const auto linkedEntry = nestedTracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    nestedTracker.drain();
    require(linkedEntry.get().success && QFile::exists(kept) && QFileInfo::exists(nestedLink),
            "cleanup must preserve links inside the cache");
#ifdef Q_OS_WIN
    require(RemoveDirectoryW(QDir::toNativeSeparators(nestedLink).toStdWString().c_str()) != 0,
            "remove only the test junction");
#else
    require(QFile::remove(nestedLink), "remove only the test symlink");
#endif
}

void thumbnailClearRetriesDeletionFailures() {
#ifdef Q_OS_WIN
    TrackerDirs dirs;
    const QString locked = QDir(dirs.thumbnails).filePath(QStringLiteral("locked.png.size"));
    writeBytes(locked, 7);
    const auto native = QDir::toNativeSeparators(locked).toStdWString();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "hold thumbnail sidecar against deletion");
    UsageRecorder recorder;
    storage::StorageUsageTracker tracker(trackerOptions(dirs, recorder.callbacks()));
    const auto failed = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    tracker.drain();
    require(!failed.get().success && QFile::exists(locked) &&
                tracker.usage().thumbnailCacheBytes == 7,
            "locked thumbnail failures retain the file and report its disk usage");
    CloseHandle(handle);
    const auto retried = tracker.requestClear(storage::StorageCacheKind::ThumbnailCache);
    tracker.drain();
    require(retried.get().success && !QDir(dirs.thumbnails).exists(),
            "a later cleanup retries files after their deletion lock is released");
#endif
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
    defaultCachesFollowTheDistributionPolicy();
    emptyLocationsDoNotInspectOrClearTheWorkingDirectory();
    nestedCachesAreCountedOnce();
    if (application.arguments().contains(QStringLiteral("--distribution-paths-only")))
        return 0;
    scanCategorizesAppOwnedLocations();
    legacyLogsOutsideConfiguredRootsAreNotScanned();
    constructionDefersScanningUntilRefresh();
    workerRestartsAfterGoingIdle();
    missingLocationsReportZeroUsage();
    refreshRequestsAreCoalesced();
    historyBytesProviderReplacesDirectoryWalk();
    staleRefreshReusesRecentScan();
    thumbnailClearRemovesOnlyThumbnailFiles();
    thumbnailClearRemovesEmptyDirectoriesWithoutCreatingMissingOnes();
    thumbnailClearSkipsLinkedDirectories();
    thumbnailClearRetriesDeletionFailures();
    recordingTempClearHonorsActiveCutoff();
    return 0;
}
