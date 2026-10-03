#include "snow_shot/presentation/screenshotocrtexttransform.h"

#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotocrlayout.h"

#include <QStringList>
#include <QVector>
#include <utility>

namespace snow_shot::presentation {

QString originalOcrText(const ScreenshotOcrPresentation& presentation) {
    QStringList lines;
    lines.reserve(presentation.lines.size());
    for (const ScreenshotOcrLine& line : presentation.lines) {
        lines.push_back(line.text);
    }
    return lines.join(QChar('\n'));
}

QString smartOcrText(const ScreenshotOcrPresentation& presentation, bool selectedOnly) {
    auto lines = presentation.lines;
    if (selectedOnly) {
        lines.clear();
        for (int index = 0; index < presentation.lines.size(); ++index) {
            const auto range = presentation.textSelectionForLine(index);
            if (range.empty()) {
                continue;
            }
            auto line = presentation.lines.at(index);
            line.text = line.text.mid(range.start, range.length);
            lines.append(std::move(line));
        }
    }
    QStringList paragraphs;
    for (const auto& line : mergeOcrLayout(lines, presentation.selection.topLeft())) {
        paragraphs.append(line.text);
    }
    return paragraphs.join(QChar('\n'));
}

QString removeOcrLineBreaks(const QString& text) {
    QString result = text;
    result.remove(QChar('\n'));
    return result;
}

QString convertOcrPunctuation(const QString& text, bool fullWidth) {
    static const QString ascii = QStringLiteral("!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~");
    static const QVector<QChar> wide = {
        QChar(0xFF01), QChar(0xFF02), QChar(0xFF03), QChar(0xFF04), QChar(0xFF05), QChar(0xFF06),
        QChar(0xFF07), QChar(0xFF08), QChar(0xFF09), QChar(0xFF0A), QChar(0xFF0B), QChar(0xFF0C),
        QChar(0xFF0D), QChar(0xFF0E), QChar(0xFF0F), QChar(0xFF1A), QChar(0xFF1B), QChar(0xFF1C),
        QChar(0xFF1D), QChar(0xFF1E), QChar(0xFF1F), QChar(0xFF20), QChar(0xFF3B), QChar(0xFF3C),
        QChar(0xFF3D), QChar(0xFF3E), QChar(0xFF3F), QChar(0xFF40), QChar(0xFF5B), QChar(0xFF5C),
        QChar(0xFF5D), QChar(0xFF5E)};
    QString result;
    result.reserve(text.size());
    for (const QChar character : text) {
        const qsizetype asciiIndex = ascii.indexOf(character);
        if (fullWidth && asciiIndex >= 0) {
            result.append(wide.at(asciiIndex));
            continue;
        }
        const qsizetype wideIndex = wide.indexOf(character);
        if (!fullWidth && wideIndex >= 0) {
            result.append(ascii.at(wideIndex));
            continue;
        }
        result.append(character);
    }
    return result;
}

QString applyOcrTextTransforms(const QString& text, const QString& formatting,
                               const QString& punctuation) {
    QString result = formatting == QStringLiteral("remove") ? removeOcrLineBreaks(text) : text;
    if (punctuation == QStringLiteral("half")) {
        result = convertOcrPunctuation(result, false);
    } else if (punctuation == QStringLiteral("full")) {
        result = convertOcrPunctuation(result, true);
    }
    return result;
}

QString applyOcrTextTransforms(const ScreenshotOcrPresentation& presentation,
                               const QString& formatting, const QString& punctuation) {
    const bool selectedOnly = presentation.hasTextSelection();
    const QString text =
        formatting == QStringLiteral("smart")
            ? smartOcrText(presentation, selectedOnly)
            : (selectedOnly ? presentation.selectedText() : originalOcrText(presentation));
    return applyOcrTextTransforms(text, formatting, punctuation);
}

} // namespace snow_shot::presentation
