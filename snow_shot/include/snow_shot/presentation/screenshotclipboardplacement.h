#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDPLACEMENT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDPLACEMENT_H

#include "snow_shot/storage/pinnedwindowplacement.h"
#include <QByteArray>
#include <QList>
#include <QRect>
#include <optional>

class QMimeData;
class QClipboard;

struct ScreenshotClipboardDisplay final {
    QString name;
    QString serial;
    QRect desktopBounds;
    QRect nativeBounds;
    QRect usableBounds;
    qreal scale = 1.0;
    friend bool operator==(const ScreenshotClipboardDisplay&,
                           const ScreenshotClipboardDisplay&) = default;
};

// Independent of QScreen and application lifetime; image density is separate
// from the platform's window geometry units.
struct ScreenshotClipboardPlacement final {
    snow_shot::storage::PinnedWindowPlacement placement;
    QRect windowRect;
    QSize rasterSize;
    QList<ScreenshotClipboardDisplay> displays;
    qsizetype anchorDisplayIndex = 0;
    QString filePath;
    qint64 fileSize = -1;
    qint64 fileModifiedMs = -1;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool matchesFile(const QString& path, qint64 size, qint64 modifiedMs) const;
};

struct ScreenshotClipboardResolvedPlacement final {
    qsizetype displayIndex = -1;
    QRect windowRect;
    QSize initialWindowSize;
    [[nodiscard]] bool isValid() const {
        return displayIndex >= 0 && !windowRect.isEmpty();
    }
};

inline constexpr qsizetype kScreenshotClipboardPlacementMaximumBytes = 16 * 1024;
[[nodiscard]] QString screenshotClipboardPlacementMimeType();
[[nodiscard]] QString screenshotClipboardPlacementNativeMimeType();
[[nodiscard]] QByteArray
encodeScreenshotClipboardPlacement(const ScreenshotClipboardPlacement& value);
[[nodiscard]] std::optional<ScreenshotClipboardPlacement>
decodeScreenshotClipboardPlacement(const QByteArray& bytes);
void ensureScreenshotClipboardPlacementMimeSupport();
void setScreenshotClipboardPlacement(QMimeData& mime, const ScreenshotClipboardPlacement& value);
[[nodiscard]] std::optional<ScreenshotClipboardPlacement>
readScreenshotClipboardPlacement(const QMimeData* mime);
// Native reads are bounded before copying, including the file-URL pin path.
[[nodiscard]] std::optional<ScreenshotClipboardPlacement>
snapshotScreenshotClipboardPlacement(QClipboard* clipboard);
[[nodiscard]] quint64 screenshotClipboardRevision();
[[nodiscard]] QString screenshotClipboardFilePath(const QString& path);
[[nodiscard]] ScreenshotClipboardResolvedPlacement
resolveScreenshotClipboardPlacement(const ScreenshotClipboardPlacement& value,
                                    const QList<ScreenshotClipboardDisplay>& displays,
                                    bool autoResizeWindow);

#endif
