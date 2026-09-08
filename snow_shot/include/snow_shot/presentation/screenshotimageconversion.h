#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSION_H

#include "snow_shot/network/snowshotapiclient.h"
#include <QCryptographicHash>

[[nodiscard]] inline QString imageConversionFingerprint(const QImage& image) {
    if (image.isNull()) {
        return {};
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::number(rgba.width()) + ':' + QByteArray::number(rgba.height()) + ':');
    for (int row = 0; row < rgba.height(); ++row) {
        hash.addData(QByteArrayView(reinterpret_cast<const char*>(rgba.constScanLine(row)),
                                    static_cast<qsizetype>(rgba.width()) * 4));
    }
    return QString::fromLatin1(hash.result().toHex());
}

struct ScreenshotImageConversionEntry {
    SnowShotImageConversionFormat format = SnowShotImageConversionFormat::Markdown;
    QString model;
    QString source;
    int promptVersion = 1;
    QString imageFingerprint;

    [[nodiscard]] bool isValid() const {
        return !model.isEmpty() && model.size() <= 256 && !source.trimmed().isEmpty() &&
               source.size() <= 4 * 1024 * 1024 && promptVersion == 1 &&
               imageFingerprint.size() == 64 &&
               (format == SnowShotImageConversionFormat::Markdown ||
                format == SnowShotImageConversionFormat::Html);
    }
};

// Only unwrap a complete, explicitly format-tagged response. Ordinary code fences are content.
[[nodiscard]] inline QString normalizedImageConversionSource(const QString& source,
                                                             SnowShotImageConversionFormat format) {
    const QString trimmed = source.trimmed();
    const qsizetype firstNewline = trimmed.indexOf(u'\n');
    const QString tag = trimmed.left(firstNewline).trimmed().toLower();
    const bool matches =
        format == SnowShotImageConversionFormat::Markdown
            ? tag == QStringLiteral("```markdown") || tag == QStringLiteral("```md")
            : tag == QStringLiteral("```html");
    if (firstNewline >= 0 && matches && trimmed.endsWith(QStringLiteral("\n```"))) {
        return trimmed.mid(firstNewline + 1, trimmed.size() - firstNewline - 5);
    }
    return source;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSION_H
