#include "snow_shot/presentation/screenshotclipboardcontent.h"

#include "../../image/snowimageqtcodec.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMimeData>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFrame>
#include <QTextOption>
#include <QThread>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <cmath>
#include <limits>

#include <snow/image/format.h>

namespace {
constexpr int kMaximumRichTextWidth = 1024;
constexpr int kMaximumRichTextHeight = 32768;
constexpr qint64 kMaximumRichTextPixels = 64LL * 1024LL * 1024LL;
constexpr qint64 kMaximumClipboardImagePixels = 64LL * 1000LL * 1000LL;
constexpr qint64 kMaximumClipboardImageBytes = 256LL * 1024LL * 1024LL;
constexpr qint64 kMaximumEncodedImageBytes = 256LL * 1024LL * 1024LL;
constexpr qreal kFormattedTextPadding = 16.0;

class RestrictedTextDocument final : public QTextDocument {
  public:
    QVariant loadResource(int type, const QUrl& name) override {
        if (!name.isValid() || name.isEmpty()) {
            return {};
        }

        const QString scheme = name.scheme().toLower();
        // Data URLs are self-contained. Every other scheme is denied before
        // Qt can perform I/O. The clipboard snapshot does not inject any
        // external resources, so an empty QVariant is the safe cache miss.
        if (scheme == QStringLiteral("data")) {
            return QTextDocument::loadResource(type, name);
        }
        return {};
    }
};

bool cancellationRequested(const ScreenshotClipboardContentReader::CancellationCheck& cancelled) {
    return cancelled && cancelled();
}

std::shared_ptr<QTextDocument> makeRestrictedDocument() {
    return std::shared_ptr<QTextDocument>(
        new RestrictedTextDocument(), [](QTextDocument* document) {
            if (document == nullptr) {
                return;
            }
            if (document->thread() == QThread::currentThread()) {
                delete document;
            } else {
                QMetaObject::invokeMethod(document, &QObject::deleteLater, Qt::QueuedConnection);
            }
        });
}

struct EncodedImageFormat {
    const char* mimeType;
    snow::image::Format format;
};

constexpr EncodedImageFormat kEncodedImageFormats[] = {
    {"image/png", snow::image::Format::png},  {"image/jpeg", snow::image::Format::jpeg},
    {"image/jpg", snow::image::Format::jpeg}, {"image/webp", snow::image::Format::webp},
    {"image/jxl", snow::image::Format::jxl},  {"image/avif", snow::image::Format::avif},
};

struct FileImageFormat {
    const char* suffix;
    snow::image::Format format;
};

constexpr FileImageFormat kFileImageFormats[] = {
    {"png", snow::image::Format::png},   {"jpg", snow::image::Format::jpeg},
    {"jpeg", snow::image::Format::jpeg}, {"webp", snow::image::Format::webp},
    {"jxl", snow::image::Format::jxl},   {"avif", snow::image::Format::avif},
};

QImage normalizedImage(QImage image) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return {};
    }
    const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
    if (pixels <= 0 || pixels > kMaximumClipboardImagePixels || image.sizeInBytes() <= 0 ||
        image.sizeInBytes() > kMaximumClipboardImageBytes) {
        return {};
    }
    image.setDevicePixelRatio(1.0);
    return image;
}

bool isOpaqueSolidBackground(const QBrush& background) {
    return background.style() == Qt::SolidPattern && background.color().isValid() &&
           background.color().alpha() == 255;
}

void preserveHtmlCanvasBackground(QTextDocument* document) {
    if (document == nullptr || document->rootFrame() == nullptr) {
        return;
    }

    QTextFrameFormat rootFormat = document->rootFrame()->frameFormat();
    if (rootFormat.background().style() != Qt::NoBrush) {
        return;
    }

    std::optional<QColor> canvasColor;
    bool hasVisibleText = false;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        const QBrush blockBackground = block.blockFormat().background();
        for (QTextBlock::iterator iterator = block.begin(); !iterator.atEnd(); ++iterator) {
            const QTextFragment fragment = iterator.fragment();
            if (!fragment.isValid() || fragment.text().trimmed().isEmpty()) {
                continue;
            }
            hasVisibleText = true;

            QBrush background = fragment.charFormat().background();
            if (background.style() == Qt::NoBrush) {
                background = blockBackground;
            }
            if (!isOpaqueSolidBackground(background)) {
                return;
            }

            const QColor color = background.color();
            if (canvasColor.has_value() && color != *canvasColor) {
                return;
            }
            canvasColor = color;
        }
    }

    if (!hasVisibleText || !canvasColor.has_value()) {
        return;
    }
    rootFormat.setBackground(*canvasColor);
    document->rootFrame()->setFrameFormat(rootFormat);
}

