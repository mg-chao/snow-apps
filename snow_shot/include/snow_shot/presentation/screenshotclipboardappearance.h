#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDAPPEARANCE_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDAPPEARANCE_H

#include "snow_shot/storage/pinnedwindowtypes.h"

class QClipboard;
class QMimeData;

// Effects remain baked into the image. This snapshot restores only its presentation
// and editing outline, independently of whether a desktop position is available.
struct ScreenshotClipboardAppearance final {
    QSize rasterSize;
    std::optional<snow_shot::storage::PinnedBorderAppearance> borderAppearance;
    bool checkerboardEnabled = false;
    std::optional<bool> showBorder;
    QString filePath;
    qint64 fileSize = -1;
    qint64 fileModifiedMs = -1;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] bool matchesFile(const QString& path, qint64 size, qint64 modifiedMs) const;
};

// Accommodate the existing bounded vector geometry representation without an image payload.
inline constexpr qsizetype kScreenshotClipboardAppearanceMaximumBytes = 64 * 1024 * 1024;
[[nodiscard]] QString screenshotClipboardAppearanceMimeType();
[[nodiscard]] QString screenshotClipboardAppearanceNativeMimeType();
[[nodiscard]] QByteArray
encodeScreenshotClipboardAppearance(const ScreenshotClipboardAppearance& value);
[[nodiscard]] std::optional<ScreenshotClipboardAppearance>
decodeScreenshotClipboardAppearance(const QByteArray& bytes);
void ensureScreenshotClipboardAppearanceMimeSupport();
void setScreenshotClipboardAppearance(QMimeData& mime, const ScreenshotClipboardAppearance& value);
[[nodiscard]] std::optional<ScreenshotClipboardAppearance>
readScreenshotClipboardAppearance(const QMimeData* mime);
[[nodiscard]] std::optional<ScreenshotClipboardAppearance>
snapshotScreenshotClipboardAppearance(QClipboard* clipboard);

#endif
