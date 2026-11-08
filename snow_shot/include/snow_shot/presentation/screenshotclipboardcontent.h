#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H

#include "snow_shot/storage/pinnedsourceidentity.h"
#include "snow_shot/storage/pinnedtextstyle.h"
#include "snow_shot/presentation/pinnedcolorswatch.h"
#include "snow_shot/image/screenshotanimationsource.h"
#include "snow_shot/presentation/screenshotclipboardplacement.h"
#include "snow_shot/presentation/screenshotclipboardappearance.h"
#include <QByteArray>
#include <QColor>
#include <QDateTime>
#include <QImage>
#include <QList>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QTransform>

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
    std::optional<snow_shot::storage::PinnedTextStyle> textStyle;

    snow_shot::storage::PinnedTextFormat textFormat =
        snow_shot::storage::PinnedTextFormat::PlainText;

    [[nodiscard]] bool isEmpty() const {
        return html.isEmpty() && text.isEmpty() && localFilePath.isEmpty();
    }
};

struct ScreenshotClipboardContent {
    snow_shot::storage::PinnedSourceIdentity sourceIdentity;
    ScreenshotClipboardContentKind kind = ScreenshotClipboardContentKind::Image;
    QImage image;
    std::shared_ptr<const ScreenshotAnimationSource> animationSource;
    QImage preparedDisplayImage;
    QTransform preparedImageTransform;
    std::shared_ptr<QTextDocument> formattedDocument;
    QString plainText;
    qreal formattedTextDevicePixelRatio = 1.0;
    ScreenshotClipboardOriginalContent originalContent;
    std::optional<ScreenshotClipboardPlacement> placement = std::nullopt;
    std::optional<ScreenshotClipboardAppearance> appearance = std::nullopt;

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
    snow_shot::storage::PinnedSourceIdentity sourceIdentity;
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
    std::optional<snow_shot::storage::PinnedTextStyle> textStyle;
    // Present only at a pin boundary. Generic image imports do not recognize colors.
    std::optional<PinnedColorSwatchStyle> colorSwatchStyle;
    QList<ScreenshotClipboardEncodedImage> encodedImages;
    std::optional<ScreenshotClipboardNativeDib> nativeDib;
    QImage detachedImage;
    std::optional<ScreenshotClipboardLocalImage> localImage;
    QString html;
    QString text;
    QColor baseColor;
    qreal devicePixelRatio = 1.0;
    std::optional<ScreenshotClipboardPlacement> placement = std::nullopt;
    std::optional<ScreenshotClipboardAppearance> appearance = std::nullopt;

    snow_shot::storage::PinnedTextFormat textFormat =
        snow_shot::storage::PinnedTextFormat::PlainText;

    [[nodiscard]] bool isValid() const {
        return !encodedImages.isEmpty() || (nativeDib.has_value() && nativeDib->isValid()) ||
               !detachedImage.isNull() || localImage.has_value() || !html.isEmpty() ||
               !text.isEmpty();
    }
};

class ScreenshotClipboardContentReader final {
  public:
    // GUI thread only. Capture once at a pin/import boundary, then pass by value to workers.
    [[nodiscard]] static snow_shot::storage::PinnedTextStyle capturePinnedTextStyle();
    using CancellationCheck = std::function<bool()>;
    // Optional admission for automation. The callback reserves total retained
    // and transient bytes before image copies, decoding, and rasterization.
    using AllocationCheck = std::function<bool(qint64)>;
    // Editors admit files against their raster budget; clipboard pins retain their own limits.
    enum class FilePurpose { Clipboard, ImageImport };

