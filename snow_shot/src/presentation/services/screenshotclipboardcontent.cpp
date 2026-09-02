#include "snow_shot/presentation/screenshotclipboardcontent.h"

#include "../../image/snowimageqtcodec.h"
#include "../pinned/screenshotpintoperfinstrumentation.h"

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
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

#if defined(Q_OS_WIN) || defined(_WIN32)
bool screenshotClipboardDibAvx2Available();
bool screenshotClipboardDibDecodeBgrxAvx2(const std::uint32_t* source,
                                          std::uint32_t* destination, int pixels);
#endif

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

std::optional<ScreenshotClipboardContentSnapshot>
snapshotMimeDataInternal(const QMimeData* mimeData, qreal devicePixelRatio,
                         const QColor& baseColor, bool includeDetachedImage) {
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

    if (includeDetachedImage) {
        if (const QVariant imageValue = mimeData->imageData(); imageValue.isValid()) {
            if (imageValue.canConvert<QImage>()) {
                snapshot.detachedImage = imageValue.value<QImage>();
            } else if (imageValue.canConvert<QPixmap>()) {
                snapshot.detachedImage = imageValue.value<QPixmap>().toImage();
            }
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

#if defined(Q_OS_WIN) || defined(_WIN32)
std::optional<ScreenshotClipboardNativeDib> captureNativeDib() {
    if (!OpenClipboard(nullptr)) {
        return std::nullopt;
    }
    const auto closeClipboard = []() { static_cast<void>(CloseClipboard()); };
    const UINT format = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5
                                                               : (IsClipboardFormatAvailable(CF_DIB)
                                                                      ? CF_DIB
                                                                      : 0);
    if (format == 0) {
        closeClipboard();
        return std::nullopt;
    }
    HGLOBAL handle = static_cast<HGLOBAL>(GetClipboardData(format));
    const SIZE_T size = handle != nullptr ? GlobalSize(handle) : 0;
    void* locked = handle != nullptr ? GlobalLock(handle) : nullptr;
    if (locked == nullptr || size < sizeof(BITMAPINFOHEADER) ||
        size > static_cast<SIZE_T>(std::numeric_limits<int>::max())) {
        if (locked != nullptr) GlobalUnlock(handle);
        closeClipboard();
        return std::nullopt;
    }
    QByteArray bytes(static_cast<const char*>(locked), static_cast<int>(size));
    GlobalUnlock(handle);
    closeClipboard();

    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(bytes.constData());
    if (header->biSize < sizeof(BITMAPINFOHEADER) ||
        header->biSize > static_cast<DWORD>(bytes.size()) || header->biWidth <= 0 ||
        header->biHeight == 0 || header->biPlanes != 1 || header->biBitCount != 32 ||
        (header->biCompression != BI_RGB && header->biCompression != BI_BITFIELDS)) {
        return std::nullopt;
    }
    const qint64 height = header->biHeight < 0
                              ? -static_cast<qint64>(header->biHeight)
                              : static_cast<qint64>(header->biHeight);
    const qint64 width = header->biWidth;
    const qint64 stride = ((width * 4) + 3) & ~qint64(3);
    const qint64 pixelOffset = static_cast<qint64>(header->biSize) +
                               (header->biCompression == BI_BITFIELDS &&
                                        header->biSize < sizeof(BITMAPV4HEADER)
                                    ? 12
                                    : 0);
    if (width <= 0 || height <= 0 || width * height > kMaximumClipboardImagePixels ||
        stride <= 0 || pixelOffset < 0 || pixelOffset + stride * height > bytes.size()) {
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
    if (mask == 0) return metadata;
    while (metadata.shift < 32 && ((mask >> metadata.shift) & 1u) == 0u) ++metadata.shift;
    unsigned highestBit = 31;
    while (highestBit > metadata.shift && ((mask >> highestBit) & 1u) == 0u) --highestBit;
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
    if (metadata.mask == 0 || metadata.bitCount == 0) return fallback;
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
    if (!native.isValid()) return {};
    const auto* header = reinterpret_cast<const BITMAPINFOHEADER*>(native.bytes.constData());
    if (native.bytes.size() < static_cast<int>(sizeof(BITMAPINFOHEADER)) ||
        header->biSize < sizeof(BITMAPINFOHEADER) ||
        header->biSize > static_cast<DWORD>(native.bytes.size()) || header->biWidth <= 0 ||
        header->biHeight == 0 || header->biPlanes != 1 || header->biBitCount != 32 ||
        (header->biCompression != BI_RGB && header->biCompression != BI_BITFIELDS)) {
        return {};
    }
    const qint64 width = header->biWidth;
    const qint64 height = header->biHeight < 0
                              ? -static_cast<qint64>(header->biHeight)
                              : static_cast<qint64>(header->biHeight);
    const bool topDown = header->biHeight < 0;
    if (height <= 0 || width > std::numeric_limits<qint64>::max() / 4 ||
        width * height > kMaximumClipboardImagePixels || width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max()) {
        return {};
    }
    const qint64 rowBytes = width * 4;
    if (rowBytes > std::numeric_limits<qint64>::max() - 3) return {};
    const qint64 stride = (rowBytes + 3) & ~qint64(3);
    const qint64 pixelOffset = static_cast<qint64>(header->biSize) +
                               (header->biCompression == BI_BITFIELDS &&
                                        header->biSize < sizeof(BITMAPV4HEADER)
                                    ? 12
                                    : 0);
    if (pixelOffset < 0 || pixelOffset > native.bytes.size() ||
        height > (native.bytes.size() - pixelOffset) / stride ||
        native.size != QSize(static_cast<int>(width), static_cast<int>(height))) {
        return {};
    }

    std::uint32_t redMask = 0x00ff0000u;
    std::uint32_t greenMask = 0x0000ff00u;
    std::uint32_t blueMask = 0x000000ffu;
    std::uint32_t alphaMask = 0;
    if (header->biCompression == BI_BITFIELDS) {
        if (native.bytes.size() < static_cast<int>(sizeof(BITMAPINFOHEADER) + 12)) return {};
        const auto* masks = reinterpret_cast<const std::uint32_t*>(
            native.bytes.constData() + sizeof(BITMAPINFOHEADER));
        redMask = masks[0];
        greenMask = masks[1];
        blueMask = masks[2];
        alphaMask = 0;
        if (header->biSize >= sizeof(BITMAPV4HEADER)) {
            if (native.bytes.size() < static_cast<int>(sizeof(BITMAPV5HEADER))) return {};
            const auto* v5 = reinterpret_cast<const BITMAPV5HEADER*>(native.bytes.constData());
            alphaMask = v5->bV5AlphaMask;
        }
    }
    const DibChannelMetadata red = makeDibChannelMetadata(redMask);
    const DibChannelMetadata green = makeDibChannelMetadata(greenMask);
    const DibChannelMetadata blue = makeDibChannelMetadata(blueMask);
    const DibChannelMetadata alpha = makeDibChannelMetadata(alphaMask);
    const bool standardOpaque = alphaMask == 0 && redMask == 0x00ff0000u &&
                                greenMask == 0x0000ff00u && blueMask == 0x000000ffu;
#if defined(Q_OS_WIN) || defined(_WIN32)
    const bool useAvx2 = standardOpaque && screenshotClipboardDibAvx2Available();
#else
    const bool useAvx2 = false;
#endif
    QImage image(native.size, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return {};
    const auto decodeRows = [&](int firstRow, int lastRow) {
        for (int y = firstRow; y < lastRow; ++y) {
            const int sourceY = topDown ? y : image.height() - 1 - y;
            const auto* source = reinterpret_cast<const std::uint32_t*>(
                native.bytes.constData() + pixelOffset + static_cast<qint64>(sourceY) * stride);
            auto* destination = reinterpret_cast<std::uint32_t*>(image.scanLine(y));
            if (standardOpaque) {
#if defined(Q_OS_WIN) || defined(_WIN32)
                if (useAvx2) {
                    static_cast<void>(screenshotClipboardDibDecodeBgrxAvx2(
                        source, destination, image.width()));
                    continue;
                }
#endif
                for (int x = 0; x < image.width(); ++x) destination[x] = source[x] | 0xff000000u;
                continue;
            }
            for (int x = 0; x < image.width(); ++x) {
                const std::uint32_t value = source[x];
                const unsigned char a = alphaMask == 0 ? 255 : dibChannel(value, alpha, 255);
                destination[x] = qPremultiply(qRgba(dibChannel(value, red, 0),
                                                    dibChannel(value, green, 0),
                                                    dibChannel(value, blue, 0), a));
            }
        }
    };
    if (standardOpaque) SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.fast_path", 1);
    else SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.generic_path", 1);
    if (useAvx2) SNOW_SHOT_PIN_PERF_COUNTER("clipboard.dib.avx2", 1);
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
            if (first >= last) break;
            workers.emplace_back([&, first, last]() { decodeRows(first, last); });
        }
        for (auto& worker : workers) worker.join();
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

std::optional<ScreenshotClipboardContentSnapshot>
ScreenshotClipboardContentReader::snapshot(QClipboard* clipboard, qreal devicePixelRatio) {
    if (clipboard == nullptr) {
        return std::nullopt;
    }
    const QColor baseColor = QGuiApplication::palette().color(QPalette::Base);
    auto snapshot = snapshotMimeDataInternal(clipboard->mimeData(), devicePixelRatio, baseColor,
                                             false);
    if (!snapshot.has_value() && std::isfinite(devicePixelRatio) && devicePixelRatio > 0.0) {
        snapshot = ScreenshotClipboardContentSnapshot{};
        snapshot->devicePixelRatio = devicePixelRatio;
        snapshot->baseColor = baseColor;
    }
#if defined(Q_OS_WIN) || defined(_WIN32)
    if (snapshot.has_value()) {
        if (auto native = captureNativeDib(); native.has_value()) {
            SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.native_dib_copied");
            snapshot->nativeDib = std::move(*native);
        }
    }
#endif
    if (snapshot.has_value() && !snapshot->nativeDib.has_value()) {
        // Providers that expose only QMimeData::imageData() remain supported.
        SNOW_SHOT_PIN_PERF_COUNTER("clipboard.native_dib_fallback", 1);
        auto fallback = snapshotMimeDataInternal(clipboard->mimeData(), devicePixelRatio, baseColor,
                                                 true);
        if (fallback.has_value()) {
            snapshot->detachedImage = std::move(fallback->detachedImage);
        }
    }
    return snapshot.has_value() && snapshot->isValid() ? snapshot : std::nullopt;
}

std::optional<ScreenshotClipboardContentSnapshot>
ScreenshotClipboardContentReader::snapshotMimeData(const QMimeData* mimeData,
                                                   qreal devicePixelRatio,
                                                   const QColor& baseColor) {
    return snapshotMimeDataInternal(mimeData, devicePixelRatio, baseColor, true);
}

std::optional<ScreenshotClipboardContent>
ScreenshotClipboardContentReader::decode(ScreenshotClipboardContentSnapshot snapshot,
                                         CancellationCheck cancelled) {
    if (!snapshot.isValid() || !std::isfinite(snapshot.devicePixelRatio) ||
        snapshot.devicePixelRatio <= 0.0 || cancellationRequested(cancelled)) {
        return std::nullopt;
    }

    {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_encoded_image");
        if (auto result = readEncodedImage(snapshot.encodedImages, cancelled);
            result.has_value()) {
            return result;
        }
    }
    if (cancellationRequested(cancelled)) {
        return std::nullopt;
    }
    {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_detached_image");
#if defined(Q_OS_WIN) || defined(_WIN32)
        if (snapshot.nativeDib.has_value()) {
            SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_native_dib");
            if (QImage image = decodeNativeDib(*snapshot.nativeDib); !image.isNull()) {
                SNOW_SHOT_PIN_PERF_MILESTONE("clipboard.native_dib_decoded");
                if (auto result = imageContent(std::move(image)); result.has_value()) {
                    return result;
                }
            }
        }
#endif
        if (auto result = imageContent(std::move(snapshot.detachedImage)); result.has_value()) {
            return result;
        }
    }
    if (snapshot.localImage.has_value()) {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_file_image");
        if (auto result = readFileImage(*snapshot.localImage, cancelled); result.has_value()) {
            return result;
        }
    }
    if (!snapshot.html.isEmpty()) {
        SNOW_SHOT_PIN_PERF_SCOPE("clipboard.decode_html_document");
        QString plainText;
        std::shared_ptr<QTextDocument> document;
        {
            SNOW_SHOT_PIN_PERF_SCOPE("clipboard.html_document_layout");
            document = makeDocument(snapshot.html, true, &plainText);
        }
        if (document != nullptr) {
            std::optional<ScreenshotClipboardContent> result;
            {
                SNOW_SHOT_PIN_PERF_SCOPE("clipboard.html_document_render");
                result = renderTextDocument(document, plainText, snapshot.devicePixelRatio,
                                            snapshot.baseColor, cancelled);
            }
            if (result.has_value()) {
                result->originalContent.html = std::move(snapshot.html);
                result->originalContent.text = std::move(snapshot.text);
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
            document = makeDocument(snapshot.text, false, &plainText);
        }
        if (document != nullptr) {
            std::optional<ScreenshotClipboardContent> result;
            {
                SNOW_SHOT_PIN_PERF_SCOPE("clipboard.text_document_render");
                result = renderTextDocument(document, plainText, snapshot.devicePixelRatio,
                                            snapshot.baseColor, cancelled);
            }
            if (result.has_value()) {
                result->originalContent.html = std::move(snapshot.html);
                result->originalContent.text = std::move(snapshot.text);
                return result;
            }
        }
    }
    return std::nullopt;
}
