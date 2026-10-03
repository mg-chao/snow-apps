#ifndef SNOW_SHOT_OCRTEXTOPTIONS_H
#define SNOW_SHOT_OCRTEXTOPTIONS_H

#include <QCoreApplication>
#include <QStringList>
#include <QVector>

namespace snow_shot {

inline constexpr const char* kOcrTextOptionsTranslationContext = "OcrTextOptions";

struct OcrTextOption {
    QString value;
    const char* label;
};

inline const QVector<OcrTextOption>& ocrFormattingOptions() {
    static const QVector<OcrTextOption> options = {
        {QStringLiteral("none"), QT_TRANSLATE_NOOP("OcrTextOptions", "None")},
        {QStringLiteral("keep"), QT_TRANSLATE_NOOP("OcrTextOptions", "Keep line breaks")},
        {QStringLiteral("remove"), QT_TRANSLATE_NOOP("OcrTextOptions", "Remove line breaks")},
        {QStringLiteral("smart"), QT_TRANSLATE_NOOP("OcrTextOptions", "Smart Typesetting")},
    };
    return options;
}

inline const QVector<OcrTextOption>& ocrPunctuationOptions() {
    static const QVector<OcrTextOption> options = {
        {QStringLiteral("none"), QT_TRANSLATE_NOOP("OcrTextOptions", "None")},
        {QStringLiteral("half"), QT_TRANSLATE_NOOP("OcrTextOptions", "Half-width")},
        {QStringLiteral("full"), QT_TRANSLATE_NOOP("OcrTextOptions", "Full-width")},
    };
    return options;
}

inline QStringList ocrTextOptionValues(const QVector<OcrTextOption>& options) {
    QStringList values;
    values.reserve(options.size());
    for (const auto& option : options) {
        values.append(option.value);
    }
    return values;
}

} // namespace snow_shot

#endif // SNOW_SHOT_OCRTEXTOPTIONS_H
