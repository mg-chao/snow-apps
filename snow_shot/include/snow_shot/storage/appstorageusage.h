#ifndef SNOW_SHOT_STORAGE_APPSTORAGEUSAGE_H
#define SNOW_SHOT_STORAGE_APPSTORAGEUSAGE_H

#include <QMetaType>
#include <QString>

namespace snow_shot::storage {

enum class StorageCacheKind {
    ThumbnailCache,
    RecordingTemp,
};

// App-wide on-disk usage snapshot covering every location the app owns: the
// app data directory (capture history, pinned windows, OCR assets, remaining
// files) plus the thumbnail-cache and recording-temp cache directories.
struct AppStorageUsage {
    qint64 historyBytes = 0;
    qint64 pinnedWindowBytes = 0;
    qint64 ocrAssetBytes = 0;
    qint64 thumbnailCacheBytes = 0;
    qint64 recordingTempBytes = 0;
    qint64 otherBytes = 0;
    bool scanning = false;

    [[nodiscard]] qint64 totalBytes() const {
        return historyBytes + pinnedWindowBytes + ocrAssetBytes + thumbnailCacheBytes +
               recordingTempBytes + otherBytes;
    }

    friend bool operator==(const AppStorageUsage& first, const AppStorageUsage& second) {
        return first.historyBytes == second.historyBytes &&
               first.pinnedWindowBytes == second.pinnedWindowBytes &&
               first.ocrAssetBytes == second.ocrAssetBytes &&
               first.thumbnailCacheBytes == second.thumbnailCacheBytes &&
               first.recordingTempBytes == second.recordingTempBytes &&
               first.otherBytes == second.otherBytes && first.scanning == second.scanning;
    }
};

} // namespace snow_shot::storage

Q_DECLARE_METATYPE(snow_shot::storage::AppStorageUsage)

#endif // SNOW_SHOT_STORAGE_APPSTORAGEUSAGE_H
