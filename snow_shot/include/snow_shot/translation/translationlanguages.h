#ifndef SNOW_SHOT_TRANSLATION_TRANSLATIONLANGUAGES_H
#define SNOW_SHOT_TRANSLATION_TRANSLATIONLANGUAGES_H

#include "snow_shot/network/snowshotapiclient.h"
#include "snow_shot/translation/translationlanguagecatalog.h"

#include <QLocale>

namespace snow_shot::translation {
[[nodiscard]] QString translationLanguageName(const QString& code);
[[nodiscard]] QString defaultTranslationTargetLanguage(const QLocale& locale);
// Returns an index into the original catalog. Custom vision models also support translation.
[[nodiscard]] int translationModelIndex(const QVector<SnowShotChatModel>& models,
                                        const QString& preferredId);
[[nodiscard]] QString translationModelGroup(const SnowShotChatModel& model);
} // namespace snow_shot::translation

#endif
