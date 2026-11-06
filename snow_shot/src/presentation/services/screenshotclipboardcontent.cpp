#include "snow_draw_engine_qt/snow_canvas_image.h"
#include "snow_shot/platform/applicationqos.h"
#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include "snow_shot/app/supportedpinfiles.h"
#include "snow_shot/image/imageimportlimits.h"
#include "snow_shot/presentation/screenshotdocumentformatting.h"
#include "screenshotclipboardcontentsnapshot.h"
#include "screenshotclipboarddibdecoder_p.h"

#include "../../image/snowimageqtcodec.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"
#ifdef Q_OS_MACOS
#include "../../platform/macos/imageclipboard.h"
#endif

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QClipboard>
#include <QColorSpace>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QTextCursor>
#include <QDir>
#include <QSet>
#include <QGuiApplication>
#include <QMimeData>
#include <QImageReader>
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
#include <QScopeGuard>
#include <QStringDecoder>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <thread>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

#include <snow/image/format.h>

namespace {
constexpr int kMaximumRichTextWidth = 1024;
constexpr int kMaximumRichTextHeight = 32768;
constexpr qint64 kMaximumRichTextPixels = 64LL * 1024LL * 1024LL;
constexpr qint64 kMaximumClipboardImagePixels = 64LL * 1000LL * 1000LL;
constexpr qint64 kMaximumClipboardImageBytes = 256LL * 1024LL * 1024LL;
constexpr qint64 kMaximumEncodedImageBytes = 256LL * 1024LL * 1024LL;
constexpr int kFormattedTextFontPixelSize = 16;
constexpr qreal kFormattedTextPadding = 16.0;

using AllocationCheck = ScreenshotClipboardContentReader::AllocationCheck;

using snow_shot::app::PinDocumentFormat;
using snow_shot::storage::PinnedTextFormat;

bool isTextFile(const QString& suffix) {
    return snow_shot::app::pinDocumentFileFormat(suffix).has_value();
}

qint64 snapshotBytes(const ScreenshotClipboardContentSnapshot& snapshot) {
    qint64 bytes =
        snapshot.detachedImage.sizeInBytes() + (snapshot.html.size() + snapshot.text.size()) * 2;
    for (const auto& encoded : snapshot.encodedImages)
        bytes += encoded.bytes.size();
    if (snapshot.nativeDib)
        bytes += snapshot.nativeDib->bytes.size();
    if (snapshot.placement)
        bytes += kScreenshotClipboardPlacementMaximumBytes;
    if (snapshot.textStyle)
        bytes += static_cast<qint64>(sizeof(snow_shot::storage::PinnedTextStyle)) +
                 snapshot.textStyle->fontFamily.size() * 2;
    if (snapshot.appearance) {
        bytes += static_cast<qint64>(sizeof(ScreenshotClipboardAppearance)) +
                 snapshot.appearance->filePath.size() * 2;
        if (snapshot.appearance->borderAppearance && snapshot.appearance->borderAppearance->region)
            bytes += snapshot.appearance->borderAppearance->region->retainedBytesEstimate();
    }
    return bytes;
}

bool admitImage(QSize size, qint64 retained, const AllocationCheck& allocate,
                qint64 maximumPixels = kMaximumClipboardImagePixels) {
    const auto pixels = static_cast<qint64>(size.width()) * size.height();
    return size.isValid() && !size.isEmpty() && pixels <= maximumPixels &&
           (!allocate || allocate(retained + pixels * 8));
}

class RestrictedTextDocument final : public QTextDocument {
  public:
    explicit RestrictedTextDocument(AllocationCheck allocate, qint64 retained)
        : m_allocate(std::move(allocate)), m_retained(retained) {}

    [[nodiscard]] bool allocationFailed() const {
        return m_failed;
    }
    [[nodiscard]] qint64 resourceBytes() const {
        return m_resourceBytes;
    }

    QVariant loadResource(int type, const QUrl& name) override {
        if (!name.isValid() || name.isEmpty()) {
            return {};
        }

        const QString scheme = name.scheme().toLower();
        // Data URLs are self-contained. Every other scheme is denied before
        // Qt can perform I/O. Denied images must return a typed null image below:
        // an invalid QVariant makes Qt's image handler open the URL directly.
        if (scheme == QStringLiteral("data")) {
            if (type == QTextDocument::ImageResource) {
                // Qt's default data-URL loader decodes before exposing dimensions.
                // Inspect the immutable encoded bytes before admitting its raster.
                if (!name.host().isEmpty())
                    return {};
                const auto data = name.toEncoded();
                const auto comma = data.indexOf(',');
                if (comma < 0 || (m_allocate && data.size() > 2 * 1024 * 1024)) {
                    m_failed = static_cast<bool>(m_allocate);
                    return {};
                }
                // Match Qt's data-URL rules: metadata and payload can both be
                // percent encoded, and the trailing base64 marker ignores case.
                const auto metadata = QByteArray::fromPercentEncoding(data.left(comma)).trimmed();
                auto encoded = QByteArray::fromPercentEncoding(data.mid(comma + 1));
                if (metadata.toLower().endsWith(";base64")) {
                    auto decoded = QByteArray::fromBase64Encoding(std::move(encoded));
                    if (!decoded)
                        return {};
                    encoded = std::move(decoded.decoded);
                }
                QBuffer buffer(&encoded);
                buffer.open(QIODevice::ReadOnly);
                QImageReader reader(&buffer);
                const auto size = reader.size();
                if (m_allocate &&
                    !admitImage(size, m_retained + m_resourceBytes + encoded.size(), m_allocate)) {
                    m_failed = true;
                    return {};
                }
                auto image = snow_shot::image_codec::readManagedImage(reader);
                if (image.isNull())
                    return m_allocate ? QVariant::fromValue(image)
                                      : QTextDocument::loadResource(type, name);
                // resource() does not cache a virtual loadResource() result.
                // Share this owner across layout, painting and later consumers.
                addResource(type, name, image);
                m_resourceBytes += image.sizeInBytes();
                return image;
            }
            return QTextDocument::loadResource(type, name);
        }
        return type == QTextDocument::ImageResource ? QVariant::fromValue(QImage()) : QVariant();
    }

