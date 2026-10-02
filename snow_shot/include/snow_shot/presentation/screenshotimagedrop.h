#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEDROP_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGEDROP_H

#include "snow_shot/presentation/screenshotclipboardcontent.h"
#include <QCryptographicHash>
#include <QColorSpace>

// Runs on an export worker, including pixel hashing. Identical dropped pixels
// share an identity without reading or changing the system clipboard.
inline std::optional<ScreenshotClipboardContent>
decodeScreenshotImageDrop(ScreenshotClipboardContentSnapshot snapshot,
                          ScreenshotClipboardContentReader::CancellationCheck cancelled = {}) {
    snapshot.text.clear();
    snapshot.html.clear();
    auto content = ScreenshotClipboardContentReader::decode(std::move(snapshot), cancelled);
    if (!content || content->kind != ScreenshotClipboardContentKind::Image)
        return std::nullopt;
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
