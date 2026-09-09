#ifndef SNOW_SHOT_PRESENTATION_TRANSLATIONLANGUAGES_H
#define SNOW_SHOT_PRESENTATION_TRANSLATIONLANGUAGES_H

#include "snow_shot/network/snowshotapiclient.h"

#include <QLocale>

namespace snow_shot::presentation {
struct TranslationLanguage {
    const char* code;
    const char* name;
};

[[nodiscard]] const QVector<TranslationLanguage>& translationLanguages();
[[nodiscard]] QString translationLanguageName(const QString& code);
[[nodiscard]] QString defaultTranslationTargetLanguage(const QLocale& locale);
// Returns an index into the original catalog; vision-only catalogs have no eligible service.
[[nodiscard]] int translationModelIndex(const QVector<SnowShotChatModel>& models,
                                        const QString& preferredId);
} // namespace snow_shot::presentation

#endif
