#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSION_H

#include "snow_shot/network/snowshotapiclient.h"

struct ScreenshotImageConversionEntry {
    SnowShotImageConversionFormat format = SnowShotImageConversionFormat::Markdown;
    QString model;
    QString source;
    int promptVersion = 1;
    QString modelFingerprint;

    [[nodiscard]] bool isValid() const {
        return !model.isEmpty() && model.size() <= 256 && !source.trimmed().isEmpty() &&
               source.size() <= 4 * 1024 * 1024 && promptVersion == 1 &&
               (format == SnowShotImageConversionFormat::Markdown ||
                format == SnowShotImageConversionFormat::Html);
    }
};

// Only unwrap a complete, explicitly format-tagged response. Ordinary code fences are content.
[[nodiscard]] inline QString normalizedImageConversionSource(const QString& source,
                                                             SnowShotImageConversionFormat format) {
    QString candidate = source;
    if (candidate.startsWith(QChar(0xfeff))) {
        candidate.remove(0, 1);
    }
    const QString trimmed = candidate.trimmed();
    const qsizetype firstNewline = trimmed.indexOf(u'\n');
    if (firstNewline < 0) {
        return candidate;
    }
    const QString opening = trimmed.left(firstNewline).trimmed();
    const QChar marker = opening.isEmpty() ? QChar{} : opening.front();
    if (marker != u'`' && marker != u'~') {
        return candidate;
    }
    qsizetype fenceLength = 0;
    while (fenceLength < opening.size() && opening.at(fenceLength) == marker) {
        ++fenceLength;
    }
    const QString tag = opening.mid(fenceLength).trimmed().toLower();
    const bool matches = format == SnowShotImageConversionFormat::Markdown
                             ? tag == QStringLiteral("markdown") || tag == QStringLiteral("md")
                             : tag == QStringLiteral("html");
    if (fenceLength < 3 || !matches) {
        return candidate;
    }
    // The first matching closing fence must end the response. Otherwise these are document
    // code blocks, and removing the first and last lines would corrupt the content.
    for (qsizetype start = firstNewline + 1; start < trimmed.size();) {
        const qsizetype newline = trimmed.indexOf(u'\n', start);
        const qsizetype end = newline < 0 ? trimmed.size() : newline;
        const QString line = trimmed.mid(start, end - start).trimmed();
        qsizetype count = 0;
        while (count < line.size() && line.at(count) == marker) {
            ++count;
        }
        if (count >= fenceLength && count == line.size()) {
            if (end != trimmed.size()) {
                return candidate;
            }
            qsizetype contentEnd = start;
            if (contentEnd > firstNewline + 1 && trimmed.at(contentEnd - 1) == u'\n') {
                --contentEnd;
                if (contentEnd > firstNewline + 1 && trimmed.at(contentEnd - 1) == u'\r') {
                    --contentEnd;
                }
            }
            return trimmed.mid(firstNewline + 1, contentEnd - firstNewline - 1);
        }
        start = end + 1;
    }
    return candidate;
}

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTIMAGECONVERSION_H
