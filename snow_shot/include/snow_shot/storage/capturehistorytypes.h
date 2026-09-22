#ifndef SNOW_SHOT_STORAGE_CAPTUREHISTORYTYPES_H
#define SNOW_SHOT_STORAGE_CAPTUREHISTORYTYPES_H

#include "snow_shot/storage/storageresult.h"
#include "snow_shot/storage/preparedpngimage.h"

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QRect>
#include <QString>
#include <QUrl>
#include <QVector>

#include <optional>

namespace snow_shot::storage {
enum class CaptureHistoryContentKind { ScreenshotSession, Image };
enum class CaptureHistorySource {
    CopiedToClipboard,
    SavedToFile,
    PinnedToScreen,
    CurrentMonitor,
    FocusedWindow,
};

struct PersistedSelection {
    QRect rectangle;
    int cornerRadius = 0;
    int shadowWidth = 0;
    QColor shadowColor;
    bool lockAspectRatio = false;
    bool lockDragAspectRatio = false;

    friend bool operator==(const PersistedSelection& first, const PersistedSelection& second) {
        return first.rectangle == second.rectangle && first.cornerRadius == second.cornerRadius &&
               first.shadowWidth == second.shadowWidth && first.shadowColor == second.shadowColor &&
               first.lockAspectRatio == second.lockAspectRatio &&
               first.lockDragAspectRatio == second.lockDragAspectRatio;
    }
};

struct CaptureHistoryDisplayDraft {
    QString stableId;
    QString name;
    QImage image;
    // Missing origins follow the matched display; explicit origins anchor captured pixels.
    std::optional<QPoint> sourceCanvasOrigin{};
    std::optional<QRect> sourceCanvasRect{};
    bool canvasUsesPoints = false;
    qreal backingScale = 0.0;
    quint32 nativeDisplayId = 0;
};

struct CaptureHistoryResultRecord {
    QSize imageSize;
    qint64 encodedBytes = 0;

    friend bool operator==(const CaptureHistoryResultRecord& first,
                           const CaptureHistoryResultRecord& second) {
        return first.imageSize == second.imageSize && first.encodedBytes == second.encodedBytes;
    }
};

struct CaptureHistoryResultAsset {
    QString recordId;
    QSize imageSize;
    QUrl localFileUrl;

    friend bool operator==(const CaptureHistoryResultAsset& first,
                           const CaptureHistoryResultAsset& second) {
        return first.recordId == second.recordId && first.imageSize == second.imageSize &&
               first.localFileUrl == second.localFileUrl;
    }
};

// Maps selection canvas coordinates to the desktop at capture time. Windows uses physical
// pixels; macOS uses points. Missing on older records, whose absolute position is unknown.
struct CaptureHistoryDesktopGeometry {
    QPoint canvasOrigin;
    bool canvasUsesPoints = false;

    friend bool operator==(const CaptureHistoryDesktopGeometry&,
                           const CaptureHistoryDesktopGeometry&) = default;
};

struct CaptureHistoryDraft {
    CaptureHistoryContentKind contentKind = CaptureHistoryContentKind::ScreenshotSession;
    QString id;
    QDateTime createdUtc;
    QRect canvasBounds;
    PersistedSelection selection;
    QByteArray canvasHistory;
    QVector<CaptureHistoryDisplayDraft> displays;
    std::optional<QImage> resultImage;
    std::optional<PreparedPngImage> preparedResultImage;
    int pngCompressionLevel = 0;
    CaptureHistorySource source = CaptureHistorySource::CopiedToClipboard;
    // Absent on records persisted before the scrolling marker existed.
    std::optional<bool> scrolling{};
    std::optional<CaptureHistoryDesktopGeometry> desktopGeometry{};
};

struct CaptureHistoryDisplayRecord {
    QString stableId;
    QString name;
    QSize imageSize;
    qint64 encodedBytes = 0;
    std::optional<QPoint> sourceCanvasOrigin{};
    std::optional<QRect> sourceCanvasRect{};
    bool canvasUsesPoints = false;
    qreal backingScale = 0.0;
    quint32 nativeDisplayId = 0;