  private:
    AllocationCheck m_allocate;
    qint64 m_retained = 0;
    qint64 m_resourceBytes = 0;
    bool m_failed = false;
};

bool cancellationRequested(const ScreenshotClipboardContentReader::CancellationCheck& cancelled) {
    return cancelled && cancelled();
}

std::shared_ptr<QTextDocument> makeRestrictedDocument(AllocationCheck allocate = {},
                                                      qint64 retained = 0) {
    return std::shared_ptr<QTextDocument>(
        new RestrictedTextDocument(std::move(allocate), retained), [](QTextDocument* document) {
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
    {"image/png", snow::image::Format::png},   {"image/jpeg", snow::image::Format::jpeg},
    {"image/jpg", snow::image::Format::jpeg},  {"image/webp", snow::image::Format::webp},
    {"image/jxl", snow::image::Format::jxl},   {"image/avif", snow::image::Format::avif},
    {"image/gif", snow::image::Format::gif},   {"image/apng", snow::image::Format::png},
    {"image/tiff", snow::image::Format::tiff},
};

using snow_shot::image::ImageFileFormat;
using snow_shot::image::kImageFileFormats;

QImage normalizedImage(QImage image, bool imageImport = false) {
    if (image.isNull() || image.width() <= 0 || image.height() <= 0) {
        return {};
    }
    const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
    const auto maximumPixels =
        imageImport ? snow_shot::image::maximumImageImportPixels : kMaximumClipboardImagePixels;
    const auto maximumBytes =
        imageImport ? snow_shot::image::maximumImageImportBytes : kMaximumClipboardImageBytes;
    if (pixels <= 0 || pixels > maximumPixels || image.sizeInBytes() <= 0 ||
        image.sizeInBytes() > maximumBytes) {
        return {};
    }
    // The immutable clipboard snapshot already retains its QImage owner. Share
    // those pixels instead of allocating a second raster while the provider or
    // snapshot still owns the first. Decoded and rendered images retain their
    // managed storage; foreign Qt owners keep their native allocation policy.
#if !defined(Q_OS_MACOS)
    if (image.devicePixelRatio() != 1.0) {
        if (!snowCanvasDetachImage(image))
            return {};
        image.setDevicePixelRatio(1.0);
    }
#endif
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

std::optional<ScreenshotClipboardContent> imageContent(QImage image, bool imageImport = false) {
    image = normalizedImage(std::move(image), imageImport);
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
                   const ScreenshotClipboardContentReader::CancellationCheck& cancelled,
                   const AllocationCheck& allocate = {}, qint64 retained = 0,
                   const std::optional<snow_shot::storage::PinnedTextStyle>& style = {}) {
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
    const qreal maximumWidth = style ? style->maximumWidth : kMaximumRichTextWidth;
    document->setTextWidth(std::ceil(std::clamp(idealWidth, 1.0, maximumWidth)));

    const QSizeF documentSize = document->documentLayout()->documentSize();
    if (!documentSize.isValid() || documentSize.isEmpty() || !std::isfinite(documentSize.width()) ||
        !std::isfinite(documentSize.height())) {
        return std::nullopt;
    }
    // The setting is a wrapping preference. Fixed-width tables, images and nonbreaking
    // lines can exceed it, so rasterize the full layout rather than clipping to it.
    const qreal width = std::ceil(documentSize.width());
    const qreal height = std::ceil(documentSize.height());
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

    const auto* restricted = static_cast<const RestrictedTextDocument*>(document.get());
    if (restricted->allocationFailed() ||
        !admitImage(physicalSize, retained + restricted->resourceBytes(), allocate))
        return std::nullopt;
    QImage image = snowCanvasAllocateImage(physicalSize, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return std::nullopt;
    }
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(style ? style->backgroundColor
                     : (baseColor.isValid() ? baseColor : QColor(Qt::white)));
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
    result.originalContent.textStyle = style;
    if (QCoreApplication* application = QCoreApplication::instance();
        result.formattedDocument != nullptr && application != nullptr &&
        result.formattedDocument->thread() != application->thread()) {
        result.formattedDocument->moveToThread(application->thread());
    }
    return result;
}

std::shared_ptr<QTextDocument>
makeDocument(const QString& source, bool html, QString* plainText,
             const AllocationCheck& allocate = {}, qint64 retained = 0,
             const std::optional<snow_shot::storage::PinnedTextStyle>& style = {},
             PinnedTextFormat textFormat = PinnedTextFormat::PlainText) {
    if (plainText == nullptr) {
        return {};
    }

    if (allocate && (source.size() > 1024 * 1024 || !allocate(retained + source.size() * 32)))
        return {};
    auto document = makeRestrictedDocument(allocate, retained + source.size() * 32);
    // Pinned content needs a readable default independent of the small application UI font.
    // Set it before HTML parsing so explicit source sizes still override inherited text.
    QFont font = document->defaultFont();
    if (style)
        font.setFamily(style->fontFamily);
    font.setPixelSize(style ? style->fontSize : kFormattedTextFontPixelSize);
    document->setDefaultFont(font);
    if (html || textFormat == PinnedTextFormat::Markdown) {
        if (source.trimmed().isEmpty())
            return {};
        const QColor foreground = style ? style->textColor : QColor(Qt::black);
        const QColor background = style ? style->backgroundColor : QColor(Qt::white);
        const auto blend = [foreground, background](qreal amount) {
            return QColor(qRound(foreground.red() * amount + background.red() * (1 - amount)),
                          qRound(foreground.green() * amount + background.green() * (1 - amount)),
                          qRound(foreground.blue() * amount + background.blue() * (1 - amount)),
                          background.alpha());
        };
        const snow_shot::presentation::ScreenshotDocumentStyle documentStyle{
            foreground,
            background.lightness() < 128 ? QColor(105, 177, 255) : QColor(22, 119, 255),
            QFontDatabase::systemFont(QFontDatabase::FixedFont).family(),
            blend(0.08),
            blend(0.7),
            blend(0.2)};
        // Existing HTML pins persist their original layout geometry. Keep HTML's
        // authored/default Qt styling; Markdown needs the conversion preview defaults.
        if (!html)
            snow_shot::presentation::setScreenshotDocumentStyle(*document, documentStyle, true);
        snow_shot::presentation::setScreenshotDocumentSource(*document, source, !html,
                                                             documentStyle, true);
        if (html)
            preserveHtmlCanvasBackground(document.get());
    } else {
        if (source.isEmpty())
            return {};
        document->setPlainText(source);
    }

    if (style) {
        // Keep explicit source brushes. Materialize only inherited colors so every
        // later document consumer renders the same foreground as the pinned raster.
        QList<QPair<int, int>> inheritedRanges;
        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            for (auto iterator = block.begin(); !iterator.atEnd(); ++iterator) {
                const auto fragment = iterator.fragment();
                if (fragment.isValid() &&
                    !fragment.charFormat().hasProperty(QTextFormat::ForegroundBrush))
                    inheritedRanges.append({fragment.position(), fragment.length()});
            }
        }
        document->setUndoRedoEnabled(false);
        QTextCursor cursor(document.get());
        QTextCharFormat format;
        format.setForeground(style->textColor);
        for (const auto& [position, length] : inheritedRanges) {
            cursor.setPosition(position);
            cursor.setPosition(position + length, QTextCursor::KeepAnchor);
            cursor.mergeCharFormat(format);
        }
    }
    *plainText = document->toPlainText();
    if (plainText->isEmpty() && document->characterCount() <= 1) {
        return {};
    }
    return document;
}

std::optional<ScreenshotClipboardContent>
readEncodedImage(const QList<ScreenshotClipboardEncodedImage>& images,
                 const ScreenshotClipboardContentReader::CancellationCheck& cancelled,
                 const AllocationCheck& allocate, qint64 retained) {
    // Clipboard providers commonly offer a static PNG thumbnail alongside the
    // original animated image. Inspect all encoded alternatives before choosing
    // the ordinary static fallback, preserving the complete source when present.
    QSet<qsizetype> invalidAnimations;
    QSet<qsizetype> singleFrameAnimations;
    for (qsizetype index = 0; index < images.size(); ++index) {
        const auto& imageData = images.at(index);
        if (cancellationRequested(cancelled))
            return std::nullopt;
        const auto format =
            std::find_if(std::begin(kEncodedImageFormats), std::end(kEncodedImageFormats),
                         [&imageData](const EncodedImageFormat& candidate) {
                             return imageData.mimeType == QLatin1String(candidate.mimeType);
                         });
        if (format == std::end(kEncodedImageFormats) ||
            (format->format != snow::image::Format::gif &&
             format->format != snow::image::Format::png &&
             format->format != snow::image::Format::webp) ||
            imageData.bytes.isEmpty() || imageData.bytes.size() > kMaximumEncodedImageBytes)
            continue;
        QString error;
        const AllocationCheck animationAdmission =
            allocate ? AllocationCheck([&](qint64 bytes) { return allocate(retained + bytes); })
                     : AllocationCheck{};
        auto animation = snow_shot::image_codec::decodeAnimationSource(
            imageData.bytes, &error, cancelled, animationAdmission);
        if (animation && animation->isAnimated()) {
            auto content = imageContent(animation->firstFrame);
            if (content) {
                content->preparedDisplayImage = animation->firstDisplayFrame;
                content->animationSource = std::move(animation);
            }
            return content;
        }
        if (animation)
            singleFrameAnimations.insert(index);
        if (!error.isEmpty())
            invalidAnimations.insert(index);
    }
    for (qsizetype index = 0; index < images.size(); ++index) {
        const auto& imageData = images.at(index);
        if (invalidAnimations.contains(index))
            continue;
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
        if (allocate &&
            !admitImage(snow_shot::image_codec::inspectSize(imageData.bytes, format->format),
                        retained, allocate))
            return std::nullopt;
        if (singleFrameAnimations.contains(index)) {
            const AllocationCheck animationAdmission =
                allocate ? AllocationCheck([&](qint64 bytes) { return allocate(retained + bytes); })
                         : AllocationCheck{};
            const auto source = snow_shot::image_codec::decodeAnimationSource(
                imageData.bytes, nullptr, cancelled, animationAdmission);
            if (source)
                return imageContent(source->firstFrame);
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
readFileContent(const ScreenshotClipboardLocalImage& localImage, qreal devicePixelRatio,
                const QColor& baseColor,
                const ScreenshotClipboardContentReader::CancellationCheck& cancelled,
                const AllocationCheck& allocate, qint64 retained, bool firstFrameOnly = false,
                const std::optional<snow_shot::storage::PinnedTextStyle>& style = {},
                const std::optional<PinnedColorSwatchStyle>& colorSwatchStyle = {}) {
    const auto format =
        std::find_if(std::begin(kImageFileFormats), std::end(kImageFileFormats),
                     [&localImage](const ImageFileFormat& candidate) {
                         return localImage.suffix == QLatin1String(candidate.suffix);
                     });
    const bool textFile = isTextFile(localImage.suffix);
    if ((firstFrameOnly && textFile) || (!textFile && format == std::end(kImageFileFormats)) ||
        cancellationRequested(cancelled)) {
        return std::nullopt;
    }

    const QFileInfo before(localImage.absolutePath);
    const auto maximumEncodedBytes =
        firstFrameOnly ? snow_shot::image::maximumImageImportBytes : kMaximumEncodedImageBytes;
    if (!before.exists() || !before.isFile() || !before.isReadable() ||
        before.size() != localImage.size ||
        before.lastModified().toUTC() != localImage.lastModifiedUtc || before.size() <= 0 ||
        before.size() > (textFile ? 1024 * 1024 : maximumEncodedBytes)) {
        return std::nullopt;
    }
    if (allocate && !allocate(retained + before.size() + 1))
        return std::nullopt;
    QFile file(before.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    const QByteArray encoded = file.read(before.size() + 1);
    file.close();
    const QFileInfo after(localImage.absolutePath);
    if (encoded.isEmpty() || encoded.size() != before.size() || after.size() != localImage.size ||
        after.lastModified().toUTC() != localImage.lastModifiedUtc ||
        cancellationRequested(cancelled)) {
        return std::nullopt;
    }
    if (textFile) {
        if (allocate && !allocate(retained + encoded.size() * 3))
            return std::nullopt;
        const auto documentFormat = *snow_shot::app::pinDocumentFileFormat(localImage.suffix);
        const bool html = documentFormat == PinDocumentFormat::Html;
        const auto encoding = html ? QStringDecoder::encodingForHtml(encoded)
                                   : QStringDecoder::encodingForData(encoded);
        QStringDecoder decoder(encoding.value_or(QStringDecoder::Utf8));
        ScreenshotClipboardContentSnapshot textSnapshot;
        textSnapshot.textFormat = documentFormat == PinDocumentFormat::Markdown
                                      ? PinnedTextFormat::Markdown
                                      : PinnedTextFormat::PlainText;
        (html ? textSnapshot.html : textSnapshot.text) = decoder.decode(encoded);
        if (decoder.hasError())
            return std::nullopt;
        textSnapshot.devicePixelRatio = devicePixelRatio;
        textSnapshot.baseColor = baseColor;
        textSnapshot.textStyle = style;
        textSnapshot.colorSwatchStyle = colorSwatchStyle;
        const AllocationCheck textAllocate = allocate ? AllocationCheck([&](qint64 bytes) {
            return allocate(retained + encoded.size() + bytes);
        })
                                                      : AllocationCheck{};
        return ScreenshotClipboardContentReader::decode(std::move(textSnapshot), cancelled,
                                                        textAllocate);
    }
    // The suffix routes image/document imports, but only the bytes determine the image codec.
    // Reuse the decoder's existing probe rather than trying the suffix codec and decoding again.
    constexpr auto codecFormat = snow::image::Format::unknown;
    if ((allocate || firstFrameOnly) &&
        !admitImage(snow_shot::image_codec::inspectCanvasSize(encoded, codecFormat),
                    retained + encoded.size(), allocate,
                    firstFrameOnly ? snow_shot::image::maximumImageImportPixels
                                   : kMaximumClipboardImagePixels))
        return std::nullopt;
    QString animationError;
    const AllocationCheck animationAdmission =
        allocate ? AllocationCheck([&](qint64 bytes) { return allocate(retained + bytes); })
                 : AllocationCheck{};
    if (firstFrameOnly) {
        auto image = snow_shot::image_codec::decodeAnimationFirstFrame(
            encoded, &animationError, cancelled, animationAdmission);
        if (!image.isNull())
            return imageContent(std::move(image), true);
        if (!animationError.isEmpty() || cancellationRequested(cancelled))
            return std::nullopt;
        auto result =
            imageContent(snow_shot::image_codec::decodeFirstFrame(encoded, codecFormat), true);
        return cancellationRequested(cancelled) ? std::nullopt : std::move(result);
    }
    auto animation = snow_shot::image_codec::decodeAnimationSource(encoded, &animationError,
                                                                   cancelled, animationAdmission);
    if (animation) {
        auto content = imageContent(animation->firstFrame);
        if (content && animation->isAnimated()) {
            content->preparedDisplayImage = animation->firstDisplayFrame;
            content->animationSource = std::move(animation);
        }
        return content;
    }
    if (!animationError.isEmpty() || cancellationRequested(cancelled))
        return std::nullopt;
    return imageContent(snow_shot::image_codec::decode(encoded, codecFormat, nullptr));
}

} // namespace

std::optional<ScreenshotClipboardContentSnapshot>
snow_shot::presentation::detail::snapshotClipboardMimeData(
    const QMimeData* mimeData, qreal devicePixelRatio, const QColor& baseColor,
    bool includeDetachedImage, const AllocationCheck& allocate,
    std::optional<ScreenshotClipboardContentSnapshot> nativeSnapshot, bool imagesOnly) {
    ensureScreenshotClipboardAppearanceMimeSupport();
    if (!std::isfinite(devicePixelRatio) || devicePixelRatio <= 0.0) {
        return std::nullopt;
    }

    const bool nativeCaptured = nativeSnapshot.has_value();
    ScreenshotClipboardContentSnapshot snapshot =
        nativeCaptured ? std::move(*nativeSnapshot) : ScreenshotClipboardContentSnapshot{};
    snapshot.devicePixelRatio = devicePixelRatio;
    snapshot.baseColor = baseColor;
    if (!mimeData)
        return snapshot.isValid() ? std::optional(std::move(snapshot)) : std::nullopt;
    if (!imagesOnly && !nativeCaptured &&
        mimeData->hasFormat(screenshotClipboardPlacementNativeMimeType())) {
        if (allocate && !allocate(kScreenshotClipboardPlacementMaximumBytes))
            return std::nullopt;
        snapshot.placement = readScreenshotClipboardPlacement(mimeData);
    }

    if (!imagesOnly && !nativeCaptured &&
        mimeData->hasFormat(screenshotClipboardAppearanceNativeMimeType())) {
        snapshot.appearance = readScreenshotClipboardAppearance(mimeData);
        if (allocate && !allocate(snapshotBytes(snapshot)))
            return std::nullopt;
    }

    for (const EncodedImageFormat& candidate : kEncodedImageFormats) {
        const QLatin1String mimeType(candidate.mimeType);
        // Reuse the exact encoded formats copied under the native clipboard lock.
        // A DIB is only a fallback and must not suppress other encoded images.
        if (std::any_of(snapshot.encodedImages.cbegin(), snapshot.encodedImages.cend(),
                        [&mimeType](const ScreenshotClipboardEncodedImage& encoded) {
                            return encoded.mimeType == mimeType;
                        }))
            continue;
        if (!mimeData->hasFormat(mimeType)) {
            continue;
        }
        // QMimeData may invoke an external lazy provider and offers no byte-size
        // query. Reserve its supported upper bound before requesting the value.
        if (allocate && !allocate(snapshotBytes(snapshot) + kMaximumEncodedImageBytes))
            return std::nullopt;
        QByteArray bytes = mimeData->data(mimeType);
        if (allocate && bytes.size() > kMaximumEncodedImageBytes)
            return std::nullopt;
        if (!bytes.isEmpty() && bytes.size() <= kMaximumEncodedImageBytes) {
            snapshot.encodedImages.push_back(
                ScreenshotClipboardEncodedImage{std::move(bytes), mimeType});
        }
    }

    if (includeDetachedImage) {
        if (allocate && mimeData->hasImage() &&
            !allocate(snapshotBytes(snapshot) + kMaximumClipboardImageBytes))
            return std::nullopt;
        if (const QVariant imageValue = mimeData->imageData(); imageValue.isValid()) {
            if (imageValue.canConvert<QImage>()) {
                snapshot.detachedImage = imageValue.value<QImage>();
            } else if (imageValue.canConvert<QPixmap>()) {
                snapshot.detachedImage = imageValue.value<QPixmap>().toImage();
            }
        }
    }

    if (imagesOnly)
        return snapshot.isValid() ? std::optional(std::move(snapshot)) : std::nullopt;

    const auto files = ScreenshotClipboardContentReader::snapshotLocalFiles(
        ScreenshotClipboardContentReader::localFilePaths(mimeData));
    if (!files.isEmpty()) {
        snapshot.localImage = files.constFirst();
    }

    if (mimeData->hasHtml()) {
        if (allocate && !allocate(snapshotBytes(snapshot) + 2 * 1024 * 1024))
            return std::nullopt;
        snapshot.html = mimeData->html();
        if (allocate && snapshot.html.size() > 1024 * 1024)
            return std::nullopt;
    }
    if (mimeData->hasText()) {
        if (allocate && !allocate(snapshotBytes(snapshot) + 2 * 1024 * 1024))
            return std::nullopt;
        snapshot.text = mimeData->text();
        if (allocate && snapshot.text.size() > 1024 * 1024)
            return std::nullopt;
    }
    return snapshot.isValid()
               ? std::optional<ScreenshotClipboardContentSnapshot>(std::move(snapshot))
               : std::nullopt;
}

namespace {
#if defined(Q_OS_WIN) || defined(_WIN32)
QByteArray captureNativeEncodedImage(const wchar_t* name, const AllocationCheck& allocate,
                                     qint64 retained) {
    const UINT format = RegisterClipboardFormatW(name);
    if (format == 0 || !IsClipboardFormatAvailable(format))
        return {};
    const auto handle = static_cast<HGLOBAL>(GetClipboardData(format));
    const SIZE_T size = handle == nullptr ? 0 : GlobalSize(handle);
    const void* memory = handle == nullptr ? nullptr : GlobalLock(handle);
    QByteArray bytes;
    if (memory != nullptr && size > 0 && size <= static_cast<SIZE_T>(kMaximumEncodedImageBytes) &&
        (!allocate || allocate(retained + static_cast<qint64>(size)))) {
        bytes = QByteArray(static_cast<const char*>(memory), static_cast<qsizetype>(size));
    }
    if (memory != nullptr)
        GlobalUnlock(handle);
    return bytes;
}

qint64 nativeDibPixelOffset(const QByteArray& bytes, const BITMAPINFOHEADER& header,
                            qint64 pixelBytes) {
    const qint64 headerSize = header.biSize;
    const qint64 colorTableSize =
        static_cast<qint64>(header.biClrUsed) * static_cast<qint64>(sizeof(RGBQUAD));
    const qint64 tableEnd = headerSize + colorTableSize;
    if (header.biCompression != BI_BITFIELDS)
        return tableEnd;
    constexpr qint64 masksSize = 3 * static_cast<qint64>(sizeof(DWORD));
    if (headerSize < static_cast<qint64>(sizeof(BITMAPV4HEADER))) {
        return tableEnd + masksSize;
    }
    qint64 payloadEnd = bytes.size();
    if (headerSize >= static_cast<qint64>(sizeof(BITMAPV5HEADER))) {
        BITMAPV5HEADER v5{};
        std::memcpy(&v5, bytes.constData(), sizeof(v5));
        if (v5.bV5ProfileSize != 0) {
            const qint64 profileStart = v5.bV5ProfileData;
            if (profileStart < tableEnd ||
                profileStart + static_cast<qint64>(v5.bV5ProfileSize) > bytes.size()) {
                return -1;
            }
            payloadEnd = profileStart;
        }
    }
    if (tableEnd > payloadEnd || pixelBytes > payloadEnd - tableEnd)
        return -1;
    // Qt/WIC can repeat masks after the embedded V4/V5 masks. Accept this
    // undeclared table only when it exactly accounts for the extra payload;
    // matching pixel values alone cannot identify a table.
    if (colorTableSize == 0 && payloadEnd - headerSize - pixelBytes == masksSize) {
        DWORD headerMasks[3]{};
        DWORD trailingMasks[3]{};
        std::memcpy(headerMasks, bytes.constData() + sizeof(BITMAPINFOHEADER), sizeof(headerMasks));
        std::memcpy(trailingMasks, bytes.constData() + headerSize, sizeof(trailingMasks));
        // WIC emits BGR order here; other producers repeat the header's RGB order.
        if ((trailingMasks[0] == headerMasks[0] && trailingMasks[1] == headerMasks[1] &&
             trailingMasks[2] == headerMasks[2]) ||
            (trailingMasks[0] == headerMasks[2] && trailingMasks[1] == headerMasks[1] &&
             trailingMasks[2] == headerMasks[0])) {
            return headerSize + masksSize;
        }
    }
    return tableEnd;
}

std::optional<ScreenshotClipboardNativeDib> captureNativeDib(const AllocationCheck& allocate,
                                                             qint64 retained) {
    const UINT format = IsClipboardFormatAvailable(CF_DIBV5)
                            ? CF_DIBV5
                            : (IsClipboardFormatAvailable(CF_DIB) ? CF_DIB : 0);
    if (format == 0) {
        return std::nullopt;
    }
    HGLOBAL handle = static_cast<HGLOBAL>(GetClipboardData(format));
    const SIZE_T size = handle != nullptr ? GlobalSize(handle) : 0;
    void* locked = handle != nullptr ? GlobalLock(handle) : nullptr;
    if (locked == nullptr || size < sizeof(BITMAPINFOHEADER) ||
        size > static_cast<SIZE_T>(std::numeric_limits<int>::max())) {
        if (locked != nullptr)
            GlobalUnlock(handle);
        return std::nullopt;
    }
    BITMAPINFOHEADER rawHeader{};
    std::memcpy(&rawHeader, locked, sizeof(rawHeader));
    const auto rawHeight = rawHeader.biHeight < 0 ? -static_cast<qint64>(rawHeader.biHeight)
                                                  : static_cast<qint64>(rawHeader.biHeight);
    if (rawHeader.biWidth <= 0 || rawHeight <= 0 ||
        static_cast<qint64>(rawHeader.biWidth) * rawHeight > kMaximumClipboardImagePixels ||
        (allocate && !allocate(retained + static_cast<qint64>(size) +
                               static_cast<qint64>(rawHeader.biWidth) * rawHeight * 4))) {
        GlobalUnlock(handle);
        return std::nullopt;
    }
    QByteArray bytes(static_cast<const char*>(locked), static_cast<int>(size));
    GlobalUnlock(handle);

    const auto& header = rawHeader;
    if (header.biSize < sizeof(BITMAPINFOHEADER) ||
        header.biSize > static_cast<DWORD>(bytes.size()) || header.biWidth <= 0 ||
        header.biHeight == 0 || header.biPlanes != 1 || header.biBitCount != 32 ||
        (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS)) {
        return std::nullopt;
    }
    const qint64 height = header.biHeight < 0 ? -static_cast<qint64>(header.biHeight)
                                              : static_cast<qint64>(header.biHeight);
    const qint64 width = header.biWidth;
    const qint64 stride = ((width * 4) + 3) & ~qint64(3);
    if (width * height > kMaximumClipboardImagePixels)
        return std::nullopt;
    const qint64 pixelOffset = nativeDibPixelOffset(bytes, header, stride * height);
    if (width <= 0 || height <= 0 || width * height > kMaximumClipboardImagePixels || stride <= 0 ||
        pixelOffset < 0 || pixelOffset + stride * height > bytes.size()) {
        return std::nullopt;
    }
    SNOW_SHOT_PIN_PERF_COUNTER("clipboard.native_dib_bytes", bytes.size());
    return ScreenshotClipboardNativeDib{
        std::move(bytes), QSize(static_cast<int>(width), static_cast<int>(height)),
        format == CF_DIBV5 ? ScreenshotClipboardNativeDibFormat::DibV5
                           : ScreenshotClipboardNativeDibFormat::Dib};
}

struct DibChannelMetadata {
    std::uint32_t mask = 0;
    std::uint32_t compactMask = 0;
    unsigned shift = 0;
    unsigned bitCount = 0;
    bool contiguous = true;
};

DibChannelMetadata makeDibChannelMetadata(std::uint32_t mask) {
    DibChannelMetadata metadata;
    metadata.mask = mask;
    if (mask == 0)
        return metadata;
    while (metadata.shift < 32 && ((mask >> metadata.shift) & 1u) == 0u)
        ++metadata.shift;
    unsigned highestBit = 31;
    while (highestBit > metadata.shift && ((mask >> highestBit) & 1u) == 0u)
        --highestBit;
    unsigned compactBit = 0;
    bool gap = false;
    for (unsigned bit = metadata.shift; bit <= highestBit; ++bit) {
        if ((mask & (1u << bit)) != 0u) {
            metadata.compactMask |= 1u << compactBit++;
        } else if (compactBit != 0) {
            gap = true;
        }
    }
    metadata.bitCount = compactBit;
    metadata.contiguous = !gap;
    return metadata;
}

unsigned char dibChannel(std::uint32_t value, const DibChannelMetadata& metadata,
                         unsigned char fallback) {
    if (metadata.mask == 0 || metadata.bitCount == 0)
        return fallback;
    std::uint32_t raw = 0;
    if (metadata.contiguous) {
        raw = (value & metadata.mask) >> metadata.shift;
    } else {
        unsigned compactBit = 0;
        for (unsigned bit = metadata.shift; bit < 32; ++bit) {
            if ((metadata.mask & (1u << bit)) != 0u) {
                raw |= ((value >> bit) & 1u) << compactBit++;
            }
        }
    }
    const std::uint32_t maxValue = metadata.compactMask;
    return static_cast<unsigned char>((static_cast<std::uint64_t>(raw) * 255u + maxValue / 2u) /
                                      maxValue);
}

QImage decodeNativeDib(const ScreenshotClipboardNativeDib& native) {
    if (!native.isValid() || native.bytes.size() < static_cast<int>(sizeof(BITMAPINFOHEADER)))
        return {};
    BITMAPINFOHEADER header{};
    std::memcpy(&header, native.bytes.constData(), sizeof(header));
    if (header.biSize < sizeof(BITMAPINFOHEADER) ||
        header.biSize > static_cast<DWORD>(native.bytes.size()) || header.biWidth <= 0 ||
        header.biHeight == 0 || header.biPlanes != 1 || header.biBitCount != 32 ||
        (header.biCompression != BI_RGB && header.biCompression != BI_BITFIELDS)) {
        return {};
    }
    const qint64 width = header.biWidth;
    const qint64 height = header.biHeight < 0 ? -static_cast<qint64>(header.biHeight)
                                              : static_cast<qint64>(header.biHeight);
    const bool topDown = header.biHeight < 0;
    if (height <= 0 || width > std::numeric_limits<qint64>::max() / 4 ||
        width * height > kMaximumClipboardImagePixels || width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max()) {
        return {};
    }
    const qint64 rowBytes = width * 4;
    if (rowBytes > std::numeric_limits<qint64>::max() - 3)
        return {};
    const qint64 stride = (rowBytes + 3) & ~qint64(3);
    const qint64 pixelOffset = nativeDibPixelOffset(native.bytes, header, stride * height);
    if (pixelOffset < 0 || pixelOffset > native.bytes.size() ||
        height > (native.bytes.size() - pixelOffset) / stride ||
        native.size != QSize(static_cast<int>(width), static_cast<int>(height))) {
        return {};
    }

    std::uint32_t redMask = 0x00ff0000u;
    std::uint32_t greenMask = 0x0000ff00u;
    std::uint32_t blueMask = 0x000000ffu;
    std::uint32_t alphaMask = 0;
    if (header.biCompression == BI_BITFIELDS) {
        if (native.bytes.size() < static_cast<int>(sizeof(BITMAPINFOHEADER) + 12))
            return {};
        std::uint32_t masks[3]{};
        std::memcpy(masks, native.bytes.constData() + sizeof(BITMAPINFOHEADER), sizeof(masks));
        redMask = masks[0];
        greenMask = masks[1];
        blueMask = masks[2];
        alphaMask = 0;
        if (header.biSize >= sizeof(BITMAPV4HEADER)) {
            BITMAPV4HEADER v4{};
            std::memcpy(&v4, native.bytes.constData(), sizeof(v4));
            alphaMask = v4.bV4AlphaMask;
        }
    }
    const DibChannelMetadata red = makeDibChannelMetadata(redMask);
    const DibChannelMetadata green = makeDibChannelMetadata(greenMask);
    const DibChannelMetadata blue = makeDibChannelMetadata(blueMask);
    const DibChannelMetadata alpha = makeDibChannelMetadata(alphaMask);
    const bool standardOpaque = alphaMask == 0 && redMask == 0x00ff0000u &&
                                greenMask == 0x0000ff00u && blueMask == 0x000000ffu;
#if defined(Q_OS_WIN) || defined(_WIN32)
    const bool useSimd = standardOpaque && screenshotClipboardDibSimdAvailable();
#else
    const bool useSimd = false;
#endif
    QImage image = snowCanvasAllocateImage(native.size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull())
        return {};
    const auto decodeRows = [&](int firstRow, int lastRow) {
        for (int y = firstRow; y < lastRow; ++y) {
            const int sourceY = topDown ? y : image.height() - 1 - y;
            const auto* source = reinterpret_cast<const std::uint8_t*>(
                native.bytes.constData() + pixelOffset + static_cast<qint64>(sourceY) * stride);
            auto* destination = reinterpret_cast<std::uint32_t*>(image.scanLine(y));
            if (standardOpaque) {
#if defined(Q_OS_WIN) || defined(_WIN32)
                if (useSimd) {
                    static_cast<void>(
                        screenshotClipboardDibDecodeBgrxSimd(source, destination, image.width()));
                    continue;
                }
#endif
                screenshotClipboardDibDecodeBgrx(source, destination, image.width());
                continue;
            }
            for (int x = 0; x < image.width(); ++x) {
                const std::uint32_t value = screenshotClipboardDibReadPixel(source + x * 4);
                const unsigned char a = alphaMask == 0 ? 255 : dibChannel(value, alpha, 255);
                destination[x] =
                    qPremultiply(qRgba(dibChannel(value, red, 0), dibChannel(value, green, 0),
                                       dibChannel(value, blue, 0), a));
            }
        }
    };
    if (standardOpaque)
        SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.fast_path", 1);
    else
        SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.generic_path", 1);
    if (useSimd) {
#if defined(Q_PROCESSOR_ARM_64)
        SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.neon", 1);
#else
        SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.avx2", 1);
#endif
    }
    SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.decoded_bytes", native.bytes.size());
    const qint64 pixels = static_cast<qint64>(image.width()) * image.height();
    const int workersCount = std::min(8, std::max(1, QThread::idealThreadCount() - 1));
    if (pixels < 1'000'000 || workersCount <= 1 || image.height() < 2) {
        decodeRows(0, image.height());
    } else {
        SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.parallel_jobs", workersCount);
        const int rowsPerWorker = (image.height() + workersCount - 1) / workersCount;
        std::vector<std::thread> workers;
        for (int worker = 0; worker < workersCount; ++worker) {
            const int first = worker * rowsPerWorker;
            const int last = std::min(image.height(), first + rowsPerWorker);
            if (first >= last)
                break;
            workers.emplace_back([&, first, last]() {
                snow_shot::platform::applyApplicationQoSToCurrentThread();
                decodeRows(first, last);
            });
        }
        for (auto& worker : workers)
            worker.join();
    }
    return image;
}
#endif
} // namespace

std::optional<ScreenshotClipboardContent>
ScreenshotClipboardContentReader::readMimeData(const QMimeData* mimeData, qreal devicePixelRatio) {
    auto captured = snapshotMimeData(mimeData, devicePixelRatio,
                                     QGuiApplication::palette().color(QPalette::Base));
    return captured.has_value() ? decode(std::move(*captured)) : std::nullopt;
}

QStringList ScreenshotClipboardContentReader::supportedImageFileExtensions() {
    return snow_shot::image::supportedImageFileExtensions();
}

QStringList ScreenshotClipboardContentReader::supportedFileExtensions() {
    return snow_shot::app::supportedPinFileExtensions();
}

QStringList ScreenshotClipboardContentReader::localFilePaths(const QMimeData* mimeData) {
    QStringList paths;
    if (mimeData != nullptr) {
        for (const QUrl& url : mimeData->urls()) {
            if (url.isLocalFile()) {
                paths.append(url.toLocalFile());
            }
        }
    }
    return paths;
}

QStringList ScreenshotClipboardContentReader::filePathsForImport(const QMimeData* mimeData) {
    if (mimeData && (mimeData->hasImage() ||
                     std::any_of(std::begin(kEncodedImageFormats), std::end(kEncodedImageFormats),
                                 [mimeData](const EncodedImageFormat& format) {
                                     return mimeData->hasFormat(QLatin1String(format.mimeType));
                                 })))
        return {};
    return localFilePaths(mimeData);
}

QList<ScreenshotClipboardLocalImage> ScreenshotClipboardContentReader::snapshotLocalFiles(
    const QStringList& paths, CancellationCheck cancelled, FilePurpose purpose) {
    const auto maximumEncodedBytes = purpose == FilePurpose::ImageImport
                                         ? snow_shot::image::maximumImageImportBytes
                                         : kMaximumEncodedImageBytes;
    QList<ScreenshotClipboardLocalImage> files;
    QSet<QString> seen;
    for (const QString& path : paths) {
        if (cancellationRequested(cancelled)) {
            return {};
        }
        const QFileInfo info(QDir::cleanPath(path));
        const QString suffix = info.suffix().toLower();
        const bool supported =
            isTextFile(suffix) ||
            std::any_of(std::begin(kImageFileFormats), std::end(kImageFileFormats),
                        [&suffix](const ImageFileFormat& format) {
                            return suffix == QLatin1String(format.suffix);
                        });
        if (!supported || !info.isFile() || !info.isReadable() || info.size() <= 0 ||
            info.size() > (isTextFile(suffix) ? 1024 * 1024 : maximumEncodedBytes)) {
            continue;
        }
        const QString absolute = info.absoluteFilePath();
        QString identity = info.canonicalFilePath();
        if (identity.isEmpty()) {
            identity = absolute;
        }
#if defined(Q_OS_WIN)
        identity = identity.toCaseFolded();
#endif
        if (seen.contains(identity)) {
            continue;
        }
        seen.insert(identity);
        files.append({absolute,
                      suffix,
                      info.size(),
                      info.lastModified().toUTC(),
                      {QStringLiteral("file:") + identity}});
    }
    return files;
}

QStringList ScreenshotClipboardContentReader::localImageFilePaths(const QMimeData* mimeData) {
    QStringList paths;
    const auto extensions = supportedImageFileExtensions();
    for (const auto& path : filePathsForImport(mimeData)) {
        if (extensions.contains(QFileInfo(path).suffix().toLower()))
            paths.append(path);
    }
    return paths;
}

static std::optional<ScreenshotClipboardContentSnapshot>
snapshotClipboardOnce(QClipboard* clipboard, qreal devicePixelRatio, AllocationCheck allocate,
                      bool imagesOnly) {
    if (clipboard == nullptr) {
        return std::nullopt;
    }
#ifdef Q_OS_MACOS
    // Our native TIFF is only another encoding of the same PNG. Independent
    // foreign bitmaps must still be snapshotted in case encoded decoding fails.
    // snapshot() checks the revision so replacement cannot invalidate this fact.
    const bool bitmapIsDerivedFromPng =
        snow_shot::platform::macos::imageClipboardBitmapIsDerivedFromPng();
#else
    constexpr bool bitmapIsDerivedFromPng = false;
#endif
    const QColor baseColor = QGuiApplication::palette().color(QPalette::Base);
    bool rejected = false;
    const AllocationCheck checked = allocate ? AllocationCheck([&](qint64 bytes) {
        rejected = rejected || !allocate(bytes);
        return !rejected;
    })
                                             : AllocationCheck{};
#if defined(Q_OS_WIN) || defined(_WIN32)
    std::optional<ScreenshotClipboardContentSnapshot> nativeSnapshot;
    if (QGuiApplication::platformName() == QStringLiteral("windows"))
        nativeSnapshot.emplace();
    if (nativeSnapshot && OpenClipboard(nullptr)) {
        const auto close = qScopeGuard([] { CloseClipboard(); });
        const std::pair<const wchar_t*, const char*> formats[] = {
            {L"PNG", "image/png"},         {L"GIF", "image/gif"},
            {L"WebP", "image/webp"},       {L"image/gif", "image/gif"},
            {L"image/webp", "image/webp"}, {L"image/apng", "image/apng"},
        };
        for (const auto& [nativeName, mimeType] : formats) {
            const auto mime = QString::fromLatin1(mimeType);
            if (std::any_of(nativeSnapshot->encodedImages.cbegin(),
                            nativeSnapshot->encodedImages.cend(),
                            [&mime](const auto& image) { return image.mimeType == mime; }))
                continue;
            auto bytes =
                captureNativeEncodedImage(nativeName, checked, snapshotBytes(*nativeSnapshot));
            if (!bytes.isEmpty())
                nativeSnapshot->encodedImages.push_back({std::move(bytes), mime});
        }
        if (auto native = captureNativeDib(checked, snapshotBytes(*nativeSnapshot)); native) {
            SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.native_dib_copied");
            nativeSnapshot->nativeDib = std::move(*native);
        }
        if (!imagesOnly) {
            const UINT format = RegisterClipboardFormatW(L"SnowShotScreenshotPlacement");
            const auto handle = static_cast<HGLOBAL>(GetClipboardData(format));
            const auto size = handle ? GlobalSize(handle) : 0;
            if (size && size <= static_cast<SIZE_T>(kScreenshotClipboardPlacementMaximumBytes) &&
                (!checked || checked(snapshotBytes(*nativeSnapshot) +
                                     kScreenshotClipboardPlacementMaximumBytes))) {
                if (const void* bytes = GlobalLock(handle)) {
                    nativeSnapshot->placement = decodeScreenshotClipboardPlacement(
                        QByteArray(static_cast<const char*>(bytes), static_cast<qsizetype>(size)));
                    GlobalUnlock(handle);
                }
            }
            const UINT appearanceFormat = RegisterClipboardFormatW(L"SnowShotScreenshotAppearance");
            const auto appearanceHandle = static_cast<HGLOBAL>(GetClipboardData(appearanceFormat));
            const auto appearanceSize = appearanceHandle ? GlobalSize(appearanceHandle) : 0;
            if (appearanceSize &&
                appearanceSize <= static_cast<SIZE_T>(kScreenshotClipboardAppearanceMaximumBytes) &&
                (!checked || checked(snapshotBytes(*nativeSnapshot) +
                                     static_cast<qint64>(appearanceSize) * 4))) {
                if (const void* bytes = GlobalLock(appearanceHandle)) {
                    nativeSnapshot->appearance = decodeScreenshotClipboardAppearance(QByteArray(
                        static_cast<const char*>(bytes), static_cast<qsizetype>(appearanceSize)));
                    GlobalUnlock(appearanceHandle);
                }
            }
        }
    }
    if (rejected)
        return std::nullopt;
    auto snapshot = snow_shot::presentation::detail::snapshotClipboardMimeData(
        clipboard->mimeData(), devicePixelRatio, baseColor, false, checked,
        std::move(nativeSnapshot), imagesOnly);
#else
    auto snapshot = snow_shot::presentation::detail::snapshotClipboardMimeData(
        clipboard->mimeData(), devicePixelRatio, baseColor, false, checked, {}, imagesOnly);
#endif
    if (rejected)
        return std::nullopt;
    if (!snapshot.has_value() && std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0) {
        snapshot = ScreenshotClipboardContentSnapshot{};
        snapshot->devicePixelRatio = devicePixelRatio;
        snapshot->baseColor = baseColor;
    }
    const bool capturedCanonicalPng =
        bitmapIsDerivedFromPng && snapshot &&
        std::any_of(snapshot->encodedImages.cbegin(), snapshot->encodedImages.cend(),
                    [](const ScreenshotClipboardEncodedImage& encoded) {
                        return encoded.mimeType == QStringLiteral("image/png");
                    });
    if (snapshot.has_value() && !snapshot->nativeDib.has_value() && !capturedCanonicalPng) {
        // Providers that expose only QMimeData::imageData() remain supported.
        SNOW_SHOT_PIN_PERF_COUNTER("clipboard.native_dib_fallback", 1);
        const auto* mime = clipboard->mimeData();
        if (mime && (!imagesOnly || mime->hasImage() || !snapshot->encodedImages.isEmpty())) {
            if (checked && !checked(snapshotBytes(*snapshot) + kMaximumClipboardImageBytes))
                return std::nullopt;
            const auto value = mime->imageData();
            if (value.canConvert<QImage>())
                snapshot->detachedImage = value.value<QImage>();
            else if (value.canConvert<QPixmap>())
                snapshot->detachedImage = value.value<QPixmap>().toImage();
        }
    }
    return !rejected && snapshot.has_value() && snapshot->isValid() ? snapshot : std::nullopt;
}

static std::optional<ScreenshotClipboardContentSnapshot> snapshotClipboard(QClipboard* clipboard,
                                                                           qreal devicePixelRatio,
                                                                           AllocationCheck allocate,
                                                                           bool imagesOnly) {
#ifdef Q_OS_MACOS
    // A process that has never copied an image must also recognize native PNG.
    snow_shot::platform::macos::initializeImageClipboardConverter();
#endif
    ensureScreenshotClipboardPlacementMimeSupport();
    ensureScreenshotClipboardAppearanceMimeSupport();
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto revision = screenshotClipboardRevision();
        auto result = snapshotClipboardOnce(clipboard, devicePixelRatio, allocate, imagesOnly);
        if (revision == screenshotClipboardRevision())
            return result;
    }
    return std::nullopt;
}

std::optional<ScreenshotClipboardContentSnapshot>
ScreenshotClipboardContentReader::snapshot(QClipboard* clipboard, qreal devicePixelRatio,
                                           AllocationCheck allocate) {
    return snapshotClipboard(clipboard, devicePixelRatio, std::move(allocate), false);
}

std::optional<ScreenshotClipboardContentSnapshot>
ScreenshotClipboardContentReader::snapshotImage(QClipboard* clipboard, qreal devicePixelRatio,
                                                AllocationCheck allocate) {
    auto captured = snapshotClipboard(clipboard, devicePixelRatio, allocate, true);
    // Lazy providers require conservative admission before capture. Queue only the bytes
    // actually retained so another window can capture its own clipboard image concurrently.
    if (captured && allocate && !allocate(snapshotBytes(*captured)))
        return std::nullopt;
    return captured;
}

std::optional<ScreenshotClipboardContentSnapshot>
ScreenshotClipboardContentReader::snapshotMimeData(const QMimeData* mimeData,
                                                   qreal devicePixelRatio, const QColor& baseColor,
                                                   AllocationCheck allocate) {
    return snow_shot::presentation::detail::snapshotClipboardMimeData(mimeData, devicePixelRatio,
                                                                      baseColor, true, allocate);
}

std::optional<ScreenshotClipboardContent>
ScreenshotClipboardContentReader::decode(ScreenshotClipboardContentSnapshot snapshot,
                                         CancellationCheck cancelled, AllocationCheck allocate) {
    if (!snow_shot::storage::isValidPinnedTextFormat(snapshot.textFormat) ||
        (snapshot.textStyle && !snapshot.textStyle->isValid()) || !snapshot.isValid() ||
        !std::isfinite(snapshot.devicePixelRatio) || snapshot.devicePixelRatio <= 0.0 ||
        cancellationRequested(cancelled)) {
        return std::nullopt;
    }
    const auto rejected = std::make_shared<bool>(false);
    const AllocationCheck checked = allocate ? AllocationCheck([allocate, rejected](qint64 bytes) {
        *rejected = *rejected || !allocate(bytes);
        return !*rejected;
    })
                                             : AllocationCheck{};
    const auto retained = snapshotBytes(snapshot);
    if (checked && !checked(retained))
        return std::nullopt;

    const auto attachPlacement = [&snapshot](std::optional<ScreenshotClipboardContent> result,
                                             const ScreenshotClipboardLocalImage* file = nullptr) {
        if (result && snapshot.placement &&
            result->image.size() == snapshot.placement->rasterSize &&
            ((!file && snapshot.placement->filePath.isEmpty()) ||
             (file && snapshot.placement->matchesFile(file->absolutePath, file->size,
                                                      file->lastModifiedUtc.toMSecsSinceEpoch()))))
            result->placement = snapshot.placement;
        if (result && !result->isFormattedText() && snapshot.appearance &&
            result->image.size() == snapshot.appearance->rasterSize &&
            ((!file && snapshot.appearance->filePath.isEmpty()) ||
             (file && snapshot.appearance->matchesFile(file->absolutePath, file->size,
                                                       file->lastModifiedUtc.toMSecsSinceEpoch()))))
            result->appearance = snapshot.appearance;
        return result;
    };

    {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_encoded_image");
        if (auto result = readEncodedImage(snapshot.encodedImages, cancelled, checked, retained);
            result.has_value()) {
            return attachPlacement(std::move(result));
        }
    }
    if (*rejected || cancellationRequested(cancelled)) {
        return std::nullopt;
    }
    {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_detached_image");
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (snapshot.nativeDib.has_value()) {
            if (!admitImage(snapshot.nativeDib->size, retained, checked))
                return std::nullopt;
            SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_native_dib");
            if (QImage image = decodeNativeDib(*snapshot.nativeDib); !image.isNull()) {
                SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.native_dib_decoded");
                if (auto result = imageContent(std::move(image)); result.has_value()) {
                    return attachPlacement(std::move(result));
                }
            }
        }
#endif
        if (!snapshot.detachedImage.isNull() &&
            !admitImage(snapshot.detachedImage.size(), retained, checked))
            return std::nullopt;
        if (auto result = imageContent(std::move(snapshot.detachedImage)); result.has_value()) {
            return attachPlacement(std::move(result));
        }
    }
    if (snapshot.localImage.has_value()) {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_file_image");
        if (auto result = readFileContent(*snapshot.localImage, snapshot.devicePixelRatio,
                                          snapshot.baseColor, cancelled, checked, retained, false,
                                          snapshot.textStyle, snapshot.colorSwatchStyle);
            result.has_value()) {
            // Text pins persist their source text, so restoring them does not
            // depend on the external file or treat that file as an image.
            if (!result->isFormattedText())
                result->originalContent.localFilePath = snapshot.localImage->absolutePath;
            result->sourceIdentity = snapshot.localImage->sourceIdentity;
            return attachPlacement(std::move(result), &*snapshot.localImage);
        }
    }
    if (snapshot.colorSwatchStyle && snapshot.html.isEmpty() &&
        snapshot.textFormat == PinnedTextFormat::PlainText &&
        parsePinnedColorSwatch(snapshot.text)) {
        ScreenshotClipboardOriginalContent original;
        original.text = snapshot.text;
        original.textStyle = snapshot.textStyle.value_or(snow_shot::storage::PinnedTextStyle{});
        original.textStyle->colorSwatch = true;
        original.textStyle->fontFamily = snapshot.colorSwatchStyle->font.family();
        original.textStyle->fontSize = 16;
        original.textStyle->colorSwatchSize = QSize(342, 210);
        original.textStyle->maximumWidth = original.textStyle->colorSwatchSize.width();
        original.textStyle->textColor = snapshot.colorSwatchStyle->foreground;
        original.textStyle->backgroundColor = snapshot.colorSwatchStyle->background;
        const AllocationCheck swatchAllocate =
            checked ? AllocationCheck([&](qint64 bytes) { return checked(retained + bytes); })
                    : AllocationCheck{};
        auto result = renderOriginalText(original, snapshot.devicePixelRatio, snapshot.baseColor,
                                         swatchAllocate, snapshot.colorSwatchStyle);
        return cancellationRequested(cancelled) ? std::nullopt : std::move(result);
    }
    if (!snapshot.html.isEmpty()) {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_html_document");
        QString plainText;
        std::shared_ptr<QTextDocument> document;
        {
            SNOW_SHOT_PIN_PERF_SCOPE("clipboard.html_document_layout");
            document = makeDocument(snapshot.html, true, &plainText, checked, retained,
                                    snapshot.textStyle);
        }
        if (document != nullptr) {
            std::optional<ScreenshotClipboardContent> result;
            {
                SNOW_SHOT_PIN_PERF_SCOPE("clipboard.html_document_render");
                result = renderTextDocument(
                    document, plainText, snapshot.devicePixelRatio, snapshot.baseColor, cancelled,
                    checked, retained + snapshot.html.size() * 32, snapshot.textStyle);
            }
            if (result.has_value()) {
                result->originalContent.html = std::move(snapshot.html);
                result->originalContent.text = std::move(snapshot.text);
                result->originalContent.textFormat = snapshot.textFormat;
                return result;
            }
        }
    }
    if (!snapshot.text.isEmpty()) {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_text_document");
        QString plainText;
        std::shared_ptr<QTextDocument> document;
        {
            SNOW_SHOT_PIN_PERF_SCOPE("clipboard.text_document_layout");
            document = makeDocument(snapshot.text, false, &plainText, checked, retained,
                                    snapshot.textStyle, snapshot.textFormat);
        }
        if (document != nullptr) {
            std::optional<ScreenshotClipboardContent> result;
            {
                SNOW_SHOT_PIN_PERF_SCOPE("clipboard.text_document_render");
                result = renderTextDocument(
                    document, plainText, snapshot.devicePixelRatio, snapshot.baseColor, cancelled,
                    checked, retained + snapshot.text.size() * 32, snapshot.textStyle);
            }
            if (result.has_value()) {
                result->originalContent.html = std::move(snapshot.html);
                result->originalContent.text = std::move(snapshot.text);
                result->originalContent.textFormat = snapshot.textFormat;
                return result;
            }
        }
    }
    return std::nullopt;
}

QImage ScreenshotClipboardContentReader::decodeImageFile(const ScreenshotClipboardLocalImage& file,
                                                         CancellationCheck cancelled,
                                                         AllocationCheck allocate) {
    const auto content = readFileContent(file, 1.0, {}, cancelled, allocate, 0, true);
    return content ? content->image : QImage{};
}

bool ScreenshotClipboardContentReader::prepareAnimationDisplay(ScreenshotClipboardContent& content,
                                                               const QTransform& transform,
                                                               CancellationCheck cancelled,
                                                               AllocationCheck allocate) {
    if (cancellationRequested(cancelled))
        return false;
    if (!content.animationSource || !content.animationSource->isAnimated())
        return true;
    const auto& source = *content.animationSource;
    const auto& display = source.firstDisplayFrame;
    if (content.image.isNull() || content.image.size() != source.canvasSize || display.isNull() ||
        display.size() != source.canvasSize ||
        display.format() != QImage::Format_ARGB32_Premultiplied ||
        (display.colorSpace().isValid() &&
         display.colorSpace() != QColorSpace(QColorSpace::SRgb)) ||
        display.devicePixelRatio() != 1.0 || !transform.isAffine() || !transform.isInvertible())
        return false;
    const QRectF bounds = transform.mapRect(QRectF(QPointF(), QSizeF(source.canvasSize)));
    if (!std::isfinite(bounds.left()) || !std::isfinite(bounds.top()) ||
        !std::isfinite(bounds.width()) || !std::isfinite(bounds.height()) || bounds.width() <= 0 ||
        bounds.height() <= 0 || bounds.width() > std::numeric_limits<int>::max() - 2 ||
        bounds.height() > std::numeric_limits<int>::max() - 2)
        return false;
    QTransform normalized;
    normalized.setMatrix(transform.m11(), transform.m12(), transform.m13(), transform.m21(),
                         transform.m22(), transform.m23(), transform.dx() - bounds.left(),
                         transform.dy() - bounds.top(), transform.m33());
    const qint64 width = static_cast<qint64>(std::ceil(bounds.width()));
    const qint64 height = static_cast<qint64>(std::ceil(bounds.height()));
    if (width * height > kMaximumClipboardImagePixels ||
        width * height * 4 > kMaximumClipboardImageBytes)
        return false;

    qint64 retained = source.encoded.size();
    const QImage* rasters[] = {&source.firstFrame, &display, &content.image,
                               &content.preparedDisplayImage};
    for (size_t index = 0; index < std::size(rasters); ++index) {
        if (rasters[index]->isNull())
            continue;
        bool shared = false;
        for (size_t earlier = 0; earlier < index; ++earlier)
            shared = shared || rasters[earlier]->constBits() == rasters[index]->constBits();
        if (!shared)
            retained += rasters[index]->sizeInBytes();
    }
    // Quarter turns and flips write directly into their final raster. Other
    // transforms may retain Qt's temporary while transferring its result into
    // managed storage; aligned rotation bounds can add a pixel at each edge.
    const auto unit = [](qreal value) { return value == 0 || value == 1 || value == -1; };
    const bool orthogonal =
        unit(normalized.m11()) && unit(normalized.m12()) && unit(normalized.m21()) &&
        unit(normalized.m22()) &&
        normalized.m11() * normalized.m11() + normalized.m12() * normalized.m12() == 1 &&
        normalized.m21() * normalized.m21() + normalized.m22() * normalized.m22() == 1 &&
        normalized.m11() * normalized.m21() + normalized.m12() * normalized.m22() == 0;
    const qint64 transformedBytes = normalized.isIdentity() ? 0
                                    : orthogonal            ? width * height * 4
                                                            : (width + 2) * (height + 2) * 8;
    if ((allocate && !allocate(retained + transformedBytes)) || cancellationRequested(cancelled))
        return false;
    QImage prepared = normalized.isIdentity()
                          ? display
                          : snowCanvasTransformImage(display, normalized, Qt::SmoothTransformation);
    if (prepared.isNull() || prepared.sizeInBytes() > kMaximumClipboardImageBytes ||
        cancellationRequested(cancelled))
        return false;
    content.preparedDisplayImage = std::move(prepared);
    content.preparedImageTransform = normalized;
    return true;
}

std::optional<ScreenshotClipboardContent> ScreenshotClipboardContentReader::renderOriginalText(
    const ScreenshotClipboardOriginalContent& original, qreal devicePixelRatio,
    const QColor& baseColor, AllocationCheck allocate,
    std::optional<PinnedColorSwatchStyle> colorSwatchStyle) {
    if (!snow_shot::storage::isValidPinnedTextFormat(original.textFormat) ||
        (original.textStyle && !original.textStyle->isValid()) ||
        (original.html.isEmpty() && original.text.isEmpty())) {
        return std::nullopt;
    }
    QString plainText;
    std::shared_ptr<QTextDocument> document;
    const auto retained = (original.html.size() + original.text.size()) * 2 +
                          (original.textStyle ? original.textStyle->fontFamily.size() * 2 : 0);
    if (original.textStyle && original.textStyle->colorSwatch && original.html.isEmpty() &&
        original.textFormat == PinnedTextFormat::PlainText) {
        const auto values = parsePinnedColorSwatch(original.text);
        const QSize logicalSize = original.textStyle->colorSwatchSize;
        if (!values || !std::isfinite(devicePixelRatio) || devicePixelRatio <= 0 ||
            devicePixelRatio > 16 ||
            !admitImage(QSize(qCeil(logicalSize.width() * devicePixelRatio),
                              qCeil(logicalSize.height() * devicePixelRatio)),
                        retained + 32768, allocate))
            return {};
        PinnedColorSwatchStyle style = colorSwatchStyle.value_or(PinnedColorSwatchStyle{});
        style.font.setFamily(original.textStyle->fontFamily);
        if (!colorSwatchStyle) {
            style.background = original.textStyle->backgroundColor;
            style.foreground = original.textStyle->textColor;
            style.secondary = style.foreground;
            style.colorLabel = QCoreApplication::translate("PinnedColorSwatch", "Color:");
            style.labels = {QCoreApplication::translate("PinnedColorSwatch", "HEX:"),
                            QCoreApplication::translate("PinnedColorSwatch", "RGB:"),
                            QCoreApplication::translate("PinnedColorSwatch", "HSL:")};
            style.rgbaLabel = QCoreApplication::translate("PinnedColorSwatch", "RGBA:");
            style.hslaLabel = QCoreApplication::translate("PinnedColorSwatch", "HSLA:");
        }
        auto rendered = renderPinnedColorSwatch(*values, style, devicePixelRatio, logicalSize);
        if (rendered.image.isNull())
            return {};
        ScreenshotClipboardContent result;
        result.kind = ScreenshotClipboardContentKind::FormattedText;
        result.image = std::move(rendered.image);
        result.formattedDocument = std::move(rendered.document);
        result.plainText = result.formattedDocument->toPlainText();
        result.formattedTextDevicePixelRatio = devicePixelRatio;
        result.originalContent = original;
        return result;
    }
    if (!original.html.isEmpty()) {
        document =
            makeDocument(original.html, true, &plainText, allocate, retained, original.textStyle);
    }
    if (document == nullptr && !original.text.isEmpty()) {
        document = makeDocument(original.text, false, &plainText, allocate, retained,
                                original.textStyle, original.textFormat);
    }
    if (document == nullptr) {
        return std::nullopt;
    }
    auto result = renderTextDocument(
        document, plainText, devicePixelRatio, baseColor, {}, allocate,
        retained + std::max(original.html.size(), original.text.size()) * 32, original.textStyle);
    if (result.has_value()) {
        result->originalContent = original;
    }
    return result;
}
