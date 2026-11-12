#ifndef SNOW_SHOT_TRANSLATION_FASTTRANSLATION_H
#define SNOW_SHOT_TRANSLATION_FASTTRANSLATION_H

#include <QString>

namespace snow_shot::translation {
inline const QString fastTranslationId = QStringLiteral("fast-translation");
inline constexpr int fastTranslationType = 1;
inline constexpr qsizetype fastTranslationMaximumItems = 50;
inline constexpr qsizetype fastTranslationMaximumLength = 5000;

inline QString fastTranslationLanguage(const QString& code) {
    if (code == u"zh-Hans")
        return QStringLiteral("zh-CHS");
    if (code == u"zh-Hant")
        return QStringLiteral("zh-CHT");
    return code;
}
} // namespace snow_shot::translation
#endif