    friend bool operator==(const CaptureHistoryDisplayRecord& first,
                           const CaptureHistoryDisplayRecord& second) {
        return first.stableId == second.stableId && first.name == second.name &&
               first.imageSize == second.imageSize && first.encodedBytes == second.encodedBytes &&
               first.sourceCanvasOrigin == second.sourceCanvasOrigin &&
               first.sourceCanvasRect == second.sourceCanvasRect &&
               first.canvasUsesPoints == second.canvasUsesPoints &&
               first.backingScale == second.backingScale &&
               first.nativeDisplayId == second.nativeDisplayId;
    }
};

struct CaptureHistoryRecord {
    CaptureHistoryContentKind contentKind = CaptureHistoryContentKind::ScreenshotSession;
    QString id;
    QDateTime createdUtc;
    QRect canvasBounds;
    PersistedSelection selection;
    QVector<CaptureHistoryDisplayRecord> displays;
    std::optional<CaptureHistoryResultRecord> result;
    qint64 canvasBytes = 0;
    qint64 totalBytes = 0;
    CaptureHistorySource source = CaptureHistorySource::CopiedToClipboard;
    // Absent on records persisted before the scrolling marker existed.
    std::optional<bool> scrolling{};
    std::optional<CaptureHistoryDesktopGeometry> desktopGeometry{};

    friend bool operator==(const CaptureHistoryRecord& first, const CaptureHistoryRecord& second) {
        return first.contentKind == second.contentKind && first.id == second.id &&
               first.createdUtc == second.createdUtc && first.canvasBounds == second.canvasBounds &&
               first.selection == second.selection && first.displays == second.displays &&
               first.result == second.result && first.canvasBytes == second.canvasBytes &&
               first.totalBytes == second.totalBytes && first.source == second.source &&
               first.scrolling == second.scrolling &&
               first.desktopGeometry == second.desktopGeometry;
    }
};

struct CaptureHistoryDisplayAsset {
    QString recordId;
    QString stableId;
    QString name;
    QSize imageSize;
    QUrl localFileUrl;

    friend bool operator==(const CaptureHistoryDisplayAsset& first,
                           const CaptureHistoryDisplayAsset& second) {
        return first.recordId == second.recordId && first.stableId == second.stableId &&
               first.name == second.name && first.imageSize == second.imageSize &&
               first.localFileUrl == second.localFileUrl;
    }
};

struct CaptureHistoryAssetSet {
    QString recordId;
    std::optional<CaptureHistoryResultAsset> result;
    QVector<CaptureHistoryDisplayAsset> displays;

    friend bool operator==(const CaptureHistoryAssetSet& first,
                           const CaptureHistoryAssetSet& second) {
        return first.recordId == second.recordId && first.result == second.result &&
               first.displays == second.displays;
    }
};

struct CaptureHistoryPayload {
    QByteArray canvasHistory;
    QVector<QImage> displayImages;
};

struct CaptureHistoryPolicy {
    static constexpr int MinimumRetentionDays = 1;
    static constexpr int MaximumRetentionDays = 365;
    static constexpr int MinimumEntries = 1;
    static constexpr int MaximumEntries = 1000;
    static constexpr int MinimumDiskMiB = 128;
    static constexpr int MaximumDiskMiB = 10240;

    bool enabled = true;
    int retentionDays = 7;
    int maxEntries = 100;
    int maxDiskMiB = 1024;
    bool keepPermanently = false;

    [[nodiscard]] bool isValid() const {
        return retentionDays >= MinimumRetentionDays && retentionDays <= MaximumRetentionDays &&
               maxEntries >= MinimumEntries && maxEntries <= MaximumEntries &&
               maxDiskMiB >= MinimumDiskMiB && maxDiskMiB <= MaximumDiskMiB;
    }

    friend bool operator==(const CaptureHistoryPolicy& first, const CaptureHistoryPolicy& second) {
        return first.enabled == second.enabled && first.retentionDays == second.retentionDays &&
               first.maxEntries == second.maxEntries && first.maxDiskMiB == second.maxDiskMiB &&
               first.keepPermanently == second.keepPermanently;
    }
};

struct CaptureHistoryUsage {
    int entryCount = 0;
    qint64 recordBytes = 0;
    qint64 indexBytes = 0;
    qint64 pendingDeletionBytes = 0;
    qint64 totalBytes = 0;

    friend bool operator==(const CaptureHistoryUsage& first, const CaptureHistoryUsage& second) {
        return first.entryCount == second.entryCount && first.recordBytes == second.recordBytes &&
               first.indexBytes == second.indexBytes &&
               first.pendingDeletionBytes == second.pendingDeletionBytes &&
               first.totalBytes == second.totalBytes;
    }
};

struct CaptureHistoryPublishResult {
    StorageResult storage;
    CaptureHistoryRecord record;
};
} // namespace snow_shot::storage

#endif // SNOW_SHOT_STORAGE_CAPTUREHISTORYTYPES_H