std::optional<ScreenshotClipboardContent> imageContent(QImage image) {
    image = normalizedImage(std::move(image));
    if (image.isNull()) {
        return std::nullopt;
    }
    ScreenshotClipboardContent result;
    result.kind = ScreenshotClipboardContentKind::Image;
    result.image = std::move(image);
    return result;
}

std::optional<ScreenshotClipboardContent>
renderTextDocument(std::shared_ptr<QTextDocument> document, QString plainText,
                   qreal devicePixelRatio, const QColor& baseColor,
                   const ScreenshotClipboardContentReader::CancellationCheck& cancelled) {
    if (document == nullptr || !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0) {
        return std::nullopt;
    }
    if (cancellationRequested(cancelled)) {
        return std::nullopt;
    }

    QTextOption option = document->defaultTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document->setDefaultTextOption(option);
    document->setDocumentMargin(kFormattedTextPadding);
    document->setTextWidth(-1.0);

    qreal idealWidth = document->idealWidth();
    if (!std::isfinite(idealWidth) || idealWidth <= 0.0) {
        idealWidth = document->documentLayout()->documentSize().width();
    }
    if (!std::isfinite(idealWidth) || idealWidth <= 0.0) {
        idealWidth = 1.0;
    }
    const int width = std::clamp(qCeil(idealWidth), 1, kMaximumRichTextWidth);
    document->setTextWidth(width);

    const QSizeF documentSize = document->documentLayout()->documentSize();
    if (!documentSize.isValid() || documentSize.isEmpty() ||
        !std::isfinite(documentSize.height())) {
        return std::nullopt;
    }
    const int height = qCeil(documentSize.height());
    const qreal physicalWidthValue = std::ceil(width * devicePixelRatio);
    const qreal physicalHeightValue = std::ceil(height * devicePixelRatio);
    if (height <= 0 || height > kMaximumRichTextHeight ||
        physicalWidthValue > std::numeric_limits<int>::max() ||
        physicalHeightValue > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    const QSize physicalSize(static_cast<int>(physicalWidthValue),
                             static_cast<int>(physicalHeightValue));
    if (!physicalSize.isValid() || physicalSize.isEmpty() ||
        static_cast<qint64>(physicalSize.width()) * physicalSize.height() >
            kMaximumRichTextPixels) {
        return std::nullopt;
    }

    QImage image(physicalSize, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return std::nullopt;
    }
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(baseColor.isValid() ? baseColor : QColor(Qt::white));
    if (cancellationRequested(cancelled)) {
        return std::nullopt;
    }
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    document->drawContents(&painter, QRectF(0.0, 0.0, width, height));
    painter.end();

    ScreenshotClipboardContent result;
    result.kind = ScreenshotClipboardContentKind::FormattedText;
    result.image = std::move(image);
    result.formattedDocument = std::move(document);
    result.plainText = std::move(plainText);
    result.formattedTextDevicePixelRatio = devicePixelRatio;
    if (QCoreApplication* application = QCoreApplication::instance();
        result.formattedDocument != nullptr && application != nullptr &&
        result.formattedDocument->thread() != application->thread()) {
        result.formattedDocument->moveToThread(application->thread());
    }
    return result;
}

std::shared_ptr<QTextDocument> makeDocument(const QString& source, bool html, QString* plainText) {
    if (plainText == nullptr) {
        return {};
    }

    auto document = makeRestrictedDocument();
    if (html) {
        if (source.trimmed().isEmpty()) {
            return {};
        }
        document->setHtml(source);
        preserveHtmlCanvasBackground(document.get());
    } else {
        if (source.isEmpty()) {
            return {};
        }
        document->setPlainText(source);
    }

    *plainText = document->toPlainText();
    if (plainText->isEmpty() && document->characterCount() <= 1) {
        return {};
    }
    return document;
}

std::optional<ScreenshotClipboardContent>
readEncodedImage(const QList<ScreenshotClipboardEncodedImage>& images,
                 const ScreenshotClipboardContentReader::CancellationCheck& cancelled) {
    for (const ScreenshotClipboardEncodedImage& imageData : images) {
        if (cancellationRequested(cancelled)) {
            return std::nullopt;
        }
        const auto format =
            std::find_if(std::begin(kEncodedImageFormats), std::end(kEncodedImageFormats),
                         [&imageData](const EncodedImageFormat& candidate) {
                             return imageData.mimeType == QLatin1String(candidate.mimeType);
                         });
        if (format == std::end(kEncodedImageFormats) || imageData.bytes.isEmpty() ||
            imageData.bytes.size() > kMaximumEncodedImageBytes) {
            continue;
        }
        if (QImage image =
                snow_shot::image_codec::decode(imageData.bytes, format->format, format->mimeType);
            !image.isNull()) {
            return imageContent(std::move(image));
        }
    }
    return std::nullopt;
}

std::optional<ScreenshotClipboardContent>
readFileImage(const ScreenshotClipboardLocalImage& localImage,
              const ScreenshotClipboardContentReader::CancellationCheck& cancelled) {
    const auto format =
        std::find_if(std::begin(kFileImageFormats), std::end(kFileImageFormats),
                     [&localImage](const FileImageFormat& candidate) {
                         return localImage.suffix == QLatin1String(candidate.suffix);
                     });
    if (format == std::end(kFileImageFormats) || cancellationRequested(cancelled)) {
        return std::nullopt;
    }

    const QFileInfo before(localImage.absolutePath);
    if (!before.exists() || !before.isFile() || !before.isReadable() ||
        before.size() != localImage.size ||
        before.lastModified().toUTC() != localImage.lastModifiedUtc || before.size() <= 0 ||
        before.size() > kMaximumEncodedImageBytes) {
        return std::nullopt;
    }
    QFile file(before.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray encoded = file.read(kMaximumEncodedImageBytes + 1);
    file.close();
    const QFileInfo after(localImage.absolutePath);
    if (encoded.isEmpty() || encoded.size() > kMaximumEncodedImageBytes ||
        after.size() != localImage.size ||
        after.lastModified().toUTC() != localImage.lastModifiedUtc ||
        cancellationRequested(cancelled)) {
        return std::nullopt;
    }
    return imageContent(snow_shot::image_codec::decode(encoded, format->format,
                                                       QByteArray("image/") + format->suffix));
}
} // namespace

std::optional<ScreenshotClipboardContent>
ScreenshotClipboardContentReader::read(QClipboard* clipboard, qreal devicePixelRatio) {
    auto captured = snapshot(clipboard, devicePixelRatio);
    return captured.has_value() ? decode(std::move(*captured)) : std::nullopt;
}

std::optional<ScreenshotClipboardContent>
ScreenshotClipboardContentReader::readMimeData(const QMimeData* mimeData, qreal devicePixelRatio) {
    auto captured = snapshotMimeData(mimeData, devicePixelRatio,
                                     QGuiApplication::palette().color(QPalette::Base));
    return captured.has_value() ? decode(std::move(*captured)) : std::nullopt;
}

std::optional<ScreenshotClipboardContentSnapshot>
ScreenshotClipboardContentReader::snapshot(QClipboard* clipboard, qreal devicePixelRatio) {
    return clipboard == nullptr
               ? std::nullopt
               : snapshotMimeData(clipboard->mimeData(), devicePixelRatio,
                                  QGuiApplication::palette().color(QPalette::Base));
}

std::optional<ScreenshotClipboardContentSnapshot>
ScreenshotClipboardContentReader::snapshotMimeData(const QMimeData* mimeData,
                                                   qreal devicePixelRatio,
                                                   const QColor& baseColor) {
    if (mimeData == nullptr || !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0) {
        return std::nullopt;
    }

    ScreenshotClipboardContentSnapshot snapshot;
    snapshot.devicePixelRatio = devicePixelRatio;
    snapshot.baseColor = baseColor;

    for (const EncodedImageFormat& candidate : kEncodedImageFormats) {
        const QLatin1String mimeType(candidate.mimeType);
        if (!mimeData->hasFormat(mimeType)) {
            continue;
        }
        QByteArray bytes = mimeData->data(mimeType);
        if (!bytes.isEmpty() && bytes.size() <= kMaximumEncodedImageBytes) {
            snapshot.encodedImages.push_back(
                ScreenshotClipboardEncodedImage{std::move(bytes), mimeType});
        }
    }

    if (const QVariant imageValue = mimeData->imageData(); imageValue.isValid()) {
        if (imageValue.canConvert<QImage>()) {
            snapshot.detachedImage = imageValue.value<QImage>();
        } else if (imageValue.canConvert<QPixmap>()) {
            snapshot.detachedImage = imageValue.value<QPixmap>().toImage();
        }
    }

    for (const QUrl& url : mimeData->urls()) {
        if (!url.isLocalFile()) {
            continue;
        }
        const QFileInfo fileInfo(url.toLocalFile());
        const QString suffix = fileInfo.suffix().toLower();
        const bool supported =
            std::any_of(std::begin(kFileImageFormats), std::end(kFileImageFormats),
                        [&suffix](const FileImageFormat& candidate) {
                            return suffix == QLatin1String(candidate.suffix);
                        });
        if (supported) {
            snapshot.localImage =
                ScreenshotClipboardLocalImage{fileInfo.absoluteFilePath(), suffix, fileInfo.size(),
                                              fileInfo.lastModified().toUTC()};
            break;
        }
    }

    if (mimeData->hasHtml()) {
        snapshot.html = mimeData->html();
    }
    if (mimeData->hasText()) {
        snapshot.text = mimeData->text();
    }
    return snapshot.isValid()
               ? std::optional<ScreenshotClipboardContentSnapshot>(std::move(snapshot))
               : std::nullopt;
}

std::optional<ScreenshotClipboardContent>
ScreenshotClipboardContentReader::decode(ScreenshotClipboardContentSnapshot snapshot,
                                         CancellationCheck cancelled) {
    if (!snapshot.isValid() || !std::isfinite(snapshot.devicePixelRatio) ||
        snapshot.devicePixelRatio <= 0.0 || cancellationRequested(cancelled)) {
        return std::nullopt;
    }

    if (auto result = readEncodedImage(snapshot.encodedImages, cancelled); result.has_value()) {
        return result;
    }
    if (cancellationRequested(cancelled)) {
        return std::nullopt;
    }
    if (auto result = imageContent(std::move(snapshot.detachedImage)); result.has_value()) {
        return result;
    }
    if (snapshot.localImage.has_value()) {
        if (auto result = readFileImage(*snapshot.localImage, cancelled); result.has_value()) {
            return result;
        }
    }
    if (!snapshot.html.isEmpty()) {
        QString plainText;
        if (auto document = makeDocument(snapshot.html, true, &plainText); document != nullptr) {
            if (auto result =
                    renderTextDocument(std::move(document), std::move(plainText),
                                       snapshot.devicePixelRatio, snapshot.baseColor, cancelled);
                result.has_value()) {
                result->originalContent.html = std::move(snapshot.html);
                result->originalContent.text = std::move(snapshot.text);
                return result;
            }
        }
    }
    if (!snapshot.text.isEmpty()) {
        QString plainText;
        if (auto document = makeDocument(snapshot.text, false, &plainText); document != nullptr) {
            if (auto result =
                    renderTextDocument(std::move(document), std::move(plainText),
                                       snapshot.devicePixelRatio, snapshot.baseColor, cancelled);
                result.has_value()) {
                result->originalContent.html = std::move(snapshot.html);
                result->originalContent.text = std::move(snapshot.text);
                return result;
            }
        }
    }
    return std::nullopt;
}
