#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QClipboard;
class QMimeData;
class QTextDocument;

enum class ScreenshotClipboardContentKind {
    Image,
    FormattedText,
};

struct ScreenshotClipboardOriginalContent final {
    QString html;
    QString text;
    // Set when the clipboard supplied a local image URL. A pin persists a
    // private copy and rewrites this path to that copy on restore.
    QString localFilePath;

    [[nodiscard]] bool isEmpty() const {
        return html.isEmpty() && text.isEmpty() && localFilePath.isEmpty();
    }
};

struct ScreenshotClipboardContent {
    ScreenshotClipboardContentKind kind = ScreenshotClipboardContentKind::Image;
    QImage image;
    std::shared_ptr<QTextDocument> formattedDocument;
    QString plainText;
    qreal formattedTextDevicePixelRatio = 1.0;
    ScreenshotClipboardOriginalContent originalContent;

    [[nodiscard]] bool isValid() const {
        return !image.isNull() && !image.size().isEmpty() &&
               (kind == ScreenshotClipboardContentKind::Image || formattedDocument != nullptr);
    }

    [[nodiscard]] bool isFormattedText() const {
        return kind == ScreenshotClipboardContentKind::FormattedText &&
               formattedDocument != nullptr;
    }
};

struct ScreenshotClipboardEncodedImage final {
    QByteArray bytes;
    QString mimeType;
};

struct ScreenshotClipboardLocalImage final {
    QString absolutePath;
    QString suffix;
    qint64 size = -1;
    QDateTime lastModifiedUtc;
};

enum class ScreenshotClipboardNativeDibFormat {
    Dib,
    DibV5,
};

struct ScreenshotClipboardNativeDib final {
    QByteArray bytes;
    QSize size;
    ScreenshotClipboardNativeDibFormat format = ScreenshotClipboardNativeDibFormat::Dib;

    [[nodiscard]] bool isValid() const {
        return !bytes.isEmpty() && size.isValid() && !size.isEmpty();
    }
};

struct ScreenshotClipboardContentSnapshot final {
    QList<ScreenshotClipboardEncodedImage> encodedImages;
    std::optional<ScreenshotClipboardNativeDib> nativeDib;
    QImage detachedImage;
    std::optional<ScreenshotClipboardLocalImage> localImage;
    QString html;
    QString text;
    QColor baseColor;
    qreal devicePixelRatio = 1.0;

    [[nodiscard]] bool isValid() const {
        return !encodedImages.isEmpty() || (nativeDib.has_value() && nativeDib->isValid()) ||
               !detachedImage.isNull() || localImage.has_value() || !html.isEmpty() ||
               !text.isEmpty();
    }
};

class ScreenshotClipboardContentReader final {
  public:
    using CancellationCheck = std::function<bool()>;
    // Optional admission for automation. The callback reserves total retained
    // and transient bytes before image copies, decoding, and rasterization.
    using AllocationCheck = std::function<bool(qint64)>;

    [[nodiscard]] static QStringList supportedFileExtensions();
    [[nodiscard]] static QStringList localFilePaths(const QMimeData* mimeData);
    // File metadata is independent of GUI state and may be captured on a worker.
    [[nodiscard]] static QList<ScreenshotClipboardLocalImage>
    snapshotLocalFiles(const QStringList& paths, CancellationCheck cancelled = {});

    // Snapshot methods must run on the GUI thread. Their return value owns all
    // MIME data needed by decode(), which is safe to run on an export worker.
    [[nodiscard]] static std::optional<ScreenshotClipboardContentSnapshot>
    snapshot(QClipboard* clipboard, qreal devicePixelRatio, AllocationCheck allocate = {});
    [[nodiscard]] static std::optional<ScreenshotClipboardContentSnapshot>
    snapshotMimeData(const QMimeData* mimeData, qreal devicePixelRatio, const QColor& baseColor,
                     AllocationCheck allocate = {});
    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    decode(ScreenshotClipboardContentSnapshot snapshot, CancellationCheck cancelled = {},
           AllocationCheck allocate = {});

    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    readMimeData(const QMimeData* mimeData, qreal devicePixelRatio);

    // Rebuilds the formatted raster from the original clipboard text at an
    // explicit DPI. This is intentionally separate from decode(), whose DPI
    // is the current display DPI.
    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    renderOriginalText(const ScreenshotClipboardOriginalContent& original, qreal devicePixelRatio,
                       const QColor& baseColor = {}, AllocationCheck allocate = {});
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H