    [[nodiscard]] static QStringList supportedImageFileExtensions();
    [[nodiscard]] static QStringList supportedFileExtensions();
    [[nodiscard]] static QStringList localFilePaths(const QMimeData* mimeData);
    // Match decode() precedence without fetching image bytes: inline images take priority
    // over accompanying file URLs. File-only publications keep their batch import path.
    [[nodiscard]] static QStringList filePathsForImport(const QMimeData* mimeData);
    // Preserves clipboard order and duplicates; file validation belongs to the import worker.
    // Inline images take priority over file URLs, as in filePathsForImport().
    [[nodiscard]] static QStringList localImageFilePaths(const QMimeData* mimeData);
    // File metadata is independent of GUI state and may be captured on a worker.
    [[nodiscard]] static QList<ScreenshotClipboardLocalImage>
    snapshotLocalFiles(const QStringList& paths, CancellationCheck cancelled = {},
                       FilePurpose purpose = FilePurpose::Clipboard);
    // Validates the captured file revision at read time, then decodes only its initial image.
    // Uses ImageImport limits; allocate reserves encoded bytes and decoded working storage.
    // Animated containers retain composition/orientation/profile semantics without playback state.
    [[nodiscard]] static QImage decodeImageFile(const ScreenshotClipboardLocalImage& file,
                                                CancellationCheck cancelled = {},
                                                AllocationCheck allocate = {});

    // Snapshot methods must run on the GUI thread. Their return value owns all
    // MIME data needed by decode(), which is safe to run on an export worker.
    [[nodiscard]] static std::optional<ScreenshotClipboardContentSnapshot>
    snapshot(QClipboard* clipboard, qreal devicePixelRatio, AllocationCheck allocate = {});
    // Image-only import: never rasterize clipboard text or follow file/HTML fallbacks.
    [[nodiscard]] static std::optional<ScreenshotClipboardContentSnapshot>
    snapshotImage(QClipboard* clipboard, qreal devicePixelRatio, AllocationCheck allocate = {});
    [[nodiscard]] static std::optional<ScreenshotClipboardContentSnapshot>
    snapshotMimeData(const QMimeData* mimeData, qreal devicePixelRatio, const QColor& baseColor,
                     AllocationCheck allocate = {});
    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    decode(ScreenshotClipboardContentSnapshot snapshot, CancellationCheck cancelled = {},
           AllocationCheck allocate = {});

    // Prepare animation pixels on the import worker before the GUI commits a
    // replacement. The fingerprint normalizes translation to the source bounds.
    // Static content keeps its existing preparation path.
    [[nodiscard]] static bool prepareAnimationDisplay(ScreenshotClipboardContent& content,
                                                      const QTransform& transform,
                                                      CancellationCheck cancelled = {},
                                                      AllocationCheck allocate = {});

    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    readMimeData(const QMimeData* mimeData, qreal devicePixelRatio);

    // Rebuilds the formatted raster from the original clipboard text at an
    // explicit DPI. This is intentionally separate from decode(), whose DPI
    // is the current display DPI.
    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    renderOriginalText(const ScreenshotClipboardOriginalContent& original, qreal devicePixelRatio,
                       const QColor& baseColor = {}, AllocationCheck allocate = {},
                       std::optional<PinnedColorSwatchStyle> colorSwatchStyle = {});
    // Keeps the original text layout but rasterizes directly into a bounded thumbnail.
    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    renderOriginalTextThumbnail(const ScreenshotClipboardOriginalContent& original,
                                qreal devicePixelRatio, QSize targetSize, QSize* naturalSize,
                                CancellationCheck cancelled = {},
                                std::optional<PinnedColorSwatchStyle> colorSwatchStyle = {});

  private:
    [[nodiscard]] static std::optional<ScreenshotClipboardContent>
    renderOriginalTextImpl(const ScreenshotClipboardOriginalContent& original,
                           qreal devicePixelRatio, const QColor& baseColor,
                           AllocationCheck allocate,
                           std::optional<PinnedColorSwatchStyle> colorSwatchStyle, QSize targetSize,
                           QSize* naturalSize, CancellationCheck cancelled);
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTCLIPBOARDCONTENT_H
