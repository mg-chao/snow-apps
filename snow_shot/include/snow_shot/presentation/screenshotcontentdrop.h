#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTCONTENTDROP_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTCONTENTDROP_H

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include <QCryptographicHash>
#include <QColorSpace>
#include <QFileInfo>
#include <QMimeData>

// Admission inspects MIME metadata and path suffixes only. File access and
// decoding remain on the export worker after the drop.
inline QStringList screenshotDropFilePaths(const QMimeData* mime) {
    auto paths = ScreenshotClipboardContentReader::localFilePaths(mime);
    const auto extensions = ScreenshotClipboardContentReader::supportedFileExtensions();
    paths.removeIf([&extensions](const QString& path) {
        return !extensions.contains(QFileInfo(path).suffix(), Qt::CaseInsensitive);
    });
    return paths;
}

inline bool acceptsScreenshotDrop(const QMimeData* mime) {
    if (!mime)
        return false;
    if (mime->hasImage())
        return true;
    for (const auto& format : mime->formats())
        if (format.startsWith(QLatin1String("image/")))
            return true;
    if (mime->hasUrls())
        return !screenshotDropFilePaths(mime).isEmpty();
    return mime->hasText() || mime->hasHtml();
}

// Runs on an export worker. Identical dropped content shares an identity
// without reading or changing the system clipboard.
inline std::optional<ScreenshotClipboardContent>
decodeScreenshotDropContent(ScreenshotClipboardContentSnapshot snapshot,
                            ScreenshotClipboardContentReader::CancellationCheck cancelled = {}) {
    auto content = ScreenshotClipboardContentReader::decode(std::move(snapshot), cancelled);
    if (!content)
        return std::nullopt;
    if (content->isFormattedText()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(content->originalContent.html.toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(content->originalContent.text.toUtf8());
        if (!content->sourceIdentity.isValid())
            content->sourceIdentity.key =
                QStringLiteral("drop-text:") + QString::fromLatin1(hash.result().toHex());
        return content;
    }
    if (!content->sourceIdentity.isValid()) {
        const QImage pixels = content->image.convertToFormat(QImage::Format_RGBA8888);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(QByteArray::number(pixels.width()) + ':' +
                     QByteArray::number(pixels.height()) + ':');
        hash.addData(pixels.colorSpace().iccProfile());
        for (int row = 0; row < pixels.height(); ++row) {
            if (cancelled && cancelled())
                return std::nullopt;
            hash.addData(QByteArrayView(reinterpret_cast<const char*>(pixels.constScanLine(row)),
                                        static_cast<qsizetype>(pixels.width()) * 4));
        }
        content->sourceIdentity.key =
            QStringLiteral("drop-image:") + QString::fromLatin1(hash.result().toHex());
    }
    return content;
}

#endif
