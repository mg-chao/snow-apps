#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H

#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QList>
#include <QString>

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

    [[nodiscard]] bool isEmpty() const {
        return html.isEmpty() && text.isEmpty();
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

struct ScreenshotClipboardContentSnapshot final {
    QList<ScreenshotClipboardEncodedImage> encodedImages;
    QImage detachedImage;
    std::optional<ScreenshotClipboardLocalImage> localImage;
    QString html;
    QString text;
    QColor baseColor;
    qreal devicePixelRatio = 1.0;

    [[nodiscard]] bool isValid() const {
        return !encodedImages.isEmpty() || !detachedImage.isNull() || localImage.has_value() ||
               !html.isEmpty() || !text.isEmpty();
    }
};

class ScreenshotClipboardContentReader final {
  public:
    using CancellationCheck = std::function<bool()>;

    // Snapshot methods must run on the GUI thread. Their return value owns all
    // MIME data needed by decode(), which is safe to run on an export worker.
    [[nodiscard]] static std::optional<ScreenshotClipboardContentSnapshot>
    snapshot(QClipboard* clipboard, qreal devicePixelRatio);
    [[nodiscard]] static std::optional<ScreenshotClipboardContentSnapshot>
    snapshotMimeData(const QMimeData* mimeData, qreal devicePixelRatio, const QColor& baseColor);
    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    decode(ScreenshotClipboardContentSnapshot snapshot, CancellationCheck cancelled = {});

    // Compatibility helpers perform both phases synchronously.
    [[nodiscard]] static std::optional<ScreenshotClipboardContent> read(QClipboard* clipboard,
                                                                        qreal devicePixelRatio);
    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    readMimeData(const QMimeData* mimeData, qreal devicePixelRatio);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H
